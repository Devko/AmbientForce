// Air's voices (dsp/airvoices.h) on their own: each sound's pitch (under Just too; the Kalimba's
// across Decay and Tone, and its loop's fundamental pole at every note), Glass's inharmonic second
// mode, the decay against Decay, Tone, no click at a strike and the modes' 1.5 ms rise, the level
// each sound is scaled to (the Kalimba's where its burst is built as it plays, against the build that
// made it whole), a burst built as it plays (the same whatever the render's pieces), no DC ringing in
// the pluck, velocity, pans and the send, stealing, sleeping, a note ringing as struck, Felt
// darkening as it fades, stability at the ends of the keyboard, odd input, determinism, and nothing
// allocating. Needs no plugin: make test-module M=airvoices runs it on its own.
#include "check.h"
#include "signal.h"
#include "../dsp/airvoices.h"
#include "../dsp/lifetime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace aft {
namespace {

using af::AirVoices;
constexpr int kBlk = 128;   // MPC's block

int samples(double s) { return static_cast<int>(std::lround(s * af::kRate)); }
size_t at(double s) { return static_cast<size_t>(samples(s)); }

// Felt Piano built (signal.h's, shared with the other suites); every other slot reads TableSet's
// fallback, the one-frame sine.
const af::TableSet& tables() {
    static af::TableSet set;
    static bool built = false;
    if (!built) {
        set.t[af::TB_FELT_PIANO].store(&testTable(af::TB_FELT_PIANO), std::memory_order_release);
        af::sineTable();
        built = true;
    }
    return set;
}

struct Out {
    Buf L, R, sendL, sendR;
};

// n more samples of v appended to o, in MPC's blocks, the send at `send`.
void play(AirVoices& v, Out& o, int n, float send = 0.0f) {
    const size_t from = o.L.size(), len = from + static_cast<size_t>(n);
    o.L.resize(len, 0.0f);
    o.R.resize(len, 0.0f);
    o.sendL.resize(len, 0.0f);
    o.sendR.resize(len, 0.0f);
    for (size_t i = from; i < len; i += kBlk) {
        const int m = static_cast<int>(std::min<size_t>(kBlk, len - i));
        v.render(tables(), &o.L[i], &o.R[i], &o.sendL[i], &o.sendR[i], send, m);
    }
}

af::AirVoicePatch patch(int sound, float decayS = 4.0f, float toneHz = 6000.0f) {
    af::AirVoicePatch p;
    p.sound = sound;
    p.decayS = decayS;
    p.toneHz = toneHz;
    return p;
}

af::HarmonyPatch equal() {
    af::HarmonyPatch h;
    h.tuning = af::TU_EQUAL;
    return h;
}

// One strike of `sound` on a fresh AirVoices: n samples of it.
Out strikeOne(const af::AirVoicePatch& p, int note, int n, float vel = 1.0f, float pan = 0.0f, uint32_t seed = 1) {
    AirVoices v;
    v.seed(seed);
    v.set(p, equal());
    v.strike(note, vel, pan);
    Out o;
    play(v, o, n);
    return o;
}

double noteHz(int note) { return 440.0 * std::exp2((note - 69) / 12.0); }
double cents(double hz, double want) { return 1200.0 * std::log2(hz / want); }

// The frequency of the strongest partial within `span` Hz of `near`, from n samples of x at `from`
// (n a power of two): Blackman-Harris, zero-padded four times, then a parabola through the log
// magnitudes of the top bin and its neighbours (ground_test.cpp's, with the length a parameter: a
// decaying partial's line is symmetric about its frequency, so the parabola finds it too).
double peakHz(const Buf& x, size_t from, size_t n, double near, double span) {
    const size_t pad = 4 * n;
    std::vector<cd> a(pad);
    for (size_t i = 0; i < n; ++i) {
        const double p = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
        const double w = 0.35875 - 0.48829 * std::cos(p) + 0.14128 * std::cos(2.0 * p) - 0.01168 * std::cos(3.0 * p);
        a[i] = x[from + i] * w;
    }
    fft(a);
    const double bin = static_cast<double>(af::kRate) / static_cast<double>(pad);
    const size_t lo = static_cast<size_t>(std::max(2.0, (near - span) / bin));
    const size_t hi = static_cast<size_t>((near + span) / bin);
    size_t k = lo;
    for (size_t i = lo; i <= hi; ++i)
        if (std::abs(a[i]) > std::abs(a[k])) k = i;
    const double m1 = std::log(std::abs(a[k - 1])), m0 = std::log(std::abs(a[k])), p1 = std::log(std::abs(a[k + 1]));
    const double d = 0.5 * (m1 - p1) / (m1 - 2.0 * m0 + p1);
    return (static_cast<double>(k) + d) * bin;
}

// The power in x[from .. from + n) above `hz` (n a power of two), in dB.
double bandDb(const Buf& x, size_t from, size_t n, double hz) {
    const Spectrum s(x, from, n);
    double e = 0.0;
    for (size_t k = 0; k < s.amp.size(); ++k)
        if (static_cast<double>(k) * s.binHz >= hz) e += s.amp[k] * s.amp[k];
    return 10.0 * std::log10(std::max(e, 1e-30));
}

// The power-weighted mean frequency of x[from .. from + n), for brightness.
double centroidHz(const Buf& x, size_t from, size_t n) {
    const Spectrum s(x, from, n);
    double num = 0.0, den = 0.0;
    for (size_t k = 1; k < s.amp.size(); ++k) {
        const double p = s.amp[k] * s.amp[k];
        num += p * static_cast<double>(k) * s.binHz;
        den += p;
    }
    return den > 0.0 ? num / den : 0.0;
}

// Check 1: note 69 under Equal has its fundamental (Bell: its prime, ratio 1) at 440 Hz within
// 1 cent, Kalimba within 2. The Kalimba's loop is tuned for the phase its low-pass and its
// fractional read add, so it holds across the keyboard too.
void testPitch() {
    std::printf("== airvoices: pitch\n");
    constexpr size_t kFft = size_t{1} << 17;   // 3 s: Bowl's pair, 1.76 Hz apart at 440, resolved
    for (int s = 0; s < af::AS_COUNT; ++s) {
        const Out o = strikeOne(patch(s, 20.0f), 69, samples(0.05) + static_cast<int>(kFft));
        const double hz = peakHz(o.L, at(0.05), kFft, 440.0, 15.0), c = cents(hz, 440.0);
        std::printf("  %-8s %.4f Hz (%+.3f cents)\n", af::kAirSoundNames[s], hz, c);
        CHECK(std::fabs(c) <= (s == af::AS_KALIMBA ? 2.0 : 1.0));
    }
    double worst = 0.0;
    for (int note : {24, 36, 48, 60, 84, 96, 108}) {
        const Out o = strikeOne(patch(af::AS_KALIMBA, 20.0f, 16000.0f), note, samples(0.01) + static_cast<int>(kFft));
        const double want = noteHz(note), hz = peakHz(o.L, at(0.01), kFft, want, std::min(15.0, 0.3 * want));
        worst = std::max(worst, std::fabs(cents(hz, want)));
    }
    std::printf("  Kalimba, notes 24..108 at Tone 16000: within %.3f cents\n", worst);
    CHECK(worst <= 2.0);

    // The tuning reaches every sound: under Just in C, E (note 64) is a pure major third over C4,
    // 261.626 x 5/4 = 327.032 Hz, 13.7 cents under its equal-tempered 329.628.
    af::HarmonyPatch just;
    just.tuning = af::TU_JUST;
    just.key = 0;
    const double third = noteHz(60) * 1.25;
    for (int s = 0; s < af::AS_COUNT; ++s) {
        AirVoices v;
        v.seed(1);
        v.set(patch(s, 20.0f), just);
        v.strike(64, 1.0f, 0.0f);
        Out o;
        play(v, o, samples(0.05) + static_cast<int>(kFft));
        const double hz = peakHz(o.L, at(0.05), kFft, third, 10.0), c = cents(hz, third);
        std::printf("  %-8s note 64 under Just in C: %.3f Hz (%+.3f cents from 5/4 over C4)\n", af::kAirSoundNames[s], hz,
                    c);
        CHECK(std::fabs(c) <= (s == af::AS_KALIMBA ? 0.25 : 1.0));
    }
}

// Check 1 for the Kalimba across its patch, as it sounds: Decay 0.33, 0.5, 4 and 20 s times Tone 200,
// 6000 and 16000, over the keyboard and closely at its top. The plan asks for 2 cents; the loop is
// tuned to within 0.1 (the header), so the check holds it to 0.25. (Decay 0.1 is testPluckPoles()'s:
// there the fundamental's line is too wide at the bottom of the keyboard for this measurement.) Decay
// and Tone move the loop's low-pass, and with it what the read's fraction makes up: when the read's
// whole samples were chosen by an iteration that could swap them back and forth, notes 105 and 108
// were 6 and 9.5 cents off at Decay 0.33 and 0.5, at every Tone; tuned on the unit circle rather than
// at the decaying pole, notes 45 to 47 were 2.0 to 2.15 cents flat at Decay 0.33 and Tone 200. Each
// strike is measured over about half its Decay, from 2^13 samples at Decay 0.33 to 2^16 at 4 and 20.
void testPluckTuning() {
    std::printf("== airvoices: Kalimba tuning across Decay and Tone\n");
    const int notes[] = {24, 31, 38, 45, 46, 47, 52, 60, 67, 71, 76, 81, 86, 90, 93, 96, 98, 99, 100, 101, 102,
                         103, 104, 105, 106, 107, 108};
    double worst = 0.0;
    for (float decay : {0.33f, 0.5f, 4.0f, 20.0f})
        for (float tone : {200.0f, 6000.0f, 16000.0f}) {
            size_t n = size_t{1} << 13;
            while (n < (size_t{1} << 16) && static_cast<double>(2 * n) <= 0.6 * decay * af::kRate) n *= 2;
            double most = 0.0;
            int mostAt = 0;
            for (int note : notes) {
                const Out o = strikeOne(patch(af::AS_KALIMBA, decay, tone), note, samples(0.035) + static_cast<int>(n));
                const double want = noteHz(note), hz = peakHz(o.L, at(0.035), n, want, std::min(15.0, 0.3 * want));
                const double c = cents(hz, want);
                if (std::fabs(c) > most) {
                    most = std::fabs(c);
                    mostAt = note;
                }
            }
            std::printf("  Decay %5.2f s, Tone %5.0f: within %.3f cents (the most at note %d)\n", decay, tone, most, mostAt);
            worst = std::max(worst, most);
        }
    CHECK(worst <= 0.25);
}

// Check 1 for the Kalimba everywhere, from its loop: the fundamental's pole, found from the
// coefficients the voice runs (the line's whole samples W, the allpass's eta, the low-pass's a and the
// gain g, as voice() shows them), at every note from 24 to 108, Decay 0.1, 0.33, 0.5, 1, 4 and 20 s
// and Tone 200, 1000, 6000 and 16000. The loop is y = g LP(AP(y z^-W)), so its poles solve
// W ln z = ln(g AP(z) LP(z)) + 2 pi i k, the fundamental's with k = 1: Newton from where the tuning
// aims, e^(-sigma + i w0). Its angle is the pitch: within 0.1 cents from Decay 0.33 up, and 0.5 at
// Decay 0.1, where the measurement above can't follow (the line is too wide at the bottom of the
// keyboard: the FFT read up to 4 cents there) and where the loop gain, worked out on the unit circle
// while the tuning is done at the pole, puts the pole a little off its aim. Its radius is the
// fundamental's fall: Decay within 5% everywhere (the plan's 10%).
void testPluckPoles() {
    std::printf("== airvoices: the Kalimba's fundamental pole, every note\n");
    double worstCents = 0.0, worstShort = 0.0, worstDecay = 0.0;
    bool fundamental = true;   // Newton found the pole with k = 1 every time
    for (float decay : {0.1f, 0.33f, 0.5f, 1.0f, 4.0f, 20.0f})
        for (float tone : {200.0f, 1000.0f, 6000.0f, 16000.0f}) {
            double most = 0.0, mostDecay = 0.0;
            for (int note = 24; note <= 108; ++note) {
                AirVoices v;
                v.set(patch(af::AS_KALIMBA, decay, tone), equal());
                v.strike(note, 1.0f, 0.0f);
                const AirVoices::VoiceView w = v.voice(0);
                const double W = w.loopDelay, eta = w.loopEta, a = w.loopA, b = 1.0 - a, g = w.loopG;
                const double w0 = 2.0 * kPi * noteHz(note) / af::kRate, sigma = 6.907755278982137 / (decay * af::kRate);
                auto F = [&](cd z) {
                    return W * std::log(z) - std::log(g) - std::log(eta * z + 1.0) + std::log(z + eta) - std::log(a) -
                           std::log(z) + std::log(z - b);
                };
                cd z = std::exp(cd(-sigma, w0));
                const double k = std::round(F(z).imag() / (2.0 * kPi));
                for (int it = 0; it < 30; ++it) {
                    const cd f = F(z) - cd(0.0, 2.0 * kPi * k);
                    const cd df = W / z - eta / (eta * z + 1.0) + 1.0 / (z + eta) - 1.0 / z + 1.0 / (z - b);
                    z -= f / df;
                }
                const double c = std::fabs(cents(std::arg(z) * af::kRate / (2.0 * kPi), noteHz(note)));
                const double t60 = 6.907755278982137 / (-std::log(std::abs(z)) * af::kRate);
                fundamental = fundamental && k == 1.0;
                most = std::max(most, c);
                mostDecay = std::max(mostDecay, std::fabs(t60 / decay - 1.0));
            }
            std::printf("  Decay %5.2f s, Tone %5.0f: the pitch within %.3f cents, the fall within %.2f%% of Decay\n", decay,
                        tone, most, 100.0 * mostDecay);
            double& c = decay < 0.2f ? worstShort : worstCents;
            c = std::max(c, most);
            worstDecay = std::max(worstDecay, mostDecay);
        }
    CHECK(fundamental);
    CHECK(worstCents <= 0.1);
    CHECK(worstShort <= 0.5);
    CHECK(worstDecay <= 0.05);
}

// Check 2: Glass's second mode at 2.32 times the fundamental, within 0.5%; Bar's at 2.756.
void testInharmonic() {
    std::printf("== airvoices: inharmonic modes\n");
    constexpr size_t kFft = size_t{1} << 16;
    struct Mode {
        int sound;
        double ratio;
    };
    for (const Mode m : {Mode{af::AS_GLASS, 2.32}, Mode{af::AS_BAR, 2.756}}) {
        const Out o = strikeOne(patch(m.sound, 20.0f, 16000.0f), 69, samples(0.05) + static_cast<int>(kFft));
        const double hz = peakHz(o.L, at(0.05), kFft, 440.0 * m.ratio, 30.0), r = hz / 440.0;
        std::printf("  %-6s second mode %.2f Hz = f x %.4f (want %.3f)\n", af::kAirSoundNames[m.sound], hz, r, m.ratio);
        CHECK(std::fabs(r / m.ratio - 1.0) <= 0.005);
    }
}

// Check 3: the fundamental's level falls 60 dB in Decay +-10%, at Decay 2 s and 8 s, for every
// modal sound and the Kalimba, at note 69 and Tone 16000. Measured from the fundamental's level in
// two Hann windows of 1.5 s, 0.4 Decay apart (a partial alone falls by exactly its decay between
// them; Bowl's partner 1.76 Hz above sits in the window's sidelobes, 34 dB down). Then the pluck high
// up with a dark Tone (notes 84 and 96 at Tone 1000), where the loop's low-pass at Tone would take
// the fundamental down 60 dB in under 20 ms whatever the gain: it is raised so Decay still holds.
void testDecay() {
    std::printf("== airvoices: decay\n");
    constexpr size_t kWin = size_t{1} << 16;
    auto t60 = [&](int sound, int note, float decay, float tone) {
        const double gap = 0.4 * decay, hz = noteHz(note);
        const size_t t1 = at(0.05), t2 = at(0.05 + gap);
        const Out o = strikeOne(patch(sound, decay, tone), note, static_cast<int>(t2 + kWin));
        return 60.0 * gap / (db(magnitude(o.L, hz, t1, t1 + kWin)) - db(magnitude(o.L, hz, t2, t2 + kWin)));
    };
    for (int s : {af::AS_GLASS, af::AS_BOWL, af::AS_BAR, af::AS_BELL, af::AS_KALIMBA})
        for (float d : {2.0f, 8.0f}) {
            const double t = t60(s, 69, d, 16000.0f);
            std::printf("  %-8s Decay %4.1f s: falls 60 dB in %.3f s (%+.1f%%)\n", af::kAirSoundNames[s], d, t,
                        100.0 * (t / d - 1.0));
            CHECK(std::fabs(t / d - 1.0) <= 0.1);
        }
    for (int note : {84, 96}) {
        const double t = t60(af::AS_KALIMBA, note, 4.0f, 1000.0f);
        std::printf("  Kalimba  note %d, Tone 1000, Decay 4 s: falls 60 dB in %.3f s\n", note, t);
        CHECK(std::fabs(t / 4.0 - 1.0) <= 0.1);
    }
}

// Check 4: Tone 500 Hz puts the band above 4 kHz at least 20 dB under where Tone 16000 has it, for
// every modal sound and the Kalimba (Felt's brightness is its table's: Tone doesn't apply). Note 96:
// every sound has modes above 4 kHz there.
void testTone() {
    std::printf("== airvoices: tone\n");
    constexpr size_t kFft = size_t{1} << 14;
    for (int s : {af::AS_GLASS, af::AS_BOWL, af::AS_BAR, af::AS_BELL, af::AS_KALIMBA}) {
        const double dark = bandDb(strikeOne(patch(s, 4.0f, 500.0f), 96, static_cast<int>(kFft)).L, 0, kFft, 4000.0);
        const double bright = bandDb(strikeOne(patch(s, 4.0f, 16000.0f), 96, static_cast<int>(kFft)).L, 0, kFft, 4000.0);
        std::printf("  %-8s above 4 kHz: Tone 500 %.1f dB, Tone 16000 %.1f dB (%.1f dB under)\n", af::kAirSoundNames[s],
                    dark, bright, bright - dark);
        CHECK(dark <= bright - 20.0);
    }
}

// Check 5: no click. The largest step between samples in a strike's first 2 ms is no more than 3
// times the largest over the next 20 ms, at Tone 16000 (the brightest strike), low, middle and high.
// This passes without the modes' rise too (2.04 at most with it taken out), so testRise() checks the
// rise itself.
void testNoClick() {
    std::printf("== airvoices: no click\n");
    double worst = 0.0;
    for (int s = 0; s < af::AS_COUNT; ++s)
        for (int note : {24, 36, 60, 84, 108}) {
            const Out o = strikeOne(patch(s, 4.0f, 16000.0f), note, samples(0.03));
            const double first = maxStep(o.L, 0, at(0.002)), next = maxStep(o.L, at(0.002), at(0.022));
            const double r = next > 0.0 ? first / next : 99.0;
            worst = std::max(worst, r);
            if (r > 2.0)
                std::printf("  %s at %d: first 2 ms %.4f, next 20 ms %.4f (%.2f)\n", af::kAirSoundNames[s], note, first,
                            next, r);
            CHECK(first <= 3.0 * next);
        }
    std::printf("  the first 2 ms's largest step over the next 20 ms's: %.2f at most\n", worst);
}

// Check 5's rise, which the steps above don't see (each z starts real, so every mode starts from 0
// and the steps are small with or without it). A modal strike's output is its modes' sum times a
// raised cosine over its first 1.5 ms (66 samples). Past the rise the modes alone play,
// sum_k A_k r_k^(n+1) sin(w_k (n+1)), with each mode's r and w as AirVoices works them out from the
// plan's table. So the A_k are fitted there by least squares, and the strike's first 2 ms must be that
// sum times 0.5 - 0.5 cos(pi (n+1) / 66): within 1e-4 of the strike's peak. With no rise the first
// 1.5 ms would be off by up to the sum itself, and a rise of 1 ms by up to a third of it.
void testRise() {
    std::printf("== airvoices: the modes' 1.5 ms rise\n");
    struct Table {
        double ratio[6], div[6];
    };
    const Table kTable[4] = {
        {{1, 2.32, 4.25, 6.63, 9.38, 12.5}, {1, 1.6, 2.4, 3.5, 4.8, 6}},          // Glass
        {{1, 1.004, 2.71, 2.717, 5.12, 8.18}, {1, 1, 1.5, 1.5, 2.2, 3}},          // Bowl
        {{1, 2.756, 5.404, 8.933, 13.34, 18.64}, {1, 2, 3.5, 5, 7, 9}},           // Bar
        {{0.5, 1, 1.183, 1.506, 2, 2.514}, {0.6, 1, 1.4, 1.8, 2.2, 3}},           // Bell
    };
    constexpr int kRise = 66, kFrom = kRise, kFit = 2048, kCheck = 88;
    constexpr double kDecay = 4.0;
    double worst = 0.0, bare = 1.0;
    for (int s = 0; s < 4; ++s)
        for (int note : {60, 84, 108}) {
            const Out o = strikeOne(patch(s, static_cast<float>(kDecay), 16000.0f), note, kFrom + kFit);
            // Each mode's rotation a sample as AirVoices rounds it: r e^(iw) in float.
            double rr[6], ww[6];
            int modes = 0;
            for (int k = 0; k < 6; ++k) {
                const double f = noteHz(note) * kTable[s].ratio[k];
                if (f > 18000.0) continue;
                const double r = std::exp(-6.907755278982137 * kTable[s].div[k] / (kDecay * af::kRate));
                const double w = 2.0 * kPi * f / af::kRate;
                const float c = static_cast<float>(r * std::cos(w)), d = static_cast<float>(r * std::sin(w));
                rr[modes] = std::hypot(static_cast<double>(c), static_cast<double>(d));
                ww[modes] = std::atan2(static_cast<double>(d), static_cast<double>(c));
                ++modes;
            }
            auto basis = [&](int k, int n) { return std::pow(rr[k], n + 1) * std::sin(ww[k] * (n + 1)); };
            // The normal equations over [kFrom, kFrom + kFit), solved by elimination with pivoting.
            double M[6][7] = {};
            for (int n = kFrom; n < kFrom + kFit; ++n) {
                double b[6];
                for (int k = 0; k < modes; ++k) b[k] = basis(k, n);
                for (int i = 0; i < modes; ++i) {
                    for (int j = 0; j < modes; ++j) M[i][j] += b[i] * b[j];
                    M[i][modes] += b[i] * o.L[static_cast<size_t>(n)];
                }
            }
            for (int c = 0; c < modes; ++c) {
                int p = c;
                for (int i = c + 1; i < modes; ++i)
                    if (std::fabs(M[i][c]) > std::fabs(M[p][c])) p = i;
                for (int j = 0; j <= modes; ++j) std::swap(M[c][j], M[p][j]);
                for (int i = 0; i < modes; ++i)
                    if (i != c) {
                        const double f = M[i][c] / M[c][c];
                        for (int j = c; j <= modes; ++j) M[i][j] -= f * M[c][j];
                    }
            }
            const double pk = peak(o.L);
            double err = 0.0, off = 0.0;
            for (int n = 0; n < kCheck; ++n) {
                double u = 0.0;
                for (int k = 0; k < modes; ++k) u += M[k][modes] / M[k][k] * basis(k, n);
                const double g = n < kRise ? 0.5 - 0.5 * std::cos(kPi * (n + 1) / kRise) : 1.0;
                err = std::max(err, std::fabs(o.L[static_cast<size_t>(n)] - g * u) / pk);
                off = std::max(off, std::fabs(o.L[static_cast<size_t>(n)] - u) / pk);
            }
            std::printf("  %-6s note %3d: %d modes; the first 2 ms off the raised cosine by %.1e of the peak (off the "
                        "bare modes by %.2f)\n",
                        af::kAirSoundNames[s], note, modes, err, off);
            worst = std::max(worst, err);
            bare = std::min(bare, off);
        }
    CHECK(worst <= 1e-4);
    CHECK(bare >= 0.05);   // the check can tell: the rise is there to see at every note
}

// The level each sound is scaled to: a strike at velocity 1 in the middle (note 72, Tone 16000)
// peaks at -6 dBFS +-1 dB. The Kalimba's noise is its own each strike: over eight seeds its mean
// lands there, each strike within 2.5 dB. Elsewhere on the keyboard, printed: the modes thin out up
// high (those over 18 kHz are left out).
void testLevels() {
    std::printf("== airvoices: levels\n");
    for (int s = 0; s < af::AS_COUNT; ++s) {
        const int seeds = s == af::AS_KALIMBA ? 8 : 1;
        double mean = 0.0, lo = 0.0, hi = -240.0;
        for (uint32_t seed = 1; seed <= static_cast<uint32_t>(seeds); ++seed) {
            const double pk = db(peak(strikeOne(patch(s, 4.0f, 16000.0f), 72, samples(1.0), 1.0f, 0.0f, seed).L));
            mean += pk / seeds;
            lo = seed == 1 ? pk : std::min(lo, pk);
            hi = std::max(hi, pk);
        }
        std::printf("  %-8s note 72: peak %.2f dBFS", af::kAirSoundNames[s], mean);
        if (seeds > 1) std::printf(" (the mean of %d strikes, %.2f to %.2f)", seeds, lo, hi);
        std::printf("; notes 24 / 48 / 96 / 108:");
        for (int note : {24, 48, 96, 108})
            std::printf(" %.1f", db(peak(strikeOne(patch(s, 4.0f, 16000.0f), note, samples(1.0)).L)));
        std::printf("\n");
        CHECK(std::fabs(mean + 6.0) <= 1.0);
        CHECK(lo >= -8.5 && hi <= -3.5);
    }
}

// The Kalimba's level at the bottom of the keyboard, where a burst is a period long and is built as
// it plays (notes 71 and under), against the build that made each burst whole at its strike (0f57d6c),
// with the same noise for each strike (seeds 1 to 256). Two things changed there:
// - The burst's scale is the windowed noise's expected RMS, not the RMS it turns out to have, so the
//   noise's luck now reaches the level: about 5.4 / sqrt(len) dB from strike to strike.
// - Its mean is taken with the weights of the loop's DC mode, not plainly. That takes a little of the
//   fundamental with it where the mode falls fast (the weights then lean on the burst's end): per
//   strike the fundamental's excitation moves by up to 2.9 dB at Decay 0.1 and 0.86 dB at 0.5 (notes
//   24 to 36), within 0.12 dB from Decay 4 up and from note 60 up.
// So, at Decay 0.1, 0.5 and 4 and Tone 200, 6000 and 16000: the first period's mean RMS and peak within
// 0.2 dB of the old build's; its RMS's spread within 1.02 sqrt(old^2 + (1.15 x 5.4 / sqrt(len))^2);
// the fundamental's mean level in the two periods after the burst within 0.5 dB at Decay 0.1 (it is
// 0.4 lower at note 24) and 0.12 dB from Decay 0.5 up; and its spread from strike to strike, about
// 6 dB in either build, within 0.3 dB of the old one.
void testPluckLevels() {
    std::printf("== airvoices: the Kalimba's level where its burst is built as it plays\n");
    struct Old {
        float decay, tone;
        int note;
        double rms, rmsSd, peak, fund, fundSd;   // dB: the means, and the spreads (standard deviations)
    };
    // Measured on 0f57d6c by this function's own loop.
    const Old kOld[] = {
        {0.1f, 16000.0f, 24, -14.934, 0.069, -4.978, -65.811, 4.990},
        {0.1f, 16000.0f, 36, -14.952, 0.098, -5.147, -53.232, 5.970},
        {0.1f, 16000.0f, 48, -14.979, 0.138, -5.410, -44.934, 5.858},
        {0.1f, 16000.0f, 60, -14.992, 0.199, -5.642, -37.844, 5.697},
        {0.1f, 16000.0f, 70, -15.079, 0.269, -6.031, -33.928, 5.613},
        {0.1f, 6000.0f, 24, -19.565, 0.296, -7.284, -65.819, 5.003},
        {0.1f, 6000.0f, 36, -19.638, 0.420, -7.815, -53.236, 5.986},
        {0.1f, 6000.0f, 48, -19.729, 0.581, -8.334, -44.929, 5.849},
        {0.1f, 6000.0f, 60, -19.790, 0.826, -9.092, -37.831, 5.646},
        {0.1f, 6000.0f, 70, -20.096, 1.009, -10.036, -34.081, 5.743},
        {0.1f, 200.0f, 24, -35.975, 1.893, -26.199, -66.737, 5.770},
        {0.1f, 200.0f, 36, -37.535, 2.783, -28.971, -54.673, 6.118},
        {0.1f, 200.0f, 48, -40.167, 3.199, -32.680, -48.716, 5.668},
        {0.1f, 200.0f, 60, -43.916, 4.062, -37.212, -47.447, 5.716},
        {0.1f, 200.0f, 70, -48.321, 4.519, -42.252, -49.425, 5.872},
        {0.5f, 16000.0f, 24, -14.934, 0.069, -4.978, -48.565, 5.686},
        {0.5f, 16000.0f, 36, -14.952, 0.098, -5.147, -43.489, 5.916},
        {0.5f, 16000.0f, 48, -14.979, 0.138, -5.410, -39.686, 5.799},
        {0.5f, 16000.0f, 60, -14.992, 0.199, -5.642, -35.134, 5.662},
        {0.5f, 16000.0f, 70, -15.079, 0.269, -6.031, -32.414, 5.638},
        {0.5f, 6000.0f, 24, -19.565, 0.296, -7.284, -48.572, 5.696},
        {0.5f, 6000.0f, 36, -19.638, 0.420, -7.815, -43.487, 5.911},
        {0.5f, 6000.0f, 48, -19.729, 0.581, -8.334, -39.656, 5.676},
        {0.5f, 6000.0f, 60, -19.790, 0.826, -9.092, -35.129, 5.638},
        {0.5f, 6000.0f, 70, -20.096, 1.009, -10.036, -32.571, 5.789},
        {0.5f, 200.0f, 24, -35.975, 1.893, -26.199, -49.239, 5.978},
        {0.5f, 200.0f, 36, -37.535, 2.783, -28.971, -44.880, 6.410},
        {0.5f, 200.0f, 48, -40.167, 3.199, -32.680, -43.473, 5.760},
        {0.5f, 200.0f, 60, -43.917, 4.062, -37.212, -44.940, 5.726},
        {0.5f, 200.0f, 70, -48.321, 4.519, -42.252, -48.009, 5.920},
        {4.0f, 16000.0f, 24, -14.934, 0.069, -4.978, -44.051, 5.801},
        {4.0f, 16000.0f, 36, -14.952, 0.098, -5.147, -41.135, 5.910},
        {4.0f, 16000.0f, 48, -14.979, 0.138, -5.410, -38.464, 5.728},
        {4.0f, 16000.0f, 60, -14.992, 0.199, -5.642, -34.527, 5.659},
        {4.0f, 16000.0f, 70, -15.079, 0.269, -6.031, -32.079, 5.646},
        {4.0f, 6000.0f, 24, -19.565, 0.296, -7.284, -44.053, 5.786},
        {4.0f, 6000.0f, 36, -19.638, 0.420, -7.815, -41.133, 5.907},
        {4.0f, 6000.0f, 48, -19.729, 0.581, -8.334, -38.433, 5.601},
        {4.0f, 6000.0f, 60, -19.790, 0.826, -9.092, -34.523, 5.639},
        {4.0f, 6000.0f, 70, -20.096, 1.009, -10.036, -32.236, 5.798},
        {4.0f, 200.0f, 24, -35.975, 1.893, -26.199, -44.723, 6.047},
        {4.0f, 200.0f, 36, -37.535, 2.783, -28.971, -42.517, 6.283},
        {4.0f, 200.0f, 48, -40.167, 3.199, -32.680, -42.371, 5.785},
        {4.0f, 200.0f, 60, -43.917, 4.062, -37.212, -44.385, 5.730},
        {4.0f, 200.0f, 70, -48.321, 4.519, -42.252, -47.689, 5.935},
    };
    constexpr int kStrikes = 256;
    auto meanSd = [](double s, double s2, double& sd) {
        const double m = s / kStrikes;
        sd = std::sqrt(std::max(s2 / kStrikes - m * m, 0.0));
        return m;
    };
    double worstFirst = 0.0, worstSpread = 0.0, worstFund = 0.0, worstFundShort = 0.0, worstFundSd = 0.0;
    for (const Old& old : kOld) {
        const double f0 = noteHz(old.note), period = af::kRate / f0;
        const size_t len = static_cast<size_t>(std::max(88.0, std::ceil(period)));
        const size_t win = static_cast<size_t>(2 * std::lround(period));
        double sr = 0.0, sr2 = 0.0, sp = 0.0, sf = 0.0, sf2 = 0.0;
        for (uint32_t seed = 1; seed <= kStrikes; ++seed) {
            const Out o = strikeOne(patch(af::AS_KALIMBA, old.decay, old.tone), old.note, static_cast<int>(len + win), 1.0f,
                                    0.0f, seed);
            const double r = db(rms(o.L, 0, len)), p = db(peak(o.L, 0, len)), f = db(magnitude(o.L, f0, len, len + win));
            sr += r;
            sr2 += r * r;
            sp += p;
            sf += f;
            sf2 += f * f;
        }
        double rmsSd = 0.0, fundSd = 0.0;
        const double rmsMean = meanSd(sr, sr2, rmsSd), peakMean = sp / kStrikes, fundMean = meanSd(sf, sf2, fundSd);
        const double luck = 1.15 * 5.4 / std::sqrt(static_cast<double>(len));
        // The two add about as independent spreads; at a dark Tone, where the filtered noise's own
        // luck is large, a little more (2%).
        const double spreadMost = 1.02 * std::sqrt(old.rmsSd * old.rmsSd + luck * luck);
        std::printf("  Decay %.1f Tone %5.0f note %d: first period RMS %+.3f dB, peak %+.3f dB, spread %.3f dB "
                    "(old %.3f, at most %.3f); fundamental %+.3f dB, spread %.2f dB (old %.2f)\n",
                    old.decay, old.tone, old.note, rmsMean - old.rms, peakMean - old.peak, rmsSd, old.rmsSd, spreadMost,
                    fundMean - old.fund, fundSd, old.fundSd);
        worstFirst = std::max({worstFirst, std::fabs(rmsMean - old.rms), std::fabs(peakMean - old.peak)});
        worstSpread = std::max(worstSpread, rmsSd / spreadMost);
        double& fund = old.decay < 0.2f ? worstFundShort : worstFund;
        fund = std::max(fund, std::fabs(fundMean - old.fund));
        worstFundSd = std::max(worstFundSd, std::fabs(fundSd - old.fundSd));
    }
    CHECK(worstFirst <= 0.2);
    CHECK(worstSpread <= 1.0);
    CHECK(worstFundShort <= 0.5);
    CHECK(worstFund <= 0.12);
    CHECK(worstFundSd <= 0.3);
}

// The strike's cost is spread over the samples that play its burst. A long burst (notes 71 and under)
// is built as the loop reaches it, never ahead: none of it at the strike, then exactly what has
// played. A short one (2 ms) is built whole at the strike, 88 samples. Seen through voice(i).burst,
// rendering in pieces of odd sizes. (A long burst's strike still draws its noise once, for its mean:
// the header has that pass's cost.) Until a long burst has all gone in, its voice's level is the most
// it can play, so a note still being struck is never the quietest. What a strike plays is the same
// bit for bit whatever the pieces it is rendered in.
void testBurstSpread() {
    std::printf("== airvoices: a burst is built as it plays\n");
    for (int note : {24, 48, 71, 72, 84, 108}) {
        AirVoices v;
        v.seed(2);
        v.set(patch(af::AS_KALIMBA, 4.0f, 6000.0f), equal());
        v.strike(note, 1.0f, 0.0f);
        const AirVoices::VoiceView w = v.voice(0);
        const bool spread = w.burstLen > 88;
        std::printf("  note %3d: a burst of %d samples, %d of them built at the strike\n", note, w.burstLen, w.burst);
        CHECK(w.burst == (spread ? 0 : w.burstLen) && w.burstLen <= 1349);
        CHECK(w.level > 0.0f);
        Out o;
        int played = 0;
        bool ahead = false;
        const int pieces[] = {77, 128, 33, 1, 96};
        for (int k = 0; played < w.burstLen + 256; ++k) {
            const int n = pieces[k % 5];
            play(v, o, n);
            played += n;
            const AirVoices::VoiceView now = v.voice(0);
            ahead = ahead || now.burst != (spread ? std::min(played, w.burstLen) : w.burstLen);
            if (spread && played < w.burstLen) ahead = ahead || now.level < w.level * 0.99f;
        }
        CHECK(!ahead);
        // And what it plays doesn't depend on the pieces: the same strike in whole blocks, bit for bit.
        const Out whole = strikeOne(patch(af::AS_KALIMBA, 4.0f, 6000.0f), note, played, 1.0f, 0.0f, 2);
        CHECK(o.L == whole.L && o.R == whole.R && o.sendL == whole.sendL);
    }
}

// No DC rings in the pluck. Its loop has a DC mode, a real pole just under 1 that falls no faster
// than the fundamental (slower where a dark Tone takes from the fundamental). A burst whose plain
// mean was taken out still set it ringing, up to 25 dB under the fundamental at Decay 0.5 and note
// 24; taken out with the mode's own weights, nothing is left for it. Measured after the burst and a
// period more, over 2^13 samples (Decay 0.5) or 2^14, the fundamental's fall taken out and under
// Blackman-Harris: the offset at least 90 dB under the fundamental's amplitude (93.8 at the least,
// measured; float's rounding of the weighted sums sets it). Notes 24 to 70 (bursts built as they
// play) and 84 (built at the strike), Decay 0.5, 4 and 20 s, Tone 200, 6000 and 16000, four strikes
// each.
void testPluckDc() {
    std::printf("== airvoices: no DC rings in the pluck\n");
    double worst = -240.0;
    for (int note : {24, 36, 48, 60, 70, 84})
        for (float decay : {0.5f, 4.0f, 20.0f})
            for (float tone : {200.0f, 6000.0f, 16000.0f}) {
                double most = -240.0;
                for (uint32_t seed = 1; seed <= 4; ++seed) {
                    const size_t n = decay < 1.0f ? size_t{1} << 13 : size_t{1} << 14;
                    const size_t from = static_cast<size_t>(2.0 * std::max(88.0, std::ceil(af::kRate / noteHz(note))) + 64);
                    const Out o = strikeOne(patch(af::AS_KALIMBA, decay, tone), note, static_cast<int>(from + n), 1.0f, 0.0f, seed);
                    // The fundamental's fall taken out first, so it stands still under the window and
                    // leaks nothing into the offset (falling, its leakage reached -25 dB at Decay 0.5).
                    const double w0 = 2.0 * kPi * noteHz(note) / af::kRate;
                    const double rise = 6.907755278982137 / (decay * af::kRate);
                    double dc = 0.0, re = 0.0, im = 0.0, wsum = 0.0;
                    for (size_t i = 0; i < n; ++i) {
                        const double p = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
                        const double w = 0.35875 - 0.48829 * std::cos(p) + 0.14128 * std::cos(2.0 * p) - 0.01168 * std::cos(3.0 * p);
                        const double x = o.L[from + i] * w * std::exp(rise * static_cast<double>(i));
                        dc += x;
                        re += x * std::cos(w0 * static_cast<double>(i));
                        im += x * std::sin(w0 * static_cast<double>(i));
                        wsum += w;
                    }
                    const double rel = db(std::fabs(dc) / wsum) - db(2.0 * std::hypot(re, im) / wsum);
                    most = std::max(most, rel);
                }
                if (most > -100.0)
                    std::printf("  note %3d, Decay %4.1f s, Tone %5.0f: the offset %.1f dB under the fundamental\n", note, decay,
                                tone, -most);
                worst = std::max(worst, most);
            }
    std::printf("  the offset at least %.1f dB under the fundamental\n", -worst);
    CHECK(worst <= -90.0);
}

// Velocity is a gain of 0.25 + 0.75 vel; a pan is equal power, unity in the middle; the send is the
// dry at spaceSend, gliding across a render from the last render's.
void testVelocityPanSend() {
    std::printf("== airvoices: velocity, pans, the send\n");
    const af::AirVoicePatch p = patch(af::AS_GLASS, 4.0f, 6000.0f);
    const Out loud = strikeOne(p, 72, samples(0.2), 1.0f), soft = strikeOne(p, 72, samples(0.2), 0.0f);
    const Out half = strikeOne(p, 72, samples(0.2), 0.5f);
    std::printf("  peaks: vel 1 %.4f, vel 0.5 %.4f, vel 0 %.4f\n", peak(loud.L), peak(half.L), peak(soft.L));
    CHECK(std::fabs(peak(soft.L) / peak(loud.L) - 0.25) < 1e-3);
    CHECK(std::fabs(peak(half.L) / peak(loud.L) - 0.625) < 1e-3);
    bool centred = true;
    for (size_t i = 0; i < loud.L.size(); ++i) centred = centred && loud.L[i] == loud.R[i];
    CHECK(centred);

    const Out left = strikeOne(p, 72, samples(0.2), 1.0f, -1.0f), right = strikeOne(p, 72, samples(0.2), 1.0f, 1.0f);
    const Out mid = strikeOne(p, 72, samples(0.2), 1.0f, 0.5f);
    CHECK(peak(left.R) == 0.0f && peak(right.L) == 0.0f);
    CHECK(std::fabs(peak(left.L) / peak(loud.L) - std::sqrt(2.0)) < 1e-3);
    // Equal power: L^2 + R^2 is 2 (the middle's 1 + 1) wherever the pan is.
    const double power = (rms(mid.L) * rms(mid.L) + rms(mid.R) * rms(mid.R)) / (rms(loud.L) * rms(loud.L));
    std::printf("  pan 0.5: L %.4f, R %.4f of the middle's, power %.4f of it x 2\n", rms(mid.L) / rms(loud.L),
                rms(mid.R) / rms(loud.L), power / 2.0);
    CHECK(std::fabs(power - 2.0) < 1e-3);

    // The send: the dry times spaceSend; a change glides across the next render.
    AirVoices v;
    v.seed(1);
    v.set(p, equal());
    v.strike(72, 1.0f, -0.4f);
    Out o;
    play(v, o, kBlk * 4, 0.3f);
    play(v, o, kBlk, 0.6f);
    play(v, o, kBlk, 0.6f);
    double err = 0.0;
    for (size_t i = 0; i < 4 * kBlk; ++i)
        err = std::max({err, std::fabs(o.sendL[i] - 0.3 * o.L[i]), std::fabs(o.sendR[i] - 0.3 * o.R[i])});
    for (size_t i = 0; i < kBlk; ++i) {
        const double s = 0.3 + 0.3 * static_cast<double>(i + 1) / kBlk;
        const size_t j = 4 * kBlk + i;
        err = std::max({err, std::fabs(o.sendL[j] - s * o.L[j]), std::fabs(o.sendR[j] - s * o.R[j])});
    }
    for (size_t i = 5 * kBlk; i < 6 * kBlk; ++i) err = std::max(err, static_cast<double>(std::fabs(o.sendL[i] - 0.6f * o.L[i])));
    std::printf("  the send against the dry times spaceSend: off by %.2g at most\n", err);
    CHECK(err < 1e-6);
}

// Check 6: a seventh strike takes the quietest voice, which fades out over 2 ms before the new note
// starts in it: six voices still, the quietest note gone, no step over 0.25 anywhere, and around the
// steal no larger a step than the voices make themselves. Then more strikes than voices within one
// fade: the latest wins, nothing sticks.
void testSteal() {
    std::printf("== airvoices: steal\n");
    AirVoices v;
    v.seed(3);
    v.set(patch(af::AS_GLASS, 4.0f, 6000.0f), equal());
    const int notes[6] = {60, 62, 64, 65, 67, 69};
    for (int k = 0; k < 6; ++k) v.strike(notes[k], notes[k] == 64 ? 0.4f : 1.0f, 0.0f);
    Out o;
    play(v, o, samples(0.3));
    CHECK(v.active() == 6);
    auto voiceOf = [&](int note) {
        for (int i = 0; i < AirVoices::kVoices; ++i)
            if (v.voice(i).stage != AirVoices::VS_FREE && v.voice(i).note == note) return i;
        return -1;
    };
    const int quiet = voiceOf(64);
    const size_t strikeAt = o.L.size();
    v.strike(72, 1.0f, 0.0f);
    CHECK(v.active() == 6);
    CHECK(quiet >= 0 && v.voice(quiet).stage == AirVoices::VS_STEAL && v.voice(quiet).next == 72);
    play(v, o, samples(0.001));   // under 2 ms: 64 still fading out
    CHECK(quiet >= 0 && v.voice(quiet).stage == AirVoices::VS_STEAL && v.voice(quiet).note == 64);
    play(v, o, samples(0.05));
    CHECK(v.active() == 6);
    CHECK(voiceOf(64) < 0 && voiceOf(72) == quiet);
    for (int note : {60, 62, 65, 67, 69}) CHECK(voiceOf(note) >= 0);
    // The fade (the new note's own strike, 2 ms on, is check 5's).
    const float steady = maxStep(o.L, strikeAt - at(0.1), strikeAt);
    const float around = maxStep(o.L, strikeAt, strikeAt + 88);
    std::printf("  largest step %.4f (steady %.4f, during the fade %.4f)\n", maxStep(o.L), steady, around);
    CHECK(maxStep(o.L) <= 0.25f && maxStep(o.R) <= 0.25f);
    CHECK(around <= 1.5f * steady);

    // Eight strikes within a fade: every voice is taken, and each waits for its fade; the last
    // strikes replace the notes waiting in the voices nearest their start.
    for (int k = 0; k < 8; ++k) v.strike(80 + k, 1.0f, 0.0f);
    CHECK(v.active() == 6);
    play(v, o, samples(0.01));
    CHECK(v.active() == 6);
    int ringing = 0;
    for (int i = 0; i < AirVoices::kVoices; ++i) ringing += v.voice(i).stage == AirVoices::VS_RING ? 1 : 0;
    CHECK(ringing == 6);
    CHECK(voiceOf(86) >= 0 && voiceOf(87) >= 0);
    CHECK(allFinite(o.L) && allFinite(o.R));
}

// Check 7: a strike that rings out leaves active() == 0 within 1.6 times its longest ring, counted
// from the end of its excitation (the plan's check 7, as amended): Decay (the fundamental's) for every
// sound but Bell, whose hum (ratio 0.5) rings Decay / 0.6. The modes' excitation is the strike
// itself; the Kalimba's is its burst, a period at note 71 and under (30.6 ms at note 24, a third of
// Decay 0.1). The pluck itself stays over -90 dBFS until 1.40 to 1.61 Decays after the strike at
// notes 24 to 26 (Tone 16000; the louder side of a hard pan longest), so bounded from the strike the
// check failed there for the bound's sake, not the voice's. The bound here is a little tighter than
// that: 1.6 Decays plus half the burst. A voice is free only once it has played under -90 dBFS on its
// louder side for 32 samples. Glass and Bar at Decay 0.1 pass that with only 0.1 dB to spare: for the
// modes it holds by construction (their level, the |z| summed, is the most they can play, and it is
// under the floor at both ends of every quiet step), and their modes happen to line up near it.
// Notes 24, 72 and 108 at Decay 0.1, 0.5 and 2 s, Tone 16000 (the loudest), pans 0 and 1. Asleep,
// render() writes nothing.
void testSleep() {
    std::printf("== airvoices: sleep\n");
    double most = 0.0;
    for (int s = 0; s < af::AS_COUNT; ++s)
        for (int note : {24, 72, 108})
            for (float d : {0.1f, 0.5f, 2.0f})
                for (float pan : {0.0f, 1.0f}) {
                    AirVoices v;
                    v.seed(1);
                    v.set(patch(s, d, 16000.0f), equal());
                    v.strike(note, 1.0f, pan);
                    Out o;
                    const int blocks = samples(4.0 * d) / kBlk;
                    for (int b = 0; b < blocks && v.active() > 0; ++b) play(v, o, kBlk);
                    const double t = static_cast<double>(o.L.size()) / af::kRate;
                    const double longest = s == af::AS_BELL ? d / 0.6 : d;
                    const double burst =
                        s == af::AS_KALIMBA ? std::max(88.0, std::ceil(af::kRate / noteHz(note))) / af::kRate : 0.0;
                    // The 32 samples it played last on its louder side (after them it writes nothing).
                    const Buf& loud = pan > 0.0f ? o.R : o.L;
                    size_t end = loud.size();
                    while (end > 32 && loud[end - 1] == 0.0f) --end;
                    const double last = db(peak(loud, end - 32, end));
                    if ((d < 0.2f && pan == 0.0f) || s == af::AS_KALIMBA)
                        std::printf("  %-8s note %3d, Decay %.1f s, pan %.0f: asleep after %.3f s (%.2f of its longest "
                                    "ring%s); its last 32 samples at most %.1f dBFS\n",
                                    af::kAirSoundNames[s], note, d, pan, t, (t - 0.5 * burst) / longest,
                                    burst > 0.0 ? " from half its burst" : "", last);
                    most = std::max(most, (t - 0.5 * burst) / longest);
                    CHECK(v.active() == 0);
                    CHECK(t <= 1.6 * longest + 0.5 * burst);
                    CHECK(last < -90.0);
                    // Asleep: nothing written, the buffers as they were.
                    float L[kBlk], R[kBlk], SL[kBlk], SR[kBlk];
                    std::fill(L, L + kBlk, 0.25f);
                    std::fill(R, R + kBlk, -0.25f);
                    std::fill(SL, SL + kBlk, 0.5f);
                    std::fill(SR, SR + kBlk, -0.5f);
                    v.render(tables(), L, R, SL, SR, 0.7f, kBlk);
                    bool same = true;
                    for (int i = 0; i < kBlk; ++i)
                        same = same && L[i] == 0.25f && R[i] == -0.25f && SL[i] == 0.5f && SR[i] == -0.5f;
                    CHECK(same);
                }
    std::printf("  asleep within %.2f of the longest ring at most\n", most);
}

// A note rings as it was struck: set() changes the next strikes only.
void testRingsAsStruck() {
    std::printf("== airvoices: a note rings as struck\n");
    for (int s = 0; s < af::AS_COUNT; ++s) {
        AirVoices a, b;
        a.seed(5);
        b.seed(5);
        a.set(patch(s, 3.0f, 8000.0f), equal());
        b.set(patch(s, 3.0f, 8000.0f), equal());
        a.strike(67, 0.8f, 0.3f);
        b.strike(67, 0.8f, 0.3f);
        Out oa, ob;
        play(a, oa, samples(0.2));
        play(b, ob, samples(0.2));
        af::HarmonyPatch h = equal();
        h.tuning = af::TU_JUST;
        h.key = 3;
        b.set(patch((s + 2) % af::AS_COUNT, 0.1f, 200.0f), h);
        play(a, oa, samples(0.5));
        play(b, ob, samples(0.5));
        CHECK(oa.L == ob.L && oa.R == ob.R);
        CHECK(b.voice(0).sound == s);
    }
}

// Felt darkens as it fades: Age moves from 0.05 to 0.8 over the decay, through the Felt Piano
// table's life.
void testFeltDarkens() {
    std::printf("== airvoices: Felt darkens\n");
    const Out o = strikeOne(patch(af::AS_FELT, 2.0f), 60, samples(2.0));
    constexpr size_t kFft = size_t{1} << 12;
    const double early = centroidHz(o.L, at(0.02), kFft), late = centroidHz(o.L, at(1.9), kFft);
    std::printf("  centroid %.0f Hz at 20 ms, %.0f Hz at 1.9 s (Age 0.76)\n", early, late);
    CHECK(late < 0.9 * early);
}

// Check 8: stability. Every sound at notes 24 and 108, Decay 20, Tone 16000, six voices struck again
// every 100 ms for 30 s (each strike taking the quietest): finite, peak at most 2. Then one modal
// strike left to ring for 60 s at Decay 20 at each end of the keyboard, where its modes' rotations
// are nearest 1 and furthest round: finite, falling to nothing, and the prime (ratio 1) still
// falling at its own rate 10 s in (30 dB in 10 s, +-10%: float's rounding hasn't moved it). Bowl's
// pair, 0.13 Hz apart at note 24, beats too slowly to read a level there; check 3 has its rate.
void testStability() {
    std::printf("== airvoices: stability\n");
    const float pans[6] = {-0.7f, 0.42f, -0.14f, 0.7f, -0.42f, 0.14f};
    float worst = 0.0f;
    bool finite = true;
    for (int s = 0; s < af::AS_COUNT; ++s)
        for (int note : {24, 108}) {
            AirVoices v;
            v.seed(9);
            v.set(patch(s, 20.0f, 16000.0f), equal());
            float L[kBlk], R[kBlk], SL[kBlk], SR[kBlk];
            float pk = 0.0f;
            int strikes = 0;
            const int every = samples(0.1);
            for (int t = 0; t < samples(30.0); t += kBlk) {
                if (t / every != (t - kBlk) / every || t == 0) v.strike(note, 1.0f, pans[strikes++ % 6]);
                std::fill(L, L + kBlk, 0.0f);
                std::fill(R, R + kBlk, 0.0f);
                std::fill(SL, SL + kBlk, 0.0f);
                std::fill(SR, SR + kBlk, 0.0f);
                v.render(tables(), L, R, SL, SR, 0.5f, kBlk);
                for (int i = 0; i < kBlk; ++i) {
                    finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]) && std::isfinite(SL[i]) && std::isfinite(SR[i]);
                    pk = std::max({pk, std::fabs(L[i]), std::fabs(R[i])});
                }
            }
            std::printf("  %-8s note %3d: %d strikes, peak %.3f, %d voices\n", af::kAirSoundNames[s], note, strikes, pk,
                        v.active());
            worst = std::max(worst, pk);
        }
    CHECK(finite);
    CHECK(worst <= 2.0f);

    for (int s : {af::AS_GLASS, af::AS_BOWL, af::AS_BAR, af::AS_BELL})
        for (int note : {24, 108}) {
            AirVoices v;
            v.seed(1);
            v.set(patch(s, 20.0f, 16000.0f), equal());
            v.strike(note, 1.0f, 0.0f);
            Out o;
            play(v, o, samples(60.0));
            const double first = rms(o.L, 0, at(1.0)), last = rms(o.L, at(59.0), at(60.0));
            constexpr size_t kWin = size_t{1} << 16;
            const double hz = noteHz(note);
            const double drop = db(magnitude(o.L, hz, at(1.0), at(1.0) + kWin)) - db(magnitude(o.L, hz, at(11.0), at(11.0) + kWin));
            std::printf("  %-6s note %3d alone for 60 s: %.1f dB in the first second, %.1f in the last (%s); the prime "
                        "falls %.2f dB from 1 s to 11 s\n",
                        af::kAirSoundNames[s], note, db(first), db(last), v.active() ? "ringing" : "asleep", drop);
            CHECK(allFinite(o.L) && allFinite(o.R));
            CHECK(db(last) < db(first) - 80.0);
            if (s != af::AS_BOWL) CHECK(std::fabs(drop / 30.0 - 1.0) <= 0.1);
        }
}

