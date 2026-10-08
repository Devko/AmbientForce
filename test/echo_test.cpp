// From EffectForce test/delay_test.cpp (3ae8a4d), eft -> aft; Diffuse and Echo added.
// dsp/delay.h: echo timing against theory (free, synced at several tempos, spread, Ping-Pong), the
// feedback ratio and the cuts against the one-pole responses, the glide landing exactly, drive,
// limiter, ducking, wow depth, and robustness (feedback 1 for 30 s, extremes, random jumps, NaN
// input, block sizes, reset, the tail). Then AmbientForce's own: Diffuse (0 is EffectForce's Delay
// bit for bit, the smear, what a pass loses, the comb a long tail narrows to, no growth, Mono,
// clicks, block sizes, odd values, the tail) and dsp/echo.h (wet only, silent() within the Delay's
// reach, nothing old when it runs again after a silence, however the silence began, and the duck's
// envelope kept across it).
#include "signal.h"
#include "../dsp/delay.h"
#include "../dsp/echo.h"

#include <complex>
#include <cstring>

namespace aft {
namespace {

using af::Delay;
using P = Delay::Params;

constexpr int kSr = 44100;

// A plain delay to measure: wet only, no feedback, the cuts wide open, no wow, drive or duck.
P plain(float ms) {
    P p;
    p.mode = Delay::STEREO;
    p.sync = false;
    p.timeMs = ms;
    p.feedback = 0.0f;
    p.spread = 0.0f;
    p.lowCutHz = 20.0f;
    p.highCutHz = 20000.0f;
    p.wow = 0.0f;
    p.drive = 0.0f;
    p.duck = 0.0f;
    p.mix = 1.0f;
    return p;
}
P synced(double beats) {
    P p = plain(375.0f);
    p.sync = true;
    p.divBeats = beats;
    return p;
}
af::Transport tempo(double bpm) {
    af::Transport t;
    t.bpm = bpm;
    return t;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool same(const Buf& a, const Buf& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }
size_t loudest(const Buf& x, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    size_t best = from;
    for (size_t i = from; i < to; ++i)
        if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
    return best;
}
Buf silence(int n) { return Buf(static_cast<size_t>(n), 0.0f); }
Buf concat(Buf a, const Buf& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Runs p for the first `at` samples, then q, in chunks of `chunk`.
void runSplit(Delay& d, const P& p, const P& q, Buf& L, Buf& R, size_t at, af::Transport t = {}, int chunk = af::kChunk) {
    Buf l1(L.begin(), L.begin() + static_cast<std::ptrdiff_t>(at)), r1(R.begin(), R.begin() + static_cast<std::ptrdiff_t>(at));
    Buf l2(L.begin() + static_cast<std::ptrdiff_t>(at), L.end()), r2(R.begin() + static_cast<std::ptrdiff_t>(at), R.end());
    run(d, p, l1, r1, t, chunk);
    run(d, q, l2, r2, t, chunk);
    L = concat(l1, l2);
    R = concat(r1, r2);
}

// A sine through a sine fades in over 10 ms (its own start is no click to measure).
Buf fadedSine(double hz, int n, float amp, double phase = 0.0) {
    Buf x = sine(hz, n, amp, phase);
    for (int i = 0; i < 441; ++i) x[static_cast<size_t>(i)] *= static_cast<float>(i) / 441.0f;
    return x;
}

// The cuts (dsp/delay.h): a = 1 - exp(-2 pi fc / rate), low-pass a / (1 - (1 - a) z^-1),
// high-pass (1 - a)(1 - z^-1) / (1 - (1 - a) z^-1); a high cut of 20 kHz is off.
double coef(double hz) { return 1.0 - std::exp(-2.0 * kPi * hz / af::kRate); }
std::complex<double> zInv(double hz) { return std::polar(1.0, -2.0 * kPi * hz / af::kRate); }
std::complex<double> lowPass(double fc, double hz) {
    if (fc >= 20000.0) return 1.0;
    const double a = coef(fc);
    return a / (1.0 - (1.0 - a) * zInv(hz));
}
std::complex<double> highPass(double fc, double hz) {
    const double a = coef(fc);
    return (1.0 - a) * (1.0 - zInv(hz)) / (1.0 - (1.0 - a) * zInv(hz));
}
double h0() { return 1.0 - coef(20.0); }   // the 20 Hz high-pass's first sample

// An impulse at `at` read back `delay` samples later through the 4-point Hermite kernel and the
// 20 Hz high-pass, in double: what one echo of the plain delay must look like.
Buf echoOf(int n, int at, double delay, double amp = 1.0) {
    const int i = static_cast<int>(delay);
    const double f = delay - i;
    std::vector<double> x(static_cast<size_t>(n), 0.0);
    const double w[4] = {(-f * f * f + 2 * f * f - f) / 2, (3 * f * f * f - 5 * f * f + 2) / 2, (-3 * f * f * f + 4 * f * f + f) / 2,
                         (f * f * f - f * f) / 2};
    for (int k = 0; k < 4; ++k) {
        const int pos = at + i - 1 + k;
        if (pos >= 0 && pos < n) x[static_cast<size_t>(pos)] += amp * w[k];
    }
    const double a = coef(20.0);
    double s = 0.0;
    Buf y(static_cast<size_t>(n));
    for (size_t k = 0; k < y.size(); ++k) {
        s += a * (x[k] - s);
        y[k] = static_cast<float>(x[k] - s);
    }
    return y;
}

// The plain delay's answer to an impulse, L and R against echoOf() at their delays.
bool echoes(Delay& d, const P& p, af::Transport t, double delayL, double delayR) {
    const int at = 64, n = at + static_cast<int>(std::max(delayL, delayR)) + 64;
    d.reset();
    Buf L = impulseAt(n, at), R = L;
    run(d, p, L, R, t);
    const Buf el = echoOf(n, at, delayL), er = echoOf(n, at, delayR);
    double worst = 0.0;
    for (size_t i = 0; i < L.size(); ++i) worst = std::max({worst, std::fabs(static_cast<double>(L[i]) - el[i]), std::fabs(static_cast<double>(R[i]) - er[i])});
    bool ok = worst < 1e-5;
    if (delayL == std::floor(delayL)) ok = ok && loudest(L) == static_cast<size_t>(at + delayL) && near(L[loudest(L)], h0(), 1e-6);
    if (!ok) std::printf("  echo %.6f / %.6f: off by %g\n", delayL, delayR, worst);
    return ok;
}

void timing() {
    Delay d;
    // Free: timeMs later, between samples as the Hermite kernel puts it.
    for (float ms : {1.0f, 2.5f, 10.0f, 250.0f, 333.3f, 1000.0f, 2000.0f}) {
        const double delay = static_cast<double>(ms) * kSr / 1000.0;
        CHECK(echoes(d, plain(ms), {}, delay, delay));
    }
    // Synced: beats x 60 / BPM seconds, at several tempos; whole numbers of samples are exact.
    const double cases[][2] = {{0.5, 120}, {1.5, 90},   {1.0 / 6, 100}, {4.0, 60},  {4.0, 30},
                               {0.0625, 175}, {0.75, 128}, {1.0 / 3, 140}, {2.0, 87}, {1.0 / 12, 300}};
    for (const auto& c : cases) {
        const double delay = c[0] * 60.0 / c[1] * kSr;
        CHECK(echoes(d, synced(c[0]), tempo(c[1]), delay, delay));
    }
    // The longest: 1 bar at 30 BPM, 8 s. Slower is clamped there.
    CHECK(echoes(d, synced(4.0), tempo(20.0), 8.0 * kSr, 8.0 * kSr));
    // Out of range: 1 ms and 2 s.
    CHECK(echoes(d, plain(0.01f), {}, 44.1, 44.1));
    CHECK(echoes(d, plain(5000.0f), {}, 2.0 * kSr, 2.0 * kSr));
}

void spread() {
    // R's time is L's x (1 + spread); L stays where it was.
    Delay d;
    for (float s : {-0.5f, -0.25f, 0.3f, 0.5f}) {
        P p = plain(200.0f);
        p.spread = s;
        CHECK(echoes(d, p, {}, 8820.0, 8820.0 * (1.0 + s)));
        P q = synced(0.5);
        q.spread = s;
        CHECK(echoes(d, q, tempo(120.0), 11025.0, 11025.0 * (1.0 + s)));
    }
    // Stereo keeps the sides apart: L's input never comes out on R.
    d.reset();
    P p = plain(50.0f);
    p.feedback = 0.7f;
    Buf L = whiteNoise(20000, 0.5f, 3), R = silence(20000);
    run(d, p, L, R);
    CHECK(peak(R) < 1e-12f && peak(L) > 0.1f);
}

void pingPong() {
    Delay d;
    const int t = 4410;
    const double h = h0();
    // The mono sum enters L; the repeats alternate L, R, L..., each fb times the last.
    {
        P p = plain(100.0f);
        p.mode = Delay::PING_PONG;
        p.feedback = 0.5f;
        Buf L = impulseAt(7 * t + 100, 0), R = L;
        run(d, p, L, R);
        for (int k = 1; k <= 6; ++k) {
            const Buf& on = k % 2 ? L : R;
            const Buf& off = k % 2 ? R : L;
            const size_t at = static_cast<size_t>(k * t);
            CHECK(near(on[at], std::pow(0.5, k - 1) * std::pow(h, k), 1e-5));
            CHECK(std::fabs(off[at]) < 1e-6);
            CHECK(loudest(on, at - t / 2, at + t / 2) == at);
        }
        CHECK(peak(L, 0, t) < 1e-6f && peak(R, 0, 2 * t) < 1e-6f);
    }
    // A side alone counts half (the mono sum), and still starts on L.
    {
        d.reset();
        P p = plain(100.0f);
        p.mode = Delay::PING_PONG;
        p.feedback = 0.5f;
        Buf L = silence(3 * t), R = impulseAt(3 * t, 0);
        run(d, p, L, R);
        CHECK(near(L[t], 0.5 * h, 1e-5) && std::fabs(R[t]) < 1e-6);
        CHECK(near(R[2 * t], 0.25 * h * h, 1e-5) && std::fabs(L[2 * t]) < 1e-6);
    }
    // With spread, R's turns take L's time x (1 + spread): L at t, R at 2.5 t, L at 3.5 t, R at 5 t.
    {
        d.reset();
        P p = plain(100.0f);
        p.mode = Delay::PING_PONG;
        p.feedback = 0.5f;
        p.spread = 0.5f;
        Buf L = impulseAt(6 * t, 0), R = L;
        run(d, p, L, R);
        const int times[] = {t, t * 5 / 2, t * 7 / 2, 5 * t};
        for (int k = 0; k < 4; ++k) {
            const Buf& on = k % 2 ? R : L;
            const size_t at = static_cast<size_t>(times[k]);
            CHECK(loudest(on, at - t / 2, at + t / 2) == at);
            CHECK(near(on[at], std::pow(0.5, k) * std::pow(h, k + 1), 1e-5));
        }
    }
}

void mono() {
    // Mono: both sides the same, the echo of the mono sum, whatever the spread and the wow.
    Delay d;
    P p = plain(120.0f);
    p.mode = Delay::MONO;
    p.feedback = 0.6f;
    p.spread = 0.4f;
    p.wow = 0.7f;
    p.drive = 0.5f;
    p.lowCutHz = 150.0f;
    p.highCutHz = 6000.0f;
    const Buf inL = whiteNoise(30000, 0.5f, 5), inR = sine(330.0, 30000, 0.5f);
    Buf L = inL, R = inR;
    run(d, p, L, R);
    CHECK(same(L, R));
    // The same as Stereo without spread fed the mono sum on both sides.
    Buf sum(inL.size());
    for (size_t i = 0; i < sum.size(); ++i) sum[i] = 0.5f * inL[i] + 0.5f * inR[i];
    Delay s;
    P q = p;
    q.mode = Delay::STEREO;
    q.spread = 0.0f;
    Buf A = sum, B = sum;
    run(s, q, A, B);
    CHECK(same(A, L));
    // Through a Fade too.
    P f = p;
    f.glide = Delay::FADE;
    P g = f;
    g.timeMs = 170.0f;
    Delay m;
    Buf C = inL, D = inR;
    runSplit(m, f, g, C, D, 10000);
    CHECK(same(C, D));
}

void feedbackRatio() {
    // Repeat k of an impulse is fb^(k-1) h0^k of it: the cuts wide open still leave the 20 Hz
    // high-pass's first sample h0 per pass. Feedback 1 holds.
    Delay d;
    const int t = 4410;
    const double h = h0();
    for (float fb : {0.25f, 0.6f, 0.9f, 1.0f}) {
        P p = plain(100.0f);
        p.feedback = fb;
        d.reset();
        Buf L = impulseAt(7 * t + 10, 0, 0.5f), R = L;
        run(d, p, L, R);
        for (int k = 1; k <= 6; ++k) {
            const double expect = 0.5 * std::pow(fb, k - 1) * std::pow(h, k);
            CHECK(near(L[static_cast<size_t>(k * t)], expect, 1e-5 * expect + 1e-8));
        }
    }
}

void cuts() {
    // Repeat n of a tone has been through the cuts n times and the feedback n - 1 times:
    // fb^(n-1) |H_lp H_hp|^n. Tone bursts 0.3 s long, repeats 0.5 s apart.
    struct Case {
        float lowCut, highCut;
        double hz;
    };
    const Case cases[] = {{1000.0f, 20000.0f, 300.0}, {2000.0f, 20000.0f, 700.0}, {20.0f, 2000.0f, 6000.0},
                          {20.0f, 500.0f, 2000.0},    {300.0f, 3000.0f, 1000.0},  {100.0f, 8000.0f, 12000.0}};
    const int t = 22050, len = 13230;
    Delay d;
    for (const Case& c : cases) {
        P p = plain(500.0f);
        p.feedback = 0.8f;
        p.lowCutHz = c.lowCut;
        p.highCutHz = c.highCut;
        Buf burst = sine(c.hz, len, 0.5f);
        for (int i = 0; i < len; ++i) burst[static_cast<size_t>(i)] *= static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / len));
        Buf L = concat(burst, silence(4 * t)), R = L;
        d.reset();
        run(d, p, L, R);
        const double ref = magnitude(burst, c.hz);
        const double once = std::abs(lowPass(c.highCut, c.hz) * highPass(c.lowCut, c.hz));
        double last = 0.0;
        for (int k = 1; k <= 3; ++k) {
            const size_t from = static_cast<size_t>(k * t);
            const double got = db(magnitude(L, c.hz, from, from + len) / ref);
            const double expect = db(std::pow(0.8, k - 1) * std::pow(once, k));
            if (!near(got, expect, 0.2)) std::printf("  cuts %g / %g at %g Hz, repeat %d: %.2f dB, expected %.2f\n", c.lowCut, c.highCut, c.hz, k, got, expect);
            CHECK(near(got, expect, 0.2));
            if (k > 1) CHECK(got < last - 1.0);   // each repeat further down
            last = got;
        }
    }
}

void glide() {
    // 120 -> 60 BPM at 1/8: the time glides from 11025 to 22050 samples (the probe's 60 ms
    // one-pole, stepped every 32 samples) and lands on 22050 exactly; float would stall short.
    Delay d;
    const P p = synced(0.5);
    Buf L = whiteNoise(kSr, 0.5f, 9), R = L;
    run(d, p, L, R, tempo(120.0));
    CHECK(d.timeSamples(0) == 11025.0 && d.timeSamples(1) == 11025.0);
    const int part = 83 * 32;   // 60.2 ms
    Buf A = silence(part), B = A;
    run(d, p, A, B, tempo(60.0));
    const double expect = 22050.0 - 11025.0 * std::exp(-part / (0.060 * kSr));
    CHECK(near(d.timeSamples(0), expect, 1e-6 * expect));
    Buf C = silence(3 * kSr), D = C;
    run(d, p, C, D, tempo(60.0));
    CHECK(allFinite(C) && allFinite(D));
    CHECK(d.timeSamples(0) == 22050.0 && d.timeSamples(1) == 22050.0);
    // And an impulse now comes back 22050 samples later, on the sample.
    const int n = 22050 + 200;
    Buf E = impulseAt(n, 64), F = E;
    run(d, p, E, F, tempo(60.0));
    const Buf e = echoOf(n, 64, 22050.0);
    double worst = 0.0;
    for (size_t i = 0; i < E.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(E[i]) - e[i]));
    CHECK(worst < 1e-5);
    CHECK(loudest(E) == 64 + 22050 && E[64 + 22050] == F[64 + 22050]);

