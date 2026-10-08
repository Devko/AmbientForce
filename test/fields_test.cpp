// Weather's fields (dsp/fields.h) and Memory (dsp/memory.h) on their own. The fields: each renders
// in time, the same twice, at -20 dBFS RMS, no step at its loop's seam on any level, its channels
// decorrelated; Night's crickets on C and out of the loop's crossfade, Surf's two swells. Memory:
// FrameDecimator against StereoDecimator; what it records (the sine 6 dB down at level 0, the slower
// levels holding it), a full ring read back from its oldest frame (its origin a multiple of 4 and
// not), the levels aligned at every origin, the guard, the seam a dip at every level, Remember's
// rules (the gap, the least recorded, the frames rounded down to a multiple of 4, pinning, the
// generation, the old ring no longer a source, the recording afresh after it), reset() keeping
// what is remembered, the same bits whatever the pieces, odd input, Weather playing a remembered
// ring across its end (at every level, against the same source unrolled), pins from another
// thread, nothing allocating. Needs no plugin: make test-module M=fields.
#include "check.h"
#include "signal.h"
#include "../dsp/fields.h"
#include "../dsp/memory.h"
#include "../dsp/weather.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace aft {
namespace {

using af::Memory;
constexpr int kSec = 44100;
constexpr int kBlk = 128;
constexpr int kFieldFrames = static_cast<int>(af::kFieldS * 44100.0f);

// --- sources --------------------------------------------------------------------------------------

// A level's sample as a float of 16-bit full scale (no gain), frame f of its ring.
float raw(const af::GrainSource& s, int level, int f, int ch) { return s.level[level][2 * f + ch] / 32768.0f; }

// A level of a source, one channel, in the source's order (from origin, wrapping at its frames), as
// raw 16-bit values (1: full scale).
Buf inOrder(const af::GrainSource& s, int level, int ch) {
    const int n = s.frames >> level, o = s.origin >> level;
    Buf x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = raw(s, level, (o + i) % n, ch);
    return x;
}

bool sameSource(const af::GrainSource& a, const af::GrainSource& b) {
    if (a.frames != b.frames || a.origin != b.origin || a.gain != b.gain) return false;
    for (int k = 0; k < af::GrainSource::kLevels; ++k) {
        const size_t n = 2 * static_cast<size_t>((a.frames >> k) + af::GrainSource::kGuard);
        if (std::memcmp(a.level[k], b.level[k], n * sizeof(int16_t)) != 0) return false;
    }
    return true;
}

// The level's 99.9th-percentile step between neighbouring frames (inside the loop), one channel.
float stepP999(const Buf& x) {
    std::vector<float> d;
    d.reserve(x.size());
    for (size_t i = 1; i < x.size(); ++i) d.push_back(std::fabs(x[i] - x[i - 1]));
    std::sort(d.begin(), d.end());
    return d[static_cast<size_t>(0.999 * static_cast<double>(d.size() - 1))];
}

double correlation(const Buf& a, const Buf& b) {
    double ab = 0.0, aa = 0.0, bb = 0.0, ma = 0.0, mb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<double>(a.size());
    mb /= static_cast<double>(b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        const double x = a[i] - ma, y = b[i] - mb;
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }
    return ab / std::sqrt(aa * bb);
}

// --- fields ---------------------------------------------------------------------------------------

// Each field: rendered in under 1 s on x86 (under ASan in make test), the same twice, ready, 12 s,
// at -20 dBFS RMS +-0.5 dB, nothing clipped, no step at its loop's seam (the last frame to the
// first) above its own 99.9th-percentile step at any level, its channels' correlation under 0.9.
void testFields() {
    std::printf("== fields: each field\n");
    CHECK(!af::renderField(-1) && !af::renderField(af::FD_COUNT));
    for (int id = 0; id < af::FD_COUNT; ++id) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::unique_ptr<af::SourceBuffer> sb = af::renderField(id);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(sb && sb->src.ready());
        if (!sb || !sb->src.ready()) continue;
        const af::GrainSource& s = sb->src;
        CHECK(s.frames == kFieldFrames && s.origin == 0);
#if defined(__x86_64__) || defined(__i386__)
        CHECK(secs < 1.0);
#endif
        const std::unique_ptr<af::SourceBuffer> again = af::renderField(id);
        CHECK(again && sameSource(s, again->src));

        const Buf L = inOrder(s, 0, 0), R = inOrder(s, 0, 1);
        double sum = 0.0;
        float top = 0.0f;
        for (size_t i = 0; i < L.size(); ++i) {
            sum += static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i];
            top = std::max(top, std::max(std::fabs(L[i]), std::fabs(R[i])));
        }
        const double level = db(std::sqrt(sum / (2.0 * static_cast<double>(L.size()))) * s.gain * 32768.0);
        CHECK(std::fabs(level + 20.0) <= 0.5);
        CHECK(top < 32767.0f / 32768.0f && sum > 0.0);   // (16-bit samples are finite:) nothing clipped, not silent
        const double corr = correlation(L, R);
        CHECK(corr < 0.9);
        // The seam at every level: the loop's last frame against its first, per channel.
        double worst = 0.0;
        for (int k = 0; k < af::GrainSource::kLevels; ++k)
            for (int ch = 0; ch < 2; ++ch) {
                const Buf x = inOrder(s, k, ch);
                const float seam = std::fabs(x.front() - x.back()), p = stepP999(x);
                CHECK(seam <= p);
                worst = std::max(worst, static_cast<double>(seam / p));
            }
        std::printf("  %-12s %5.0f ms  %6.2f dBFS RMS  peak %6.2f dBFS  L/R %5.2f  seam %.2f of the p99.9 step\n",
                    af::kFieldNames[id], 1000.0 * secs, level, db(top), corr, worst);
    }
}

