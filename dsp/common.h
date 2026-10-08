// From EffectForce dsp/common.h (4160e87), namespace ef -> af.
#pragma once
// What every module shares: MPC's rate, the control chunk, the transport, sync divisions,
// smoothing, interpolation and a delay line. The module contract (EffectForce's docs/DESIGN.md,
// "Modules"; Space keeps it for the Reverb inside):
//
//   Module();                                       allocates (UI thread)
//   void reset();                                   clears all state; the next set() jumps to its targets
//   void set(const Params& p, const Transport& t);  once per chunk, before process()
//   void process(float* L, float* R, int n);        in place, 1 <= n <= kChunk
//   int  tailSamples() const;                       how long it rings after the input stops (0: none)
//
// No allocation, locks or exceptions in set / process; finite output for any finite input and any
// parameters; a NaN or infinity in the input never gets into a feedback path (sanitize()).
#include "fastmath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace af {

constexpr float kRate = 44100.0f;   // MPC OS always runs 44.1 kHz
constexpr int kChunk = 32;          // control rate: set() once per chunk of at most this many samples

// MPC's transport at the chunk's first sample.
struct Transport {
    double bpm = 120.0;     // always usable: 120 when MPC doesn't say
    double beats = 0.0;     // song position in quarter notes (valid only if `valid`)
    bool playing = false;
    bool valid = false;
};

// Sync divisions in quarter-note beats, shortest first (a Q-Link turn walks them in order). The
// names are the options surface.py shows, same order.
struct Division {
    const char* name;
    double beats;
};
inline constexpr Division kDelayDivs[] = {
    {"1/64", 0.0625}, {"1/32T", 1.0 / 12}, {"1/32", 0.125}, {"1/16T", 1.0 / 6}, {"1/16", 0.25}, {"1/8T", 1.0 / 3},
    {"1/16.", 0.375}, {"1/8", 0.5}, {"1/4T", 2.0 / 3}, {"1/8.", 0.75}, {"1/4", 1.0}, {"1/2T", 4.0 / 3},
    {"1/4.", 1.5}, {"1/2", 2.0}, {"1/2.", 3.0}, {"1 bar", 4.0},
};
inline constexpr int kNumDelayDivs = static_cast<int>(sizeof kDelayDivs / sizeof kDelayDivs[0]);
inline constexpr Division kLfoDivs[] = {
    {"1/16", 0.25}, {"1/8T", 1.0 / 3}, {"1/8", 0.5}, {"1/4T", 2.0 / 3}, {"1/8.", 0.75}, {"1/4", 1.0},
    {"1/4.", 1.5}, {"1/2", 2.0}, {"1/2.", 3.0}, {"1 bar", 4.0}, {"2 bars", 8.0}, {"4 bars", 16.0},
    {"8 bars", 32.0}, {"16 bars", 64.0},
};
inline constexpr int kNumLfoDivs = static_cast<int>(sizeof kLfoDivs / sizeof kLfoDivs[0]);

// The strata's synced cycles (Ground's Breath, the sways): Free runs at the rate knob, Sync one cycle
// per division of 4/4 bars, a quarter note to 64 bars. The names are surface.py's options, in order.
inline constexpr const char* kSyncNames[] = {"Free", "Sync"};
inline constexpr const char* kBarDivNames[] = {"1/4", "1/2", "1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars",
                                               "32 Bars", "64 Bars"};
inline constexpr float kBarDivBeats[] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f, 256.0f};
inline constexpr int kNumBarDivs = static_cast<int>(sizeof kBarDivBeats / sizeof kBarDivBeats[0]);
static_assert(sizeof kBarDivNames / sizeof kBarDivNames[0] == kNumBarDivs, "a name per division");

// Where a synced cycle stands on a BeatClock (BeatClock::cycle): its phase, 0..1, and how far it
// moves in a control step, in cycles.
struct ClockCycle {
    double phase = 0.0, adv = 0.0;
};

// A stratum's beat count for its synced cycles: the engine's one count (Engine::clockStrata), set
// before each render at its first sample. That is MPC's song position while the transport plays,
// and on from where it was at the tempo while it is stopped, the same for every stratum whether it
// sounds or not, so the strata keep to one grid. The stratum moves it on by each control step's
// samples (advance()) and reads its cycles at the step's end.
struct BeatClock {
    double beats = 0.0;   // quarter notes
    double bpm = 120.0;
    void set(double tempo, double songBeats) {
        bpm = std::isfinite(tempo) && tempo >= 1.0 ? tempo : 120.0;
        if (std::isfinite(songBeats)) beats = songBeats;
    }
    void advance(int samples) { beats += static_cast<double>(samples) * bpm / (60.0 * static_cast<double>(kRate)); }
    // A synced cycle, one per division of divBeats quarter notes, the division doubled until the cycle
    // runs no faster than maxHz at this tempo (a sway at 1/4 runs 4 Hz at 240 BPM, and at 300 BPM
    // 2.5 Hz, as 1/2): where it stands now, and how far it moves in `seconds`.
    ClockCycle cycle(double divBeats, double maxHz, double seconds) const {
        const double perSecond = bpm / 60.0;   // quarter notes
        double b = divBeats > 0.0 ? divBeats : 1.0;
        while (perSecond > maxHz * b) b *= 2.0;
        const double inv = 1.0 / b, c = beats * inv;
        return {c - floorFast(c), perSecond * inv * seconds};
    }
};
constexpr double kMaxSyncSwayHz = 4.0;     // a synced sway runs no faster (a free one tops out at 2 Hz)
constexpr double kMaxSyncBreathHz = 8.0;   // a synced breath no faster (Breath Rate tops out at 8.96 Hz free)

