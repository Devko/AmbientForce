// From PolyForce dsp/wavetable.cpp (db7cba3), namespace pf -> af; the FFT, appendFrame and addFrames (as FrameBuilder::add, without the peak normalising) and newTableId only.
#include "wavetable.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <limits>

namespace af {
namespace {

using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

// In-place iterative radix-2 FFT for one size, load time only. Twiddles and the bit-reversal
// permutation are tabled once per plan (a recurrence drifts, and sin/cos per butterfly is
// slow on the Force). The inverse includes the 1/N. Butterflies multiply by hand:
// std::complex's operator* calls __muldc3 (NaN/inf fix-ups) unless -ffast-math.
class Fft {
public:
    explicit Fft(size_t n) : n_(n), w_(n / 2), rev_(n) {
        for (size_t k = 0; k < n / 2; ++k) {
            const double a = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
            w_[k] = cd(std::cos(a), std::sin(a));
        }
        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            rev_[i] = static_cast<uint32_t>(j);
        }
    }

    void run(std::vector<cd>& a, bool inverse) const {
        for (size_t i = 1; i < n_; ++i)
            if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t step = n_ / len;
            for (size_t i = 0; i < n_; i += len) {
                for (size_t k = 0; k < len / 2; ++k) {
                    const cd w = w_[k * step];
                    const double wr = w.real(), wi = inverse ? -w.imag() : w.imag();
                    const cd u = a[i + k];
                    const cd x = a[i + k + len / 2];
                    const cd v(x.real() * wr - x.imag() * wi, x.real() * wi + x.imag() * wr);
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                }
            }
        }
        if (inverse)
            for (auto& x : a) x /= static_cast<double>(n_);
    }

private:
    size_t n_;
    std::vector<cd> w_;
    std::vector<uint32_t> rev_;
};

// Appends one frame (every level, as floats) as 16-bit samples with its own scale: the largest
// value over all levels (a band-limited level can overshoot level 0) maps to 32767. A frame with
// a non-finite value gets a NaN scale, for the caller to refuse.
void appendFrame(Wavetable& t, const float* src) {
    float peak = 0.0f;
    bool finite = true;
    for (int s = 0; s < kFrameStride; ++s) {
        finite = finite && std::isfinite(src[s]);
        peak = std::max(peak, std::fabs(src[s]));
    }
    const size_t base = t.data.size();
    t.data.resize(base + kFrameStride, 0);
    if (!finite) {
        t.scale.push_back(std::numeric_limits<float>::quiet_NaN());
        return;
    }
    t.scale.push_back(peak / 32767.0f);
    if (peak <= 0.0f) return;   // silent: zeros, scale 0
    const float inv = 32767.0f / peak;
    for (int s = 0; s < kFrameStride; ++s)
        t.data[base + static_cast<size_t>(s)] = static_cast<int16_t>(std::clamp(std::lrint(src[s] * inv), -32767L, 32767L));
}

} // namespace

// The inverse plans of every mip length, shared by all frames of one build.
struct FrameBuilder::Plans {
    Fft p2048{2048}, p1024{1024}, p512{512}, p256{256};
    const Fft& forLength(int n) const {
        return n == 2048 ? p2048 : n == 1024 ? p1024 : n == 512 ? p512 : p256;
    }
};

FrameBuilder::FrameBuilder() : plans_(new Plans) {}
FrameBuilder::~FrameBuilder() = default;

// Up to two frames (b may be null) at every mip level, one inverse FFT per level for both.
void FrameBuilder::add(Wavetable& t, const Spectrum& sa, const Spectrum* sb) const {
    const int count = sb ? 2 : 1;
    std::vector<float> f32(static_cast<size_t>(count) * kFrameStride);   // as floats, then appendFrame
    std::vector<cd> x(static_cast<size_t>(kTableSize));
    for (int k = 0; k < kMipLevels; ++k) {
        const int n = mipLength(k);
        const int top = std::min(kMaxHarmonic - 1, kMaxHarmonic >> k);
        x.assign(static_cast<size_t>(n), cd(0.0, 0.0));
        const double half = n / 2.0;
        for (int h = 1; h <= top; ++h) {
            // X = Ca + i * Cb, where Ca/Cb are the Hermitian spectra of the two real frames.
            const size_t hh = static_cast<size_t>(h);
            const cd ca(sa.a[hh] * half, -sa.b[hh] * half);
            const cd cb = sb ? cd(sb->a[hh] * half, -sb->b[hh] * half) : cd(0.0, 0.0);
            const cd i(0.0, 1.0);
            x[hh] = ca + i * cb;
            x[static_cast<size_t>(n - h)] = std::conj(ca) + i * std::conj(cb);
        }
        plans_->forLength(n).run(x, true);
        for (int f = 0; f < count; ++f) {
            float* dst = &f32[static_cast<size_t>(f) * kFrameStride + static_cast<size_t>(mipOffset(k))];
            for (int s = 0; s < n; ++s) {
                const cd v = x[static_cast<size_t>(s)];
                dst[s] = static_cast<float>(f ? v.imag() : v.real());
            }
            dst[n] = dst[0];
        }
    }
    for (int f = 0; f < count; ++f) appendFrame(t, &f32[static_cast<size_t>(f) * kFrameStride]);
    t.frames += count;
}

uint32_t newTableId() {
    static std::atomic<uint32_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace af
