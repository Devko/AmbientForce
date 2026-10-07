#pragma once
// The lifetime oscillator (CONCEPT.md 5.2). A wavetable oscillator over PolyForce's table layout
// (wavetable.h); LifeScan, the read position that moves through a lifetime table (Age, Sway and
// Smear); and the Couple modes that join oscillator A to oscillator B. Header-only: templates and
// small inline functions, compiled into each stratum that uses them.
//
// Two reads, chosen at compile time: 4-point Hermite interpolation (TableOsc) and PolyForce's
// linear one (TableOscLinear). Mip levels 2 to 5 keep 8 samples per cycle of their top harmonic
// (levels 0 and 1, below 51 Hz, fewer; levels 6 and up more), and linear interpolation there
// leaves images of the top harmonics folded anywhere into the audio band: at worst -64 dB under a
// saw's fundamental, on level 5, where those harmonics are loudest against it (32 harmonics in
// 256 samples; note 72, folded to 14.6 kHz). Hermite takes them to -74.5 dB, for twice the reads.
// Lifetime frames, whose top harmonics are much quieter than a saw's, leave less either way.
//
// The cost, as ARM instructions a sample in the inner loop (-O3, the device's flags), over one
// frame / a pair of frames / a position crossing frames within the render; FM adds 3 to 5:
//   Hermite  41 / 65 / 84
//   linear   20 / 34 / 52
// and about 100 more per render (the pitch, the position, the read's set-up), 3 a sample at the
// control step's 32. With 32-sample renders a moving position crosses frames in about 1 render in
// 3 at the fastest sway (depth 1, 2 Hz), 1 in 10 at 0.5 Hz, 1 in 30 at full Smear, and 1 in 250
// at Bloom's and Ground's default settings. By PolyForce's calibration (its ARM instructions per
// block against its device bench: about 1 ns an instruction), a Hermite oscillator over a pair of
// frames is about 0.29% of a 128-frame block and a linear one over one frame about 0.09%. So
// Bloom's oscillator A reads with Hermite and its oscillator B, mostly a digital wave, linearly
// (renderCoupled()); Ground's five partials read linearly too (ground.h: a drone lives low, where
// the linear read's images stay 65 dB down or more). The device bench (Task 11) has the final word;
// if Bloom runs over, its unison drops from 2 to 1 first (CONCEPT.md 11, the caps' order).
//
// Known limits, for the voices (Ground, Bloom):
// - FM widens A's spectrum, but A's mip level comes from A's own pitch, so at high notes with deep
//   FM the partials pushed past Nyquist fold back down.
// - A change of table mid-note is not crossfaded: the oscillator reads the new table from the next
//   render on (its position glides on, but from one table's frame to another's there is a step).
//   A voice that lets the table change while it sounds fades across it itself.
#include "common.h"
#include "wavetable.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace af {

enum Couple : int { CP_MIX, CP_FM, CP_AM, CP_RING, CP_COUNT };

// Where in the table's life to read, and how that point moves.
struct LifePos {
    float age = 0.5f;      // 0..1: 0 just struck, 1 almost gone
    float sway = 0.0f;     // 0..1: depth of the slow back-and-forth (+-0.25 of the table at 1)
    float swayHz = 0.05f;  // 0.002..2
    float smear = 0.0f;    // 0..1: fast random micro-motion of the position (+-0.03 at 1)
};

namespace lifeosc {

inline float unit(float x) { return x > 0.0f ? std::min(x, 1.0f) : 0.0f; }   // 0..1, NaN to 0
inline float rand01(uint32_t& s) { return static_cast<float>(xorshift(s) >> 8) * (1.0f / 16777216.0f); }
// A phase as a fraction of a cycle, 0..1; a NaN or an infinity has none: 0.
inline double cycles(float phase) {
    const double p = std::isfinite(phase) ? static_cast<double>(phase) : 0.0;
    return p - std::floor(p);
}

} // namespace lifeosc

