#pragma once
// The AmbientForce engine (docs/CONCEPT.md 4 and 8): the harmony brain, Ground and Bloom, Space,
// and the output stage, behind the interface the plugin drives (MIDI at sample offsets, the
// transport once per block, a Patch when a parameter changes).
//
// Routing (CONCEPT.md 4.1). A key goes through Input (mapInput) once, on its way down; the
// harmony keeps the key and its mapped note together, so a key's note-off finds its note even if
// the patch changed in between. Then every stratum hears it by its Listen mode:
// - Bloom, Notes: the chord on the mapped note (buildChord, led from the chord Bloom played last
//   when Leading is on; with Chord Off the single note), owned by the key. Harmony: it moves to
//   the harmony's chord whenever that changes (Harmony::version()), and lets go when the memory
//   forgets it. Free: it moves to the tonic chord (its Chord type, a triad under Chord Off) from
//   the first note on, and moves again when the key, scale, chord or voicing change.
// - Ground, Notes: the lowest note held (fading when none is). Harmony: the harmony's root. Free:
//   the tonic, from the first note on.
// Nothing sounds before the first note-on, Free strata included: the engine is asleep. It wakes
// at the first note-on and stays awake through note-offs, until Stop or reset() put it to sleep.
//
// Keys, the pedal and Hold. A key up while the pedal (CC 64) is down is held by the pedal until
// it comes up; with Hold on it is latched instead. The first note-on after every finger has left
// the keys starts a new chord and lets the latched keys go, after the new chord has started
// (Bloom's play() before release(), so the notes the two chords share carry on); a key pressed
// while a finger is still down joins the chord. A key held either way still counts as held for the
// harmony: the memory's bars count from when the chord is let go, not from when the fingers left
// it, and Ground in Notes mode stays on the chord the pedal or Hold keeps sounding. (Under Chord
// Off, where the harmony's chord is the notes held, a new chord's latched keys leave the harmony
// before its first key goes down, or they would stay in its chord.) The harmony keeps 16 keys:
// full, a new key lets the oldest one the pedal or Hold keeps go first (a pedal down through a
// long phrase), while 16 fingers keep theirs and the new key isn't heard there. Hold turned off
// lets the latched keys go (to the pedal, if it is down). CC 123 lets every key go, held by a
// finger, the pedal or Hold, but the engine stays awake: what the harmony remembers goes on
// sounding.
//
// Listen changed while sounding: Ground is given its new target at once (gliding by Gravity, or
// fading when the new mode has none). Bloom moved to Harmony or Free moves to that chord at once,
// the notes in common carrying on; moved back to Notes, the keys held (by a finger, the pedal or
// Hold) play their chords again, oldest first, before the harmony's or the tonic's chord lets go,
// so Bloom and Ground agree.
//
// Stop (CONCEPT.md 7.4): the transport stopping (playing to stopped, from setTransport) or a
// suspend that resumes within kStopWindowS applies On Stop: Keep does nothing; Fade fades the
// output to silence over kFadeS (linear in dB down to -60 dB), then resets and sleeps; Cut resets
// at once. A note-on or the transport starting during a fade turns it round: the output comes
// back up at kRecoverDbPerS. On Stop changed during a fade applies at once: Keep turns it round,
// Cut resets. A longer suspend resets. CC 120 and reset() silence everything at once, forget the
// harmony and the keys, and sleep.
//
// The mix: Ground and Bloom each render their dry (panned by groundPan / bloomPan, equal power,
// unity in the middle) into the dry bus and add their sends into the Space bus (Ground at
// groundSpace, Bloom at bloomSpace). Bloom's send is 0 whenever Space can't carry a tail (the
// return or Bloom's send at 0, or Freeze on: a frozen reverb takes no input): it then releases as
// Tail Voice. Ground's is 0 with the return at 0 too, so a Space nobody hears isn't run. With
// Bloom's Tail on Space, the reverb's decay is held at Bloom's Release or longer (in Abyss, which
// rings four times its Decay, at a quarter of it), so the tail a voice hands off carries on. The
// volume, the return and the pans glide in a straight line over 10 ms from where the next control
// step finds them changed, a step a sample carried from one piece to the next, so a MIDI event
// that cuts a piece short never makes one jump.
//
// The output: dry + spaceReturn x wet -> tilt -> volume -> the non-finite guard -> limiter ->
// Stop's fade.
// - Tilt: one first-order shelf pivoting at 800 Hz, the highs up and the lows down by 6 dB at
//   tilt 1 (and the other way at -1), 0 dB at the pivot. Bypassed at 0.
// - Volume before the limiter, so the ceiling holds at every volume: the presets are level-matched
//   well under it and the knob reaches +6 dB, where the limiter takes the extra rather than the
//   output passing -1 dBFS. (After the limiter, +6 dB would put a limited peak at +5 dBFS.)
// - The guard: any sample that isn't finite zeroes the whole render() call, resets every DSP
//   state (Ground, Bloom, Space, the output's filters) and is counted; the keys and the harmony
//   stay, so Harmony and Free strata come back by themselves. It looks before the limiter, whose
//   soft clip would turn an infinity into a finite peak.
// - The limiter: no lookahead. A stereo-linked gain computer (1 ms attack, 150 ms release) holds
//   the peak at kKnee (0.95 of the ceiling); what the attack lets through goes into a soft clip
//   (softclip(), a tanh-like curve) from kKnee up to kCeiling, which it never passes: the output
//   stays at or under -1 dBFS (0.8913) whatever comes in. It keeps its gain as the distance under
//   1, so the release (a float step of 1.5e-4 of that distance) never stalls short of 1; with no
//   peak over the knee in a piece and the gain within 0.1% of 1, it lands on 1 (a step of 0.009
//   dB). Under the knee, with the gain back at 1, it only looks.
//
// Control rate: every kChunk samples the harmony's memory advances at the transport's tempo, the
// routes are looked at again (the memory may have forgotten the chord), Stop's fade and the tilt
// take a step. The steps sit on the sample count's multiples of kChunk, so the host's blocks
// (MPC's 128) never cut one; only a MIDI event does, at its sample. Ground and Bloom are rendered a
// control step at a time: they read their tables (TableSet::get) and step their own controls at
// every call.
//
// Idle: asleep, or awake with nothing to hear (Ground !audible(), Bloom with no voice in use) and
// Space::silent(): render() writes zeros and runs no DSP. Space can stay unsilent for minutes
// (Abyss at Decay 30), and a set() that lengthens its reach can make it unsilent again with no
// input: it then simply runs that much longer.
//
// Cost, as ARM instructions per 128-sample block (qemu's count, the device's flags, tables of a
// lifetime table's size): asleep 0.4k. The Init patch (the knobs' defaults, squared) holding a
// triad 206k: Space 113k, Bloom 46k, Ground 35k, the engine's own routing, mix and output 4.5k. The
// worst case (6 voices of unison 2 with FM, every Ground partial and Body, Abyss with shimmer) 394k:
// Bloom 224k, Space 107k, Ground 46k, the engine 7k. By PolyForce's ~1 ns an instruction that is
// 7.1% and 13.6% of a block, against CONCEPT.md 11's 15% gate for everything, Air, Weather, Echo
// and Patina still to come: Bloom's unison 2 is the first cap to fall, as planned there.
//
// Real-time rules: everything is allocated in the constructor (Space's buffers). Nothing on the
// audio thread allocates, locks or throws.
#include "bloom.h"
#include "ground.h"
#include "harmony.h"
#include "lifetime.h"
#include "space.h"