// Odd input: parameters out of range, NaN and infinities, notes off the keyboard, renders of 0, 1
// and more than 128 samples. Everything stays finite and in range (velocities past 1 count as 1,
// pans past the sides as the sides).
void testOddInput() {
    std::printf("== airvoices: odd input\n");
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const float odd[] = {nan, inf, -inf, -5.0f, 0.0f, 1e9f};
    AirVoices v;
    v.seed(4);
    Out o;
    int k = 0;
    for (int sound : {-3, 0, 2, 4, 5, 99})
        for (float x : odd) {
            af::AirVoicePatch p;
            p.sound = sound;
            p.toneHz = x;
            p.decayS = odd[(k + 1) % 6];
            p.width = odd[(k + 2) % 6];
            af::HarmonyPatch h;
            h.key = k % 3 == 0 ? -7 : 30;
            h.tuning = k % 4 == 0 ? 17 : -2;
            v.set(p, h);
            v.strike(k % 2 ? -100 : 1000, odd[(k + 3) % 6], odd[(k + 4) % 6]);
            v.strike(60 + k % 12, 1.0f, 0.0f);
            play(v, o, 777);
            ++k;
        }
    float L[300] = {}, R[300] = {}, SL[300] = {}, SR[300] = {};
    v.render(tables(), L, R, SL, SR, nan, 0);
    v.render(tables(), L, R, SL, SR, inf, 1);
    v.render(tables(), L, R, SL, SR, -1.0f, 300);
    bool finite = allFinite(o.L) && allFinite(o.R) && allFinite(o.sendL) && allFinite(o.sendR);
    for (int i = 0; i < 300; ++i)
        finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]) && std::isfinite(SL[i]) && std::isfinite(SR[i]);
    std::printf("  %d patches: peak %.3f\n", k, std::max(peak(o.L), peak(o.R)));
    CHECK(finite);
    // Six voices at full, panned hard and ringing long, can pass check 8's 2; twice that is a fault.
    CHECK(std::max(peak(o.L), peak(o.R)) <= 4.0f);
    CHECK(v.active() >= 0 && v.active() <= AirVoices::kVoices);
    for (int i = 0; i < AirVoices::kVoices; ++i) {
        const AirVoices::VoiceView w = v.voice(i);
        CHECK(w.stage == AirVoices::VS_FREE || (w.note >= 24 && w.note <= 108));
        CHECK(w.sound >= 0 && w.sound < af::AS_COUNT);
    }
    CHECK(v.voice(-1).stage == AirVoices::VS_FREE && v.voice(AirVoices::kVoices).stage == AirVoices::VS_FREE);
}

