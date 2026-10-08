#pragma once
// Weather (docs/CONCEPT.md 5.4): a cloud of up to 16 grains over one fixed source (dsp/grainsrc.h):
// a procedural field, a WAV, or Memory's remembered 16 s. The grains are EffectForce's Grain voices
// (its Cloud and Stretch), reading a source that never changes under them instead of a live
// recording, plus Stream, plain playback that drifts. The engine decides when Weather sounds
// (Listen: gate()) and which pitch classes To Key Chord takes (setChord()).
//
// The anchor: where in the source the grains gather, `position` (0 its start, 1 its end) plus a
// wander, both in shares of the source, wrapping. Every control step the wander moves at
// drift^2 / 20 of the source a second times a direction that glides (0.5 s) to a new random target
// every 1 to 3 s, targets of +-0.5..1; at +-0.5 it turns back. Drift 1 crosses the source in about
// 20 s, Drift 0 keeps it still.
//
// The modes. Every grain is Hann-windowed (sin^2 up for half its length, down for the other half).
// - Cloud: grains of `sizeS` at intervals of sizeS / grains, each jittered +-30%, starting at the
//   anchor +- a scatter (uniform within +-spray^2 x 2 s, at most half the source), panned at
//   random within +-width, backwards with probability `reverse`. At 1 / sqrt(0.375 grains): 0.375
//   is a Hann window's mean square, so uncorrelated grains keep the source's loudness at any count.
// - Stretch: a head that starts at the anchor and crawls through the source at 1/8 of real time
//   (time stretched 8 times, pitch untouched); grains of `sizeS`, `grains` overlapping at
//   intervals jittered +-15%, start just behind it (up to 5 + 60 x spray ms), panned and reversed
//   as Cloud's, at Cloud's level.
// - Stream: the source as it is. Two grains of sizeS (at least 0.2 s, an even number of samples)
//   at exactly half a grain apart, each starting where the read point has got to (the anchor plus
//   the samples since Stream began, wrapping), centred. Two windows half a grain apart add up to
//   exactly 1 (sin^2 + cos^2), so at pitch 0 with Drift 0 consecutive grains read the same
//   samples and play the source itself, seams and all: a grain at rate 1 plays at 1. Transposed
//   (Pitch or To Key: a granular shift, the read point still moving at rate 1) a grain reads ahead
//   of or behind its neighbour, the two are uncorrelated, and their overlap keeps only 3/4 of the
//   power on average: such a grain plays at sqrt(4/3), Cloud's law for two. Spray, Reverse and
//   Width don't apply (a pan per grain would make neighbours differ and the joins dip on one side);
//   Drift moves the read point, so a drifting Stream's neighbours read slightly different places
//   (inaudible on fields, a slow phasing on a tone).
//
// A grain's transposition t in semitones, drawn when it starts:
// - To Key Off: `pitch`, plus up to +-6 cents x spray of random detune in Cloud.
// - Scale or Chord: a pitch class q drawn uniformly from the scale's (key and scale, from the
//   HarmonyPatch) or from setChord()'s (none given: the scale's), then t = round(pitch) + d with d
//   in -6..+5 and t = q mod 12: every source's root is C (plan decision 5). No detune. A t over +24
//   (pitch over +19) moves down an octave, EffectForce's fold: the levels serve up to two octaves.
// Its rate is 2^(t / 12) frames a sample. Faster than 1.26 it reads level 1 at half that, faster than
// 2.2 level 2 at a quarter (EffectForce's thresholds), so what a shift would push past 22 kHz was
// filtered out before (85 dB down): +24 never aliases.
//
// The reads: EffectForce's, cubic (4-point Hermite), two samples a vector, the 16-bit samples
// converted as they load, the taps' differences taken as integers (the source's gain folded into
// the grain's). A grain's window runs on a recurrence, exact to 3e-6 (weather.cpp says how; checked).
// Positions in double; each grain reads its level's ring from `origin` on (position 0 is the
// source's start), wrapping once a control step through the guard frames, so a grain stays inside
// the source's loop however long it reads. The source's frames need not be its ring's capacity (a
// Memory not yet full).
//
// After the grains, in stereo: the output's tilt shelf (TiltShelf, dsp/svf.h: one first-order shelf
// pivoting at 800 Hz, +-6 dB at the ends, bypassed at 0, gliding as the output's does: the whole
// range in 50 ms) and a one-pole high-pass (bypassed at 20 Hz; its cutoff glides 1/4 octave a control
// step). Coefficients once a control step.
//
// Duck: an envelope of the duckPeak given with each render(), rising toward a louder peak with a
// 50 ms time constant and falling 60 dB in 1.5 s (a 0.22 s time constant: 3 s after Bloom stops
// Weather is back within 0.01 dB, where a 1.5 s time constant would still hold it 6 dB down), sets
// the gain 1 / (1 + 16 duck env): at duck 1, -6 dB under a peak at -24 dBFS, -19 dB at -6 dBFS
// (EffectForce's Delay's law). It glides across the call.
//
// The gate: gate(true) fades in from -60 dB to 0 over kGateS, linear in dB; gate(false) fades out
// the same way, and at -60 dB every grain stops and nothing is rendered (audible() false) until
// the gate opens again, when Stream and Stretch start from the anchor afresh. Grains go on starting
// while it fades. With nothing to hear (level 0 or muted, the glide down done) there is nothing to
// fade: turned off then, or reaching it while it fades out, the gate closes at once. The engine
// may be skipping Weather, and a fade left part done would play out when the level came back.
//
// Changes: Pitch, To Key, Size, Grains, Spray, Reverse and Width take effect with the next grain. A
// mode change fades every grain out over 20 ms (EffectForce's mode fade) while the new mode's start.
// A source change (another source, or none: nullptr or !ready()) does the same, and the grains
// fading out of the old source read a copy of what they have left to read, taken at the change
// (at most 1280 frames each): the old source may be freed or rewritten as soon as render() returns.
// reset() stops every grain at once and closes the gate, as Ground's and Bloom's resets do (the
// engine's reset, its guard, Stop's fade at -60 dB, CC 120 all go silent at once).
//
// Level, mute and the send, as Ground's: `level` is a gain (the patch map squares the knob); the
// level and mute glide in a straight line over 10 ms; the send is taken after level, gate and duck.
// Once the glide down has reached 0 (or the gate has closed: audible() false) every grain stops
// and the source is forgotten. The engine may skip Weather from then on, and the loader or a
// Remember may free the source meanwhile, so no grain may keep reading it: one that did would be
// copied from the freed source at the next change. Rendered on anyway, it reads nothing, starts
// no grain and only takes its control steps. Brought back (the level raised, unmuted) it takes its
// source afresh at the next render(), as when the gate opens: the cloud builds up again from its
// first grain, Stream and Stretch start from the anchor, under the level's 10 ms glide up.
//
// Control rate: a render of any n is cut into steps of at most kChunk, each a control step of its
// own length, as Ground's (the engine's pieces sit on its own 32-sample grid, cut only by MIDI
// events, so the same events play the same samples whatever MPC's blocks). Each step the gate, the
// level's glide, the wander, the tilt and the high-pass move on by its length; the gains ramp
// across it. Grains start on the sample their interval puts them on (in Weather's own time, 64-bit),
// whatever the steps.
//
// Cost, as ARM instructions per 128-sample block (qemu's count, the device's flags, a 12 s source,
// rendered in the engine's pieces of 32): the plan's worst case (Grains 16, Cloud, +12 so level-1
// reads, To Key Chord, tilt, HP and Duck on) 58.3k, against the plan's 60k; Stretch 58.7k,
// Stream 13.0k, without tilt, HP and Duck 56.3k, 8 grains 33.6k, 1 grain 9.8k. A grain costs 3.4k
// (27 instructions a sample: 16.5 the reads, 4.5 the positions and window, the rest its step's
// setup), the rest 6.3k. Grains 16 keeps about 15.3 sounding on average (the intervals' jitter
// against the cap); a block with all 16 would be about 61k. By the plan's 0.0245 points of p99 a
// thousand: 1.4 points (device: pending). A block where the source changes costs about 28k more
// (the earlier review's count): the grains' copies of what they have left to read, up to 16 x 1280
// frames. Level 0 was 3.5k while its grains still moved on unread; they stop now, so it is less
// (not measured again), and the engine may skip it altogether.
//
// Real-time rules: everything is fixed-size, allocated in the constructor (the grains' copy room,
// 86 KB). Nothing allocates, locks or throws after it.
#include "common.h"
#include "grainsrc.h"
#include "harmony.h"

