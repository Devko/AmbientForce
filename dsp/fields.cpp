// Weather's fields: see fields.h. Pink noise is Paul Kellet's refined filter (published on
// musicdsp.org); the filters are Andrew Simper's trapezoidal SVF (svf.h's, here in double and with
// its coefficients set as they go). All of it runs once per field on the loader thread, so it is
// written plainly, in double.
#include "fields.h"

#include "common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace af {

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kPre = 44100;   // a second rendered first and dropped: everything settled when the field begins
constexpr int kLen = static_cast<int>((kFieldS + kLoopFadeS) * 44100.0f + 0.5f);   // what buildSource gets
constexpr int kN = kPre + kLen;
static_assert(static_cast<int>(kFieldS * 44100.0f) % 4 == 0, "buildSource's loop is exactly kFieldS");
constexpr double kItdS = 0.0004;          // the far ear's delay for an event panned hard
constexpr double kKneeFrom = 6.5, kKneeTo = 9.0;   // the soft knee, in the field's RMS

// --- generators ---------------------------------------------------------------------------------

// xorshift32 (fastmath.h's, through common.h), its seed spread first so neighbouring seeds start
// far apart.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) {
        seed ^= seed >> 16;
        seed *= 0x7FEB352Du;
        seed ^= seed >> 15;
        seed *= 0x846CA68Bu;
        seed ^= seed >> 16;
        s = seed ? seed : 0x9E3779B9u;
    }
    double uni() { return static_cast<double>(xorshift(s) >> 8) * (1.0 / 16777216.0); }   // [0, 1)
    double bi() { return 2.0 * uni() - 1.0; }                                               // [-1, 1)
    double range(double a, double b) { return a + (b - a) * uni(); }
    double logRange(double a, double b) { return a * std::pow(b / a, uni()); }
    double gap(double perSecond) { return -std::log(1.0 - uni()) / perSecond; }   // a Poisson process's
};
uint32_t sub(uint32_t seed, uint32_t k) { return seed + 0x632BE5ABu * (k + 1); }

