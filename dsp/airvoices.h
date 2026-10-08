#pragma once
// Air's voices (docs/CONCEPT.md 5.3): six voices that ring once struck, and are free again when they
// have rung out. The stratum around them (Air: its generator, Listen, its level) strikes them with
// a note, a velocity and a pan; AirVoices only sounds them. Six sounds:
//
// - Glass, Bowl, Bar and Bell, the modal sounds: six modes a voice, each a decaying complex
//   one-pole, z = z c with c = r e^{iw}, its output the imaginary part. A strike sets every z to
//   the mode's amplitude a_k lp(f_k) (real: the output starts at 0 and rises as a sine), lp being
//   12 dB an octave over Tone (1 / sqrt(1 + (f / Tone)^4)); mode k falls 60 dB in decayS / d_k.
//   Four modes to a vector, two vectors a voice (lanes 6 and 7 stay 0), so a sample is four
//   multiplies and four multiply-adds a voice. A mode over 18 kHz is left out (its z and c 0). The
//   output rises over the strike's first 1.5 ms (a raised cosine, a sample at a time), so the high
//   modes, all starting at once, never click.
//
//     Sound | Ratios f_k / f                       | Amplitudes a_k                  | Decay divisors d_k
//     Glass | 1, 2.32, 4.25, 6.63, 9.38, 12.5      | 1, 0.6, 0.4, 0.25, 0.15, 0.1    | 1, 1.6, 2.4, 3.5, 4.8, 6
//     Bowl  | 1, 1.004, 2.71, 2.717, 5.12, 8.18    | 1, 0.8, 0.5, 0.4, 0.25, 0.12    | 1, 1, 1.5, 1.5, 2.2, 3
//     Bar   | 1, 2.756, 5.404, 8.933, 13.34, 18.64 | 1, 0.5, 0.25, 0.12, 0.06, 0.03  | 1, 2, 3.5, 5, 7, 9
//     Bell  | 0.5, 1, 1.183, 1.506, 2, 2.514      | 0.6, 1, 0.7, 0.5, 0.6, 0.3      | 0.6, 1, 1.4, 1.8, 2.2, 3
//
//   Bowl's close pairs beat (1.004: 1.76 Hz at 440). Bell's prime is ratio 1 (its pitch and its
//   Decay); its hum, an octave under, rings longest, Decay / 0.6. Each c is worked out in double and
//   rounded once: at Decay 20 the slowest mode's |c| is 1 - 4.7e-6, 80 of float's steps under 1, and
//   a c that rounding took to 1 would be pulled back under it. Ten seconds into a ring at Decay 20
//   the prime still falls at its rate to within 0.4%, at note 24 and at 108.
// - Kalimba: a Karplus-Strong pluck. A delay line of the period (whole samples, then a first-order
//   allpass for the fraction, which loses nothing at any frequency), a one-pole low-pass in the loop
//   and a loop gain, the line tuned so the whole loop's phase delay at the fundamental's pole is one
//   period. The pole is inside the unit circle by the fundamental's decay, where the low-pass's slope
//   would pull a loop tuned on the circle up to 2 cents flat (Tone 200, Decay 0.33). The whole
//   samples are chosen once and the allpass's coefficient solved for the rest, so nothing iterates:
//   within 0.1 cents from note 24 to 108 at Decay 0.33 to 20 and Tone 200 to 16000 (0.001 at Decay
//   4 and over), where an iteration that swapped the whole samples back and forth left notes 105 and
//   108 up to 10 cents off at short Decays. The loop gain makes up what the low-pass takes from
//   the fundamental each pass, so it falls 60 dB in decayS exactly. The low-pass sits at Tone, but
//   where Tone would take more from the fundamental than a gain under 1 can make up (a dark Tone, a
//   high note, a long Decay: at Tone 6000 a note over C6 could ring no longer than half a second)
//   its pole moves up just far enough; the loop is never louder than 1 anywhere, so it is stable,
//   and never rings longer than 60 s. It is excited by a burst of noise through two one-poles at
//   Tone (12 dB an octave, as the modes' Tone), 2 ms long or a period if that is longer (below note
//   69 a 2 ms burst would leave the loop silent between its passes: a pulse train), under a Hann
//   window, its mean taken out; the output is what goes into the line, burst and all, so it sounds
//   at once. Its level: what rings is the burst folded onto one period (over note 84 the 2 ms burst
//   makes more than two passes and gathers as it goes in), so the burst goes in at the sound's scale
//   over the RMS of the raw windowed noise so folded: about as loud at every pitch (eight strikes at
//   note 108 peak at most 2.2 dB over their mean, where the burst's own peak setting it left 3.7),
//   and a darker Tone takes its share away.
// - Felt: the Felt Piano lifetime table (TB_FELT_PIANO) read by TableOscLinear, Age moving from 0.05
//   to 0.8 over decayS (the note darkens as it fades), a 5 ms attack (a raised cosine) and a fall to
//   -60 dB at decayS. Age stands on whole frames, so the read is a single frame's (20 instructions a
//   sample, a pair's 37), and moves on across one control step, the oscillator's glide crossfading
//   the frames; it moves at most every 11.6 ms (at Decay 4 s it reaches the next frame every 29
//   steps; at 0.5 s it moves 4 frames every 16 steps). Tone doesn't apply: Felt's brightness is the
//   table's. A voice takes the table TableSet has at its first render and keeps it to the end (until
//   the table is built, the sine: plugin/tables.h), so it is never switched mid-note.
//
// Each sound's level is its own constant, set by measuring: a strike at velocity 1 at note 72, Tone
// 16000, pan 0 peaks at -6 dBFS (the Kalimba's mean over its random bursts). Glass 0.2350, Bowl
// 0.1760, Bar 0.2610, Bell 0.1740 times the modes' amplitudes; the Kalimba's burst at 0.218 over its
// fold's RMS; Felt 0.4018 times the table. From note 24 to 108 they peak between -5 and -9.5 dBFS
// (the modes thin out high up; the pluck is set by its RMS, and its crest is lower where its burst
// makes several passes).
//
// The voices:
// - A strike takes a free voice, else the quietest ringing one (by its level now), which fades out
//   over 2 ms (linearly) before the new note starts in it, on its sample. If every voice is already
//   fading for a steal, the strike replaces the note waiting longest.
// - Velocity is a gain of 0.25 + 0.75 vel (a soft strike is still heard); the pan is equal power,
//   unity in the middle, sqrt 2 at the sides. AirVoicePatch::width is Air's: it draws each pan
//   within it, so AirVoices takes the pan as given.
// - A note rings as it was struck: set() changes the next strikes, not a note ringing (its sound,
//   Tone, Decay and tuning).
// - A voice's level, for the quietest and for sleeping, is the most it can play now: the modes'
//   |z| summed; the pluck's peak, held falling at the fundamental's rate and pushed up by what each
//   step played; Felt's envelope times a frame's peak. A voice under -90 dBFS (that level times its
//   louder side) for 32 samples is free and isn't rendered: a strike that rings out sleeps by 1.3 to
//   1.4 decayS (Bell 1.2 times its hum's ring). With every voice free, render() only looks at them.
// - Control rate: each voice works out its gains once per step of at most kChunk samples (ending
//   where a steal's fade does) and moves them in a straight line across it, so nothing steps.
// - The send is the dry at spaceSend, gliding across a render from the last render's (the first
//   render after reset() takes it as given).
//
// Denormals: a voice stops being rendered at -90 dBFS; a fast mode can pass under float's normal
// range while its voice rings on, which NEON (and the plugin's flush on x86) makes 0.
//
// The cost, as ARM instructions per 128-sample block with six voices ringing (qemu's count, the
// device's flags), rendered in the engine's 32-sample pieces / in whole blocks: the modal sounds
// 19.4k / 18.7k (18.5 a voice and sample in the modes' loop), Kalimba 24.2k / 23.4k, Felt 29.5k /
// 28.8k at Decay 20, 30.0k at Decay 4 and 30.6k at Decay 0.5 struck again every 0.3 s (its moves
// between frames). Felt is the dearest, inside Air's 32k (docs/plans/2026-10-07-m2-weather.md, the
// budget): about 0.75 points of the 15% gate by its 0.0245 points a thousand (device: pending).
// Asleep, a render looks at the six voices and returns. A strike costs, once, in its block: Felt
// 0.5k, a modal sound 2.4k (its six coefficients in double), the pluck 9.5k in the middle of the
// keyboard and about 76k at note 24, where its burst is a period, 1349 samples at 54 instructions a
// sample (Air's generator plays from C4 up; only played notes go that low).
//
// Real-time rules: everything is fixed-size (the pluck's line and burst, 16 KB a voice). Nothing
// allocates, locks or throws after the constructor.
#include "common.h"
#include "harmony.h"
#include "lifeosc.h"
#include "lifetime.h"
#include "simd.h"
#include "svf.h"

