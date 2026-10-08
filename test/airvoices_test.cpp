// Air's voices (dsp/airvoices.h) on their own: each sound's pitch, Glass's inharmonic second mode,
// the decay against Decay, Tone, no click at a strike, the level each sound is scaled to, velocity,
// pans and the send, stealing, sleeping, a note ringing as struck, Felt darkening as it fades,
// stability at the ends of the keyboard, odd input, determinism, and nothing allocating. Needs no
// plugin: make test-module M=airvoices runs it on its own.
#include "check.h"
#include "signal.h"
#include "../dsp/airvoices.h"
#include "../dsp/lifetime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
// The sanitizer runtime's (sanitizer/allocator_interface.h, which GCC doesn't install): hooks its
// allocator calls on every allocation and free.
extern "C" int __sanitizer_install_malloc_and_free_hooks(void (*malloc_hook)(const volatile void*, size_t),
                                                         void (*free_hook)(const volatile void*));
#define AIR_COUNTS_ALLOCS 1
#endif

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
}

// Check 1 for the Kalimba across its patch: Decay 0.33, 0.5, 4 and 20 s times Tone 200, 6000 and
// 16000, over the keyboard and closely at its top, within the plan's 2 cents. Decay and Tone move the
// loop's low-pass, and with it what the read's fraction makes up: when the read's whole samples were
// chosen by an iteration that could swap them back and forth, notes 105 and 108 were 6 and 9.5 cents
// off at Decay 0.33 and 0.5, at every Tone; tuned on the unit circle rather than at the decaying
// pole, notes 45 to 47 were 2.0 to 2.15 cents flat at Decay 0.33 and Tone 200. Each strike is
// measured over about half its Decay, from 2^13 samples at Decay 0.33 to 2^16 at 4 and 20.
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
                if (std::fabs(c) > 2.0)
                    std::printf("    note %d at Decay %.2f s, Tone %.0f: %+.2f cents\n", note, decay, tone, c);
            }
            std::printf("  Decay %5.2f s, Tone %5.0f: within %.3f cents (the most at note %d)\n", decay, tone, most, mostAt);
            worst = std::max(worst, most);
        }
    CHECK(worst <= 2.0);
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

// Check 7: a strike that rings out leaves active() == 0 within 1.6 times its longest ring: Decay
// (the fundamental's) for every sound but Bell, whose hum (ratio 0.5) rings Decay / 0.6. Asleep,
// render() writes nothing.
void testSleep() {
    std::printf("== airvoices: sleep\n");
    for (int s = 0; s < af::AS_COUNT; ++s)
        for (float d : {0.5f, 2.0f}) {
            AirVoices v;
            v.seed(1);
            v.set(patch(s, d, 6000.0f), equal());
            v.strike(72, 1.0f, 0.0f);
            float L[kBlk], R[kBlk], SL[kBlk], SR[kBlk];
            int blocks = 0;
            const int most = samples(4.0 * d) / kBlk;
            while (v.active() > 0 && blocks < most) {
                std::fill(L, L + kBlk, 0.0f);
                std::fill(R, R + kBlk, 0.0f);
                v.render(tables(), L, R, SL, SR, 0.0f, kBlk);
                ++blocks;
            }
            const double t = static_cast<double>(blocks * kBlk) / af::kRate;
            const double longest = s == af::AS_BELL ? d / 0.6 : d;
            std::printf("  %-8s Decay %.1f s: asleep after %.3f s (%.2f of its longest ring)\n", af::kAirSoundNames[s], d,
                        t, t / longest);
            CHECK(v.active() == 0);
            CHECK(t <= 1.6 * longest);
            // Asleep: nothing written, the buffers as they were.
            std::fill(L, L + kBlk, 0.25f);
            std::fill(R, R + kBlk, -0.25f);
            std::fill(SL, SL + kBlk, 0.5f);
            std::fill(SR, SR + kBlk, -0.5f);
            v.render(tables(), L, R, SL, SR, 0.7f, kBlk);
            bool same = true;
            for (int i = 0; i < kBlk; ++i) same = same && L[i] == 0.25f && R[i] == -0.25f && SL[i] == 0.5f && SR[i] == -0.5f;
            CHECK(same);
        }
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

#if AIR_COUNTS_ALLOCS
thread_local bool t_counting = false;
int g_allocs = 0;
void onMalloc(const volatile void*, size_t) {
    if (t_counting) ++g_allocs;
}
void onFree(const volatile void*) {}
#endif

void testNoAllocation() {
    std::printf("== airvoices: no allocation\n");
#if AIR_COUNTS_ALLOCS
    static const bool hooked = __sanitizer_install_malloc_and_free_hooks(onMalloc, onFree) != 0;
    CHECK(hooked);
    tables();
    AirVoices v;
    v.seed(12);
    float L[kBlk] = {}, R[kBlk] = {}, SL[kBlk] = {}, SR[kBlk] = {};
    t_counting = true;
    g_allocs = 0;
    for (int s = 0; s < af::AS_COUNT; ++s) {
        v.set(patch(s, 2.0f, 5000.0f), equal());
        for (int k = 0; k < 8; ++k) v.strike(40 + 7 * k, 0.7f, 0.1f * k - 0.4f);
        for (int i = 0; i < 100; ++i) v.render(tables(), L, R, SL, SR, 0.5f, i % 2 ? kBlk : 77);
    }
    v.reset();
    t_counting = false;
    std::printf("  %d allocations\n", g_allocs);
    CHECK(g_allocs == 0);
#else
    std::printf("  (counted under ASan: make test)\n");
#endif
}

} // namespace

void airVoicesTests() {
    testPitch();
    testPluckTuning();
    testInharmonic();
    testDecay();
    testTone();
    testNoClick();
    testLevels();
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

// make test-module M=airvoices names the suite by its file.
void airvoicesTests() { airVoicesTests(); }

} // namespace aft
