#pragma once
// Ground (docs/CONCEPT.md 5.1): the drone. One voice per instance, not one per note, so it costs
// little and can hold forever. The engine decides what it follows (Listen); Ground only follows
// setTarget().
//
// - Five partials, Sub, Root, Fifth, Octave and Color, each a TableOsc (Hermite) on the Ground
//   table, all at one LifeScan position. Sub is an octave under the root and Octave one above.
//   Under Just and Pythagorean the Fifth is 3/2 and Color its tuning's ratio for the interval
//   (Just: m3 6/5, M3 5/4, 4th 4/3, m7 9/5, 9th 9/4, 11th 8/3; Pythagorean, its stacked fifths:
//   32/27, 81/64, 4/3, 16/9, 9/4, 8/3); under Equal they are 7 and 3, 4, 5, 10, 14, 17 semitones.
//   A partial at level 0 isn't rendered.
// - The root: the target's pitch class in octave Register (C1 = 24, C2 = 36, C3 = 48), tuned by
//   tunedPitch(). The target's own octave doesn't count: Register places it.
// - Beat: partial i moves by beatHz * kBeat[i] in Hz, not cents, so the partials beat at the same
//   rate in every register. A harmonic moves with its partial, k times as far as the fundamental:
//   where Root's 3rd harmonic meets Fifth's 2nd they beat at |2 kBeat[Fifth] - 3 kBeat[Root]| =
//   2.5 times beatHz. In Just the ratios are exact, so the beating dialled in is all there is.
// - Gravity: the root glides to a new target in semitones (log pitch), exponentially with time
//   constant gravityS / 3, so it is 95% there after gravityS. Gravity 0 jumps. A target that
//   starts the drone from silence is jumped to, never glided to.
// - Fade: linear in dB from -60 to 0 over fadeS when the drone starts, and back when it stops
//   (target -1). At -60 it stops rendering and sounding() turns false. A new target while it fades
//   out fades it back in from where it is.
// - Body: two band-pass SVFs on a vowel's first two formants, mixed with the dry: 0..1/3 fades
//   in "a" (800 / 1150 Hz), 1/3..2/3 moves it to "o" (450 / 800), 2/3..1 on to "u" (350 / 600).
//   It shapes the drone toward a choir; at 0 it costs nothing.
// - Tone: a gentle low-pass SVF (Q 0.707, no peak) after the sum, in stereo.
// - Breath: a sine LFO at breathHz moves the level by +-3 dB and the cutoff by +-1 octave, both
//   times breath.
// - Width: partial i is panned to width * kPan[i], equal power (unity in the middle, as
//   PolyForce pans).
//
// Levels: `level` is a gain, not the knob. The patch map squares the knob (the family's audio
// taper), as it does for every level, so Ground doesn't. The send is taken after level, fade and
// breath: level 0, mute or silence send nothing.
//
// Control rate: once per chunk of kChunk samples the glide, fade, LifeScan, breath, cutoff, body
// and the partials' pitches take one step; the gains ramp across the chunk and the filters glide
// (svf.h), so nothing steps. A render of any n is cut into chunks of at most kChunk, each one
// control step of its own length.
//
// Cost, as ARM instructions per 128-sample block (qemu's count, the device's flags): every
// partial on 60k, the default drone (four partials) 50k, Root alone 20k; Body adds 5.6k and
// Breath 2.9k (it keeps the Tone's cutoff gliding). By PolyForce's ~1 ns an instruction that is
// 2.1%, 1.7% and 0.7% of a block, over CONCEPT.md 11's 0.8%. Each partial is about 10k, most of
// it the Hermite read of a frame pair; reading Sub, Octave and Color linearly (TableOscLinear)
// would save about 12k. The device bench (Task 11) has the final word.
//
// Real-time rules: everything is fixed-size. Nothing allocates, locks or throws after the
// constructor.
#include "common.h"
#include "harmony.h"
#include "lifeosc.h"
#include "lifetime.h"
#include "svf.h"

#include <cstdint>