// Night's crickets on C: the largest peak above 2 kHz within 20 cents of C8 (4186 Hz).
void testNightOnC() {
    std::printf("== fields: Night on C\n");
    const auto sb = af::renderField(af::FD_NIGHT);
    CHECK(sb != nullptr);
    if (!sb) return;
    const Buf L = inOrder(sb->src, 0, 0), R = inOrder(sb->src, 0, 1);
    Buf mono(L.size());
    for (size_t i = 0; i < L.size(); ++i) mono[i] = L[i] + R[i];
    const Spectrum sp(mono, 0, size_t{1} << 19);
    size_t best = 0;
    for (size_t k = static_cast<size_t>(2000.0 / sp.binHz); k < sp.amp.size(); ++k)
        if (sp.amp[k] > sp.amp[best]) best = k;
    const double hz = static_cast<double>(best) * sp.binHz, cents = 1200.0 * std::log2(hz / 4186.009);
    std::printf("  the largest peak above 2 kHz: %.1f Hz (%+.1f cents from C8)\n", hz, cents);
    CHECK(std::fabs(cents) <= 20.0);
    // No train in the loop's crossfade (its first 0.25 s, where the signal past the loop's end
    // fades out under its start: a train there would be added to itself), against the first
    // train's (0.30 s on).
    const size_t fade = static_cast<size_t>(af::kLoopFadeS * kSec), train = static_cast<size_t>(0.30 * kSec);
    const double inFade = magnitude(mono, 4186.009, 0, fade), inTrain = magnitude(mono, 4186.009, train, train + fade);
    std::printf("  C8 in the crossfade %.1f dB under the first train's\n", db(inTrain / inFade));
    CHECK(db(inTrain / inFade) >= 40.0);
}

// Surf's swells: its 0.5 s RMS envelope over the 12 s rises at least 6 dB over its minima twice and
// falls 6 dB from its maxima twice (going round the loop), the two crests 6 s apart (+-1 s).
void testSurfSwells() {
    std::printf("== fields: Surf's swells\n");
    const auto sb = af::renderField(af::FD_SURF);
    CHECK(sb != nullptr);
    if (!sb) return;
    const Buf L = inOrder(sb->src, 0, 0), R = inOrder(sb->src, 0, 1);
    const int win = kSec / 2, n = static_cast<int>(L.size()) / win;
    std::vector<double> env(static_cast<size_t>(n));
    std::printf("  dB:");
    for (int w = 0; w < n; ++w) {
        double s = 0.0;
        for (int i = w * win; i < (w + 1) * win; ++i) s += static_cast<double>(L[static_cast<size_t>(i)]) * L[static_cast<size_t>(i)] + static_cast<double>(R[static_cast<size_t>(i)]) * R[static_cast<size_t>(i)];
        env[static_cast<size_t>(w)] = db(std::sqrt(s / (2.0 * win)));
        std::printf(" %.0f", env[static_cast<size_t>(w)]);
    }
    std::printf("\n");
    // Round the loop from its lowest window: a crest counts once the envelope has risen 6 dB over
    // the lowest point before it and then fallen 6 dB from it.
    const int start = static_cast<int>(std::min_element(env.begin(), env.end()) - env.begin());
    double low = env[static_cast<size_t>(start)], high = low;
    int highAt = start;
    bool risen = false;
    std::vector<int> crests;
    for (int j = 1; j <= n; ++j) {
        const int w = (start + j) % n;
        const double e = env[static_cast<size_t>(w)];
        if (!risen) {
            low = std::min(low, e);
            if (e - low >= 6.0) {
                risen = true;
                high = e;
                highAt = w;
            }
        } else {
            if (e > high) {
                high = e;
                highAt = w;
            }
            if (high - e >= 6.0) {
                crests.push_back(highAt);
                risen = false;
                low = e;
            }
        }
    }
    std::printf("  crests at windows:");
    for (int c : crests) std::printf(" %d", c);
    std::printf("\n");
    CHECK(crests.size() == 2);
    if (crests.size() == 2) {
        const int apart = std::abs(crests[1] - crests[0]);
        CHECK(std::abs(std::min(apart, n - apart) - 12) <= 2);   // 6 s of 0.5 s windows
    }
}

// --- Memory ---------------------------------------------------------------------------------------

