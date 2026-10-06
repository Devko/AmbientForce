// The lifetime oscillator (dsp/lifeosc.h): pitch, aliasing, the position glide and the mip level
// crossfade, Sway and Smear, the Couple modes, determinism and the phase over a long run.
// Needs no plugin: make test-module M=lifeosc runs it on its own.
#include "check.h"
#include "signal.h"
#include "../dsp/lifeosc.h"
#include "../dsp/lifetime.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <vector>

namespace aft {
namespace {

using cd = std::complex<double>;
constexpr int kOscBlock = 128;   // MPC's block
constexpr float kStepS = static_cast<float>(af::kChunk) / af::kRate;

// The tables the checks play, each built once per run.
const af::Wavetable& table(int id) {
    static af::Wavetable t[af::TB_COUNT];
    static bool built[af::TB_COUNT] = {};
    if (!built[id]) {
        CHECK(af::buildTable(id, t[id]));
        built[id] = true;
    }
    return t[id];
}

// n samples of one oscillator (either read) at a fixed pitch and position, in blocks of `block`.
template <class Osc>
Buf play(Osc& o, const af::Wavetable& t, float inc, float pos, int n, int block = kOscBlock) {
    Buf x(static_cast<size_t>(n));
    for (int i = 0; i < n; i += block) o.render(t, inc, pos, &x[static_cast<size_t>(i)], std::min(block, n - i));
    return x;
}

// The test's own FFT (radix-2, forward).
void fft(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    std::vector<cd> w(n / 2);
    for (size_t k = 0; k < n / 2; ++k) w[k] = std::polar(1.0, -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n));
    for (size_t len = 2; len <= n; len <<= 1)
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w[k * (n / len)];
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
}

// A spectrum as each bin's sine amplitude (a sine of peak A on a bin reads A). Blackman-Harris:
// its sidelobes are under -92 dB, so a strong partial's leakage can't fill a -90 dB measurement.
struct Spectrum {
    std::vector<double> amp;
    double binHz = 0.0;
    explicit Spectrum(const Buf& x) {   // x.size(): a power of two
        const size_t n = x.size();
        std::vector<cd> a(n);
        double wsum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double p = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
            const double w = 0.35875 - 0.48829 * std::cos(p) + 0.14128 * std::cos(2.0 * p) - 0.01168 * std::cos(3.0 * p);
            a[i] = x[i] * w;
            wsum += w;
        }
        fft(a);
        amp.resize(n / 2);
        for (size_t k = 0; k < n / 2; ++k) amp[k] = 2.0 * std::abs(a[k]) / wsum;
        binHz = static_cast<double>(af::kRate) / static_cast<double>(n);
    }
    // The strongest bin within `bins` of hz: a partial's amplitude (up to 0.8 dB low between bins).
    double at(double hz, int bins = 3) const {
        const long c = std::lround(hz / binHz);
        double m = 0.0;
        for (long k = std::max(1L, c - bins); k <= std::min(static_cast<long>(amp.size()) - 1, c + bins); ++k)
            m = std::max(m, amp[static_cast<size_t>(k)]);
        return m;
    }
};

// Frequency by rising zero crossings (interpolated), over x[from..to).
double zeroCrossHz(const Buf& x, size_t from, size_t to) {
    double first = -1.0, last = 0.0;
    int n = 0;
    for (size_t i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
            if (first < 0.0) first = t;
            else {
                last = t;
                ++n;
            }
        }
    return n > 0 ? static_cast<double>(af::kRate) * n / (last - first) : 0.0;
}