// The moving read position, one per voice (each voice its own sway phase and its own smear).
// Control rate: step() once per control step (kChunk samples) with the time it covers.
//
// Sway is a sine of depth +-0.25 sway around Age. Near an end of the table the swing would leave
// 0..1, and there it is reflected, not clamped: a clamp holds the position on the end frame for
// as long as the sine stays outside (at Age 1 and full depth, half of every cycle), so the sound
// stops moving there and the Sway knob seems to do less the nearer Age is to an end. Reflected,
// the position turns back at the end and keeps moving all the time with the depth asked for; the
// turn is a corner in how the timbre moves, never a step in the sound.
//
// Smear is a smoothed random walk: from one random point in -1..1 to the next over 50..200 ms
// (each leg its own random length), along a smoothstep, so it starts and ends each leg at rest
// and never steps. It walks whatever the Smear knob says (the knob only scales it to +-0.03), so
// turning it up starts from wherever the walk is, not from a fixed place.
class LifeScan {
public:
    // The smear's random numbers, and a sway phase of its own (a different seed per voice).
    void seed(uint32_t s) {
        rng_ = s * 0x9E3779B1u + 0x6A09E667u;   // nearby seeds far apart
        if (rng_ == 0) rng_ = 0x6A09E667u;      // xorshift's one stuck state
        xorshift(rng_);
        reset(lifeosc::rand01(rng_));
    }
    // The sway's phase (cycles: 0 starts at Age and rises; not finite: 0), and the smear back to
    // its centre.
    void reset(float phase) {
        sway_ = lifeosc::cycles(phase);
        from_ = to_ = 0.0f;
        t_ = 1.0f;   // the next step starts a new leg, from the centre
        rate_ = 0.0f;
    }
    // The position 0..1 for the next control step, `seconds` after the last.
    float step(const LifePos& p, float seconds) {
        // A NaN or an infinity would stay in the phase and the walk for good: it counts as nothing.
        seconds = std::isfinite(seconds) && seconds > 0.0f ? seconds : 0.0f;
        const float hz = p.swayHz > 0.0f ? std::min(p.swayHz, 2.0f) : 0.0f;
        // Double: at 0.002 Hz the phase moves 1.5e-6 a step, which a float near 1 would round by
        // up to 2% (a slower sway in half of each cycle).
        sway_ += static_cast<double>(hz) * seconds;
        sway_ -= floorFast(sway_);
        t_ += seconds * rate_;
        if (t_ >= 1.0f) {
            from_ = to_;
            to_ = randBipolar(rng_);
            rate_ = 1.0f / (0.05f + 0.15f * lifeosc::rand01(rng_));
            t_ = 0.0f;
        }
        const float s = t_ * t_ * (3.0f - 2.0f * t_);
        const float smear = from_ + (to_ - from_) * s;
        float x = lifeosc::unit(p.age) + 0.25f * lifeosc::unit(p.sway) * sinCycle(static_cast<float>(sway_)) +
                  0.03f * lifeosc::unit(p.smear) * smear;
        if (x < 0.0f) x = -x;                 // past an end: back the other way (at most 0.28 past it)
        else if (x > 1.0f) x = 2.0f - x;
        return clampf(x, 0.0f, 1.0f);
    }

private:
    double sway_ = 0.0;                 // cycles, 0..1
    uint32_t rng_ = 0x6A09E667u;
    float from_ = 0.0f, to_ = 0.0f;     // the smear's leg, -1..1
    float t_ = 1.0f, rate_ = 0.0f;      // how far along it (0..1), and legs per second
};