    // Changing the target every chunk (automation) never lands but never misbehaves.
    d.reset();
    Buf G = whiteNoise(2 * kSr, 0.5f, 10), H = G;
    for (size_t pos = 0; pos < G.size(); pos += af::kChunk) {
        P q = plain(10.0f + 990.0f * static_cast<float>((pos / af::kChunk) % 2));
        q.feedback = 0.7f;
        d.set(q, {});
        d.process(&G[pos], &H[pos], static_cast<int>(std::min<size_t>(af::kChunk, G.size() - pos)));
    }
    CHECK(allFinite(G) && peak(G) < 4.0f);
}

// The plain delay without feedback in double: the drive's shaper on what is written, the 20 Hz
// high-pass on what is read `delay` samples later.
Buf driven(const Buf& x, int delay, double drive) {
    const double a = coef(20.0);
    double s = 0.0;
    Buf y(x.size());
    for (size_t n = 0; n < x.size(); ++n) {
        const double u = n >= static_cast<size_t>(delay) ? x[n - static_cast<size_t>(delay)] : 0.0;
        const double c = std::clamp(u, -0.5, 0.5);
        const double v = u + drive * (c - 4.0 / 3.0 * c * c * c - u);
        s += a * (v - s);
        y[n] = static_cast<float>(v - s);
    }
    return y;
}

void fadeLands() {
    // Fade, 120 -> 60 BPM at 1/8: the old head keeps reading 11025 samples back while the new one
    // at 22050 fades in over 50 ms; then 22050 exactly, and an impulse comes back on the sample.
    Delay d;
    P p = synced(0.5);
    p.glide = Delay::FADE;
    Buf L = whiteNoise(kSr, 0.5f, 9), R = L;
    run(d, p, L, R, tempo(120.0));
    CHECK(d.timeSamples(0) == 11025.0);
    Buf A = silence(30 * 32), B = A;
    run(d, p, A, B, tempo(60.0));
    CHECK(d.timeSamples(0) == 11025.0 && d.timeSamples(1) == 11025.0);   // no glide: still the old head
    Buf C = silence(71 * 32), D = C;
    run(d, p, C, D, tempo(60.0));
    CHECK(d.timeSamples(0) == 22050.0 && d.timeSamples(1) == 22050.0);   // 50 ms on: the new one
    Buf S = silence(22050), T = S;   // the noise out of the line
    run(d, p, S, T, tempo(60.0));
    const int n = 22050 + 200;
    Buf E = impulseAt(n, 64), F = E;
    run(d, p, E, F, tempo(60.0));
    const Buf e = echoOf(n, 64, 22050.0);
    double worst = 0.0;
    for (size_t i = 0; i < E.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(E[i]) - e[i]));
    CHECK(worst < 1e-5 && loudest(E) == 64 + 22050);
    // A change under half a sample snaps: no fade, the new time at once.
    P q = plain(100.0f);
    q.glide = Delay::FADE;
    d.reset();
    Buf G = silence(64), H = G;
    run(d, q, G, H);
    q.timeMs = 100.01f;   // 4410.44
    run(d, q, G, H);
    CHECK(d.timeSamples(0) == static_cast<double>(100.01f) * kSr / 1000.0);
}

// The frequency of x over [from, from + len): zero crossings, placed between samples, per second.
double frequency(const Buf& x, size_t from, size_t len) {
    double first = -1.0, last = -1.0;
    int count = 0;
    for (size_t i = from + 1; i < from + len; ++i) {
        if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / (static_cast<double>(x[i - 1]) - x[i]);
            if (first < 0.0) first = t;
            last = t;
            ++count;
        }
    }
    return count > 1 ? 0.5 * (count - 1) * kSr / (last - first) : 0.0;
}

void fadeNoBend() {
    // A 1 kHz sine, 100% wet, through a tempo change. Tape bends its pitch far; Fade doesn't: with
    // the heads in phase (120 -> 60 BPM at 1/8: 250 cycles apart) not at all, a third of a cycle
    // apart (100 -> 90 BPM) only the crossfade's phase walk, under 12 Hz.
    const double bpms[2][2] = {{120.0, 60.0}, {100.0, 90.0}};
    for (int c = 0; c < 2; ++c) {
        for (int glide : {Delay::TAPE, Delay::FADE}) {
            Delay d;
            P p = synced(0.5);
            p.glide = glide;
            const size_t at = 32 * 1000;   // the change
            Buf L = sine(1000.0, 3 * kSr, 0.5f), R = L;
            Buf l1(L.begin(), L.begin() + at), r1(R.begin(), R.begin() + at), l2(L.begin() + at, L.end()), r2(R.begin() + at, R.end());
            run(d, p, l1, r1, tempo(bpms[c][0]));
            run(d, p, l2, r2, tempo(bpms[c][1]));
            const Buf out = concat(l1, l2);
            double worst = 0.0;
            for (size_t m = at - 4410; m + 441 <= at + 22050; m += 441) worst = std::max(worst, std::fabs(frequency(out, m, 441) - 1000.0));
            if (glide == Delay::TAPE) CHECK(worst > 100.0);
            else CHECK(worst < (c == 0 ? 0.05 : 12.0));
        }
    }
}

void fadeLevel() {
    // Steady noise, 100% wet, 300 -> 400 ms: the two heads read unrelated noise, and the equal-power
    // crossfade keeps the level through the 50 ms (a straight one would dip 3 dB halfway).
    Delay d;
    P p = plain(300.0f);
    p.glide = Delay::FADE;
    P q = p;
    q.timeMs = 400.0f;
    const size_t at = 32 * 1000;
    Buf L = whiteNoise(2 * kSr, 0.5f, 91), R = L;
    runSplit(d, p, q, L, R, at);
    const double before = rms(L, at - 13230, at);
    for (size_t m = at; m < at + 3 * 882; m += 882) CHECK(near(db(rms(L, m, m + 882) / before), 0.0, 1.0));
}

void fadeClicks() {
    // Two 200 Hz sines through the delay, fb 0.5, mix 0.5: one time change, the time moved every
    // chunk (an LFO on it), Tape <-> Fade every chunk under a slower LFO, and switching while
    // the time jumps and Ping-Pong's spread changes. No step beyond twice what a sine at the
    // output's peak makes.
    const int n = 1378 * af::kChunk;
    for (int what = 0; what < 4; ++what) {
        Delay d;
        Buf L = fadedSine(200.0, n, 0.5f), R = fadedSine(200.0, n, 0.5f, 1.0);
        for (int pos = 0, c = 0; pos < n; pos += af::kChunk, ++c) {
            const double sec = static_cast<double>(pos) / kSr;
            P p = plain(100.0f);
            p.feedback = 0.5f;
            p.mix = 0.5f;
            p.glide = Delay::FADE;
            switch (what) {
                case 0: p.timeMs = pos < n / 2 ? 100.0f : 137.0f; break;
                case 1: p.timeMs = static_cast<float>(150.0 + 100.0 * std::sin(2.0 * kPi * 2.0 * sec)); break;
                case 2:
                    p.timeMs = static_cast<float>(150.0 + 20.0 * std::sin(2.0 * kPi * sec));
                    p.glide = c % 2 ? Delay::TAPE : Delay::FADE;
                    break;
                default:
                    p.mode = Delay::PING_PONG;
                    p.timeMs = (c / 37) % 2 ? 180.0f : 100.0f;
                    p.spread = (c / 23) % 2 ? 0.3f : -0.2f;
                    p.glide = (c / 50) % 2 ? Delay::TAPE : Delay::FADE;
                    break;
            }
            d.set(p, {});
            d.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], af::kChunk);
        }
        const double natural = std::max(peak(L), peak(R)) * 2.0 * kPi * 200.0 / kSr;
        CHECK(allFinite(L) && allFinite(R));
        CHECK(std::max(maxStep(L), maxStep(R)) < 2.0 * natural);
    }
}

