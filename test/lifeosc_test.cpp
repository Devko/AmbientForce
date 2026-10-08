// The lifetime oscillator (dsp/lifeosc.h): pitch, aliasing, the position glide and the mip level
// crossfade, every read against a plain model of it, Sway and Smear, the Couple modes,
// determinism and the phase over a long run.
// Needs no plugin: make test-module M=lifeosc runs it on its own.
#include "check.h"
#include "signal.h"
#include "../dsp/lifeosc.h"
#include "../dsp/lifetime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <vector>

namespace aft {
namespace {

constexpr int kOscBlock = 128;   // MPC's block
constexpr float kStepS = static_cast<float>(af::kChunk) / af::kRate;
const af::BeatClock kClock;   // the clock a free scan is stepped with (it doesn't read it)

// n samples of one oscillator (either read) at a fixed pitch and position, in blocks of `block`.
template <class Osc>
Buf play(Osc& o, const af::Wavetable& t, float inc, float pos, int n, int block = kOscBlock) {
    Buf x(static_cast<size_t>(n));
    for (int i = 0; i < n; i += block) o.render(t, inc, pos, &x[static_cast<size_t>(i)], std::min(block, n - i));
    return x;
}

// Check 1: the sine table plays a clean 440 Hz at full scale.
void testPitch() {
    std::printf("== lifeosc: pitch\n");
    af::TableOsc o;
    o.reset();
    const float inc = 440.0f / af::kRate;
    const Buf x = play(o, testTable(af::TB_SINE), inc, 0.0f, 1 << 16);
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
        const Buf x = play(o, testTable(af::TB_SAW), inc, 0.0f, 1 << 16);
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
    const af::Wavetable& t = testTable(af::TB_SINE_BLOOM);
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
    const af::Wavetable& t = testTable(af::TB_SAW);
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

// --- every read against a plain model of it ---------------------------------------------------
//
// The model reads one sample at a time, in doubles, from Wavetable::at: the position glides from
// the last render's to this one's as documented (sample i of n at f0 + (f1 - f0) (i + 1) / n, the
// ends pos x (frames - 1) as floats), the frame pair and the share come from it, each frame is
// interpolated on its own (common.h's Hermite formula, or linear) and the two are crossfaded. The
// one thing it takes from the oscillator is the phase's own definition: a 32-bit fraction of a
// cycle stepping by inc x 2^32, read 2^-24 cycles finer with FM.

double hermiteRef(double xm, double x0, double x1, double x2, double t) {
    const double c1 = 0.5 * (x1 - xm);
    const double c2 = xm - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
    const double c3 = 0.5 * (x2 - xm) + 1.5 * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// One sample at frame position f (0..frames - 1) and phase p, level `mip`.
double modelSample(const af::Wavetable& t, int mip, double f, uint32_t p, bool hermite) {
    const int bits = af::kMipBits[mip], len = af::mipLength(mip);
    const uint32_t j = p >> (32 - bits), wrap = static_cast<uint32_t>(len - 1);
    const double u = static_cast<double>(p & ((1u << (32 - bits)) - 1u)) / static_cast<double>(1u << (32 - bits));
    auto frame = [&](int fr) {
        auto at = [&](uint32_t i) { return static_cast<double>(t.at(fr, mip, static_cast<int>(i))); };
        if (hermite) return hermiteRef(at((j - 1) & wrap), at(j), at(j + 1), at((j + 2) & wrap), u);
        return at(j) + u * (at(j + 1) - at(j));
    };
    if (t.frames == 1) return frame(0);
    const int a = std::min(static_cast<int>(std::floor(f)), t.frames - 2);
    const double x = f - a;
    return (1.0 - x) * frame(a) + x * frame(a + 1);
}

struct ModelRun {
    double err[4] = {};     // the largest difference from the model, by kind
    int kinds[4][2] = {};   // renders: [one frame, pair still, pair gliding, walk][FM]
};

// `renders` renders of random lengths from random positions and moves (stays, small moves,
// frame crossings, jumps, the ends), at a random pitch, with and without FM, against the model.
template <class Osc>
void modelRuns(const af::Wavetable& t, bool hermite, uint32_t seed, int sequences, int renders, ModelRun& r) {
    uint32_t rng = seed;
    auto rand01 = [&] { return af::lifeosc::rand01(rng); };
    const float frames1 = static_cast<float>(t.frames - 1);
    std::vector<float> out(256), fm(256);
    for (int q = 0; q < sequences; ++q) {
        Osc o;
        const float phase0 = rand01();
        o.reset(phase0);
        uint32_t ph = static_cast<uint32_t>(static_cast<uint64_t>(static_cast<double>(phase0) * 4294967296.0));
        const float inc = 20.0f * std::pow(400.0f, rand01()) / af::kRate;   // 20 Hz .. 8 kHz
        const int mip = af::mipFor(inc);
        const uint32_t dph = static_cast<uint32_t>(inc * 4294967296.0f);
        const bool withFm = (q & 1) != 0;
        float last = rand01();
        bool started = false;
        for (int k = 0; k < renders; ++k) {
            const int n = 1 + static_cast<int>(rand01() * 128.0f);
            const float move = rand01();
            float pos = last;
            if (move < 0.2f) pos = last;                                                   // stays
            else if (move < 0.45f) pos = af::clampf(last + 0.004f * (rand01() - 0.5f), 0.0f, 1.0f);   // within a pair, mostly
            else if (move < 0.7f) pos = af::clampf(last + 0.04f * (rand01() - 0.5f), 0.0f, 1.0f);     // a few frames
            else if (move < 0.85f) pos = rand01();                                         // a jump
            else pos = move < 0.925f ? 0.0f : 1.0f;                                        // an end
            for (int i = 0; i < n; ++i) fm[static_cast<size_t>(i)] = withFm ? 1.2f * (rand01() - 0.5f) : 0.0f;
            o.render(t, inc, pos, out.data(), n, withFm ? fm.data() : nullptr);

            const float f1 = pos * frames1, f0 = started ? last * frames1 : f1;
            int kind = 0;
            if (t.frames > 1) {
                const int a0 = std::min(static_cast<int>(f0), t.frames - 2), a1 = std::min(static_cast<int>(f1), t.frames - 2);
                const float x0 = f0 - static_cast<float>(a0);
                kind = a0 != a1 ? 3 : f0 != f1 ? 2 : (x0 == 0.0f || x0 == 1.0f) ? 0 : 1;
            }
            ++r.kinds[kind][withFm ? 1 : 0];
            for (int i = 0; i < n; ++i) {
                const double f = f0 + (static_cast<double>(f1) - f0) * (i + 1) / n;
                uint32_t p = ph;
                if (withFm) p += static_cast<uint32_t>(static_cast<int32_t>(fm[static_cast<size_t>(i)] * 16777216.0f)) << 8;
                ph += dph;
                const double e = std::fabs(out[static_cast<size_t>(i)] - modelSample(t, mip, f, p, hermite));
                r.err[kind] = std::max(r.err[kind], e);
            }
            last = pos;
            started = true;
        }
    }
}

void testModel() {
    std::printf("== lifeosc: every read against a plain model\n");
    const auto t0 = std::chrono::steady_clock::now();
    ModelRun h, l;
    modelRuns<af::TableOsc>(testTable(af::TB_FELT_PIANO), true, 0x1234567u, 40, 50, h);
    modelRuns<af::TableOscLinear>(testTable(af::TB_FELT_PIANO), false, 0x2345678u, 40, 50, l);
    modelRuns<af::TableOsc>(testTable(af::TB_SAW), true, 0x3456789u, 4, 50, h);
    modelRuns<af::TableOscLinear>(testTable(af::TB_SAW), false, 0x456789au, 4, 50, l);
    const double ms = 1e3 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  4400 renders in %.0f ms\n", ms);
    for (const ModelRun* r : {&h, &l}) {
        std::printf("  %s within: one frame %.1e, pair still %.1e, pair gliding %.1e, walk %.1e\n",
                    r == &h ? "Hermite" : "linear", r->err[0], r->err[1], r->err[2], r->err[3]);
        std::printf("    renders: one frame %d / %d, pair still %d / %d, pair gliding %d / %d, walk %d / %d (FM off / on)\n",
                    r->kinds[0][0], r->kinds[0][1], r->kinds[1][0], r->kinds[1][1], r->kinds[2][0], r->kinds[2][1],
                    r->kinds[3][0], r->kinds[3][1]);
        bool all = true;
        for (const auto& kind : r->kinds) all = all && kind[0] >= 50 && kind[1] >= 50;
        CHECK(all);   // every kind, with and without FM, often
        CHECK(*std::max_element(r->err, r->err + 4) < 1e-6);
    }
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
        const float v = s.step(p, kStepS, kClock);
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
        const float v = e.step(q, kStepS, kClock);
        elo = std::min(elo, v);
        ehi = std::max(ehi, v);
        atEnd += v >= 0.999f ? 1 : 0;
    }
    std::printf("  age 0.95: %.4f..%.4f, %.1f%% of the time on the last frame\n", elo, ehi, 100.0 * atEnd / steps);
    CHECK(elo >= 0.70f - 1e-4f && elo <= 0.71f && ehi <= 1.0f && ehi >= 0.999f);
    CHECK(atEnd < steps / 20);   // a clamp would hold it there 44% of the time

    // Age 0.05: -0.20..0.30, the part before the start folded back the same way.
    af::LifeScan z;
    z.seed(3);
    const af::LifePos r{0.05f, 1.0f, 0.5f, 0.0f};
    float zlo = 1.0f, zhi = 0.0f;
    int atStart = 0;
    for (int i = 0; i < steps; ++i) {
        const float v = z.step(r, kStepS, kClock);
        zlo = std::min(zlo, v);
        zhi = std::max(zhi, v);
        atStart += v <= 0.001f ? 1 : 0;
    }
    std::printf("  age 0.05: %.4f..%.4f, %.1f%% of the time on the first frame\n", zlo, zhi, 100.0 * atStart / steps);
    CHECK(zlo >= 0.0f && zlo <= 0.001f && zhi >= 0.30f - 1e-4f && zhi <= 0.30f + 1e-4f);
    CHECK(atStart < steps / 20);

    // reset(phase) sets the sway's phase: two scans from different seeds, reset alike, agree.
    af::LifeScan a, b;
    a.seed(3);
    b.seed(4);
    a.reset(0.25f);
    b.reset(0.25f);
    bool same = true;
    for (int i = 0; i < 1000; ++i) same = same && a.step(p, kStepS, kClock) == b.step(p, kStepS, kClock);
    CHECK(same);
    a.reset(0.25f);
    CHECK(std::fabs(a.step(af::LifePos{0.5f, 1.0f, 0.0f, 0.0f}, 0.0f, kClock) - 0.75f) < 1e-4f);   // a quarter cycle in: the top

    // Synced (swayBeats, on a BeatClock that MPC moves on): the sway is pulled onto the clock's phase, one cycle per
    // 4 beats, at Age on the downbeat and rising, and stays there exactly; two scans of other seeds sway together, and
    // one staggered by a quarter cycle (setSyncOffset) sits a quarter cycle on. A
    // jump of the clock glides (no step over 0.02 of the table at full sway) and lands on the bar again. Back on
    // Free, each goes its own way again, and lands on its own phase exactly: from then on it reads what a twin that
    // stayed Free all along reads, bit for bit.
    af::BeatClock clock;
    const af::LifePos bar{0.5f, 1.0f, 0.05f, 0.0f, 4.0f};
    const af::LifePos unsynced{0.5f, 1.0f, 0.05f, 0.0f, 0.0f};
    double beat = 0.0;
    float va = 0.0f, vb = 0.0f, vd = 0.0f, jumpStep = 0.0f;
    af::LifeScan d;   // staggered a quarter cycle on (setSyncOffset, as Bloom staggers its voices)
    d.seed(5);
    d.setSyncOffset(0.25);
    af::LifeScan twin = a;   // Free throughout
    float vt = 0.0f;
    // `steps` control steps with MPC playing from `beat` on.
    const auto play = [&](int steps) {
        for (int i = 0; i < steps; ++i) {
            clock.set(120.0, beat);
            clock.advance(32);
            beat += 32.0 * 2.0 / 44100.0;
            const float wa = a.step(bar, kStepS, clock);
            jumpStep = std::max(jumpStep, std::fabs(wa - va));
            va = wa;
            vb = b.step(bar, kStepS, clock);
            vd = d.step(bar, kStepS, clock);
            vt = twin.step(unsynced, kStepS, clock);
        }
    };
    const auto onBar = [&]() {
        return std::fabs(va - (0.5f + 0.25f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * beat / 4.0)))) < 1e-4f;
    };
    play(static_cast<int>(1.5f / kStepS));
    jumpStep = 0.0f;
    const bool locked = onBar() && va == vb &&
                        std::fabs(vd - (0.5f + 0.25f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * (beat / 4.0 + 0.25))))) < 1e-4f;
    beat = 101.0;   // a locate
    play(static_cast<int>(0.5f / kStepS));
    const bool relocked = onBar() && va == vb;
    std::printf("  synced to 1 bar: on it and together %s, a locate's largest step %.4f, on the bar again %s\n",
                locked ? "yes" : "NO", jumpStep, relocked ? "yes" : "NO");
    CHECK(locked && relocked && jumpStep < 0.02f);   // set at once, the locate jumped 0.5
    // Back on Free the two scans drift back to their own phases: apart again, and on the twin's exactly.
    float fa = 0.0f, fb = 0.0f;
    bool landed = false;
    int sameAfter = -1;
    for (int i = 0; i < static_cast<int>(1.5f / kStepS); ++i) {
        fa = a.step(unsynced, kStepS, clock);
        fb = b.step(unsynced, kStepS, clock);
        vt = twin.step(unsynced, kStepS, clock);
        if (fa == vt && !landed) sameAfter = i;
        landed = fa == vt;
    }
    std::printf("  back on Free: %.4f and %.4f; on its own phase, bit for bit, %.2f s on\n", fa, fb, sameAfter * kStepS);
    CHECK(std::fabs(fa - fb) > 0.01f && landed && sameAfter > 0);
    // A clock moved on only by the steps (a stratum on its own, never set): at 120 BPM a 4-beat sway takes 2 s.
    af::LifeScan c;
    c.seed(7);
    af::BeatClock run;
    run.set(120.0, 0.0);
    float first = 0.0f;
    for (int i = 0; i < static_cast<int>(1.0f / kStepS); ++i) {   // pulled onto the clock first
        run.advance(32);
        first = c.step(bar, kStepS, run);
    }
    float back = -1.0f;
    for (int i = 0; i < static_cast<int>(2.0f / kStepS); ++i) {
        run.advance(32);
        back = c.step(bar, kStepS, run);
    }
    std::printf("  synced to 1 bar: %.4f, and %.4f two seconds on\n", first, back);
    CHECK(std::fabs(back - first) < 2e-3f);
}

