// From EffectForce dsp/delay.h (7cf6e95), namespace ef -> af; Diffuse added.
#pragma once
// Delay (EffectForce's docs/DESIGN.md, "Modules"; here inside Echo, dsp/echo.h): EffectForce's probe
// delay grown up. Two delay lines, L and R, each read at its own time and fed back through the cuts,
// the diffusers, a drive and a limiter; the wet ducks under the input.
//
// Time: free (1..2000 ms) or synced (divBeats quarter notes at MPC's tempo), in double precision.
// A new target (tempo, division, automation, spread, mode) is reached one of two ways (`glide`):
// Tape glides there (glideTime() below), so the repeats bend in pitch; Fade keeps reading the old
// time and crossfades to a second read head at the new one over 50 ms, no bend. The crossfade is
// equal power: steady material keeps its level (a lone pure tone swells or dips by up to 3 dB
// halfway, as its two copies meet in or out of phase). A target that moves during a fade waits
// for it to end, then the latest one is faded to: at most two reads per side. A change under half
// a sample just snaps. R's time is L's x (1 + spread), so L stays on the grid and spread
// -0.5..0.5 makes R half as long .. half again as long. A time within a millionth of a sample of
// a whole number is made whole: a 1/16T at 100 BPM is 4410 samples, not
// 4410.000000000001, and reads the line without interpolating. The lines hold 8 s (1 bar at
// 30 BPM) plus the wow's depth; a longer time (R with spread at the slowest tempos) is clamped to
// 8 s, a shorter one to 1 ms (R: 0.5 ms).
//
// Modes. Stereo: each side its own line and feedback. Ping-Pong: the input's mono sum enters L
// only, L's output feeds R and R's feeds L, so the repeats alternate L, R, L... (R's come at L's
// time x (1 + spread) after L's). Mono: both lines get the mono sum, the same time, the same wow
// and the same diffusion, so both sides are the same (spread doesn't apply). A mode change
// crossfades the routing over one chunk.
//
// The loop, per side: read -> low-pass -> high-pass -> diffusers -> the wet, and x feedback (+ the
// input) -> drive -> limiter -> write. Repeat n has passed the cuts n times: one-pole filters,
// 6 dB / octave per pass, matched to the analog pole (a = 1 - exp(-2 pi fc / rate)); a high cut of
// 20 kHz is exactly off. The drive is a cubic soft clipper, c - 4/3 c^3 with c = u clamped to +-1/2
// (unity gain for small signals, flat at +-1/3 from +-1/2 on), blended in by the drive amount: 0 is
// exactly linear. The limiter keeps what is written under 0 dBFS (instant attack, 100 ms release,
// after a clamp at +-8 so a wild input can't hold its gain down for long). No stage has a gain
// over 1, so feedback 1 holds the repeats (the cuts still take their share each pass) and never
// runs away; drive 1 makes them fade slowly as it squares them off.
//
// Diffuse (AmbientForce's): four Schroeder allpasses per side, 7.3, 11.9, 17.1 and 23.7 ms on L,
// 8.1, 12.7, 18.3 and 24.9 ms on R (never in step), coefficient 0.65 each. Each pass blends the
// repeat with its diffused self, y + diffuse x (allpasses(y) - y), so repeat n has been through
// them n times: the repeats smear more with each pass, from a clear echo into a wash for the
// reverb. Diffuse sets the blend, not the coefficient: an allpass at coefficient 0 is no bypass
// but a pure delay of its length, so a coefficient of 0.65 x diffuse would put the repeats 60 ms
// late a pass as soon as Diffuse left 0, and back on time with a jump when it landed there. A
// blend of a signal and its allpassed self never boosts any frequency (|1 - d + d H| <= 1 when
// |H| = 1), so feedback 1 never runs away: it holds the repeats at Diffuse 0 and 1, and between
// them each pass loses up to 2.3 dB on average (at 0.5, 1.8 dB at 0.3) where the clear and the
// diffused repeat meet out of phase. Diffused energy comes the allpasses' lengths later on average
// (60 ms on L, 64 ms on R, a pass): heavily diffused repeats trail the beat. Mono's R takes L's
// diffusion. At Diffuse 0 none of it runs, and the Delay is EffectForce's bit for bit; a change
// glides across the chunk like the other ramps, and diffusers that start again (after reset() or
// after Diffuse sat at 0) start empty, so nothing old comes back out of them. The allpasses run
// four samples at a time (diffuseRun() in delay.cpp says how): about 4k ARM instructions a
// 128-sample block on top of the Delay's own 24k (dsp/echo.h has the counts).
//
// Wow: a 0.5 Hz wow (+-3 ms at wow 1) and a 6 Hz flutter (+-0.2 ms) move the read times; R's
// wow runs a quarter cycle ahead of L's and its flutter at 6.6 Hz. Under 12 ms the depth shrinks to
// a quarter of the time. Reads are cubic (4-point Hermite): linear interpolation would dull the
// repeats more wherever the wow puts them between samples.
//
// Ducking: an envelope of the input's louder side (5 ms attack, 250 ms release) sets the wet's
// gain to 1 / (1 + 16 duck env): at duck 1, -6 dB with the input at -24 dBFS, -19 dB at -6 dBFS.
// Only the wet ducks, not the loop, so the repeats bloom in the gaps.
//
// reset() is cheap (EffectForce's rack called it on the audio thread when the module came back on;
// here the engine's reset and its guard do): the lines aren't cleared. Until they have been
// written all the way round, each run of samples first zeroes the few old samples its taps can
// reach, so nothing from before ever comes out.
#include "common.h"