// Check 1: the sine table plays a clean 440 Hz at full scale.
void testPitch() {
    std::printf("== lifeosc: pitch\n");
    af::TableOsc o;
    o.reset();
    const float inc = 440.0f / af::kRate;
    const Buf x = play(o, table(af::TB_SINE), inc, 0.0f, 1 << 16);
    const Spectrum s(x);
    size_t top = 1;
    for (size_t k = 1; k < s.amp.size(); ++k)
        if (s.amp[k] > s.amp[top]) top = k;
    CHECK(std::fabs(static_cast<double>(top) * s.binHz - 440.0) <= s.binHz);
    CHECK(std::fabs(magnitude(x, 440.0) - 1.0) < 1e-3);   // a full-scale sine: the frames are read as they are
    double worst = -400.0;
    for (int h = 2; h * 440.0 < 20000.0; ++h) worst = std::max(worst, db(s.at(h * 440.0) / s.at(440.0)));
    std::printf("  440 Hz: harmonics at most %.0f dB\n", worst);
    CHECK(worst < -90.0);
    CHECK(allFinite(x) && peak(x) <= 1.0001f);
}

// Check 2: the saw from C1 to C8 folds nothing back below 15 kHz louder than -70 dB under its
// fundamental. Everything that isn't a harmonic of the note counts: a harmonic of the table folded
// back (the mip choice) and the interpolation's own images (the read). The linear read (B's) leaves
// its images at -64 dB (level 5, note 72): it is held to -60.
template <class Osc>
void aliasSweep(const char* name, double limit) {
    double worstAll = -400.0;
    int worstNote = 0;
    for (int note = 24; note <= 108; note += 12) {
        Osc o;
        o.reset();
        const float inc = af::noteHz(static_cast<float>(note)) / af::kRate;
        const Buf x = play(o, table(af::TB_SAW), inc, 0.0f, 1 << 16);
        const Spectrum s(x);
        const double f0 = static_cast<double>(inc) * af::kRate;
        double worst = 0.0;
        for (size_t k = 1; k < s.amp.size() && static_cast<double>(k) * s.binHz < 15000.0; ++k) {
            const double hz = static_cast<double>(k) * s.binHz;
            if (std::fabs(hz - std::round(hz / f0) * f0) < 8.0 * s.binHz) continue;   // a true harmonic's main lobe
            worst = std::max(worst, s.amp[k]);
        }
        const double rel = db(worst / s.at(f0));
        if (rel > worstAll) {
            worstAll = rel;
            worstNote = note;
        }
        CHECK(rel < limit);
    }
    std::printf("  %s: worst %.1f dB (note %d)\n", name, worstAll, worstNote);
}

void testAliasing() {
    std::printf("== lifeosc: no aliasing (saw, notes 24..108)\n");
    aliasSweep<af::TableOsc>("Hermite", -70.0);
    aliasSweep<af::TableOscLinear>("linear", -60.0);
}