// FrameDecimator (halfband.h, Memory's) is StereoDecimator's filter: over noise, in runs of every
// length from 1 to 13 pairs (its six at a time and what is left over), the same frames bit for bit,
// into another buffer and in place (as Memory runs it); and empty again after reset().
void testFrameDecimator() {
    std::printf("== memory: FrameDecimator is StereoDecimator's filter\n");
    const int frames = 2 * 6000;
    const Buf x = whiteNoise(2 * frames, 0.8f, 33);   // L R interleaved
    af::StereoDecimator ref;
    af::FrameDecimator fd;
    Buf a(static_cast<size_t>(frames)), b(static_cast<size_t>(frames));
    for (int p = 0; p < frames / 2; ++p) {
        const float* f = &x[static_cast<size_t>(4 * p)];
        ref.process(af::f4{f[0], f[2], f[1], f[3]}, a[static_cast<size_t>(2 * p)], a[static_cast<size_t>(2 * p + 1)]);
    }
    for (int p = 0, run = 1; p < frames / 2; p += run, run = run % 13 + 1) {
        const int n = std::min(run, frames / 2 - p);
        fd.process(&x[static_cast<size_t>(4 * p)], n, &b[static_cast<size_t>(2 * p)]);
    }
    af::FrameDecimator inPlace;
    Buf c(static_cast<size_t>(frames)), t(52);
    for (int p = 0, run = 1; p < frames / 2; p += run, run = run % 13 + 1) {
        const int n = std::min(run, frames / 2 - p);
        std::copy(&x[static_cast<size_t>(4 * p)], &x[static_cast<size_t>(4 * (p + n))], t.begin());
        inPlace.process(t.data(), n, t.data());
        std::copy(t.begin(), t.begin() + 2 * n, &c[static_cast<size_t>(2 * p)]);
    }
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) worst = std::max({worst, std::fabs(a[i] - b[i]), std::fabs(a[i] - c[i])});
    std::printf("  largest difference %.2g\n", static_cast<double>(worst));
    CHECK(worst == 0.0f);
    fd.reset();
    float out[2] = {1.0f, 1.0f};
    const float zeros[4] = {};
    fd.process(zeros, 1, out);
    CHECK(out[0] == 0.0f && out[1] == 0.0f);
}

// n samples of each channel written in pieces of `piece` (0: random pieces 1..300 from `seed`).
void feed(Memory& m, const Buf& L, const Buf& R, int piece = kBlk, uint32_t seed = 1) {
    const int n = static_cast<int>(L.size());
    for (int i = 0; i < n;) {
        int k = piece;
        if (piece <= 0) {
            seed = seed * 1664525u + 1013904223u;
            k = 1 + static_cast<int>((seed >> 8) % 300u);
        }
        k = std::min(k, n - i);
        m.write(&L[static_cast<size_t>(i)], &R[static_cast<size_t>(i)], k);
        i += k;
    }
}
void feed(Memory& m, const Buf& x, int piece = kBlk) { feed(m, x, x, piece); }

// Memory records (the plan's check 4): 3 s of a 500 Hz sine, remembered: a source of 3 s (+-4
// frames) from frame 0, its gain 2/32768, its guard right after it; level 0 the sine 6 dB down in
// 16 bit (within 0.1 dB), each channel its own; levels 1 and 2 the sine too (the same level, and
// nothing else in them).
void testRecords() {
    std::printf("== memory: records\n");
    Memory m;
    CHECK(m.remembered() == nullptr && m.fill() == 0.0f && m.generation() == 0);
    const Buf L = sine(500.0, 3 * kSec, 0.5f), R = sine(500.0, 3 * kSec, 0.25f, 1.0);
    feed(m, L, R);
    CHECK(std::fabs(m.fill() - 3.0f / 16.0f) < 1e-6f);
    CHECK(m.remember());
    const af::GrainSource* s = m.remembered();
    CHECK(s && s->ready());
    if (!s || !s->ready()) return;
    CHECK(std::abs(s->frames - 3 * kSec) <= 4 && s->origin == 0 && s->gain == 2.0f / 32768.0f);
    CHECK(m.generation() == 1 && m.fill() == 0.0f);
    // The guard right after the frames it holds (Weather wraps there), at every level.
    for (int k = 0; k < af::GrainSource::kLevels; ++k) {
        const int len = s->frames >> k;
        CHECK(std::memcmp(s->level[k] + 2 * len, s->level[k], sizeof(int16_t) * 2 * af::GrainSource::kGuard) == 0);
    }
    const float amp[2] = {0.25f, 0.125f};   // each channel's sine, 6 dB down
    for (int k = 0; k < af::GrainSource::kLevels; ++k)
        for (int ch = 0; ch < 2; ++ch) {
            const Buf x = inOrder(*s, k, ch);
            // Away from the seam's fades (5 ms) and level 2's start (the decimators settling).
            const size_t from = static_cast<size_t>(2000 >> k), to = x.size() - from;
            const double mag = magnitude(x, 500.0 * (1 << k), from, to), r = rms(x, from, to) * std::sqrt(2.0);
            std::printf("  level %d %s: %.3f dB, its RMS %.3f dB of the sine's\n", k, ch ? "R" : "L",
                        db(mag / amp[ch]), db(r / amp[ch]));
            CHECK(std::fabs(db(mag / amp[ch])) <= 0.1);
            CHECK(std::fabs(db(r / amp[ch])) <= 0.1);
        }
}