// Tape's output on three busy scenarios, hashed (FNV-1a over the bits of L, then R).
uint64_t tapeHash(int scenario) {
    Delay d;
    const int n = 3 * kSr;
    Buf L = whiteNoise(n, 0.7f, 81), R = sine(330.0, n, 0.6f);
    for (size_t pos = 0, c = 0; pos < L.size(); pos += af::kChunk, ++c) {
        P p;
        af::Transport t;
        t.bpm = 120.0;
        p.wow = 0.6f;
        p.drive = 0.4f;
        p.duck = 0.5f;
        p.spread = 0.25f;
        if (scenario == 1) {
            p.mode = Delay::PING_PONG;
            p.feedback = 0.8f;
            p.wow = 1.0f;
            p.spread = -0.3f;
            if (pos > 44100) t.bpm = 87.0;
        } else if (scenario == 2) {
            p.mode = static_cast<int>((c / 200) % 3);
            p.sync = false;
            p.timeMs = 5.0f + 1495.0f * static_cast<float>((c * 7919) % 100) / 99.0f * ((c / 10) % 2);
            p.drive = 1.0f;
            p.duck = 1.0f;
            p.feedback = 0.95f;
        }
        d.set(p, t);
        d.process(&L[pos], &R[pos], static_cast<int>(std::min<size_t>(af::kChunk, L.size() - pos)));
    }
    uint64_t h = 1469598103934665603ull;
    for (const Buf* b : {&L, &R})
        for (float v : *b) {
            uint32_t bits;
            std::memcpy(&bits, &v, sizeof bits);
            for (int k = 0; k < 4; ++k) h = (h ^ ((bits >> (8 * k)) & 0xffu)) * 1099511628211ull;
        }
    return h;
}

void tapeUnchanged() {
    // Tape is bit for bit what it was before Fade came: the hashes were taken from that code, on
    // x86 and on the device's build (NEON's reciprocal estimate and GCC's contractions differ; a new
    // compiler may move the device's, not x86's). Not against the profile-guided objects (make
    // test-arm-pgo): their profile moves the contractions with every change of the code, so no hash
    // stays; the plain builds (make test, test-arm) keep the check.
#if AF_PGO_OBJECTS
    std::printf("  (the Tape hash is checked in the plain builds, not against profile-guided objects)\n");
    return;
#endif
#if AF_NEON
    const uint64_t before[3] = {0x690cbedd772c6b55ull, 0x49268bdd94b108f1ull, 0x81639e54566b2b03ull};
#else
    const uint64_t before[3] = {0x6b493ad57ae03251ull, 0xead2494d4735b109ull, 0x557c2696329e4b99ull};
#endif
    for (int k = 0; k < 3; ++k) CHECK(tapeHash(k) == before[k]);
}

void drive() {
    Delay d;
    // The shaper, c - 4/3 c^3 with c = u clamped to +-1/2, blended in by the drive: the echo
    // against the model, from -54 dBFS to -1 dBFS (0 is linear, 1 flattens at 1/3).
    for (float amp : {0.002f, 0.25f, 0.6f, 0.9f}) {
        for (float drv : {0.0f, 0.5f, 1.0f}) {
            P p = plain(50.0f);
            p.drive = drv;
            Buf L = sine(500.0, 20000, amp), R = L;
            const Buf in = L;
            d.reset();
            run(d, p, L, R);
            const Buf m = driven(in, 2205, drv);
            double worst = 0.0;
            for (size_t i = 0; i < L.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(L[i]) - m[i]));
            CHECK(worst < 2e-6 * amp + 1e-7);
        }
    }
    // On A sin: the fundamental A - A^3, the 3rd harmonic A^3 / 3 (the high-pass takes 0.08% at 500 Hz).
    {
        P p = plain(50.0f);
        p.drive = 1.0f;
        const double a = 0.25, hp = std::abs(highPass(20.0, 500.0));
        Buf L = sine(500.0, 30000, static_cast<float>(a)), R = L, ref = sine(500.0, 30000, 1.0f);
        d.reset();
        run(d, p, L, R);
        const double unit = magnitude(ref, 500.0, 4000);   // the measurement's own scale for a 1.0 sine
        CHECK(near(magnitude(L, 500.0, 4000) / unit, hp * (a - a * a * a), 1e-4));
        CHECK(near(magnitude(L, 1500.0, 4000) / unit, a * a * a / 3.0, 0.01 * a * a * a / 3.0));
    }
}

void limiter() {
    Delay d;
    // A sine at +9.5 dBFS comes back held at 0 dBFS.
    P p = plain(50.0f);
    Buf L = sine(500.0, 30000, 3.0f), R = L;
    run(d, p, L, R);
    CHECK(peak(L, 6000) < 1.005f && peak(L, 6000) > 0.98f);
    // Under 0 dBFS it does nothing (drive 0's linearity above already runs at -1 dBFS).
    d.reset();
    Buf M = sine(500.0, 30000, 0.95f), N = M;
    const Buf in = M;
    run(d, p, M, N);
    CHECK(near(magnitude(M, 500.0, 6000) / magnitude(in, 500.0, 6000), std::abs(highPass(20.0, 500.0)), 1e-4));
}

// The plain delay's level over a window, through a model of the duck's envelope (dsp/delay.h:
// peak follower, 5 ms attack, 250 ms release) and its law 1 / (1 + 16 duck env).
std::vector<double> duckModel(const Buf& in, float duck) {
    const double att = 1.0 - std::exp(-1.0 / (0.005 * kSr)), rel = 1.0 - std::exp(-1.0 / (0.25 * kSr));
    std::vector<double> g(in.size());
    double env = 0.0;
    for (size_t i = 0; i < in.size(); ++i) {
        const double level = std::fabs(in[i]);
        env += (level > env ? att : rel) * (level - env);
        g[i] = 1.0 / (1.0 + 16.0 * duck * env);
    }
    return g;
}

void ducking() {
    // Ducked against unducked: the loop is the same, so their ratio is the wet's gain alone. While
    // the input plays it follows the law; after it stops the repeats come back up.
    const int playing = kSr, n = 3 * kSr;
    const Buf in = concat(sine(1000.0, playing, 0.5f), silence(n - playing));
    for (float duck : {0.5f, 1.0f}) {
        Delay a, b;
        P p = plain(100.0f);
        p.feedback = 0.7f;
        Buf L0 = in, R0 = in, L1 = in, R1 = in;
        run(a, p, L0, R0);
        p.duck = duck;
        run(b, p, L1, R1);
        const std::vector<double> g = duckModel(in, duck);
        for (int w = 0; w < 5; ++w) {   // during the input, after the first echo
            const size_t from = static_cast<size_t>(0.2 * kSr) + static_cast<size_t>(w) * 6615, to = from + 6615;
            double model = 0.0, weight = 0.0;
            for (size_t i = from; i < to; ++i) {
                model += g[i] * g[i] * L0[i] * L0[i];
                weight += L0[i] * L0[i];
            }
            const double got = db(rms(L1, from, to) / rms(L0, from, to)), expect = db(std::sqrt(model / weight));
            CHECK(near(got, expect, 0.3));
            CHECK(got < (duck == 1.0f ? -15.0 : -10.0));
        }
        // 1.25 s after the input stops: within half a dB again.
        const size_t from = static_cast<size_t>(2.25 * kSr), to = from + 4410;
        const double back = db(rms(L1, from, to) / rms(L0, from, to));
        CHECK(back > -0.5 && back <= 0.0);
        CHECK(near(back, db(g[from + 2205]), 0.2));
        CHECK(same(R1, L1));
    }
}

// The echo's delay, sample by sample, from its phase against a 100 Hz input: the correlation with
// sin and cos over one period (441 samples) at points `step` apart from `from` on.
std::vector<double> delays(const Buf& out, double hz, double nominal, size_t from, size_t to, size_t step) {
    const int period = static_cast<int>(std::lround(kSr / hz));
    const double w = 2.0 * kPi * hz / kSr, theta = std::arg(highPass(20.0, hz));
    std::vector<double> d;
    for (size_t m = from; m + static_cast<size_t>(period) <= to; m += step) {
        double i = 0.0, q = 0.0;
        for (int k = 0; k < period; ++k) {
            const double ph = w * static_cast<double>(m + static_cast<size_t>(k));
            i += out[m + static_cast<size_t>(k)] * std::sin(ph);
            q += out[m + static_cast<size_t>(k)] * std::cos(ph);
        }
        // out = g sin(w (n - D) + theta): psi = atan2(q, i) = theta - w D, modulo a period.
        double x = theta - std::atan2(q, i) - w * nominal;
        x = std::remainder(x, 2.0 * kPi);
        d.push_back(nominal + x / w);
    }
    return d;
}

void wow() {
    // 200 ms (20 periods of 100 Hz). Without wow the delay is constant; with wow 1 it swings by the
    // documented +-3 ms wow plus +-0.2 ms flutter (+-141 samples) around it, half that at wow 0.5,
    // and R differs from L (its wow a quarter cycle ahead).
    const int n = 5 * kSr;
    const double nominal = 8820.0;
    double swing[3] = {};
    for (int k = 0; k < 3; ++k) {
        Delay d;
        P p = plain(200.0f);
        p.wow = 0.5f * static_cast<float>(k);
        Buf L = sine(100.0, n, 0.5f), R = L;
        run(d, p, L, R);
        const size_t from = static_cast<size_t>(0.5 * kSr), to = from + 4 * kSr;   // two wow cycles
        const std::vector<double> dl = delays(L, 100.0, nominal, from, to, 64), dr = delays(R, 100.0, nominal, from, to, 64);
        double lo = 1e9, hi = -1e9, mean = 0.0, apart = 0.0;
        for (size_t i = 0; i < dl.size(); ++i) {
            lo = std::min(lo, dl[i]);
            hi = std::max(hi, dl[i]);
            mean += dl[i];
            apart = std::max(apart, std::fabs(dl[i] - dr[i]));
        }
        mean /= static_cast<double>(dl.size());
        swing[k] = hi - lo;
        CHECK(near(mean, nominal, k == 0 ? 0.01 : 10.0));
        if (k == 0) CHECK(swing[0] < 0.01 && apart < 0.01);
        if (k == 2) CHECK(apart > 100.0);   // a quarter cycle: up to sqrt(2) x 132
    }
    const double depth = 0.003 * kSr, flutter = 0.0002 * kSr;
    CHECK(swing[2] > 2.0 * (depth - flutter) && swing[2] < 2.0 * (depth + flutter) + 1.0);
    CHECK(near(swing[1] / swing[2], 0.5, 0.05));
    // Short times keep the depth under a quarter of the time: 10 ms swings at most +-2.5 ms.
    Delay d;
    P p = plain(10.0f);
    p.wow = 1.0f;
    Buf L = sine(100.0, n, 0.5f), R = L;
    run(d, p, L, R);
    CHECK(allFinite(L) && allFinite(R));
}

