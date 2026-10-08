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
//   the fundamental's pole is within 0.04 cents of the note from note 24 to 108 at Decay 0.33 to 20
//   and Tone 200 to 16000, and within 0.43 at Decay 0.1 (an iteration that swapped the whole samples
//   back and forth left notes 105 and 108 up to 10 cents off at short Decays). The loop gain makes
//   up what the low-pass takes from the fundamental each pass, so it falls 60 dB in decayS.
//   That gain is worked out from the low-pass's response on the unit circle while the tuning is done
//   at the pole, so a short Decay at a dark Tone falls a little off it: the fall within 4.8% of Decay
//   at Decay 0.1 and Tone 200 (the plan allows 10), 1.8% at 0.33, 1.3% from 0.5 up (left so). The
//   low-pass sits at Tone, but where Tone would take more from the fundamental than a gain under 1
//   can make up (a dark Tone, a high note, a long Decay: at Tone 6000 a note over C6 could ring no
//   longer than half a second) its pole moves up just far enough; the loop is never louder than 1
//   anywhere, so it is stable, and never rings longer than 60 s.
//   It is excited by a burst of noise through two one-poles at Tone (12 dB an octave, as the modes'
//   Tone), 2 ms long or a period if that is longer (below note 69 a 2 ms burst would leave the loop
//   silent between its passes: a pulse train), under a Hann window, its mean taken out; the output
//   is what goes into the line, burst and all, so it sounds at once.
//   The mean is taken with the weights of the loop's DC mode (its real pole p, just under 1: the
//   burst's sum of b_n p^-n is 0), so nothing is left to ring as an offset: it is over 93 dB under
//   the fundamental. Kept for that: a plain mean left the mode as little as 25 dB under the
//   fundamental (note 24, Decay 0.5), falling no faster than the note, an offset into the send and a
//   step on a steal's fade. What it costs: where the mode falls fast, the weights lean on the burst's
//   end and take a little of the fundamental with the mean, so a strike's fundamental moves, against
//   the plain mean, by up to 2.9 dB at Decay 0.1 (notes 24 to 48; 0.5 at note 60, 0.13 at 70) and 0.86
//   dB at Decay 0.5 (notes 24 and 36; 0.18 at 48), 0.35 dB lower on average at Decay 0.1 and note 24,
//   and within 0.12 dB from Decay 4 up.
//   Its level: what rings is the burst folded onto one period (over note 84 the 2 ms burst makes
//   more than two passes and gathers as it goes in). A 2 ms burst (note 72 and up) is built whole at
//   the strike and goes in at the sound's scale over the RMS of its raw windowed noise so folded:
//   about as loud at every pitch (eight strikes at note 108 peak at most 2.2 dB over their mean,
//   where the burst's own peak setting it left 3.7), and a darker Tone takes its share away. A
//   longer one (a period, note 71 and under) is built as it plays, so that its strike costs only a
//   pass over its noise for the mean (the cost, below). Its scale is therefore the RMS the windowed
//   noise has on average, sqrt(len / (8 fold)) (the noise's square averages 1/3 and the window's
//   fourth power 3/8), not the one it turns out to have, and the noise's luck moves its level from
//   strike to strike by about 5.4 / sqrt(len) dB (one standard deviation): 0.15 dB at note 24, 0.3
//   at 48, 0.55 at 71, on top of what the filtered noise varies anyway. That was accepted so the
//   strike's cost is spread over the samples that play it. Against the burst built whole, with the
//   same noise, the first period's mean level is within 0.1 dB at every Tone and Decay.
// - Felt: the Felt Piano lifetime table (TB_FELT_PIANO) read by TableOscLinear, Age moving from 0.05
//   to 0.8 over decayS (the note darkens as it fades), a 5 ms attack (a raised cosine) and a fall to
//   -60 dB at decayS. Age stands on whole frames, so the read is a single frame's (20 instructions a
//   sample, a pair's 37), and moves on across one control step, the oscillator's glide crossfading
//   the frames; it moves at most every 11.6 ms (at Decay 4 s it reaches the next frame every 29
//   steps; at 0.5 s it moves 4 frames every 16 steps). Tone doesn't apply: Felt's brightness is the
//   table's. A voice takes the table TableSet has at its first render and keeps it to the end (until
//   the table is built, the sine: plugin/tables.h), so it is never switched mid-note. It keeps the
//   table's pointer from one render to the next, so the table must outlive it, as plugin/tables.h
//   promises while an instance lives: an AirVoices still around after releaseTables() must be
//   reset() before it renders again.
//
// Each sound's level is its own constant, set by measuring: a strike at velocity 1 at note 72, Tone
// 16000, pan 0 peaks at -6 dBFS (the Kalimba's mean over its random bursts). Glass 0.2350, Bowl
// 0.1760, Bar 0.2610, Bell 0.1740 times the modes' amplitudes; the Kalimba's burst at 0.218 over its
// fold's RMS (a long burst's: its expected RMS); Felt 0.4018 times the table. From note 24 to 108
// they peak between -4.4 and -9.5 dBFS, a strike each (the modes thin out high up; the pluck is set
// by its RMS, and its crest is lower where its burst makes several passes).
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
//   step played (while a long burst is still going in, the most that burst can play); Felt's
//   envelope times a frame's peak. A voice under -90 dBFS (that level times its louder side, at a
//   step's start and end) for 32 samples is free and isn't rendered: a strike that rings out sleeps
//   by 1.3 to 1.5 decayS (the Kalimba's counted from its burst's end; Bell 1.2 times its hum's
//   ring). With every voice free, render() only looks at them.
// - Control rate: each voice works out its gains once per step of at most kChunk samples (ending
//   where a steal's fade does, and a long burst's first pass) and moves them in a straight line
//   across it, so nothing steps.
// - The send is the dry at spaceSend, gliding across a render from the last render's (the first
//   render after reset() takes it as given).
//
// Denormals: a voice stops being rendered at -90 dBFS; a fast mode can pass under float's normal
// range while its voice rings on, which NEON (and the plugin's flush on x86) makes 0.
//
// The cost, as ARM instructions per 128-sample block with six voices ringing (qemu's count, the
// device's flags), rendered in the engine's 32-sample pieces / in whole blocks: the modal sounds
// 19.7k / 18.9k (18.5 a voice and sample in the modes' loop), Kalimba 20.9k / 20.1k, Felt 29.7k /
// 29.0k at Decay 20, 30.1k at Decay 4 and 30.7k at Decay 0.5 struck again every 0.3 s (its moves
// between frames). Felt is the dearest, inside Air's 32k (docs/plans/2026-10-07-m2-weather.md, the
// budget; that share is for ringing): about 0.82 points of the 15% gate at the budget's 0.0268
// points a thousand (device: pending). Asleep, a render looks at the six voices and returns.
//
// Strikes are spikes on top of that, and bounded: at most twelve notes begin in a block, six in
// strike() (into free voices) and six more where a steal's 2 ms fade ends in the next render
// (voiceStep() begins the waiting note there; a strike while every voice is fading replaces a
// waiting note rather than adding one). A strike costs, once: Felt 0.24k, a modal sound 1.74k (its
// six coefficients in double), the pluck 4.1k to 6.5k from note 71 up. Under that the pluck's burst
// is a period (1349 samples at note 24): the strike draws it once for its mean, 21 instructions a
// sample (30.6k at note 24, where building it whole took 72k), and it is drawn again as it plays,
// 15 a sample over the ring, so six plucks struck at note 24 cost 32.6k / 31.8k a block for the ten
// blocks until their bursts have gone in. The dearest blocks, with their render: note-24 plucks
// struck in them, one 37.3k, six 216k, twelve 402k: about 1, 5.8 and 10.8 points (device: pending),
// well under a block's time but over Air's share. Air's generator plays from C4 up (a pluck there
// costs 5.8k a strike), so only notes played to Air go that low. The bench's `air strike` (Task 8)
// counts one through the engine: six plucks struck from C1 (24 to 33) cost 210.6k in their block
// and 10.0k a block for the ten blocks their bursts take.
//
// Real-time rules: everything is fixed-size (the pluck's line, 8 KB a voice, and its burst's 120
// samples). Nothing allocates, locks or throws after the constructor.
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
        int burst = 0;        // the pluck's burst: samples of it drawn so far, of burstLen
        int burstLen = 0;
        // The pluck's loop as it runs: the read's whole samples, the allpass's coefficient, the
        // low-pass's (s += a (y - s)) and the loop gain, so the tests can find its poles.
        int loopDelay = 0;
        float loopEta = 0.0f, loopA = 0.0f, loopG = 0.0f;
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
    // render()'s first half, for a caller that mixes the voices itself (Air: its level, pan and
    // sends in one pass): n samples of the voices' dry into bus() from its sample `from` (from + n
    // <= kMaxBlock), L and R side by side, those samples cleared first. The same control steps as
    // render() of n. False with every voice free (the samples only cleared).
    bool renderBus(const TableSet& tables, int from, int n);
    const f2* bus() const { return bus_; }
    // Voices ringing above -90 dBFS (or fading for a steal).
    int active() const {
        int n = 0;
        for (const Voice& v : v_) n += v.stage != VS_FREE ? 1 : 0;
        return n;
    }
    VoiceView voice(int i) const;