// A full ring (check 5): 20 s of a ramp written (in pieces of 77, crossing the ring's end anywhere),
// and 20 s and 3 frames (the origin no multiple of 4): the source is 16 s, its origin the oldest of
// the last 16 s, and level 0 read on from origin is the ramp's last 16 s exactly (but for the
// seam's fades); each channel its own; the guard at every level.
void testFullRing() {
    std::printf("== memory: a full ring\n");
    for (const int n : {20 * kSec, 20 * kSec + 3}) {
        Memory m;
        Buf L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
        const auto ramp = [](int i) { return static_cast<float>(i % 65536 - 32768) / 16384.0f; };   // every 16-bit value in turn
        for (int i = 0; i < n; ++i) {
            L[static_cast<size_t>(i)] = ramp(i);
            R[static_cast<size_t>(i)] = ramp(i + 12345);
        }
        feed(m, L, R, 77);
        CHECK(m.fill() == 1.0f);
        CHECK(m.remember());
        const af::GrainSource* s = m.remembered();
        CHECK(s && s->ready());
        if (!s) continue;
        CHECK(s->frames == Memory::kFrames && s->origin == n % Memory::kFrames);
        int wrong = 0;
        for (int j = Memory::kSeamFade; j < Memory::kFrames - Memory::kSeamFade; ++j) {
            const int f = (s->origin + j) % Memory::kFrames, i = n - Memory::kFrames + j;
            if (s->level[0][2 * f] != i % 65536 - 32768 || s->level[0][2 * f + 1] != (i + 12345) % 65536 - 32768) ++wrong;
        }
        std::printf("  origin %d, %d frames out of place\n", s->origin, wrong);
        CHECK(wrong == 0);
        // The guard: the ring's first frames again past its end, at every level.
        for (int k = 0; k < af::GrainSource::kLevels; ++k) {
            const int len = s->frames >> k;
            CHECK(std::memcmp(s->level[k] + 2 * len, s->level[k], sizeof(int16_t) * 2 * af::GrainSource::kGuard) == 0);
        }
    }
}

// The seam (check 6), full (its origin a multiple of 4 and not) and not yet full: at every level,
// across the seam (newest frame against oldest) the largest step is no larger than the sine's own
// elsewhere (the 5 ms fades: a dip, never a step), and the two frames meeting there are silent.
void testSeam() {
    std::printf("== memory: the seam\n");
    for (const int frames : {3 * kSec, 20 * kSec, 20 * kSec + 3}) {
        Memory m;
        const float a = 0.6f;
        feed(m, sine(441.7, frames, a), 100);
        CHECK(m.remember());
        const af::GrainSource* s = m.remembered();
        if (!s) continue;
        for (int k = 0; k < af::GrainSource::kLevels; ++k)
            for (int ch = 0; ch < 2; ++ch) {
                const Buf x = inOrder(*s, k, ch);   // the seam between its last frame and its first
                const size_t n = x.size(), reach = static_cast<size_t>(400 >> k);
                Buf across;
                for (size_t i = n - reach; i < n; ++i) across.push_back(x[i]);
                for (size_t i = 0; i < reach; ++i) across.push_back(x[i]);
                const float own = maxStep(x, n / 3, 2 * n / 3), at = maxStep(across);
                if (ch == 0)
                    std::printf("  %7d frames, level %d: across the seam %.4f, the sine's own %.4f\n", frames, k, at, own);
                CHECK(at <= own * 1.02f);
                CHECK(x.front() == 0.0f && x.back() == 0.0f);
            }
    }
}

// seal() (a sleep that keeps the ring): a 440 Hz sine at 0.8 cut at its peak, sealed, and recorded on
// from its opposite peak for 1 s, then remembered. At every level the join's largest step is at most
// the sine's own (its steady parts, both sides), where unsealed it is 1.6 against 0.06 at level 0;
// level 0 is the unsealed recording's bit for bit but for the kSeamFade frames each side of the join,
// which dip (none further from 0 than it); and pieces at random record the same bits as whole blocks
// (the fade-in counts frames, not pieces). With 0, 1, 68 and 127 frames staged at the seal, the ring
// not yet full and full (its end just behind the join: the fades cross it at every level).
void testSeal() {
    std::printf("== memory: a seal joins with a dip\n");
    const double hz = 440.0, w = 2.0 * kPi * hz / kSec;
    const float a = 0.8f;
    for (const bool full : {false, true})
        for (const int staged : {0, 1, 68, 127}) {
            // Flushed whole before the seal; full: 5513 blocks, the ring's end 64 frames behind.
            const int before = (full ? 5513 : 344) * kBlk + staged, after = kSec;
            const Buf old = sine(hz, before, a, 0.5 * kPi - w * (before - 1)), next = sine(hz, after, a, -0.5 * kPi);
            std::unique_ptr<Memory> m[3];   // sealed in whole blocks, sealed in pieces at random, unsealed
            for (int v = 0; v < 3; ++v) {
                m[v] = std::make_unique<Memory>();
                feed(*m[v], old, v == 1 ? 0 : kBlk);
                if (v < 2) m[v]->seal();
                feed(*m[v], next, v == 1 ? 0 : kBlk);
                CHECK(m[v]->remember());
            }
            const af::GrainSource *s = m[0]->remembered(), *u = m[2]->remembered();
            if (!s || !u || !m[1]->remembered()) continue;
            CHECK(sameSource(*s, *m[1]->remembered()));
            const int join = full ? Memory::kFrames - after : before;   // in the source's order
            bool ok = true;
            std::printf("  %-8s %3d staged:", full ? "full," : "not full,", staged);
            for (int k = 0; k < af::GrainSource::kLevels; ++k) {
                const Buf x = inOrder(*s, k, 0);
                const size_t j = static_cast<size_t>(join >> k), reach = static_cast<size_t>(300 >> k);
                const size_t far = static_cast<size_t>(8000 >> k), near = static_cast<size_t>(1000 >> k);
                const float at = maxStep(x, j - reach, j + reach);
                const float own = std::max(maxStep(x, j - far, j - near), maxStep(x, j + near, j + far));
                std::printf("  %d: %.4f of %.4f", k, at, own);
                ok = ok && at <= own;
            }
            const Buf x = inOrder(*s, 0, 0), y = inOrder(*u, 0, 0);
            const size_t j = static_cast<size_t>(join), n = Memory::kSeamFade;
            int outside = 0, louder = 0;
            for (size_t i = 0; i < x.size(); ++i) {
                if (i + n < j || i >= j + n) outside += x[i] != y[i];
                else louder += std::fabs(x[i]) > std::fabs(y[i]);
            }
            std::printf("; level 0 apart from unsealed: %d outside the fades, %d louder in them\n", outside, louder);
            CHECK(ok && outside == 0 && louder == 0);
        }
}