// The same seed and the same calls play the same samples (the Kalimba's noise included); another
// seed, another noise; reset() starts the seed's numbers again.
void testDeterminism() {
    std::printf("== airvoices: determinism\n");
    auto run = [](AirVoices& v) {
        Out o;
        for (int s = 0; s < af::AS_COUNT; ++s) {
            v.set(patch(s, 1.0f, 3000.0f + 2000.0f * s), equal());
            v.strike(50 + 5 * s, 0.9f, 0.1f * s - 0.2f);
            play(v, o, samples(0.15));
        }
        return o;
    };
    AirVoices a, b, c;
    a.seed(7);
    b.seed(7);
    c.seed(8);
    const Out oa = run(a), ob = run(b), oc = run(c);
    CHECK(oa.L == ob.L && oa.R == ob.R && oa.sendL == ob.sendL);
    CHECK(oa.L != oc.L);
    a.reset();
    const Out again = run(a);
    CHECK(again.L == oa.L && again.R == oa.R);
    a.reset();
    CHECK(a.active() == 0);
}

void testNoAllocation() {
    std::printf("== airvoices: no allocation\n");
#if AFT_COUNTS_ALLOCS
    CHECK(hookAllocations());
    tables();
    AirVoices v;
    v.seed(12);
    float L[kBlk] = {}, R[kBlk] = {}, SL[kBlk] = {}, SR[kBlk] = {};
    countAllocations();
    for (int s = 0; s < af::AS_COUNT; ++s) {
        v.set(patch(s, 2.0f, 5000.0f), equal());
        for (int k = 0; k < 8; ++k) v.strike(40 + 7 * k, 0.7f, 0.1f * k - 0.4f);
        for (int i = 0; i < 100; ++i) v.render(tables(), L, R, SL, SR, 0.5f, i % 2 ? kBlk : 77);
    }
    v.reset();
    const int allocs = allocationsCounted();
    std::printf("  %d allocations\n", allocs);
    CHECK(allocs == 0);
#else
    std::printf("  (counted under ASan: make test)\n");
#endif
}

} // namespace

void airvoicesTests() {
    testPitch();
    testPluckTuning();
    testPluckPoles();
    testInharmonic();
    testDecay();
    testTone();
    testNoClick();
    testRise();
    testLevels();
    testPluckLevels();
    testBurstSpread();
    testPluckDc();
    testVelocityPanSend();
    testSteal();
    testSleep();
    testRingsAsStruck();
    testFeltDarkens();
    testStability();
    testOddInput();
    testDeterminism();
    testNoAllocation();
}

} // namespace aft