private:
    static constexpr int kLine = 2048;            // the pluck's delay line: periods down to 21.5 Hz
    static constexpr int kLineMask = kLine - 1;
    // The longest read is kLine - 4 samples (the read's whole samples at most kLine - 5), and a burst
    // is at most a period, so a long burst always outlasts its first pass. Only a bound: a period at
    // note 24 is 1349 samples.
    static constexpr int kBurstMax = kLine - 4;
    // 2 ms: the shortest burst, and the longest built whole at its strike (see above).
    static constexpr int kBurst = 88;

    // A strike as it was given, with the patch as it was then: a note rings as struck.
    struct Strike {
        int note = -1;
        int sound = AS_GLASS;
        float vel = 0.0f, pan = 0.0f, toneHz = 6000.0f, decayS = 4.0f;
        double pitch = 0.0;      // tuned, fractional MIDI
        uint64_t order = 0;      // strikes before it: the oldest waiting note is the one replaced
    };

    // A long burst's noise, drawn as the burst goes in: where the noise, its two one-poles and the
    // window's phasor stand, and the mean and scale the strike worked out for it.
    struct Noise {
        uint32_t rng = 1;
        float lp1 = 0.0f, lp2 = 0.0f;   // the one-poles at Tone
        float c = 1.0f, s = 0.0f;       // the window's phasor at the next sample (cos, sin)
        float tc = 1.0f, ts = 0.0f;     // its turn a sample
        float tone = 0.0f;              // the one-poles' coefficient
        float mean = 0.0f;              // the windowed noise's mean (the DC mode's weights)
        float scale = 0.0f;             // the burst's gain
        float peak = 0.0f;              // the largest sample gone in so far
        int made = 0;                   // samples built (a short burst's at the strike)
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
        // A long burst's first pass, its samples written straight into the line: until here (the
        // read's whole samples) the loop reads nothing back. 0: a short burst, built at the strike.
        int firstPass = 0;
        float most = 0.0f;       // a long burst's level until it has all gone in: the most it can play
        Noise noise;
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
        // The pluck's excitation as the loop adds it, then zeros: a short burst whole; a long one's
        // samples for a step once its first pass is over.
        float burst[kBurst + kChunk] = {};
        float line[kLine] = {};                      // the pluck's delay line
    };

    void begin(Voice& v, const Strike& s);             // the note starts in v now
    void beginModal(Voice& v, const Strike& s, double hz);
    void beginPluck(Voice& v, const Strike& s, double hz);
    void pluckBurst(Voice& v, f2 dP, f2* bus, int m);  // a step of the pluck while its burst goes in
    static float drawBurst(Noise& z, float* out, int m);   // a long burst's next m samples
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