namespace lifeosc {

// Per mip level: where it starts in a frame, and 1 / 2^(32 - its bits) (the phase's fraction
// bits as 0..1).
struct MipTables {
    int offset[kMipLevels];
    float frac[kMipLevels];
};
constexpr MipTables mipTables() {
    MipTables m{};
    for (int k = 0; k < kMipLevels; ++k) {
        m.offset[k] = mipOffset(k);
        m.frac[k] = 1.0f / static_cast<float>(1u << (32 - kMipBits[k]));
    }
    return m;
}
inline constexpr MipTables kMip = mipTables();

// How a span of samples reads the frames. ONE: a single frame (a digital wave, or a position on a
// frame). PAIR: the same two frames throughout, their shares gliding. WALK: the position crosses
// frames within the span, so each sample finds its own pair.
enum Kind { K_ONE, K_PAIR, K_WALK };

struct Span {
    const int16_t* a = nullptr;   // ONE, PAIR: the first frame's level; WALK: frame 0's level
    const int16_t* b = nullptr;   // PAIR: the second frame's level
    const float* scale = nullptr; // WALK: every frame's scale
    float sa = 1.0f;              // ONE: the frame's scale
    float wa = 0.0f, wb = 0.0f;   // PAIR: the frames' weights (scale x share) at sample i are
    float dwa = 0.0f, dwb = 0.0f; // wa + dwa (i + 1) and wb + dwb (i + 1)
    float x0 = 0.0f, dx = 0.0f;   // WALK: the frame position before the first sample, its step
    int last = 0;                 // WALK: the last pair's first frame (frames - 2)
    uint32_t wrap = 0;            // the level's length - 1
    int shift = 21;               // 32 - the level's bits: phase >> shift is the sample
    uint32_t fracMask = 0;
    float frac = 0.0f;            // 1 / 2^shift
};

// The samples around j: p[j - 1], p[j], p[j + 1] (the guard sample past the end) and p[j + 2],
// wrapped around the level.
AF_INLINE void taps(const int16_t* p, uint32_t j, uint32_t wrap, float& xm, float& x0, float& x1, float& x2) {
    xm = static_cast<float>(p[(j - 1) & wrap]);
    x0 = static_cast<float>(p[j]);
    x1 = static_cast<float>(p[j + 1]);
    x2 = static_cast<float>(p[(j + 2) & wrap]);
}

// n samples into out; returns the phase after the last. The phase is a 32-bit fixed-point fraction
// of a cycle: its top bits index the level, the rest are the interpolation fraction, and the
// wrap-around is integer overflow. FM: fm[i] (cycles) moves the read, not the phase itself.
// Hermite: four samples a frame; else two (linear, PolyForce's read). K: the Kind. The span comes
// by value: a span whose address went out would give span() a stack guard on every render.
template <bool Hermite, int K, bool FM>
uint32_t play(const Span s, uint32_t ph, uint32_t dph, float* out, int n, const float* fm) {
    // Locals: the stores to out could otherwise alias the span's fields, and every field would be
    // loaded again for every sample.
    const int16_t* const a = s.a;
    const int16_t* const b = s.b;
    const float* const scale = s.scale;
    const float sa = s.sa, dwa = s.dwa, dwb = s.dwb, xs = s.x0, dx = s.dx, frac = s.frac;
    const uint32_t wrap = s.wrap, fracMask = s.fracMask;
    const int shift = s.shift, last = s.last;
    const float wa0 = s.wa, wb0 = s.wb;
    float k = 0.0f;   // i + 1, counted in floats (no conversion per sample)
    for (int i = 0; i < n; ++i) {
        uint32_t p = ph;
        // 2^24 steps a cycle, then into the top bits: offsets up to +-128 cycles, no overflow.
        if constexpr (FM) p += static_cast<uint32_t>(static_cast<int32_t>(fm[i] * 16777216.0f)) << 8;
        ph += dph;
        const uint32_t j = p >> shift;
        const float t = static_cast<float>(p & fracMask) * frac;
        if constexpr (K == K_ONE) {
            if constexpr (Hermite) {
                float am, a0, a1, a2;
                taps(a, j, wrap, am, a0, a1, a2);
                out[i] = hermite(am, a0, a1, a2, t) * sa;
            } else {
                const float a0 = static_cast<float>(a[j]), a1 = static_cast<float>(a[j + 1]);
                out[i] = (a0 + t * (a1 - a0)) * sa;
            }
        } else {
            const int16_t *A = a, *B = b;
            float wa, wb;
            k += 1.0f;
            if constexpr (K == K_WALK) {
                const float x = xs + dx * k;
                const int f = std::min(std::max(static_cast<int>(x), 0), last);
                const float u = x - static_cast<float>(f);
                A = a + static_cast<ptrdiff_t>(f) * kFrameStride;
                B = A + kFrameStride;
                wa = scale[f] * (1.0f - u);
                wb = scale[f + 1] * u;
            } else {
                // From the start, not step by step: adding the steps up drifts by up to 6e-6 over
                // a 128-sample render.
                wa = wa0 + dwa * k;
                wb = wb0 + dwb * k;
            }
            // Interpolation and crossfade are both linear in the samples: mix the taps, then
            // interpolate once.
            if constexpr (Hermite) {
                float am, a0, a1, a2, bm, b0, b1, b2;
                taps(A, j, wrap, am, a0, a1, a2);
                taps(B, j, wrap, bm, b0, b1, b2);
                out[i] = hermite(am * wa + bm * wb, a0 * wa + b0 * wb, a1 * wa + b1 * wb, a2 * wa + b2 * wb, t);
            } else {
                const float c0 = static_cast<float>(A[j]) * wa + static_cast<float>(B[j]) * wb;
                const float c1 = static_cast<float>(A[j + 1]) * wa + static_cast<float>(B[j + 1]) * wb;
                out[i] = c0 + t * (c1 - c0);
            }
        }
    }
    return ph;
}

} // namespace lifeosc

