// The table library (dsp/wavetable.*, dsp/lifetime.*, plugin/tables.*): names, band limits per
// mip level, equal RMS across a life, the life curves, size, determinism, and the builder
// thread's handoff to the audio side.
#include "check.h"
#include "../dsp/lifetime.h"
#include "../dsp/wavetable.h"
#include "../plugin/tables.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <thread>
#include <vector>

namespace aft {
namespace {

using cd = std::complex<double>;
using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// The test's own FFT (plain radix-2, forward), so a slip in the builder's can't hide itself.
void fft(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    std::vector<cd> w(n / 2);
    for (size_t k = 0; k < n / 2; ++k) w[k] = std::polar(1.0, -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n));
    for (size_t len = 2; len <= n; len <<= 1)
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w[k * (n / len)];
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
}

// |X_h|^2 for bins h = 0..n/2 of one mip level, read the way the oscillator reads it (sample * scale).
std::vector<double> power(const af::Wavetable& t, int frame, int mip) {
    const int n = af::mipLength(mip);
    std::vector<cd> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = t.at(frame, mip, i);
    fft(x);
    std::vector<double> p(static_cast<size_t>(n / 2 + 1));
    for (size_t h = 0; h < p.size(); ++h) p[h] = std::norm(x[h]);
    return p;
}

double rmsOf(const af::Wavetable& t, int frame) {
    double s = 0.0;
    for (int i = 0; i < af::kTableSize; ++i) s += static_cast<double>(t.at(frame, 0, i)) * t.at(frame, 0, i);
    return std::sqrt(s / af::kTableSize);
}

// The power-weighted mean harmonic number of the full-band level. Power, not amplitude: the 16-bit
// rounding floor, spread over 1023 bins, would otherwise outweigh the faint partials of a
// near-sine frame and wobble the mean.
double centroid(const af::Wavetable& t, int frame) {
    const std::vector<double> p = power(t, frame, 0);
    double s = 0.0, w = 0.0;
    for (int h = 1; h < af::kMaxHarmonic; ++h) {
        s += h * p[static_cast<size_t>(h)];
        w += p[static_cast<size_t>(h)];
    }
    return w > 0.0 ? s / w : 0.0;
}

double db(double ratio) { return 10.0 * std::log10(std::max(ratio, 1e-300)); }

void testNames() {
    std::printf("== tables: names, lifetime or not\n");
    static const char* const kNames[af::TB_COUNT] = {"Felt Piano", "Celesta", "Glass Harmonica", "Cello Tasto",
                                                     "Choir Ah-Oo", "Reed Organ", "Sine Bloom", "Tape Strings",
                                                     "Sine", "Triangle", "Saw", "Square"};
    bool names = true, kinds = true;
    for (int id = 0; id < af::TB_COUNT; ++id) {
        names = names && std::string(af::tableName(id)) == kNames[id];
        kinds = kinds && af::isLifetime(id) == (id < af::TB_SINE);
    }
    CHECK(names);
    CHECK(kinds);
    CHECK(std::string(af::tableName(-1)).empty());
    CHECK(std::string(af::tableName(af::TB_COUNT)).empty());
    CHECK(!af::isLifetime(-1) && !af::isLifetime(af::TB_COUNT));
    af::Wavetable none;
    CHECK(!af::buildTable(af::TB_COUNT, none) && !af::buildTable(-1, none));
    // The mip choice (ported): 20 Hz keeps all 1024 harmonics (their aliases fold back above
    // 18 kHz), 55 Hz the 256 of level 2, 15 kHz the fundamental only.
    CHECK(af::mipFor(20.0f / 44100.0f) == 0);
    CHECK(af::mipFor(55.0f / 44100.0f) == 2);
    CHECK(af::mipFor(15000.0f / 44100.0f) == af::kMipLevels - 1);
}

// The shape every table has: frames, layout, size (check 6), the guard samples, finite scales.
void testShapes(const std::vector<af::Wavetable>& tables) {
    std::printf("== tables: frames, layout, size\n");
    for (int id = 0; id < af::TB_COUNT; ++id) {
        const af::Wavetable& t = tables[static_cast<size_t>(id)];
        const int frames = af::isLifetime(id) ? af::kLifeFrames : 1;
        CHECK(t.name == af::tableName(id));
        CHECK(t.frames == frames);
        CHECK(t.data.size() == static_cast<size_t>(frames) * af::kFrameStride);
        CHECK(t.scale.size() == static_cast<size_t>(frames));
        CHECK(t.id != 0);
        bool finite = true, guards = true;
        for (int f = 0; f < t.frames; ++f) {
            finite = finite && std::isfinite(t.scale[static_cast<size_t>(f)]) && t.scale[static_cast<size_t>(f)] > 0.0f;
            for (int k = 0; k < af::kMipLevels; ++k) guards = guards && t.get(f, k)[af::mipLength(k)] == t.get(f, k)[0];
        }
        CHECK(finite);
        CHECK(guards);
        if (af::isLifetime(id)) CHECK(t.bytes() <= 4800000);   // 4.73 MB: 256 x 9227 x 2 + the scales
    }
}

// Check 1: every level k carries nothing above harmonic 1024 >> k (level 0: nothing on Nyquist).
// Frames 0, 1, 63, 128, 254 and 255 of each table: the ends and the middle of a life, both of a
// pair that shares an inverse FFT, and fast enough under ASan.
void testBandLimits(const std::vector<af::Wavetable>& tables) {
    std::printf("== tables: band limits per mip level (frames 0, 1, 63, 128, 254, 255)\n");
    const int frames[] = {0, 1, 63, 128, 254, 255};
    for (int id = 0; id < af::TB_COUNT; ++id) {
        const af::Wavetable& t = tables[static_cast<size_t>(id)];
        double worst = -400.0;
        for (int f : frames) {
            if (f >= t.frames) continue;
            for (int k = 0; k < af::kMipLevels; ++k) {
                const size_t top = static_cast<size_t>(std::min(af::kMaxHarmonic - 1, af::kMaxHarmonic >> k));
                const std::vector<double> p = power(t, f, k);
                double all = 0.0, above = 0.0;
                for (size_t h = 0; h < p.size(); ++h) {
                    all += p[h];
                    if (h > top) above += p[h];
                }
                worst = std::max(worst, db(above / all));
            }
        }
        std::printf("  %-16s worst %.0f dB\n", af::tableName(id), worst);
        CHECK(worst < -80.0);
    }
}

// Check 2: a lifetime table carries timbre, not level: frames 0, 128 and 255 within 0.5 dB of each
// other, and all at a full-scale sine's RMS.
void testEqualRms(const std::vector<af::Wavetable>& tables) {
    std::printf("== tables: equal RMS across a life\n");
    for (int id = 0; id < af::TB_SINE; ++id) {
        const af::Wavetable& t = tables[static_cast<size_t>(id)];
        CHECK(t.frames == af::kLifeFrames);
        if (t.frames != af::kLifeFrames) continue;
        const double r[3] = {rmsOf(t, 0), rmsOf(t, 128), rmsOf(t, 255)};
        const double lo = std::min({r[0], r[1], r[2]}), hi = std::max({r[0], r[1], r[2]});
        CHECK(20.0 * std::log10(hi / lo) <= 0.5);
        CHECK(std::fabs(20.0 * std::log10(lo / std::sqrt(0.5))) < 0.1);
        CHECK(std::fabs(20.0 * std::log10(hi / std::sqrt(0.5))) < 0.1);
    }
}

// Check 3: Felt Piano darkens frame by frame (its upper harmonics die first), Sine Bloom brightens
// from a pure sine. "Monotonic" to within the 16-bit rounding (1e-4 harmonics).
void testLifeCurves(const std::vector<af::Wavetable>& tables) {
    std::printf("== tables: the life curves\n");
    for (int id : {af::TB_FELT_PIANO, af::TB_SINE_BLOOM}) {
        const af::Wavetable& t = tables[static_cast<size_t>(id)];
        CHECK(t.frames == af::kLifeFrames);
        if (t.frames != af::kLifeFrames) return;
        const double sign = id == af::TB_FELT_PIANO ? -1.0 : 1.0;   // falls, or rises
        std::vector<double> c(af::kLifeFrames);
        int wrong = 0;
        for (int f = 0; f < af::kLifeFrames; ++f) {
            c[static_cast<size_t>(f)] = centroid(t, f);
            if (f > 0 && sign * (c[static_cast<size_t>(f)] - c[static_cast<size_t>(f) - 1]) < -1e-4) ++wrong;
        }
        std::printf("  %-12s centroid (harmonics): frame 0 %.3f, 64 %.3f, 128 %.3f, 192 %.3f, 255 %.3f; %d steps the wrong way\n",
                    af::tableName(id), c[0], c[64], c[128], c[192], c[255], wrong);
        CHECK(wrong == 0);
        CHECK(sign * (c[255] - c[0]) > 0.3);
    }
    // Sine Bloom starts as a pure sine: everything above the fundamental under -60 dB.
    const std::vector<double> p = power(tables[af::TB_SINE_BLOOM], 0, 0);
    double rest = 0.0;
    for (size_t h = 2; h < p.size(); ++h) rest += p[h];
    CHECK(db(rest / p[1]) < -60.0);
}

// Checks 4 and 5: a fresh TableSet reads the sine everywhere; a published table is what get()
// returns; the shared builder publishes all 12 within 30 s, the same data as building here.
void testHandoff(const std::vector<af::Wavetable>& built, Clock::time_point started) {
    std::printf("== tables: the builder thread and the handoff\n");
    af::TableSet s;
    bool fallback = true;
    for (int id = -1; id <= af::TB_COUNT; ++id) fallback = fallback && &s.get(id) == &af::sineTable();
    CHECK(fallback);
    CHECK(af::sineTable().frames == 1);
    s.t[af::TB_SAW].store(&built[af::TB_SAW], std::memory_order_release);
    CHECK(&s.get(af::TB_SAW) == &built[af::TB_SAW]);
    CHECK(&s.get(af::TB_SQUARE) == &af::sineTable());

    af::TableSet& shared = af::sharedTables();
    auto published = [&shared] {
        int n = 0;
        for (int id = 0; id < af::TB_COUNT; ++id) n += shared.t[id].load(std::memory_order_acquire) != nullptr;
        return n;
    };
    while (published() < af::TB_COUNT && secondsSince(started) < 30.0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const double took = secondsSince(started);
    std::printf("  the builder published %d of %d tables in %.1f s\n", published(), af::TB_COUNT, took);
    CHECK(published() == af::TB_COUNT);

    // A later call returns at once and leaves the published tables as they are.
    const af::Wavetable* before[af::TB_COUNT];
    for (int id = 0; id < af::TB_COUNT; ++id) before[id] = shared.t[id].load(std::memory_order_acquire);
    const Clock::time_point again = Clock::now();
    af::ensureTablesBuilding();
    CHECK(secondsSince(again) < 0.05);
    bool kept = true;
    for (int id = 0; id < af::TB_COUNT; ++id) kept = kept && shared.t[id].load(std::memory_order_acquire) == before[id];
    CHECK(kept);

    bool same = true, picked = true;
    for (int id = 0; id < af::TB_COUNT; ++id) {
        const af::Wavetable* p = shared.t[id].load(std::memory_order_acquire);
        if (!p) continue;
        picked = picked && &shared.get(id) == p;
        const af::Wavetable& b = built[static_cast<size_t>(id)];
        same = same && p->name == b.name && p->frames == b.frames && p->data == b.data && p->scale == b.scale;
    }
    CHECK(picked);
    CHECK(same);   // built twice (here and on the builder thread): identical
}

} // namespace

void tablesTests() {
    const Clock::time_point started = Clock::now();
    af::ensureTablesBuilding();   // the builder works alongside the builds below
    testNames();

    std::vector<af::Wavetable> built(af::TB_COUNT);
    std::printf("  built in");
    for (int id = 0; id < af::TB_COUNT; ++id) {
        const Clock::time_point t0 = Clock::now();
        CHECK(af::buildTable(id, built[static_cast<size_t>(id)]));
        std::printf("%s %s %.0f ms", id ? "," : "", af::tableName(id), 1e3 * secondsSince(t0));
    }
    std::printf("\n");

    testShapes(built);
    testBandLimits(built);
    testEqualRms(built);
    testLifeCurves(built);
    testHandoff(built, started);
}

} // namespace aft