void holds() {
    // Feedback 1, drive 1, loud noise for 30 s: bounded, and still ringing after the input stops.
    for (float drv : {1.0f, 0.0f}) {
        Delay d;
        P p;   // the defaults: 1/8. at 120 BPM, cuts 100 Hz .. 8 kHz
        p.feedback = 1.0f;
        p.drive = drv;
        p.mix = 1.0f;
        const Buf in = concat(whiteNoise(30 * kSr, 1.0f, 11), silence(3 * kSr));
        Buf L = in, R = concat(whiteNoise(30 * kSr, 1.0f, 12), silence(3 * kSr));
        run(d, p, L, R, tempo(120.0));
        CHECK(allFinite(L) && allFinite(R));
        CHECK(peak(L) < 4.0f && peak(R) < 4.0f);
        CHECK(rms(L, 32 * kSr, 33 * kSr) > 1e-3 && rms(R, 32 * kSr, 33 * kSr) > 1e-3);
        CHECK(d.tailSamples() >= (1 << 30));
    }
    // With the cuts open and under 0 dBFS, feedback 1 holds a burst at its level.
    Delay d;
    P p = plain(250.0f);
    p.feedback = 1.0f;
    const Buf in = concat(whiteNoise(kSr / 5, 0.25f, 13), silence(6 * kSr));
    Buf L = in, R = in;
    run(d, p, L, R);
    const double early = rms(L, kSr, 2 * kSr), late = rms(L, 5 * kSr, 6 * kSr);
    CHECK(near(db(late / early), 0.0, 0.3));
}

void smooth() {
    // Each parameter jumping between its extremes every chunk, on two 200 Hz sines (faded in): no
    // step in the output beyond twice what a sine at the output's peak makes. Left unsmoothed, a
    // jump would step by up to the whole level at once.
    const int n = 1378 * af::kChunk;   // 1 s
    for (int what = 0; what < 8; ++what) {
        Delay d;
        Buf L = sine(200.0, n, 0.5f), R = sine(200.0, n, 0.5f, 1.0);
        for (int i = 0; i < 441; ++i) {
            L[static_cast<size_t>(i)] *= static_cast<float>(i) / 441.0f;
            R[static_cast<size_t>(i)] *= static_cast<float>(i) / 441.0f;
        }
        for (int pos = 0; pos < n; pos += af::kChunk) {
            const bool odd = (pos / af::kChunk) % 2;
            P p = plain(100.0f);
            p.feedback = 0.5f;
            p.mix = 0.5f;
            switch (what) {
                case 0: p.mix = odd ? 1.0f : 0.0f; break;
                case 1: p.feedback = odd ? 0.95f : 0.0f; break;
                case 2: p.lowCutHz = odd ? 2000.0f : 20.0f, p.highCutHz = odd ? 500.0f : 20000.0f; break;
                case 3: p.drive = odd ? 1.0f : 0.0f; break;
                case 4: p.wow = odd ? 1.0f : 0.0f; break;
                case 5: p.duck = odd ? 1.0f : 0.0f; break;
                case 6: p.mode = odd ? Delay::PING_PONG : ((pos / af::kChunk) % 4 ? Delay::MONO : Delay::STEREO); break;
                default:
                    p.mix = odd ? 1.0f : 0.0f;
                    p.feedback = odd ? 0.95f : 0.0f;
                    p.lowCutHz = odd ? 2000.0f : 20.0f;
                    p.highCutHz = odd ? 500.0f : 20000.0f;
                    p.drive = p.wow = p.duck = odd ? 1.0f : 0.0f;
                    p.mode = odd ? Delay::PING_PONG : Delay::MONO;
                    break;
            }
            d.set(p, {});
            d.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], af::kChunk);
        }
        const double natural = std::max(peak(L), peak(R)) * 2.0 * kPi * 200.0 / kSr;
        CHECK(allFinite(L) && allFinite(R));
        CHECK(std::max(maxStep(L), maxStep(R)) < 2.0 * natural);
    }
}

void mixZero() {
    // Mix 0 is the input, bit for bit, whatever the delay does.
    Delay d;
    for (int mode = 0; mode < Delay::kModes; ++mode) {
        P p;
        p.mode = mode;
        p.feedback = 0.9f;
        p.wow = 1.0f;
        p.drive = 1.0f;
        p.duck = 1.0f;
        p.spread = -0.3f;
        p.mix = 0.0f;
        const Buf inL = whiteNoise(30000, 1.0f, 21), inR = whiteNoise(30000, 1.0f, 22);
        Buf L = inL, R = inR;
        d.reset();
        run(d, p, L, R);
        CHECK(same(L, inL) && same(R, inR));
    }
}

void extremes() {
    // Loud noise at random corners of the parameter space and both tempo extremes, half a second
    // each, and the two outermost corners for 4 s.
    Delay d;
    uint32_t s = 777;
    float worst = 0.0f;
    bool finite = true;
    const auto corner = [&](uint32_t bits) {
        P p;
        p.mode = static_cast<int>(bits % 3);
        p.sync = bits & 8;
        p.timeMs = bits & 16 ? 2000.0f : 1.0f;
        p.divBeats = bits & 16 ? 4.0 : 0.0625;
        p.feedback = bits & 32 ? 1.0f : 0.0f;
        p.spread = bits & 64 ? 0.5f : -0.5f;
        p.lowCutHz = bits & 128 ? 2000.0f : 20.0f;
        p.highCutHz = bits & 256 ? 20000.0f : 500.0f;
        p.wow = bits & 512 ? 1.0f : 0.0f;
        p.drive = bits & 1024 ? 1.0f : 0.0f;
        p.duck = bits & 2048 ? 1.0f : 0.0f;
        p.mix = bits & 4096 ? 1.0f : 0.0f;
        p.glide = bits & 16384 ? Delay::FADE : Delay::TAPE;
        return p;
    };
    for (int k = 0; k < 120; ++k) {
        const uint32_t bits = af::xorshift(s);
        d.reset();
        Buf L = whiteNoise(kSr / 2, 1.0f, bits), R = whiteNoise(kSr / 2, 1.0f, bits + 1);
        run(d, corner(bits), L, R, tempo(bits & 8192 ? 300.0 : 30.0));
        finite = finite && allFinite(L) && allFinite(R);
        worst = std::max({worst, peak(L), peak(R)});
    }
    for (uint32_t bits : {0u, 0xffffu, 0xfff2u}) {
        d.reset();
        Buf L = whiteNoise(4 * kSr, 1.0f, 31), R = whiteNoise(4 * kSr, 1.0f, 32);
        run(d, corner(bits), L, R, tempo(bits & 8192 ? 300.0 : 30.0));
        finite = finite && allFinite(L) && allFinite(R);
        worst = std::max({worst, peak(L), peak(R)});
    }
    CHECK(finite);
    CHECK(worst < 4.0f);
    // Out-of-range and non-finite parameters and tempos are clamped.
    const float wild[] = {-1e30f, -5.0f, 1e9f, INFINITY, -INFINITY, std::nanf("")};
    for (float w : wild) {
        P p;
        p.mode = w > 0.0f ? 7 : -3;
        p.glide = w > 0.0f ? 5 : -2;
        p.sync = w > 0.0f;
        p.timeMs = p.feedback = p.spread = p.lowCutHz = p.highCutHz = p.wow = p.drive = p.duck = p.mix = w;
        p.divBeats = w;
        d.reset();
        Buf A = whiteNoise(kSr, 1.0f, 33), B = A;
        run(d, p, A, B, tempo(w));
        CHECK(allFinite(A) && allFinite(B) && peak(A) < 4.0f);
        CHECK(d.tailSamples() > 0);
    }
}

void randomJumps() {
    // Every parameter somewhere new every chunk (out of range too, now and then NaN), the tempo too,
    // under loud noise, for 10 s.
    Delay d;
    uint32_t s = 4242;
    const int n = 10 * kSr;
    Buf L = whiteNoise(n, 1.0f, 41), R = whiteNoise(n, 1.0f, 42);
    for (int pos = 0; pos < n; pos += af::kChunk) {
        P p;
        p.mode = static_cast<int>(af::xorshift(s) % 5) - 1;
        p.sync = af::xorshift(s) & 1;
        p.timeMs = static_cast<float>(std::exp2(af::randBipolar(s) * 6.0 + 6.0));
        p.divBeats = std::exp2(af::randBipolar(s) * 5.0 - 1.0);
        p.feedback = 0.6f + 0.6f * af::randBipolar(s);
        p.spread = 0.7f * af::randBipolar(s);
        p.lowCutHz = static_cast<float>(std::exp2(af::randBipolar(s) * 6.0 + 7.0));
        p.highCutHz = static_cast<float>(std::exp2(af::randBipolar(s) * 3.0 + 12.0));
        p.wow = 0.5f + 0.75f * af::randBipolar(s);
        p.drive = 0.5f + 0.75f * af::randBipolar(s);
        p.duck = 0.5f + 0.75f * af::randBipolar(s);
        p.mix = 0.5f + 0.75f * af::randBipolar(s);
        p.glide = static_cast<int>(af::xorshift(s) % 4) - 1;
        if (af::xorshift(s) % 50 == 0) p.timeMs = std::nanf("");
        if (af::xorshift(s) % 50 == 0) p.feedback = std::nanf("");
        if (af::xorshift(s) % 50 == 0) p.spread = INFINITY;
        d.set(p, tempo(30.0 + 270.0 * (0.5 + 0.5 * af::randBipolar(s))));
        d.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], std::min(af::kChunk, n - pos));
    }
    CHECK(allFinite(L) && allFinite(R));
    CHECK(peak(L) < 4.0f && peak(R) < 4.0f);
}

void nanInput() {
    // NaN and infinities in the input act as silence: the output is exactly what zeros there give
    // (in the dry too), and finite throughout. Absurd finite levels stay finite.
    for (int mode = 0; mode < Delay::kModes; ++mode) {
        Delay a, b;
        P p;
        p.mode = mode;
        p.feedback = 0.8f;
        p.wow = 0.5f;
        p.drive = 0.3f;
        p.duck = 0.5f;
        p.spread = 0.2f;
        p.mix = 0.6f;
        Buf clean = sine(300.0, 30000, 0.5f);
        clean[1000] = clean[2000] = clean[3000] = 0.0f;
        Buf bad = clean;
        bad[1000] = std::nanf("");
        bad[2000] = INFINITY;
        bad[3000] = -INFINITY;
        Buf cl = clean, cr = clean, bl = bad, br = bad;
        br[2000] = std::nanf("");
        cr[2000] = 0.0f;
        run(a, p, cl, cr);
        run(b, p, bl, br);
        CHECK(allFinite(bl) && allFinite(br));
        CHECK(same(cl, bl) && same(cr, br));

        Buf huge = whiteNoise(30000, 1.0f, 51), h2 = huge;
        huge[100] = 1e30f;
        huge[200] = -3e38f;
        h2[200] = 3.4e38f;
        h2[300] = -3.4e38f;
        a.reset();
        p.mix = 0.5f;
        run(a, p, huge, h2);
        CHECK(allFinite(huge) && allFinite(h2));
        CHECK(peak(huge, 400) < 4.0f && peak(h2, 400) < 4.0f);

        // While a Fade runs (from sample 672 to about 2900) too.
        Delay e, f;
        P g = p;
        g.glide = Delay::FADE;
        g.mix = 0.6f;
        P h = g;
        h.feedback = 0.8f;
        h.timeMs = 250.0f;
        Buf cl2 = clean, cr2 = clean, bl2 = bad, br2 = bad;
        br2[2000] = std::nanf("");
        runSplit(e, g, h, cl2, cr2, 640);
        runSplit(f, g, h, bl2, br2, 640);
        CHECK(allFinite(bl2) && allFinite(br2));
        CHECK(same(cl2, bl2) && same(cr2, br2));
    }
}

