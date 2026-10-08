#include "airvoices.h"

#include <algorithm>
#include <cmath>

namespace af {

namespace {

constexpr double kRateD = 44100.0;
constexpr double kTwoPi = 6.283185307179586;
constexpr double kLn1000 = 6.907755278982137;    // -60 dB, in nepers
constexpr float kLog2Thousand = 9.96578428f;      // -60 dB, in octaves of amplitude
constexpr float kFloor = 3.1622777e-5f;           // -90 dBFS: under it for kQuietSamples, a voice is free
constexpr int kQuietSamples = 32;
constexpr int kStealSamples = 88;                 // 2 ms: a taken voice's fade
constexpr int kRiseSamples = 66;                  // 1.5 ms: the modes' raised-cosine rise
constexpr int kAgeMost = 1 << 30;                 // a note's age stops here (6.8 hours)
constexpr double kMaxModeHz = 18000.0;            // a mode above this is left out
constexpr double kMaxRingS = 60.0;                // the pluck's loop never rings longer than this
constexpr int kFeltRise = 220;                    // 5 ms: Felt's attack
constexpr int kFeltMove = 512;                    // 11.6 ms: Felt's Age moves to another frame at most this often
constexpr float kFeltAge0 = 0.05f, kFeltAgeSpan = 0.75f;   // Age from 0.05 to 0.8 over the decay

// The modal sounds: each mode's frequency over the note's, its amplitude, and its decay divisor
// (the mode rings decayS / d to -60 dB).
struct Modal {
    float ratio[AirVoices::kModes], amp[AirVoices::kModes], div[AirVoices::kModes];
};
constexpr Modal kModal[4] = {
    {{1.0f, 2.32f, 4.25f, 6.63f, 9.38f, 12.5f},     // Glass
     {1.0f, 0.6f, 0.4f, 0.25f, 0.15f, 0.1f},
     {1.0f, 1.6f, 2.4f, 3.5f, 4.8f, 6.0f}},
    {{1.0f, 1.004f, 2.71f, 2.717f, 5.12f, 8.18f},   // Bowl
     {1.0f, 0.8f, 0.5f, 0.4f, 0.25f, 0.12f},
     {1.0f, 1.0f, 1.5f, 1.5f, 2.2f, 3.0f}},
    {{1.0f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f},   // Bar
     {1.0f, 0.5f, 0.25f, 0.12f, 0.06f, 0.03f},
     {1.0f, 2.0f, 3.5f, 5.0f, 7.0f, 9.0f}},
    {{0.5f, 1.0f, 1.183f, 1.506f, 2.0f, 2.514f},    // Bell
     {0.6f, 1.0f, 0.7f, 0.5f, 0.6f, 0.3f},
     {0.6f, 1.0f, 1.4f, 1.8f, 2.2f, 3.0f}},
};

// Each sound's scale, so that a strike at velocity 1 in the middle of the keyboard (note 72, Tone
// 16000, pan 0) peaks at -6 dBFS (measured by test/airvoices_test.cpp's levels check).
constexpr float kScale[AS_COUNT] = {0.2350f, 0.1760f, 0.2610f, 0.1740f, 0.218f, 0.4018f};
// A Felt Piano frame's peak (1.25 measured, the frames being a full-scale sine's RMS), a little
// over: Felt's level for sleeping and stealing comes from its envelope, not its samples.
constexpr float kFeltPeak = 1.3f;

// The modes' rise: a raised cosine reaching 1 at sample kRiseSamples - 1, then 1 for a control
// step's worth past it, so a step that ends the rise reads on without a test.
struct RiseTable {
    float g[kRiseSamples + kChunk];
    RiseTable() {
        for (int i = 0; i < kRiseSamples + kChunk; ++i) {
            const double x = i < kRiseSamples ? static_cast<double>(i + 1) / kRiseSamples : 1.0;
            g[i] = static_cast<float>(0.5 - 0.5 * std::cos(0.5 * kTwoPi * x));
        }
    }
};
const RiseTable kRise;

// Equal-power pan gains for -1..1: unity in the middle (exactly, so L and R are equal there), sqrt 2
// at the sides (Ground's).
f2 panGains(float pan) {
    if (pan == 0.0f) return splat2(1.0f);
    const float a = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.785398163f;   // 0 .. pi/2
    return f2{1.41421356f * sinQuarter(1.570796327f - a), 1.41421356f * sinQuarter(a)};
}

// The four lanes' sum, in both lanes of a pair: (s0 + s2) + (s1 + s3) on both sides of the build.
AF_INLINE f2 sum4(f4 s) {
#if AF_NEON
    const float32x2_t t = vadd_f32(vget_low_f32(s), vget_high_f32(s));
    return vpadd_f32(t, t);
#else
    const float x = (s[0] + s[2]) + (s[1] + s[3]);
    return f2{x, x};
#endif
}

// sqrt per lane, 0 at 0: NEON's reciprocal square-root estimate and two refining steps (about 1e-6).
AF_INLINE f4 sqrt4(f4 x) {
#if AF_NEON
    const f4 y = vmaxq_f32(x, vdupq_n_f32(1e-30f));
    f4 r = vrsqrteq_f32(y);
    r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(y, r), r));
    r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(y, r), r));
    return vmulq_f32(x, r);
#else
    return f4{std::sqrt(x[0]), std::sqrt(x[1]), std::sqrt(x[2]), std::sqrt(x[3])};