// Check 3: a jump of the position from the first frame to the last within one block glides across
// it: no sample-to-sample step past 1.5x the steady state's on either side. A spliced jump (the
// same phase, the position switched at a block edge) does step, so the check has teeth.
void testGlide() {
    std::printf("== lifeosc: position glide\n");
    const af::Wavetable& t = table(af::TB_SINE_BLOOM);
    const float inc = 220.0f / af::kRate;
    auto at = [](int block) { return static_cast<size_t>(block * kOscBlock); };   // a block's first sample
    af::TableOsc o;
    o.reset(0.1f);
    Buf x(at(17));
    for (int k = 0; k < 17; ++k) o.render(t, inc, k < 8 ? 0.0f : 1.0f, &x[at(k)], kOscBlock);
    const float before = maxStep(x, at(1), at(8)), after = maxStep(x, at(10), at(17));
    const float glide = maxStep(x, at(8) - 1, at(9) + 2);
    const float steady = std::max(before, after);

    af::TableOsc a, b;
    a.reset(0.1f);
    b.reset(0.1f);
    const Buf x0 = play(a, t, inc, 0.0f, 17 * kOscBlock), x1 = play(b, t, inc, 1.0f, 17 * kOscBlock);
    float jump = 0.0f;
    for (int k = 1; k < 17; ++k) jump = std::max(jump, std::fabs(x1[at(k)] - x0[at(k) - 1]));
    std::printf("  steady %.4f / %.4f, glide %.4f, spliced jump %.4f\n", before, after, glide, jump);
    CHECK(glide <= 1.5f * steady);
    CHECK(jump > 1.5f * steady);
    CHECK(allFinite(x));

    // A slow move (a sway's) from block to block: no step at the block edges either.
    af::TableOsc s;
    s.reset(0.3f);
    Buf y(at(8));
    for (int k = 0; k < 8; ++k) s.render(t, inc, 0.40f + 0.003f * static_cast<float>(k), &y[at(k)], kOscBlock);
    float inside = 0.0f, edges = 0.0f;
    for (int k = 0; k < 8; ++k) {
        inside = std::max(inside, maxStep(y, at(k) + 1, at(k + 1)));
        if (k > 0) edges = std::max(edges, std::fabs(y[at(k)] - y[at(k) - 1]));
    }
    CHECK(edges <= 1.05f * inside);

    // The glide is a straight line that ends on the new position: the block's last sample is
    // what the position itself plays (an oscillator that was there all along, at the same phase).
    af::TableOsc r, q;
    r.reset(0.3f);
    q.reset(0.3f);
    Buf z(at(3)), zq(at(3));
    for (int k = 0; k < 3; ++k) {
        r.render(t, inc, k < 2 ? 0.40f : 0.43f, &z[at(k)], kOscBlock);
        q.render(t, inc, 0.43f, &zq[at(k)], kOscBlock);
    }
    CHECK(std::fabs(z[at(3) - 1] - zq[at(3) - 1]) < 1e-5f);
    float apart = 0.0f;   // ...and where it came from was elsewhere
    for (size_t i = at(1); i < at(2); ++i) apart = std::max(apart, std::fabs(z[i] - zq[i]));
    CHECK(apart > 1e-2f);
}

// The level crossfade: when the pitch crosses into the next mip level between two renders, the
// render that changes runs from the old level to the new one, sample by sample.
void testMipCrossfade() {
    std::printf("== lifeosc: mip level crossfade\n");
    const af::Wavetable& t = table(af::TB_SAW);
    const float edge = af::kAliasLimit / 64.0f;   // above it, level 5 (32 harmonics) instead of level 4 (64)
    const float lo = edge * (1.0f - 1e-6f), hi = edge * (1.0f + 1e-6f);
    CHECK(af::mipFor(lo) == 4 && af::mipFor(hi) == 5);
    constexpr int n = af::kChunk;
    af::TableOsc x, ra, rb;
    x.reset();
    ra.reset();
    rb.reset();
    Buf bx(2 * n), ba(2 * n), bb(2 * n);
    x.render(t, lo, 0.0f, &bx[0], n);
    x.render(t, hi, 0.0f, &bx[n], n);
    ra.render(t, lo, 0.0f, &ba[0], n);   // stays on level 4
    ra.render(t, lo, 0.0f, &ba[n], n);
    rb.render(t, hi, 0.0f, &bb[0], n);   // on level 5 throughout
    rb.render(t, hi, 0.0f, &bb[n], n);
    float err = 0.0f, apart = 0.0f;
    for (int i = 0; i < n; ++i) {
        const size_t k = static_cast<size_t>(n + i);
        const float w = static_cast<float>(i + 1) / n;
        err = std::max(err, std::fabs(bx[k] - (ba[k] + w * (bb[k] - ba[k]))));
        apart = std::max(apart, std::fabs(bb[k] - ba[k]));
    }
    std::printf("  levels 4 and 5 differ by up to %.3f; the crossfade within %.1e\n", apart, err);
    CHECK(apart > 0.01f);
    CHECK(err < 1e-3f);   // what's left: the references' pitches, 2e-6 apart (a step would leave up to `apart`)
}