namespace af {

enum ColorInterval : int { CI_MIN3, CI_MAJ3, CI_FOURTH, CI_MIN7, CI_NINTH, CI_ELEVENTH, CI_COUNT };

struct GroundPatch {
    int listen = LI_HARMONY;        // the engine's: Ground itself only follows setTarget()
    bool mute = false;
    float level = 0.7f;             // a gain, 0..1 (the patch map applies the audio taper)
    float cutoffHz = 2500.0f;       // Tone: low-pass, 40..16000
    int table = TB_CELLO_TASTO;
    LifePos pos{0.5f, 0.3f, 0.05f, 0.0f};
    float beatHz = 0.3f;            // 0..3: how fast the partials beat against each other
    float gravityS = 6.0f;          // 0..30: the glide to a new root (95% there after gravityS)
    float fadeS = 4.0f;             // 0.05..30: fade in when it starts, fade out when it stops
    float sub = 0.3f, root = 1.0f, fifth = 0.5f, octave = 0.25f, color = 0.0f;   // partial levels 0..1
    int colorInterval = CI_NINTH;
    int registerOct = 2;            // the root's octave: 1 (C1..B1), 2 (C2..), 3 (C3..)
    float body = 0.0f;              // 0: off; 0..1/3 fades in an "a" vowel; then a -> o -> u
    float breath = 0.3f;            // 0..1: a slow swell of level and brightness
    float breathHz = 0.07f;         // 0..5
    float width = 0.5f;             // 0..1: the partials spread across the stereo field
};

class Ground {
public:
    enum Partial : int { PT_SUB, PT_ROOT, PT_FIFTH, PT_OCTAVE, PT_COLOR, PT_COUNT };
    // Hz per Hz of Beat, and the place in the stereo field per unit of Width, per partial.
    static constexpr float kBeat[PT_COUNT] = {0.0f, -0.5f, 0.5f, -0.25f, 0.25f};
    static constexpr float kPan[PT_COUNT] = {0.0f, -0.3f, 0.3f, -0.6f, 0.6f};

    Ground();
    // The random numbers (the scan's sway phase and smear, the oscillators' and the breath's start
    // phases), then reset(): the same seed and the same calls play the same samples.
    void seed(uint32_t s);
    void set(const GroundPatch& p, const HarmonyPatch& h);
    // A MIDI note: its pitch class is what counts (Register gives the octave). -1 (any negative):
    // stop, fading out.
    void setTarget(int rootNote);
    // Silent, no target, every phase back to where the seed puts it. The patch stays.
    void reset();
    // Adds into outL/outR, and adds the send into sendL/sendR at `spaceSend` (a gain, 0..1). n <= 128
    // (any n works; it is cut into control chunks). Silent: returns at once.
    void render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR,
                float spaceSend, int n);
    // Fading in, held or fading out: on, whatever the level or mute (muted it costs only its
    // control steps, and unmuted it is there again).
    bool sounding() const { return sounding_; }

    // For tests and the engine's Info: the target note (-1: none), and the root's pitch now and
    // where it is gliding to, in fractional semitones (the octave from Register, tuned).
    int target() const { return target_; }
    double pitch() const { return pitch_; }
    double goal() const { return goal_; }

private:
    double rootPitch(int note) const;   // the tuned pitch Register and the tuning put `note` at
    void stop();                        // the fade has reached -60: silent, ready to start afresh
    void chunk(const Wavetable& t, float* outL, float* outR, float* sendL, float* sendR, float spaceSend, int n);

    // The patch, kept in range.
    HarmonyPatch h_;
    bool mute_ = false;
    int table_ = TB_CELLO_TASTO;
    LifePos pos_{};
    float level_ = 0.7f, cutoff_ = 2500.0f, beat_ = 0.3f, gravity_ = 6.0f, fade_ = 4.0f;
    float body_ = 0.0f, breath_ = 0.3f, breathHz_ = 0.07f;
    int register_ = 2;
    float ratio_[PT_COUNT] = {};        // each partial's frequency over the root's
    float gainL_[PT_COUNT] = {}, gainR_[PT_COUNT] = {};   // level times pan, where set() aims them
    double glideKeep_ = 0.0;            // what of the distance to the goal a full chunk leaves

    // State.
    uint32_t seed_ = 1;
    LifeScan scan_;
    TableOsc osc_[PT_COUNT];
    float curL_[PT_COUNT] = {}, curR_[PT_COUNT] = {};     // each partial's gains as the last chunk ended
    int target_ = -1;
    double pitch_ = 0.0, goal_ = 0.0;   // the root now and where it glides to (semitones)
    bool sounding_ = false;
    bool fresh_ = true;                 // the next chunk starts from silence: glides jump, no ramps
    float fadeDb_ = -60.0f;
    double breathPhase_ = 0.0;          // cycles, 0..1
    float gain_ = 0.0f, send_ = 0.0f;   // the output's gain and the send's, as the last chunk ended
    float wet_ = 0.0f;                  // Body's share (0..1) as the last chunk ended
    Glide<1, 1> tone_;                  // the Tone's g, gliding in octaves
    Glide<2, 2> formant_;               // Body's two formants' g
    SvfState toneSvf_, f1Svf_, f2Svf_;
};

} // namespace af