void blockSizes() {
    // Constant parameters: chunks of 1, 7 and 32 give the same output, bit for bit.
    std::vector<P> sets;
    {
        P p;   // the defaults, with everything on
        p.wow = 0.6f;
        p.drive = 0.4f;
        p.duck = 0.7f;
        p.spread = 0.25f;
        sets.push_back(p);
        p.mode = Delay::PING_PONG;
        p.feedback = 0.9f;
        sets.push_back(p);
        p.mode = Delay::MONO;
        p.sync = false;
        p.timeMs = 3.0f;
        sets.push_back(p);
    }
    for (const P& p : sets) {
        Delay d;
        const Buf inL = whiteNoise(40000, 0.8f, 61), inR = sine(220.0, 40000, 0.7f);
        Buf L32 = inL, R32 = inR;
        run(d, p, L32, R32, tempo(133.0), 32);
        for (int chunk : {1, 7}) {
            d.reset();
            Buf L = inL, R = inR;
            run(d, p, L, R, tempo(133.0), chunk);
            CHECK(same(L, L32) && same(R, R32));
        }
    }
    // A time change (at a sample all three chunkings share): both glides, wow on, Ping-Pong.
    for (int glide : {Delay::TAPE, Delay::FADE}) {
        P p = sets[1], q = sets[1];
        p.glide = q.glide = glide;
        q.divBeats = 1.0;
        q.spread = -0.4f;
        Delay d;
        const Buf inL = whiteNoise(40000, 0.8f, 62), inR = sine(220.0, 40000, 0.7f);
        Buf L32 = inL, R32 = inR;
        runSplit(d, p, q, L32, R32, 22400, tempo(133.0), 32);
        for (int chunk : {1, 7}) {
            d.reset();
            Buf L = inL, R = inR;
            runSplit(d, p, q, L, R, 22400, tempo(133.0), chunk);
            CHECK(same(L, L32) && same(R, R32));
        }
    }
}

void resetClears() {
    // After reset() the delay is as good as new: nothing of what the lines held comes back (they
    // aren't cleared, only hidden), and the next set() jumps.
    Delay used, fresh;
    P p = plain(1500.0f);
    p.feedback = 0.95f;
    p.wow = 1.0f;
    Buf L = whiteNoise(3 * kSr, 1.0f, 71), R = L;
    run(used, p, L, R);
    used.reset();
    P q;
    q.sync = false;
    q.timeMs = 1800.0f;
    q.feedback = 0.5f;
    q.mode = Delay::PING_PONG;
    q.spread = -0.2f;
    q.wow = 0.3f;
    q.mix = 1.0f;
    Buf a = impulseAt(4 * kSr, 100), b = a, c = a, e = a;
    run(used, q, a, b);
    run(fresh, q, c, e);
    CHECK(same(a, c) && same(b, e));
    // Silence after reset: silence out, though the time reaches back over the old noise.
    Buf M = whiteNoise(3 * kSr, 1.0f, 72), N = M;
    run(used, p, M, N);
    used.reset();
    Buf s = silence(3 * kSr), t = s;
    run(used, plain(2000.0f), s, t);
    CHECK(peak(s) < 1e-15f && peak(t) < 1e-15f);

    // The lines full of loud noise, then reset: times jumping (and gliding) between 1 ms and the
    // 8 s maximum, with the wow and spread, sweep the taps across all of it. Still nothing comes
    // out, and with an input the output is a new delay's to the bit.
    const auto sweep = [](Delay& d, Buf& l, Buf& r) {
        const int times[] = {2000, 1, 700, 8000, 5, 1500};
        for (size_t pos = 0, c = 0; pos < l.size(); pos += af::kChunk, ++c) {
            const int ms = times[(c / 40) % 6];
            P p = ms == 8000 ? synced(4.0) : plain(static_cast<float>(ms));
            p.mode = Delay::PING_PONG;
            p.feedback = 0.7f;
            p.spread = 0.5f;
            p.wow = 1.0f;
            d.set(p, tempo(30.0));
            d.process(&l[pos], &r[pos], static_cast<int>(std::min<size_t>(af::kChunk, l.size() - pos)));
        }
    };
    for (bool input : {false, true}) {
        Delay a, b;
        P loud = plain(2000.0f);
        loud.feedback = 0.9f;
        Buf N1 = whiteNoise(9 * kSr, 1.0f, 73), N2 = whiteNoise(9 * kSr, 1.0f, 74);
        run(a, loud, N1, N2);
        a.reset();
        Buf l1 = input ? sine(440.0, 6 * kSr, 0.3f) : silence(6 * kSr), r1 = l1, l2 = l1, r2 = l1;
        sweep(a, l1, r1);
        sweep(b, l2, r2);
        CHECK(same(l1, l2) && same(r1, r2));
        if (!input) CHECK(peak(l1) < 1e-15f && peak(r1) < 1e-15f);
    }
}

void tail() {
    // tailSamples() covers the measured ring of a tone burst (at the cuts' passband) down to
    // -60 dB, without much to spare.
    struct Case {
        int mode;
        float ms, fb, spread, wow;
    };
    const Case cases[] = {{Delay::STEREO, 100.0f, 0.0f, 0.0f, 0.0f},    {Delay::STEREO, 100.0f, 0.3f, 0.0f, 0.0f},
                          {Delay::STEREO, 100.0f, 0.5f, 0.5f, 0.0f},    {Delay::STEREO, 250.0f, 0.9f, 0.0f, 1.0f},
                          {Delay::PING_PONG, 100.0f, 0.7f, 0.5f, 0.0f}, {Delay::MONO, 40.0f, 0.95f, 0.0f, 0.0f}};
    const int len = 882;   // 20 ms of 1 kHz
    Buf burst = sine(1000.0, len, 1.0f);
    for (int i = 0; i < len; ++i) burst[static_cast<size_t>(i)] *= static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / len));
    Delay d;
    for (const Case& c : cases) {
        P p = plain(c.ms);
        p.mode = c.mode;
        p.feedback = c.fb;
        p.spread = c.spread;
        p.wow = c.wow;
        d.reset();
        d.set(p, {});
        const int predicted = d.tailSamples();
        Buf L = concat(burst, silence(predicted + kSr)), R = L;
        run(d, p, L, R);
        size_t last = 0;
        for (size_t i = 0; i < L.size(); ++i)
            if (std::fabs(L[i]) > 1e-3f || std::fabs(R[i]) > 1e-3f) last = i;
        const double period = c.ms * 0.001 * kSr * (1.0 + c.spread) + c.wow * 141.0;
        CHECK(static_cast<double>(last) <= predicted);
        CHECK(predicted <= last + 4.0 * period + 0.06 * kSr);
    }
    P p = plain(100.0f);
    p.feedback = 1.0f;
    d.set(p, {});
    CHECK(d.tailSamples() == (1 << 30));
}


// --- Diffuse (AmbientForce's) ------------------------------------------------------------------

// FNV-1a over the bits of L, then R.
uint64_t hashOf(const Buf& L, const Buf& R) {
    uint64_t h = 1469598103934665603ull;
    for (const Buf* b : {&L, &R})
        for (float v : *b) {
            uint32_t bits;
            std::memcpy(&bits, &v, sizeof bits);
            for (int k = 0; k < 4; ++k) h = (h ^ ((bits >> (8 * k)) & 0xffu)) * 1099511628211ull;
        }
    return h;
}