#endif
}

double hzOf(double pitch) { return 440.0 * std::exp2((pitch - 69.0) / 12.0); }

// z c for four modes, its real and imaginary parts. NEON multiplies and subtracts (or adds) a
// product in one instruction; written as operators, GCC negates the product first.
AF_INLINE void turn(f4 zr, f4 zi, f4 cr, f4 ci, f4& r, f4& i) {
#if AF_NEON
    r = vfmsq_f32(vmulq_f32(zr, cr), zi, ci);
    i = vfmaq_f32(vmulq_f32(zr, ci), zi, cr);
#else
    r = zr * cr - zi * ci;
    i = zr * ci + zi * cr;
#endif
}

// acc + p x: a mono sample into its pan's two gains. NEON multiplies by the scalar in place.
AF_INLINE f2 panIn(f2 acc, f2 p, float x) {
#if AF_NEON
    return vmla_n_f32(acc, p, x);
#else
    return acc + p * splat2(x);
#endif
}

// The modes: each sample every z turns by its c (z = z c), and the voice's output is the sum of
// their imaginary parts. Two samples a pass, the state going from one set of registers to the other
// and back (one sample a pass, the compiler copies all four vectors every sample). Rise: the
// strike's first kRiseSamples, the raised cosine on the output.
template <bool Rise>
void modalLoop(f4* zr, f4* zi, const f4* cr, const f4* ci, f2 P, f2 dP, const float* rise, f2* bus, int m) {
    f4 ar0 = zr[0], ai0 = zi[0], ar1 = zr[1], ai1 = zi[1], br0, bi0, br1, bi1;
    const f4 cr0 = cr[0], ci0 = ci[0], cr1 = cr[1], ci1 = ci[1];
    auto out = [&](f4 i0, f4 i1, int j) {
        f2 y = sum4(i0 + i1);
        if constexpr (Rise) y = y * splat2(rise[j]);
        P = P + dP;
        bus[j] = bus[j] + P * y;
    };
    int j = 0;
    for (; j + 2 <= m; j += 2) {
        turn(ar0, ai0, cr0, ci0, br0, bi0);
        turn(ar1, ai1, cr1, ci1, br1, bi1);
        out(bi0, bi1, j);
        turn(br0, bi0, cr0, ci0, ar0, ai0);
        turn(br1, bi1, cr1, ci1, ar1, ai1);
        out(ai0, ai1, j + 1);
    }
    if (j < m) {   // an odd step (a steal's fade, a short render)
        turn(ar0, ai0, cr0, ci0, br0, bi0);
        turn(ar1, ai1, cr1, ci1, br1, bi1);
        out(bi0, bi1, j);
        ar0 = br0;
        ai0 = bi0;
        ar1 = br1;
        ai1 = bi1;
    }
    zr[0] = ar0;
    zi[0] = ai0;
    zr[1] = ar1;
    zi[1] = ai1;
}

// The bus (L and R side by side) to zero, two samples a store on NEON.
void clearBus(f2* bus, int n) {
    int i = 0;
#if AF_NEON
    float* const b = reinterpret_cast<float*>(bus);   // a vector type aliases its element type
    for (; i + 2 <= n; i += 2) vst1q_f32(b + 2 * i, vdupq_n_f32(0.0f));
#endif
    for (; i < n; ++i) bus[i] = splat2(0.0f);
}

// The bus into the outputs, and into the send at a gain gliding from s0 by ds a sample (the first
// sample at s0 + ds). On NEON four samples a pass, the bus taken apart into L and R as it loads
// (about 6 ARM instructions a sample): a sample at a time, GCC moved every R through a core
// register and back, at three times the cost.
void mixOut(const f2* bus, float* outL, float* outR, float* sendL, float* sendR, float s0, float ds, int n) {
    int i = 0;
#if AF_NEON
    const float* const b = reinterpret_cast<const float*>(bus);
    const float first[4] = {s0 + ds, s0 + 2.0f * ds, s0 + 3.0f * ds, s0 + 4.0f * ds};
    f4 s = vld1q_f32(first);
    const f4 step = vdupq_n_f32(4.0f * ds);
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t d = vld2q_f32(b + 2 * i);
        vst1q_f32(outL + i, vaddq_f32(vld1q_f32(outL + i), d.val[0]));
        vst1q_f32(outR + i, vaddq_f32(vld1q_f32(outR + i), d.val[1]));
        vst1q_f32(sendL + i, vmlaq_f32(vld1q_f32(sendL + i), d.val[0], s));
        vst1q_f32(sendR + i, vmlaq_f32(vld1q_f32(sendR + i), d.val[1], s));
        s = vaddq_f32(s, step);
    }
#endif
    for (; i < n; ++i) {
        const float g = s0 + ds * static_cast<float>(i + 1);
        const f2 d = bus[i];
        outL[i] += d[0];
        outR[i] += d[1];
        sendL[i] += d[0] * g;
        sendR[i] += d[1] * g;
    }
}