// Check 4: Sway at depth 1 moves Age 0.5 over 0.25..0.75 and reaches both ends; at the top end
// of the table it reflects, so it keeps moving instead of sitting on the last frame.
void testSway() {
    std::printf("== lifeosc: sway\n");
    af::LifeScan s;
    s.seed(1);
    const af::LifePos p{0.5f, 1.0f, 0.1f, 0.0f};
    float lo = 1.0f, hi = 0.0f, prev = -1.0f, jump = 0.0f;
    const int steps = static_cast<int>(10.0f / kStepS);
    for (int i = 0; i < steps; ++i) {
        const float v = s.step(p, kStepS);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        if (prev >= 0.0f) jump = std::max(jump, std::fabs(v - prev));
        prev = v;
    }
    std::printf("  age 0.5: %.4f..%.4f, largest step %.1e\n", lo, hi, jump);
    CHECK(lo >= 0.25f - 1e-4f && hi <= 0.75f + 1e-4f);
    CHECK(lo <= 0.25f + 0.01f && hi >= 0.75f - 0.01f);   // both ends within 2% of the range
    CHECK(jump < 2e-4f);                                  // 0.25 x 2 pi x 0.1 Hz x 0.73 ms

    // Age 0.95: 0.70..1.2, the part past the end folded back.
    af::LifeScan e;
    e.seed(2);
    const af::LifePos q{0.95f, 1.0f, 0.5f, 0.0f};
    float elo = 1.0f, ehi = 0.0f;
    int atEnd = 0;
    for (int i = 0; i < steps; ++i) {
        const float v = e.step(q, kStepS);
        elo = std::min(elo, v);
        ehi = std::max(ehi, v);
        atEnd += v >= 0.999f ? 1 : 0;
    }
    std::printf("  age 0.95: %.4f..%.4f, %.1f%% of the time on the last frame\n", elo, ehi, 100.0 * atEnd / steps);
    CHECK(elo >= 0.70f - 1e-4f && elo <= 0.71f && ehi <= 1.0f && ehi >= 0.999f);
    CHECK(atEnd < steps / 20);   // a clamp would hold it there 44% of the time

    // reset(phase) sets the sway's phase: two scans from different seeds, reset alike, agree.
    af::LifeScan a, b;
    a.seed(3);
    b.seed(4);
    a.reset(0.25f);
    b.reset(0.25f);
    bool same = true;
    for (int i = 0; i < 1000; ++i) same = same && a.step(p, kStepS) == b.step(p, kStepS);
    CHECK(same);
    a.reset(0.25f);
    CHECK(std::fabs(a.step(af::LifePos{0.5f, 1.0f, 0.0f, 0.0f}, 0.0f) - 0.75f) < 1e-4f);   // a quarter cycle in: the top
}

// Check 5: Smear at 1 wanders within Age +- 0.03, smoothly, changing direction every 50-200 ms
// or so, and the same seed walks the same way.
void testSmear() {
    std::printf("== lifeosc: smear\n");
    const af::LifePos p{0.5f, 0.0f, 0.05f, 1.0f};
    const int steps = static_cast<int>(10.0f / kStepS);
    auto walk = [&](uint32_t seed) {
        af::LifeScan s;
        s.seed(seed);
        std::vector<float> v(static_cast<size_t>(steps));
        for (float& x : v) x = s.step(p, kStepS);
        return v;
    };
    const std::vector<float> v = walk(5);
    float lo = 1.0f, hi = 0.0f, jump = 0.0f;
    int turns = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        lo = std::min(lo, v[i]);
        hi = std::max(hi, v[i]);
        if (i > 0) jump = std::max(jump, std::fabs(v[i] - v[i - 1]));
        if (i > 1 && (v[i] - v[i - 1]) * (v[i - 1] - v[i - 2]) < 0.0f) ++turns;
    }
    std::printf("  %.4f..%.4f, largest step %.1e, %d turns in 10 s\n", lo, hi, jump, turns);
    CHECK(lo >= 0.47f - 1e-6f && hi <= 0.53f + 1e-6f);
    CHECK(hi - lo > 0.03f);    // it moves, over most of its range
    CHECK(jump < 2e-3f);       // never a step (a frame is 0.0039)
    CHECK(turns >= 25 && turns <= 200);
    CHECK(walk(5) == v);       // deterministic
    CHECK(walk(6) != v);
}

