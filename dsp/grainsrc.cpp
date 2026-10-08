// A source for Weather's grains: see grainsrc.h. The levels are made as EffectForce's Grain makes them
// while it records (dsp/grain.cpp's decimate(), d94bde0): pairs of frames, early first, through
// halfband.h's StereoDecimator; here over a loop, warmed up on its own end.
#include "grainsrc.h"

#include "common.h"
#include "halfband.h"

#include <algorithm>
#include <cmath>

namespace af {

namespace {

constexpr int kWarmPairs = 4096;   // a decimator's slowest pole is gone (0.966^4096) long before this

// A float sample (full scale 1) as 16 bit, rounded to nearest, clamped.
int16_t toInt16(float x) {
    const float s = std::floor(x * 32768.0f + 0.5f);
    return static_cast<int16_t>(s >= 32767.0f ? 32767.0f : (s <= -32768.0f ? -32768.0f : s));
}

// One level from the one above it (n frames, L R interleaved, a loop): n / 2 frames into out.
// The decimator first runs round the loop's end, so the pass that counts starts where the loop's
// own end leaves it.
void decimateLoop(const std::vector<float>& in, int n, std::vector<float>& out) {
    const int pairs = n / 2;
    StereoDecimator d;
    const auto pair = [&](int p, float& l, float& r) {
        const float* f = &in[static_cast<size_t>(4 * p)];
        d.process(f4{f[0], f[2], f[1], f[3]}, l, r);   // (left early, left late, right early, right late)
    };
    float l, r;
    for (int w = 0; w < kWarmPairs; ++w) pair(((w - kWarmPairs) % pairs + pairs) % pairs, l, r);
    out.assign(static_cast<size_t>(2 * pairs), 0.0f);
    for (int p = 0; p < pairs; ++p) {
        pair(p, l, r);
        out[static_cast<size_t>(2 * p)] = l;
        out[static_cast<size_t>(2 * p + 1)] = r;
    }
}

// A level's samples as 16 bit, then the guard: its first frames again (round and round a level
// shorter than the guard).
void store(const std::vector<float>& x, int n, std::vector<int16_t>& out) {
    out.resize(static_cast<size_t>(2 * (n + GrainSource::kGuard)));
    for (int i = 0; i < 2 * n; ++i) out[static_cast<size_t>(i)] = toInt16(x[static_cast<size_t>(i)]);
    for (int i = 0; i < GrainSource::kGuard; ++i) {
        out[static_cast<size_t>(2 * (n + i))] = out[static_cast<size_t>(2 * (i % n))];
        out[static_cast<size_t>(2 * (n + i) + 1)] = out[static_cast<size_t>(2 * (i % n) + 1)];
    }
}

} // namespace

size_t SourceBuffer::bytes() const {
    size_t b = 0;
    for (const std::vector<int16_t>& d : data) b += d.size() * sizeof(int16_t);
    return b;
}

std::unique_ptr<SourceBuffer> buildSource(const float* L, const float* R, int frames) {
    if (!L || !R || frames <= 0) return nullptr;

    // The loop: len frames, a multiple of 4; the `fade` frames past it fade into its start.
    int fade = std::min(static_cast<int>(std::lround(kLoopFadeS * kRate)), frames / 2);
    int len = (frames - fade) / 4 * 4;
    if (len < 4) {
        len = 4;
        fade = 0;
    } else {
        fade = std::min(frames - len, len - 1);   // (len - 1: the loop's last frame stays the signal's)
    }
    const auto in = [&](const float* x, int i) { return i < frames ? sanitize(x[i]) : 0.0f; };
    std::vector<float> lev(static_cast<size_t>(2 * len));
    for (int i = 0; i < len; ++i) {
        float l = in(L, i), r = in(R, i);
        if (i < fade) {   // equal power: sin up for the start, cos down for what ran past the end
            const double th = 0.5 * 3.14159265358979323846 * i / fade;
            const float a = static_cast<float>(std::sin(th)), b = static_cast<float>(std::cos(th));
            l = a * l + b * in(L, len + i);
            r = a * r + b * in(R, len + i);
        }
        lev[static_cast<size_t>(2 * i)] = l;
        lev[static_cast<size_t>(2 * i + 1)] = r;
    }

    // The level: the loop's RMS at kSourceRmsDb, unless that would take its peak past full scale.
    double sum = 0.0;
    float top = 0.0f;
    for (const float x : lev) {
        sum += static_cast<double>(x) * x;
        top = std::max(top, std::fabs(x));
    }
    const double rmsNow = std::sqrt(sum / static_cast<double>(lev.size()));
    float scale = 1.0f;
    if (rmsNow > 0.0) {
        scale = static_cast<float>(std::pow(10.0, kSourceRmsDb / 20.0) / rmsNow);
        scale = std::min(scale, 1.0f / top);
    }
    for (float& x : lev) x *= scale;

    auto sb = std::make_unique<SourceBuffer>();
    std::vector<float> slower;
    store(lev, len, sb->data[0]);
    decimateLoop(lev, len, slower);
    store(slower, len / 2, sb->data[1]);
    decimateLoop(slower, len / 2, lev);
    store(lev, len / 4, sb->data[2]);
    sb->src.frames = len;
    sb->src.origin = 0;
    sb->src.gain = 1.0f / 32768.0f;
    for (int k = 0; k < GrainSource::kLevels; ++k) sb->src.level[k] = sb->data[k].data();
    return sb;
}

} // namespace af