// One oscillator over one table: phase, frame crossfade, mip choice. Frames are read as they are
// stored (int16 * scale): lifetime frames are equal-RMS already, so there is no level per frame.
// Hermite: the 4-point read (TableOsc), else the linear one (TableOscLinear); see the top of this
// file for what each costs and leaves behind.
//
// The phase is a 32-bit fixed-point fraction of a cycle, as in PolyForce. Adding the increment
// wraps it by integer overflow, which is exact: after any number of samples, hours of them, the
// phase is exactly where that many increments put it. Its one error is the increment's rounding
// to 2^-32 cycles, a pitch at most 1e-5 Hz off that never changes. (A float phase wrapped every
// cycle would round each increment to its 2^-24 grid, a pitch error of up to 0.1 cent in the bass
// that changes with the phase; a float that is never wrapped loses a bit each time its count
// doubles.)
template <bool Hermite>
class TableOscT {
public:
    // phase: cycles (a NaN or an infinity: 0).
    void reset(float phase = 0.0f) {
        const double p = lifeosc::cycles(phase);
        ph_ = static_cast<uint32_t>(static_cast<uint64_t>(p * 4294967296.0));   // a p that rounds to 1 wraps to 0
        mip_ = -1;
    }

    // inc: cycles per sample (0..0.49); pos: 0..1 over the frames. Renders n samples into out
    // (overwrites). fmIn: a per-sample phase offset in cycles, or nullptr; it must not be out, and
    // it must stay within +-128 cycles (it is converted to an integer, which past that overflows).
    // renderCoupled()'s is amt * 0.5 * B, under a cycle. It isn't clamped here: a clamp costs every
    // FM sample 6 or 7 ARM instructions (measured; 44 -> 51 over one frame) for a bound nothing
    // reaches.
    //
    // The position glides: sample i of n plays pos_last + (pos - pos_last) (i + 1) / n, so the last
    // sample is at pos and a moving Age never steps (the first render after reset() starts at pos).
    // The mip level is chosen once per render from inc. When it changes, this render fades from
    // the old level to the new one across its n samples, as the position glides: a level change
    // drops or adds the top octave of harmonics, which as a step would tick.
    void render(const Wavetable& t, float inc, float pos, float* out, int n, const float* fmIn = nullptr) {
        if (n <= 0) return;
        if (t.frames <= 0) {
            std::fill(out, out + n, 0.0f);
            return;
        }
        pitch(inc);
        pos = lifeosc::unit(pos);
        const float frames1 = static_cast<float>(t.frames - 1);
        const float f1 = pos * frames1, f0 = mip_ < 0 ? f1 : pos_ * frames1;
        const int was = mip_;
        pos_ = pos;
        mip_ = level_;
        if (was < 0 || was == level_)
            ph_ = span(t, level_, ph_, step_, f0, f1, out, n, fmIn);
        else
            fadeLevels(t, was, f0, f1, out, n, fmIn);
    }

    // What render() would do to the oscillator, without reading the table or writing anything:
    // the phase moves on n samples, and the position and the level are left where render() would
    // leave them. For an oscillator nobody hears for a while that has to come back in step (B
    // against A, renderCoupled()). FM doesn't move the phase itself, so it doesn't matter here.
    void skip(const Wavetable& t, float inc, float pos, int n) {
        if (n <= 0 || t.frames <= 0) return;   // render() leaves the state alone too
        pitch(inc);
        pos_ = lifeosc::unit(pos);
        mip_ = level_;
        ph_ += static_cast<uint32_t>(n) * step_;   // modulo 2^32, as n additions would be
    }

private:
    // The pitch as render() and skip() take it: 0..0.49 cycles a sample (a NaN: 0); its mip level
    // and phase step are worked out again only when it changes (mipFor is a search).
    void pitch(float inc) {
        inc = inc > 0.0f ? std::min(inc, 0.49f) : 0.0f;
        if (inc == inc_) return;
        inc_ = inc;
        level_ = mipFor(std::max(inc, 1e-6f));   // never kAliasLimit / 0
        step_ = static_cast<uint32_t>(inc * 4294967296.0f);
    }