// Checks 6 and 7, and Mix and AM: the Couple modes through renderCoupled, one control step at a
// time (A Hermite, B linear).
Buf coupled(int mode, float amt, float blend, int tableA, double hzA, int tableB, double hzB, int n = 1 << 16) {
    af::TableOsc a;
    af::TableOscLinear b;
    a.reset();
    b.reset();
    Buf x(static_cast<size_t>(n));
    float scratch[2 * af::kChunk];
    for (int i = 0; i < n; i += af::kChunk)
        af::renderCoupled(a, table(tableA), static_cast<float>(hzA) / af::kRate, b, table(tableB),
                          static_cast<float>(hzB) / af::kRate, 0.5f, mode, amt, blend, &x[static_cast<size_t>(i)], scratch,
                          std::min(af::kChunk, n - i));
    return x;
}

void testCouple() {
    std::printf("== lifeosc: couple\n");
    // Amplitudes by magnitude() (signal.h): Hann-windowed at the exact frequency, so a partial
    // between two FFT bins reads true.
    {   // FM, both sines at f, amt 0.5: B moves A's phase by up to 0.25 cycles (pi / 2), so the
        // partials are Bessel's: 2f at J1 + J3 = 0.636 and 3f at J2 - J4 = 0.236, f at J0 - J2 = 0.222.
        const double f = 441.0;
        const Buf x = coupled(af::CP_FM, 0.5f, 0.0f, af::TB_SINE, f, af::TB_SINE, f);
        const double a1 = magnitude(x, f), a2 = magnitude(x, 2 * f), a3 = magnitude(x, 3 * f);
        std::printf("  FM: f %.3f, 2f %.3f (%+.0f dB), 3f %.3f (%+.0f dB)\n", a1, a2, db(a2 / a1), a3, db(a3 / a1));
        CHECK(db(a2 / a1) > -40.0 && db(a3 / a1) > -40.0);
        CHECK(std::fabs(a2 - 0.636) < 0.005 && std::fabs(a3 - 0.236) < 0.005 && std::fabs(a1 - 0.222) < 0.005);
        const Buf z = coupled(af::CP_FM, 0.0f, 0.0f, af::TB_SINE, f, af::TB_SINE, f);
        CHECK(db(magnitude(z, 2 * f) / magnitude(z, f)) < -90.0);   // no depth, no FM
    }
    const double f1 = 1000.0, f2 = 300.0;
    {   // Ring, amt 1: A times B, the sum and the difference, no carrier.
        const Buf x = coupled(af::CP_RING, 1.0f, 0.0f, af::TB_SINE, f1, af::TB_SINE, f2);
        std::printf("  Ring: f1-f2 %.3f, f1+f2 %.3f, f1 %.0f dB\n", magnitude(x, f1 - f2), magnitude(x, f1 + f2),
                    db(magnitude(x, f1)));
        CHECK(std::fabs(magnitude(x, f1 - f2) - 0.5) < 0.005 && std::fabs(magnitude(x, f1 + f2) - 0.5) < 0.005);
        CHECK(db(magnitude(x, f1)) < -40.0 && db(magnitude(x, f2)) < -40.0);
    }
    {   // AM, amt 1: A times (0.5 + 0.5 B): the carrier at half, the sidebands at a quarter.
        const Buf x = coupled(af::CP_AM, 1.0f, 0.0f, af::TB_SINE, f1, af::TB_SINE, f2);
        CHECK(std::fabs(magnitude(x, f1) - 0.5) < 0.005);
        CHECK(std::fabs(magnitude(x, f1 - f2) - 0.25) < 0.005 && std::fabs(magnitude(x, f1 + f2) - 0.25) < 0.005);
    }
    {   // Mix: the blend alone (amt does nothing); blend 1 is B alone.
        const Buf x = coupled(af::CP_MIX, 1.0f, 0.5f, af::TB_SINE, f1, af::TB_SINE, f2);
        CHECK(std::fabs(magnitude(x, f1) - 0.5) < 0.005 && std::fabs(magnitude(x, f2) - 0.5) < 0.005);
        CHECK(db(magnitude(x, f1 + f2)) < -90.0);
        const Buf b1 = coupled(af::CP_RING, 1.0f, 1.0f, af::TB_SAW, f1, af::TB_SQUARE, f2, 4096);
        af::TableOscLinear o;
        o.reset();
        CHECK(b1 == play(o, table(af::TB_SQUARE), static_cast<float>(f2) / af::kRate, 0.5f, 4096, af::kChunk));
    }
    {   // Mix at blend 0 doesn't render B: the same output, sample for sample, as rendering B and
        // mixing none of it in. (The pitches are worked out once for both: the device build may
        // divide by the rate in one place and multiply by its reciprocal in another, an ulp apart.)
        const int n = 4096;
        const float incA = static_cast<float>(f1) / af::kRate, incB = static_cast<float>(f2) / af::kRate;
        const af::Wavetable &ta = table(af::TB_SINE_BLOOM), &tb = table(af::TB_SAW);
        af::TableOsc a, as;
        af::TableOscLinear b, bs;
        a.reset();
        b.reset();
        as.reset();
        bs.reset();
        Buf skipped(static_cast<size_t>(n)), full(static_cast<size_t>(n));
        float scratch[2 * af::kChunk], bb[af::kChunk];
        for (int i = 0; i < n; i += af::kChunk) {
            af::renderCoupled(as, ta, incA, bs, tb, incB, 0.5f, af::CP_MIX, 0.7f, 0.0f, &skipped[static_cast<size_t>(i)],
                              scratch, af::kChunk);
            float* o = &full[static_cast<size_t>(i)];
            b.render(tb, incB, 0.5f, bb, af::kChunk);
            a.render(ta, incA, 0.5f, o, af::kChunk);
            af::couple(af::CP_MIX, 0.7f, 0.0f, o, bb, o, af::kChunk);
        }
        CHECK(skipped == full);
        CHECK(peak(skipped) > 0.5f);
    }
}

