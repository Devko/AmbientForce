#pragma once
// Ground (docs/CONCEPT.md 5.1): the drone. One voice per instance, not one per note, so it costs
// little and can hold forever. The engine decides what it follows (Listen); Ground only follows
// setTarget().
//
// - Five partials, Sub, Root, Fifth, Octave and Color, all on the Ground table at one LifeScan
//   position. Sub is an octave under the root and Octave one above. Under Just and Pythagorean
//   the Fifth is 3/2 and Color its tuning's ratio for the interval (Just: m3 6/5, M3 5/4, 4th 4/3,
//   m7 9/5, 9th 9/4, 11th 8/3; Pythagorean, its stacked fifths: 32/27, 81/64, 4/3, 16/9, 9/4,
//   8/3); under Equal they are 7 and 3, 4, 5, 10, 14, 17 semitones.
// - Every partial reads linearly (TableOscLinear), half the reads of Hermite: a drone lives low,
//   and the linear read's images stay far down there. On Saw (the brightest table) at the highest
//   root, B3, with the Tone open, they are at most -74 dB under Root's fundamental, -70.5 dB under
//   the Fifth's (at 11.2 kHz) and -66 dB under Color's 11th (654 Hz, at 14.2 kHz); lifeosc.h holds
//   the linear read to -60 dB, and lifetime frames, quieter at the top, leave less. A partial at
//   level 0 isn't read; it is skipped along, so it comes back in step.
// - The root: the target's pitch class (its octave doesn't count), tuned by tunedPitch(). From
//   silence it sits in Register's octave (C1 = 24, C2 = 36, C3 = 48). A new root while it sounds
//   takes the octave of its pitch class nearest where the root is now, the shortest glide (B to C
//   rises a semitone), kept within base - 6 .. base + 17, base being Register's C.
// - Register changed while it sounds doesn't glide: the drone dips out over kDip samples (40 ms),
//   moves by the octaves at the bottom, and comes back over as long.
// - Beat is the rate, in Hz, at which Root and Fifth beat where they meet: Root's 3rd harmonic
//   against Fifth's 2nd. Each partial moves by beatHz * kBeat[i] in Hz, not cents, so the rate is
//   the same in every register; a harmonic moves k times as far as its fundamental, so Root at
//   -0.2 and Fifth at +0.2 meet at 2 (0.2) + 3 (0.2) = 1 times beatHz. The others keep the same
//   shape (the plan's offsets, scaled by 0.4): Sub's 2nd against Root beats at 0.2 times it, Root's
//   2nd against Octave at 0.3, Fifth's 4th against Octave's 3rd at 1.1: a few slower and similar
//   rates around the one Beat sets. In Just the ratios are exact: the beating dialled in is all
//   there is.
// - Gravity: the root glides to a new target in semitones (log pitch), exponentially with time
//   constant gravityS / 3, so it is 95% there after gravityS. Gravity 0 jumps.
// - Fade: linear in dB from -60 to 0 over fadeS when the drone starts, and back when it stops.
//   Its states: off (sounding() false, nothing rendered); in or held (a target: the level rises to
//   0 dB and stays); out (target -1: it falls, and at -60 dB it is off). A new target while it
//   fades out turns it around where it is.
// - Body: two band-pass SVFs on a vowel's first two formants, mixed with the dry: 0..1/3 fades
//   in "a" (800 / 1150 Hz), 1/3..2/3 moves it to "o" (450 / 800), 2/3..1 on to "u" (350 / 600).
//   It shapes the drone toward a choir, lifting a harmonic on a formant by 4.7 dB at most and
//   keeping the level within 2.5 dB of the dry's; at 0 it costs nothing.
// - Tone: a gentle low-pass SVF (Q 0.707, no peak) after the sum, in stereo. Its coefficients are
//   worked out once per control step: Breath moves the cutoff at most 0.023 octaves a step. A
//   cutoff that moves more than kToneJump octaves in a step (a jump of the knob) glides there
//   across the step instead, evenly in octaves, the coefficients worked out for every sample: an
//   opening from 40 Hz to 16 kHz in one step would otherwise let the drone's top in at once.
// - Breath: a sine LFO at breathHz moves the level by +-3 dB and the cutoff by +-1 octave, both
//   times breath.
// - Width: partial i is panned to width * kPan[i], equal power (unity in the middle, as
//   PolyForce pans): Sub in the middle, Root and Octave to the left, Fifth and Color to the right.
// - A new table (another one chosen, or the slot's table published over the sine it played
//   until it was built) fades in over kTableFade samples (20 ms), both tables read only for that
//   long: a switch never steps. The old one is read after the switch: published tables stay while
//   the instance lives (plugin/tables.h).
//
// Levels: `level` is a gain, not the knob. The patch map squares the knob (the family's audio
// taper), as it does for every level, so Ground doesn't. The partials' sum is scaled by
// kHeadroom (ground.cpp says how it was set). The send is taken after level, fade and breath:
// level 0, mute or silence send nothing.
//
// Mute: render() may be skipped while !audible() (the engine's idle gate); Ground then stands
// still, its fades, glides and scans paused where they were. audible() stays true after a mute or
// a level of 0 until the gain has ramped down (the next step), so a skipped Ground has always
// reached silence first, and unmuted it ramps up from there: no click either way. Rendered while
// muted it is cheap too (nothing is read: the oscillators are skipped along), and its fades,
// glides and scans keep running, so unmuted it is where it would have been.
//
// Control rate: once per control step of kChunk samples the glide, fade, dip, LifeScan, breath,
// Tone, Body and the partials' pitches take one step; the gains ramp across the step and Body's
// formants glide (svf.h), so nothing steps. A render of any n is cut into steps of at most kChunk,
// each a control step of its own length.
//
// Cost, as ARM instructions per 128-sample block (qemu's count, the device's flags): every
// partial on 40.0k, the default drone (Sub, Root, Fifth, Octave) 34.3k, Root alone 17.0k; each
// partial about 5.8k, Body 5.3k more, Breath 0.6k (the Tone's coefficients, once a step). A table
// change reads both tables for its 20 ms. By PolyForce's ~1 ns an instruction that is 1.4%, 1.2%
// and 0.6% of a block, against CONCEPT.md 11's 0.8%; the device bench (Task 11) has the final word.
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
    // Hz per Hz of Beat (Root against Fifth beats at exactly Beat), and the place in the stereo
    // field per unit of Width, per partial.
    static constexpr float kBeat[PT_COUNT] = {0.0f, -0.2f, 0.2f, -0.1f, 0.1f};
    static constexpr float kPan[PT_COUNT] = {0.0f, -0.3f, 0.3f, -0.6f, 0.6f};
    static constexpr int kTableFade = 882;   // 20 ms: a new table fades in over this many samples
    static constexpr int kDip = 1764;        // 40 ms: a Register change dips out, and back, over this
    static constexpr float kToneJump = 0.125f;   // octaves in a step past which the Tone glides

    Ground();
    // The random numbers (the scan's sway phase and smear, the oscillators' and the breath's start
    // phases), then reset(): the same seed and the same calls play the same samples.
    void seed(uint32_t s);
    // Cheap enough per block: what needs a libm call (the glide's rate, Equal's ratios, the pans) is
    // worked out again only when its input changes.
    void set(const GroundPatch& p, const HarmonyPatch& h);
    // A MIDI note: only its pitch class counts (the octave is chosen as above). -1 (any negative):
    // stop, fading out. The same pitch class again changes nothing.
    void setTarget(int rootNote);
    // Silent, no target, every phase back to where the seed puts it. The patch stays.
    void reset();
    // Adds into outL/outR, and adds the send into sendL/sendR at `spaceSend` (a gain, 0..1). n <= 128
    // (any n works; it is cut into control steps). Off: returns at once. It may be skipped while
    // !audible() (see Mute above).
    void render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR,
                float spaceSend, int n);
    // Fading in, held or fading out, muted or not.
    bool sounding() const { return sounding_; }
    // Something can be heard, or is still ramping down: sounding, and either the gain hasn't reached
    // 0 yet or it is neither muted nor at level 0. While it is false, render() may be skipped.
    bool audible() const { return sounding_ && (gain_ > 0.0f || (!mute_ && level_ > 0.0f)); }

    // For tests and the engine's Info: the target note (-1: none); the MIDI note the root sits on
    // (its octave chosen; -1: off); the root's pitch now and where it glides to, in fractional
    // semitones (tuned).
    int target() const { return target_; }
    int note() const { return note_; }
    double pitch() const { return pitch_; }
    double goal() const { return goal_; }