    // The render in which the mip level changes, from `was` to level_: both levels, the old fading
    // into the new. Out of line: it is rare, and its scratch buffer would otherwise give every
    // render a bigger stack frame (and a stack guard).
    __attribute__((noinline)) void fadeLevels(const Wavetable& t, int was, float f0, float f1, float* out, int n,
                                              const float* fmIn) {
        const float df = (f1 - f0) / static_cast<float>(n), w = 1.0f / static_cast<float>(n);
        for (int o = 0; o < n; o += kChunk) {
            const int m = std::min(kChunk, n - o);
            const float fs = f0 + df * static_cast<float>(o);
            const float fe = (o + m) == n ? f1 : f0 + df * static_cast<float>(o + m);
            const float* fm = fmIn ? fmIn + o : nullptr;
            float fresh[kChunk];
            span(t, was, ph_, step_, fs, fe, out + o, m, fm);
            ph_ = span(t, level_, ph_, step_, fs, fe, fresh, m, fm);
            for (int i = 0; i < m; ++i) out[o + i] += static_cast<float>(o + i + 1) * w * (fresh[i] - out[o + i]);
        }
    }

    // n samples of level `level` from phase ph, the frame position gliding from f0 (before the
    // first sample) to f1 (the last). Returns the phase after them.
    static uint32_t span(const Wavetable& t, int level, uint32_t ph, uint32_t step, float f0, float f1, float* out,
                         int n, const float* fm) {
        using namespace lifeosc;
        Span s;
        s.wrap = static_cast<uint32_t>(mipLength(level) - 1);
        s.shift = 32 - kMipBits[level];
        s.fracMask = (1u << s.shift) - 1u;
        s.frac = kMip.frac[level];
        const int16_t* base = t.data.data() + kMip.offset[level];
        Kind kind = K_ONE;
        int f = 0;
        if (t.frames > 1) {
            s.last = t.frames - 2;
            const int a0 = std::min(static_cast<int>(f0), s.last), a1 = std::min(static_cast<int>(f1), s.last);
            const float x0 = f0 - static_cast<float>(a0), x1 = f1 - static_cast<float>(a1);
            if (a0 != a1) {
                kind = K_WALK;
                s.a = base;
                s.scale = t.scale.data();
                s.x0 = f0;
                s.dx = (f1 - f0) / static_cast<float>(n);
            } else if (x0 == 0.0f && x1 == 0.0f) {
                f = a0;
            } else if (x0 == 1.0f && x1 == 1.0f) {
                f = a0 + 1;   // pos 1: the last frame alone
            } else {
                kind = K_PAIR;
                f = a0;
                s.b = base + static_cast<ptrdiff_t>(f + 1) * kFrameStride;
                const float sa = t.scale[static_cast<size_t>(f)], sb = t.scale[static_cast<size_t>(f + 1)];
                const float dx = (x1 - x0) / static_cast<float>(n);
                s.wa = sa - sa * x0;
                s.wb = sb * x0;
                s.dwa = -sa * dx;
                s.dwb = sb * dx;
            }
        }
        if (kind != K_WALK) {
            s.a = base + static_cast<ptrdiff_t>(f) * kFrameStride;
            s.sa = t.scale[static_cast<size_t>(f)];
        }
        const bool withFm = fm != nullptr;
        switch (kind) {
            case K_ONE:
                return withFm ? play<Hermite, K_ONE, true>(s, ph, step, out, n, fm)
                              : play<Hermite, K_ONE, false>(s, ph, step, out, n, fm);
            case K_PAIR:
                return withFm ? play<Hermite, K_PAIR, true>(s, ph, step, out, n, fm)
                              : play<Hermite, K_PAIR, false>(s, ph, step, out, n, fm);
            case K_WALK:
                return withFm ? play<Hermite, K_WALK, true>(s, ph, step, out, n, fm)
                              : play<Hermite, K_WALK, false>(s, ph, step, out, n, fm);
        }
        return ph;   // not reached: the switch has every kind
    }