// The same seed, the same sound: a LifeScan moving a TableOsc over a lifetime table, twice.
void testDeterminism() {
    std::printf("== lifeosc: determinism\n");
    auto voice = [](uint32_t seed) {
        af::LifeScan s;
        s.seed(seed);
        af::TableOsc o;
        o.reset();
        const af::LifePos p{0.6f, 0.7f, 1.5f, 1.0f};
        Buf x(44100);
        for (size_t i = 0; i < x.size(); i += af::kChunk) {
            const int n = static_cast<int>(std::min<size_t>(af::kChunk, x.size() - i));
            o.render(table(af::TB_SINE_BLOOM), 196.0f / af::kRate, s.step(p, kStepS), &x[i], n);
        }
        return x;
    };
    const Buf a = voice(11);
    CHECK(a == voice(11));
    CHECK(a != voice(12));
    CHECK(allFinite(a) && peak(a) < 2.0f);
}

// The phase is a 32-bit fixed-point fraction of a cycle: it wraps by integer overflow, exactly,
// so it holds its precision however long a note lasts (dsp/lifeosc.h). Here: a phase just short
// of a full cycle wraps without a seam, and the pitch at the end of a long run is the pitch at
// its start.
void testPhase() {
    std::printf("== lifeosc: phase over a long run\n");
    const af::Wavetable& t = table(af::TB_SINE);
    const float inc = 1000.3f / af::kRate;
    af::TableOsc o;
    const double start = 1.0 - 1.0 / 4096.0;
    o.reset(static_cast<float>(start));
    const Buf w = play(o, t, inc, 0.0f, 4096);
    double err = 0.0;
    for (size_t i = 0; i < w.size(); ++i) {
        const double ph = start + static_cast<double>(i) * static_cast<double>(inc);
        err = std::max(err, std::fabs(w[i] - std::sin(2.0 * kPi * ph)));
    }
    CHECK(err < 2e-4);

    const int n = 1 << 21;   // 48 s
    o.reset();
    const Buf x = play(o, t, inc, 0.0f, n);
    const double early = zeroCrossHz(x, 0, 1 << 16);
    const double late = zeroCrossHz(x, static_cast<size_t>(n - (1 << 16)), static_cast<size_t>(n));
    std::printf("  wrap error %.1e; %.6f Hz at the start, %.6f Hz after 48 s\n", err, early, late);
    CHECK(std::fabs(late - early) < 1e-4);
    CHECK(std::fabs(late - static_cast<double>(inc) * af::kRate) < 1e-3);
}