// A cycle's phase (0..1) drawn to where it should be, so it never jumps. `cur` moves on by `adv`
// cycles (the target's own speed: following a target that moves leaves no lag), then a share of
// what is left, the short way round, with a time constant of kPhasePullS: MPC starting, locating or
// looping, or Free <-> Sync, glides instead of stepping, close after about 50 ms. Within 1e-9 of
// the target it lands on it exactly, so a locked cycle is exactly the clock's and a free one
// exactly its own; from half a cycle away that takes about a second (0.05 s x ln(0.5 / 1e-9)).
// The arguments of its floors stay within -2..2: floorFast, no library call.
constexpr double kPhasePullS = 0.05;
// 1 - e^-x by its series, which a constant can use (std::exp can't), for x well under 1: at a
// control step's x, 0.0145, the terms fall under a double's last bit after eight (at x = 5 twenty
// terms would still be 1e-5 off).
constexpr double oneMinusExpNeg(double x) {
    double sum = 0.0, term = -1.0;
    for (int k = 1; k <= 20; ++k) {
        term *= -x / k;
        sum += term;
    }
    return sum;
}
// A full control step's share of what is left, worked out once: every step is one but those a MIDI
// event or a strum cuts short, and exp() costs about 70 ARM instructions. The others work theirs
// out with expm1, which agrees with the series to the last bit.
constexpr double kPullSamples = static_cast<double>(kRate) * kPhasePullS;   // the time constant
constexpr double kStepPull = oneMinusExpNeg(kChunk / kPullSamples);
static_assert(kChunk / kPullSamples < 0.1, "the series is for x well under 1");
// `samples`: the step's length (a negative one counts as 0).
inline double pullPhase(double cur, double adv, double want, int samples) {
    const double next = cur + adv;
    double err = want - next;
    err -= floorFast(err + 0.5);   // -0.5..0.5
    if (std::fabs(err) < 1e-9) return want;
    const double share =
        samples == kChunk ? kStepPull : -std::expm1(-static_cast<double>(std::max(samples, 0)) / kPullSamples);
    const double to = next + err * share;
    return to - floorFast(to);
}

// One of a stratum's cycles, Ground's Breath or a sway: a phase of its own, moving on at its free
// rate every step (Free's target), and the phase it plays, pulled toward its target each step
// (pullPhase): synced, the clock's, `offset` cycles on (Bloom staggers its voices by it, the breath
// tops on the downbeat by it); free, its own. A cycle nobody steps (a stratum or a voice not
// rendered) stands still, its own phase too, as a free cycle always has. So the first step after
// silence lands on the target at once (land()) instead of gliding from where it stood: there is
// nothing to hear jump, and a glide would start a synced cycle off its place. A free cycle has no
// place to start on but its own phase, and goes on from where it stood.
class PulledCycle {
public:
    // maxHz: a synced cycle's division doubles until the cycle runs no faster. offset: where it sits
    // against the clock, in cycles (not finite: 0).
    explicit PulledCycle(double maxHz, double offset = 0.0) : maxHz_(maxHz) { setOffset(offset); }
    void setOffset(double cycles) { offset_ = std::isfinite(cycles) ? cycles - std::floor(cycles) : 0.0; }

    void reset(double p) {
        phase_ = own_ = p;
        landing_ = false;
    }
    void land() { landing_ = true; }   // the next step puts the phase on its target
    // One control step, `samples` long; returns the phase. ownAdv: how far its own phase moves, in
    // cycles, worked out by the caller in its own arithmetic (each keeps what its Free has always
    // played, bit for bit). divBeats > 0: synced, a cycle per divBeats quarter notes on `clock`
    // (moved on to this step's end).
    double step(double ownAdv, int samples, const BeatClock& clock, double divBeats) {
        const bool onOwn = phase_ == own_;
        own_ += ownAdv;
        own_ -= floorFast(own_);
        if (divBeats > 0.0) {
            const double seconds = static_cast<double>(samples) / static_cast<double>(kRate);
            const ClockCycle c = clock.cycle(divBeats, maxHz_, seconds);
            double want = c.phase + offset_;
            want -= floorFast(want);
            phase_ = landing_ ? want : pullPhase(phase_, c.adv, want, samples);
        } else {
            // On its own phase it stays there: the pull would land on it exactly (the same sum, so
            // nothing is left to pull), and a free step costs about what it did before the pull.
            phase_ = landing_ || onOwn ? own_ : pullPhase(phase_, ownAdv, own_, samples);
        }
        landing_ = false;
        return phase_;
    }
    double phase() const { return phase_; }
    double own() const { return own_; }

private:
    double maxHz_, offset_ = 0.0;
    double phase_ = 0.0;     // the phase it plays, cycles 0..1
    double own_ = 0.0;       // its own phase, 0..1
    bool landing_ = false;
};

