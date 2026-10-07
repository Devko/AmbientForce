// From EffectForce test/signal.h (4160e87), eft -> aft; the FFT, spectra and tables shared by the
// lifetime suites added.
#pragma once
// Signals and measurements for the module tests: generators, levels, a single-frequency magnitude
// (Goertzel), and running a module over a buffer in control chunks the way Space runs its Reverb.
// For the lifetime suites (tables, lifeosc, ground): the tests' own FFT, a spectrum, a pitch by
// zero crossings, and the table library built once per run.
#include "check.h"
#include "../dsp/common.h"
#include "../dsp/lifetime.h"
#include "../dsp/wavetable.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace aft {

using Buf = std::vector<float>;
constexpr double kPi = 3.14159265358979323846;

inline Buf sine(double hz, int n, float amp = 0.5f, double phase = 0.0) {
    Buf x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * kPi * hz * i / af::kRate + phase));
    return x;
}
inline Buf whiteNoise(int n, float amp = 0.5f, uint32_t seed = 1) {
    Buf x(static_cast<size_t>(n));
    for (float& s : x) {
        seed = seed * 1664525u + 1013904223u;
        s = amp * static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f;
    }
    return x;
}
inline Buf impulseAt(int n, int at, float v = 1.0f) {
    Buf x(static_cast<size_t>(n), 0.0f);
    x[static_cast<size_t>(at)] = v;
    return x;
}

inline double rms(const Buf& x, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    double s = 0.0;
    for (size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
    return to > from ? std::sqrt(s / static_cast<double>(to - from)) : 0.0;
}
inline double db(double v) { return v > 1e-12 ? 20.0 * std::log10(v) : -240.0; }
inline float peak(const Buf& x, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    float m = 0.0f;
    for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(x[i]));
    return m;
}
inline bool allFinite(const Buf& x) {
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}
// The largest jump between neighbouring samples: a click detector for smooth test signals.
inline float maxStep(const Buf& x, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    float m = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < to; ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}

// Amplitude of the `hz` component of x[from..to) (Goertzel, Hann-windowed), as a sine's peak.
inline double magnitude(const Buf& x, double hz, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    const size_t n = to - from;
    const double w = 2.0 * kPi * hz / af::kRate, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
        wsum += win;
        const double s0 = x[from + i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}
// The gain in dB a module applies to a sine at hz (after `settle` samples of settling).
template <class M>
double gainAt(M& m, const typename M::Params& p, double hz, int settle = 8192, int len = 16384);

// Runs a module over L / R in chunks of `chunk` samples (set() before each), as Space does.
// The transport advances while playing.
template <class M>
void run(M& m, const typename M::Params& p, Buf& L, Buf& R, af::Transport t = {}, int chunk = af::kChunk) {
    for (size_t pos = 0; pos < L.size(); pos += static_cast<size_t>(chunk)) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), L.size() - pos));
        m.set(p, t);
        m.process(&L[pos], &R[pos], n);
        if (t.playing) t.beats += n / static_cast<double>(af::kRate) * t.bpm / 60.0;
    }
}

template <class M>
double gainAt(M& m, const typename M::Params& p, double hz, int settle, int len) {
    m.reset();
    Buf L = sine(hz, settle + len, 0.25f), R = L;
    const Buf in = L;
    run(m, p, L, R);
    return db(magnitude(L, hz, static_cast<size_t>(settle)) / magnitude(in, hz, static_cast<size_t>(settle)));
}

// --- spectra, pitches and tables (the lifetime suites) ----------------------------------------

using cd = std::complex<double>;

// The twiddles for a power-of-two n, worked out once per size (main thread only).
inline const std::vector<cd>& twiddles(size_t n) {
    static std::vector<cd> w[32];
    size_t k = 0;
    while ((size_t{1} << k) < n) ++k;
    if (w[k].size() != n / 2) {
        w[k].resize(n / 2);
        for (size_t i = 0; i < n / 2; ++i) w[k][i] = std::polar(1.0, -2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
    }
    return w[k];
}

// The tests' own FFT (plain radix-2, forward), so a slip in the table builder's can't hide itself.
inline void fft(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    const std::vector<cd>& w = twiddles(n);
    for (size_t len = 2; len <= n; len <<= 1)
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w[k * (n / len)];
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
}

// A spectrum as each bin's sine amplitude (a sine of peak A on a bin reads A). Blackman-Harris:
// its sidelobes are under -92 dB, so a strong partial's leakage can't fill a -90 dB measurement.
struct Spectrum {
    std::vector<double> amp;
    double binHz = 0.0;
    // n samples of x from `from`; n a power of two (0: all of x, whose size must be one).
    explicit Spectrum(const Buf& x, size_t from = 0, size_t n = 0) {
        if (n == 0) n = x.size() - from;
        std::vector<cd> a(n);
        double wsum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double p = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
            const double w = 0.35875 - 0.48829 * std::cos(p) + 0.14128 * std::cos(2.0 * p) - 0.01168 * std::cos(3.0 * p);
            a[i] = x[from + i] * w;
            wsum += w;
        }
        fft(a);
        amp.resize(n / 2);
        for (size_t k = 0; k < n / 2; ++k) amp[k] = 2.0 * std::abs(a[k]) / wsum;
        binHz = static_cast<double>(af::kRate) / static_cast<double>(n);
    }
    // The strongest bin within `bins` of hz: a partial's amplitude (up to 0.8 dB low between bins).
    double at(double hz, int bins = 3) const {
        const long c = std::lround(hz / binHz);
        double m = 0.0;
        for (long k = std::max(1L, c - bins); k <= std::min(static_cast<long>(amp.size()) - 1, c + bins); ++k)
            m = std::max(m, amp[static_cast<size_t>(k)]);
        return m;
    }
};

// Frequency by rising zero crossings (interpolated), over x[from..to): for a pure tone.
inline double zeroCrossHz(const Buf& x, size_t from, size_t to) {
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

// The table library, each table built the first time a suite asks for it and then shared by all
// of them (main thread only): one build per run.
inline const af::Wavetable& testTable(int id) {
    static af::Wavetable t[af::TB_COUNT];
    static bool built[af::TB_COUNT] = {};
    if (!built[id]) {
        CHECK(af::buildTable(id, t[id]));
        built[id] = true;
    }
    return t[id];
}

} // namespace aft