// A mono read into the bus at gains P + dP (i + 1). On NEON two samples a pass, their four gains in
// one vector: 6 ARM instructions a sample, where a sample a pass took 8.
AF_INLINE void mixMono(const float* x, f2 P, f2 dP, f2* bus, int m) {
    int i = 0;
#if AF_NEON
    float* const b = reinterpret_cast<float*>(bus);
    const f2 p1 = vadd_f32(P, dP);
    f4 p = vcombine_f32(p1, vadd_f32(p1, dP));
    const f2 d2 = vadd_f32(dP, dP);
    const f4 step = vcombine_f32(d2, d2);
    for (; i + 2 <= m; i += 2) {
        const f4 y = vcombine_f32(vld1_dup_f32(x + i), vld1_dup_f32(x + i + 1));   // x0 x0 x1 x1
        vst1q_f32(b + 2 * i, vmlaq_f32(vld1q_f32(b + 2 * i), p, y));
        p = vaddq_f32(p, step);
    }
    P = vsub_f32(vget_low_f32(p), dP);   // the gain before sample i
#endif
    for (; i < m; ++i) {
        P = P + dP;
        bus[i] = panIn(bus[i], P, x[i]);
    }
}

// The pluck's burst noise, a sample on: white noise (x), then two one-poles at Tone (12 dB an
// octave), whose output it returns. Every pass over a burst draws its noise here: the strike's pass
// for the mean and the samples drawn as the burst plays must be the same numbers, or the mean
// doesn't cancel.
AF_INLINE float burstNoise(uint32_t& rng, float& lp1, float& lp2, float tone, float& x) {
    x = randBipolar(rng);
    lp1 += tone * (x - lp1);
    lp2 += tone * (lp1 - lp2);
    return lp2;
}

// The burst's window comes from a phasor (c, s) turned by (tc, ts) a sample: sin^2 (pi (i + 1/2) /
// len) = (1 - c) / 2 before the turn.
AF_INLINE void turnPhasor(float& c, float& s, float tc, float ts) {
    const float nc = c * tc - s * ts;
    s = c * ts + s * tc;
    c = nc;
}

// The largest |x[i]| over m samples, and over `most`. NEON four at a time: in the loop that draws the
// samples it took 5 of its 29 instructions a sample.
AF_INLINE float peakOf(const float* x, int m, float most) {
    int i = 0;
#if AF_NEON
    f4 mx = vdupq_n_f32(0.0f);
    for (; i + 4 <= m; i += 4) mx = vmaxq_f32(mx, vabsq_f32(vld1q_f32(x + i)));
    const float32x2_t h = vpmax_f32(vget_low_f32(mx), vget_high_f32(mx));
    most = std::max(most, vget_lane_f32(vpmax_f32(h, h), 0));
#endif
    for (; i < m; ++i) most = std::max(most, std::fabs(x[i]));
    return most;
}

// The pluck's loop state, as locals while a step runs (members could alias the buffers).
struct Pluck {
    float* line;
    int w, delay, mask;
    float eta, ap, a, lp, g;
};

// The pluck's loop: the line read whole samples back, the allpass for the fraction (transposed, so
// one state: y = eta x + s, s = x - eta y), the low-pass, the loop gain, plus the burst while it
// lasts. The output is what goes into the line. Returns the sum of the output's squares. Wrap: the
// read or the write runs past the line's end in this step, so each index is masked as it moves.
template <bool Burst, bool Wrap>
float pluckRun(Pluck& k, const float* burst, f2 P, f2 dP, f2* bus, int m) {
    float* const line = k.line;
    const int mask = k.mask;
    const float eta = k.eta, a = k.a, g = k.g;
    int w = k.w, rd = (k.w - k.delay) & mask;
    float ap = k.ap, s = k.lp, e = 0.0f;
    for (int i = 0; i < m; ++i) {
        const float r = line[rd];
        const float y = eta * r + ap;
        ap = r - eta * y;
        s += a * (y - s);
        float x = g * s;
        if constexpr (Burst) x += burst[i];
        line[w] = x;
        if constexpr (Wrap) {
            rd = (rd + 1) & mask;
            w = (w + 1) & mask;
        } else {
            ++rd;
            ++w;
        }
        e += x * x;
        P = P + dP;
        bus[i] = panIn(bus[i], P, x);
    }
    k.w = w & mask;
    k.ap = ap;
    k.lp = s;
    return e;
}

// A step of the loop. Neither index wraps in most steps (each does once in kLine / m of them), and
// those run without the masks (3.4k ARM instructions a block fewer with six plucks ringing).
template <bool Burst>
float pluckLoop(Pluck& k, const float* burst, f2 P, f2 dP, f2* bus, int m) {
    const int last = k.mask + 1 - m, rd = (k.w - k.delay) & k.mask;
    return k.w <= last && rd <= last ? pluckRun<Burst, false>(k, burst, P, dP, bus, m)
                                     : pluckRun<Burst, true>(k, burst, P, dP, bus, m);
}

} // namespace

AirVoices::AirVoices() {
    set(AirVoicePatch{}, HarmonyPatch{});
    reset();
}

void AirVoices::seed(uint32_t s) {
    seed_ = s;
    reset();
}

void AirVoices::set(const AirVoicePatch& p, const HarmonyPatch& h) {
    p_.sound = std::min(std::max(p.sound, 0), AS_COUNT - 1);
    p_.toneHz = clampParam(p.toneHz, 200.0f, 16000.0f, 6000.0f);
    p_.decayS = clampParam(p.decayS, 0.1f, 20.0f, 4.0f);
    p_.width = lifeosc::unit(p.width);
    h_ = h;
}