#include <cstdint>

namespace af {

// The volume at or below which the output is off: the knob's text shows "-inf dB" from here.
constexpr float kVolumeOffDb = -59.5f;

// What Stop does to a sounding landscape (docs/CONCEPT.md 7.4).
enum OnStop : int { OS_KEEP, OS_FADE, OS_CUT, OS_COUNT };
inline constexpr const char* kOnStopNames[] = {"Keep", "Fade", "Cut"};   // the surface's (harmony.h says why)
static_assert(sizeof kOnStopNames / sizeof *kOnStopNames == OS_COUNT, "a name per On Stop");

// Init's Space: a slow hall for pads, not the Reverb's own defaults (EffectForce's, for any input).
constexpr Space::Params initSpace() {
    Space::Params s;
    s.reverb.mode = Reverb::HALL;
    s.reverb.size = 0.6f;
    s.reverb.decayS = 8.0f;
    s.reverb.predelayMs = 30.0f;
    s.reverb.dampHz = 6000.0f;
    s.reverb.lowCutHz = 120.0f;
    s.reverb.mod = 0.4f;
    s.reverb.width = 1.0f;
    s.rise = 0.2f;
    return s;
}

// Everything the engine plays, in real values (seconds, Hz, gains): plugin/patch_map.cpp builds it
// from the parameters. Levels and sends arrive as gains, the knobs' audio taper (gain = knob^2) being
// the patch map's. The defaults are Init's (the surface's defaults through the patch map, which
// test/params_test.cpp holds them to): the levels and sends are the default knobs squared.
struct Patch {
    float volumeDb = -6.0f;
    float tilt = 0.0f;            // -1..1: ±6 dB at the extremes, pivot 800 Hz
    HarmonyPatch harmony;
    bool hold = false;            // latch: chords stay until the next one
    int onStop = OS_FADE;
    GroundPatch ground;
    float groundSpace = 0.4f * 0.4f;   // Ground's send: the knob at 40%
    float groundPan = 0.0f;
    BloomPatch bloom;
    float bloomSpace = 0.5f * 0.5f;    // Bloom's send: the knob at 50%
    float bloomPan = 0.0f;
    Space::Params space = initSpace();
    float spaceReturn = 0.8f * 0.8f;   // the return's level: the knob at 80%
};

class Engine {
public:
    static constexpr double kStopWindowS = 0.25;  // a suspend resumed within this is a Stop, longer a reset
    static constexpr float kFadeS = 8.0f;         // On Stop's Fade, to -60 dB
    static constexpr float kRecoverDbPerS = 30.0f;   // a fade turned round comes back this fast
    static constexpr float kCeiling = 0.891f;     // -1 dBFS: the output never passes it
    static constexpr float kKnee = 0.95f * kCeiling;   // the limiter holds peaks here; the soft clip above