private:
    // What one control step works out for its samples: the gains at its two ends, Body's share,
    // the read position, the partials' pitches.
    struct Step {
        int n = 0;
        bool ending = false;        // the fade-out reaches -60 dB in this step
        bool fromSilence = false;   // the output's gain ended the last step at 0 (see chunk())
        float g0 = 0.0f, g1 = 0.0f, s0 = 0.0f, s1 = 0.0f, w0 = 0.0f, w1 = 0.0f;
        float pos = 0.0f;
        float inc[PT_COUNT] = {};
        bool toneGlides = false;    // the cutoff jumps: g from toneG0, times toneRate a sample
        float toneG0 = 0.0f, toneRate = 1.0f;
    };

    int base() const { return 12 * (register_ + 1); }   // Register's C
    void shiftRegister();                // at the bottom of a dip: the root moves by the octaves
    void stop();                         // the fade has reached -60: off, ready to start afresh
    void chunk(const Wavetable& want, float* outL, float* outR, float* sendL, float* sendR, float spaceSend, int n);
    Step control(float spaceSend, int n);
    void partials(const Step& s, const Wavetable& want, float* accL, float* accR);
    void filters(const Step& s, const float* accL, const float* accR, float* yL, float* yR);
    void output(const Step& s, const float* yL, const float* yR, float* outL, float* outR, float* sendL, float* sendR);
    void silent(const Step& s, const Wavetable& want);   // a step with nothing to hear: skip along

    // The patch, kept in range.
    HarmonyPatch h_;
    bool mute_ = false;
    int table_ = TB_CELLO_TASTO;
    LifePos pos_{};
    float level_ = 0.7f, cutoff_ = 2500.0f, beat_ = 0.3f, gravity_ = 6.0f, fade_ = 4.0f;
    float body_ = 0.0f, breath_ = 0.3f, breathHz_ = 0.07f;
    int wantRegister_ = 2;               // the Register asked for; register_ follows it (a dip)
    float ratio_[PT_COUNT] = {};         // each partial's frequency over the root's
    float gainL_[PT_COUNT] = {}, gainR_[PT_COUNT] = {};   // level times pan, where set() aims them
    // set()'s caches: what its libm calls were last worked out for.
    float gravityFor_ = -1.0f, widthFor_ = -1.0f;
    int ratiosFor_ = -1;
    double glideKeep_ = 0.0;             // what of the distance to the goal a full step leaves
    float panL_[PT_COUNT] = {}, panR_[PT_COUNT] = {};

    // State.
    uint32_t seed_ = 1;
    LifeScan scan_;
    TableOscLinear osc_[PT_COUNT];       // per partial (PT_SUB .. PT_COLOR)
    // A table change: the oscillators as they were at the switch go on reading the old table while
    // it fades out.
    TableOscLinear fadeOsc_[PT_COUNT];
    const Wavetable* table0_ = nullptr;  // the table played (faded to)
    const Wavetable* fadeFrom_ = nullptr;   // the table fading out; nullptr: none
    int tableFade_ = 0;                  // samples of the fade still to go
    float curL_[PT_COUNT] = {}, curR_[PT_COUNT] = {};     // each partial's gains as the last step ended
    int target_ = -1, note_ = -1;
    int register_ = 2;                   // the Register the root is placed by now
    double pitch_ = 0.0, goal_ = 0.0;    // the root now and where it glides to (semitones)
    bool sounding_ = false;
    bool fresh_ = true;                  // the drone starts from off: its first step puts the root on its goal
    float fadeDb_ = -60.0f;
    float dip_ = 1.0f;                   // a Register change's gain: 1 none, 0 the bottom
    double breathPhase_ = 0.0;           // cycles, 0..1
    float gain_ = 0.0f, send_ = 0.0f;    // the output's gain and the send's, as the last step ended
    float wet_ = 0.0f;                   // Body's share (0..1) as the last step ended
    float toneHz_ = -1.0f;               // the cutoff toneUpdate_ was worked out for
    float toneG_ = 0.0f;                 // its g
    SvfUpdate toneUpdate_{};
    Glide<2, 2> formant_;                // Body's two formants' g
    SvfState toneSvf_, f1Svf_, f2Svf_;
};

} // namespace af