// Both sides the same, value by value (a zero may carry either sign).
bool sameValues(const Buf& a, const Buf& b, size_t from = 0) {
    if (a.size() != b.size()) return false;
    for (size_t i = from; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

// Ten seconds of noise (a half-second gap at 4 s) through the Delay at Diffuse 0, everything else
// in use and moving: the tempo at 3 s (Tape glides), free times with Fade from 5 s (a fade at 7 s),
// the mix at 6 s, feedback 1 from 8 s. `ran`: whether the diffusers ran.
uint64_t diffuseZeroHash(int mode, bool& ran) {
    Delay d;
    const int n = 10 * kSr;
    Buf L = whiteNoise(n, 0.7f, 101 + static_cast<uint32_t>(mode)), R = whiteNoise(n, 0.7f, 201 + static_cast<uint32_t>(mode));
    std::fill(L.begin() + 4 * kSr, L.begin() + 9 * kSr / 2, 0.0f);
    std::fill(R.begin() + 4 * kSr, R.begin() + 9 * kSr / 2, 0.0f);
    for (size_t pos = 0; pos < L.size(); pos += af::kChunk) {
        const double sec = static_cast<double>(pos) / kSr;
        P p;
        p.mode = mode;
        p.feedback = sec < 8.0 ? 0.85f : 1.0f;
        p.spread = 0.2f;
        p.lowCutHz = 150.0f;
        p.highCutHz = 6000.0f;
        p.wow = 0.6f;
        p.drive = 0.3f;
        p.duck = 0.5f;
        p.mix = sec < 6.0 ? 1.0f : 0.6f;
        if (sec >= 5.0) {
            p.sync = false;
            p.glide = Delay::FADE;
            p.timeMs = sec < 7.0 ? 333.0f : 512.0f;
        }
        p.diffuse = 0.0f;
        af::Transport t;
        t.bpm = sec < 3.0 ? 120.0 : 97.0;
        d.set(p, t);
        d.process(&L[pos], &R[pos], static_cast<int>(std::min<size_t>(af::kChunk, L.size() - pos)));
        ran = ran || d.diffusing();
    }
    return hashOf(L, R);
}

// Diffuse 0 is EffectForce's Delay, bit for bit, at every mode: the fingerprints of EffectForce's
// own dsp/delay.cpp (7cf6e95) over the same ten seconds, taken once with a scratch harness that
// builds it from EffectForce's repo with this suite's flags (x86 at make test's, -O1 with ASan;
// -O3 gives the same. The device's at make test-arm's: NEON fuses multiply-adds, so its bits
// differ). Not against profile-guided objects (make test-arm-pgo): the profile moves the fusing
// with every change of the code, so no fingerprint stays. In every build: the diffusers never ran.
void diffuseZero() {
    bool ran = false;
    uint64_t got[Delay::kModes];
    for (int mode = 0; mode < Delay::kModes; ++mode) got[mode] = diffuseZeroHash(mode, ran);
    CHECK(!ran);
#if AF_PGO_OBJECTS
    std::printf("  (Diffuse 0's fingerprints are checked in the plain builds, not against profile-guided objects)\n");
    return;
#endif
#if AF_NEON
    const uint64_t effectForce[Delay::kModes] = {0x9f6001c5eddf2732ull, 0x18746121166c7accull, 0xb9c8ab6fb7ffabd6ull};
#else
    const uint64_t effectForce[Delay::kModes] = {0xf6a4afbb49a74bb3ull, 0x33435f15e9b9625full, 0x446a83e633a1e9daull};
#endif
    for (int mode = 0; mode < Delay::kModes; ++mode) CHECK(got[mode] == effectForce[mode]);
}

// The Delay as Echo plays it (initEcho()'s cuts), free at `ms`, without the wow and the duck so an
// impulse's repeats can be measured.
P echoLike(float ms, float fb, float diffuse) {
    P p = af::initEcho().delay;
    p.sync = false;
    p.timeMs = ms;
    p.feedback = fb;
    p.wow = 0.0f;
    p.duck = 0.0f;
    p.diffuse = diffuse;
    p.mix = 1.0f;
    return p;
}

// Diffuse smears: an impulse at feedback 0.7 and 300 ms. The third repeat's window runs from its
// own time to the fourth's: the allpasses are causal, so nothing of a repeat comes before its time.
// Diffused, a repeat trails on past the next one's time (each pass smears it over 60 ms and more),
// so the window loses the third's late tail and takes the second's: over the spacing of the repeats
// the two nearly balance. At Diffuse 1 the third repeat is spread over at least 4 times as many
// samples above -40 dB of its peak as at Diffuse 0, with its energy in the window within 1.5 dB.
void diffuseSmears() {
    const int t = 13230;   // 300 ms
    const size_t from = 3 * static_cast<size_t>(t), to = 4 * static_cast<size_t>(t);
    int wide[2] = {};
    double energy[2] = {};
    for (int k = 0; k < 2; ++k) {
        Delay d;
        Buf L = impulseAt(5 * t, 0), R = L;
        run(d, echoLike(300.0f, 0.7f, static_cast<float>(k)), L, R);
        CHECK(d.diffusing() == (k == 1));
        const float top = std::max(peak(L, from, to), peak(R, from, to));
        for (size_t i = from; i < to; ++i) {
            wide[k] += (std::fabs(L[i]) > 0.01f * top) + (std::fabs(R[i]) > 0.01f * top);
            energy[k] += static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i];
        }
    }
    const double change = 10.0 * std::log10(energy[1] / energy[0]);
    std::printf("  diffuse: the third repeat above -40 dB of its peak over %d samples at Diffuse 1, %d at 0; "
                "its energy %+.2f dB\n", wide[1] / 2, wide[0] / 2, change);
    CHECK(wide[1] >= 4 * wide[0]);
    CHECK(near(change, 0.0, 1.5));
}

// What a pass of the blend loses (dsp/delay.h): white noise through one pass (feedback 0, the cuts
// open), its level against Diffuse 0's. A signal and its allpassed self, (1 - d) + d H, keep
// (1 - d)^2 + d^2 + 2 d (1 - d) h0 of its power on average, h0 = 0.65^4 the allpasses' first
// sample: -1.84 dB at 0.3, -2.30 dB at 0.5, nothing at 1.
void diffuseLoss() {
    const Buf nl = whiteNoise(4 * kSr, 0.5f, 501), nr = whiteNoise(4 * kSr, 0.5f, 502);
    double level[4] = {};
    const float amounts[4] = {0.0f, 0.3f, 0.5f, 1.0f};
    for (int k = 0; k < 4; ++k) {
        Delay d;
        P p = plain(100.0f);
        p.diffuse = amounts[k];
        Buf L = nl, R = nr;
        run(d, p, L, R);
        level[k] = rms(L, kSr) + rms(R, kSr);
    }
    const double expect[4] = {0.0, -1.84, -2.30, 0.0};
    std::printf("  diffuse: a pass loses at 0.3, 0.5, 1:");
    for (int k = 1; k < 4; ++k) {
        const double got = db(level[k] / level[0]);
        std::printf(" %.2f dB", got);
        CHECK(near(got, expect[k], 0.15));
    }
    std::printf("\n");
}

// The comb (dsp/delay.h): a pass of the blend loses noise where the clear and the diffused repeat
// meet out of phase, but nothing where they meet in phase, so with a long feedback the tail narrows
// pass by pass to a comb of those frequencies and holds there. Feedback 1, 200 ms, the cuts open, a
// 0.25 s burst: at Diffuse 0.3, from second 10 to second 20 (50 passes) it loses under 0.1 dB a
// pass, not diffuseLoss()'s 1.84 of a first one, and second 20's spectrum has under a quarter as
// many bins within 20 dB of its strongest as Diffuse 0's.
void diffuseComb() {
    int within[2] = {};
    double loss[2] = {};
    for (int k = 0; k < 2; ++k) {
        Delay d;
        P p = plain(200.0f);
        p.feedback = 1.0f;
        p.diffuse = k ? 0.3f : 0.0f;
        Buf L = whiteNoise(21 * kSr, 0.5f, 7), R = whiteNoise(21 * kSr, 0.5f, 8);
        std::fill(L.begin() + kSr / 4, L.end(), 0.0f);
        std::fill(R.begin() + kSr / 4, R.end(), 0.0f);
        run(d, p, L, R);
        const size_t s10 = 10 * static_cast<size_t>(kSr), s20 = 20 * static_cast<size_t>(kSr), len = 32768;
        loss[k] = db(rms(L, s20, s20 + len) / rms(L, s10, s10 + len)) / 50.0;
        const Spectrum s(L, s20, len);
        const double top = *std::max_element(s.amp.begin(), s.amp.end());
        within[k] = static_cast<int>(std::count_if(s.amp.begin(), s.amp.end(), [top](double a) { return a > 0.1 * top; }));
    }
    std::printf("  diffuse: feedback 1, the cuts open, seconds 10 to 20: %.3f dB a pass at Diffuse 0.3 (%.3f at 0); "
                "%d bins within 20 dB of the top (%d at 0)\n", loss[1], loss[0], within[1], within[0]);
    CHECK(loss[1] > -0.1);
    CHECK(4 * within[1] < within[0]);
}

// No growth: Echo at feedback 1, Diffuse 1 and wow 1, every mode, a 1 s burst and then 120 s: the
// last second's peak no higher than the peak of the second after the burst. With the cuts open:
// the 20 Hz high-pass and the reads' interpolation are all that lose anything then, so whatever
// the allpasses did would show (Echo's own cuts only take more: 150 Hz..4.5 kHz leave nothing of
// the burst after 120 s).
void diffuseNoGrowth() {
    const int blocks = (121 * kSr + 127) / 128;
    std::printf("  diffuse: feedback 1, Diffuse 1, wow 1, the cuts open, peak in second 2 and second 121:");
    for (int mode = 0; mode < Delay::kModes; ++mode) {
        af::Echo e;
        af::Echo::Params p = af::initEcho();
        p.delay.mode = mode;
        p.delay.feedback = 1.0f;
        p.delay.diffuse = 1.0f;
        p.delay.wow = 1.0f;
        p.delay.lowCutHz = 20.0f;
        p.delay.highCutHz = 20000.0f;
        const Buf nl = whiteNoise(kSr, 0.25f, 301 + static_cast<uint32_t>(mode)), nr = whiteNoise(kSr, 0.25f, 311);
        float l[128], r[128], second2 = 0.0f, last = 0.0f;
        bool finite = true;
        for (int b = 0; b < blocks; ++b) {
            const size_t at0 = static_cast<size_t>(b) * 128;
            for (size_t k = 0; k < 128; ++k) {
                l[k] = at0 + k < nl.size() ? nl[at0 + k] : 0.0f;
                r[k] = at0 + k < nr.size() ? nr[at0 + k] : 0.0f;
            }
            e.set(p, tempo(120.0));
            e.process(l, r, l, r, 128);
            for (size_t k = 0; k < 128; ++k) {
                const size_t i = at0 + k;
                const float v = std::max(std::fabs(l[k]), std::fabs(r[k]));
                finite = finite && std::isfinite(l[k]) && std::isfinite(r[k]);
                if (i >= static_cast<size_t>(kSr) && i < 2 * static_cast<size_t>(kSr)) second2 = std::max(second2, v);
                if (i >= 120 * static_cast<size_t>(kSr) && i < 121 * static_cast<size_t>(kSr)) last = std::max(last, v);
            }
        }
        std::printf(" %s %.3f, %.3f;", af::kEchoModeNames[mode], second2, last);
        CHECK(finite);
        CHECK(second2 > 0.01f && last <= second2);
    }
    std::printf("\n");
}

// Mono: both sides the same with the diffusers on too (R takes L's diffusion, whatever its own),
// from the start, and from the chunk after a switch from Stereo or Ping-Pong on.
void diffuseMono() {
    P p = plain(120.0f);
    p.mode = Delay::MONO;
    p.feedback = 0.6f;
    p.spread = 0.4f;
    p.wow = 0.7f;
    p.drive = 0.5f;
    p.lowCutHz = 150.0f;
    p.highCutHz = 6000.0f;
    p.diffuse = 0.7f;
    const Buf inL = whiteNoise(30000, 0.5f, 5), inR = sine(330.0, 30000, 0.5f);
    {
        Delay d;
        Buf L = inL, R = inR;
        run(d, p, L, R);
        CHECK(d.diffusing());
        CHECK(sameValues(L, R) && rms(L, 10000) > 0.01);
    }
    for (int from : {Delay::STEREO, Delay::PING_PONG}) {
        P q = p;
        q.mode = from;
        Delay d;
        Buf L = inL, R = inR;
        runSplit(d, q, p, L, R, 16000);
        CHECK(sameValues(L, R, 16000 + af::kChunk) && !sameValues(L, R, 8000));
    }
}

// A Diffuse change glides across the chunk: Diffuse jumping between its ends, or between 0.3 and
// 0.8, every chunk, and the mode jumping with Diffuse at 1, on two 200 Hz sines through feedback
// 0.5: no step beyond twice what a sine at the output's peak makes (smooth()'s measure).
void diffuseSmooth() {
    const int n = 1378 * af::kChunk;   // 1 s
    for (int what = 0; what < 3; ++what) {
        Delay d;
        Buf L = fadedSine(200.0, n, 0.5f), R = fadedSine(200.0, n, 0.5f, 1.0);
        for (int pos = 0; pos < n; pos += af::kChunk) {
            const int c = pos / af::kChunk;
            const bool odd = c % 2;
            P p = plain(100.0f);
            p.feedback = 0.5f;
            p.mix = 0.5f;
            switch (what) {
                case 0: p.diffuse = odd ? 1.0f : 0.0f; break;
                case 1: p.diffuse = odd ? 0.8f : 0.3f; break;
                default:
                    p.diffuse = 1.0f;
                    p.mode = odd ? Delay::PING_PONG : (c % 4 ? Delay::MONO : Delay::STEREO);
                    break;
            }
            d.set(p, {});
            d.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], af::kChunk);
        }
        const double natural = std::max(peak(L), peak(R)) * 2.0 * kPi * 200.0 / kSr;
        CHECK(allFinite(L) && allFinite(R));
        CHECK(std::max(maxStep(L), maxStep(R)) < 2.0 * natural);
    }
}

// Constant parameters with the diffusers on: chunks of 1, 7 and 32 give the same output, bit for
// bit, in every mode, and at times under a segment (where the diffusers' sub-runs get shorter);
// and through a time change (Tape and Fade).
void diffuseBlockSizes() {
    for (int mode = 0; mode <= Delay::kModes; ++mode) {
        P p;
        p.mode = mode % Delay::kModes;
        p.feedback = 0.9f;
        p.wow = 0.6f;
        p.duck = 0.7f;
        p.spread = 0.25f;
        p.diffuse = 0.6f;
        P q = p;
        q.divBeats = 1.0;
        q.glide = mode == Delay::MONO ? Delay::FADE : Delay::TAPE;
        if (mode == Delay::kModes) {   // R reads 0.5 to 0.75 ms back: shorter than a segment
            p.mode = q.mode = Delay::PING_PONG;
            p.sync = q.sync = false;
            p.timeMs = 1.0f;
            q.timeMs = 1.5f;
            p.spread = q.spread = -0.5f;
            p.wow = q.wow = 1.0f;
        }
        const Buf inL = whiteNoise(40000, 0.8f, 63), inR = sine(220.0, 40000, 0.7f);
        Delay d;
        Buf L32 = inL, R32 = inR;
        runSplit(d, p, q, L32, R32, 22400, tempo(133.0), 32);
        for (int chunk : {1, 7}) {
            d.reset();
            Buf L = inL, R = inR;
            runSplit(d, p, q, L, R, 22400, tempo(133.0), chunk);
            CHECK(same(L, L32) && same(R, R32));
        }
    }
}