void AirVoices::reset() {
    rng_ = seed_ * 0x9E3779B1u + 0x85EBCA6Bu;   // nearby seeds far apart
    if (rng_ == 0) rng_ = 0x85EBCA6Bu;          // xorshift's one stuck state
    xorshift(rng_);
    strikes_ = 0;
    send_ = -1.0f;   // none yet: the first render's send starts where it is asked
    for (Voice& v : v_) {
        v.stage = VS_FREE;
        v.P = splat2(0.0f);
        v.level = 0.0f;
        v.table = nullptr;
    }
}

void AirVoices::strike(int note, float vel, float pan) {
    Strike s;
    s.note = std::min(std::max(note, 24), 108);
    s.sound = p_.sound;
    s.vel = clampParam(vel, 0.0f, 1.0f, 0.0f);
    s.pan = clampParam(pan, -1.0f, 1.0f, 0.0f);
    s.toneHz = p_.toneHz;
    s.decayS = p_.decayS;
    s.pitch = tunedPitch(h_, s.note);
    s.order = strikes_++;

    // A free voice; else the quietest ringing one; else (every voice already fading for a steal) the
    // one whose waiting note was struck first, which this one replaces.
    Voice* take = nullptr;
    for (Voice& v : v_)
        if (v.stage == VS_FREE) {
            take = &v;
            break;
        }
    if (!take)
        for (Voice& v : v_)
            if (v.stage == VS_RING && (!take || v.level < take->level)) take = &v;
    if (!take)
        for (Voice& v : v_)
            if (!take || v.next.order < take->next.order) take = &v;
    if (take->stage == VS_FREE) {
        begin(*take, s);
    } else {
        if (take->stage == VS_RING) {
            take->stage = VS_STEAL;
            take->stealLeft = kStealSamples;
        }
        take->next = s;
    }
}

void AirVoices::begin(Voice& v, const Strike& s) {
    v.now = s;
    v.stage = VS_RING;
    v.stealLeft = 0;
    v.quiet = 0;
    v.age = 0;
    v.vel = 0.25f + 0.75f * s.vel;
    const f2 pan = panGains(s.pan);
    v.panMax = std::max(pan[0], pan[1]);
    v.pan = pan * splat2(v.vel);
    v.firstPass = 0;
    const double hz = hzOf(s.pitch);
    if (s.sound == AS_KALIMBA) beginPluck(v, s, hz);
    else if (s.sound == AS_FELT) beginFelt(v, s, hz);
    else beginModal(v, s, hz);
}

void AirVoices::beginModal(Voice& v, const Strike& s, double hz) {
    const Modal& md = kModal[s.sound];
    float zr[8] = {}, cr[8] = {}, ci[8] = {};
    float level = 0.0f;
    for (int k = 0; k < kModes; ++k) {
        const double f = hz * md.ratio[k];
        if (f > kMaxModeHz) continue;
        const double q = f / s.toneHz, lp = 1.0 / std::sqrt(1.0 + q * q * q * q);   // 12 dB an octave over Tone
        const double r = std::exp(-kLn1000 * md.div[k] / (static_cast<double>(s.decayS) * kRateD));
        const double w = kTwoPi * f / kRateD;
        // Worked out in double and rounded once. At Decay 20 s the slowest mode's |c| is 1 - 4.7e-6,
        // 80 of float's steps under 1, so rounding can't take it to 1; checked all the same, since a
        // mode at or past 1 would ring for ever, or grow.
        float c = static_cast<float>(r * std::cos(w)), d = static_cast<float>(r * std::sin(w));
        const double m2 = static_cast<double>(c) * c + static_cast<double>(d) * d;
        if (m2 >= 1.0) {
            const double pull = (1.0 - 1e-6) / std::sqrt(m2);
            c = static_cast<float>(c * pull);
            d = static_cast<float>(d * pull);
        }
        zr[k] = static_cast<float>(md.amp[k] * lp) * kScale[s.sound];
        cr[k] = c;
        ci[k] = d;
        level += zr[k];
    }
    for (int j = 0; j < 2; ++j) {
        v.zr[j] = load4(zr + 4 * j);
        v.zi[j] = splat(0.0f);
        v.cr[j] = load4(cr + 4 * j);
        v.ci[j] = load4(ci + 4 * j);
    }
    v.P = v.pan;   // the rise shapes the start
    v.level = level * v.vel;
}

