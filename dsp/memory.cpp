// Memory: see memory.h. The recording is EffectForce's Grain recording (dsp/grain.cpp, d94bde0):
// level 0 as it comes, each pair of its frames through the halfband decimator into level 1 and
// each pair of those into level 2, the guard written as the ring's first frames are; the seam's
// fade is Grain's hold fade (freeze(): 220 frames at level 0, window1's sin^2). Here the samples
// are 16 bit, recorded 128 frames at a time (so the decimators always take whole pairs), the
// decimator is halfband.h's FrameDecimator, two rings take turns, and nothing else of Grain's
// came along.
#include "memory.h"

#include "simd.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace af {

namespace {

constexpr int kGuard = GrainSource::kGuard;
constexpr float kIn = 2.0f;                       // the input that reaches 16-bit full scale (6 dB of headroom)
constexpr float kToInt = 32768.0f / kIn;          // staged samples are the input times this: a power of two, exact
constexpr int64_t kGapFrames = static_cast<int64_t>(Memory::kRememberGapS * 44100.0f);
constexpr int kMinFrames = static_cast<int>(Memory::kMinS * 44100.0f);
static_assert(Memory::kFrames == static_cast<int>(Memory::kSeconds * 44100.0f) && Memory::kFrames % 4 == 0,
              "16 s, and the levels halve twice");
static_assert((Memory::kSeamFade >> 2) * 4 == Memory::kSeamFade, "the fade halves twice too");
static_assert(Memory::kSeamFade <= kGuard && kMinFrames > 2 * Memory::kSeamFade, "a seam's fades never meet");

// Four staged frames (x: L R L R ..., in 16-bit units) as 16 bit into q, all four or the first
// `count`: rounded to nearest, saturated. The rounding: floor(y + 32768.5) - 32768, the sum in
// float, so the same on every machine; the floor is a conversion of a number never negative but in
// saturation (NEON's conversion truncates and saturates by itself; elsewhere the clamp comes first,
// which gives the same). On NEON the frames load apart into L and R and store together again.
AF_INLINE void put4(const float* x, int16_t* q, int count) {
#if AF_NEON
    const float32x4x2_t f = vld2q_f32(x);
    const f4 off = vdupq_n_f32(32768.5f);
    const uint16x4_t flip = vdup_n_u16(0x8000);
    int16x4x2_t v;
    v.val[0] = vreinterpret_s16_u16(veor_u16(vqmovn_u32(vcvtq_u32_f32(vaddq_f32(f.val[0], off))), flip));
    v.val[1] = vreinterpret_s16_u16(veor_u16(vqmovn_u32(vcvtq_u32_f32(vaddq_f32(f.val[1], off))), flip));
    if (count == 4) {
        vst2_s16(q, v);
    } else {
        vst2_lane_s16(q, v, 0);
        if (count > 1) vst2_lane_s16(q + 2, v, 1);
        if (count > 2) vst2_lane_s16(q + 4, v, 2);
    }
#else
    for (int i = 0; i < 2 * count; ++i) {
        const float z = x[i] + 32768.5f;
        q[i] = static_cast<int16_t>(static_cast<int32_t>(std::min(std::max(z, 0.0f), 65535.0f)) - 32768);
    }
#endif
}

// Four frames of L and R staged at x, interleaved, in 16-bit units: each sample not finite made 0,
// times kToInt, clamped to full scale (+-kIn before the scaling). The decimators take them as they
// are (a power of two changes none of their arithmetic but the exponents).
AF_INLINE void stage4(const float* l4, const float* r4, float* x) {
#if AF_NEON
    const f4 lo = vdupq_n_f32(-kIn * kToInt), hi = vdupq_n_f32(kIn * kToInt), scale = vdupq_n_f32(kToInt);
    f4 l = vmulq_f32(vld1q_f32(l4), scale), r = vmulq_f32(vld1q_f32(r4), scale);
    l = vreinterpretq_f32_u32(vandq_u32(vceqq_f32(l, l), vreinterpretq_u32_f32(l)));   // NaN -> +0
    r = vreinterpretq_f32_u32(vandq_u32(vceqq_f32(r, r), vreinterpretq_u32_f32(r)));
    const float32x4x2_t z = vzipq_f32(vminq_f32(vmaxq_f32(l, lo), hi), vminq_f32(vmaxq_f32(r, lo), hi));
    vst1q_f32(x, z.val[0]);
    vst1q_f32(x + 4, z.val[1]);
#else
    for (int i = 0; i < 4; ++i) {
        const float l = l4[i] * kToInt, r = r4[i] * kToInt;
        x[2 * i] = std::min(std::max(l == l ? l : 0.0f, -kIn * kToInt), kIn * kToInt);
        x[2 * i + 1] = std::min(std::max(r == r ? r : 0.0f, -kIn * kToInt), kIn * kToInt);
    }
#endif
}

// n frames staged at x, four at a time. The last few are copied out and padded with zeros first,
// so nothing past L + n is read (the frames staged past n are overwritten before they count).
void stage(const float* L, const float* R, int n, float* x) {
    int i = 0;
    for (; i + 4 <= n; i += 4) stage4(L + i, R + i, x + 2 * i);
    if (i < n) {
        alignas(16) float l4[4] = {}, r4[4] = {};
        for (int j = 0; i + j < n; ++j) {
            l4[j] = L[i + j];
            r4[j] = R[i + j];
        }
        stage4(l4, r4, x + 2 * i);
    }
}

// `count` frames of x (L R floats) as 16 bit into a level's ring (b, `frames` long) from its frame
// w, four at a time (a last group of fewer stores only those: x has room for the four). Wraps at
// the ring's end; what lands in its first kGuard frames lands past its end too. Returns the next
// frame.
int putLevel(int16_t* b, int frames, int w, const float* x, int count) {
    for (int i = 0; i < count;) {
        const int run = std::min(count - i, frames - w);
        int16_t* const q = b + 2 * w;
        const float* f = x + 2 * i;
        int16_t* d = q;
        for (int g = run >> 2; g > 0; --g, f += 8, d += 8) put4(f, d, 4);
        if (run & 3) put4(f, d, run & 3);
        if (w < kGuard) std::memcpy(b + 2 * (frames + w), q, sizeof(int16_t) * 2 * static_cast<size_t>(std::min(run, kGuard - w)));
        i += run;
        w += run;
        if (w == frames) w = 0;
    }
    return w;
}

// A frame (L R) times g (0..1), rounded as put4 rounds.
void scaleFrame(int16_t* f, float g) {
    for (int c = 0; c < 2; ++c) {
        const float z = static_cast<float>(f[c]) * g + 32768.5f;
        f[c] = static_cast<int16_t>(static_cast<int32_t>(z) - 32768);
    }
}

} // namespace