    // What the tests (and nothing else) look at.
    struct Info {
        bool awake = false;           // a note-on has woken it, and no Stop or reset put it to sleep
        bool idle = false;            // the last piece (a control step, or what an event left of it) ran no DSP
        bool fading = false;          // On Stop's Fade under way
        float fadeDb = 0.0f;          // where it is (0: none)
        bool suspended = false;       // suspend() without its resume() (the plugin calls both at once; the tests apart)
        int groundTarget = -1;        // the note Ground was last given (-1: none)
        bool groundAudible = false;
        int bloomActive = 0;
        int harmonyRoot = -1;
        uint32_t harmonyVersion = 0;
        uint32_t guards = 0;          // blocks the non-finite guard zeroed
        uint64_t samples = 0;         // rendered so far
        float limiterGain = 1.0f;
        bool limiting = false;        // the last piece ran the limiter's gain computer
        bool gliding = false;         // the volume, the return or a pan is still on its way
        float spaceDecayS = 0.0f;     // the decay Space was given (the tail's hold applied)
    };

    explicit Engine(const TableSet& tables);

    void setPatch(const Patch& p);           // between render() calls
    void noteOn(int note, int velocity);     // velocity 0 = note off
    void noteOff(int note);
    void pitchBend(float) {}
    void sustain(bool down);
    void allNotesOff();                      // CC 123: every key let go; the engine stays awake
    void reset();                            // CC 120: silence now, the harmony forgotten, asleep
    void controller(int, int) {}
    void aftertouch(float) {}
    void polyAftertouch(int, float) {}
    void resetControllers() { sustain(false); }   // CC 121: the pedal back up
    void seed(uint32_t s);                   // per instance: Ground's and Bloom's random numbers
    // MPC's tempo and position (quarter notes), once per block before render(). playing falling to
    // false is a Stop.
    void setTransport(double bpm, double beats, bool playing, bool beatsValid);
    // The host stopped processing, and started again `awayS` seconds later: within kStopWindowS
    // that is a Stop (On Stop applies), longer a reset. A resume() without a suspend() does nothing.
    void suspend();
    void resume(double awayS);

    void render(float* outL, float* outR, int n);   // overwrites n samples (any n)
    int  activeVoices() const;                      // Bloom's voices in use, and Ground if it sounds

    Info info() const;
    const Harmony& harmony() const { return harmony_; }
    const Ground& ground() const { return ground_; }
    const Bloom& bloom() const { return bloom_; }

    // Test hooks: the next render() finds a NaN in the reverb's return; the sample count jumps.
    void testInjectNaN() { poison_ = true; }
    void testSetSamples(uint64_t n) { samples_ = n; }

private:
    enum KeyState : uint8_t { K_UP, K_DOWN, K_PEDAL, K_HOLD };

