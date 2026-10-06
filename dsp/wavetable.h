// From PolyForce dsp/wavetable.h (db7cba3), namespace pf -> af; the layout, mipFor, newTableId and the frame builder from wavetable.cpp only (no WAV loader, no built-in tables).
#pragma once
// Band-limited wavetables. A table is a stack of single-cycle frames (the "position" axis);
// every frame is stored as kMipLevels pre-filtered copies, level k keeping harmonics
// 1..(1024 >> k). An oscillator picks the level whose top harmonic stays clear of
// aliasing for the pitch it plays, so the inner loop is a plain lookup with no filtering.
//
// Level k is stored at its own length, 1 << kMipBits[k] samples (+1 guard sample): linear
// interpolation needs ~8 samples per cycle of the top harmonic, so the high levels (few
// harmonics) are short. 9,227 samples per frame instead of 11 x 2,049 = 22,539.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace af {

constexpr int kTableBits   = 11;
constexpr int kTableSize   = 1 << kTableBits;   // samples of level 0 (2048)
constexpr int kMaxHarmonic = kTableSize / 2;    // 1024
constexpr int kMipLevels   = 11;                // 1024, 512, ... 1 harmonics

// Samples of level k = 1 << kMipBits[k] = clamp(8 * (1024 >> k), 256, 2048).
constexpr int kMipBits[kMipLevels] = {11, 11, 11, 10, 9, 8, 8, 8, 8, 8, 8};

constexpr int mipLength(int k) { return 1 << kMipBits[k]; }
constexpr int mipOffset(int k) { return k == 0 ? 0 : mipOffset(k - 1) + mipLength(k - 1) + 1; }
constexpr int kFrameStride = mipOffset(kMipLevels - 1) + mipLength(kMipLevels - 1) + 1;
static_assert(kFrameStride == 9227, "mip layout changed: update the comment above");

// Samples are 16-bit with one scale per frame (value = sample * scale[frame]): half the memory of
// floats (4.5 MB for 256 frames), the rounding ~96 dB under each frame's own peak, so a quiet
// frame keeps its resolution. The scale is only how a frame is stored: sample * scale is the
// frame's value, whatever level the table's maker gave it.
struct Wavetable {
    std::string name;
    int frames = 0;
    uint32_t id = 0;               // unique per table built (newTableId): a cache's key
    std::vector<int16_t> data;     // [frame][level][mipLength(level) + 1]
    std::vector<float> scale;      // [frame]

    // Level `mip` of `frame`: mipLength(mip) samples plus a guard sample (= sample 0).
    const int16_t* get(int frame, int mip) const {
        return data.data() + static_cast<size_t>(frame) * kFrameStride + static_cast<size_t>(mipOffset(mip));
    }
    float at(int frame, int mip, int i) const { return static_cast<float>(get(frame, mip)[i]) * scale[static_cast<size_t>(frame)]; }
    size_t bytes() const { return data.size() * sizeof(int16_t) + scale.size() * sizeof(float); }
};

uint32_t newTableId();   // 1, 2, 3, ... (thread-safe)

// Highest harmonic h allowed at phase increment `inc` (cycles/sample): we let partials go
// above Nyquist as long as their alias folds back above ~18 kHz (h * inc <= 0.59 at 44.1 kHz),
// which keeps the top octave bright instead of dulling every note to the safe limit.
constexpr float kAliasLimit = 0.59f;

inline int mipFor(float inc) {
    const float allowed = kAliasLimit / inc;
    int k = 0;
    while (k < kMipLevels - 1 && static_cast<float>(kMaxHarmonic >> k) > allowed) ++k;
    return k;
}

// A frame as its harmonics: a[h] cos(2 pi h x) + b[h] sin(2 pi h x) for h = 1..kMaxHarmonic-1
// (index 0 unused; harmonic 1024 would sit on level 0's Nyquist, where a sine is zero).
struct Spectrum {
    std::vector<double> a = std::vector<double>(kMaxHarmonic, 0.0);
    std::vector<double> b = std::vector<double>(kMaxHarmonic, 0.0);
};

// Turns spectra into frames: each mip level by an inverse FFT of the harmonics it keeps (so it
// is band-limited exactly), stored as 16-bit samples with the frame's scale. Load time, not real
// time: it allocates. One builder per table build: it holds the FFT plans.
class FrameBuilder {
public:
    FrameBuilder();
    ~FrameBuilder();
    FrameBuilder(const FrameBuilder&) = delete;
    FrameBuilder& operator=(const FrameBuilder&) = delete;

    // Appends frame a, then b if not null (two real frames share each inverse FFT), at every
    // mip level, as they are (no normalising). A frame with a non-finite value gets a NaN scale.
    void add(Wavetable& t, const Spectrum& a, const Spectrum* b = nullptr) const;

private:
    struct Plans;
    std::unique_ptr<const Plans> plans_;
};

} // namespace af