// Odd values: Diffuse out of range or not finite is clamped (NaN: 0), under loud noise at feedback
// 0.9; and 10 s of every parameter jumping every chunk, Diffuse too (now and then NaN). Finite and
// bounded throughout.
void diffuseOdd() {
    const float wild[] = {-1e30f, -5.0f, 1e9f, INFINITY, -INFINITY, std::nanf("")};
    for (float w : wild) {
        Delay d;
        P p;
        p.feedback = 0.9f;
        p.diffuse = w;
        Buf A = whiteNoise(kSr, 1.0f, 33), B = whiteNoise(kSr, 1.0f, 34);
        run(d, p, A, B, tempo(120.0));
        CHECK(allFinite(A) && allFinite(B) && peak(A) < 4.0f && peak(B) < 4.0f);
        CHECK(d.diffusing() == (w > 0.0f));
    }
    Delay d;
    uint32_t s = 999;
    const int n = 10 * kSr;
    Buf L = whiteNoise(n, 1.0f, 43), R = whiteNoise(n, 1.0f, 44);
    for (int pos = 0; pos < n; pos += af::kChunk) {
        P p;
        p.mode = static_cast<int>(af::xorshift(s) % 3);
        p.sync = af::xorshift(s) & 1;
        p.timeMs = static_cast<float>(std::exp2(af::randBipolar(s) * 6.0 + 6.0));
        p.feedback = 0.6f + 0.6f * af::randBipolar(s);
        p.spread = 0.7f * af::randBipolar(s);
        p.wow = 0.5f + 0.75f * af::randBipolar(s);
        p.drive = 0.5f + 0.75f * af::randBipolar(s);
        p.duck = 0.5f + 0.75f * af::randBipolar(s);
        p.mix = 0.5f + 0.75f * af::randBipolar(s);
        p.diffuse = af::xorshift(s) % 4 == 0 ? 0.0f : 0.5f + 0.75f * af::randBipolar(s);
        if (af::xorshift(s) % 50 == 0) p.diffuse = std::nanf("");
        d.set(p, tempo(30.0 + 270.0 * (0.5 + 0.5 * af::randBipolar(s))));
        d.process(&L[static_cast<size_t>(pos)], &R[static_cast<size_t>(pos)], std::min(af::kChunk, n - pos));
    }
    CHECK(allFinite(L) && allFinite(R));
    CHECK(peak(L) < 4.0f && peak(R) < 4.0f);
}

// Diffusers that start again start empty. After reset(): an impulse comes out as a new Delay's to
// the bit. After Diffuse sat at 0 while the repeats died (feedback 0): nothing comes back when it
// returns to 1, though the diffusers were full of loud noise when it left.
void diffuseRestart() {
    P loud = plain(400.0f);
    loud.feedback = 0.9f;
    loud.diffuse = 1.0f;
    loud.mode = Delay::PING_PONG;
    {
        Delay used, fresh;
        Buf N1 = whiteNoise(3 * kSr, 1.0f, 75), N2 = whiteNoise(3 * kSr, 1.0f, 76);
        run(used, loud, N1, N2);
        used.reset();
        P q = plain(250.0f);
        q.feedback = 0.6f;
        q.diffuse = 0.5f;
        q.wow = 0.4f;
        Buf a = impulseAt(2 * kSr, 100), b = a, c = a, e = a;
        run(used, q, a, b);
        run(fresh, q, c, e);
        CHECK(same(a, c) && same(b, e));
    }
    {
        Delay d;
        Buf N1 = whiteNoise(2 * kSr, 1.0f, 77), N2 = whiteNoise(2 * kSr, 1.0f, 78);
        run(d, loud, N1, N2);
        P off = loud;
        off.diffuse = 0.0f;
        off.feedback = 0.0f;
        Buf s = silence(3 * kSr), t = s;
        run(d, off, s, t);
        CHECK(!d.diffusing() && peak(s, 2 * kSr) < 1e-15f && peak(t, 2 * kSr) < 1e-15f);
        P on = off;
        on.diffuse = 1.0f;
        Buf u = silence(kSr), v = u;
        run(d, on, u, v);
        CHECK(d.diffusing() && peak(u) < 1e-15f && peak(v) < 1e-15f);
    }
}

// tailSamples() with the diffusers: it covers the measured ring of a noise burst (every frequency,
// so the slowest the allpasses hold too) down to -60 dB, where the allpasses are most of the loop
// too (10 ms at feedback 0.98); at Diffuse 1 with no more than half as much again and a second to
// spare. (Between 0 and 1 the blend loses a little a pass, which the tail doesn't count: there it
// only covers.)
void diffuseTail() {
    struct Case {
        int mode;
        float ms, fb, diffuse;
    };
    const Case cases[] = {{Delay::STEREO, 100.0f, 0.0f, 1.0f},  {Delay::STEREO, 100.0f, 0.5f, 1.0f},
                          {Delay::STEREO, 250.0f, 0.9f, 0.3f},  {Delay::PING_PONG, 100.0f, 0.7f, 1.0f},
                          {Delay::MONO, 40.0f, 0.95f, 1.0f},    {Delay::STEREO, 2000.0f, 0.5f, 1.0f},
                          {Delay::STEREO, 10.0f, 0.98f, 1.0f}};
    const int len = 882;
    Buf burst = whiteNoise(len, 1.0f, 17);
    for (int i = 0; i < len; ++i) burst[static_cast<size_t>(i)] *= static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / len));
    std::printf("  diffuse: the ring to -60 dB against tailSamples():");
    for (const Case& c : cases) {
        P p = plain(c.ms);
        p.mode = c.mode;
        p.feedback = c.fb;
        p.diffuse = c.diffuse;
        Delay d;
        d.set(p, {});
        const int predicted = d.tailSamples();
        Buf L = concat(burst, silence(predicted + kSr)), R = L;
        run(d, p, L, R);
        size_t last = 0;
        for (size_t i = 0; i < L.size(); ++i)
            if (std::fabs(L[i]) > 1e-3f || std::fabs(R[i]) > 1e-3f) last = i;
        std::printf(" %.2f s / %.2f s;", static_cast<double>(last) / kSr, static_cast<double>(predicted) / kSr);
        CHECK(static_cast<double>(last) <= predicted);
        if (c.diffuse == 1.0f) CHECK(predicted <= 1.5 * static_cast<double>(last) + kSr);
    }
    std::printf("\n");
}

// --- Echo: the send / return -------------------------------------------------------------------

// What Echo returned for the sends, and silent() after each block.
struct Return {
    Buf L, R;
    std::vector<bool> silent;
};
// Echo as the engine runs it: 128-frame blocks, set() before each, in place (the return over the
// send).
Return runEcho(af::Echo& e, const af::Echo::Params& p, const Buf& sendL, const Buf& sendR, af::Transport t = {}) {
    Return o{sendL, sendR, {}};
    for (size_t pos = 0; pos < o.L.size(); pos += 128) {
        const int n = static_cast<int>(std::min<size_t>(128, o.L.size() - pos));
        e.set(p, t);
        e.process(&o.L[pos], &o.R[pos], &o.L[pos], &o.R[pos], n);
        o.silent.push_back(e.silent());
    }
    return o;
}

// Echo is the Delay at mix 1, bit for bit, whatever the block (1, 7, 128, 300 samples; separate
// output buffers or the send's own), and whatever mix it is asked for.
void echoIsDelay() {
    af::Echo::Params p = af::initEcho();
    p.delay.feedback = 0.7f;
    p.delay.wow = 0.6f;
    p.delay.spread = 0.2f;
    p.delay.mode = Delay::PING_PONG;
    const Buf inL = whiteNoise(3 * kSr, 0.5f, 401), inR = sine(330.0, 3 * kSr, 0.5f);
    Delay d;
    P dp = p.delay;
    dp.mix = 1.0f;
    Buf L = inL, R = inR;
    run(d, dp, L, R, tempo(120.0));
    p.delay.mix = 0.2f;   // ignored: a return is wet only
    for (int block : {1, 7, 128, 300}) {
        af::Echo e;
        Buf oL(inL.size()), oR(inR.size());
        for (size_t pos = 0; pos < inL.size(); pos += static_cast<size_t>(block)) {
            const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), inL.size() - pos));
            e.set(p, tempo(120.0));
            e.process(&inL[pos], &inR[pos], &oL[pos], &oR[pos], n);
        }
        CHECK(same(oL, L) && same(oR, R));
    }
    af::Echo e;
    const Return o = runEcho(e, p, inL, inR, tempo(120.0));
    CHECK(same(o.L, L) && same(o.R, R));
}

// Echo is wet only: an impulse gives nothing at the output before the delay time (initEcho(): a
// dotted quarter at 120 BPM, 33075 samples, less the wow's depth at 0.3 and the Hermite read's
// sample ahead), not a bit, then its repeat, in every mode. An Echo never set() is wet only too
// (initEcho(), mix 1). The impulse is the first sample: the loop's -360 dB offset against
// denormals goes in from there on, and comes round with it.
void wetOnly() {
    const size_t time = 33075, depth = 43;   // 0.3 x 3.2 ms
    for (int mode = -1; mode < Delay::kModes; ++mode) {
        af::Echo e;
        Buf L = impulseAt(2 * static_cast<int>(time), 0), R = L;
        if (mode >= 0) {
            af::Echo::Params p = af::initEcho();
            p.delay.mode = mode;
            const Return o = runEcho(e, p, L, R, tempo(120.0));
            L = o.L;
            R = o.R;
        } else {
            for (size_t pos = 0; pos < L.size(); pos += 128)
                e.process(&L[pos], &R[pos], &L[pos], &R[pos], static_cast<int>(std::min<size_t>(128, L.size() - pos)));
        }
        const size_t before = time - depth - 2;
        CHECK(peak(L, 0, before) == 0.0f && peak(R, 0, before) == 0.0f);
        CHECK(std::max(peak(L, before, before + 2 * depth + 400), peak(R, before, before + 2 * depth + 400)) > 0.05f);
    }
}

// How far the Delay inside Echo reaches with p (Delay::reachSamples()): a Delay set the same way.
size_t reachOf(const af::Echo::Params& p) {
    Delay d;
    P dp = p.delay;
    dp.mix = 1.0f;
    d.set(dp, tempo(120.0));
    return static_cast<size_t>(d.reachSamples());
}

// A burst of `burstS` seconds of noise into Echo, then nothing for `totalS` in all: where the
// return falls under -120 dBFS for good, where silent() starts saying so for good, and whether it
// said so before the return had been quiet for the Delay's reach (early), or still didn't once it
// had (late). Echo counts in 32-sample chunks and the test asks after each 128-sample block, so
// "once it had" allows a chunk and a block more.
struct Quieting {
    size_t end = 0, from = 0, reach = 0;
    bool early = false, late = false;
};
Quieting quieting(af::Echo& e, const af::Echo::Params& p, double burstS, double totalS) {
    const int n = static_cast<int>(totalS * kSr), burst = static_cast<int>(burstS * kSr);
    Buf x = whiteNoise(n, 0.5f, 91), y = whiteNoise(n, 0.5f, 92);
    std::fill(x.begin() + burst, x.end(), 0.0f);
    std::fill(y.begin() + burst, y.end(), 0.0f);
    e.reset();
    const Return o = runEcho(e, p, x, y, tempo(120.0));
    Quieting q;
    q.reach = reachOf(p);
    for (size_t i = 0; i < o.L.size(); ++i)
        if (std::max(std::fabs(o.L[i]), std::fabs(o.R[i])) > 1e-6f) q.end = i + 1;
    q.from = o.L.size();
    for (size_t b = 0; b < o.silent.size(); ++b) {
        const size_t blockEnd = std::min((b + 1) * 128, o.L.size());
        q.early = q.early || (o.silent[b] && blockEnd < q.end + q.reach);
        q.late = q.late || (!o.silent[b] && blockEnd >= q.end + q.reach + af::kChunk + 128);
        if (!o.silent[b]) q.from = o.L.size();
        else if (q.from == o.L.size()) q.from = blockEnd;
    }
    return q;
}