void AirVoices::beginPluck(Voice& v, const Strike& s, double hz) {
    const double period = kRateD / hz, w0 = kTwoPi * hz / kRateD, cw = std::cos(w0), sw = std::sin(w0);
    // The loop gain that takes the fundamental down 60 dB in decayS, and the most the loop may
    // have anywhere (so it never rings longer than kMaxRingS: under 1, the loop is stable).
    const double want = std::pow(10.0, -3.0 / (static_cast<double>(s.decayS) * hz));
    const double most = std::pow(10.0, -3.0 / (kMaxRingS * hz));
    // The low-pass's pole: at Tone, unless Tone would take more from the fundamental each pass than
    // the most gain could make up (a dark Tone, a high note, a long Decay). Then it moves up just
    // far enough: the pole whose |H(w0)| is want / most, the smaller root of
    // (1 - b)^2 = T^2 (1 - 2 b cos w0 + b^2).
    const double t = want / most, t2 = t * t;
    const double c = (1.0 - t2 * cw) / (1.0 - t2);
    const double b = std::min(std::exp(-kTwoPi * s.toneHz / kRateD), c - std::sqrt(c * c - 1.0));
    const double a = 1.0 - b;
    // Its gain a pass at the fundamental, |a / (1 - b e^-iw0)|.
    const double lpGain = a / std::hypot(1.0 - b * cw, b * sw);
    // The tuning. The fundamental falls 60 dB in decayS, so its pole sits inside the unit circle, at
    // z0 = e^(-sigma + i w0), e^-sigma its fall a sample. Off the circle a low-pass's slope pulls the
    // pole's frequency down (a loop tuned on the circle played notes 45 to 47 2 cents flat at Tone
    // 200 and Decay 0.33), so the phase delays are taken at z0. The low-pass's there is its own with
    // b e^sigma for b.
    const double rho = std::exp(kLn1000 / (static_cast<double>(s.decayS) * kRateD)), br = b * rho;
    const double lpDelay = std::atan2(br * sw, 1.0 - br * cw) / w0;
    // The read: whole samples, chosen once, then a first-order allpass for the rest, tau, which stays
    // within 0.5..1.5, where its phase delay is smoothest. The allpass loses nothing at any frequency,
    // so Decay alone sets the fundamental's ring (Lagrange's cubic took up to 0.03 dB a pass at the
    // top of the keyboard). Its coefficient is the one whose phase delay at z0 is tau exactly: with
    // u = 1 / z0, arg((eta + u) / (1 + eta u)) = -w0 tau is A eta^2 + B eta + C = 0, and eta is the
    // root near (1 - tau) / (1 + tau) (the other is near -1), written so nothing cancels. Nothing is
    // iterated, so nothing has to settle: an iteration that chose the whole samples afresh each time
    // could swap them back and forth, and left the top notes up to 10 cents off at short Decays.
    const double d = std::min(std::max(period - lpDelay, 1.5), static_cast<double>(kLine - 4));
    const int whole = static_cast<int>(std::floor(d - 0.5));
    const double tau = d - whole;
    const double A = rho * std::sin(w0 * (1.0 + tau)), B = (1.0 + rho * rho) * std::sin(w0 * tau);
    const double C = -rho * std::sin(w0 * (1.0 - tau));
    const double eta = 2.0 * C / (-B - std::sqrt(std::max(B * B - 4.0 * A * C, 0.0)));
    const double g = std::min(want / lpGain, most);
    v.delay = whole;
    v.eta = static_cast<float>(eta);
    v.ap = 0.0f;
    v.lpA = static_cast<float>(a);
    v.lp = 0.0f;
    v.loopG = static_cast<float>(g);
    v.holdLog2 = static_cast<float>(std::log2(g * lpGain) / period);
    // The loop's DC mode: its real pole p, just under 1, where g AP(p) LP(p) p^-whole = 1. A burst
    // excites it by B(p) = sum b_n p^-n, so the burst's mean is taken out with the same weights, which
    // leaves it nothing to ring. (A plain mean leaves B(1) at 0 instead, and B(p) as much as 25 dB
    // under the fundamental at Decay 0.5 and note 24, falling no faster than the note.) Newton from
    // p = g^(1 / the loop's delay at DC); twice is plenty.
    double p = std::pow(g, 1.0 / (whole + (1.0 - eta) / (1.0 + eta) + b / (1.0 - b)));
    for (int it = 0; it < 2; ++it) {
        const double f = std::log(g * (eta * p + 1.0) / (p + eta) * a * p / (p - b)) - whole * std::log(p);
        const double df = eta / (eta * p + 1.0) - 1.0 / (p + eta) + 1.0 / p - 1.0 / (p - b) - whole / p;
        p -= f / df;
    }
    const float dcTurn = static_cast<float>(1.0 / p);   // p^-1: the weights' step a sample

    // The burst: white noise through two one-poles at Tone (12 dB an octave, as the modes' Tone),
    // 2 ms or a period if that is longer (a shorter burst would leave the loop silent between its
    // passes, a pulse train), under a Hann window, its mean taken out with the DC mode's weights
    // (above). The window, sin^2 (pi (i + 1/2) / len) = (1 - cos) / 2, comes from a phasor turned a
    // step a sample.
    const int len = std::min(std::max(kBurst, static_cast<int>(std::ceil(period))), kBurstMax);
    const int fold = std::min(std::max(static_cast<int>(std::lround(period)), 1), len);
    v.burstLen = len;
    v.w = 0;
    v.P = v.pan;
    const float tone = static_cast<float>(1.0 - std::exp(-kTwoPi * s.toneHz / kRateD));
    const double turnBy = kTwoPi / len;
    const float tc = static_cast<float>(std::cos(turnBy)), ts = static_cast<float>(std::sin(turnBy));
    const float c0 = static_cast<float>(std::cos(0.5 * turnBy)), s0 = static_cast<float>(std::sin(0.5 * turnBy));

    if (len > kBurst) {
        // A long burst (a period over 2 ms: note 71 and under) is built as it plays (pluckBurst()).
        // Its mean is needed from its first sample, so the strike draws the whole noise once for it
        // (the statistics alone, nothing stored), and the samples are drawn again, the same, as they
        // go in. Built whole at the strike, the 1349 samples of note 24 took 72k instructions in its
        // block, 52 a sample; this pass takes 21 a sample, and drawing them as they play costs 15 a
        // sample more than the ring.
        //
        // Its level doesn't wait for the noise's fold: the burst goes in at the sound's scale over the
        // RMS the windowed noise has on average when folded onto a period, not the one it turns out to
        // have. The noise's square averages 1/3 and the window's fourth power 3/8, and a burst a
        // period long barely folds over (len and fold are equal or one apart), so that RMS is
        // sqrt(len / (8 fold)). The noise's own luck, which the measured RMS took away, now moves a
        // strike's level by about 5.4 / sqrt(len) dB (one standard deviation): 0.15 dB at note 24,
        // 0.3 at 48, 0.55 at 71, on top of the spread the filtered noise has anyway (0.1 to 0.9 dB
        // at Tone 16000, more at a darker Tone). Accepted to keep the strike's pass lean: the measured
        // RMS needs the noise folded onto a period, a load and a store a sample and a pass more.
        //
        // The weights ride on the window's phasor: started at half and turned by p^-1 as well, it
        // gives the weighted window, h - c with h = p^-i / 2, as it turns (2.7k fewer at note 24).
        // Two samples a pass let the state go from one set of registers to the other and back,
        // where one a pass copied three of them every sample (2.7k fewer again; the draw as the burst
        // plays gains nothing from it).
        Noise& z = v.noise;
        z.rng = rng_;
        uint32_t rng = rng_;
        float lp1 = 0.0f, lp2 = 0.0f, x, sum = 0.0f, wsum = 0.0f, pc = 0.5f * c0, ps = 0.5f * s0, h = 0.5f;
        const float tcq = tc * dcTurn, tsq = ts * dcTurn;
#pragma GCC unroll 2
        for (int i = 0; i < len; ++i) {
            const float u = burstNoise(rng, lp1, lp2, tone, x);
            const float wq = h - pc;
            turnPhasor(pc, ps, tcq, tsq);
            h *= dcTurn;
            sum += u * wq;
            wsum += wq;
        }
        rng_ = rng;
        z.lp1 = 0.0f;
        z.lp2 = 0.0f;
        z.c = c0;
        z.s = s0;
        z.tc = tc;
        z.ts = ts;
        z.tone = tone;
        z.mean = sum / wsum;
        z.scale = static_cast<float>(kScale[AS_KALIMBA] * std::sqrt(8.0 * fold / len));
        z.peak = 0.0f;
        z.made = 0;
        v.firstPass = whole;   // under len: whole <= period - 0.5
        // Until the burst has all gone in, its level is the most it can play (the noise and its
        // one-poles are within +-1, the window within 0..1), so a note still being struck isn't the
        // quietest. Then it is the burst's peak held from the strike, as a short burst's is.
        v.most = (1.0f + std::fabs(z.mean)) * z.scale;
        v.env = 0.0f;
        v.level = v.most * v.vel;
        return;
    }

    // A short burst (2 ms, note 72 and up) is built at the strike: kBurst samples, the strike 6.4k
    // to 6.6k instructions with the tuning. Its level: what rings is the burst folded onto one period
    // (over note 84 it makes more than two passes, and gathers in the loop as it goes in), so the raw
    // windowed noise folded so is what sets it: the burst goes in at the sound's scale over that
    // fold's RMS. Every strike then rings about as loud at every pitch, where the noise's own luck
    // put up to 8 dB between two strikes at the top of the keyboard; a darker Tone takes its share
    // away, as it does the modes'. The line holds the fold meanwhile (the loop writes it over before
    // reading it).
    v.firstPass = 0;
    v.noise.made = len;
    std::fill(v.line, v.line + fold, 0.0f);
    uint32_t rng = rng_;
    float lp1 = 0.0f, lp2 = 0.0f, x, sum = 0.0f, wsum = 0.0f, pc = c0, ps = s0, q = 1.0f;
    for (int i = 0, j = 0; i < len; ++i) {
        const float u = burstNoise(rng, lp1, lp2, tone, x);
        const float win = 0.5f - 0.5f * pc;
        turnPhasor(pc, ps, tc, ts);
        v.burst[i] = u * win;
        sum += v.burst[i] * q;
        wsum += win * q;
        q *= dcTurn;
        v.line[j] += x * win;
        j = j + 1 == fold ? 0 : j + 1;
    }
    rng_ = rng;
    float energy = 0.0f;
    for (int j = 0; j < fold; ++j) energy += v.line[j] * v.line[j];
    const float rms = std::sqrt(energy / static_cast<float>(fold));
    const float mean = sum / wsum, scale = rms > 0.0f ? kScale[AS_KALIMBA] / rms : 0.0f;
    pc = c0;
    ps = s0;
    for (int i = 0; i < len; ++i) {
        const float win = 0.5f - 0.5f * pc;
        turnPhasor(pc, ps, tc, ts);
        v.burst[i] = (v.burst[i] - mean * win) * scale;
    }
    std::fill(v.burst + len, v.burst + len + kChunk, 0.0f);   // a step running past the end reads zeros
    // The line: only what the loop reads before writing it needs clearing.
    for (int k = kLine - whole; k < kLine; ++k) v.line[k] = 0.0f;
    v.env = peakOf(v.burst, len, 0.0f);
    v.level = v.env * v.vel;
}