Memory::Memory() : since_(kGapFrames) {
    for (Ring& r : ring_) {
        for (int k = 0; k < GrainSource::kLevels; ++k) {
            r.level[k].assign(static_cast<size_t>(2 * ((kFrames >> k) + kGuard)), 0);
            r.src.level[k] = r.level[k].data();
        }
        r.src.frames = 0;
        r.src.origin = 0;
        r.src.gain = kIn / 32768.0f;
    }
    // window1 (EffectForce): sin^2 of a quarter turn, 0 at the seam's own frame.
    for (int k = 0; k < GrainSource::kLevels; ++k) {
        const int n = kSeamFade >> k;
        for (int i = 0; i < n; ++i) {
            const double s = std::sin(0.5 * 3.14159265358979323846 * i / n);
            fade_[k][i] = static_cast<float>(s * s);
        }
    }
}

void Memory::restart() {
    staged_ = 0;
    written_ = 0;
    for (int& w : w_) w = 0;
    down1_.reset();
    down2_.reset();
}

void Memory::reset() { restart(); }

void Memory::write(const float* L, const float* R, int n) {
    while (n > 0) {
        const int m = std::min(n, kBlock - staged_);
        stage(L, R, m, x_ + 2 * staged_);
        staged_ += m;
        written_ = std::min(written_ + m, kFrames);
        since_ = std::min(since_ + m, kGapFrames);
        if (staged_ == kBlock) flush();
        L += m;
        R += m;
        n -= m;
    }
}