// A synced sway's division doubles where the sway would run faster than 4 Hz at the tempo (kMaxSyncSwayHz; a free one
// tops out at 2 Hz): 1/4 at 300 BPM would be 5 Hz and runs 2.5, one a half note; at 239 BPM, 3.98 Hz, under the cap,
// it stays one a beat. The rate measured from the position's swing.
void testSwayCap() {
    std::printf("== lifeosc: a synced sway's division doubles past 4 Hz\n");
    const auto rate = [](double bpm) {
        af::LifeScan s;
        s.seed(13);
        af::BeatClock clock;
        clock.set(bpm, 0.0);
        const af::LifePos p{0.5f, 1.0f, 0.05f, 0.0f, 1.0f};   // 1/4
        float prev = 0.5f;
        double first = -1.0, last = -1.0;
        int ups = 0;
        const int steps = static_cast<int>(3.0f / kStepS);
        for (int k = 0; k < steps; ++k) {
            clock.advance(af::kChunk);
            const float x = s.step(p, kStepS, clock) - 0.5f;   // the sway around Age
            if (k > steps / 3 && prev < 0.0f && x >= 0.0f) {   // after a second: an upward crossing
                const double at = (k - static_cast<double>(x) / (x - prev)) * kStepS;
                if (first < 0.0) first = at;
                last = at;
                ++ups;
            }
            prev = x;
        }
        return ups > 1 ? (ups - 1) / (last - first) : 0.0;
    };
    const double fast = rate(300.0), under = rate(239.0);
    std::printf("  1/4 at 300 BPM: %.3f Hz (doubled once from 5); at 239 BPM: %.3f Hz (3.983: not doubled)\n", fast, under);
    CHECK(std::fabs(fast - 2.5) < 0.01 && std::fabs(under - 239.0 / 60.0) < 0.01);
}