// m samples of a long burst into out, as the strike's pass for the mean drew them, the mean out and at
// its scale. Returns the sum of their squares; the burst's peak so far in z.peak.
float AirVoices::drawBurst(Noise& z, float* out, int m) {
    uint32_t rng = z.rng;
    const float tone = z.tone, tc = z.tc, ts = z.ts, mean = z.mean, scale = z.scale;
    float lp1 = z.lp1, lp2 = z.lp2, x, pc = z.c, ps = z.s, e = 0.0f;
    for (int i = 0; i < m; ++i) {
        const float u = burstNoise(rng, lp1, lp2, tone, x);
        const float win = 0.5f - 0.5f * pc;
        turnPhasor(pc, ps, tc, ts);
        const float y = (u * win - mean * win) * scale;
        out[i] = y;
        e += y * y;
    }
    z.rng = rng;
    z.lp1 = lp1;
    z.lp2 = lp2;
    z.c = pc;
    z.s = ps;
    z.peak = peakOf(out, m, z.peak);
    z.made += m;
    return e;
}

void AirVoices::beginFelt(Voice& v, const Strike& s, double hz) {
    v.osc.reset(0.0f);
    v.table = nullptr;
    v.frame = -1;
    v.framePos = 0.0f;
    v.moved = 0;
    v.inc = static_cast<float>(hz / kRateD);
    const float samples = s.decayS * kRate;
    v.ageStep = kFeltAgeSpan / samples;
    v.fallLog2 = -kLog2Thousand / samples;
    v.pan = v.pan * splat2(kScale[AS_FELT]);
    v.P = splat2(0.0f);   // the attack rises from nothing
    v.level = kScale[AS_FELT] * kFeltPeak * v.vel;
}