// The frames staged into the ring recording: level 0, then the pairs of its frames into level 1
// and theirs into level 2 (in x_, each written behind what is read). Every flush but the last
// before restart() (a Remember's) takes kBlock frames, so the decimators always have whole pairs.
void Memory::flush() {
    Ring& r = ring_[rec_];
    const int m0 = staged_, m1 = m0 >> 1, m2 = m1 >> 1;
    w_[0] = putLevel(r.level[0].data(), kFrames, w_[0], x_, m0);
    down1_.process(x_, m1, x_);
    w_[1] = putLevel(r.level[1].data(), kFrames >> 1, w_[1], x_, m1);
    down2_.process(x_, m2, x_);
    w_[2] = putLevel(r.level[2].data(), kFrames >> 2, w_[2], x_, m2);
    staged_ = 0;
}

bool Memory::remember() {
    if (since_ < kGapFrames || written_ < kMinFrames) return false;
    int idle = 0;
    if (!gate_.compare_exchange_strong(idle, -1, std::memory_order_acquire, std::memory_order_relaxed)) return false;
    flush();   // (only now: a flush of less than kBlock is the last before restart())
    Ring& r = ring_[rec_];
    const bool full = written_ >= kFrames;
    const int frames = full ? kFrames : written_ / 4 * 4;
    for (int k = 0; k < GrainSource::kLevels; ++k) {
        // The seam: the oldest frame (full: the next one recording would have taken; not full:
        // frame 0) against the one before it, the newest.
        const int len = frames >> k, seam = full ? w_[k] : 0, n = kSeamFade >> k;
        int16_t* const b = r.level[k].data();
        for (int i = 0; i < n; ++i) {
            const int after = seam + i >= len ? seam + i - len : seam + i;
            const int before = seam - 1 - i < 0 ? seam - 1 - i + len : seam - 1 - i;
            scaleFrame(b + 2 * after, fade_[k][i]);
            scaleFrame(b + 2 * before, fade_[k][i]);
        }
        std::memcpy(b + 2 * len, b, sizeof(int16_t) * 2 * kGuard);   // the guard: its first frames again
    }
    r.src.frames = frames;
    r.src.origin = full ? w_[0] : 0;
    rem_.store(rec_, std::memory_order_relaxed);
    rec_ ^= 1;
    ring_[rec_].src.frames = 0;   // recorded over from here: not a source
    restart();
    since_ = 0;
    gen_.fetch_add(1, std::memory_order_relaxed);
    gate_.store(0, std::memory_order_release);
    return true;
}

const GrainSource* Memory::remembered() const {
    const int r = rem_.load(std::memory_order_acquire);
    return r < 0 ? nullptr : &ring_[r].src;
}

float Memory::fill() const { return static_cast<float>(written_) / static_cast<float>(kFrames); }

uint32_t Memory::generation() const { return gen_.load(std::memory_order_acquire); }

bool Memory::pin() {
    for (;;) {
        int g = gate_.load(std::memory_order_acquire);
        if (g < 0) {   // a Remember under way: microseconds
            std::this_thread::yield();
            continue;
        }
        if (gate_.compare_exchange_weak(g, g + 1, std::memory_order_acquire, std::memory_order_relaxed)) break;
    }
    if (rem_.load(std::memory_order_acquire) < 0) {
        unpin();
        return false;
    }
    return true;
}

void Memory::unpin() {
    int g = gate_.load(std::memory_order_relaxed);
    while (g > 0 && !gate_.compare_exchange_weak(g, g - 1, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

} // namespace af
