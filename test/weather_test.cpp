// Weather (dsp/weather.h) and its sources (dsp/grainsrc.h) on their own: a source's loop, level and
// slower levels; silence without a source; pitch to the cent and no aliasing two octaves up; To Key's
// pitch classes; the level across grain counts; Stream's seamless joins and its level drifting or
// transposed; the gate's fade; Duck; a source or mode change mid-cloud (the old source scribbled
// over under it); origin; the window against the Hann; mute; a source freed while Weather is
// silent; a call cut into the engine's pieces; determinism, stability, and nothing allocating.
// Needs no plugin: make test-module M=weather.
#include "check.h"
#include "signal.h"
#include "../dsp/grainsrc.h"
#include "../dsp/weather.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
// The sanitizer runtime's (sanitizer/allocator_interface.h, which GCC doesn't install): hooks its
// allocator calls on every allocation and free.
extern "C" int __sanitizer_install_malloc_and_free_hooks(void (*malloc_hook)(const volatile void*, size_t),
                                                         void (*free_hook)(const volatile void*));
#define WEATHER_COUNTS_ALLOCS 1
#endif

namespace aft {
namespace {

using af::Weather;
constexpr int kBlk = 128;   // MPC's block
constexpr int kSec = 44100;

struct Out {
    Buf L, R, sendL, sendR;
};

// A source from one signal on both channels, or two.
std::unique_ptr<af::SourceBuffer> source(const Buf& L, const Buf& R) {
    return af::buildSource(L.data(), R.data(), static_cast<int>(L.size()));
}
std::unique_ptr<af::SourceBuffer> source(const Buf& x) { return source(x, x); }

// A level of a source as floats, one channel (frames of it, no guard).
Buf levelOf(const af::GrainSource& s, int level, int ch) {
    const int n = s.frames >> level;
    Buf x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = s.level[level][2 * i + ch] * s.gain;
    return x;
}

// The patch the checks start from: audible, everything else Init's.
af::WeatherPatch loud(int mode = af::WM_CLOUD) {
    af::WeatherPatch p;
    p.level = 1.0f;
    p.mode = mode;
    return p;
}

// n samples of w over src in blocks of `blk` (MPC's by default), into zeroed buffers (render adds),
// the send at 0.5, Bloom's peak `duckPeak` throughout.
Out play(Weather& w, const af::GrainSource* src, int n, float duckPeak = 0.0f, int blk = kBlk) {
    const size_t len = static_cast<size_t>(n);
    Out o{Buf(len), Buf(len), Buf(len), Buf(len)};
    for (int i = 0; i < n; i += blk) {
        const size_t at = static_cast<size_t>(i);
        w.render(src, duckPeak, &o.L[at], &o.R[at], &o.sendL[at], &o.sendR[at], 0.5f, std::min(blk, n - i));
    }
    return o;
}
Out cat(Out a, const Out& b) {
    a.L.insert(a.L.end(), b.L.begin(), b.L.end());
    a.R.insert(a.R.end(), b.R.begin(), b.R.end());
    a.sendL.insert(a.sendL.end(), b.sendL.begin(), b.sendL.end());
    a.sendR.insert(a.sendR.end(), b.sendR.begin(), b.sendR.end());
    return a;
}

// White noise through four one-poles at 2 kHz: next to nothing above 4 kHz.
Buf lowNoise(int n, uint32_t seed) {
    Buf x = whiteNoise(n, 0.5f, seed);
    const float a = 1.0f - std::exp(-2.0f * static_cast<float>(kPi) * 2000.0f / kSec);
    for (int pass = 0; pass < 4; ++pass) {
        float s = 0.0f;
        for (float& v : x) v = s += a * (v - s);
    }
    return x;
}

bool same(const Buf& a, const Buf& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}
// Both channels' RMS together, from `from` on, in dBFS.
double dbRms(const Out& o, size_t from) {
    const double l = rms(o.L, from), r = rms(o.R, from);
    return db(std::sqrt(0.5 * (l * l + r * r)));
}
size_t at(double seconds) { return static_cast<size_t>(seconds * kSec); }
double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }

// The frequency of the tone near `hz` in x[from, to) (EffectForce's grain test): x shifted down by
// hz, summed over a few periods, and the phase that turns through in 20 ms, averaged weighted by
// level. Grains of one tone at random phases are still that tone; the wander of their sum's phase
// averages out, where a single long spectrum's peak is speckled by it.
double toneHz(const Buf& x, double hz, size_t from, size_t to) {
    using cd = std::complex<double>;
    const double w = 2.0 * kPi * hz / kSec;
    const size_t avg = std::max<size_t>(441, static_cast<size_t>(4.0 * kSec / hz)), lag = 882;
    std::vector<cd> sum(to - from + 1, cd(0.0, 0.0));
    for (size_t i = from; i < to; ++i)
        sum[i - from + 1] = sum[i - from] + static_cast<double>(x[i]) * std::polar(1.0, -w * static_cast<double>(i));
    cd c(0.0, 0.0);
    for (size_t i = 0; i + avg + lag < sum.size(); i += 8)
        c += std::conj(sum[i + avg] - sum[i]) * (sum[i + lag + avg] - sum[i + lag]);
    return hz + std::arg(c) / (2.0 * kPi * static_cast<double>(lag) / kSec);
}

// 1. A source: the level, the seam, the slower levels, the guard, determinism.
void testBuildSource() {
    std::printf("== weather: building a source\n");
    const Buf s = sine(1000.0, 3 * kSec, 0.5f);
    const auto sb = source(s);
    CHECK(sb && sb->src.ready());
    if (!sb) return;
    const af::GrainSource& g = sb->src;
    CHECK(g.frames % 4 == 0 && g.origin == 0);
    CHECK(g.frames == 3 * kSec - 11028);   // 3 s less the fade, rounded down to a multiple of 4
    CHECK(sb->bytes() == sizeof(int16_t) * 2 * static_cast<size_t>((g.frames + 256) + (g.frames / 2 + 256) + (g.frames / 4 + 256)));
    const Buf l0 = levelOf(g, 0, 0), r0 = levelOf(g, 0, 1);
    const double lvl = db(std::sqrt(0.5 * (rms(l0) * rms(l0) + rms(r0) * rms(r0))));
    std::printf("  1 kHz: %.3f dBFS RMS\n", lvl);
    CHECK(std::fabs(lvl - af::kSourceRmsDb) < 0.1);
    // The seam: the last frame runs on into the first as the sine did.
    const float own = maxStep(l0), seam = std::fabs(l0.front() - l0.back());
    std::printf("  seam step %.4f, the sine's largest %.4f\n", seam, own);
    CHECK(seam <= 1.5f * own);
    // Level 1 (22.05 kHz) and 2 (11.025 kHz) hold the sine at its level; a 1 kHz component there
    // reads as 2 and 4 kHz would at 44.1 kHz.
    for (int level = 1; level <= 2; ++level) {
        const Buf x = levelOf(g, level, 0);
        const size_t skip = x.size() / 4;   // past the loop's crossfade, where the sine swells
        const double d = db(rms(x, skip) / rms(l0, skip << level));
        const double m = db(magnitude(x, 1000.0 * (1 << level), skip) / magnitude(l0, 1000.0, skip << level));
        std::printf("  level %d: RMS %+.4f dB, the 1 kHz tone %+.4f dB\n", level, d, m);
        CHECK(std::fabs(d) < 0.1 && std::fabs(m) < 0.1);
    }
    // The guard: every level's start again past its end.
    bool guard = true;
    for (int level = 0; level < 3; ++level) {
        const int n = g.frames >> level;
        for (int i = 0; i < 2 * af::GrainSource::kGuard; ++i) guard &= g.level[level][2 * n + i] == g.level[level][i];
    }
    CHECK(guard);
    // 21 kHz is gone from level 1 (it would fold to 1.05 kHz).
    const auto hi = source(sine(21000.0, 3 * kSec, 0.5f));
    const double down = db(rms(levelOf(hi->src, 1, 0)) / rms(levelOf(hi->src, 0, 0)));
    std::printf("  21 kHz in level 1: %.1f dB\n", down);
    CHECK(down <= -80.0);
    // The same signal, the same source.
    const auto again = source(s);
    bool equal = true;
    for (int level = 0; level < 3; ++level) equal &= again->data[level] == sb->data[level];
    CHECK(equal);
    // Odd lengths and odd input: frames a multiple of 4, finite, a NaN counted as silence.
    for (int n : {1, 3, 7, 9, 100, 4411, 22051}) {
        Buf x = whiteNoise(n, 0.5f, static_cast<uint32_t>(n));
        x[0] = std::nanf("");
        const auto o = source(x);
        CHECK(o && o->src.frames >= 4 && o->src.frames % 4 == 0 && o->src.frames <= std::max(4, n));
    }
    CHECK(af::buildSource(s.data(), s.data(), 0) == nullptr);
    const Buf quiet(1000, 0.0f);
    const auto q = source(quiet);
    CHECK(q && q->src.ready() && rms(levelOf(q->src, 0, 0)) == 0.0);
}

// 2. Without a source nothing is added, not even -0.
void testSilenceWithoutSource() {
    std::printf("== weather: silence without a source\n");
    Weather w;
    w.seed(3);
    w.set(loud(), af::HarmonyPatch{});
    w.gate(true);
    const size_t n = 4 * kSec;
    Buf L(n), R(n), SL(n), SR(n);
    for (size_t i = 0; i < n; ++i) L[i] = R[i] = SL[i] = SR[i] = i % 7 ? 0.25f * static_cast<float>(i % 7) : -0.0f;
    const Buf L0 = L, R0 = R, SL0 = SL, SR0 = SR;
    const auto over = [&](const af::GrainSource* s) {   // the whole buffer, in MPC's blocks
        for (size_t i = 0; i < n; i += kBlk)
            w.render(s, 0.0f, &L[i], &R[i], &SL[i], &SR[i], 0.5f, static_cast<int>(std::min<size_t>(kBlk, n - i)));
    };
    over(nullptr);
    CHECK(same(L, L0) && same(R, R0) && same(SL, SL0) && same(SR, SR0));
    CHECK(w.grainsOn() == 0);
    // A source with nothing ready in it is none.
    af::GrainSource empty;
    over(&empty);
    CHECK(same(L, L0) && same(R, R0));
    // So is one without its slower levels, or of frames not a multiple of 4: there is no fallback
    // (level 0 read at up to 4 frames a sample would alias, and outrun a source change's copies).
    const auto sb = source(whiteNoise(kSec, 0.5f, 5));
    af::GrainSource only0 = sb->src, odd = sb->src;
    only0.level[1] = only0.level[2] = nullptr;
    odd.frames -= 2;
    CHECK(sb->src.ready() && !only0.ready() && !odd.ready());
    af::WeatherPatch p = loud();
    p.pitch = 24.0f;   // (level 2's reads)
    w.set(p, af::HarmonyPatch{});
    for (const af::GrainSource* s : {&only0, &odd}) {
        over(s);
        CHECK(same(L, L0) && same(R, R0) && w.grainsOn() == 0);
    }
}

// 3. Pitch: +7 lands on the fifth within 3 cents; +24 on two octaves up with nothing else under
// 15 kHz above -70 dB (level 2's read: no aliases).
void testPitch() {
    std::printf("== weather: pitch\n");
    const auto sb = source(sine(440.0, 6 * kSec, 0.5f));
    for (float semis : {7.0f, -12.0f, 24.0f}) {
        Weather w;
        w.seed(5);
        af::WeatherPatch p = loud();
        p.spray = 0.0f;
        p.pitch = semis;
        w.set(p, af::HarmonyPatch{});
        w.gate(true);
        const Out o = play(w, &sb->src, 10 * kSec);
        const double want = 440.0 * std::pow(2.0, semis / 12.0);
        const double f = toneHz(o.L, want, at(3.0), at(10.0));
        std::printf("  %+.0f: %.3f Hz (%+.2f cents)\n", semis, f, cents(f, want));
        CHECK(std::fabs(cents(f, want)) < 3.0);
        if (semis == 24.0f) {
            const Spectrum sp(o.L, at(3.0), 1 << 17);
            const double top = sp.at(want);
            double worst = 0.0, worstHz = 0.0;
            for (size_t k = 1; k < sp.amp.size(); ++k) {
                const double hz = static_cast<double>(k) * sp.binHz;
                if (hz > 15000.0) break;
                if (std::fabs(hz - want) < 150.0) continue;   // the tone and its windows' sidebands
                if (sp.amp[k] > worst) {
                    worst = sp.amp[k];
                    worstHz = hz;
                }
            }
            std::printf("  +24: the most outside the tone %.1f dB (at %.0f Hz)\n", db(worst / top), worstHz);
            CHECK(db(worst / top) < -70.0);
        }
    }
}

// 4. To Key: each grain's transposition is on the chord's pitch classes (the source on C), within
// -6..+5 of Pitch; Scale draws the key's scale; a high Pitch folds down an octave past +24.
struct Spawns {
    std::vector<float> semis;
};
void record(void* ctx, float semis) { static_cast<Spawns*>(ctx)->semis.push_back(semis); }

void testToKey() {
    std::printf("== weather: To Key\n");
    const auto sb = source(sine(261.63, 4 * kSec, 0.5f));
    struct Case {
        int toKey;
        uint16_t chord;
        float pitch;
        int key, scale;
        uint16_t want;   // the pitch classes allowed
    };
    const uint16_t dMaj = (1 << 2) | (1 << 6) | (1 << 9);
    const uint16_t ePent = (1 << 4) | (1 << 6) | (1 << 8) | (1 << 11) | (1 << 1);   // E major pentatonic
    for (const Case c : {Case{af::TK_CHORD, dMaj, 0.0f, 0, af::SC_MAJOR, dMaj},
                         Case{af::TK_CHORD, dMaj, -9.0f, 0, af::SC_MAJOR, dMaj},
                         Case{af::TK_CHORD, 0, 0.0f, 4, af::SC_MAJ_PENT, ePent},   // no chord: the scale's
                         Case{af::TK_SCALE, dMaj, 12.4f, 4, af::SC_MAJ_PENT, ePent},
                         Case{af::TK_CHORD, dMaj, 24.0f, 0, af::SC_MAJOR, dMaj}}) {
        Weather w;
        w.seed(7);
        Spawns s;
        w.setSpawnHook(record, &s);
        af::WeatherPatch p = loud();
        p.toKey = c.toKey;
        p.pitch = c.pitch;
        p.spray = 1.0f;   // To Key never detunes, whatever Spray
        p.sizeS = 0.05f;
        p.grains = 16;
        af::HarmonyPatch h;
        h.key = c.key;
        h.scale = c.scale;
        w.set(p, h);
        w.setChord(c.chord);
        w.gate(true);
        play(w, &sb->src, 3 * kSec);
        bool onKey = true, near = true;
        uint16_t seen = 0;
        const int base = static_cast<int>(std::lround(c.pitch));
        for (float t : s.semis) {
            const int ti = static_cast<int>(std::lround(t));
            const int pc = ((ti % 12) + 12) % 12;
            onKey &= std::fabs(t - static_cast<float>(ti)) < 1e-6f && ((c.want >> pc) & 1);
            seen |= static_cast<uint16_t>(1 << pc);
            // Within -6..+5 of Pitch, or (past +24) an octave under that.
            const int unfolded = ti + 12 > 24 && ti < base - 6 ? ti + 12 : ti;
            near &= unfolded >= base - 6 && unfolded <= base + 5;
        }
        std::printf("  to key %d, pitch %+.1f: %zu grains, classes %03x of %03x\n", c.toKey, c.pitch, s.semis.size(), seen, c.want);
        CHECK(s.semis.size() > 300);
        CHECK(onKey && near && seen == c.want);
    }
}

// 5. The level holds across grain counts: uncorrelated grains at 1 / sqrt(0.375 grains).
void testLevel() {
    std::printf("== weather: level across grains\n");
    // Mono: the pans then keep the power (a hard-panned grain of uncorrelated channels keeps half
    // of it, EffectForce's pan law, which narrows a grain's own stereo as it moves out).
    const auto sb = source(whiteNoise(10 * kSec, 0.5f, 11));
    double lo = 1e9, hi = -1e9;
    for (int grains : {1, 4, 16}) {
        Weather w;
        w.seed(9);
        af::WeatherPatch p = loud();
        p.grains = grains;
        p.spray = 0.6f;
        w.set(p, af::HarmonyPatch{});
        w.gate(true);
        const Out o = play(w, &sb->src, 24 * kSec);
        const double lvl = dbRms(o, at(4.0));
        std::printf("  %2d grains: %.2f dBFS RMS\n", grains, lvl);
        lo = std::min(lo, lvl);
        hi = std::max(hi, lvl);
    }
    CHECK(hi - lo <= 1.5);
    CHECK(std::fabs(0.5 * (hi + lo) - af::kSourceRmsDb) <= 1.5);   // at the source's own level
}

// 50 ms RMS windows of x[from, to) (11 periods of 220 Hz exactly): the largest over the smallest, in dB.
double envelopeSpread(const Buf& x, size_t from, size_t to) {
    const size_t win = 2205;
    double lo = 1e9, hi = 0.0;
    for (size_t i = from; i + win <= to; i += win / 2) {
        const double r = rms(x, i, i + win);
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    return db(hi / lo);
}

// 6. Stream: consecutive grains join without a dip (a tone stays a tone at one level), and
// transposed it keeps the level it has untransposed (on noise).
void testStream() {
    std::printf("== weather: Stream\n");
    const auto tone = source(sine(220.0, 10 * kSec, 0.5f));
    Weather w;
    w.seed(13);
    af::WeatherPatch p = loud(af::WM_STREAM);
    p.sizeS = 0.5f;
    p.drift = 0.0f;
    p.position = 0.1f;
    w.set(p, af::HarmonyPatch{});
    w.gate(true);
    const Out o = play(w, &tone->src, 7 * kSec);
    // From the gate's top on, for 4 s: the read point starts a second in and never reaches the
    // loop's end, where its own crossfade (equal power, so a tone swells there) is.
    const double spread = envelopeSpread(o.L, at(2.5), at(6.5));
    const double lvl = db(rms(o.L, at(2.5), at(6.5)));
    std::printf("  220 Hz, size 0.5 s: the envelope spreads %.3f dB, at %.2f dBFS RMS\n", spread, lvl);
    CHECK(spread < 1.0);
    CHECK(std::fabs(lvl - af::kSourceRmsDb) < 0.5);   // the source itself

    // Noise under 4 kHz, so neither the reads' interpolation nor the slower levels take any of it:
    // drifting (Init's Drift) or transposed, neighbours are uncorrelated and play at sqrt(4/3);
    // still and at rate 1, they join and play at 1. Each at the source's own level.
    const auto noise = source(lowNoise(10 * kSec, 21), lowNoise(10 * kSec, 22));
    for (float drift : {0.2f, 0.0f})
        for (float semis : {0.0f, 7.0f, -5.0f}) {
            Weather s;
            s.seed(17);
            af::WeatherPatch q = loud(af::WM_STREAM);
            q.pitch = semis;
            q.drift = drift;
            s.set(q, af::HarmonyPatch{});
            s.gate(true);
            const Out n = play(s, &noise->src, 20 * kSec);
            const double r = dbRms(n, at(3.0));
            std::printf("  noise, drift %.1f, %+.0f: %.2f dBFS RMS\n", drift, semis, r);
            CHECK(std::fabs(r - af::kSourceRmsDb) < 0.75);
        }
}

// The cloud the gate, Duck and the source checks listen to: noise, 16 grains.
af::WeatherPatch dense() {
    af::WeatherPatch p = loud();
    p.grains = 16;
    p.spray = 0.6f;
    return p;
}

// 7. The gate: half way through its fade in, -30 dB of where it settles; closed, silent within
// kGateS + 0.1 s, and nothing more is added.
void testGate() {
    std::printf("== weather: the gate\n");
    const auto sb = source(whiteNoise(10 * kSec, 0.5f, 31), whiteNoise(10 * kSec, 0.5f, 32));
    Weather w;
    w.seed(19);
    w.set(dense(), af::HarmonyPatch{});
    CHECK(!w.audible());
    w.gate(true);
    CHECK(w.audible());
    const Out o = play(w, &sb->src, 8 * kSec);
    const double half = db(rms(o.L, at(0.95), at(1.05)) / rms(o.L, at(3.0), at(8.0)));
    std::printf("  at kGateS / 2: %.2f dB\n", half);
    CHECK(std::fabs(half + 30.0) <= 3.0);
    w.gate(false);
    int blocks = 0;
    while (w.audible() && blocks < 10 * kSec / kBlk) {
        play(w, &sb->src, kBlk);
        ++blocks;
    }
    const double closed = static_cast<double>(blocks * kBlk) / kSec;
    std::printf("  closed after %.3f s\n", closed);
    CHECK(!w.audible() && closed <= Weather::kGateS + 0.1);
    CHECK(w.grainsOn() == 0);
    const Out after = play(w, &sb->src, kSec);
    CHECK(peak(after.L) == 0.0f && peak(after.R) == 0.0f && peak(after.sendL) == 0.0f);
    // A level of 0 or a mute: nothing to hear, so nothing to render.
    w.gate(true);
    af::WeatherPatch p = dense();
    p.level = 0.0f;
    w.set(p, af::HarmonyPatch{});
    CHECK(!w.audible());
    p.level = 1.0f;
    p.mute = true;
    w.set(p, af::HarmonyPatch{});
    CHECK(!w.audible());
}

// 8. Duck: under Bloom at -6 dBFS for 2 s Weather is 12 dB down or more; 3 s after it stops, back
// within 1 dB. The same seed undocked is the reference, sample for sample.
void testDuck() {
    std::printf("== weather: Duck\n");
    const auto sb = source(whiteNoise(10 * kSec, 0.5f, 41), whiteNoise(10 * kSec, 0.5f, 42));
    Weather a, b;
    a.seed(23);
    b.seed(23);
    af::WeatherPatch p = dense();
    b.set(p, af::HarmonyPatch{});
    p.duck = 1.0f;
    a.set(p, af::HarmonyPatch{});
    a.gate(true);
    b.gate(true);
    const Out a0 = play(a, &sb->src, 3 * kSec), b0 = play(b, &sb->src, 3 * kSec);
    CHECK(same(a0.L, b0.L));   // nothing to duck under: the same
    const Out a1 = play(a, &sb->src, 2 * kSec, 0.5f), b1 = play(b, &sb->src, 2 * kSec, 0.5f);
    const Out a2 = play(a, &sb->src, 3 * kSec), b2 = play(b, &sb->src, 3 * kSec);
    const double under = db(rms(a1.L, at(1.0)) / rms(b1.L, at(1.0)));
    const double back = db(rms(a2.L, at(2.8)) / rms(b2.L, at(2.8)));
    std::printf("  under -6 dBFS: %.2f dB; 3 s after: %.3f dB\n", under, back);
    CHECK(under <= -12.0);
    CHECK(std::fabs(back) <= 1.0);
    CHECK(maxStep(a1.L) <= 1.2f * maxStep(b1.L));   // it glides
}

// 9. A source change mid-cloud: every grain fades out over 20 ms reading a copy of the old
// source (scribbled over just after the change here, as a freed one could be), the new grains
// fade in; nothing steps more than the clouds themselves do. And to no source: a fade, then silence.
void testSourceChange() {
    std::printf("== weather: a source change\n");
    const auto s1 = source(sine(300.0, 6 * kSec, 0.5f)), s2 = source(sine(500.0, 6 * kSec, 0.5f), sine(510.0, 6 * kSec, 0.5f));
    af::WeatherPatch p = loud();
    p.sizeS = 0.1f;
    p.grains = 6;
    // The clouds' own largest steps, each on its own.
    float own = 0.0f;
    for (const af::SourceBuffer* s : {s1.get(), s2.get()}) {
        Weather w;
        w.seed(29);
        w.set(p, af::HarmonyPatch{});
        w.gate(true);
        const Out o = play(w, &s->src, 6 * kSec);
        own = std::max({own, maxStep(o.L, at(2.0)), maxStep(o.R, at(2.0))});
    }
    // The change, twice over: one keeps the old source intact, the other scribbles over it.
    std::vector<int16_t> keep[3];
    for (int l = 0; l < 3; ++l) keep[l] = s1->data[l];
    Weather a, b;
    for (Weather* w : {&a, &b}) {
        w->seed(29);
        w->set(p, af::HarmonyPatch{});
        w->gate(true);
    }
    Out oa = play(a, &s1->src, 3 * kSec), ob = play(b, &s1->src, 3 * kSec);
    const int on = a.grainsOn();
    oa = cat(oa, play(a, &s2->src, kBlk));
    ob = cat(ob, play(b, &s2->src, kBlk));
    for (int l = 0; l < 3; ++l) std::fill(s1->data[l].begin(), s1->data[l].end(), int16_t{32767});
    oa = cat(oa, play(a, &s2->src, kSec));
    for (int l = 0; l < 3; ++l) s1->data[l] = keep[l];
    ob = cat(ob, play(b, &s2->src, kSec));
    CHECK(same(oa.L, ob.L) && same(oa.R, ob.R));
    const float step = std::max(maxStep(oa.L, at(2.5), at(3.5)), maxStep(oa.R, at(2.5), at(3.5)));
    std::printf("  %d grains change sources: largest step %.4f, the clouds' own %.4f\n", on, step, own);
    CHECK(on > 2);
    CHECK(step <= own);
    // Back to s1 and then to none: the grains fade within 20 ms and a block, then exact silence.
    const Out back = play(a, &s1->src, kSec);
    const Out none = play(a, nullptr, kSec);
    const float stepNone = std::max(maxStep(none.L), maxStep(back.L, at(0.5)));
    size_t last = none.L.size();
    while (last > 0 && none.L[last - 1] == 0.0f && none.R[last - 1] == 0.0f) --last;
    std::printf("  to none: largest step %.4f; silent from sample %zu\n", stepNone, last);
    CHECK(stepNone <= own);
    CHECK(peak(none.L, Weather::kModeFade + kBlk) == 0.0f && peak(none.R, Weather::kModeFade + kBlk) == 0.0f);
    CHECK(a.grainsOn() == 0);
    // A mode change fades the same way.
    Weather m;
    m.seed(31);
    m.set(p, af::HarmonyPatch{});
    m.gate(true);
    Out om = play(m, &s1->src, 3 * kSec);
    af::WeatherPatch q = p;
    q.mode = af::WM_STREAM;
    m.set(q, af::HarmonyPatch{});
    om = cat(om, play(m, &s1->src, kSec));
    q.mode = af::WM_STRETCH;
    m.set(q, af::HarmonyPatch{});
    om = cat(om, play(m, &s1->src, kSec));
    const float modeStep = maxStep(om.L, at(2.5));
    std::printf("  mode changes: largest step %.4f\n", modeStep);
    CHECK(modeStep <= own);
}

// The reader goes by origin: a source rotated in its ring, its origin where its start went, plays
// the same samples (grains on level 0, so the rings match frame for frame).
void testOrigin() {
    std::printf("== weather: origin\n");
    const auto sb = source(whiteNoise(3 * kSec, 0.5f, 51), whiteNoise(3 * kSec, 0.5f, 52));
    const af::GrainSource& g = sb->src;
    const int shift = 12345;
    std::vector<int16_t> rot[3];
    af::GrainSource r = g;
    r.origin = shift * 4;   // levels 1 and 2 rotate by whole frames too
    for (int l = 0; l < 3; ++l) {
        const int n = g.frames >> l, by = (shift * 4) >> l;
        rot[l].resize(static_cast<size_t>(2 * (n + af::GrainSource::kGuard)));
        for (int i = 0; i < n + af::GrainSource::kGuard; ++i)
            for (int c = 0; c < 2; ++c)
                rot[l][static_cast<size_t>(2 * i + c)] = g.level[l][2 * (((i - by) % n + n) % n) + c];
        r.level[l] = rot[l].data();
    }
    Weather a, b;
    for (Weather* w : {&a, &b}) {
        w->seed(37);
        af::WeatherPatch p = loud();
        p.spray = 1.0f;
        p.drift = 1.0f;
        p.reverse = 0.5f;
        p.sizeS = 1.5f;
        w->set(p, af::HarmonyPatch{});
        w->gate(true);
    }
    const Out oa = play(a, &g, 6 * kSec), ob = play(b, &r, 6 * kSec);
    // The same reads; a position offset by the origin may round its fraction a float ulp apart.
    double diff = 0.0;
    for (size_t i = 0; i < oa.L.size(); ++i)
        diff = std::max({diff, std::fabs(static_cast<double>(oa.L[i]) - ob.L[i]), std::fabs(static_cast<double>(oa.R[i]) - ob.R[i])});
    std::printf("  rotated by %d frames: the largest difference %.2e\n", r.origin, diff);
    CHECK(diff < 1e-5);
    CHECK(rms(oa.L, at(3.0)) > 0.03);
}

// A grain is a Hann window: one grain over a constant source traces sin^2(pi t / L) (its window runs
// on a recurrence across steps, worked out exactly every few), short and long, forward and back.
void testWindow() {
    std::printf("== weather: the window\n");
    const int n = 3 * kSec;
    std::vector<int16_t> flat(static_cast<size_t>(2 * (n + af::GrainSource::kGuard)), int16_t{16384});
    af::GrainSource dc;
    dc.frames = n;
    for (int l = 0; l < 3; ++l) dc.level[l] = flat.data();   // a constant is the same at every level
    for (float size : {0.02f, 0.3f, 2.0f})
        for (float reverse : {0.0f, 1.0f}) {
            Weather w;
            w.seed(83);
            af::WeatherPatch p = loud();
            p.grains = 1;
            p.sizeS = size;
            p.spray = 0.0f;
            p.drift = 0.0f;
            p.width = 0.0f;
            p.reverse = reverse;
            w.set(p, af::HarmonyPatch{});
            w.gate(true);
            play(w, nullptr, 3 * kSec);   // the gate fully open, and no grain yet
            // Up to 0.6 of the grain: the next one comes at 0.7 of its length or later.
            const int len = static_cast<int>(size * kSec + 0.5f), upto = len * 6 / 10;
            const Out o = play(w, &dc, upto + kBlk);
            const double amp = 0.5 / std::sqrt(0.375);   // 16384 / 32768 at one grain's level
            double worst = 0.0;
            for (int t = 0; t < upto; ++t) {
                const double s = std::sin(kPi * t / len);
                worst = std::max(worst, std::fabs(o.L[static_cast<size_t>(t)] / amp - s * s));
            }
            std::printf("  %.2f s%s: off the Hann by %.1e at most\n", size, reverse > 0.0f ? ", backwards" : "", worst);
            CHECK(worst < 4e-6);
        }
}

// Backward grains born within a step's reach of a level's first frame: a grain starting part way
// into a step still reads whole groups of four, past its first step's last sample, and every one
// of those reads must stay inside the level (ASan watches the frames before it). The anchor 25 to
// 40 frames in, short reversed grains at rate 1, at 1.26 (+4, the fastest level 0 is read at) and
// on levels 1 and 2.
void testBackwardsNearTheStart() {
    std::printf("== weather: backward reads near a level's start\n");
    const auto sb = source(whiteNoise(kSec, 0.5f, 95), whiteNoise(kSec, 0.5f, 96));
    const double frames = static_cast<double>(sb->src.frames);
    bool finite = true;
    for (float semis : {0.0f, 4.0f, 12.0f, 24.0f})
        for (double in = 25.5; in <= 40.0; in += 1.5) {
            Weather w;
            w.seed(71);
            af::WeatherPatch p = loud();
            p.reverse = 1.0f;
            p.spray = 0.0f;
            p.drift = 0.0f;
            p.sizeS = 0.02f;
            p.grains = 16;
            p.pitch = semis;
            p.position = static_cast<float>(in / frames);
            w.set(p, af::HarmonyPatch{});
            w.gate(true);
            const Out o = play(w, &sb->src, kSec / 2);
            finite &= allFinite(o.L) && allFinite(o.R);
        }
    CHECK(finite);
}

// Mute and level glide over 10 ms, no step; silent after.
void testMute() {
    std::printf("== weather: mute\n");
    const auto sb = source(sine(300.0, 6 * kSec, 0.5f));
    Weather w;
    w.seed(41);
    af::WeatherPatch p = loud();
    w.set(p, af::HarmonyPatch{});
    w.gate(true);
    const Out o = play(w, &sb->src, 4 * kSec);
    p.mute = true;
    w.set(p, af::HarmonyPatch{});
    const Out m = play(w, &sb->src, kSec);
    const float own = maxStep(o.L, at(2.0)), step = maxStep(m.L);   // the cloud's own steps, over 2 s
    std::printf("  muting: largest step %.4f (own %.4f)\n", step, own);
    CHECK(step <= own);
    CHECK(peak(m.L, 441 + 32) == 0.0f && peak(m.sendL, 441 + 32) == 0.0f);
    CHECK(!w.audible());
    p.mute = false;
    w.set(p, af::HarmonyPatch{});
    CHECK(w.audible());
    const Out u = play(w, &sb->src, kSec);
    const float glide = maxStep(u.L, 0, at(0.03));   // where a click would be: the 10 ms glide up
    std::printf("  unmuting: largest step %.4f in its glide, then %.4f RMS\n", glide, rms(u.L, at(0.5)));
    CHECK(glide <= own && rms(u.L, at(0.5)) > 0.03);
    // The send is the dry at spaceSend (0.5 here).
    bool half = true;
    for (size_t i = 0; i < u.L.size(); ++i) half &= std::fabs(u.sendL[i] - 0.5f * u.L[i]) <= 1e-6f;
    CHECK(half);
}

// With nothing to hear (level 0 or muted, the glide down done) the engine may skip Weather, and the
// loader or a Remember may free its source meanwhile. Once audible() is false every grain has
// stopped and the source is forgotten, so the level coming back over another source reads nothing
// of the freed one (ASan would report it). Rendered on while silent, no grain starts.
void testFreedWhileSilent() {
    std::printf("== weather: a source freed while silent\n");
    const auto b = source(whiteNoise(4 * kSec, 0.5f, 93));
    for (bool mute : {false, true}) {
        auto a = source(whiteNoise(4 * kSec, 0.5f, 91), whiteNoise(4 * kSec, 0.5f, 92));
        Weather w;
        w.seed(67);
        Spawns s;
        w.setSpawnHook(record, &s);
        af::WeatherPatch p = dense();
        w.set(p, af::HarmonyPatch{});
        w.gate(true);
        play(w, &a->src, 3 * kSec);
        const int on = w.grainsOn();
        CHECK(on > 2);
        p.mute = mute;
        p.level = mute ? 1.0f : 0.0f;
        w.set(p, af::HarmonyPatch{});
        int blocks = 0;
        while (w.audible() && blocks < kSec / kBlk) {
            play(w, &a->src, kBlk);
            ++blocks;
        }
        std::printf("  %s: %d grains, silent after %d blocks with %d left\n", mute ? "muted" : "level 0", on,
                    blocks, w.grainsOn());
        CHECK(!w.audible() && w.grainsOn() == 0);
        // An engine that renders it anyway: nothing starts, nothing is added.
        const size_t spawned = s.semis.size();
        const Out idle = play(w, &a->src, kSec);
        CHECK(s.semis.size() == spawned && w.grainsOn() == 0);
        CHECK(peak(idle.L) == 0.0f && peak(idle.R) == 0.0f && peak(idle.sendL) == 0.0f);
        a.reset();   // freed while Weather is silent
        p.mute = false;
        p.level = 1.0f;
        w.set(p, af::HarmonyPatch{});
        CHECK(w.audible());
        const Out o = play(w, &b->src, 2 * kSec);
        const double lvl = dbRms(o, at(1.0));
        std::printf("  back over another source: %d grains, %.2f dBFS RMS\n", w.grainsOn(), lvl);
        CHECK(w.grainsOn() > 2);
        CHECK(std::fabs(lvl - af::kSourceRmsDb) <= 1.5);
    }
}

// The gate turned off while Weather has nothing to hear closes at once: there is nothing to fade,
// and the engine may be skipping Weather, so a fade left part done would play out when the level
// came back. Turned off while skipped, and already half way down when the level went to 0.
void testGateWhileSilent() {
    std::printf("== weather: the gate off while silent\n");
    const auto sb = source(whiteNoise(4 * kSec, 0.5f, 97));
    for (bool half : {false, true}) {
        Weather w;
        w.seed(73);
        af::WeatherPatch p = dense();
        w.set(p, af::HarmonyPatch{});
        w.gate(true);
        play(w, &sb->src, 3 * kSec);
        if (half) {   // the gate off first: half way down (-30 dB) when the level goes
            w.gate(false);
            play(w, &sb->src, kSec);
            CHECK(w.audible());
        }
        p.level = 0.0f;
        w.set(p, af::HarmonyPatch{});
        int blocks = 0;
        while (w.audible() && blocks < kSec / kBlk) {
            play(w, &sb->src, kBlk);
            ++blocks;
        }
        if (!half) w.gate(false);   // while the engine skips it: no render
        p.level = 1.0f;
        w.set(p, af::HarmonyPatch{});
        const bool shut = !w.audible();
        const Out o = play(w, &sb->src, 3 * kSec);
        std::printf("  %s, the level back: %s, peak %.4f\n", half ? "half way down" : "off while skipped",
                    shut ? "closed" : "still fading", peak(o.L));
        CHECK(shut);
        CHECK(peak(o.L) == 0.0f && peak(o.R) == 0.0f && peak(o.sendL) == 0.0f);
        // Opened again, it fades in as from silence.
        w.gate(true);
        const Out back = play(w, &sb->src, 3 * kSec);
        CHECK(w.audible() && rms(back.L, at(2.5)) > 0.03);
    }
}

// A call is cut into control steps of 32 from its start, as Ground's: a block of 128 plays what four calls of
// 32 do (the engine's pieces). Its steps are what the engine makes them, so the engine's own grid keeps
// MPC's block size out of the sound.
void testBlocks() {
    std::printf("== weather: block sizes\n");
    const auto sb = source(whiteNoise(4 * kSec, 0.5f, 61), whiteNoise(4 * kSec, 0.5f, 62));
    for (int mode = 0; mode < af::WM_COUNT; ++mode) {
        Out o[2];
        for (int i = 0; i < 2; ++i) {
            Weather w;
            w.seed(43);
            af::WeatherPatch p = dense();
            p.mode = mode;
            p.pitch = 5.0f;
            p.reverse = 0.3f;
            p.tilt = 0.5f;
            p.hpHz = 300.0f;
            w.set(p, af::HarmonyPatch{});
            w.gate(true);
            o[i] = play(w, &sb->src, 4 * kSec, 0.0f, i == 0 ? kBlk : af::kChunk);
        }
        CHECK(same(o[0].L, o[1].L) && same(o[0].R, o[1].R) && same(o[0].sendL, o[1].sendL));
    }
}

// 10. The same seed plays the same samples; every mode at the extremes for 60 s stays finite and
// under 4.
void testDeterminismAndStability() {
    std::printf("== weather: determinism and stability\n");
    const auto sb = source(whiteNoise(8 * kSec, 0.5f, 71), whiteNoise(8 * kSec, 0.5f, 72));
    const auto tone = source(sine(110.0, 5 * kSec, 0.5f));
    {
        Out o[3];
        for (int i = 0; i < 3; ++i) {
            Weather w;
            w.seed(i < 2 ? 47 : 48);
            w.set(dense(), af::HarmonyPatch{});
            w.gate(true);
            o[i] = play(w, &sb->src, 3 * kSec);
        }
        CHECK(same(o[0].L, o[1].L) && same(o[0].R, o[1].R));
        CHECK(!same(o[0].L, o[2].L));
        // reset() starts afresh: the same samples again.
        Weather w;
        w.seed(47);
        w.set(dense(), af::HarmonyPatch{});
        w.gate(true);
        play(w, &sb->src, kSec);
        w.reset();
        CHECK(!w.audible() && w.grainsOn() == 0);
        w.set(dense(), af::HarmonyPatch{});
        w.gate(true);
        CHECK(same(play(w, &sb->src, 3 * kSec).L, o[0].L));
    }
    // Each mode for 60 s, the extremes taking turns every 2.5 s (size 0.02 and 2 s, pitch -24 and
    // +24, Off and Chord), over noise and a low tone.
    float worst = 0.0f;
    bool finite = true;
    for (int mode = 0; mode < af::WM_COUNT; ++mode) {
        Weather w;
        w.seed(53);
        w.setChord(0x891);
        w.gate(true);
        for (int turn = 0; turn < 24; ++turn) {
            af::WeatherPatch p = loud(mode);
            p.grains = 16;
            p.sizeS = turn & 1 ? 2.0f : 0.02f;
            p.pitch = turn & 2 ? 24.0f : -24.0f;
            p.toKey = turn & 4 ? af::TK_CHORD : af::TK_OFF;
            p.reverse = 1.0f;
            p.spray = 1.0f;
            p.drift = 1.0f;
            p.width = 1.0f;
            p.tilt = turn & 8 ? 1.0f : -1.0f;
            p.hpHz = turn & 16 ? 2000.0f : 20.0f;
            w.set(p, af::HarmonyPatch{});
            const Out o = play(w, &(turn % 3 ? sb : tone)->src, kSec * 5 / 2, turn % 5 == 0 ? 0.3f : 0.0f);
            finite &= allFinite(o.L) && allFinite(o.R) && allFinite(o.sendL) && allFinite(o.sendR);
            worst = std::max({worst, peak(o.L), peak(o.R)});
        }
    }
    std::printf("  60 s at the extremes, every mode: peak %.3f\n", worst);
    CHECK(finite && worst <= 4.0f);
    // Odd parameters and input: NaN everywhere, a wild duck peak.
    Weather w;
    af::WeatherPatch p;
    const float nan = std::nanf("");
    p.level = p.position = p.drift = p.spray = p.sizeS = p.pitch = p.reverse = p.width = p.tilt = p.hpHz = p.duck = nan;
    p.mode = 99;
    p.toKey = -3;
    p.grains = 1000;
    w.set(p, af::HarmonyPatch{});
    w.gate(true);
    const Out o = play(w, &sb->src, 2 * kSec, nan);
    CHECK(allFinite(o.L) && allFinite(o.R));
    p = dense();
    p.duck = 1.0f;
    w.set(p, af::HarmonyPatch{});
    const Out o2 = play(w, &sb->src, 2 * kSec, 1e30f);
    CHECK(allFinite(o2.L) && allFinite(o2.R));
    const Out o3 = play(w, &sb->src, 2 * kSec, std::numeric_limits<float>::infinity());
    CHECK(allFinite(o3.L) && allFinite(o3.R));
}

#if WEATHER_COUNTS_ALLOCS
// ASan's allocator calls these for every allocation; only the test's own thread counts.
thread_local bool t_counting = false;
int g_allocs = 0;
void onMalloc(const volatile void*, size_t) {
    if (t_counting) ++g_allocs;
}
void onFree(const volatile void*) {}
#endif

// Nothing in set, setChord, gate, render or reset allocates.
void testNoAllocation() {
    std::printf("== weather: no allocation\n");
#if WEATHER_COUNTS_ALLOCS
    static const bool hooked = __sanitizer_install_malloc_and_free_hooks(onMalloc, onFree) != 0;
    CHECK(hooked);
    const auto s1 = source(whiteNoise(2 * kSec, 0.5f, 81)), s2 = source(sine(200.0, 2 * kSec, 0.5f));
    Weather w;
    w.seed(59);
    float L[kBlk] = {}, R[kBlk] = {}, SL[kBlk] = {}, SR[kBlk] = {};
    t_counting = true;
    g_allocs = 0;
    for (int mode = 0; mode < af::WM_COUNT; ++mode) {
        af::WeatherPatch p = dense();
        p.mode = mode;
        p.toKey = af::TK_CHORD;
        p.tilt = -0.5f;
        p.hpHz = 200.0f;
        p.duck = 0.5f;
        w.set(p, af::HarmonyPatch{});
        w.setChord(0x91);
        w.gate(true);
        for (int i = 0; i < 300; ++i) w.render(i < 150 ? &s1->src : &s2->src, 0.1f, L, R, SL, SR, 0.5f, i % 3 ? kBlk : 77);
        w.render(nullptr, 0.0f, L, R, SL, SR, 0.5f, kBlk);
    }
    w.gate(false);
    for (int i = 0; i < 800; ++i) w.render(&s1->src, 0.0f, L, R, SL, SR, 0.5f, kBlk);
    w.reset();
    t_counting = false;
    std::printf("  %d allocations\n", g_allocs);
    CHECK(g_allocs == 0);
#else
    std::printf("  (counted under ASan: make test)\n");
#endif
}

} // namespace

void weatherTests() {
    testBuildSource();
    testSilenceWithoutSource();
    testPitch();
    testToKey();
    testLevel();
    testStream();
    testGate();
    testDuck();
    testSourceChange();
    testOrigin();
    testWindow();
    testBackwardsNearTheStart();
    testMute();
    testFreedWhileSilent();
    testGateWhileSilent();
    testBlocks();
    testDeterminismAndStability();
    testNoAllocation();
}

} // namespace aft