void AirVoices::render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR,
                       float spaceSend, int n) {
    for (int o = 0; o < n; o += kMaxBlock)
        renderBlock(tables, outL + o, outR + o, sendL + o, sendR + o, spaceSend, std::min(kMaxBlock, n - o));
}

void AirVoices::renderBlock(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR,
                            float spaceSend, int n) {
    if (n <= 0) return;
    spaceSend = clampParam(spaceSend, 0.0f, 1.0f, 0.0f);
    if (send_ < 0.0f) send_ = spaceSend;
    if (active() > 0) {
        renderBus(tables, 0, n);
        mixOut(bus_, outL, outR, sendL, sendR, send_, (spaceSend - send_) / static_cast<float>(n), n);
    }
    send_ = spaceSend;
}

bool AirVoices::renderBus(const TableSet& tables, int from, int n) {
    if (n <= 0) return false;
    clearBus(bus_ + from, n);
    if (active() == 0) return false;
    const Wavetable& felt = tables.get(TB_FELT_PIANO);
    for (Voice& v : v_) {
        for (int o = from; o < from + n && v.stage != VS_FREE;) {
            int m = std::min(kChunk, from + n - o);
            if (v.stage == VS_STEAL) m = std::min(m, v.stealLeft);
            if (v.age < v.firstPass) m = std::min(m, v.firstPass - v.age);   // a long burst's first pass ends a step
            voiceStep(v, felt, o, m);
            o += m;
        }
    }
    return true;
}