// Odd input: nothing to render, no pitch, out-of-range pitch and position, a one-frame table at
// any position.
void testEdges() {
    std::printf("== lifeosc: edges\n");
    af::TableOsc o;
    o.reset();
    float x[af::kChunk];
    std::fill(x, x + af::kChunk, 7.0f);
    o.render(table(af::TB_SAW), 0.01f, 0.0f, x, 0);
    CHECK(x[0] == 7.0f);
    bool finite = true;
    for (float inc : {0.0f, -0.1f, 0.7f, 2.0f, std::nanf("")})
        for (float pos : {-1.0f, 0.0f, 0.5f, 1.0f, 3.0f, std::nanf("")}) {
            o.render(table(af::TB_SINE_BLOOM), inc, pos, x, af::kChunk);
            for (float v : x) finite = finite && std::isfinite(v) && std::fabs(v) < 4.0f;
        }
    CHECK(finite);
    af::TableOsc a, b;
    a.reset();
    b.reset();
    CHECK(play(a, table(af::TB_SQUARE), 0.01f, 0.0f, 512) == play(b, table(af::TB_SQUARE), 0.01f, 0.83f, 512));
    // pos 1 sits on the last frame: the same as a position just short of it, give or take its
    // share of the next-to-last.
    af::TableOsc c, d;
    c.reset();
    d.reset();
    const Buf e1 = play(c, table(af::TB_SINE_BLOOM), 0.01f, 1.0f, 512);
    const Buf e2 = play(d, table(af::TB_SINE_BLOOM), 0.01f, 1.0f - 1e-6f, 512);
    float diff = 0.0f;
    for (size_t i = 0; i < e1.size(); ++i) diff = std::max(diff, std::fabs(e1[i] - e2[i]));
    CHECK(diff < 1e-3f);

    // A scan fed NaN (a rate, a time, a position) gives a position all the same, and recovers.
    af::LifeScan s;
    s.seed(9);
    bool ok = true;
    const float nan = std::nanf("");
    for (const af::LifePos& p : {af::LifePos{nan, nan, nan, nan}, af::LifePos{0.5f, 1.0f, nan, 1.0f}})
        for (float dt : {kStepS, nan, -1.0f}) {
            const float v = s.step(p, dt);
            ok = ok && v >= 0.0f && v <= 1.0f;
        }
    const af::LifePos sane{0.5f, 1.0f, 0.5f, 1.0f};
    float lo = 1.0f, hi = 0.0f;
    for (int i = 0; i < 4000; ++i) {
        const float v = s.step(sane, kStepS);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    CHECK(ok);
    CHECK(lo < 0.3f && hi > 0.7f);   // still swaying
}

} // namespace

void lifeoscTests() {
    testPitch();
    testAliasing();
    testGlide();
    testMipCrossfade();
    testSway();
    testSmear();
    testCouple();
    testDeterminism();
    testPhase();
    testEdges();
}

} // namespace aft