#include <cstdint>

namespace af {

enum AirSound : int { AS_GLASS, AS_BOWL, AS_BAR, AS_BELL, AS_KALIMBA, AS_FELT, AS_COUNT };
// The surface's names for them (harmony.h says why).
inline constexpr const char* kAirSoundNames[] = {"Glass", "Bowl", "Bar", "Bell", "Kalimba", "Felt"};
static_assert(sizeof kAirSoundNames / sizeof *kAirSoundNames == AS_COUNT, "a name per sound");

struct AirVoicePatch {
    int sound = AS_GLASS;
    float toneHz = 6000.0f;    // 200..16000: the strike's brightness (the modes' and the pluck's low-pass)
    float decayS = 4.0f;       // 0.1..20: the fundamental's ring to -60 dB
    float width = 0.7f;        // 0..1: how far apart the notes' pans may fall (Air draws them; see above)
};

class AirVoices {
public:
    static constexpr int kVoices = 6;     // the cap: 6 -> 4 if the device bench asks
    static constexpr int kModes = 6;
    static constexpr int kMaxBlock = 128; // render's n at most (more is rendered kMaxBlock at a time)

    enum Stage : int { VS_FREE, VS_RING, VS_STEAL };
    // What the tests see of a voice.
    struct VoiceView {
        int stage = VS_FREE;
        int note = -1;        // the note ringing (in VS_STEAL the one fading out); -1: free
        int next = -1;        // VS_STEAL: the note waiting for the fade to end
        int sound = AS_GLASS; // the sound it was struck with
        float level = 0.0f;   // its level as the last control step ended (velocity in, pan not)
    };

