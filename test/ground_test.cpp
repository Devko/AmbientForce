// Ground (dsp/ground.h): the just partials, the register, beating in Hz, gravity, the fade, the
// gains, no steps when the patch or the table changes, Body and Breath under stress, the headroom,
// determinism. Needs no plugin: make test-module M=ground runs it on its own.
#include "check.h"
#include "signal.h"
#include "../dsp/ground.h"
#include "../dsp/lifetime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace aft {
namespace {

using cd = std::complex<double>;
constexpr int kHostBlock = 128;   // MPC's block
constexpr int kSec = 44100;

// The tables the checks play, built once per run and published the way the plugin's builder does.
const af::TableSet& tables() {
    static af::Wavetable t[af::TB_COUNT];
    static af::TableSet set;
    static bool built = false;
    if (!built) {
        for (int id : {af::TB_SINE, af::TB_SAW, af::TB_SQUARE, af::TB_CELLO_TASTO, af::TB_CHOIR_AH_OO}) {
            CHECK(af::buildTable(id, t[id]));
            set.t[id].store(&t[id], std::memory_order_release);
        }
        built = true;
    }
    return set;
}

struct Out {
    Buf L, R, sendL, sendR;
};

// n samples of g in MPC's blocks, into zeroed buffers (render adds), the send at `send`.
Out play(af::Ground& g, int n, float send = 0.5f) {
    const size_t len = static_cast<size_t>(n);
    Out o{Buf(len), Buf(len), Buf(len), Buf(len)};
    for (int i = 0; i < n; i += kHostBlock) {
        const size_t at = static_cast<size_t>(i);
        g.render(tables(), &o.L[at], &o.R[at], &o.sendL[at], &o.sendR[at], send, std::min(kHostBlock, n - i));
    }
    return o;
}

// A steady drone to measure: the partials asked for at full level, no beat, no breath, no body,
// a 50 ms fade.
af::GroundPatch steady(int table, float root, float fifth) {
    af::GroundPatch p;
    p.table = table;
    p.level = 1.0f;
    p.sub = p.octave = p.color = 0.0f;
    p.root = root;
    p.fifth = fifth;
    p.beatHz = 0.0f;
    p.breath = 0.0f;
    p.body = 0.0f;
    p.fadeS = 0.05f;
    return p;
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

// The frequency of the strongest partial within `span` Hz of `near`, from 2^16 samples of x at
// `from`: Blackman-Harris, zero-padded to 2^18, then a parabola through the log magnitudes of the
// top bin and its neighbours. The log of a Gaussian is a parabola and Blackman-Harris is nearly
// one, so the peak lands within a small part of a bin (0.17 Hz here).
double peakHz(const Buf& x, size_t from, double near, double span) {
    constexpr size_t n = 1 << 16, pad = 4 * n;
    std::vector<cd> a(pad);
    for (size_t i = 0; i < n; ++i) {
        const double p = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
        const double w = 0.35875 - 0.48829 * std::cos(p) + 0.14128 * std::cos(2.0 * p) - 0.01168 * std::cos(3.0 * p);
        a[i] = x[from + i] * w;
    }
    fft(a);
    const double bin = static_cast<double>(af::kRate) / static_cast<double>(pad);
    const size_t lo = static_cast<size_t>(std::max(1.0, (near - span) / bin));
    const size_t hi = static_cast<size_t>((near + span) / bin);
    size_t k = lo;
    for (size_t i = lo; i <= hi; ++i)
        if (std::abs(a[i]) > std::abs(a[k])) k = i;
    const double m1 = std::log(std::abs(a[k - 1])), m0 = std::log(std::abs(a[k])), p1 = std::log(std::abs(a[k + 1]));
    const double d = 0.5 * (m1 - p1) / (m1 - 2.0 * m0 + p1);
    return (static_cast<double>(k) + d) * bin;
}

// Frequency by rising zero crossings (interpolated), over x[from..to): for a pure tone.
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

// How fast the amplitude of x[from..to) near `hz` beats, and how deep (the envelope's swing over
// its peak). x is moved down by hz (complex), then smoothed by two moving averages, one period of
// f0 long and one of f0 / 2: they put nulls on every other partial of a drone on f0 (its root's and
// its fifth's harmonics, all at least f0 / 2 from hz), and pass the slow beat. The rate: the
// envelope's rising crossings through the middle of its swing.
struct Beat {
    double hz = 0.0, depth = 0.0;
};
Beat beat(const Buf& x, size_t from, size_t to, double hz, double f0) {
    const size_t n = to - from;
    std::vector<cd> z(n);
    for (size_t i = 0; i < n; ++i)
        z[i] = static_cast<double>(x[from + i]) * std::polar(1.0, -2.0 * kPi * hz * static_cast<double>(from + i) / af::kRate);
    auto boxcar = [](const std::vector<cd>& v, size_t len) {
        std::vector<cd> out(v.size() - len + 1);
        cd s = 0.0;
        for (size_t i = 0; i < len; ++i) s += v[i];
        out[0] = s / static_cast<double>(len);
        for (size_t i = len; i < v.size(); ++i) {
            s += v[i] - v[i - len];
            out[i - len + 1] = s / static_cast<double>(len);
        }
        return out;
    };
    const auto len1 = static_cast<size_t>(std::lround(af::kRate / f0));
    const std::vector<cd> e = boxcar(boxcar(z, len1), 2 * len1);
    std::vector<double> env(e.size());
    for (size_t i = 0; i < e.size(); ++i) env[i] = std::abs(e[i]);
    const double lo = *std::min_element(env.begin(), env.end()), hi = *std::max_element(env.begin(), env.end());
    const double mid = 0.5 * (lo + hi);
    double first = -1.0, last = 0.0;
    int count = 0;
    for (size_t i = 1; i < env.size(); ++i)
        if (env[i - 1] < mid && env[i] >= mid) {
            const double t = static_cast<double>(i - 1) + (mid - env[i - 1]) / (env[i] - env[i - 1]);
            if (first < 0.0) first = t;
            else {
                last = t;
                ++count;
            }
        }
    Beat b;
    b.hz = count > 0 ? static_cast<double>(af::kRate) * count / (last - first) : 0.0;
    b.depth = hi > 0.0 ? (hi - lo) / hi : 0.0;
    return b;
}

Buf sum(const Out& o) {
    Buf s(o.L.size());
    for (size_t i = 0; i < s.size(); ++i) s[i] = o.L[i] + o.R[i];
    return s;
}

// Check 1: in Just the Fifth is 3/2 of the Root, to the hundredth of a hertz, and the root is
// the target's pitch class in Register's octave. Each tuning's Color intervals, likewise.
void testPartials() {
    std::printf("== ground: just partials\n");
    af::HarmonyPatch h;   // C, Just
    af::Ground g;
    g.seed(1);
    g.set(steady(af::TB_SINE, 1.0f, 1.0f), h);
    g.setTarget(48);
    const Out o = play(g, kSec / 2 + (1 << 16));
    const double root = peakHz(o.L, kSec / 2, 65.4, 3.0), fifth = peakHz(o.L, kSec / 2, 98.1, 3.0);
    std::printf("  root %.4f Hz (C2 %.4f), fifth %.4f Hz = root x %.6f\n", root, af::noteHz(36.0f), fifth, fifth / root);
    CHECK(std::fabs(root - 65.40639) < 0.01);
    CHECK(std::fabs(fifth - 1.5 * root) < 0.01);
    CHECK(g.pitch() == 36.0);
    for (int note : {36, 60, 0, 120}) {   // the octave the target is in doesn't count
        g.setTarget(note);
        CHECK(g.goal() == 36.0);
    }
    g.setTarget(43);   // G: under Just in C, the key's own fifth
    CHECK(std::fabs(g.goal() - af::tunedPitch(h, 43)) < 1e-9 && std::fabs(g.goal() - 43.0195500086538738) < 1e-6);

    // Equal: the Fifth is 7 semitones.
    af::HarmonyPatch eq = h;
    eq.tuning = af::TU_EQUAL;
    af::Ground e;
    e.seed(1);
    e.set(steady(af::TB_SINE, 1.0f, 1.0f), eq);
    e.setTarget(48);
    const Out oe = play(e, kSec / 2 + (1 << 16));
    const double er = peakHz(oe.L, kSec / 2, 65.4, 3.0), ef = peakHz(oe.L, kSec / 2, 98.0, 3.0);
    CHECK(std::fabs(ef - er * std::exp2(7.0 / 12.0)) < 0.01);

    // Color alone, a pure tone, per tuning and interval: its ratio over the root.
    const double just[af::CI_COUNT] = {6.0 / 5, 5.0 / 4, 4.0 / 3, 9.0 / 5, 9.0 / 4, 8.0 / 3};
    const double pyth[af::CI_COUNT] = {32.0 / 27, 81.0 / 64, 4.0 / 3, 16.0 / 9, 9.0 / 4, 8.0 / 3};
    const int semis[af::CI_COUNT] = {3, 4, 5, 10, 14, 17};
    double worst = 0.0;
    for (int tuning : {af::TU_JUST, af::TU_PYTHAGOREAN, af::TU_EQUAL})
        for (int ci = 0; ci < af::CI_COUNT; ++ci) {
            af::HarmonyPatch hc = h;
            hc.tuning = tuning;
            af::GroundPatch p = steady(af::TB_SINE, 0.0f, 0.0f);
            p.color = 1.0f;
            p.colorInterval = ci;
            p.registerOct = 3;
            af::Ground c;
            c.seed(2);
            c.set(p, hc);
            c.setTarget(57);   // A: in C, Just and Pythagorean move it off Equal's 220 Hz
            const Out oc = play(c, 3 * kSec);
            const double rootHz = 440.0 * std::exp2((af::tunedPitch(hc, 57) - 69.0) / 12.0);
            const double ratio = tuning == af::TU_JUST ? just[ci] : tuning == af::TU_PYTHAGOREAN ? pyth[ci] : std::exp2(semis[ci] / 12.0);
            const double err = std::fabs(zeroCrossHz(oc.L, kSec / 2, oc.L.size()) - ratio * rootHz);
            worst = std::max(worst, err);
            CHECK(err < 0.01);
        }
    std::printf("  Color, 3 tunings x 6 intervals: within %.1e Hz of ratio x root\n", worst);
}

// Check 2: Beat is in Hz. With Root and Fifth on a saw, Root's 3rd harmonic and Fifth's 2nd meet,
// and their sum's envelope beats there at Beat, 1.0 Hz, the same in register 2 and 3. (Root moves
// -0.2 Hz and Fifth +0.2 per Hz of Beat: their harmonics there -0.6 and +0.4.)
void testBeat() {
    std::printf("== ground: beat in Hz\n");
    const double want = 1.0;
    CHECK(std::fabs(2.0 * af::Ground::kBeat[af::Ground::PT_FIFTH] - 3.0 * af::Ground::kBeat[af::Ground::PT_ROOT] - want) < 1e-6);
    double rates[2];
    for (int r = 0; r < 2; ++r) {
        af::GroundPatch p = steady(af::TB_SAW, 1.0f, 1.0f);
        p.beatHz = 1.0f;
        p.registerOct = 2 + r;
        af::Ground g;
        g.seed(3);
        g.set(p, af::HarmonyPatch{});
        g.setTarget(48);
        const Buf x = sum(play(g, 13 * kSec));
        const double f0 = af::noteHz(36.0f + 12.0f * static_cast<float>(r));
        const Beat b = beat(x, kSec / 2, x.size(), 3.0 * f0, f0);
        rates[r] = b.hz;
        std::printf("  register %d: root %.2f Hz, the envelope at its 3rd harmonic beats at %.4f Hz (depth %.2f)\n",
                    2 + r, f0, b.hz, b.depth);
        CHECK(std::fabs(b.hz - want) < 0.05);
        CHECK(b.depth > 0.5);   // a beat, not a ripple: the saw's h3 (1/3) against its h2 (1/2)
    }
    CHECK(std::fabs(rates[0] - rates[1]) < 0.05);

    // Beat 0: nothing beats (Just: exact ratios).
    af::Ground z;
    z.seed(3);
    z.set(steady(af::TB_SAW, 1.0f, 1.0f), af::HarmonyPatch{});
    z.setTarget(48);
    const Buf x = sum(play(z, 4 * kSec));
    const double f0 = af::noteHz(36.0f);
    const Beat b = beat(x, kSec / 2, x.size(), 3.0 * f0, f0);
    std::printf("  beat 0: envelope swing %.1e of its peak\n", b.depth);
    CHECK(b.depth < 1e-3);
}

// Check 3: Gravity 6 s from 48 to 53: an exponential with a 2 s time constant, so 63% of the way
// after 2 s and 95% after 6. In Equal 53 is 53; in Just (C) F is a pure fourth, 52.98, and the
// glide goes there.
void testGravity() {
    std::printf("== ground: gravity\n");
    for (int tuning : {af::TU_EQUAL, af::TU_JUST}) {
        af::HarmonyPatch h;
        h.tuning = tuning;
        af::GroundPatch p = steady(af::TB_SINE, 1.0f, 0.0f);
        p.registerOct = 3;
        p.gravityS = 6.0f;
        af::Ground g;
        g.seed(4);
        g.set(p, h);
        g.setTarget(48);
        play(g, kSec);
        CHECK(g.pitch() == 48.0);   // from silence it jumps
        g.setTarget(53);
        const double goal = af::tunedPitch(h, 53);
        CHECK(g.goal() == goal);
        play(g, 2 * kSec);
        const double at2 = g.pitch();
        play(g, 4 * kSec);
        const double at6 = g.pitch();
        std::printf("  %s: %.4f after 2 s, %.4f after 6 s (goal %.4f)\n", tuning == af::TU_EQUAL ? "Equal" : "Just", at2, at6, goal);
        CHECK(at2 >= 50.5 && at2 <= 52.0);
        CHECK(std::fabs(at6 - goal) <= 0.25);
        if (tuning == af::TU_EQUAL) CHECK(std::fabs(at6 - 53.0) <= 0.25);

        // The sound follows the pitch: a minute on, the root plays the goal.
        const Out o = play(g, 30 * kSec);
        const double hz = zeroCrossHz(o.L, o.L.size() - 3 * kSec, o.L.size());
        CHECK(std::fabs(hz - 440.0 * std::exp2((goal - 69.0) / 12.0)) < 0.01);
    }
    // Gravity 0 jumps.
    af::GroundPatch p = steady(af::TB_SINE, 1.0f, 0.0f);
    p.gravityS = 0.0f;
    af::Ground g;
    g.seed(4);
    g.set(p, af::HarmonyPatch{});
    g.setTarget(48);
    play(g, kSec / 10);
    g.setTarget(53);
    play(g, kHostBlock);
    CHECK(g.pitch() == g.goal());
}

// Check 4: nothing before a target; a 1 s fade reaches -30 dB at 0.5 s; a stop goes silent
// within fadeS + 0.1 s; a target while it fades out turns it around where it is. Levels against
// a twin that faded in at once: the same seed plays the same phases, so the RMS ratio over any
// window is the gain between them.
void testFade() {
    std::printf("== ground: fade\n");
    af::GroundPatch p = steady(af::TB_SINE, 1.0f, 0.0f);
    p.fadeS = 1.0f;
    af::GroundPatch q = p;
    q.fadeS = 0.05f;
    af::Ground g, ref;
    g.seed(5);
    ref.seed(5);
    g.set(p, af::HarmonyPatch{});
    ref.set(q, af::HarmonyPatch{});
    const Out before = play(g, kSec / 2);
    play(ref, kSec / 2);
    CHECK(!g.sounding());
    CHECK(peak(before.L) == 0.0f && peak(before.R) == 0.0f && peak(before.sendL) == 0.0f && peak(before.sendR) == 0.0f);

    g.setTarget(48);
    ref.setTarget(48);
    CHECK(g.sounding());
    const Out a = play(g, 3 * kSec / 2), b = play(ref, 3 * kSec / 2);
    auto gainDb = [](const Buf& x, const Buf& y, double at) {   // a 10 ms window centred on `at` s
        const auto c = static_cast<size_t>(at * kSec);
        return db(rms(x, c - 220, c + 221) / rms(y, c - 220, c + 221));
    };
    const double half = gainDb(a.L, b.L, 0.5), full = gainDb(a.L, b.L, 1.2);
    std::printf("  fade in 1 s: %.2f dB at 0.5 s, %.3f dB at 1.2 s\n", half, full);
    CHECK(std::fabs(half + 30.0) <= 3.0);
    CHECK(std::fabs(full) < 0.01);

    // Stop: silent within fadeS + 0.1 s, not long before it.
    g.setTarget(-1);
    ref.setTarget(-1);
    int n = 0;
    while (g.sounding() && n < 3 * kSec) {
        play(g, kHostBlock);
        n += kHostBlock;
    }
    std::printf("  fade out 1 s: silent after %.3f s\n", static_cast<double>(n) / kSec);
    CHECK(!g.sounding() && n <= static_cast<int>(1.1 * kSec) && n >= static_cast<int>(0.9 * kSec));
    const Out after = play(g, kSec / 5);
    CHECK(peak(after.L) == 0.0f && peak(after.R) == 0.0f && peak(after.sendL) == 0.0f);

    // Turned around: half way out (-30 dB), a new target fades it back in from there.
    af::Ground t, tw;
    t.seed(6);
    tw.seed(6);
    t.set(p, af::HarmonyPatch{});
    tw.set(q, af::HarmonyPatch{});
    t.setTarget(48);
    tw.setTarget(48);
    play(t, 3 * kSec / 2);
    play(tw, 3 * kSec / 2);
    t.setTarget(-1);
    const Out out = play(t, kSec / 2), outRef = play(tw, kSec / 2);
    t.setTarget(48);
    const Out back = play(t, kSec), backRef = play(tw, kSec);
    const double low = gainDb(out.L, outRef.L, 0.49), turn = gainDb(back.L, backRef.L, 0.01);
    const double up = gainDb(back.L, backRef.L, 0.25);
    std::printf("  out to %.1f dB, back in from %.1f dB, %.1f dB 0.25 s on\n", low, turn, up);
    CHECK(std::fabs(low + 30.0) <= 3.0);
    CHECK(std::fabs(turn - low) < 2.0);
    CHECK(std::fabs(up + 15.0) <= 3.0);
}

// Check 5: level 0 or mute: the output and the send are exactly 0, and stay so after a level
// that falls to 0 mid-note has ramped down.
void testGains() {
    std::printf("== ground: level 0 and mute\n");
    for (int k = 0; k < 3; ++k) {
        af::GroundPatch p;   // the default drone, every moving part on
        p.body = 0.5f;
        if (k == 0) p.level = 0.0f;
        if (k == 1) p.mute = true;
        af::Ground g;
        g.seed(7);
        g.set(p, af::HarmonyPatch{});
        g.setTarget(50);
        if (k == 2) {
            const Out o = play(g, 6 * kSec);
            CHECK(peak(o.L) > 0.01f && peak(o.sendL) > 0.001f);   // it plays
            p.level = 0.0f;
            g.set(p, af::HarmonyPatch{});
            play(g, kHostBlock);   // the ramp down
        }
        const Out o = play(g, 2 * kSec, 1.0f);
        const bool zero = peak(o.L) == 0.0f && peak(o.R) == 0.0f && peak(o.sendL) == 0.0f && peak(o.sendR) == 0.0f;
        CHECK(zero);
        CHECK(g.sounding());   // still on: unmuted, it is there
    }
    // The send is the output times spaceSend.
    af::Ground g;
    g.seed(7);
    g.set(af::GroundPatch{}, af::HarmonyPatch{});
    g.setTarget(50);
    const Out o = play(g, 2 * kSec, 0.25f);
    double err = 0.0;
    for (size_t i = 0; i < o.L.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(o.sendL[i] - 0.25f * o.L[i])));
    CHECK(err < 1e-6);
}

// No steps: level, Tone, Body, Width and a partial all jump in one set(), and a pure drone (sine
// partials) moves across the change at most a few times as fast as it did before it. The gains
// ramp over a chunk, so a 70% drop adds 2% of the drone a sample to its own slope; a step would
// move it 30 times as far as the steady drone moves in a sample.
void testNoSteps() {
    std::printf("== ground: no steps when the patch jumps\n");
    af::GroundPatch p = steady(af::TB_SINE, 1.0f, 0.7f);
    p.sub = 0.5f;
    p.octave = 0.5f;
    p.color = 0.5f;
    af::Ground g;
    g.seed(8);
    g.set(p, af::HarmonyPatch{});
    g.setTarget(45);
    const Out a = play(g, kSec);
    const float steadyStep = maxStep(a.L, a.L.size() / 2), top = peak(a.L, a.L.size() / 2);
    // The change lands where the drone is near its peak, where a step would be largest.
    float last = a.L.back();
    for (int k = 0; k < 2 * kSec / kHostBlock && std::fabs(last) < 0.7f * top; ++k) last = play(g, kHostBlock).L.back();
    CHECK(std::fabs(last) >= 0.7f * top);
    p.level = 0.3f;
    p.cutoffHz = 150.0f;
    p.body = 1.0f;
    p.width = 1.0f;
    p.sub = 0.0f;
    g.set(p, af::HarmonyPatch{});
    const Out b = play(g, kSec / 10);
    const float jump = std::max(maxStep(b.L), std::fabs(b.L[0] - last));
    std::printf("  largest step %.5f before, %.5f across the change (at %.2f, the peak %.2f)\n", steadyStep, jump, last, top);
    CHECK(jump <= 3.0f * steadyStep);   // ramps over a chunk; a step would be ~30x
}

// Check 6: Tone at both ends, Body 1 and Breath 1 for a minute: finite and at most 4. Then the
// headroom: the loudest Ground found (every table, Body, Color and Tone tried with every partial at
// 1, level 1, Breath 1, Beat 3 and Width 0: Square, Body 1, an 11th) peaks under 1.5.
void testStability() {
    std::printf("== ground: stability\n");
    for (float cutoff : {40.0f, 16000.0f}) {
        af::GroundPatch p;
        p.cutoffHz = cutoff;
        p.body = 1.0f;
        p.breath = 1.0f;
        p.breathHz = 0.5f;
        p.beatHz = 3.0f;
        p.pos.smear = 1.0f;
        af::Ground g;
        g.seed(9);
        g.set(p, af::HarmonyPatch{});
        g.setTarget(40);
        float pk = 0.0f;
        bool finite = true;
        for (int s = 0; s < 60; ++s) {
            if (s == 20) g.setTarget(47);
            if (s == 40) g.setTarget(33);
            const Out o = play(g, kSec, 1.0f);
            finite = finite && allFinite(o.L) && allFinite(o.R) && allFinite(o.sendL) && allFinite(o.sendR);
            pk = std::max({pk, peak(o.L), peak(o.R)});
        }
        std::printf("  cutoff %.0f Hz: peak %.3f\n", cutoff, pk);
        CHECK(finite);
        CHECK(pk <= 4.0f);
    }
    af::GroundPatch p;
    p.table = af::TB_SQUARE;
    p.level = 1.0f;
    p.sub = p.root = p.fifth = p.octave = p.color = 1.0f;
    p.colorInterval = af::CI_ELEVENTH;
    p.body = 1.0f;
    p.breath = 1.0f;
    p.breathHz = 0.5f;
    p.beatHz = 3.0f;
    p.width = 0.0f;
    p.fadeS = 0.05f;
    af::Ground g;
    g.seed(1);
    g.set(p, af::HarmonyPatch{});
    g.setTarget(43);
    const Out o = play(g, 20 * kSec);
    const float top = std::max(peak(o.L), peak(o.R));
    std::printf("  the loudest found (Square, Body 1, every partial at 1, level 1, Breath 1): peak %.3f\n", top);
    CHECK(allFinite(o.L) && allFinite(o.R));
    CHECK(top <= 1.5f && top > 1.0f);
    // The default drone, for the engine's gain staging.
    af::GroundPatch d;
    d.breath = 0.0f;
    d.fadeS = 0.05f;
    af::Ground dg;
    dg.seed(1);
    dg.set(d, af::HarmonyPatch{});
    dg.setTarget(43);
    const Out od = play(dg, 4 * kSec);
    std::printf("  the default drone (Breath 0): RMS %.1f dBFS, peak %.3f\n", db(rms(od.L, kSec)), peak(od.L, kSec));
}

// A new table fades in over 20 ms. Switched where the old table and the new differ most, the
// drone steps no further than it moves anyway; up to the switch it is the old table's twin,
// sample for sample, half way through the fade it is neither, and 50 ms on it is the new one's
// twin. Twins: the same seed and patch but for the table, so the same phases and positions.
template <class Switch>
void tableSwitch(const char* name, const af::TableSet& set, const af::GroundPatch& pg, Switch doSwitch,
                 const af::GroundPatch& pa, const af::GroundPatch& pb) {
    af::Ground g, ta, tb;
    for (af::Ground* x : {&g, &ta, &tb}) x->seed(21);
    g.set(pg, af::HarmonyPatch{});
    ta.set(pa, af::HarmonyPatch{});
    tb.set(pb, af::HarmonyPatch{});
    for (af::Ground* x : {&g, &ta, &tb}) x->setTarget(45);
    auto left = [](af::Ground& x, const af::TableSet& s, int n) {
        Buf L(static_cast<size_t>(n)), R(L.size()), sL(L.size()), sR(L.size());
        for (int i = 0; i < n; i += kHostBlock) {
            const size_t at = static_cast<size_t>(i);
            x.render(s, &L[at], &R[at], &sL[at], &sR[at], 0.5f, std::min(kHostBlock, n - i));
        }
        return L;
    };
    const Buf g0 = left(g, set, kSec), a0 = left(ta, tables(), kSec), b0 = left(tb, tables(), kSec);
    bool twin = g0 == a0;
    const size_t half = a0.size() / 2;
    const float steadyStep = std::max(maxStep(a0, half), maxStep(b0, half));
    float apart = 0.0f;
    for (size_t i = half; i < a0.size(); ++i) apart = std::max(apart, std::fabs(a0[i] - b0[i]));
    // The switch lands where the two tables are far apart, where a step would be largest.
    float lg = g0.back(), la = a0.back(), lb = b0.back();
    for (int k = 0; k < 2 * kSec / kHostBlock && std::fabs(la - lb) < 0.6f * apart; ++k) {
        const Buf gx = left(g, set, kHostBlock), ax = left(ta, tables(), kHostBlock);
        twin = twin && gx == ax;
        lg = gx.back();
        la = ax.back();
        lb = left(tb, tables(), kHostBlock).back();
    }
    CHECK(twin);
    CHECK(std::fabs(la - lb) >= 0.6f * apart && std::fabs(la - lb) > 5.0f * steadyStep);   // a step would show
    doSwitch(g);
    const Buf g1 = left(g, set, kSec / 10), a1 = left(ta, tables(), kSec / 10), b1 = left(tb, tables(), kSec / 10);
    const float jump = std::max(maxStep(g1), std::fabs(g1[0] - lg));
    float midA = 0.0f, midB = 0.0f, end = 0.0f;
    for (size_t i = 220; i < 660; ++i) {   // 5..15 ms
        midA = std::max(midA, std::fabs(g1[i] - a1[i]));
        midB = std::max(midB, std::fabs(g1[i] - b1[i]));
    }
    for (size_t i = kSec / 20; i < g1.size(); ++i) end = std::max(end, std::fabs(g1[i] - b1[i]));
    std::printf("  %s: largest step %.5f steady, %.5f across the switch (the tables %.3f apart there); "
                "mid-fade %.3f / %.3f off either; %.1e off the new one after 50 ms\n",
                name, steadyStep, jump, std::fabs(la - lb), midA, midB, end);
    CHECK(jump <= 3.0f * steadyStep);
    CHECK(midA > 0.1f * apart && midB > 0.1f * apart);
    CHECK(end < 1e-5f);
}

void testTableSwitch() {
    std::printf("== ground: a table change fades\n");
    af::GroundPatch a = steady(af::TB_CELLO_TASTO, 1.0f, 0.7f);
    a.sub = 0.5f;
    a.octave = 0.5f;
    a.color = 0.5f;
    a.level = 0.7f;
    a.cutoffHz = 16000.0f;   // open: the Tone would soften a step over a few samples
    af::GroundPatch b = a;
    b.table = af::TB_CHOIR_AH_OO;
    tableSwitch("Cello Tasto to Choir Ah-Oo", tables(), a, [&](af::Ground& g) { g.set(b, af::HarmonyPatch{}); }, a, b);

    // A slot still on the sine when its table is published (the builder finishing mid-note).
    static af::TableSet pending;   // Cello Tasto's slot empty: the sine
    af::GroundPatch s = a;
    s.table = af::TB_SINE;
    tableSwitch("the sine to Cello Tasto, published", pending, a,
                [&](af::Ground&) { pending.t[af::TB_CELLO_TASTO].store(&tables().get(af::TB_CELLO_TASTO), std::memory_order_release); },
                s, a);
}

// The same seed, the same samples, with everything moving; another seed, others.
void testDeterminism() {
    std::printf("== ground: determinism\n");
    auto voice = [](uint32_t seed) {
        af::GroundPatch p;
        p.color = 0.4f;
        p.body = 0.6f;
        p.breath = 0.8f;
        p.breathHz = 1.0f;
        p.pos.smear = 0.7f;
        p.gravityS = 0.5f;
        af::Ground g;
        g.seed(seed);
        g.set(p, af::HarmonyPatch{});
        g.setTarget(41);
        Buf x = play(g, kSec).L;
        g.setTarget(46);
        const Buf y = play(g, kSec).L;
        x.insert(x.end(), y.begin(), y.end());
        return x;
    };
    const Buf a = voice(11);
    CHECK(a == voice(11));
    CHECK(a != voice(12));
    // reset() starts it over as the seed did.
    af::Ground g;
    g.seed(13);
    g.set(af::GroundPatch{}, af::HarmonyPatch{});
    g.setTarget(41);
    const Buf first = play(g, kSec / 2).L;
    g.reset();
    CHECK(!g.sounding() && g.target() == -1);
    g.setTarget(41);
    CHECK(play(g, kSec / 2).L == first);
}

} // namespace

void groundTests() {
    testPartials();
    testBeat();
    testGravity();
    testFade();
    testGains();
    testNoSteps();
    testTableSwitch();
    testStability();
    testDeterminism();
}

} // namespace aft