// Remember's rules (check 7): under 0.5 s recorded refused (22049 frames no, 22050 yes, a source of
// 22048: rounded down to a multiple of 4); within 2 s of recording since the last refused (88199
// no, 88200 yes); pinned refused, then allowed once unpinned; pin() with nothing remembered false
// (holding no pin); the generation counts only the Remembers that happened; the ring recording
// after one is empty.
void testRules() {
    std::printf("== memory: Remember's rules\n");
    Memory m;
    const Buf x = whiteNoise(4 * kSec, 0.3f, 17);
    CHECK(!m.pin());              // nothing remembered: no pin held...
    feed(m, Buf(x.begin(), x.begin() + 22049));
    CHECK(!m.remember());         // under 0.5 s
    CHECK(m.generation() == 0 && m.remembered() == nullptr);
    feed(m, Buf(x.begin(), x.begin() + 1));
    CHECK(m.remember());          // ...so this one isn't refused
    CHECK(m.generation() == 1 && m.fill() == 0.0f);
    // 22050 frames held, 2 over a multiple of 4: the source is the 22048 under it (ready, so Weather
    // plays it).
    CHECK(m.remembered() && m.remembered()->ready() && m.remembered()->frames == 22048);
    feed(m, Buf(x.begin(), x.begin() + 88199));
    CHECK(!m.remember());         // within 2 s of the last
    CHECK(m.generation() == 1);
    feed(m, Buf(x.begin(), x.begin() + 1));
    CHECK(m.remember());
    CHECK(m.generation() == 2);
    const af::GrainSource* held = m.remembered();
    CHECK(m.pin());
    feed(m, x);
    CHECK(!m.remember());         // pinned
    CHECK(m.generation() == 2 && m.remembered() == held);
    CHECK(m.pin());               // pins count
    m.unpin();
    CHECK(!m.remember());
    m.unpin();
    CHECK(m.remember());
    CHECK(m.generation() == 3 && m.remembered() != held);
    CHECK(!held->ready());        // the old ring, recorded over from here: not a source
    m.unpin();                    // an unpin too many changes nothing
    feed(m, x);
    CHECK(m.remember());
    CHECK(m.generation() == 4);
    // fill(): 8 s of 16, then full.
    feed(m, whiteNoise(8 * kSec, 0.3f, 3));
    CHECK(std::fabs(m.fill() - 0.5f) < 1e-6f);
    feed(m, whiteNoise(9 * kSec, 0.3f, 4));
    CHECK(m.fill() == 1.0f);
}

// What a Memory remembers of x written from fresh.
std::unique_ptr<Memory> fresh(const Buf& L, const Buf& R) {
    auto m = std::make_unique<Memory>();
    feed(*m, L, R);
    CHECK(m->remember());
    return m;
}

// reset() (check 8) leaves the remembered source as it was (pointer, fields, every sample); the
// ring recording starts afresh: after a reset, as after a Remember, what is remembered next is what
// a fresh Memory remembers of the same signal, bit for bit.
void testReset() {
    std::printf("== memory: reset\n");
    const Buf a = sine(330.0, 3 * kSec, 0.4f), b = whiteNoise(5 * kSec, 0.4f, 9), c = sine(550.0, 4 * kSec, 0.3f, 0.5);
    Memory m;
    feed(m, a, a);
    CHECK(m.remember());
    const af::GrainSource* s = m.remembered();
    const af::GrainSource before = *s;
    std::vector<int16_t> copy(s->level[0], s->level[0] + 2 * (s->frames + af::GrainSource::kGuard));
    feed(m, b, b);
    m.reset();
    CHECK(m.fill() == 0.0f);
    CHECK(m.remembered() == s && s->frames == before.frames && s->origin == before.origin && s->gain == before.gain);
    CHECK(std::equal(copy.begin(), copy.end(), s->level[0]));
    feed(m, c, b);
    CHECK(std::fabs(m.fill() - 4.0f / 16.0f) < 1e-6f);
    CHECK(m.remember());
    const auto ref = fresh(c, b);
    CHECK(sameSource(*m.remembered(), *ref->remembered()));
    // After a Remember: the next one remembers only what came after it.
    feed(m, a, c);
    CHECK(m.remember());
    const auto ref2 = fresh(a, c);
    CHECK(sameSource(*m.remembered(), *ref2->remembered()));
}