// PulledCycle (common.h), the cycle under the breath and the sways: a jump of the target by just under half a cycle
// is mostly made up within 50 ms (the time constant: 1 / e of it left) and landed on exactly (the same double) within
// about a second, never past it; land() puts the next step on the target at once (from silence); and a free cycle's
// phase is its own, exactly, every step.
void testPulledCycle() {
    std::printf("== lifeosc: a pulled cycle's glide and landing\n");
    af::BeatClock clock;
    clock.set(120.0, 0.0);
    af::PulledCycle c;
    c.reset(0.3);
    c.land();
    // One step on the clock, synced to a bar; how far from the clock's phase it is then (the short way round).
    const auto step = [&]() {
        clock.advance(af::kChunk);
        const double ph = c.step(0.001, kStepS, clock, 4.0, af::kMaxSyncSwayHz, 0.0);
        const af::ClockCycle want = clock.cycle(4.0, af::kMaxSyncSwayHz, kStepS);
        double d = want.phase - ph;
        d -= std::floor(d + 0.5);
        return d;
    };
    const bool atOnce = step() == 0.0;   // landed: no glide from 0.3
    clock.beats += 0.4999 * 4.0;         // a locate, just under half a cycle on
    double after50 = 0.0;
    int steps = 0;
    for (double d = 1.0; d != 0.0 && steps < 4000; ++steps) {
        d = step();
        if (steps == static_cast<int>(0.05f / kStepS) - 1) after50 = std::fabs(d);
    }
    std::printf("  from 0.4999 of a cycle away: %.3f of it left after 50 ms, landed exactly after %.2f s\n",
                after50 / 0.4999, steps * kStepS);
    CHECK(atOnce && after50 / 0.4999 < 0.4 && steps * kStepS > 0.7f && steps * kStepS < 1.1f);
    // Free: the phase is its own, bit for bit, step after step (no pull to make).
    af::PulledCycle f;
    f.reset(0.7);
    bool own = true;
    for (int i = 0; i < 10000; ++i) own = own && f.step(0.000913, kStepS, clock, 0.0, af::kMaxSyncSwayHz, 0.0) == f.own;
    CHECK(own);
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
        for (float& x : v) x = s.step(p, kStepS, kClock);
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
        af::renderCoupled(a, testTable(tableA), static_cast<float>(hzA) / af::kRate, b, testTable(tableB),
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
        CHECK(b1 == play(o, testTable(af::TB_SQUARE), static_cast<float>(f2) / af::kRate, 0.5f, 4096, af::kChunk));
    }
    {   // Mix at blend 0 doesn't read B but keeps it going: two sines in tune, one step at blend 0,
        // then half of each. In step they add up to full scale; had B stood still for the step
        // (0.32 cycles at 441 Hz) they would come to 0.54.
        const float inc = 441.0f / af::kRate;
        af::TableOsc a;
        af::TableOscLinear b;
        a.reset();
        b.reset();
        Buf x(64 * af::kChunk);
        float scratch[2 * af::kChunk];
        for (int k = 0; k < 64; ++k)
            af::renderCoupled(a, testTable(af::TB_SINE), inc, b, testTable(af::TB_SINE), inc, 0.5f, af::CP_MIX, 0.0f,
                              k == 0 ? 0.0f : 0.5f, &x[static_cast<size_t>(k * af::kChunk)], scratch, af::kChunk);
        std::printf("  Mix, blend 0 for a step, then 0.5: peak %.4f\n", peak(x, af::kChunk));
        CHECK(std::fabs(peak(x, af::kChunk) - 1.0f) < 1e-3f);
    }
    {   // In every mode, the blend at 0.5, then at an end (0: B skipped at Mix; 1: A skipped in
        // every mode) for 32 steps, then at 0.5 again, while both positions move and both pitches
        // cross into the next mip level (step 40): sample for sample what rendering both
        // throughout and coupling them at the blend gives. (The pitches are worked out once for
        // both: the device build may divide by the rate in one place and multiply by its
        // reciprocal in another, an ulp apart.)
        const af::Wavetable& t = testTable(af::TB_SINE_BLOOM);
        const float edge = af::kAliasLimit / 64.0f, incLo = edge * 0.99f, incHi = edge * 1.01f;
        const float incA0 = incLo * 0.5f, incA1 = incHi * 0.5f;   // the level below's edge, an octave down
        CHECK(af::mipFor(incA0) != af::mipFor(incA1) && af::mipFor(incLo) != af::mipFor(incHi));
        bool same = true;
        for (int mode = 0; mode < af::CP_COUNT; ++mode)
            for (float end : {0.0f, 1.0f}) {
                af::TableOsc a, as;
                af::TableOscLinear b, bs;
                a.reset();
                b.reset();
                as.reset();
                bs.reset();
                Buf skipped(96 * af::kChunk), full(96 * af::kChunk);
                float scratch[2 * af::kChunk], bb[af::kChunk], fm[af::kChunk];
                for (int k = 0; k < 96; ++k) {
                    const float blend = k >= 32 && k < 64 ? end : 0.5f, pos = 0.3f + 0.002f * static_cast<float>(k);
                    const float incA = k < 40 ? incA0 : incA1, incB = k < 40 ? incLo : incHi;
                    float* o = &full[static_cast<size_t>(k * af::kChunk)];
                    af::renderCoupled(as, t, incA, bs, t, incB, pos, mode, 0.7f, blend,
                                      &skipped[static_cast<size_t>(k * af::kChunk)], scratch, af::kChunk);
                    b.render(t, incB, pos, bb, af::kChunk);
                    af::fmInput(0.7f, bb, fm, af::kChunk);
                    a.render(t, incA, pos, o, af::kChunk, mode == af::CP_FM ? fm : nullptr);
                    af::couple(mode, 0.7f, blend, o, bb, o, af::kChunk);
                }
                same = same && skipped == full && peak(skipped) > 0.3f;
            }
        CHECK(same);
    }
    {   // AM and Ring at amt 0 are Mix, sample for sample.
        const Buf mix = coupled(af::CP_MIX, 0.0f, 0.3f, af::TB_SINE_BLOOM, f1, af::TB_SAW, f2, 4096);
        CHECK(coupled(af::CP_AM, 0.0f, 0.3f, af::TB_SINE_BLOOM, f1, af::TB_SAW, f2, 4096) == mix);
        CHECK(coupled(af::CP_RING, 0.0f, 0.3f, af::TB_SINE_BLOOM, f1, af::TB_SAW, f2, 4096) == mix);
    }
    {   // skip() leaves an oscillator where render() would: the phase, the level, the position.
        const af::Wavetable& t = testTable(af::TB_SINE_BLOOM);
        af::TableOscLinear r, s;
        r.reset(0.2f);
        s.reset(0.2f);
        float x[af::kChunk], y[af::kChunk];
        r.render(t, 0.003f, 0.1f, x, af::kChunk);
        s.render(t, 0.003f, 0.1f, y, af::kChunk);
        r.render(t, 0.02f, 0.6f, x, 17);   // another level, another position, an odd length
        s.skip(t, 0.02f, 0.6f, 17);
        r.render(t, 0.02f, 0.6f, x, af::kChunk);
        s.render(t, 0.02f, 0.6f, y, af::kChunk);
        CHECK(std::equal(x, x + af::kChunk, y));
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
            o.render(testTable(af::TB_SINE_BLOOM), 196.0f / af::kRate, s.step(p, kStepS, kClock), &x[i], n);
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
    const af::Wavetable& t = testTable(af::TB_SINE);
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
    o.render(testTable(af::TB_SAW), 0.01f, 0.0f, x, 0);
    CHECK(x[0] == 7.0f);
    bool finite = true;
    for (float inc : {0.0f, -0.1f, 0.7f, 2.0f, std::nanf("")})
        for (float pos : {-1.0f, 0.0f, 0.5f, 1.0f, 3.0f, std::nanf("")}) {
            o.render(testTable(af::TB_SINE_BLOOM), inc, pos, x, af::kChunk);
            for (float v : x) finite = finite && std::isfinite(v) && std::fabs(v) < 4.0f;
        }
    CHECK(finite);
    af::TableOsc a, b;
    a.reset();
    b.reset();
    CHECK(play(a, testTable(af::TB_SQUARE), 0.01f, 0.0f, 512) == play(b, testTable(af::TB_SQUARE), 0.01f, 0.83f, 512));
    // pos 1 sits on the last frame: the same as a position just short of it, give or take its
    // share of the next-to-last.
    af::TableOsc c, d;
    c.reset();
    d.reset();
    const Buf e1 = play(c, testTable(af::TB_SINE_BLOOM), 0.01f, 1.0f, 512);
    const Buf e2 = play(d, testTable(af::TB_SINE_BLOOM), 0.01f, 1.0f - 1e-6f, 512);
    float diff = 0.0f;
    for (size_t i = 0; i < e1.size(); ++i) diff = std::max(diff, std::fabs(e1[i] - e2[i]));
    CHECK(diff < 1e-3f);

    // A scan fed a NaN or an infinity (a rate, a time, a position, a phase) gives a position all
    // the same, and keeps swaying afterwards.
    const float nan = std::nanf(""), inf = INFINITY;
    const af::LifePos sane{0.5f, 1.0f, 0.5f, 1.0f};
    auto swaysOn = [&](af::LifeScan& s) {
        float lo = 1.0f, hi = 0.0f;
        for (int i = 0; i < 4000; ++i) {
            const float v = s.step(sane, kStepS, kClock);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        return lo < 0.3f && hi > 0.7f;
    };
    for (const af::LifePos& p : {af::LifePos{nan, nan, nan, nan}, af::LifePos{0.5f, 1.0f, nan, 1.0f},
                                 af::LifePos{inf, inf, inf, inf}, af::LifePos{0.5f, 1.0f, -inf, 1.0f},
                                 af::LifePos{0.5f, 1.0f, 0.5f, 1.0f, nan}, af::LifePos{0.5f, 1.0f, 0.5f, 1.0f, inf}})
        for (float dt : {kStepS, nan, -1.0f, inf, -inf}) {
            af::LifeScan s;
            s.seed(9);
            const float v = s.step(p, dt, kClock);
            CHECK(v >= 0.0f && v <= 1.0f);
            CHECK(swaysOn(s));
        }
    for (float phase : {nan, inf, -inf}) {
        af::LifeScan s;
        s.seed(9);
        s.reset(phase);
        CHECK(swaysOn(s));
    }

    // An oscillator reset to a phase that isn't a number starts at 0.
    for (float phase : {nan, inf, -inf}) {
        af::TableOsc p, q;
        p.reset(phase);
        q.reset(0.0f);
        CHECK(play(p, testTable(af::TB_SINE), 0.01f, 0.0f, 256) == play(q, testTable(af::TB_SINE), 0.01f, 0.0f, 256));
    }
}

} // namespace

void lifeoscTests() {
    testPitch();
    testAliasing();
    testGlide();
    testMipCrossfade();
    testModel();
    testSway();
    testSwayCap();
    testPulledCycle();
    testSmear();
    testCouple();
    testDeterminism();
    testPhase();
    testEdges();
}

} // namespace aft