// silent(): the repeats of a 0.3 s burst at feedback 0.3 (Echo's defaults, free at 200 ms), of the
// same through Ping-Pong at 400 ms with Diffuse 1 and duck 1, and at 1 s with feedback 0.2 (a reach
// over a second): silent once the return has been under -120 dBFS for the Delay's reach, give or
// take a chunk and a block (quieting()), and not before. Never while feedback 1 holds the repeats,
// nor while a send comes in that duck 1 hides the repeats under. A new or reset Echo is silent.
void silence() {
    af::Echo e;
    CHECK(e.silent());
    af::Echo::Params p = af::initEcho();
    p.delay.sync = false;
    p.delay.timeMs = 200.0f;
    p.delay.feedback = 0.3f;
    af::Echo::Params q = p;
    q.delay.mode = Delay::PING_PONG;
    q.delay.timeMs = 400.0f;
    q.delay.diffuse = 1.0f;
    q.delay.duck = 1.0f;
    af::Echo::Params s = p;
    s.delay.timeMs = 1000.0f;
    s.delay.feedback = 0.2f;
    const struct {
        const af::Echo::Params* p;
        double seconds;
    } cases[] = {{&p, 8.0}, {&q, 8.0}, {&s, 12.0}};
    std::printf("  echo: the return under -120 dBFS, the reach, then silent():");
    for (const auto& c : cases) {
        const Quieting r = quieting(e, *c.p, 0.3, c.seconds);
        std::printf(" %.2f s + %.2f s, %.2f s;", r.end / af::kRate, r.reach / af::kRate, r.from / af::kRate);
        CHECK(r.end > static_cast<size_t>(0.3 * kSr) && r.end + r.reach + static_cast<size_t>(kSr) < static_cast<size_t>(c.seconds * kSr));
        CHECK(!r.early && !r.late);
    }
    std::printf("\n");

    // Feedback 1: the repeats hold, never silent.
    af::Echo::Params f = p;
    f.delay.feedback = 1.0f;
    Buf x = whiteNoise(5 * kSr, 0.5f, 93), y = whiteNoise(5 * kSr, 0.5f, 94);
    std::fill(x.begin() + kSr / 4, x.end(), 0.0f);
    std::fill(y.begin() + kSr / 4, y.end(), 0.0f);
    e.reset();
    const Return held = runEcho(e, f, x, y, tempo(120.0));
    CHECK(rms(held.L, 4 * kSr) > 1e-3);
    CHECK(std::none_of(held.silent.begin(), held.silent.end(), [](bool v) { return v; }));

    // A steady send at duck 1: the repeats ducked far down, the send still coming in.
    af::Echo::Params d = p;
    d.delay.duck = 1.0f;
    e.reset();
    const Return ducked = runEcho(e, d, whiteNoise(3 * kSr, 0.5f, 95), whiteNoise(3 * kSr, 0.5f, 96), tempo(120.0));
    CHECK(std::none_of(ducked.silent.begin(), ducked.silent.end(), [](bool v) { return v; }));

    e.reset();
    CHECK(e.silent());
}

// Back after silent(): the engine stops running Echo once silent() says so, and runs it again when
// a send brings something or silent() turns false. A longer time turns it false (the reach grows
// past the quiet so far) while the lines still hold the burst of two seconds before, at ages the
// longer time reads: without a fresh start it came back at about -50 dBFS. Here: a 0.3 s burst at
// 200 ms, run until silent(), then skipped while the time is set to 2 s, then run again with an
// impulse 3 s on, or in the very first sample (the block that starts afresh takes it in): nothing
// comes out before the impulse's repeat (not 1e-15), which comes at the new time. Woken by the time
// alone, Echo is silent again after its first block. Every mode, with Diffuse 0.3 and without.
void resume() {
    std::printf("  echo: back after silent(), a 2 s time, the loudest before the new repeat:");
    for (float diffuse : {0.3f, 0.0f}) {
        for (int mode = 0; mode < Delay::kModes; ++mode) {
            for (size_t later : {3 * static_cast<size_t>(kSr), size_t{0}}) {
                af::Echo e;
                af::Echo::Params p = af::initEcho();
                p.delay.sync = false;
                p.delay.timeMs = 200.0f;
                p.delay.feedback = 0.3f;
                p.delay.mode = mode;
                p.delay.diffuse = diffuse;
                const size_t burst = static_cast<size_t>(0.3 * kSr);
                Buf x = whiteNoise(10 * kSr, 0.5f, 97), y = whiteNoise(10 * kSr, 0.5f, 98);
                std::fill(x.begin() + static_cast<std::ptrdiff_t>(burst), x.end(), 0.0f);
                std::fill(y.begin() + static_cast<std::ptrdiff_t>(burst), y.end(), 0.0f);
                for (size_t pos = 0; pos < x.size() && !(pos > burst && e.silent()); pos += 128) {
                    e.set(p, tempo(120.0));
                    e.process(&x[pos], &y[pos], &x[pos], &y[pos], 128);
                }
                CHECK(e.silent());

                p.delay.timeMs = 2000.0f;   // skipped: set() goes on, process() doesn't
                for (int b = 0; b < 100; ++b) e.set(p, tempo(120.0));
                CHECK(!e.silent());

                const size_t time = 88200, depth = 43;   // wow 0.3: 42.3 samples
                Buf L(later + time + 2000, 0.0f);
                L[later] = 1.0f;
                const Return o = runEcho(e, p, L, L, tempo(120.0));
                const size_t before = later + time - depth - 2;
                const float old = std::max(peak(o.L, 0, before), peak(o.R, 0, before));
                std::printf(" %.3g", static_cast<double>(old));
                CHECK(old < 1e-15f);
                CHECK(std::max(peak(o.L, before, o.L.size()), peak(o.R, before, o.R.size())) > 0.05f);
                if (later > 0) CHECK(o.silent[0]);
            }
        }
    }
    std::printf("\n");
}

// The same when the skipping starts at a set(), Echo run the engine's way: set() every block, then
// process() only while the send is up or silent() is false. 1 s, feedback 0, Diffuse 0, a 0.3 s
// burst; just after its repeat the time goes to 20 ms. The heads glide down, and each set() works
// the reach out again from where the last process() left them, so here a set() makes silent()
// true and the engine stops, with no process() having ended silent. At 4 s the time goes to 2 s:
// Echo runs one block, nothing old in it (without a fresh start the burst came back at -3 dBFS),
// and is silent again.
void resumeAfterSet() {
    af::Echo e;
    af::Echo::Params p = af::initEcho();
    p.delay.sync = false;
    p.delay.timeMs = 1000.0f;
    p.delay.feedback = 0.0f;
    p.delay.diffuse = 0.0f;
    const size_t burst = static_cast<size_t>(0.3 * kSr);
    const Buf x = whiteNoise(static_cast<int>(burst), 0.5f, 99), y = whiteNoise(static_cast<int>(burst), 0.5f, 100);
    const int down = 690, up = 4 * kSr / 128, end = up + 3 * kSr / 128;   // 2.003 s, 4 s, 7 s
    bool skipped = false, atSet = false;
    int ran = 0;
    float old = 0.0f, l[128], r[128];
    for (int b = 0; b < end; ++b) {
        if (b == down) p.delay.timeMs = 20.0f;
        if (b == up) p.delay.timeMs = 2000.0f;
        const bool was = e.silent();
        e.set(p, tempo(120.0));
        const size_t pos = static_cast<size_t>(b) * 128;
        if (pos < burst || !e.silent()) {
            for (size_t k = 0; k < 128; ++k) {
                l[k] = pos + k < burst ? x[pos + k] : 0.0f;
                r[k] = pos + k < burst ? y[pos + k] : 0.0f;
            }
            e.process(l, r, l, r, 128);
            if (b >= up) {
                ++ran;
                for (size_t k = 0; k < 128; ++k) old = std::max({old, std::fabs(l[k]), std::fabs(r[k])});
            }
        } else if (!skipped && b < up) {
            skipped = true;
            atSet = !was;
        }
    }
    std::printf("  echo: skipped from a set() %s, woken by the time: %d block(s) run, the loudest %.3g\n",
                atSet ? "yes" : "no", ran, static_cast<double>(old));
    CHECK(skipped && atSet);   // the case this is about
    CHECK(ran == 1);
    CHECK(old < 1e-15f);
}

// The fresh start keeps the duck's envelope: it follows the send, quiet since before the silence,
// so it has fallen on its own, and it still says how loud the send has just been. A loud burst at
// 20 ms (feedback 0, wow 0, duck 1, and Diffuse 0, whose ring would keep Echo from silence until
// the envelope had all but gone), run until silent() 54 ms after the burst, one block more (the
// fresh start), then a quiet phrase: its first repeat is ducked as a Delay that ran on all along
// ducks it, within 0.5 dB (from an emptied envelope it came through 15 dB louder).
void resumeDuck() {
    af::Echo e;
    af::Echo::Params p = af::initEcho();
    p.delay.sync = false;
    p.delay.timeMs = 20.0f;
    p.delay.feedback = 0.0f;
    p.delay.wow = 0.0f;
    p.delay.duck = 1.0f;
    p.delay.diffuse = 0.0f;
    P dp = p.delay;
    dp.mix = 1.0f;
    Delay d;
    const size_t burst = static_cast<size_t>(0.3 * kSr), n = 2 * static_cast<size_t>(kSr);
    Buf x = whiteNoise(static_cast<int>(n), 0.5f, 101), y = whiteNoise(static_cast<int>(n), 0.5f, 102);
    std::fill(x.begin() + static_cast<std::ptrdiff_t>(burst), x.end(), 0.0f);
    std::fill(y.begin() + static_cast<std::ptrdiff_t>(burst), y.end(), 0.0f);
    size_t start = 0;   // the quiet phrase: from the block after the fresh start
    Buf eL = x, eR = y, dL = x, dR = y;
    for (size_t pos = 0; pos < n; pos += 128) {
        if (start == 0 && pos > burst && e.silent()) {
            start = pos + 128;
            const Buf ql = whiteNoise(static_cast<int>(n - start), 0.01f, 103), qr = whiteNoise(static_cast<int>(n - start), 0.01f, 104);
            for (size_t i = start; i < n; ++i) {
                eL[i] = dL[i] = ql[i - start];
                eR[i] = dR[i] = qr[i - start];
            }
        }
        const int m = static_cast<int>(std::min<size_t>(128, n - pos));
        e.set(p, tempo(120.0));
        e.process(&eL[pos], &eR[pos], &eL[pos], &eR[pos], m);
        d.set(dp, tempo(120.0));
        d.process(&dL[pos], &dR[pos], m);
    }
    const size_t from = start + 882, to = from + 441;   // the repeat's first 10 ms
    const double off = db((rms(eL, from, to) + rms(eR, from, to)) / (rms(dL, from, to) + rms(dR, from, to)));
    std::printf("  echo: the duck after a fresh start, against a Delay run on: %+.2f dB\n", off);
    CHECK(start > burst && start < burst + static_cast<size_t>(kSr / 10));   // the envelope still up
    CHECK(near(off, 0.0, 0.5));
}

} // namespace

void echoTests() {
    timing();
    spread();
    pingPong();
    mono();
    feedbackRatio();
    cuts();
    glide();
    fadeLands();
    fadeNoBend();
    fadeLevel();
    fadeClicks();
    tapeUnchanged();
    drive();
    limiter();
    ducking();
    wow();
    holds();
    smooth();
    mixZero();
    extremes();
    randomJumps();
    nanInput();
    blockSizes();
    resetClears();
    tail();
    // AmbientForce's
    diffuseZero();
    diffuseSmears();
    diffuseLoss();
    diffuseComb();
    diffuseNoGrowth();
    diffuseMono();
    diffuseSmooth();
    diffuseBlockSizes();
    diffuseOdd();
    diffuseRestart();
    diffuseTail();
    echoIsDelay();
    wetOnly();
    silence();
    resume();
    resumeAfterSet();
    resumeDuck();
}

} // namespace aft