// The same bits whatever the pieces: a ring not full (3.3 s, its frames rounded down to a multiple of
// 4) and one gone round (17.3 s), written in pieces of 1, 33, 77, 100, 128 and at random (1..300,
// more than a block too), remember the same source as pieces of 128 do, every level, the guard too.
void testPieces() {
    std::printf("== memory: the same whatever the pieces\n");
    for (const double seconds : {3.3, 17.3}) {
        const int n = static_cast<int>(seconds * kSec);
        Buf L = whiteNoise(n, 0.5f, 5), R = sine(1234.5, n, 0.7f);
        for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] += 0.8f * std::sin(0.001f * static_cast<float>(i));
        const auto ref = fresh(L, R);
        const af::GrainSource* r = ref->remembered();
        CHECK(r && r->ready() && r->frames == std::min(n - n % 4, Memory::kFrames));
        if (seconds < 4.0) CHECK(n % 4 != 0);   // (so the rounding is tried)
        int same = 0, tried = 0;
        for (const int piece : {1, 33, 77, 100, 0}) {
            Memory m;
            feed(m, L, R, piece, 77);
            CHECK(m.remember());
            ++tried;
            if (m.remembered() && sameSource(*m.remembered(), *ref->remembered())) ++same;
        }
        std::printf("  %.1f s (%d frames, a source of %d): %d of %d the same\n", seconds, n, r ? r->frames : 0, same, tried);
        CHECK(same == tried);
    }
}

// Odd input: NaN and infinities are 0 and never reach the decimators (a sine after them is recorded
// at every level as from fresh); a finite sample however large is full scale; -0 is 0.
void testOddInput() {
    std::printf("== memory: odd input\n");
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    Buf L = sine(500.0, kSec, 0.5f), R = L;
    for (int i = 0; i < kSec; i += 97) {
        L[static_cast<size_t>(i)] = nan;
        R[static_cast<size_t>(i)] = i % 2 ? inf : -inf;
    }
    L[1000] = inf;   // (past the seam's fade)
    R[1001] = -inf;
    L[2000] = -0.0f;
    R[2000] = 1e30f;
    L[3000] = FLT_MAX;   // (too large to scale: still full scale)
    R[3000] = -FLT_MAX;
    Memory m;
    feed(m, L, R, 33);
    const Buf tone = sine(500.0, 2 * kSec, 0.5f);
    feed(m, tone, tone, 128);
    CHECK(m.remember());
    const af::GrainSource* s = m.remembered();
    if (!s) return;
    CHECK(s->level[0][2 * 1000] == 0 && s->level[0][2 * 1001 + 1] == 0 && s->level[0][2 * 2000] == 0);
    CHECK(s->level[0][2 * 2000 + 1] == 32767);
    CHECK(s->level[0][2 * 3000] == 32767 && s->level[0][2 * 3000 + 1] == -32768);
    CHECK(s->level[0][2 * 1067] == 0 && s->level[0][2 * 1067 + 1] == 0);   // 97 x 11: NaN, and inf
    for (int k = 0; k < af::GrainSource::kLevels; ++k) {
        const Buf x = inOrder(*s, k, 0);
        const size_t from = static_cast<size_t>((kSec + 4000) >> k), to = x.size() - static_cast<size_t>(1000 >> k);
        CHECK(std::fabs(db(magnitude(x, 500.0 * (1 << k), from, to) / 0.25)) <= 0.1);
        CHECK(std::fabs(db(rms(x, from, to) * std::sqrt(2.0) / 0.25)) <= 0.1);
    }
}

