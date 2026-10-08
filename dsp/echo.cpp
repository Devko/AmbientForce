#include "echo.h"
#include "simd.h"

#include <algorithm>
#include <cstring>

namespace af {

namespace {

constexpr float kQuiet = 1e-6f;            // -120 dBFS: under it, nothing is coming in or out
constexpr uint32_t kQuietMax = 1u << 30;   // the quiet count stops here (6.8 hours, far past any reach)

// |x|, and 0 for a NaN or an infinity (by the bits: an exponent of all ones), as the Delay takes
// its input: four at a time.
AF_INLINE f4 absFinite4(f4 x) {
#if AF_NEON
    const uint32x4_t bits = vandq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(0x7fffffffu));
    return vreinterpretq_f32_u32(vandq_u32(bits, vcltq_u32(bits, vdupq_n_u32(0x7f800000u))));
#else
    i4 bits;
    std::memcpy(&bits, &x, sizeof bits);
    bits &= 0x7fffffff;
    bits &= bits < 0x7f800000;   // all ones where finite
    f4 r;
    std::memcpy(&r, &bits, sizeof r);
    return r;
#endif
}

AF_INLINE f4 abs4(f4 x) {
#if AF_NEON
    return vabsq_f32(x);
#else
    return max4(x, -x);
#endif
}

// The louder side's peak over m samples (with Finite, of the finite ones: the send).
template <bool Finite>
float peakOf(const float* l, const float* r, int m) {
    f4 top = splat(0.0f);
    int k = 0;
    for (; k + 4 <= m; k += 4) {
        const f4 a = load4(l + k), b = load4(r + k);
        top = max4(top, Finite ? max4(absFinite4(a), absFinite4(b)) : max4(abs4(a), abs4(b)));
    }
    float t[4];
    store4(t, top);
    float p = std::max(std::max(t[0], t[1]), std::max(t[2], t[3]));
    for (; k < m; ++k) {
        const float a = Finite ? std::fabs(sanitize(l[k])) : std::fabs(l[k]), b = Finite ? std::fabs(sanitize(r[k])) : std::fabs(r[k]);
        p = std::max(p, std::max(a, b));
    }
    return p;
}

} // namespace

// Wet only before any set() too: Delay::Params' own mix is EffectForce's 0.3.
Echo::Echo() : dp_(initEcho().delay) {
    dp_.mix = 1.0f;
    reset();
}

// Silent from here: the Delay starts afresh, and its next set() jumps to what it is given (the
// set() first, so silent() asks how far it reaches with what it was last given).
void Echo::reset() {
    delay_.set(dp_, t_);
    delay_.reset();
    jump_ = true;
    held_ = asleep_ = false;
    quiet_ = kQuietMax;
}

// The Delay takes its targets here, once: its chunks then glide to them as they would with a
// set() before each (set() only moves targets, and the Delay reads nothing of the transport but
// the tempo).
void Echo::set(const Params& p, const Transport& t) {
    dp_ = p.delay;
    dp_.mix = 1.0f;
    t_ = t;
    delay_.set(dp_, t_);
    jump_ = false;
}

void Echo::process(const float* sendL, const float* sendR, float* outL, float* outR, int n) {
    // The last call ended silent(), and something had been through the Delay since it last started
    // afresh: its lines still hold that, at ages a longer time (set while the engine skipped this)
    // would read. It starts afresh now, once, and jumps to what it was last given (echo.h).
    if (asleep_) {
        delay_.reset();
        held_ = asleep_ = false;
        jump_ = true;
    }
    if (jump_) {   // reset() and no set() since, or a fresh start: the Delay jumps to its targets
        delay_.set(dp_, t_);
        jump_ = false;
    }
    for (int i = 0; i < n; i += kChunk) {
        const int m = std::min(kChunk, n - i);

        // The send's level, before the return is written (it may be written over the send).
        const bool in = peakOf<true>(sendL + i, sendR + i, m) > kQuiet;

        // The wet, a chunk at a time. Its level before the duck says whether a repeat is left:
        // while nothing comes in, the duck's gain only rises (its envelope falls), so the lower of
        // the chunk's first and last gains is the least it had. While something comes in it isn't
        // measured: such a chunk starts the quiet count again whatever the wet is, so silent() is
        // what it would be if it were. The Delay's output is always finite.
        if (outL != sendL) std::memmove(outL + i, sendL + i, sizeof(float) * static_cast<size_t>(m));
        if (outR != sendR) std::memmove(outR + i, sendR + i, sizeof(float) * static_cast<size_t>(m));
        const float duckWas = delay_.duckGain();
        delay_.process(outL + i, outR + i, m);
        const bool sound = in || peakOf<false>(outL + i, outR + i, m) > kQuiet * std::min(duckWas, delay_.duckGain());

        quiet_ = sound ? 0 : std::min(quiet_ + static_cast<uint32_t>(m), kQuietMax);
        held_ = held_ || sound;
    }
    asleep_ = held_ && silent();
}

bool Echo::silent() const { return quiet_ >= static_cast<uint32_t>(delay_.reachSamples()); }

} // namespace af