void AirVoices::voiceStep(Voice& v, const Wavetable& felt, int o, int m) {
    const int end = v.age + m;   // v.age <= kAgeMost, m <= kChunk: no overflow
    const float before = v.level;   // as the step starts: with its end's, the most the step played
    // The gain at the step's end: a steal's fade; Felt's envelope (its 5 ms raised-cosine attack,
    // and the fall to -60 dB at decayS). The modes and the pluck carry their own decay.
    float gain = v.stage == VS_STEAL ? static_cast<float>(v.stealLeft - m) * (1.0f / kStealSamples) : 1.0f;
    float fall = 1.0f;
    if (v.now.sound == AS_FELT) {
        const float ts = static_cast<float>(end);
        fall = exp2Fast(v.fallLog2 * ts);
        gain *= fall;
        if (end < kFeltRise) {
            const float r = sinCycle(ts * (0.25f / kFeltRise));
            gain *= r * r;
        }
    }
    const f2 P1 = v.pan * splat2(gain);
    const f2 dP = (P1 - v.P) * splat2(1.0f / static_cast<float>(m));
    f2* bus = bus_ + o;
    const int sound = v.now.sound;
    if (sound == AS_KALIMBA) {
        if (v.age < v.burstLen) {
            pluckBurst(v, dP, bus, m);
        } else {
            Pluck k{v.line, v.w, v.delay, kLineMask, v.eta, v.ap, v.lpA, v.lp, v.loopG};
            const float e = pluckLoop<false>(k, nullptr, v.P, dP, bus, m);
            v.w = k.w;
            v.ap = k.ap;
            v.lp = k.lp;
            // The level held at the slowest the loop can fall, and pushed up by what this step played
            // (its RMS as a sine's peak).
            v.env = std::max(v.env * exp2Fast(v.holdLog2 * static_cast<float>(m)),
                             std::sqrt(2.0f * e / static_cast<float>(m)));
            v.level = v.env * v.vel;
        }
    } else if (sound == AS_FELT) {
        if (!v.table) v.table = &felt;
        const Wavetable& t = *v.table;
        // Age on whole frames: the read stays on one frame (twenty ARM instructions a sample, where
        // a pair of frames takes 37), and moves on across one step, the oscillator's glide
        // crossfading the frames (a step that crosses frames reads them sample by sample, 52). It
        // moves at most every kFeltMove samples: at Decay 4 s Age reaches the next frame every 29
        // steps anyway, at 0.5 s every 3.6 steps, where it now moves four frames every 16. The
        // frame's position is worked out in double and rounded once: times the frames, it comes
        // back to the frame exactly in float (every one of 256), as the single-frame read needs.
        v.moved = std::min(v.moved + m, kFeltMove);
        if (t.frames > 1 && (v.moved == kFeltMove || v.frame < 0)) {
            const float frames1 = static_cast<float>(t.frames - 1);
            const float age = kFeltAge0 + std::min(kFeltAgeSpan, v.ageStep * static_cast<float>(end));
            const int frame = static_cast<int>(age * frames1 + 0.5f);
            if (frame != v.frame) {
                v.frame = frame;
                v.framePos = static_cast<float>(static_cast<double>(frame) / (t.frames - 1));
                v.moved = 0;
            }
        }
        v.osc.render(t, v.inc, v.framePos, read_, m);
        mixMono(read_, v.P, dP, bus, m);
        v.level = kScale[AS_FELT] * kFeltPeak * v.vel * fall;
    } else {
        if (v.age < kRiseSamples) modalLoop<true>(v.zr, v.zi, v.cr, v.ci, v.P, dP, kRise.g + v.age, bus, m);
        else modalLoop<false>(v.zr, v.zi, v.cr, v.ci, v.P, dP, nullptr, bus, m);
        // Each mode's |z| is its amplitude: their sum is the most the voice can play.
        const f4 mag = sqrt4(v.zr[0] * v.zr[0] + v.zi[0] * v.zi[0]) + sqrt4(v.zr[1] * v.zr[1] + v.zi[1] * v.zi[1]);
        v.level = sum4(mag)[0] * v.vel;
    }
    v.P = P1;
    v.age = std::min(end, kAgeMost);

    if (v.stage == VS_STEAL) {
        v.stealLeft -= m;
        if (v.stealLeft <= 0) begin(v, v.next);
        return;
    }
    // Quiet only if the whole step was: a level falls across a step (at Decay 0.1 a fundamental by
    // 0.4 dB, Bar's top mode by 4 dB), so the step's end alone let a voice go free having played up
    // to -89.6 dBFS.
    v.quiet = std::max(before, v.level) * v.panMax < kFloor ? v.quiet + m : 0;
    if (v.quiet >= kQuietSamples) {
        v.stage = VS_FREE;
        v.P = splat2(0.0f);
        v.level = 0.0f;
        v.table = nullptr;
    }
}

// A step of the pluck while its burst goes in (after, voiceStep() runs the loop alone).
void AirVoices::pluckBurst(Voice& v, f2 dP, f2* bus, int m) {
    float e;
    if (v.age < v.firstPass) {
        // A long burst's first pass: the loop has nothing to read back yet, so what goes in is all it
        // plays, and it goes straight into the line, where it starts (renderBlock() ends a step where
        // the first pass does; no wrap, the first pass being under a period).
        float* const y = v.line + v.w;
        e = drawBurst(v.noise, y, m);
        mixMono(y, v.P, dP, bus, m);
        v.w += m;
    } else {
        // The burst's samples from here: a short one's from its start, built at the strike; a long
        // one's drawn now, then zeros past its end.
        const float* burst = v.burst;
        if (v.firstPass > 0) {
            const int left = std::min(v.burstLen - v.age, m);
            drawBurst(v.noise, v.burst, left);
            std::fill(v.burst + left, v.burst + m, 0.0f);
        } else {
            burst += v.age;   // under kBurst
        }
        Pluck k{v.line, v.w, v.delay, kLineMask, v.eta, v.ap, v.lpA, v.lp, v.loopG};
        e = pluckLoop<true>(k, burst, v.P, dP, bus, m);
        v.w = k.w;
        v.ap = k.ap;
        v.lp = k.lp;
    }
    // The level as voiceStep() holds it. A short burst's peak is held from the strike; a long one's
    // from the step it has all gone in, at what it would be had it been held from the strike. Until
    // then its level is the most it can play.
    const int end = v.age + m;
    v.env = std::max(v.env * exp2Fast(v.holdLog2 * static_cast<float>(m)), std::sqrt(2.0f * e / static_cast<float>(m)));
    if (v.firstPass > 0) {
        if (end < v.burstLen) {
            v.level = std::max(v.env, v.most) * v.vel;
            return;
        }
        v.env = std::max(v.env, v.noise.peak * exp2Fast(v.holdLog2 * static_cast<float>(end)));
    }
    v.level = v.env * v.vel;
}

AirVoices::VoiceView AirVoices::voice(int i) const {
    VoiceView w;
    if (i < 0 || i >= kVoices) return w;
    const Voice& v = v_[i];
    w.stage = v.stage;
    if (v.stage == VS_FREE) return w;
    w.note = v.now.note;
    w.next = v.stage == VS_STEAL ? v.next.note : -1;
    w.sound = v.now.sound;
    w.level = v.level;
    if (v.now.sound == AS_KALIMBA) {
        w.burst = v.noise.made;
        w.burstLen = v.burstLen;
        w.loopDelay = v.delay;
        w.loopEta = v.eta;
        w.loopA = v.lpA;
        w.loopG = v.loopG;
    }
    return w;
}

} // namespace af