#include <cstdint>
#include <vector>

namespace af {

enum WeatherMode : int { WM_CLOUD, WM_STRETCH, WM_STREAM, WM_COUNT };
inline constexpr const char* kWeatherModeNames[] = {"Cloud", "Stretch", "Stream"};
enum ToKey : int { TK_OFF, TK_SCALE, TK_CHORD, TK_COUNT };
inline constexpr const char* kToKeyNames[] = {"Off", "Scale", "Chord"};
static_assert(sizeof kWeatherModeNames / sizeof *kWeatherModeNames == WM_COUNT &&
                  sizeof kToKeyNames / sizeof *kToKeyNames == TK_COUNT,
              "a name per value");

struct WeatherPatch {
    int listen = LI_FREE;      // the engine's
    bool mute = false;
    float level = 0.0f;        // a gain (the knob squared): Init has Weather asleep
    bool memory = false;       // the source is Memory's remembered 16 s (the plugin sets it from the source's key)
    int mode = WM_CLOUD;
    float position = 0.5f;     // 0..1: where in the source the cloud sits (0: its start)
    float drift = 0.2f;        // 0..1: how fast that point wanders (0 still; 1 across the source in ~20 s)
    float spray = 0.3f;        // 0..1: how far grains scatter around it (± spray^2 x 2 s, at most half the source)
    float sizeS = 0.25f;       // 0.02..2: a grain's length
    int grains = 8;            // 1..16: how many overlap
    float pitch = 0.0f;        // -24..+24 semitones
    int toKey = TK_OFF;
    float reverse = 0.0f;      // 0..1: the chance a grain plays backwards
    float width = 0.7f;        // 0..1: how far the grains' pans spread
    float tilt = 0.0f;         // -1..1: ±6 dB at the ends, pivot 800 Hz (the output's shelf)
    float hpHz = 20.0f;        // 20..2000: a high-pass; 20 is off
    float duck = 0.0f;         // 0..1: Weather recedes while Bloom plays
};

class Weather {
public:
    static constexpr int kGrains = 16;           // the cap: 16 -> 12 if the device bench asks
    static constexpr float kGateS = 2.0f;        // the gate's fade in and out
    static constexpr int kModeFade = 882;        // 20 ms: a mode or source change fades the grains out
    static constexpr int kCopyFrames = 1280;     // what a grain fading out of an old source may still read