#include <vector>

namespace af {

class Delay {
public:
    enum Mode : int { STEREO, PING_PONG, MONO, kModes };
    enum GlideType : int { TAPE, FADE, kGlides };

    struct Params {
        int mode = STEREO;
        bool sync = true;            // time from MPC's tempo
        float timeMs = 375.0f;       // 1..2000, free
        double divBeats = 0.75;      // synced: quarter-note beats (kDelayDivs): 1/8. by default
        float feedback = 0.4f;       // 0..1: 1 holds the repeats
        float spread = 0.0f;         // -0.5..0.5: R's time is L's x (1 + spread)
        float lowCutHz = 100.0f;     // 20..2000: high-pass in the loop (so on the wet too)
        float highCutHz = 8000.0f;   // 500..20000: low-pass in the loop; 20000 = off
        float wow = 0.0f;            // 0..1: wow and flutter
        float drive = 0.0f;          // 0..1: saturation in the loop
        float duck = 0.0f;           // 0..1: the wet ducks under the input
        float mix = 0.3f;            // 0..1 dry / wet
        int glide = TAPE;            // how the time changes: Tape (pitch bends) or Fade (crossfade)
        float diffuse = 0.0f;        // 0..1: the repeats smeared, more with each pass
    };

    Delay();   // allocates the lines, 2.8 MB, and the diffusers, 23 KB (UI thread)
    void reset();
    void set(const Params& p, const Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const { return tail_; }

    // The longest anything takes to come out once it has gone in, or to go round once: the
    // longest read (either head, wherever its glide is going, at the wow's depth) and, while the
    // diffusers run, the longer side's allpasses. In and out both quiet for this long: the
    // Delay holds nothing (Echo's silent()).
    int reachSamples() const { return reach_; }
    // The wet's gain now (the duck's), so Echo can tell a quiet wet from a ducked one.
    float duckGain() const { return duck_; }
    // Whether the last process() ran the diffusers (Diffuse 0 never does): for tests.
    bool diffusing() const { return diffused_; }

    // The delay time of L (side 0) or R (side 1) in samples, without the wow: Tape's glided time;
    // in Fade the head being read (the old one until a fade ends).
    double timeSamples(int side) const { return side ? tR_ : tL_; }

    static constexpr int kStages = 4;   // the diffusers' allpasses, per side

private:
    // The values that move in straight lines across a chunk. DIFFUSE and MONO_OUT (1 in Mono: R
    // takes L's diffusion) only move anything while the diffusers run.
    enum : int { FB, DRIVE, MIX, LP, HP, MONO_IN, CROSS, R_IN, DIFFUSE, MONO_OUT, kRamps };

    // Tape: how the delay time follows a new target (a tempo or division change, automation): the
    // time one segment (32 samples) on, the samples between in a straight line. The probe's 60 ms
    // one-pole glide, as a tape delay's motor would: echoes already in flight bend in pitch while
    // the time moves, more the further it has to go. The last millionth of a sample snaps, so it
    // lands exactly. In double: in float the step drops under the precision of a time near 22050
    // samples and the glide stalls ~2.6 samples short of its target, for good. (EffectForce's
    // docs/PROBE.md, "Your part": the choice is Params::glide now; Fade is the other branch in
    // segment().)
    double glideTime(double current, double target) const {
        const double d = target - current;
        return std::fabs(d) < 1e-6 ? target : current + d * glide_;
    }

    void segment();
    void wowNow(double tl, double tr, float& l, float& r) const;
    template <bool Moving, bool Fading>
    void run(float* L, float* R, int n);          // EffectForce's loop: Diffuse 0
    template <bool Moving, bool Fading>
    void diffuseRun(float* L, float* R, int n);   // the same with the diffusers

    const double glide_;          // glideTime()'s step per segment
    std::vector<float> lineL_, lineR_;
    int w_ = 0;                   // where the next sample goes, in both lines
    int written_ = 0;             // samples written since reset(), up to the lines' length

    float cur_[kRamps] = {}, tgt_[kRamps] = {}, step_[kRamps] = {};
    float lastLowCut_ = -1.0f, lastHighCut_ = -1.0f;

    double tL_ = 1.0, tR_ = 1.0, tgtL_ = 1.0, tgtR_ = 1.0;   // glided times at the segment's end
    double teL_ = 1.0, teR_ = 1.0;                           // with the wow, this sample
    double teEndL_ = 1.0, teEndR_ = 1.0, teStepL_ = 0.0, teStepR_ = 0.0;
    int segLeft_ = 0;

    // Fade's second read head, per side: its time (the first head's while there's no fade) and
    // the two heads' gains (A L, A R, B L, B R); fadeL_ / fadeR_ count the fade's segments, -1
    // with none running.
    int glideType_ = TAPE;
    double tBL_ = 1.0, tBR_ = 1.0, teBL_ = 1.0, teBR_ = 1.0;
    double teBEndL_ = 1.0, teBEndR_ = 1.0, teBStepL_ = 0.0, teBStepR_ = 0.0;
    int fadeL_ = -1, fadeR_ = -1;
    bool fading_ = false;   // either side, this segment
    float gain_[4] = {1.0f, 1.0f, 0.0f, 0.0f}, gainEnd_[4] = {1.0f, 1.0f, 0.0f, 0.0f}, gainStep_[4] = {};

    float lpL_ = 0.0f, lpR_ = 0.0f, hpL_ = 0.0f, hpR_ = 0.0f;   // the cuts' states
    float gainL_ = 1.0f, gainR_ = 1.0f;                         // the limiter's
    float env_ = 0.0f;                                          // the input's envelope
    float duck_ = 1.0f, duckEnd_ = 1.0f, duckStep_ = 0.0f;      // the wet's gain
    float wowAmt_ = 0.0f, wowTgt_ = 0.0f, duckAmt_ = 0.0f, duckTgt_ = 0.0f, mono_ = 0.0f, monoTgt_ = 0.0f;
    float phWow_ = 0.0f, phFlutL_ = 0.0f, phFlutR_ = 0.0f;      // cycles

    // The diffusers: each allpass a ring of its own length (and a copy of its start), L's four then
    // R's in one block, and where each one is (read, then written over).
    std::vector<float> ap_;
    int apPos_[2][kStages] = {};
    bool apStale_ = true;     // they hold what came before: cleared when they next start
    bool diffused_ = false;

    int tail_ = 0;
    float tailFb_ = -1.0f;        // what tail_ was worked out for
    double tailLongest_ = -1.0, repeats_ = 1.0;
    bool tailDiffuse_ = false;
    int reach_ = 0;
    bool fresh_ = true;
};

} // namespace af