    AirVoices();
    void seed(uint32_t s);
    void set(const AirVoicePatch& p, const HarmonyPatch& h);   // h: the tuning (tunedPitch)
    // A note now (MIDI 24..108, tuned by h), vel 0..1, pan -1..1: a free voice, else the quietest
    // (a voice taken fades out over 2 ms before the new note starts in it).
    void strike(int note, float vel, float pan);
    void reset();
    // Adds the dry into outL/outR and the send at spaceSend into sendL/sendR. n <= 128.
    void render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR,
                float spaceSend, int n);
    int active() const;        // voices ringing above -90 dBFS (or fading for a steal)
    VoiceView voice(int i) const;

private:
    static constexpr int kLine = 2048;            // the pluck's delay line: periods down to 21.5 Hz
    static constexpr int kLineMask = kLine - 1;
    static constexpr int kBurstMax = kLine - 2 * kChunk;   // the longest burst (a period at note 24 is 1349)

    // A strike as it was given, with the patch as it was then: a note rings as struck.
    struct Strike {
        int note = -1;
        int sound = AS_GLASS;
        float vel = 0.0f, pan = 0.0f, toneHz = 6000.0f, decayS = 4.0f;
        double pitch = 0.0;      // tuned, fractional MIDI
        uint64_t order = 0;      // strikes before it: the oldest waiting note is the one replaced
    };

    struct Voice {
        // Modal: the modes four to a vector, each a complex state z and its rotation c a sample
        // (real and imaginary parts apart). Lanes 6 and 7, and a mode over 18 kHz, stay at 0.
        f4 zr[2] = {}, zi[2] = {}, cr[2] = {}, ci[2] = {};
        f2 pan = {};             // L, R: the pan's gains (unity in the middle) times the velocity's
        f2 P = {};               // the output's gains as the last control step ended
        int stage = VS_FREE;
        Strike now, next;        // the note ringing, and (VS_STEAL) the one waiting for the fade
        int stealLeft = 0;       // samples of the steal's fade to go
        int quiet = 0;           // samples under -90 dBFS so far
        // Samples since the note began, held at 2^30 (airvoices.cpp's kAgeMost): no note rings that
        // long (Bell's hum at Decay 20 sleeps after about 40 s), and 32 bits convert to float
        // without a library call on ARMv7.
        int age = 0;
        float vel = 0.0f;        // the velocity's gain, 0.25 + 0.75 vel
        float panMax = 1.0f;     // the louder side's pan gain
        float level = 0.0f;      // the mono level as the last step ended, velocity in
        // Pluck.
        int w = 0, delay = 1;    // where the next sample goes; the read's whole samples behind it
        int burstLen = 0;        // samples of the burst
        float eta = 0.0f, ap = 0.0f;                 // the allpass: its coefficient and its state
        float lpA = 0.0f, lp = 0.0f, loopG = 0.0f;   // the loop's low-pass coefficient and state; its gain
        float env = 0.0f, holdLog2 = 0.0f;           // the held level, and its fall a sample (log2)
        // Felt.
        TableOscLinear osc;
        const Wavetable* table = nullptr;            // taken at the note's first render, kept to its end
        float inc = 0.0f, ageStep = 0.0f, fallLog2 = 0.0f;   // pitch; Age and the decay a sample
        int frame = -1;                              // the frame Age stands on (-1: none yet)
        float framePos = 0.0f;                       // ... as the read's position, 0..1
        int moved = 0;                               // samples since Age last moved (up to kFeltMove)
        float burst[kLine] = {};                     // the pluck's excitation, then zeros
        float line[kLine] = {};                      // the pluck's delay line
    };

    void begin(Voice& v, const Strike& s);             // the note starts in v now
    void beginModal(Voice& v, const Strike& s, double hz);
    void beginPluck(Voice& v, const Strike& s, double hz);
    void beginFelt(Voice& v, const Strike& s, double hz);
    void renderBlock(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR,
                     float spaceSend, int n);
    void voiceStep(Voice& v, const Wavetable& felt, int o, int m);   // one control step of m samples

    AirVoicePatch p_;           // kept in range
    HarmonyPatch h_;
    uint32_t seed_ = 1;
    uint32_t rng_ = 1;          // the pluck's noise
    uint64_t strikes_ = 0;
    float send_ = -1.0f;        // the last render's spaceSend (-1: none since reset())
    Voice v_[kVoices];
    f2 bus_[kMaxBlock] = {};    // the voices' dry, L and R side by side
    // Felt's read for a step: a member, not a local (a local array costs every step a stack guard).
    float read_[kChunk] = {};
};

} // namespace af
