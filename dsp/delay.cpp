// From EffectForce dsp/delay.cpp (7cf6e95), namespace ef -> af, EF_ -> AF_; Diffuse added.
#include "delay.h"
#include "simd.h"

#include <cstring>

namespace af {

namespace {

// L and R side by side in a two-lane vector: NEON's 64-bit registers on the device, GCC's generic
// vectors elsewhere (the x86 tests run the same arithmetic lane by lane). Comparisons and min / max
// stay in the vector unit: VFP's scalar compares stall on the transfer of their flags.
#if AF_NEON
using Lr = float32x2_t;
using LrMask = uint32x2_t;
#else
typedef float Lr __attribute__((vector_size(8)));
typedef int32_t LrMask __attribute__((vector_size(8)));
#endif

AF_INLINE Lr both(float x) { return Lr{x, x}; }

// Straight between memory and the lanes: built from two scalars, GCC goes through the stack.
AF_INLINE Lr loadLr(const float* l, const float* r) {
#if AF_NEON
    return vld1_lane_f32(r, vld1_dup_f32(l), 1);
#else
    return Lr{*l, *r};
#endif
}
AF_INLINE void storeLr(float* l, float* r, Lr v) {
#if AF_NEON
    vst1_lane_f32(l, v, 0);
    vst1_lane_f32(r, v, 1);
#else
    *l = v[0];
    *r = v[1];
#endif
}
AF_INLINE Lr pairLr(float l, float r) {
#if AF_NEON
    return vset_lane_f32(r, vdup_n_f32(l), 1);
#else
    return Lr{l, r};
#endif
}

AF_INLINE Lr minLr(Lr a, Lr b) {
#if AF_NEON
    return vmin_f32(a, b);
#else
    return a < b ? a : b;
#endif
}
AF_INLINE Lr maxLr(Lr a, Lr b) {
#if AF_NEON
    return vmax_f32(a, b);
#else
    return a > b ? a : b;
#endif
}
AF_INLINE Lr absLr(Lr a) {
#if AF_NEON
    return vabs_f32(a);
#else
    return a < Lr{} ? -a : a;
#endif
}
AF_INLINE Lr swapLr(Lr a) {   // (R, L)
#if AF_NEON
    return vrev64_f32(a);
#else
    return Lr{a[1], a[0]};
#endif
}
// Two floats side by side in memory (L, R), one 64-bit load or store.
AF_INLINE Lr loadPair(const float* p) {
#if AF_NEON
    return vld1_f32(p);
#else
    return Lr{p[0], p[1]};
#endif
}
AF_INLINE void storePair(float* p, Lr v) {
#if AF_NEON
    vst1_f32(p, v);
#else
    p[0] = v[0];
    p[1] = v[1];
#endif
}
AF_INLINE Lr louderLr(Lr a) {   // the larger lane, in both
#if AF_NEON
    return vpmax_f32(a, a);
#else
    return maxLr(a, swapLr(a));
#endif
}
AF_INLINE LrMask greaterLr(Lr a, Lr b) {
#if AF_NEON
    return vcgt_f32(a, b);
#else
    return a > b;
#endif
}
AF_INLINE Lr pickLr(LrMask m, Lr yes, Lr no) {
#if AF_NEON
    return vbsl_f32(m, yes, no);
#else
    return m ? yes : no;
#endif
}
// 1 / d for d >= 1: NEON's estimate and one Newton-Raphson step (about 1e-5), no VFP divider.
AF_INLINE Lr recipLr(Lr d) {
#if AF_NEON
    const Lr e = vrecpe_f32(d);
    return vmul_f32(e, vrecps_f32(d, e));
#else
    return both(1.0f) / d;
#endif
}
// NaN and infinities become 0, by their bits (an exponent of all ones).
AF_INLINE Lr finiteLr(Lr x) {
#if AF_NEON
    const uint32x2_t bits = vand_u32(vreinterpret_u32_f32(x), vdup_n_u32(0x7fffffffu));
    return vbsl_f32(vclt_u32(bits, vdup_n_u32(0x7f800000u)), x, vdup_n_f32(0.0f));
#else
    LrMask bits;
    std::memcpy(&bits, &x, sizeof bits);
    return (bits & 0x7fffffff) < 0x7f800000 ? x : Lr{};
#endif
}
// common.h's hermite() for both lanes.
AF_INLINE Lr hermiteLr(Lr xm1, Lr x0, Lr x1, Lr x2, Lr t) {
    const Lr c1 = both(0.5f) * (x1 - xm1);
    const Lr c2 = xm1 - both(2.5f) * x0 + both(2.0f) * x1 - both(0.5f) * x2;
    const Lr c3 = both(0.5f) * (x2 - xm1) + both(1.5f) * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// A parameter clamped to its range; NaN (which no comparison catches) becomes `nan`.
template <class T>
T clampOr(T x, T lo, T hi, T nan) {
    return x >= lo ? (x <= hi ? x : hi) : (x < lo ? lo : nan);
}

constexpr int kSeg = 32;                      // the glide, the wow and the duck gain step per segment
constexpr double kMaxTime = 8.0 * kRate;      // 1 bar at 30 BPM
constexpr double kMinTime = 0.001 * kRate;    // 1 ms
constexpr float kWowDepth = 0.003f * kRate;   // +-3 ms at wow 1
constexpr float kFlutter = 0.2f / 3.0f;       // the flutter's depth against the wow's: +-0.2 ms
constexpr float kWowHz = 0.5f, kFlutterHzL = 6.0f, kFlutterHzR = 6.6f;
constexpr float kWowLagR = 0.25f;             // cycles
// The lines: the longest time plus the wow's depth, and the cubic read's two samples on either side.
constexpr int kLen = static_cast<int>(kMaxTime) + static_cast<int>(kWowDepth * (1.0f + kFlutter)) + 8;
constexpr int kGuard = 3;                     // the first samples again past the end: a read never wraps
constexpr int kMaxAge = kLen - 3;             // the oldest whole delay a read may ask for
constexpr float kClamp = 8.0f;                // +18 dBFS: the most the limiter ever sees
constexpr float kTiny = 1e-18f;               // a DC offset (-360 dB) under everything in the loop: no denormals
constexpr float kDuckLaw = 16.0f;
constexpr float kMonoStep = 1.0f / 16.0f;     // Mono's share of R's wow, per segment: 12 ms to switch
constexpr int kTailForever = 1 << 30;         // feedback 1: 6.8 hours
constexpr int kFadeSegs = 69;                 // Fade's crossfade: 69 segments, 50 ms

// Diffuse: each side's allpasses (7.3, 11.9, 17.1, 23.7 ms on L; 8.1, 12.7, 18.3, 24.9 ms on R, to
// the nearest sample), where each one's ring starts in the block of them (each followed by a copy
// of its first kApGuard samples: a run never wraps), and each side's total.
constexpr int kStages = Delay::kStages;
constexpr int kApGuard = kSeg;
struct ApLayout {
    int len[2][kStages] = {}, off[2][kStages] = {}, sum[2] = {};
    int total = 0;
};
constexpr ApLayout apLayout() {
    constexpr double ms[2][kStages] = {{7.3, 11.9, 17.1, 23.7}, {8.1, 12.7, 18.3, 24.9}};
    ApLayout a;
    for (int side = 0; side < 2; ++side) {
        for (int s = 0; s < kStages; ++s) {
            a.len[side][s] = static_cast<int>(ms[side][s] * kRate / 1000.0 + 0.5);
            a.off[side][s] = a.total;
            a.total += a.len[side][s] + kApGuard;
            a.sum[side] += a.len[side][s];
        }
    }
    return a;
}
constexpr ApLayout kAp = apLayout();
constexpr int kApLongest = kAp.sum[1] > kAp.sum[0] ? kAp.sum[1] : kAp.sum[0];   // R's: 2822 samples, 64 ms
constexpr float kApCoef = 0.65f;
// How long the longest allpass rings until -60 dB (0.65 a lap: 17 laps of 24.9 ms), for the tail.
constexpr int kApRing = 17 * kAp.len[1][kStages - 1];

float onePole(float hz) { return 1.0f - std::exp(-2.0f * kPi * hz / kRate); }
float perSegment(float seconds) { return 1.0f - std::exp(-kSeg / (seconds * kRate)); }
float perSample(float seconds) { return 1.0f - std::exp(-1.0f / (seconds * kRate)); }

// Set when the plugin loads (no guard to take on the audio thread).
const float kWowSmooth = perSegment(0.1f), kDuckSmooth = perSegment(0.02f);   // the amounts glide
const float kRelease = std::exp(1.0f / (0.1f * kRate));                        // the limiter's, per sample
const float kAttack = perSample(0.005f), kFall = perSample(0.25f);             // the duck's envelope

// Both lines read at their times: the taps at ages i - 1 .. i + 2, four samples in a row, through
// the Hermite kernel.
AF_INLINE Lr readLr(const float* bl, const float* br, int w, double teL, double teR) {
    const int il = std::clamp(static_cast<int>(teL), 2, kMaxAge), ir = std::clamp(static_cast<int>(teR), 2, kMaxAge);
    const Lr frac = pairLr(static_cast<float>(teL - il), static_cast<float>(teR - ir));
    int ql = w - il - 2, qr = w - ir - 2;
    if (ql < 0) ql += kLen;
    if (qr < 0) qr += kLen;
    const float* pl = bl + ql;
    const float* pr = br + qr;
    return hermiteLr(loadLr(pl + 3, pr + 3), loadLr(pl + 2, pr + 2), loadLr(pl + 1, pr + 1), loadLr(pl, pr), frac);
}

// Fade's equal-power gains for the old and the new head at the end of segment `fade` of the
// crossfade (none running: 1 and 0), exact at both ends.
void fadeGains(int fade, float& a, float& b) {
    const float x = fade < 0 ? 0.0f : static_cast<float>(fade + 1) / kFadeSegs;
    a = x <= 0.0f ? 1.0f : (x >= 1.0f ? 0.0f : sinQuarter(0.5f * kPi * (1.0f - x)));
    b = x <= 0.0f ? 0.0f : (x >= 1.0f ? 1.0f : sinQuarter(0.5f * kPi * x));
}

// A smoothed amount one segment on; snaps the last bit so it lands (wow 0 is exactly no wow).
float approach(float cur, float target, float coef) {
    const float d = target - cur;
    return std::fabs(d) < 1e-5f ? target : cur + d * coef;
}
float wrap1(float x) { return x >= 1.0f ? x - 1.0f : x; }

// Within a millionth of a sample of a whole number: whole (t > 0).
double wholeIfClose(double t) {
    const double r = floorFast(t + 0.5);
    return std::fabs(t - r) < 1e-6 ? r : t;
}

// Until a line has been written all the way round since reset(), it still holds what came before.
// Before a run of n samples at times te + d .. te + n d, zero what of that its taps can reach
// (ages past `written`; those written since stay): a few dozen samples, and only the first time
// round, instead of 1.4 MB at once or a check on every tap.
void hideOld(float* line, int w, int written, double te, double d, int n) {
    const double lo = std::min(te + d, te + n * d), hi = std::max(te + d, te + n * d);
    // At sample k a tap of age a reads what is k samples younger now; ages under 1 are written
    // in the run before they're read.
    const int from = std::max({written + 1, std::clamp(static_cast<int>(lo), 2, kMaxAge) - n, 1});
    const int to = std::clamp(static_cast<int>(hi), 2, kMaxAge) + 2;
    for (int a = from; a <= to; ++a) {
        const int j = w - a < 0 ? w - a + kLen : w - a;
        line[j] = 0.0f;
        if (j < kGuard) line[kLen + j] = 0.0f;
    }
}

// Up to four floats from p (the rest 0), and back: a sub-run's last few samples take the same
// arithmetic as the others, so where a run is cut never shows in the output.
AF_INLINE f4 loadSome(const float* p, int r) {
    float t[4] = {};
    for (int j = 0; j < r; ++j) t[j] = p[j];
    return load4(t);
}
AF_INLINE void storeSome(float* p, f4 v, int r) {
    float t[4];
    store4(t, v);
    for (int j = 0; j < r; ++j) p[j] = t[j];
}

// Four (L, R) pairs side by side in memory, as four Ls and four Rs, and back (r of them at the end
// of a sub-run).
AF_INLINE void loadPairs(const float* p, f4& l, f4& r) {
#if AF_NEON
    const float32x4x2_t v = vld2q_f32(p);
    l = v.val[0];
    r = v.val[1];
#else
    l = f4{p[0], p[2], p[4], p[6]};
    r = f4{p[1], p[3], p[5], p[7]};
#endif
}
AF_INLINE void storePairs(float* p, f4 l, f4 r) {
#if AF_NEON
    vst2q_f32(p, float32x4x2_t{{l, r}});
#else
    for (int j = 0; j < 4; ++j) {
        p[2 * j] = l[j];
        p[2 * j + 1] = r[j];
    }
#endif
}
AF_INLINE void loadSomePairs(const float* p, int r, f4& a, f4& b) {
    float t[8] = {};
    for (int j = 0; j < 2 * r; ++j) t[j] = p[j];
    loadPairs(t, a, b);
}
AF_INLINE void storeSomePairs(float* p, int r, f4 a, f4 b) {
    float t[8];
    storePairs(t, a, b);
    for (int j = 0; j < 2 * r; ++j) p[j] = t[j];
}

// Four samples of one side through its four allpasses (r of them at the end of a sub-run). An
// allpass of D samples is (z^-D - g) / (1 - g z^-D): its ring takes the input plus g times what
// comes round, and it passes on what comes round less g times that.
template <bool Whole>
AF_INLINE f4 allpasses(float* const (&ring)[kStages], f4 a, int k, int r) {
    const f4 g = splat(kApCoef);
    for (int s = 0; s < kStages; ++s) {
        const f4 z = Whole ? load4(ring[s] + k) : loadSome(ring[s] + k, r);
        const f4 fed = a + g * z;
        if (Whole) store4(ring[s] + k, fed);
        else storeSome(ring[s] + k, fed, r);
        a = z - g * fed;
    }
    return a;
}

// The blends of four samples (r at the end): with the clear repeats by Diffuse (dv), and in Mono
// L's diffusion for R too (mv), both exact at 0 and 1, back into the repeats yl, yr.
AF_INLINE void blend(f4 dv, f4 mv, f4 yl, f4 yr, f4 dl, f4 dr, f4& l, f4& r) {
    const f4 one = splat(1.0f);
    l = yl * (one - dv) + dl * dv;
    r = (yr * (one - dv) + dr * dv) * (one - mv) + l * mv;
}

// The diffusers over a sub-run of m samples (m <= kSeg), four samples at a time: the repeats y (L
// and R side by side) through each side's four allpasses, then blended, back into y. Moving: the
// blends' amounts per sample (difs, monos); otherwise they hold (dif, mono). Each ring has kApGuard
// samples past its end that copy its first ones, so a sub-run reads and writes it in a straight
// line; then what went past the end goes back to the start, and what went to the start into the
// copy.
template <bool Moving>
AF_INLINE void diffuseSides(float* ap, int (&pos)[2][kStages], float (&y)[2 * kSeg], int m, const float* difs,
                            const float* monos, float dif, float mono) {
    const int whole = m & ~3, rest = m - whole;
    float* ringL[kStages];
    float* ringR[kStages];
    for (int s = 0; s < kStages; ++s) {
        ringL[s] = ap + kAp.off[0][s] + pos[0][s];
        ringR[s] = ap + kAp.off[1][s] + pos[1][s];
    }
    f4 yl, yr, l, r;
    for (int k = 0; k < whole; k += 4) {
        loadPairs(&y[2 * k], yl, yr);
        blend(Moving ? load4(difs + k) : splat(dif), Moving ? load4(monos + k) : splat(mono), yl, yr,
              allpasses<true>(ringL, yl, k, 4), allpasses<true>(ringR, yr, k, 4), l, r);
        storePairs(&y[2 * k], l, r);
    }
    if (rest > 0) {
        loadSomePairs(&y[2 * whole], rest, yl, yr);
        blend(Moving ? loadSome(difs + whole, rest) : splat(dif), Moving ? loadSome(monos + whole, rest) : splat(mono), yl,
              yr, allpasses<false>(ringL, yl, whole, rest), allpasses<false>(ringR, yr, whole, rest), l, r);
        storeSomePairs(&y[2 * whole], rest, l, r);
    }
    for (int side = 0; side < 2; ++side) {
        for (int s = 0; s < kStages; ++s) {
            float* const base = ap + kAp.off[side][s];
            const int len = kAp.len[side][s];
            int& p = pos[side][s];
            if (p + m > len) std::memcpy(base, base + len, sizeof(float) * static_cast<size_t>(p + m - len));
            else if (p < kApGuard) std::memcpy(base + len + p, base + p, sizeof(float) * static_cast<size_t>(std::min(p + m, kApGuard) - p));
            p = p + m >= len ? p + m - len : p + m;
        }
    }
}

} // namespace

Delay::Delay()
    : glide_(1.0 - std::exp(-kSeg / (0.060 * kRate))),
      lineL_(static_cast<size_t>(kLen + kGuard), 0.0f),
      lineR_(static_cast<size_t>(kLen + kGuard), 0.0f),
      ap_(static_cast<size_t>(kAp.total), 0.0f) {
    reset();
}

void Delay::reset() {
    w_ = 0;
    written_ = 0;
    lpL_ = lpR_ = hpL_ = hpR_ = 0.0f;
    gainL_ = gainR_ = 1.0f;
    env_ = 0.0f;
    phWow_ = phFlutL_ = phFlutR_ = 0.0f;
    segLeft_ = 0;
    fadeL_ = fadeR_ = -1;
    fading_ = false;
    apStale_ = true;
    diffused_ = false;
    fresh_ = true;
}

void Delay::set(const Params& p, const Transport& t) {
    const int mode = std::clamp(p.mode, 0, kModes - 1);
    glideType_ = std::clamp(p.glide, 0, kGlides - 1);   // a fade under way finishes first
    double base;
    if (p.sync) {
        const double beats = clampOr(p.divBeats, kDelayDivs[0].beats, kDelayDivs[kNumDelayDivs - 1].beats, 0.75);
        base = divSeconds(beats, clampOr(t.bpm, 1.0, 1000.0, 120.0)) * kRate;
    } else {
        base = static_cast<double>(clampOr(p.timeMs, 1.0f, 2000.0f, 375.0f)) * kRate / 1000.0;
    }
    const double spread = clampOr(p.spread, -0.5f, 0.5f, 0.0f);
    tgtL_ = wholeIfClose(std::clamp(base, kMinTime, kMaxTime));
    tgtR_ = mode == MONO ? tgtL_ : wholeIfClose(std::clamp(tgtL_ * (1.0 + spread), 0.5 * kMinTime, kMaxTime));

    const float fb = clampOr(p.feedback, 0.0f, 1.0f, 0.0f);
    tgt_[FB] = fb;
    tgt_[DRIVE] = clampOr(p.drive, 0.0f, 1.0f, 0.0f);
    tgt_[MIX] = clampOr(p.mix, 0.0f, 1.0f, 0.0f);
    const float lc = clampOr(p.lowCutHz, 20.0f, 2000.0f, 100.0f), hc = clampOr(p.highCutHz, 500.0f, 20000.0f, 20000.0f);
    if (lc != lastLowCut_) {
        lastLowCut_ = lc;
        tgt_[HP] = onePole(lc);
    }
    if (hc != lastHighCut_) {
        lastHighCut_ = hc;
        tgt_[LP] = hc >= 20000.0f ? 1.0f : onePole(hc);
    }
    tgt_[MONO_IN] = mode == STEREO ? 0.0f : 1.0f;
    tgt_[CROSS] = mode == PING_PONG ? 1.0f : 0.0f;
    tgt_[R_IN] = mode == PING_PONG ? 0.0f : 1.0f;
    tgt_[DIFFUSE] = clampOr(p.diffuse, 0.0f, 1.0f, 0.0f);
    tgt_[MONO_OUT] = mode == MONO ? 1.0f : 0.0f;
    wowTgt_ = clampOr(p.wow, 0.0f, 1.0f, 0.0f);
    duckTgt_ = clampOr(p.duck, 0.0f, 1.0f, 0.0f);
    monoTgt_ = mode == MONO ? 1.0f : 0.0f;

    if (fresh_) {   // jump: no ramps, no glide, the wow and the duck where they would be by now
        for (int k = 0; k < kRamps; ++k) cur_[k] = tgt_[k];
        tL_ = tgtL_;
        tR_ = tgtR_;
        wowAmt_ = wowTgt_;
        duckAmt_ = duckTgt_;
        mono_ = monoTgt_;
        float ml, mr;
        wowNow(tL_, tR_, ml, mr);
        teL_ = teEndL_ = tL_ + ml;
        teR_ = teEndR_ = tR_ + mr;
        teStepL_ = teStepR_ = 0.0;
        tBL_ = tL_;
        tBR_ = tR_;
        teBL_ = teBEndL_ = teL_;
        teBR_ = teBEndR_ = teR_;
        teBStepL_ = teBStepR_ = 0.0;
        for (int k = 0; k < 4; ++k) {
            gain_[k] = gainEnd_[k] = k < 2 ? 1.0f : 0.0f;
            gainStep_[k] = 0.0f;
        }
        duck_ = duckEnd_ = 1.0f / (1.0f + kDuckLaw * duckAmt_ * env_);
        duckStep_ = 0.0f;
        fresh_ = false;
    }

    // Down to -60 dB: repeat k is fb^(k-1) of the first (at the cuts' passband), at most k times
    // the longer side's time away (in Ping-Pong too: the sides take turns). Plus the wow and the
    // 20 Hz high-pass's ring. Only when the feedback or the times change (the logs only for the
    // feedback). Diffusing, each repeat also comes up to twice the longer side's allpasses later,
    // and the last rings on as the longest allpass does. On average a pass through them takes
    // their length, but an allpass holds the frequencies at its poles up to (1 + g) / (1 - g) =
    // 4.7 times as long, and the slowest of those set the end: a noise burst's -60 dB edge was
    // measured at most 1.7 times their length later a pass (at 1 to 100 ms, feedback 0.5 to 0.99).
    // Between Diffuse 0 and 1 the repeats fall faster still (the blend loses a little a pass).
    const bool diffuse = tgt_[DIFFUSE] > 0.0f || cur_[DIFFUSE] > 0.0f;
    const double longest = std::max({tgtL_, tgtR_, tL_, tR_, tBL_, tBR_}) + wowTgt_ * kWowDepth * (1.0f + kFlutter);
    if (fb != tailFb_ || longest != tailLongest_ || diffuse != tailDiffuse_) {
        if (fb != tailFb_) {
            tailFb_ = fb;
            repeats_ = fb > 1e-6f && fb < 1.0f ? std::floor(std::log(1e-3) / std::log(static_cast<double>(fb))) + 1.0 : 1.0;
        }
        tailLongest_ = longest;
        tailDiffuse_ = diffuse;
        double tail = longest * repeats_ + 0.06 * kRate;
        if (diffuse) tail += 2.0 * kApLongest * repeats_ + kApRing;
        tail_ = fb >= 1.0f ? kTailForever : static_cast<int>(std::min(tail, static_cast<double>(kTailForever)));
    }

    // Once round: the furthest either head reads (its glide's start or end, the wow at its deeper
    // amount while it glides), the Hermite kernel's reach and a segment's glide, and the diffusers
    // while they run or may start.
    const double heads = std::max({tgtL_, tgtR_, tL_, tR_, tBL_, tBR_}) + std::max(wowTgt_, wowAmt_) * kWowDepth * (1.0f + kFlutter);
    reach_ = static_cast<int>(heads) + 3 + kSeg + (diffuse ? kApLongest : 0);
}

// The wow's offsets (samples) for heads at times tl and tr, at the phases and amounts now.
void Delay::wowNow(double tl, double tr, float& l, float& r) const {
    const float depthL = wowAmt_ * std::min(kWowDepth, 0.25f * static_cast<float>(tl));
    const float depthR = wowAmt_ * std::min(kWowDepth, 0.25f * static_cast<float>(tr));
    l = depthL * (sinCycle(phWow_) + kFlutter * sinCycle(phFlutL_));
    const float own = depthR * (sinCycle(wrap1(phWow_ + kWowLagR)) + kFlutter * sinCycle(phFlutR_));
    r = (1.0f - mono_) * own + mono_ * l;   // exact at 0 and 1: Mono's R is L
}

// Every 32 samples: the glide, the wow and the duck gain at the segment's end, and their steps.
void Delay::segment() {
    constexpr float kWowInc = kSeg * kWowHz / kRate, kFlutIncL = kSeg * kFlutterHzL / kRate,
                    kFlutIncR = kSeg * kFlutterHzR / kRate;

    teL_ = teEndL_;   // the last segment lands exactly where it was aimed
    teR_ = teEndR_;
    teBL_ = teBEndL_;
    teBR_ = teBEndR_;
    for (int k = 0; k < 4; ++k) gain_[k] = gainEnd_[k];
    duck_ = duckEnd_;

    wowAmt_ = approach(wowAmt_, wowTgt_, kWowSmooth);
    duckAmt_ = approach(duckAmt_, duckTgt_, kDuckSmooth);
    mono_ = mono_ < monoTgt_ ? std::min(monoTgt_, mono_ + kMonoStep) : std::max(monoTgt_, mono_ - kMonoStep);
    phWow_ = wrap1(phWow_ + kWowInc);
    phFlutL_ = wrap1(phFlutL_ + kFlutIncL);
    phFlutR_ = wrap1(phFlutR_ + kFlutIncR);

    // Each side's heads. A fade that is over hands the reading to its new head. Then Tape glides;
    // Fade snaps a change under half a sample, or starts a fade to the target (true).
    const auto heads = [this](double& t, double& tB, double& te, double& teB, float& gA, float& gB, int& fade,
                              double target) {
        if (fade >= 0 && ++fade == kFadeSegs) {
            t = tB;
            te = teB;
            gA = 1.0f;
            gB = 0.0f;
            fade = -1;
        }
        if (fade >= 0) return false;
        if (glideType_ == TAPE) {
            t = glideTime(t, target);
        } else if (std::fabs(target - t) < 0.5) {
            t = target;
        } else {
            tB = target;
            fade = 0;
            return true;
        }
        tB = t;
        return false;
    };
    const bool newL = heads(tL_, tBL_, teL_, teBL_, gain_[0], gain_[2], fadeL_, tgtL_);
    const bool newR = heads(tR_, tBR_, teR_, teBR_, gain_[1], gain_[3], fadeR_, tgtR_);

    float ml, mr;
    wowNow(tL_, tR_, ml, mr);
    teEndL_ = tL_ + ml;
    teEndR_ = tR_ + mr;
    teStepL_ = (teEndL_ - teL_) * (1.0 / kSeg);
    teStepR_ = (teEndR_ - teR_) * (1.0 / kSeg);

    fading_ = fadeL_ >= 0 || fadeR_ >= 0;
    if (fading_) {
        wowNow(tBL_, tBR_, ml, mr);
        teBEndL_ = tBL_ + ml;
        teBEndR_ = tBR_ + mr;
        if (newL) teBL_ = teBEndL_;   // a new head starts where it will be: its gain is still near 0
        if (newR) teBR_ = teBEndR_;
    }
    if (fadeL_ < 0) {   // no fade on this side: B reads what A does, at gain 0
        teBL_ = teL_;
        teBEndL_ = teEndL_;
    }
    if (fadeR_ < 0) {
        teBR_ = teR_;
        teBEndR_ = teEndR_;
    }
    teBStepL_ = (teBEndL_ - teBL_) * (1.0 / kSeg);
    teBStepR_ = (teBEndR_ - teBR_) * (1.0 / kSeg);
    fadeGains(fadeL_, gainEnd_[0], gainEnd_[2]);
    fadeGains(fadeR_, gainEnd_[1], gainEnd_[3]);
    for (int k = 0; k < 4; ++k) gainStep_[k] = (gainEnd_[k] - gain_[k]) * (1.0f / kSeg);

    duckEnd_ = 1.0f / (1.0f + kDuckLaw * duckAmt_ * env_);
    duckStep_ = (duckEnd_ - duck_) * (1.0f / kSeg);
    segLeft_ = kSeg;
}

void Delay::process(float* L, float* R, int n) {
    if (n <= 0) return;
    const float inv = 1.0f / static_cast<float>(n);
    bool moving = false;
    for (int k = 0; k < kRamps; ++k) {
        step_[k] = (tgt_[k] - cur_[k]) * inv;
        moving = moving || tgt_[k] != cur_[k];
    }
    // The diffusers run while Diffuse is above 0 or on its way to or from it. Starting again they
    // start empty: what they held is from before the reset, or from before Diffuse sat at 0.
    const bool diffusing = cur_[DIFFUSE] != 0.0f || tgt_[DIFFUSE] != 0.0f;
    if (diffusing && apStale_) {
        std::fill(ap_.begin(), ap_.end(), 0.0f);
        for (auto& side : apPos_)
            for (int& pos : side) pos = 0;
        apStale_ = false;
    }
    if (!diffusing) apStale_ = true;
    diffused_ = diffusing;
    for (int i = 0; i < n;) {
        if (segLeft_ == 0) segment();
        const int m = std::min(n - i, segLeft_);
        if (diffusing) {
            if (moving) {
                if (fading_) diffuseRun<true, true>(L + i, R + i, m);
                else diffuseRun<true, false>(L + i, R + i, m);
            } else {
                if (fading_) diffuseRun<false, true>(L + i, R + i, m);
                else diffuseRun<false, false>(L + i, R + i, m);
            }
        } else if (moving) {   // EffectForce's Delay
            if (fading_) run<true, true>(L + i, R + i, m);
            else run<true, false>(L + i, R + i, m);
        } else {
            if (fading_) run<false, true>(L + i, R + i, m);
            else run<false, false>(L + i, R + i, m);
        }
        segLeft_ -= m;
        i += m;
    }
    for (int k = 0; k < kRamps; ++k) cur_[k] = tgt_[k];   // lands exactly
}

template <bool Moving, bool Fading>
void Delay::run(float* L, float* R, int n) {
    // Locals: the compiler can't keep members in registers across the stores to L, R and the lines.
    float* const bl = lineL_.data();
    float* const br = lineR_.data();
    int w = w_, written = written_;
    double teL = teL_, teR = teR_;
    const double dl = teStepL_, dr = teStepR_;
    Lr lp = {lpL_, lpR_}, hp = {hpL_, hpR_}, gain = {gainL_, gainR_}, env = both(env_), duck = both(duck_);
    const Lr dDuck = both(duckStep_), attack = both(kAttack), fall = both(kFall), release = both(kRelease);
    Lr fb = both(cur_[FB]), drive = both(cur_[DRIVE]), mix = both(cur_[MIX]), aLp = both(cur_[LP]), aHp = both(cur_[HP]);
    Lr monoIn = both(cur_[MONO_IN]), cross = both(cur_[CROSS]), rIn = Lr{1.0f, cur_[R_IN]};
    const Lr sFb = both(step_[FB]), sDrive = both(step_[DRIVE]), sMix = both(step_[MIX]), sLp = both(step_[LP]),
             sHp = both(step_[HP]), sMonoIn = both(step_[MONO_IN]), sCross = both(step_[CROSS]), sRIn = Lr{0.0f, step_[R_IN]};
    double teBL = teBL_, teBR = teBR_;
    const double dbl = teBStepL_, dbr = teBStepR_;
    Lr gainA = {gain_[0], gain_[1]}, gainB = {gain_[2], gain_[3]};
    const Lr sGainA = {gainStep_[0], gainStep_[1]}, sGainB = {gainStep_[2], gainStep_[3]};
    if (written < kLen) {
        hideOld(bl, w, written, teL, dl, n);
        hideOld(br, w, written, teR, dr, n);
        if (Fading) {
            hideOld(bl, w, written, teBL, dbl, n);
            hideOld(br, w, written, teBR, dbr, n);
        }
    }

    for (int k = 0; k < n; ++k) {
        if (Moving) {
            fb += sFb;
            drive += sDrive;
            mix += sMix;
            aLp += sLp;
            aHp += sHp;
            monoIn += sMonoIn;
            cross += sCross;
            rIn += sRIn;
        }
        teL += dl;
        teR += dr;
        duck += dDuck;
        const Lr x = finiteLr(loadLr(L + k, R + k));

        Lr r = readLr(bl, br, w, teL, teR);
        if (Fading) {   // and the new head, crossfaded in
            teBL += dbl;
            teBR += dbr;
            gainA += sGainA;
            gainB += sGainB;
            r = r * gainA + readLr(bl, br, w, teBL, teBR) * gainB;
        }

        // The cuts, at the tap: the wet and the feedback both pass them.
        lp += aLp * (r - lp);
        hp += aHp * (lp - hp);
        const Lr y = lp - hp;

        // Into the lines: the input (or its mono sum; none into R in Ping-Pong) and the feedback
        // (from the other side in Ping-Pong). Halves first: L + R could overflow.
        const Lr sum = both(0.5f) * x + both(0.5f) * swapLr(x);
        const Lr in = (x * (both(1.0f) - monoIn) + sum * monoIn) * rIn;
        const Lr back = y * (both(1.0f) - cross) + swapLr(y) * cross;
        const Lr u = minLr(maxLr(in + fb * back + both(kTiny), both(-kClamp)), both(kClamp));

        const Lr c = minLr(maxLr(u, both(-0.5f)), both(0.5f));
        Lr v = u + drive * (c - both(4.0f / 3.0f) * c * c * c - u);

        // The limiter: the gain recovers, then drops at once to what keeps this sample at 1.
        const Lr a = absLr(v);
        gain = minLr(gain * release, both(1.0f));
        gain = pickLr(greaterLr(a * gain, both(1.0f)), recipLr(maxLr(a, both(1.0f))), gain);
        v *= gain;

        storeLr(bl + w, br + w, v);
        if (w < kGuard) storeLr(bl + kLen + w, br + kLen + w, v);
        w = w + 1 == kLen ? 0 : w + 1;

        // The duck's envelope follows the input's louder side (up to +18 dBFS: a wild sample
        // mustn't hold the wet down for long).
        const Lr level = minLr(louderLr(absLr(x)), both(kClamp)) + both(kTiny);
        env += pickLr(greaterLr(level, env), attack, fall) * (level - env);

        storeLr(L + k, R + k, x * (both(1.0f) - mix) + y * duck * mix);   // exact at mix 0
    }

    w_ = w;
    written_ = std::min(written + n, kLen);
    teL_ = teL;
    teR_ = teR;
    lpL_ = lp[0];
    lpR_ = lp[1];
    hpL_ = hp[0];
    hpR_ = hp[1];
    gainL_ = gain[0];
    gainR_ = gain[1];
    env_ = env[0];
    duck_ = duck[0];
    if (Fading) {
        teBL_ = teBL;
        teBR_ = teBR;
        gain_[0] = gainA[0];
        gain_[1] = gainA[1];
        gain_[2] = gainB[0];
        gain_[3] = gainB[1];
    }
    if (Moving) {
        cur_[FB] = fb[0];
        cur_[DRIVE] = drive[0];
        cur_[MIX] = mix[0];
        cur_[LP] = aLp[0];
        cur_[HP] = aHp[0];
        cur_[MONO_IN] = monoIn[0];
        cur_[CROSS] = cross[0];
        cur_[R_IN] = rIn[1];
    }
}

// The loop while the diffusers run (Diffuse above 0, or on its way to or from it): run()'s, cut in
// two around the allpasses so that they can take four samples at a time. A sub-run reads both lines
// and passes the cuts sample by sample (into y), the allpasses take y a side, four samples at a time,
// and blend it (diffuseSides()), and each sample is then fed back, driven, limited, written and
// sent out as in run(). That is exactly what running it all sample by sample would give as long as
// no read in a sub-run reaches a sample written in it, so a sub-run is never longer than the
// shortest read's age less the Hermite kernel's reach: a whole segment unless a read comes within
// 0.8 ms (the shortest times, with deep wow or a negative spread). The allpasses never read
// anything written in the same run: their shortest is 322 samples. (One loop with the allpasses in
// it, their rings gathered before the run and scattered after, counted 13% more ARM instructions:
// run()'s loop is short of registers already.)
template <bool Moving, bool Fading>
void Delay::diffuseRun(float* L, float* R, int n) {
    float* const bl = lineL_.data();
    float* const br = lineR_.data();
    int w = w_, written = written_;
    double teL = teL_, teR = teR_;
    const double dl = teStepL_, dr = teStepR_;
    Lr lp = {lpL_, lpR_}, hp = {hpL_, hpR_}, gain = {gainL_, gainR_}, env = both(env_), duck = both(duck_);
    const Lr dDuck = both(duckStep_), attack = both(kAttack), fall = both(kFall), release = both(kRelease);
    Lr fb = both(cur_[FB]), drive = both(cur_[DRIVE]), mix = both(cur_[MIX]), aLp = both(cur_[LP]), aHp = both(cur_[HP]);
    Lr monoIn = both(cur_[MONO_IN]), cross = both(cur_[CROSS]), rIn = Lr{1.0f, cur_[R_IN]};
    const Lr sFb = both(step_[FB]), sDrive = both(step_[DRIVE]), sMix = both(step_[MIX]), sLp = both(step_[LP]),
             sHp = both(step_[HP]), sMonoIn = both(step_[MONO_IN]), sCross = both(step_[CROSS]), sRIn = Lr{0.0f, step_[R_IN]};
    float dif = cur_[DIFFUSE], monoOut = cur_[MONO_OUT];
    const float sDif = step_[DIFFUSE], sMonoOut = step_[MONO_OUT];
    double teBL = teBL_, teBR = teBR_;
    const double dbl = teBStepL_, dbr = teBStepR_;
    Lr gainA = {gain_[0], gain_[1]}, gainB = {gain_[2], gain_[3]};
    const Lr sGainA = {gainStep_[0], gainStep_[1]}, sGainB = {gainStep_[2], gainStep_[3]};
    if (written < kLen) {
        hideOld(bl, w, written, teL, dl, n);
        hideOld(br, w, written, teR, dr, n);
        if (Fading) {
            hideOld(bl, w, written, teBL, dbl, n);
            hideOld(br, w, written, teBR, dbr, n);
        }
    }

    float y[2 * kSeg], difs[kSeg], monos[kSeg];   // a sub-run's repeats (L and R side by side), the blends' ramps
    for (int i = 0; i < n;) {
        // The shortest read in what is left of the run (the times move in straight lines: at an end).
        const int left = n - i;
        double age = std::min({teL + dl, teL + left * dl, teR + dr, teR + left * dr});
        if (Fading) age = std::min({age, teBL + dbl, teBL + left * dbl, teBR + dbr, teBR + left * dbr});
        const int m = std::clamp(static_cast<int>(age) - 2, 1, left);

        // The reads and the cuts, as in run().
        for (int k = 0, wk = w; k < m; ++k) {
            if (Moving) {
                aLp += sLp;
                aHp += sHp;
                difs[k] = dif += sDif;
                monos[k] = monoOut += sMonoOut;
            }
            teL += dl;
            teR += dr;
            Lr r = readLr(bl, br, wk, teL, teR);
            if (Fading) {
                teBL += dbl;
                teBR += dbr;
                gainA += sGainA;
                gainB += sGainB;
                r = r * gainA + readLr(bl, br, wk, teBL, teBR) * gainB;
            }
            lp += aLp * (r - lp);
            hp += aHp * (lp - hp);
            storePair(&y[2 * k], lp - hp);
            wk = wk + 1 == kLen ? 0 : wk + 1;
        }

        diffuseSides<Moving>(ap_.data(), apPos_, y, m, difs, monos, dif, monoOut);

        // Fed back, driven, limited, written and sent out, as in run().
        for (int k = 0; k < m; ++k) {
            if (Moving) {
                fb += sFb;
                drive += sDrive;
                mix += sMix;
                monoIn += sMonoIn;
                cross += sCross;
                rIn += sRIn;
            }
            duck += dDuck;
            float* const pl = L + i + k;
            float* const pr = R + i + k;
            const Lr x = finiteLr(loadLr(pl, pr));
            const Lr yk = loadPair(&y[2 * k]);

            const Lr sum = both(0.5f) * x + both(0.5f) * swapLr(x);
            const Lr in = (x * (both(1.0f) - monoIn) + sum * monoIn) * rIn;
            const Lr back = yk * (both(1.0f) - cross) + swapLr(yk) * cross;
            const Lr u = minLr(maxLr(in + fb * back + both(kTiny), both(-kClamp)), both(kClamp));

            const Lr c = minLr(maxLr(u, both(-0.5f)), both(0.5f));
            Lr v = u + drive * (c - both(4.0f / 3.0f) * c * c * c - u);

            const Lr a = absLr(v);
            gain = minLr(gain * release, both(1.0f));
            gain = pickLr(greaterLr(a * gain, both(1.0f)), recipLr(maxLr(a, both(1.0f))), gain);
            v *= gain;

            storeLr(bl + w, br + w, v);
            if (w < kGuard) storeLr(bl + kLen + w, br + kLen + w, v);
            w = w + 1 == kLen ? 0 : w + 1;

            const Lr level = minLr(louderLr(absLr(x)), both(kClamp)) + both(kTiny);
            env += pickLr(greaterLr(level, env), attack, fall) * (level - env);

            storeLr(pl, pr, x * (both(1.0f) - mix) + yk * duck * mix);
        }
        i += m;
    }

    w_ = w;
    written_ = std::min(written + n, kLen);
    teL_ = teL;
    teR_ = teR;
    lpL_ = lp[0];
    lpR_ = lp[1];
    hpL_ = hp[0];
    hpR_ = hp[1];
    gainL_ = gain[0];
    gainR_ = gain[1];
    env_ = env[0];
    duck_ = duck[0];
    if (Fading) {
        teBL_ = teBL;
        teBR_ = teBR;
        gain_[0] = gainA[0];
        gain_[1] = gainA[1];
        gain_[2] = gainB[0];
        gain_[3] = gainB[1];
    }
    if (Moving) {
        cur_[FB] = fb[0];
        cur_[DRIVE] = drive[0];
        cur_[MIX] = mix[0];
        cur_[LP] = aLp[0];
        cur_[HP] = aHp[0];
        cur_[MONO_IN] = monoIn[0];
        cur_[CROSS] = cross[0];
        cur_[R_IN] = rIn[1];
        cur_[DIFFUSE] = dif;
        cur_[MONO_OUT] = monoOut;
    }
}

} // namespace af