    Weather();                                   // allocates the copy room (UI thread)
    // The random numbers (where grains fall, their pans, pitches and timing, the wander), then
    // reset(): the same seed and the same calls play the same samples.
    void seed(uint32_t s);
    void set(const WeatherPatch& p, const HarmonyPatch& h);
    void setChord(uint16_t pcs);                 // To Key Chord's pitch classes (the engine's, by Listen)
    void gate(bool on);                          // sounding or not (Listen): fades over kGateS
    void reset();
    // src: what the grains read (nullptr or !ready(): they fade out and none start), looked at only
    // while audible(). A different source than the last call's fades every grain out over 20 ms
    // and starts afresh.
    // duckPeak: Bloom's dry peak over this piece (|L|, |R| largest), for Duck.
    // Adds the dry into outL/outR, the send at spaceSend into sendL/sendR. n <= 128.
    void render(const GrainSource* src, float duckPeak, float* outL, float* outR, float* sendL, float* sendR,
                float spaceSend, int n);
    // Gated on, or still fading out, and its level (or the level's glide down) above 0. It doesn't
    // look at the source: a source arriving while the engine skipped Weather would never be seen.
    // With none, a render only takes its control steps. Once it is false Weather holds no pointer
    // into any source (every grain has stopped, the source is forgotten), so render() may be
    // skipped and the source freed meanwhile; only set() or gate() makes it true again.
    bool audible() const { return !closed_ && (gain_ > 0.0f || (!mute_ && level_ > 0.0f)); }

    // For the tests: grains sounding now, and a hook called with each grain's transposition
    // (semitones) as it starts (nullptr: none; the plugin never sets one).
    int grainsOn() const;
    using SpawnHook = void (*)(void* ctx, float semis);
    void setSpawnHook(SpawnHook hook, void* ctx) {
        hook_ = hook;
        hookCtx_ = ctx;
    }

private:
    // A grain. Its envelope's ramp a(t) = min(t slope, (end - t) slope, (relEnd - t) relSlope, 1),
    // clamped to 0..1, makes the gain sin^2(pi / 2 a): with slope 2 / end, the Hann window
    // sin^2(pi t / end). A release (a mode or source change) adds the third term from where the
    // envelope is, always at a step's first sample.
    struct Voice {
        double start = 0.0;              // its ring position (level frames) at its own time 0
        double rate = 0.0;               // level frames a sample (< 0: backwards)
        const int16_t* data = nullptr;   // the level it reads (interleaved L R, then the guard), or its copy
        int frames = 0;                  // that ring's frames (a copy's: its length, never wrapped)
        int t = 0;                       // its own time at the next step's first sample (< 0: starts later)
        int end = 0;                     // its own time when the natural envelope reaches 0
        int relEnd = 0;                  // and when a release does
        float slope = 0.0f, relSlope = 1.0f;
        float phi = 0.0f, k = 0.0f, dk = 0.0f;   // pi / end, 4 sin^2(2 phi), 2 sin(2 phi): the window's recurrence
        // Where it left off: sin(phi t) at the next step's t .. t + 3, and the step that took it there.
        alignas(16) float sin[4] = {}, dsin[4] = {};
        int exactIn = 0;                 // whole steps it may still run before it starts exactly afresh
        float gd[2] = {}, gx[2] = {};    // L and R: the gains of its own side and of the other one
        bool on = false;
        bool copied = false;             // reading its copy of an old source
    };

