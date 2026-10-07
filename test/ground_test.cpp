// Ground (dsp/ground.h): the just partials, the linear read's images, beating in Hz, gravity, the
// octave a root takes and a Register change, the fade, the gains and mute, no steps when the patch
// or the table changes, Body, Breath, Width and Tone, odd parameters and odd sequences, stability,
// the headroom, determinism. Needs no plugin: make test-module M=ground runs it on its own.
#include "check.h"
#include "signal.h"
#include "../dsp/ground.h"
#include "../dsp/lifetime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <vector>

namespace aft {
namespace {

constexpr int kHostBlock = 128;   // MPC's block
constexpr int kSec = 44100;

// The library as the plugin's builder publishes it, every table built once per run (signal.h).
const af::TableSet& tables() {
    static af::TableSet set;
    static bool published = false;
    if (!published) {
        for (int id = 0; id < af::TB_COUNT; ++id) set.t[id].store(&testTable(id), std::memory_order_release);
        published = true;
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

// A partial's level in a patch, by its index.
float& partialLevel(af::GroundPatch& p, int i) {
    float* const level[af::Ground::PT_COUNT] = {&p.sub, &p.root, &p.fifth, &p.octave, &p.color};
    return *level[i];
}

double toneHz(double note) { return 440.0 * std::exp2((note - 69.0) / 12.0); }

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
    g.setTarget(43);   // G: under Just in C a pure fifth, taken a fourth below (the nearer place)
    CHECK(g.note() == 31 && std::fabs(g.goal() - af::tunedPitch(h, 31)) < 1e-9);
    CHECK(std::fabs(g.goal() - 31.0195500086538738) < 1e-6);

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

// The linear read's images: every partial reads linearly. On Saw, the brightest table, at the
// highest root (B3) with the Tone open, nothing below 15 kHz off a partial's own harmonics comes
// within 60 dB of its fundamental (lifeosc.h holds the linear read to -60 dB as well).
void testImages() {
    std::printf("== ground: the linear read's images (Saw, B3, Tone 16 kHz)\n");
    const af::HarmonyPatch h;
    const double rootHz = toneHz(af::tunedPitch(h, 59));
    const double ratio[af::Ground::PT_COUNT] = {0.5, 1.0, 1.5, 2.0, 8.0 / 3.0};   // Color: the 11th, the highest
    const char* const name[af::Ground::PT_COUNT] = {"Sub", "Root", "Fifth", "Octave", "Color (11th)"};
    for (int i = 0; i < af::Ground::PT_COUNT; ++i) {
        af::GroundPatch p = steady(af::TB_SAW, 0.0f, 0.0f);
        partialLevel(p, i) = 1.0f;
        p.colorInterval = af::CI_ELEVENTH;
        p.registerOct = 3;
        p.cutoffHz = 16000.0f;
        af::Ground g;
        g.seed(40);
        g.set(p, h);
        g.setTarget(59);
        const Out o = play(g, kSec / 2 + (1 << 16));
        const Spectrum sp(o.L, kSec / 2, 1 << 16);
        const double f = ratio[i] * rootHz;
        double worst = 0.0, at = 0.0;
        for (size_t k = 1; k < sp.amp.size() && static_cast<double>(k) * sp.binHz < 15000.0; ++k) {
            const double hz = static_cast<double>(k) * sp.binHz;
            if (std::fabs(hz - std::round(hz / f) * f) < 8.0 * sp.binHz) continue;   // a harmonic's main lobe
            if (sp.amp[k] > worst) {
                worst = sp.amp[k];
                at = hz;
            }
        }
        const double rel = db(worst / sp.at(f));
        std::printf("  %-12s %5.1f Hz: images at most %.1f dB, at %.0f Hz\n", name[i], f, rel, at);
        CHECK(rel < -60.0);
    }
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

// The octave a new root takes: its pitch class's place nearest where the root is now (the
// shortest glide), within base - 6 .. base + 17. Equal, so a note is its pitch.
void testOctaves() {
    std::printf("== ground: the octave of a new root\n");
    af::HarmonyPatch eq;
    eq.tuning = af::TU_EQUAL;
    af::GroundPatch p = steady(af::TB_SINE, 1.0f, 0.0f);
    p.gravityS = 1.0f;
    {   // B to C rises a semitone (register 2: B2 to C3), not falls eleven.
        af::Ground g;
        g.seed(30);
        g.set(p, eq);
        g.setTarget(71);   // B, from off: in Register's octave
        play(g, kSec / 10);
        CHECK(g.note() == 47);
        g.setTarget(60);
        CHECK(g.note() == 48 && g.goal() == 48.0);
    }
    {   // In B (Just), I to ii: B to C#, a whole tone (9/8) up.
        af::HarmonyPatch hb;
        hb.key = 11;
        af::Ground g;
        g.seed(31);
        g.set(p, hb);
        g.setTarget(47);
        play(g, kSec / 10);
        const double from = g.pitch();
        g.setTarget(61);
        CHECK(g.note() == 49);
        CHECK(std::fabs(g.goal() - from - 12.0 * std::log2(9.0 / 8.0)) < 1e-9);
    }
    // A random walk of targets, gliding a little between them, in every register: always within
    // the bounds, always the nearer of the pitch class's two places there.
    uint32_t r = 0x2468ACE1u;
    int longest = 0;
    bool inside = true, nearest = true;
    for (int reg = 1; reg <= 3; ++reg) {
        p.registerOct = reg;
        af::Ground g;
        g.seed(32);
        g.set(p, eq);
        g.setTarget(static_cast<int>(af::xorshift(r) % 128));
        const int base = 12 * (reg + 1);
        for (int step = 0; step < 1000; ++step) {
            const int target = static_cast<int>(af::xorshift(r) % 128), before = g.note();
            const double now = g.pitch();
            g.setTarget(target);
            int want = before;
            if (target % 12 != before % 12) {
                const int lo = base - 6 + ((target - (base - 6)) % 12 + 12) % 12;
                want = std::fabs(lo + 12 - now) < std::fabs(lo - now) ? lo + 12 : lo;
            }
            nearest = nearest && g.note() == want;
            inside = inside && g.note() >= base - 6 && g.note() < base + 18;
            longest = std::max(longest, std::abs(g.note() - before));
            play(g, kHostBlock * static_cast<int>(1 + af::xorshift(r) % 8));
        }
    }
    std::printf("  3000 targets at random: within the bounds %s, the nearer place %s; the longest move %d semitones\n",
                inside ? "always" : "NOT always", nearest ? "always" : "NOT always", longest);
    CHECK(inside);
    CHECK(nearest);
}

// Register changed while it sounds: no glide. It dips out over 40 ms, moves an octave at the
// bottom, and comes back over 40 ms, never stepping further than the steady drone moves in a
// sample. Muted, there is nothing to dip: the next step moves it.
void testRegisterChange() {
    std::printf("== ground: a Register change dips, it doesn't glide\n");
    af::HarmonyPatch eq;
    eq.tuning = af::TU_EQUAL;
    af::GroundPatch p = steady(af::TB_SINE, 1.0f, 0.0f);
    p.gravityS = 6.0f;   // a glide would be slow, and plain to see
    af::Ground g;
    g.seed(33);
    g.set(p, eq);
    g.setTarget(59);   // B2 (47): an 8 ms period, short against the dip
    const Out a = play(g, kSec / 2);
    CHECK(g.note() == 47);
    p.registerOct = 3;
    g.set(p, eq);
    Buf x;
    bool twoPitches = true;
    int moved = -1;
    for (int b = 0; b < kSec / 5 / kHostBlock; ++b) {
        const Out o = play(g, kHostBlock);
        x.insert(x.end(), o.L.begin(), o.L.end());
        twoPitches = twoPitches && (g.pitch() == 47.0 || g.pitch() == 59.0);
        if (moved < 0 && g.pitch() == 59.0) moved = b;
    }
    const double movedMs = 1000.0 * (moved + 1) * kHostBlock / kSec;
    // The level: the drone's peak in 8 ms windows (a period of B2) against the steady drone's.
    const float steadyPeak = peak(a.L, a.L.size() / 2);
    float lowest = 1e9f;
    for (size_t i = 0; i + 353 <= x.size(); i += 44) lowest = std::min(lowest, peak(x, i, i + 353));
    const double back = db(rms(x, x.size() - kSec / 20, x.size()) / rms(a.L, a.L.size() / 2));
    const float steadyStep = std::max(maxStep(a.L, a.L.size() / 2), maxStep(x, x.size() - kSec / 20));
    const float jump = std::max(maxStep(x), std::fabs(x[0] - a.L.back()));
    std::printf("  47 -> 59 at %.1f ms, no pitch between; the level down to %.3f of its peak, back at %+.2f dB; "
                "largest step %.5f against %.5f steady\n", movedMs, lowest / steadyPeak, back, jump, steadyStep);
    CHECK(twoPitches && moved >= 0 && g.note() == 59 && g.goal() == 59.0);
    CHECK(movedMs > 35.0 && movedMs < 50.0);
    CHECK(lowest < 0.15f * steadyPeak);
    CHECK(std::fabs(back) < 0.2);
    CHECK(jump <= 1.1f * steadyStep);

    p.mute = true;
    g.set(p, eq);
    play(g, kHostBlock);   // the gain ramps down
    p.registerOct = 1;
    g.set(p, eq);
    play(g, kHostBlock);
    CHECK(g.note() == 35 && g.pitch() == 35.0);
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
        CHECK(g.sounding() && !g.audible());   // still on (unmuted, it is there), but nothing to hear
    }
    // The send is the output times spaceSend.
    af::Ground g;
    g.seed(7);
    g.set(af::GroundPatch{}, af::HarmonyPatch{});
    CHECK(!g.audible());
    g.setTarget(50);
    CHECK(g.audible());
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

// Body: a harmonic on a formant is lifted (4.7 dB at most), one far under the formants takes the
// dry's share (0.75, -2.5 dB), and the most-lifted harmonic moves down a -> o -> u, each time within
// a harmonic of the vowel's first formant (800, 450, 350 Hz). Gains per harmonic of a saw drone on
// C2 against Body 0 with the same seed: the filters are linear, so the ratio is their response.
void testBody() {
    std::printf("== ground: Body\n");
    af::GroundPatch p = steady(af::TB_SAW, 1.0f, 0.0f);
    p.cutoffHz = 16000.0f;
    p.width = 0.0f;
    auto render = [&p](float body) {
        p.body = body;
        af::Ground g;
        g.seed(34);
        g.set(p, af::HarmonyPatch{});
        g.setTarget(48);
        return play(g, 3 * kSec / 2).L;
    };
    const Buf dry = render(0.0f);
    const double f0 = toneHz(36.0);   // C2, the tonic: Just leaves it where Equal has it
    const float bodies[3] = {1.0f / 3.0f, 2.0f / 3.0f, 1.0f};
    const double first[3] = {800.0, 450.0, 350.0};
    const char* const vowel[3] = {"a", "o", "u"};
    const size_t from = kSec / 2;
    double before = 1e9;
    bool down = true;
    for (int v = 0; v < 3; ++v) {
        const Buf wet = render(bodies[v]);
        auto gain = [&](double hz) { return db(magnitude(wet, hz, from) / magnitude(dry, hz, from)); };
        double top = -100.0, topHz = 0.0;
        for (int k = 1; k * f0 < 1500.0; ++k) {
            const double gk = gain(k * f0);
            if (gk > top) {
                top = gk;
                topHz = k * f0;
            }
        }
        const double low = gain(2.0 * f0);
        std::printf("  \"%s\" (Body %.2f): the most lifted %.0f Hz, %+.2f dB; %.0f Hz %+.2f dB\n", vowel[v], bodies[v], topHz,
                    top, 2.0 * f0, low);
        CHECK(top > 3.0 && top < 4.9);
        CHECK(std::fabs(topHz - first[v]) <= f0);
        CHECK(std::fabs(low - db(0.75)) < 0.5);
        down = down && topHz < before;
        before = topHz;
    }
    CHECK(down);
}

// Breath: a sine at breathHz moving the level by +-3 dB times breath and the cutoff by +-1 octave
// times breath. The level: a pure drone against a twin without Breath (the same seed, the same
// phases), its gain in 10 ms windows; the swing's top and bottom half a cycle apart. The cutoff:
// a saw drone's 31st harmonic (2.03 kHz) against its fundamental under a Tone of 1 kHz, swung to
// 500 Hz and 2 kHz, against the twin's; the low-pass's response says what to expect.
double lowPass(double hz, double cutoff) {   // the Tone's response: Q 0.707, through the bilinear map
    const double r = std::tan(kPi * hz / af::kRate) / std::tan(kPi * cutoff / af::kRate);
    return 1.0 / std::sqrt(1.0 + r * r * r * r);
}

void testBreath() {
    std::printf("== ground: Breath\n");
    for (float breath : {1.0f, 0.5f}) {
        af::GroundPatch q = steady(af::TB_SINE, 1.0f, 0.0f);
        q.cutoffHz = 16000.0f;
        af::GroundPatch p = q;
        p.breath = breath;
        p.breathHz = 1.0f;
        af::Ground g, t;
        g.seed(35);
        t.seed(35);
        g.set(p, af::HarmonyPatch{});
        t.set(q, af::HarmonyPatch{});
        g.setTarget(48);
        t.setTarget(48);
        const Buf x = play(g, 2 * kSec + kSec / 10).L, y = play(t, 2 * kSec + kSec / 10).L;
        double hi = -100.0, lo = 100.0;
        size_t hiAt = 0, loAt = 0;
        for (size_t c = kSec / 10; c + 441 <= x.size(); c += 220) {
            const double gdb = db(rms(x, c, c + 441) / rms(y, c, c + 441));
            if (gdb > hi) {
                hi = gdb;
                hiAt = c;
            }
            if (gdb < lo) {
                lo = gdb;
                loAt = c;
            }
        }
        const double apart = std::fmod(std::fabs(static_cast<double>(hiAt) - static_cast<double>(loAt)) / kSec, 1.0);
        std::printf("  Breath %.1f at 1 Hz: the level %+.2f / %+.2f dB, top and bottom %.3f s apart (mod 1 s)\n", breath, hi, lo,
                    apart);
        CHECK(std::fabs(hi - 3.0 * breath) < 0.15 && std::fabs(lo + 3.0 * breath) < 0.15);
        CHECK(std::fabs(apart - 0.5) < 0.03);
    }
    af::GroundPatch q = steady(af::TB_SAW, 1.0f, 0.0f);
    q.cutoffHz = 1000.0f;
    af::GroundPatch p = q;
    p.breath = 1.0f;
    p.breathHz = 0.25f;
    af::Ground g, t;
    g.seed(36);
    t.seed(36);
    g.set(p, af::HarmonyPatch{});
    t.set(q, af::HarmonyPatch{});
    g.setTarget(48);
    t.setTarget(48);
    const Buf x = play(g, 9 * kSec / 2).L, y = play(t, 9 * kSec / 2).L;
    const double f0 = toneHz(36.0), fh = 31.0 * f0;
    double hi = -100.0, lo = 100.0;
    for (size_t c = kSec / 10; c + 2205 <= x.size(); c += 1102) {
        const double r = db(magnitude(x, fh, c, c + 2205) / magnitude(x, f0, c, c + 2205)) -
                         db(magnitude(y, fh, c, c + 2205) / magnitude(y, f0, c, c + 2205));
        hi = std::max(hi, r);
        lo = std::min(lo, r);
    }
    auto rel = [&](double cutoff) {
        return db(lowPass(fh, cutoff) / lowPass(f0, cutoff)) - db(lowPass(fh, 1000.0) / lowPass(f0, 1000.0));
    };
    std::printf("  Breath 1 on a 1 kHz Tone: %.0f Hz against the fundamental %+.1f / %+.1f dB (2 kHz, 500 Hz: %+.1f / %+.1f)\n",
                fh, hi, lo, rel(2000.0), rel(500.0));
    CHECK(std::fabs(hi - rel(2000.0)) < 1.0 && std::fabs(lo - rel(500.0)) < 1.0);
}

// Width: at 0, L and R are the same, sample for sample; at 1 each partial sits where kPan puts it,
// by the equal-power law, unity in the middle: Sub in the middle, Root and Octave left, Fifth and
// Color right, L^2 + R^2 twice what the partial has in the middle (at Width 0, the same phases).
void testWidth() {
    std::printf("== ground: Width\n");
    {
        af::GroundPatch p;   // every moving part
        p.color = 0.5f;
        p.body = 0.5f;
        p.width = 0.0f;
        p.fadeS = 0.05f;
        af::Ground g;
        g.seed(37);
        g.set(p, af::HarmonyPatch{});
        g.setTarget(50);
        const Out o = play(g, kSec);
        CHECK(o.L == o.R && o.sendL == o.sendR && peak(o.L) > 0.01f);
    }
    const char* const name[af::Ground::PT_COUNT] = {"Sub", "Root", "Fifth", "Octave", "Color"};
    for (int i = 0; i < af::Ground::PT_COUNT; ++i) {
        af::GroundPatch p = steady(af::TB_SINE, 0.0f, 0.0f);
        partialLevel(p, i) = 1.0f;
        p.cutoffHz = 16000.0f;
        auto render = [&p](float width) {
            p.width = width;
            af::Ground g;
            g.seed(38);
            g.set(p, af::HarmonyPatch{});
            g.setTarget(48);
            return play(g, kSec);
        };
        const Out o = render(1.0f);
        const double l = rms(o.L, kSec / 2), r = rms(o.R, kSec / 2), middle = rms(render(0.0f).L, kSec / 2);
        const double a = (af::Ground::kPan[i] + 1.0) * kPi / 4.0;
        const double want = i == af::Ground::PT_SUB ? 0.0 : db(std::cos(a) / std::sin(a));
        std::printf("  %-6s at %+.1f: L over R %+.2f dB (the law %+.2f), power %.4f of the middle's\n", name[i],
                    af::Ground::kPan[i], db(l / r), want, (l * l + r * r) / (2.0 * middle * middle));
        CHECK(std::fabs(db(l / r) - want) < 0.05);
        CHECK(std::fabs((l * l + r * r) / (2.0 * middle * middle) - 1.0) < 0.01);
    }
}

// Tone: a gentle low-pass (Q 0.707), flat under the cutoff, -3 dB at it, 12 dB an octave above it:
// a saw drone on C2 under a Tone of 1 kHz against 16 kHz, harmonic by harmonic.
// A jump of the Tone glides across its step: from 40 Hz to 16 kHz and back, switched where the
// drone under the two cutoffs differs most, it steps no further than it moves anyway. Twins with
// each cutoff throughout (the same seed, the same phases) say where that is.
void toneJump(float from, float to) {
    af::GroundPatch a = steady(af::TB_CELLO_TASTO, 1.0f, 0.7f);
    a.sub = a.octave = a.color = 0.5f;
    a.cutoffHz = from;
    af::GroundPatch b = a;
    b.cutoffHz = to;
    af::Ground g, ta, tb;
    for (af::Ground* x : {&g, &ta, &tb}) x->seed(46);
    g.set(a, af::HarmonyPatch{});
    ta.set(a, af::HarmonyPatch{});
    tb.set(b, af::HarmonyPatch{});
    for (af::Ground* x : {&g, &ta, &tb}) x->setTarget(45);
    const Buf g0 = play(g, kSec).L, a0 = play(ta, kSec).L, b0 = play(tb, kSec).L;
    const size_t half = a0.size() / 2;
    const float steadyStep = std::max(maxStep(a0, half), maxStep(b0, half));
    float apart = 0.0f;
    for (size_t i = half; i < a0.size(); ++i) apart = std::max(apart, std::fabs(a0[i] - b0[i]));
    float lg = g0.back(), la = a0.back(), lb = b0.back();
    for (int k = 0; k < 2 * kSec / kHostBlock && std::fabs(la - lb) < 0.6f * apart; ++k) {
        lg = play(g, kHostBlock).L.back();
        la = play(ta, kHostBlock).L.back();
        lb = play(tb, kHostBlock).L.back();
    }
    CHECK(std::fabs(la - lb) >= 0.6f * apart && std::fabs(la - lb) > 5.0f * steadyStep);   // a step would show
    g.set(b, af::HarmonyPatch{});
    const Buf g1 = play(g, kSec / 5).L, b1 = play(tb, kSec / 5).L;
    const float jump = std::max(maxStep(g1), std::fabs(g1[0] - lg));
    float end = 0.0f;   // 150 ms on: what the filter held at the jump has died away (at 40 Hz, slowly)
    for (size_t i = 3 * kSec / 20; i < g1.size(); ++i) end = std::max(end, std::fabs(g1[i] - b1[i]));
    std::printf("  Tone %.0f -> %.0f Hz: largest step %.5f against %.5f steady (the cutoffs %.3f apart there); %.1e off "
                "the twin 150 ms on\n", from, to, jump, steadyStep, std::fabs(la - lb), end);
    CHECK(jump <= 3.0f * steadyStep);
    CHECK(end < 1e-5f);
}

void testTone() {
    std::printf("== ground: Tone\n");
    toneJump(40.0f, 16000.0f);
    toneJump(16000.0f, 40.0f);
    af::GroundPatch p = steady(af::TB_SAW, 1.0f, 0.0f);
    p.width = 0.0f;
    auto render = [&p](float cutoff) {
        p.cutoffHz = cutoff;
        af::Ground g;
        g.seed(39);
        g.set(p, af::HarmonyPatch{});
        g.setTarget(48);
        return play(g, 3 * kSec / 2).L;
    };
    const Buf a = render(1000.0f), b = render(16000.0f);
    const double f0 = toneHz(36.0);
    std::printf(" ");
    for (int k : {4, 15, 31, 61}) {
        const double f = k * f0;
        const double got = db(magnitude(a, f, kSec / 2) / magnitude(b, f, kSec / 2));
        const double want = db(lowPass(f, 1000.0) / lowPass(f, 16000.0));
        std::printf(" %.0f Hz %+.2f dB (%+.2f)", f, got, want);
        CHECK(std::fabs(got - want) < 0.2);
    }
    std::printf("\n");
}

// Odd numbers in every parameter: NaN and the infinities in each float of the patch, the
// harmony's and the patch's ints out of range, a NaN send. Every sample is finite, and a sane
// patch afterwards plays.
void testOddParameters() {
    std::printf("== ground: NaN and infinite parameters\n");
    const float nan = std::nanf(""), inf = std::numeric_limits<float>::infinity();
    bool finite = true, plays = true;
    float top = 0.0f;
    for (float v : {nan, inf, -inf}) {
        af::GroundPatch p;
        p.level = p.cutoffHz = p.beatHz = p.gravityS = p.fadeS = v;
        p.sub = p.root = p.fifth = p.octave = p.color = v;
        p.body = p.breath = p.breathHz = p.width = v;
        p.pos = af::LifePos{v, v, v, v};
        p.table = 999;
        p.colorInterval = -3;
        p.registerOct = 77;
        af::HarmonyPatch h;
        h.key = -5;
        h.tuning = 99;
        af::Ground g;
        g.seed(41);
        g.set(p, h);
        g.setTarget(50);
        const Out o = play(g, kSec, nan);
        finite = finite && allFinite(o.L) && allFinite(o.R) && allFinite(o.sendL) && allFinite(o.sendR);
        top = std::max({top, peak(o.L), peak(o.R)});
        g.set(af::GroundPatch{}, af::HarmonyPatch{});
        const Out s = play(g, 5 * kSec);   // the default fade is 4 s
        finite = finite && allFinite(s.L) && allFinite(s.R) && allFinite(s.sendL) && allFinite(s.sendR);
        plays = plays && peak(s.L, 4 * kSec) > 0.01f;
    }
    std::printf("  peak %.3f\n", top);
    CHECK(finite);
    CHECK(top <= 4.0f);
    CHECK(plays);
}

// The odd sequences. A new root while it glides goes on from where the root is now, the octave
// chosen from there (from the goal it would be another); Gravity 0 mid-glide lands at once. Two
// table changes within 20 ms fade one after the other, never stepping, and end on the second. A
// fade-out that ends while muted ends it: unmuted, silence until a new target, which starts afresh.
// A mute and an unmute ramp, and unmuted the drone is where it would have been.
void testOddSequences() {
    std::printf("== ground: odd sequences\n");
    af::HarmonyPatch eq;
    eq.tuning = af::TU_EQUAL;
    {
        af::GroundPatch p = steady(af::TB_SINE, 1.0f, 0.0f);
        p.gravityS = 2.0f;
        af::Ground g;
        g.seed(42);
        g.set(p, eq);
        g.setTarget(48);   // C2, 36
        const Out a = play(g, kSec / 5);
        g.setTarget(52);   // E: 40
        play(g, kSec / 2);
        const double now = g.pitch();
        g.setTarget(57);   // A: 33 from where the root is (38.1), 45 from the goal (40)
        const Out b = play(g, kHostBlock);
        std::printf("  re-target mid-glide at %.2f: to %d, %.3f one block on\n", now, g.note(), g.pitch());
        CHECK(g.note() == 33);
        CHECK(std::fabs(g.pitch() - now) < 0.05);
        CHECK(maxStep(b.L) <= 1.3f * maxStep(a.L, a.L.size() / 2));
        p.gravityS = 0.0f;
        g.set(p, eq);
        play(g, kHostBlock);
        CHECK(g.pitch() == 33.0);
    }
    {
        af::GroundPatch a = steady(af::TB_CELLO_TASTO, 1.0f, 0.7f);
        a.sub = a.octave = a.color = 0.5f;
        a.cutoffHz = 16000.0f;
        af::GroundPatch b = a, c = a;
        b.table = af::TB_CHOIR_AH_OO;
        c.table = af::TB_TAPE_STRINGS;
        af::Ground g, tc;
        g.seed(43);
        tc.seed(43);
        g.set(a, af::HarmonyPatch{});
        tc.set(c, af::HarmonyPatch{});
        g.setTarget(45);
        tc.setTarget(45);
        const Buf g0 = play(g, kSec).L, c0 = play(tc, kSec).L;
        const float steadyStep = std::max(maxStep(g0, g0.size() / 2), maxStep(c0, c0.size() / 2));
        g.set(b, af::HarmonyPatch{});
        const Buf g1 = play(g, kSec / 100).L;   // 10 ms into the first fade
        play(tc, kSec / 100);
        g.set(c, af::HarmonyPatch{});
        const Buf g2 = play(g, kSec / 10).L, c2 = play(tc, kSec / 10).L;
        const float jump = std::max({maxStep(g1), maxStep(g2), std::fabs(g1[0] - g0.back()), std::fabs(g2[0] - g1.back())});
        float end = 0.0f;
        for (size_t i = kSec / 20; i < g2.size(); ++i) end = std::max(end, std::fabs(g2[i] - c2[i]));
        std::printf("  two table changes 10 ms apart: largest step %.5f against %.5f steady; %.1e off the second "
                    "table 50 ms on\n", jump, steadyStep, end);
        CHECK(jump <= 3.0f * steadyStep);
        CHECK(end < 1e-5f);
    }
    {
        af::GroundPatch p = steady(af::TB_SINE, 1.0f, 0.0f);
        p.fadeS = 0.5f;
        af::Ground g;
        g.seed(44);
        g.set(p, af::HarmonyPatch{});
        g.setTarget(48);
        const float full = peak(play(g, kSec).L, kSec / 2);
        p.mute = true;
        g.set(p, af::HarmonyPatch{});
        g.setTarget(-1);
        play(g, 3 * kSec / 5);
        CHECK(!g.sounding() && !g.audible());
        p.mute = false;
        g.set(p, af::HarmonyPatch{});
        const Out o = play(g, kSec / 5);
        CHECK(peak(o.L) == 0.0f && !g.sounding());
        g.setTarget(48);
        CHECK(g.sounding() && g.audible());
        const Out s = play(g, kSec / 10);
        std::printf("  a fade-out ended while muted; the next target starts at %.1f dB\n", db(peak(s.L, 0, 441) / full));
        CHECK(peak(s.L, 0, 441) < 0.01f * full);   // from the bottom of the fade, not where it was
    }
    {
        af::GroundPatch p;   // the default drone: Breath and Sway moving
        p.fadeS = 0.05f;
        af::Ground g, t;
        g.seed(45);
        t.seed(45);
        g.set(p, af::HarmonyPatch{});
        t.set(p, af::HarmonyPatch{});
        g.setTarget(50);
        t.setTarget(50);
        const Out a = play(g, kSec);
        play(t, kSec);
        const float steadyStep = maxStep(a.L, a.L.size() / 2);
        p.mute = true;
        g.set(p, af::HarmonyPatch{});
        const Out m = play(g, kSec / 2);
        play(t, kSec / 2);
        const float out = std::max(maxStep(m.L, 0, kHostBlock + 1), std::fabs(m.L[0] - a.L.back()));
        CHECK(peak(m.L, af::kChunk) == 0.0f);   // after the step's ramp: nothing
        p.mute = false;
        g.set(p, af::HarmonyPatch{});
        const Out u = play(g, kSec / 10), ut = play(t, kSec / 10);
        const float in = std::max(maxStep(u.L), std::fabs(u.L[0]));
        float back = 0.0f;
        for (size_t i = kSec / 20; i < u.L.size(); ++i) back = std::max(back, std::fabs(u.L[i] - ut.L[i]));
        std::printf("  mute and unmute: steps %.5f / %.5f against %.5f steady; %.1e off the unmuted twin 50 ms on\n", out,
                    in, steadyStep, back);
        CHECK(out <= 3.0f * steadyStep && in <= 3.0f * steadyStep);
        CHECK(back < 1e-6f);
    }
    {   // The engine's idle gate: render() skipped while !audible(), zeros in its place. A mute
        // leaves it audible until the step that ramps it down has run.
        af::GroundPatch p;
        p.fadeS = 0.05f;
        af::Ground g;
        g.seed(47);
        g.set(p, af::HarmonyPatch{});
        g.setTarget(50);
        Buf x = play(g, kSec).L;
        const float steadyStep = maxStep(x, x.size() / 2);
        auto block = [&g, &x]() {   // one block as the engine would run it
            const Out o = g.audible() ? play(g, kHostBlock) : Out{Buf(kHostBlock), Buf(kHostBlock), Buf(kHostBlock), Buf(kHostBlock)};
            x.insert(x.end(), o.L.begin(), o.L.end());
        };
        p.mute = true;
        g.set(p, af::HarmonyPatch{});
        const bool rampsFirst = g.audible();
        block();
        const bool thenIdle = !g.audible();
        for (int b = 0; b < kSec / 2 / kHostBlock; ++b) block();
        p.mute = false;
        g.set(p, af::HarmonyPatch{});
        const bool back = g.audible();
        for (int b = 0; b < kSec / 10 / kHostBlock; ++b) block();
        const float jump = maxStep(x, static_cast<size_t>(kSec - 1));
        std::printf("  render skipped while not audible: largest step %.5f against %.5f steady\n", jump, steadyStep);
        CHECK(rampsFirst && thenIdle && back);
        CHECK(jump <= 3.0f * steadyStep);
        p.level = 0.0f;   // level 0 the same way
        g.set(p, af::HarmonyPatch{});
        CHECK(g.audible());
        block();
        CHECK(!g.audible() && g.sounding());
    }
}

// Check 6: Tone at both ends, Body 1 and Breath 1 for a minute: finite and at most 4.
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
}

// The headroom: with every partial at 1, level 1, Breath 1 and Width 0, Ground peaks under 1.5 at
// any root. A sweep: every table; the 12 pitch classes in register 3 (where Body's formants catch
// the strongest harmonics) and C, E and G# in registers 1 and 2; Body 0, 2/3 and 1; the 4th and the
// m7 in turn. A quarter of a second each, so Breath runs at 5 Hz (a crest in every render) and
// every render has its own seed (its own alignment of the partials). The long sweep behind
// kHeadroom (ground.cpp) found 1.26; this one, shorter, finds a little less.
void testHeadroom() {
    std::printf("== ground: headroom\n");
    const auto start = std::chrono::steady_clock::now();
    struct Root {
        int reg, pc;
    };
    std::vector<Root> roots;
    for (int pc = 0; pc < 12; ++pc) roots.push_back({3, pc});
    for (int reg : {1, 2})
        for (int pc : {0, 4, 8}) roots.push_back({reg, pc});
    float worst = 0.0f, worstBody = 0.0f;
    int worstTable = 0, worstReg = 0, worstPc = 0, renders = 0;
    bool finite = true;
    uint32_t seed = 100;
    float L[kHostBlock], R[kHostBlock], sL[kHostBlock], sR[kHostBlock];
    for (int table = 0; table < af::TB_COUNT; ++table)
        for (size_t k = 0; k < roots.size(); ++k)
            for (float body : {0.0f, 2.0f / 3.0f, 1.0f}) {
                af::GroundPatch p;
                p.table = table;
                p.level = 1.0f;
                p.sub = p.root = p.fifth = p.octave = p.color = 1.0f;
                p.colorInterval = k % 2 ? af::CI_MIN7 : af::CI_FOURTH;
                p.registerOct = roots[k].reg;
                p.body = body;
                p.breath = 1.0f;
                p.breathHz = 5.0f;
                p.beatHz = 3.0f;
                p.width = 0.0f;
                p.fadeS = 0.05f;
                af::Ground g;
                g.seed(seed++);
                g.set(p, af::HarmonyPatch{});
                g.setTarget(roots[k].pc);
                float pk = 0.0f;
                for (int b = 0; b < kSec / 4 / kHostBlock; ++b) {
                    std::fill(L, L + kHostBlock, 0.0f);
                    std::fill(R, R + kHostBlock, 0.0f);
                    g.render(tables(), L, R, sL, sR, 0.5f, kHostBlock);
                    for (int i = 0; i < kHostBlock; ++i) {
                        finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
                        pk = std::max({pk, std::fabs(L[i]), std::fabs(R[i])});
                    }
                }
                ++renders;
                if (pk > worst) {
                    worst = pk;
                    worstTable = table;
                    worstReg = roots[k].reg;
                    worstPc = roots[k].pc;
                    worstBody = body;
                }
            }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("  %d renders: the loudest %.3f (%s, register %d, pitch class %d, Body %.2f) (%.1f s)\n", renders, worst,
                af::tableName(worstTable), worstReg, worstPc, worstBody, secs);
    CHECK(finite);
    CHECK(worst <= 1.5f);
    CHECK(worst > 0.5f);   // only that the sweep reaches the loud cases at all (it finds about 1.2)

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
    testImages();
    testBeat();
    testGravity();
    testOctaves();
    testRegisterChange();
    testFade();
    testGains();
    testNoSteps();
    testTableSwitch();
    testBody();
    testBreath();
    testWidth();
    testTone();
    testOddParameters();
    testOddSequences();
    testStability();
    testHeadroom();
    testDeterminism();
}

} // namespace aft