// Weather plays what Memory remembered at the level it was recorded (the gain puts the headroom
// back): Stream at pitch 0 without drift reads the source itself, here across a full ring's end
// (through its guard; origin elsewhere) with no step, the tone as written; and across a ring not
// yet full, whose end is its seam: a dip, never a step.
void testWeatherReadsMemory() {
    std::printf("== memory: Weather reads it\n");
    for (const int seconds : {20, 3}) {
        Memory m;
        const float a = 0.3f;
        feed(m, sine(300.0, seconds * kSec, a));
        CHECK(m.remember());
        const af::GrainSource* s = m.remembered();
        if (!s) continue;
        // The anchor 2.3 s before the ring's end (full) or the source's (not full): Weather's gate
        // is open by then.
        const double cross = 2.3 * kSec;
        const double pos = seconds == 20 ? Memory::kFrames - s->origin - cross : s->frames - cross;
        af::WeatherPatch p;
        p.level = 1.0f;
        p.mode = af::WM_STREAM;
        p.drift = 0.0f;
        p.position = static_cast<float>(pos / s->frames);
        af::Weather w;
        w.seed(3);
        w.set(p, af::HarmonyPatch{});
        w.gate(true);
        const int n = 3 * kSec;
        Buf L(static_cast<size_t>(n)), R(static_cast<size_t>(n)), sL(static_cast<size_t>(n)), sR(static_cast<size_t>(n));
        for (int i = 0; i < n; i += kBlk)
            w.render(s, 0.0f, &L[static_cast<size_t>(i)], &R[static_cast<size_t>(i)], &sL[static_cast<size_t>(i)],
                     &sR[static_cast<size_t>(i)], 0.0f, std::min(kBlk, n - i));
        const size_t from = static_cast<size_t>(2.1 * kSec), to = static_cast<size_t>(2.6 * kSec);
        const float own = maxStep(L, static_cast<size_t>(2.0 * kSec), static_cast<size_t>(2.2 * kSec));
        const float at = maxStep(L, from, to);
        std::printf("  %2d s: the tone %.3f dB of what was written; across the end %.4f, its own %.4f\n", seconds,
                    db(magnitude(L, 300.0, from, static_cast<size_t>(2.25 * kSec)) / a), at, own);
        CHECK(allFinite(L) && allFinite(R));
        CHECK(std::fabs(db(magnitude(L, 300.0, from, static_cast<size_t>(2.25 * kSec)) / a)) <= 0.2);
        CHECK(at <= own * 1.05f);
        if (seconds == 20) CHECK(std::fabs(db(magnitude(L, 300.0, from, to) / a)) <= 0.2);   // no dip: not the seam
    }
}

// The levels line up where Weather reads them (after the review's probe): a 60 Hz sine (cosine on R)
// recorded, level k read at (origin + p - its offset) / 2^k, linearly, is level 0 at origin + p to
// within 8 of the sine's 13107 (one frame off at level 1 would be about 220), in a full ring of
// every origin mod 4, one exactly full, and rings not yet full.
void testLevelsAligned() {
    std::printf("== memory: the levels aligned at every origin\n");
    const double kTau = 3.19, off[3] = {0.0, 1.0 - kTau, 3.0 - 3.0 * kTau};   // weather.cpp's kOffset
    const auto at = [](const af::GrainSource& s, int k, double q, int ch) {
        const int n = s.frames >> k;
        q = std::fmod(q, n);
        if (q < 0.0) q += n;
        const int i = static_cast<int>(q);
        const double f = q - i, a = s.level[k][2 * i + ch], b = s.level[k][2 * ((i + 1) % n) + ch];
        return a + f * (b - a);
    };
    double worst = 0.0;
    int tried = 0;
    for (const int total : {20 * kSec, 20 * kSec + 1, 20 * kSec + 2, 20 * kSec + 3, Memory::kFrames, 3 * kSec,
                            3 * kSec + 1}) {
        Memory m;
        feed(m, sine(60.0, total, 0.8f), sine(60.0, total, 0.8f, 0.5 * kPi));
        if (!m.remember()) continue;
        const af::GrainSource& s = *m.remembered();
        ++tried;
        double err = 0.0;
        for (int p = 5000; p < 15000; ++p)
            for (int ch = 0; ch < 2; ++ch) {
                const double ref = at(s, 0, s.origin + p, ch);
                for (int k = 1; k < af::GrainSource::kLevels; ++k)
                    err = std::max(err, std::fabs(at(s, k, (s.origin + p - off[k]) / (1 << k), ch) - ref));
            }
        std::printf("  %6d frames, origin %6d: at most %.1f off\n", total, s.origin, err);
        worst = std::max(worst, err);
    }
    CHECK(tried == 7 && worst <= 8.0);
}

// A source's levels turned so that its frame origin - origin % 4 comes first (the origin then
// origin % 4, a whole frame of every level): the same source, without the ring's end where it was.
struct Unrolled {
    std::vector<int16_t> data[af::GrainSource::kLevels];
    af::GrainSource src;
};
std::unique_ptr<Unrolled> unrolled(const af::GrainSource& s) {
    auto u = std::make_unique<Unrolled>();
    const int keep = s.origin % 4;
    for (int k = 0; k < af::GrainSource::kLevels; ++k) {
        const int n = s.frames >> k, turn = (s.origin - keep) >> k;
        u->data[k].resize(2 * static_cast<size_t>(n + af::GrainSource::kGuard));
        for (int j = 0; j < n + af::GrainSource::kGuard; ++j) {
            const int f = (j % n + turn) % n;
            u->data[k][2 * static_cast<size_t>(j)] = s.level[k][2 * f];
            u->data[k][2 * static_cast<size_t>(j) + 1] = s.level[k][2 * f + 1];
        }
        u->src.level[k] = u->data[k].data();
    }
    u->src.frames = s.frames;
    u->src.origin = keep;
    u->src.gain = s.gain;
    return u;
}