// Paul Kellet's refined pink filter: within 0.05 dB of -3 dB an octave above 9 Hz.
struct Pink {
    double b[7] = {};
    double tick(double w) {
        b[0] = 0.99886 * b[0] + w * 0.0555179;
        b[1] = 0.99332 * b[1] + w * 0.0750759;
        b[2] = 0.96900 * b[2] + w * 0.1538520;
        b[3] = 0.86650 * b[3] + w * 0.3104856;
        b[4] = 0.55000 * b[4] + w * 0.5329522;
        b[5] = -0.7616 * b[5] - w * 0.0168980;
        const double p = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362;
        b[6] = w * 0.115926;
        return 0.11 * p;
    }
};
// Brown noise as these fields use it: white through a leaky integrator, flat up to about 140 Hz and
// falling 6 dB an octave above (the low-passes after it keep the band where it is brown).
struct Brown {
    double b = 0.0;
    double tick(double w) {
        b = (b + 0.02 * w) / 1.02;
        return 3.5 * b;
    }
};
// The trapezoidal SVF; bandpass() is unity at its centre.
struct Svf {
    double k = 1.0, a1 = 1.0, a2 = 0.0, a3 = 0.0, ic1 = 0.0, ic2 = 0.0, lp = 0.0, bp = 0.0;
    void set(double hz, double q) {
        const double g = std::tan(kPi * std::min(hz, 0.45 * kSr) / kSr);
        k = 1.0 / q;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void tick(double v0) {
        const double v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        lp = v2;
        bp = v1;
    }
    double lowpass(double x) {
        tick(x);
        return lp;
    }
    double bandpass(double x) {
        tick(x);
        return k * bp;
    }
};
// A one-pole low-pass at hz; highpass() is what it leaves.
struct OnePole {
    double a = 0.0, y = 0.0;
    explicit OnePole(double hz) : a(1.0 - std::exp(-2.0 * kPi * hz / kSr)) {}
    double lowpass(double x) { return y += a * (x - y); }
    double highpass(double x) { return x - lowpass(x); }
};
// A smoothed random walk in 0..1: a new random target every minS..maxS seconds, followed through
// two one-poles of tauS, so it bends and never kinks.
struct Wander {
    Rng r;
    double minS, maxS, c, target, a, b;
    int left;
    Wander(uint32_t seed, double minS_, double maxS_, double tauS)
        : r(seed), minS(minS_), maxS(maxS_), c(1.0 - std::exp(-1.0 / (tauS * kSr))) {
        target = a = b = r.uni();
        left = static_cast<int>(r.range(minS, maxS) * kSr);
    }
    double tick() {
        if (--left <= 0) {
            target = r.uni();
            left = static_cast<int>(r.range(minS, maxS) * kSr);
        }
        a += c * (target - a);
        b += c * (a - b);
        return b;
    }
};

double clamp01(double x) { return std::min(1.0, std::max(0.0, x)); }

// --- buffers ------------------------------------------------------------------------------------

struct Stereo {
    std::vector<float> L, R;
    Stereo() : L(static_cast<size_t>(kN)), R(static_cast<size_t>(kN)) {}
};

// The RMS of what is kept (both channels).
double rmsOf(const Stereo& s) {
    double sum = 0.0;
    for (int i = kPre; i < kN; ++i) sum += static_cast<double>(s.L[i]) * s.L[i] + static_cast<double>(s.R[i]) * s.R[i];
    return std::sqrt(sum / (2.0 * kLen));
}

// A part into the field at `db` relative to the field's unit (its own RMS made 1).
void mix(Stereo& out, const Stereo& part, double db) {
    const double r = rmsOf(part);
    if (r <= 0.0) return;
    const float g = static_cast<float>(std::pow(10.0, db / 20.0) / r);
    for (int i = 0; i < kN; ++i) {
        out.L[i] += g * part.L[i];
        out.R[i] += g * part.R[i];
    }
}

// An event (mono, n samples) into out from sample `at`, panned at constant power (pan -1..1), the
// far side up to itdS later.
void place(Stereo& out, int at, const std::vector<double>& ev, int n, double pan, double itdS = kItdS) {
    const double th = (pan + 1.0) * 0.25 * kPi;
    const double gl = std::cos(th), gr = std::sin(th);
    const int d = static_cast<int>(std::lround(std::fabs(pan) * itdS * kSr));
    const int atL = at + (pan > 0.0 ? d : 0), atR = at + (pan < 0.0 ? d : 0);
    for (int i = std::max(0, -std::max(atL, atR)); i < n; ++i) {
        const double x = ev[static_cast<size_t>(i)];
        if (atL + i >= 0 && atL + i < kN) out.L[static_cast<size_t>(atL + i)] += static_cast<float>(gl * x);
        if (atR + i >= 0 && atR + i < kN) out.R[static_cast<size_t>(atR + i)] += static_cast<float>(gr * x);
    }
}

// A decaying click: a burst of noise (time constant tc, rising over `rise` seconds) through a
// band-pass at f (Q q), rendered until it is 80 dB down, its peak made amp (a click's loudness is
// its level, whatever its band: a wide band would otherwise pass more of the burst), added into ev
// from `from`. Returns where it ends.
int click(Rng& r, std::vector<double>& ev, int from, double f, double q, double tc, double rise, double amp) {
    const double ring = q / (kPi * f);   // the band-pass's decay time constant
    const int n = std::min(static_cast<int>(ev.size()) - from, static_cast<int>((5.0 * tc + rise + 9.2 * ring) * kSr) + 1);
    Svf bp;
    bp.set(f, q);
    const double dec = std::exp(-1.0 / (tc * kSr)), riseN = rise * kSr;
    double e = 1.0, top = 0.0;
    std::vector<double> one(static_cast<size_t>(std::max(n, 0)));
    for (int i = 0; i < n; ++i) {
        double x = e * r.bi();
        if (i < riseN) {
            const double s = std::sin(0.5 * kPi * i / riseN);
            x *= s * s;
        }
        e *= dec;
        one[static_cast<size_t>(i)] = bp.bandpass(x);
        top = std::max(top, std::fabs(one[static_cast<size_t>(i)]));
    }
    const double g = top > 0.0 ? amp / top : 0.0;
    for (int i = 0; i < n; ++i) ev[static_cast<size_t>(from + i)] += g * one[static_cast<size_t>(i)];
    return from + n;
}

// Poisson events, perSecond of them, from the pre-roll on; each made by `make` into ev (zeroed),
// returning its length and its pan, then placed.
template <class Make>
void events(Rng& r, Stereo& out, double perSecond, Make make) {
    std::vector<double> ev(4096);
    int used = 0;
    for (double t = r.gap(perSecond); t * kSr < kN; t += r.gap(perSecond)) {
        std::fill(ev.begin(), ev.begin() + used, 0.0);
        double pan = 0.0;
        used = make(ev, pan);
        place(out, static_cast<int>(t * kSr), ev, used, pan);
    }
}

// --- the fields ---------------------------------------------------------------------------------

void rainOnRoof(uint32_t seed, Stereo& out) {
    Stereo drops, rumble;
    Rng r(seed);
    events(r, drops, 300.0, [&](std::vector<double>& ev, double& pan) {
        const double f = r.logRange(1000.0, 4000.0), q = r.range(2.0, 6.0), tc = r.range(0.0002, 0.0008);
        const double amp = std::pow(10.0, -0.9 * r.uni());   // within 18 dB
        pan = r.bi();
        return click(r, ev, 0, f, q, tc, 0.0001, amp);
    });
    for (int c = 0; c < 2; ++c) {
        Rng n(sub(seed, 1 + c));
        Brown b;
        Svf lp;
        lp.set(250.0, 0.7);
        OnePole hp(30.0);
        std::vector<float>& y = c ? rumble.R : rumble.L;
        for (int i = 0; i < kN; ++i) y[static_cast<size_t>(i)] = static_cast<float>(hp.highpass(lp.lowpass(b.tick(n.bi()))));
    }
    mix(out, drops, 0.0);
    mix(out, rumble, -7.0);
}

void lightRain(uint32_t seed, Stereo& out) {
    Stereo drops, bed;
    Rng r(seed);
    events(r, drops, 60.0, [&](std::vector<double>& ev, double& pan) {
        const double f = r.logRange(2500.0, 7000.0), q = r.range(3.0, 8.0), tc = r.range(0.0008, 0.002);
        const double amp = std::pow(10.0, -0.6 * r.uni());   // within 12 dB
        pan = r.bi();
        return click(r, ev, 0, f, q, tc, 0.0006, amp);
    });
    for (int c = 0; c < 2; ++c) {
        Rng n(sub(seed, 1 + c));
        Pink p;
        OnePole lp(6000.0);
        std::vector<float>& y = c ? bed.R : bed.L;
        for (int i = 0; i < kN; ++i) y[static_cast<size_t>(i)] = static_cast<float>(lp.lowpass(p.tick(n.bi())));
    }
    mix(out, drops, 0.0);
    mix(out, bed, -5.0);
}

void windHigh(uint32_t seed, Stereo& out) {
    Wander centre(sub(seed, 1), 1.5, 4.0, 0.7), gust(sub(seed, 2), 2.0, 6.0, 0.5);
    Wander side[2] = {Wander(sub(seed, 3), 2.0, 5.0, 1.0), Wander(sub(seed, 4), 2.0, 5.0, 1.0)};
    Rng n[2] = {Rng(sub(seed, 5)), Rng(sub(seed, 6))};
    Svf bp[2];
    for (int i = 0; i < kN; ++i) {
        const double c = centre.tick(), g = gust.tick(), s[2] = {side[0].tick(), side[1].tick()};
        const double gain = 0.15 + 0.85 * g * g;
        for (int ch = 0; ch < 2; ++ch) {
            if ((i & 15) == 0) {
                const double w = clamp01(0.55 * c + 0.3 * g + 0.3 * (s[ch] - 0.5) + 0.075);
                bp[ch].set(800.0 * std::pow(3000.0 / 800.0, w), 3.0);
            }
            (ch ? out.R : out.L)[static_cast<size_t>(i)] = static_cast<float>(gain * bp[ch].bandpass(n[ch].bi()));
        }
    }
}

void windLow(uint32_t seed, Stereo& out) {
    Wander shared(sub(seed, 1), 3.0, 7.0, 1.2), gust(sub(seed, 2), 3.0, 8.0, 1.2);
    Wander side[2] = {Wander(sub(seed, 3), 2.0, 5.0, 1.0), Wander(sub(seed, 4), 2.0, 5.0, 1.0)};
    Rng n[2] = {Rng(sub(seed, 5)), Rng(sub(seed, 6))};
    Brown b[2];
    Svf lp[2];
    OnePole hp[2] = {OnePole(25.0), OnePole(25.0)};
    for (int i = 0; i < kN; ++i) {
        const double sh = shared.tick(), g = gust.tick(), s[2] = {side[0].tick(), side[1].tick()};
        const double gain = 0.3 + 0.7 * g * g;
        for (int ch = 0; ch < 2; ++ch) {
            if ((i & 15) == 0) lp[ch].set(150.0 * std::pow(4.0, clamp01(0.6 * sh + 0.4 * s[ch])), 0.8);
            (ch ? out.R : out.L)[static_cast<size_t>(i)] =
                static_cast<float>(gain * hp[ch].highpass(lp[ch].lowpass(b[ch].tick(n[ch].bi()))));
        }
    }
}

// Surf's swell at t seconds from the field's start: two to the loop, each 2.4 s up and 3.6 s down.
double swell(double t) {
    double ph = std::fmod(t, 12.0);
    if (ph < 0.0) ph += 12.0;
    const double crest = ph < 6.0 ? 1.0 : 0.85;
    const double p = std::fmod(ph, 6.0) / 6.0;
    double e;
    if (p < 0.4) {
        e = std::sin(0.5 * kPi * p / 0.4);
    } else {
        e = std::cos(0.5 * kPi * (p - 0.4) / 0.6);
    }
    return crest * e * e;
}

void surf(uint32_t seed, Stereo& out) {
    Rng n[2] = {Rng(sub(seed, 1)), Rng(sub(seed, 2))};
    Pink p[2];
    Svf lp[2];
    double e = 0.0;
    for (int i = 0; i < kN; ++i) {
        if ((i & 15) == 0) {
            e = swell(static_cast<double>(i - kPre) / kSr);
            for (Svf& f : lp) f.set(400.0 * std::pow(15.0, e), 0.6);
        }
        const double gain = 0.16 + 0.84 * e;
        for (int ch = 0; ch < 2; ++ch)
            (ch ? out.R : out.L)[static_cast<size_t>(i)] = static_cast<float>(gain * lp[ch].lowpass(p[ch].tick(n[ch].bi())));
    }
}

void stream(uint32_t seed, Stereo& out) {
    static constexpr int kNotes[] = {72, 74, 76, 79, 81, 84, 86, 88, 91, 93, 96};   // C pentatonic, C5..C7
    constexpr int kNumNotes = static_cast<int>(sizeof kNotes / sizeof *kNotes);
    Stereo bubbles, bed;
    Rng r(seed);
    events(r, bubbles, 35.0, [&](std::vector<double>& ev, double& pan) {
        const int note = kNotes[std::min(kNumNotes - 1, static_cast<int>(r.uni() * kNumNotes))];
        const double f0 = 440.0 * std::pow(2.0, (note - 69) / 12.0), d = r.range(0.02, 0.06);
        const double amp = std::pow(10.0, -0.9 * r.uni());   // within 18 dB
        pan = 0.9 * r.bi();
        const int n = std::min(static_cast<int>(ev.size()), static_cast<int>(d * kSr));
        const double attack = 0.0015 * kSr, taper = 0.004 * kSr;
        const double rise = std::pow(1.5, 1.0 / (d * kSr)), fall = std::exp(-1.0 / (0.35 * d * kSr));
        double ph = r.uni(), w = f0 / kSr, decay = amp;   // cycles
        for (int i = 0; i < n; ++i) {
            ph += w;
            ph -= ph >= 1.0 ? 1.0 : 0.0;
            w *= rise;   // up a fifth over d, exponentially
            double env = decay;
            decay *= fall;
            if (i < attack) {
                const double s = std::sin(0.5 * kPi * i / attack);
                env *= s * s;
            }
            if (i > n - taper) {
                const double c = std::cos(0.5 * kPi * (i - (n - taper)) / taper);
                env *= c * c;
            }
            ev[static_cast<size_t>(i)] = env * sinCycle(static_cast<float>(ph));
        }
        return n;
    });
    Wander centre(sub(seed, 1), 2.0, 5.0, 1.0);
    Rng n[2] = {Rng(sub(seed, 2)), Rng(sub(seed, 3))};
    Pink p[2];
    Svf bp[2];
    OnePole lp[2] = {OnePole(800.0), OnePole(800.0)};
    for (int i = 0; i < kN; ++i) {
        const double c = centre.tick();
        for (int ch = 0; ch < 2; ++ch) {
            if ((i & 15) == 0) bp[ch].set(900.0 * std::pow(1600.0 / 900.0, c), 0.7);
            const double y = bp[ch].bandpass(n[ch].bi()) + 0.5 * lp[ch].lowpass(p[ch].tick(n[ch].bi()));
            (ch ? bed.R : bed.L)[static_cast<size_t>(i)] = static_cast<float>(y);
        }
    }
    mix(out, bubbles, 0.0);
    mix(out, bed, -8.0);
}

void embers(uint32_t seed, Stereo& out) {
    Stereo crackles, hiss, warmth;
    Rng r(seed);
    events(r, crackles, 15.0, [&](std::vector<double>& ev, double& pan) {
        const double amp = std::pow(10.0, -1.2 * r.uni());   // within 24 dB
        pan = 0.8 * r.bi();
        const int clicks = 1 + std::min(2, static_cast<int>(r.uni() * 3.0));
        int end = 0;
        for (int c = 0; c < clicks; ++c) {
            const int at = c == 0 ? 0 : static_cast<int>(r.range(0.0, 0.002) * kSr);
            const double f = r.logRange(2000.0, 8000.0), q = r.range(0.7, 2.0), tc = r.range(0.00003, 0.00015);
            end = std::max(end, click(r, ev, at, f, q, tc, 0.0, c == 0 ? amp : amp * r.range(0.3, 1.0)));
        }
        return end;
    });
    for (int c = 0; c < 2; ++c) {
        Rng n(sub(seed, 1 + c));
        OnePole lp(7000.0), hp(400.0);
        Brown b;
        Svf warm;
        warm.set(200.0, 0.7);
        for (int i = 0; i < kN; ++i) {
            (c ? hiss.R : hiss.L)[static_cast<size_t>(i)] = static_cast<float>(hp.highpass(lp.lowpass(n.bi())));
            (c ? warmth.R : warmth.L)[static_cast<size_t>(i)] = static_cast<float>(warm.lowpass(b.tick(n.bi())));
        }
    }
    mix(out, crackles, 0.0);
    mix(out, hiss, 8.0);
    mix(out, warmth, -2.0);
}

// A cricket: trains of three FM pulses (the modulator at the carrier, its index falling from 1.2 to
// 0.2 over each pulse; edges of 15% of it raised-cosine), a train every `period` s from `offset` on,
// panned; each train's level within 2 dB under amp at random. The carrier runs on the field's own
// time, so its partials sit exactly on fc.
void cricket(Rng& r, Stereo& out, double fc, double period, double offset, double pulseS, double spacing, double pan,
             double amp) {
    const int pulse = static_cast<int>(pulseS * kSr), edge = static_cast<int>(0.15 * pulse);
    std::vector<double> ev(static_cast<size_t>(2.0 * spacing * kSr) + static_cast<size_t>(pulse) + 1);
    for (double ts = offset - std::ceil(1.0 / period) * period; ts < kFieldS + kLoopFadeS; ts += period) {
        const double level = amp * std::pow(10.0, -0.1 * r.uni());
        const int at = kPre + static_cast<int>(std::lround(ts * kSr));
        std::fill(ev.begin(), ev.end(), 0.0);
        for (int p = 0; p < 3; ++p) {
            const int from = static_cast<int>(std::lround(p * spacing * kSr));
            for (int j = 0; j < pulse; ++j) {
                const double tau = static_cast<double>(j) / pulse;
                double env = 1.0;
                if (j < edge) env = std::pow(std::sin(0.5 * kPi * j / edge), 2.0);
                else if (j >= pulse - edge) env = std::pow(std::sin(0.5 * kPi * (pulse - j) / edge), 2.0);
                const double w = 2.0 * kPi * fc * static_cast<double>(at + from + j - kPre) / kSr;
                ev[static_cast<size_t>(from + j)] = level * env * std::sin(w + (0.2 + 1.0 * (1.0 - tau)) * std::sin(w));
            }
        }
        place(out, at, ev, static_cast<int>(ev.size()), pan, 0.0);   // no delay: a steady tone would comb
    }
}

void night(uint32_t seed, Stereo& out) {
    Stereo crickets, bed;
    Rng r(seed);
    // A train lasts 95 ms. The two take turns: the periods share 0.25 s, and in it the C8 cricket's
    // trains take 0.05-0.145 s and the G7's 0.175-0.27 s, so they never sound together (their peaks
    // would add). Neither crosses the loop's seam (0-0.25 s): their first trains start at 0.30 s and
    // 0.425 s, and both periods divide 12 s.
    cricket(r, crickets, 4186.009, 0.50, 0.300, 0.025, 0.035, -0.45, 1.0);   // C8
    cricket(r, crickets, 3135.963, 0.75, 0.425, 0.025, 0.035, 0.45, 0.708);  // G7, 3 dB softer
    for (int c = 0; c < 2; ++c) {
        Rng n(sub(seed, 1 + c));
        Brown b;
        Svf lp;
        lp.set(250.0, 0.7);
        OnePole hp(30.0);
        std::vector<float>& y = c ? bed.R : bed.L;
        for (int i = 0; i < kN; ++i) y[static_cast<size_t>(i)] = static_cast<float>(hp.highpass(lp.lowpass(b.tick(n.bi()))));
    }
    mix(out, crickets, 0.0);
    mix(out, bed, -6.0);
}

// Peaks over kKneeFrom times the RMS rounded off (tanh), never reaching kKneeTo times it.
void knee(Stereo& f) {
    const double r = rmsOf(f), from = kKneeFrom * r, span = (kKneeTo - kKneeFrom) * r;
    if (r <= 0.0) return;
    const auto soft = [&](float x) {
        const double a = std::fabs(x);
        if (a <= from) return x;
        const double y = from + span * std::tanh((a - from) / span);
        return static_cast<float>(x < 0.0f ? -y : y);
    };
    for (int i = kPre; i < kN; ++i) {
        f.L[static_cast<size_t>(i)] = soft(f.L[static_cast<size_t>(i)]);
        f.R[static_cast<size_t>(i)] = soft(f.R[static_cast<size_t>(i)]);
    }
}

} // namespace

std::unique_ptr<SourceBuffer> renderField(int id) {
    if (id < 0 || id >= FD_COUNT) return nullptr;
    Stereo f;
    const uint32_t seed = 0xF1E1D000u + static_cast<uint32_t>(id) * 0x9E3779B9u;
    switch (id) {
        case FD_RAIN_ROOF: rainOnRoof(seed, f); break;
        case FD_LIGHT_RAIN: lightRain(seed, f); break;
        case FD_WIND_HIGH: windHigh(seed, f); break;
        case FD_WIND_LOW: windLow(seed, f); break;
        case FD_SURF: surf(seed, f); break;
        case FD_STREAM: stream(seed, f); break;
        case FD_EMBERS: embers(seed, f); break;
        default: night(seed, f); break;
    }
    knee(f);
    return buildSource(f.L.data() + kPre, f.R.data() + kPre, kLen);
}

} // namespace af