    uint32_t ph_ = 0;
    float pos_ = 0.0f;    // the last render's position, where the next one's glide starts
    int mip_ = -1;        // the last render's level; -1: none since reset() (no glide, no level fade)
    float inc_ = -1.0f;   // the pitch last worked out (none yet), its level and its phase step
    int level_ = 0;
    uint32_t step_ = 0;
};

using TableOsc = TableOscT<true>;         // the lifetime oscillator: Bloom's A
using TableOscLinear = TableOscT<false>;  // half the reads: Bloom's oscillator B, Ground's partials

// --- Couple: oscillator A with oscillator B -----------------------------------------------------
//
// The voice renders B first, each control step, into a scratch buffer; then A, in FM mode with B
// as its phase input; then couple() joins them. renderCoupled() does all three.
//   Mix:  (1 - blend) A + blend B
//   FM:   A read at its phase + amt * 0.5 * B (cycles), then mixed with B by blend
//   AM:   A (1 - amt + amt (0.5 + 0.5 B)), then the blend
//   Ring: (1 - amt) A + amt A B, then the blend

// B as A's FM input: amt * 0.5 cycles at full scale.
inline void fmInput(float amt, const float* b, float* fm, int n) {
    const float k = 0.5f * lifeosc::unit(amt);
    for (int i = 0; i < n; ++i) fm[i] = k * b[i];
}

// a and b into out (out may be a or b). AM and Ring are both A (c + d B): AM c = 1 - amt / 2,
// d = amt / 2; Ring c = 1 - amt, d = amt. Mix and FM leave A as it is (FM's work is done in A's
// render).
inline void couple(int mode, float amt, float blend, const float* a, const float* b, float* out, int n) {
    amt = lifeosc::unit(amt);
    blend = lifeosc::unit(blend);
    float c = 1.0f, d = 0.0f;
    if (mode == CP_AM) {
        c = 1.0f - 0.5f * amt;
        d = 0.5f * amt;
    } else if (mode == CP_RING) {
        c = 1.0f - amt;
        d = amt;
    }
    const float ka = 1.0f - blend, kc = ka * c, kd = ka * d;
    for (int i = 0; i < n; ++i) out[i] = a[i] * (kc + kd * b[i]) + blend * b[i];
}

// One control step of a coupled pair, both oscillators at the same position. scratch: 2n floats.
//
// B reads linearly (TableOscLinear). It is mostly a digital wave, there as a blend or a modulator,
// and the linear read's images (-64 dB under a saw's fundamental at worst, folded to 14.6 kHz,
// under a reverb) are not worth the Hermite read's cost a second time per voice (see the top of
// this file).
//
// An oscillator that isn't heard isn't read, but it is skip()ped along, not left standing: at
// blend 1 (B alone, in every mode) A, and at Mix with blend 0 B. Where B's phase stands against
// A's matters to every mode, Mix too whenever the two are in tune (the same pitch, or octaves
// apart, as Bloom's B octave sets them): two sines in tune add up to anything from silence to
// twice the level, and when the blend moves again that must not depend on how long it sat at an
// end. Every step ends up exactly where rendering both throughout would have.
inline void renderCoupled(TableOsc& oa, const Wavetable& ta, float incA, TableOscLinear& ob, const Wavetable& tb,
                          float incB, float pos, int mode, float amt, float blend, float* out, float* scratch, int n) {
    if (n <= 0) return;
    const float bl = lifeosc::unit(blend);
    if (bl == 1.0f) {   // what couple() would leave: B, exactly
        ob.render(tb, incB, pos, out, n);
        oa.skip(ta, incA, pos, n);
        return;
    }
    const bool mix = mode != CP_FM && mode != CP_AM && mode != CP_RING;   // what couple() plays as Mix
    if (mix && bl == 0.0f) {   // what couple() would leave: A, exactly
        ob.skip(tb, incB, pos, n);
        oa.render(ta, incA, pos, out, n);
        return;
    }
    float* b = scratch;
    ob.render(tb, incB, pos, b, n);
    if (mode == CP_FM) {
        float* fm = scratch + n;
        fmInput(amt, b, fm, n);
        oa.render(ta, incA, pos, out, n, fm);
    } else {
        oa.render(ta, incA, pos, out, n);
    }
    couple(mode, amt, blend, out, b, out, n);
}

} // namespace af