    void control(int m);
    void step(int m, float d0, float dStep, float* outL, float* outR, float* sendL, float* sendR);
    void startGrains(int m);
    void spawn(int k, double pos, float semis, bool back, int length, float pan, float amp);
    float transposition(float detune, float u);
    void release(Voice& v, int fade);
    void changeSource(const GrainSource* src);
    void silence();
    void begin();
    void renderVoice(Voice& v, int m);
    template <bool Cross>
    void readVoice(const Voice& v, int k0, int groups);
    float uniform();

    // The patch, kept in range.
    HarmonyPatch h_;
    bool mute_ = false;
    int mode_ = WM_CLOUD, grains_ = 8, toKey_ = TK_OFF;
    float level_ = 0.0f, position_ = 0.5f, drift_ = 0.2f, spray_ = 0.3f, sizeS_ = 0.25f, pitch_ = 0.0f;
    float reverse_ = 0.0f, width_ = 0.7f, tilt_ = 0.0f, hpHz_ = 20.0f, duck_ = 0.0f;
    float cloudAmp_ = 1.0f;              // 1 / sqrt(0.375 grains), for grainsFor_
    int grainsFor_ = -1;
    // The pitch classes To Key draws from: the scale's (from the HarmonyPatch), setChord()'s.
    int keyPcs_[12] = {}, nKeyPcs_ = 0, chordPcs_[12] = {}, nChordPcs_ = 0;

    // The source as the last render() had it (a copy of its fields: a change shows even when a
    // GrainSource is rewritten in place), and whether it was ready.
    GrainSource src_;
    bool hasSrc_ = false;

    // State.
    uint32_t seed_ = 1, rng_ = 1;
    Voice voice_[kGrains];
    std::vector<int16_t> copy_;          // kGrains x kCopyFrames frames: the copies of an old source
    uint64_t now_ = 0;                   // samples rendered since reset() (64 bit: days on end)
    double nextSpawn_ = 0.0;             // when the next grain starts (samples since reset())
    uint64_t began_ = 0;                 // when Stream or Stretch began reading from the anchor
    // Stream's last grain: none since it began, at rate 1 (the next may join it), or transposed;
    // and the anchor it started from.
    enum StreamJoin : int { SJ_NONE, SJ_JOINS, SJ_SHIFTED };
    int stream_ = SJ_NONE;
    double streamAnchor_ = 0.0;
    bool dropAll_ = false;               // a mode change: every grain fades out at the next step
    // The wander (shares of the source), its direction, where that glides, and when it turns next.
    float wander_ = 0.0f, dir_ = 0.0f, dirTarget_ = 0.0f, dirLeftS_ = 0.0f;
    double anchor_ = 0.0;                // this step's anchor, a share of the source (0..1)

    // The gate and the gains: the level's glide, the gate in dB, the output's gain and the send's
    // as the last step ended and where this one ends.
    bool gateOn_ = false, closed_ = true, ending_ = false;
    float gateDb_ = -60.0f;
    float levelNow_ = 0.0f, levelStep_ = 0.0f, levelTo_ = 0.0f;   // the level glides from now to To, Step a sample
    float gain_ = 0.0f, gain0_ = 0.0f, send_ = 0.0f, send0_ = 0.0f, spaceSend_ = 0.0f;
    float duckEnv_ = 0.0f, duckGain_ = 1.0f;   // Duck's envelope, and its gain as the last call ended

    // The filters: the tilt's glide and coefficients, the high-pass's, their states (L R).
    float tiltNow_ = 0.0f, tiltFor_ = 0.0f, tiltHigh_ = 1.0f, tiltLow_ = 1.0f, tiltG_ = 0.0f;
    float hpNow_ = 20.0f, hpFor_ = 0.0f, hpG_ = 0.0f;
    float tiltS_[2] = {}, hpS_[2] = {};

    SpawnHook hook_ = nullptr;
    void* hookCtx_ = nullptr;

    // A step's grains, staged: the sum (L R interleaved; whole groups of four may run 3 samples past
    // the step); per sample a read's first int16 (its address on the device); per group of four
    // samples their fractions, then their envelopes, so one pointer walks both.
    alignas(16) float acc_[2 * (kChunk + 4)];
    alignas(16) int idx_[kChunk + 8];
    alignas(16) float stage_[2 * (kChunk + 8)];
};

} // namespace af