// Weather reading a full ring across its end, through the guard, at every level (Stream at pitch 0,
// +12 and +24: levels 0, 1 and 2; the origin no multiple of 4), plays what it plays from the same
// source laid out without that end: the same samples to within float rounding.
void testAcrossTheEnd() {
    std::printf("== memory: Weather across a ring's end at every level\n");
    Memory m;
    const int n = 20 * kSec + 3;
    Buf x = whiteNoise(n, 0.3f, 41);
    const Buf tone = sine(300.0, n, 0.3f);
    for (size_t i = 0; i < x.size(); ++i) x[i] += tone[i];
    feed(m, x, sine(700.0, n, 0.4f));
    CHECK(m.remember());
    const af::GrainSource* s = m.remembered();
    if (!s) return;
    const auto u = unrolled(*s);
    for (const float pitch : {0.0f, 12.0f, 24.0f}) {
        af::WeatherPatch p;
        p.level = 1.0f;
        p.mode = af::WM_STREAM;
        p.drift = 0.0f;
        p.pitch = pitch;
        // The read point a second before the ring's end (the source's frame kFrames - origin).
        p.position = static_cast<float>((Memory::kFrames - s->origin - kSec) / static_cast<double>(s->frames));
        Buf out[2][4];
        for (int which = 0; which < 2; ++which) {
            af::Weather w;
            w.seed(7);
            w.set(p, af::HarmonyPatch{});
            w.gate(true);
            const int len = 3 * kSec;
            for (Buf& b : out[which]) b.assign(static_cast<size_t>(len), 0.0f);
            for (int i = 0; i < len; i += kBlk) {
                const size_t at = static_cast<size_t>(i);
                w.render(which ? &u->src : s, 0.0f, &out[which][0][at], &out[which][1][at], &out[which][2][at],
                         &out[which][3][at], 0.0f, std::min(kBlk, len - i));
            }
        }
        float worst = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < out[0][c].size(); ++i) worst = std::max(worst, std::fabs(out[0][c][i] - out[1][c][i]));
        const double level = rms(out[0][0], static_cast<size_t>(2 * kSec));
        std::printf("  pitch %+3.0f: largest difference %.2g, the output at %.1f dBFS RMS\n", static_cast<double>(pitch),
                    static_cast<double>(worst), db(level));
        CHECK(worst <= 1e-5f && level > 0.05);
    }
}

// pin() from another thread (Keep on the loader thread) while the audio thread writes and asks to
// remember at every block: while pinned the remembered source and its generation never change.
void testPinThreads() {
    std::printf("== memory: pins from another thread\n");
    Memory m;
    const Buf x = whiteNoise(kBlk, 0.5f, 21);
    for (int i = 0; i < kSec; i += kBlk) m.write(x.data(), x.data(), kBlk);
    CHECK(m.remember());
    std::atomic<bool> stop{false};
    std::atomic<int> pins{0}, broken{0};
    std::thread keeper([&] {
        while (!stop.load()) {
            if (!m.pin()) continue;
            const uint32_t gen = m.generation();
            const af::GrainSource* s = m.remembered();
            const af::GrainSource was = *s;
            long sum = 0;
            for (int i = 0; i < 4096; ++i) sum += s->level[0][i];
            std::this_thread::yield();
            long again = 0;
            for (int i = 0; i < 4096; ++i) again += s->level[0][i];
            if (sum != again || m.generation() != gen || m.remembered() != s || s->frames != was.frames ||
                s->origin != was.origin)
                ++broken;
            ++pins;
            m.unpin();
        }
    });
    int remembers = 0;
    for (int b = 0; remembers < 12 && b < 20000000; ++b) {
        m.write(x.data(), x.data(), kBlk);
        if (m.remember()) ++remembers;
    }
    stop = true;
    keeper.join();
    std::printf("  %d Remembers, %d pins, %d broken\n", remembers, pins.load(), broken.load());
    CHECK(remembers == 12 && pins.load() > 0 && broken.load() == 0);
    CHECK(m.generation() == 13);
}

// Nothing in write, seal, remember, reset, remembered, fill, generation, pin or unpin allocates.
void testNoAllocation() {
    std::printf("== memory: no allocation\n");
#if AFT_COUNTS_ALLOCS
    CHECK(hookAllocations());
    Memory m;
    const Buf x = whiteNoise(301, 0.5f, 5);
    countAllocations();
    int remembered = 0;
    for (int i = 0; i < 9000; ++i) {
        m.write(x.data(), x.data() + 1, i % 7 ? kBlk : 1 + i % 300);
        if (m.remember()) ++remembered;
        if (i == 5000) m.reset();
        if (i % 1500 == 700) m.seal();
        if (i % 1000 == 0 && m.pin()) m.unpin();
        (void)m.remembered();
        (void)m.fill();
        (void)m.generation();
    }
    const int allocs = allocationsCounted();
    std::printf("  %d allocations (%d Remembers)\n", allocs, remembered);
    CHECK(allocs == 0 && remembered > 3);
#else
    std::printf("  (counted under ASan: make test)\n");
#endif
}

} // namespace

void fieldsTests() {
    testFields();
    testNightOnC();
    testSurfSwells();
    testFrameDecimator();
    testRecords();
    testFullRing();
    testSeam();
    testSeal();
    testRules();
    testReset();
    testPieces();
    testOddInput();
    testWeatherReadsMemory();
    testLevelsAligned();
    testAcrossTheEnd();
    testPinThreads();
    testNoAllocation();
}

} // namespace aft
