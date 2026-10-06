#pragma once
// The AmbientForce engine, for now a stub: a sine per note, so the plugin around it has something
// to play and the tests something to measure. The real engine (the harmony brain, Ground, Bloom,
// Space) replaces it behind the same interface.
//
// Six voices; a seventh note takes the oldest releasing voice, or the oldest of all when none is
// releasing. A 10 ms linear attack and a 300 ms linear release (from wherever the level is), the
// sustain pedal holding released keys. Mono: L = R.
//
// Real-time rules: no allocation, no locks, no exceptions after construction. The plugin layer
// feeds it a Patch once per block (only when something changed).
#include <cstdint>

namespace af {

// The volume at or below which the output is off: the knob's text shows "-inf dB" from here.
constexpr float kVolumeOffDb = -59.5f;

struct Patch {
    float volumeDb = -6.0f;
};

class Engine {
public:
    static constexpr int kVoices = 6;

    explicit Engine(float sampleRate = 44100.0f);

    void setPatch(const Patch& p);           // between render() calls
    void noteOn(int note, int velocity);     // velocity 0 = note off
    void noteOff(int note);
    void pitchBend(float) {}
    void sustain(bool down);
    void allNotesOff();                      // release every note, held by a key or the pedal (CC 123)
    void reset();                            // silence now: CC 120, suspend, transport stop
    void controller(int, int) {}
    void aftertouch(float) {}
    void polyAftertouch(int, float) {}
    void resetControllers() { sustain(false); }   // CC 121: the pedal back up
    void seed(uint32_t s);                   // per instance; the stub has no random numbers yet
    // MPC's tempo and position (quarter notes), once per block before render().
    void setTransport(double bpm, double beats, bool playing, bool beatsValid);

    void render(float* outL, float* outR, int n);   // overwrites n samples
    int  activeVoices() const;                      // voices sounding, releases included

private:
    struct Voice {
        int      note = -1;          // -1: free
        float    phase = 0.0f;       // 0..1
        float    inc = 0.0f;         // phase per sample
        float    amp = 0.0f;         // from the velocity
        float    level = 0.0f;       // the envelope, 0..1
        float    step = 0.0f;        // envelope change per sample: > 0 attack, < 0 release, 0 held
        bool     key = false;        // its key is down
        bool     pedal = false;      // its key is up, the pedal holds it
        uint64_t age = 0;            // when it started: the oldest is taken first
    };

    void release(Voice& v);

    float    sr_;
    float    attackStep_, releaseStep_;   // per sample, at full level
    float    gain_ = 0.0f;                // the volume as a factor, gliding to target_
    float    target_ = 0.0f;
    float    glide_;                      // the gain's one-pole coefficient per sample
    bool     pedal_ = false;
    uint64_t started_ = 0;                // notes started so far: each voice's age
    uint32_t seed_ = 1;
    double   bpm_ = 120.0, beats_ = 0.0;
    bool     playing_ = false, beatsValid_ = false;
    Voice    voices_[kVoices];
};

} // namespace af