// A synced time's length in seconds (the Delay's sync: dsp/delay.cpp).
inline double divSeconds(double beats, double bpm) { return beats * 60.0 / bpm; }

// --- levels -----------------------------------------------------------------------------------

inline float dbToGain(float db) { return exp2Fast(db * 0.166096404744f); }   // log2(10) / 20
inline float gainToDb(float g) { return g > 1e-10f ? 6.02059991328f * log2Fast(g) : -200.0f; }
inline float sanitize(float x) { return std::isfinite(x) ? x : 0.0f; }

// --- smoothing --------------------------------------------------------------------------------

// The per-sample coefficient of a one-pole smoother with time constant `seconds`.
inline float smoothCoef(float seconds) { return 1.0f - std::exp(-1.0f / (seconds * kRate)); }

// A value that moves in a straight line to its target over one chunk: set() calls to(), process()
// calls next() per sample. jump() for the first set() after reset().
class Ramp {
public:
    void jump(float v) {
        cur_ = target_ = v;
        step_ = 0.0f;
        left_ = 0;
    }
    void to(float target, int n) {
        target_ = target;
        if (cur_ == target) {   // there already: it stays (a ramp of 0 steps gives the same values)
            step_ = 0.0f;
            left_ = 0;
            return;
        }
        left_ = std::max(n, 1);
        step_ = (target_ - cur_) / static_cast<float>(left_);
    }
    float next() {
        if (left_ > 0) {
            cur_ += step_;
            if (--left_ == 0) cur_ = target_;
        }
        return cur_;
    }
    float value() const { return cur_; }
    float target() const { return target_; }
    bool moving() const { return left_ > 0; }   // false: next() returns value() throughout

private:
    float cur_ = 0.0f, target_ = 0.0f, step_ = 0.0f;
    int left_ = 0;
};

// A gain that moves in a straight line to its target over `samples` from wherever it is when the
// target changes, taken on a piece at a time (AmbientForce's: Air's and Weather's pans and Echo
// sends, which glide as the engine's own gains do). A step under half an ulp of `now` lands, as one
// past the target does.
struct LineGlide {
    float now = 0.0f, to = 0.0f, step = 0.0f;
    void aim(float target, int samples) {
        if (target == to) return;
        to = target;
        step = (to - now) / static_cast<float>(samples);
    }
    void land() {
        now = to;
        step = 0.0f;
    }
    float move(int n) {   // on by n samples; where it is then
        if (now != to) {
            const float x = now + step * static_cast<float>(n);
            now = x == now || (step > 0.0f ? x >= to : x <= to) ? to : x;
        }
        return now;
    }
    bool still() const { return now == to; }
};

// --- interpolation and delay lines --------------------------------------------------------------

// 4-point, 3rd-order Hermite between x0 (t = 0) and x1 (t = 1).
inline float hermite(float xm1, float x0, float x1, float x2, float t) {
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// A mono delay line, power-of-two sized: write one sample, read any number of taps behind it.
// Delays are in samples, measured from the sample just written (delay 1 = the previous one).
class DelayLine {
public:
    explicit DelayLine(int minSize = 1) { resize(minSize); }

    void resize(int minSize) {   // allocates: UI thread only
        int n = 1;
        while (n < minSize + 4) n <<= 1;
        buf_.assign(static_cast<size_t>(n), 0.0f);
        mask_ = n - 1;
        w_ = 0;
    }
    int capacity() const { return mask_ - 3; }   // the longest delay a cubic read can reach
    void clear() { std::fill(buf_.begin(), buf_.end(), 0.0f); }

    void write(float x) {
        w_ = (w_ + 1) & mask_;
        buf_[static_cast<size_t>(w_)] = x;
    }
    float at(int delay) const { return buf_[static_cast<size_t>((w_ - delay) & mask_)]; }
    float readLinear(float delay) const {
        const int i = static_cast<int>(delay);
        const float f = delay - static_cast<float>(i);
        const float a = at(i), b = at(i + 1);
        return a + (b - a) * f;
    }
    float readCubic(float delay) const {   // delay >= 1
        const int i = static_cast<int>(delay);
        const float f = delay - static_cast<float>(i);
        return hermite(at(i - 1), at(i), at(i + 1), at(i + 2), f);
    }

private:
    std::vector<float> buf_;
    int mask_ = 0, w_ = 0;
};

} // namespace af