    // A gain gliding to its target in a straight line: aim() (at a control step) sets the step a
    // sample when the target has changed; next() and apply() take it a sample at a time, across
    // pieces, and land on it exactly.
    struct Glide {
        float now = 1.0f, target = 1.0f, to = 1.0f, step = 0.0f;
        void aim(int samples);               // to the target over `samples`, if it changed
        float next();
        void apply(float* L, float* R, int n);
        void land();                         // at the target now
        bool still() const { return step == 0.0f; }
    };

    void changed(const Patch& was);          // what a patch change does to what sounds: Hold, Listen, On Stop
    void playKey(int key, bool again);       // Bloom (Notes) plays the key's chord
    void releaseLatched(int except);         // the keys Hold latched go
    void makeRoom(bool latchedOut);          // a full harmony lets the oldest pedal or Hold key go
    void letGo(int key);                     // the key's notes released, the key gone from the harmony
    void stop();                             // On Stop
    void route();                            // Ground's target and Bloom's chord from the harmony
    void control();                          // a control step
    void clearDsp();                         // every DSP state, silent (reset() and the guard)
    void settle();                           // the output where it settles: glides landed, filters empty
    bool piece(float* outL, float* outR, int n);   // n <= kChunk samples; false: not finite
    bool output(float* L, float* R, int n);   // false: a sample isn't finite
    void tiltFor(float t);                   // the tilt's coefficients for t

    const TableSet& tables_;
    Patch p_;
    Harmony harmony_;
    Ground ground_;
    Bloom bloom_;
    Space space_;
    Space::Params spaceParams_;              // what Space is given: the patch's, the tail's hold applied
    Transport transport_;

    // Keys: down, held by the pedal or latched by Hold; the note Input mapped each to, its
    // velocity, and the chord Bloom played for it (Notes).
    KeyState key_[128] = {};
    int mapped_[128] = {};
    float keyVel_[128] = {};
    uint64_t keyAge_[128] = {};              // the note-on that put each key down: the oldest goes first
    uint64_t keysPressed_ = 0;
    Chord keyChord_[128];
    bool pedal_ = false;
    Chord prev_;                             // the chord Bloom played last (Notes): Leading's start
    float lastVel_ = 0.0f;                   // the last note-on's velocity, 0..1: Harmony and Free moves

    // Routes: what Ground and Bloom were last given.
    int groundWant_ = -1;
    uint32_t bloomVersion_ = 0;              // the harmony's version Bloom (Harmony) moved to
    bool bloomSync_ = false;                 // Bloom (Harmony) moves to the harmony's chord at the next route
    Chord freeChord_, freePlayed_;           // the tonic chord (Free), and what Bloom was moved to

    bool awake_ = false;
    bool idle_ = true;
    bool suspended_ = false;
    bool fading_ = false;
    float fadeDb_ = 0.0f;
    Glide fade_;                             // Stop's fade as a gain
    uint64_t samples_ = 0;
    uint32_t guards_ = 0;
    bool poison_ = false;

    // The mix and the output.
    float groundSend_ = 0.0f, bloomSend_ = 0.0f;
    Glide ret_;                              // the Space return
    Glide gPanL_, gPanR_, bPanL_, bPanR_;    // the strata's pans
    Glide volume_;                           // the volume as a gain
    float tilt_ = 0.0f;                      // where the tilt glides, from tiltNow_
    float tiltNow_ = 0.0f;
    float tiltG_ = 0.0f, tiltLow_ = 1.0f, tiltHigh_ = 1.0f;   // the one-pole's G, the shelf's gains
    float tiltS_[2] = {};                    // the one-pole's state, L and R
    float limitD_ = 0.0f;                    // the limiter's gain under 1 (1 - gain)
    bool limiting_ = false;

    // A piece's buffers: Ground's and Bloom's dry when panned, the sends (and Space's wet in them).
    float gL_[kChunk] = {}, gR_[kChunk] = {}, bL_[kChunk] = {}, bR_[kChunk] = {};
    float sendL_[kChunk] = {}, sendR_[kChunk] = {};
};

// Process-wide, summed over every instance, for the soak (tools/soak.cpp), which only sees the
// plugin's entry points: the guard's trips (render() calls it zeroed) and the samples the limiter's
// gain computer ran on. Relaxed atomics, one add a piece at most; they wrap, so read differences.
uint32_t guardTrips();
uint32_t limitedSamples();

} // namespace af
