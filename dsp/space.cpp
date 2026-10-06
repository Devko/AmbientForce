#include "space.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace af {

namespace {

constexpr float kQuiet = 1e-6f;              // -120 dBFS: under it, nothing is coming in or out
constexpr uint32_t kQuietMax = 1u << 30;     // the quiet counts stop here (6.8 hours, far past any reach)
constexpr float kFull = 0.25f;               // the send's envelope at which Rise ducks all it may: -12 dBFS
constexpr float kClamp = 8.0f;               // +18 dBFS: a wild sample mustn't hold the wet down for long
// The gain this near its target is there, so Rise 0 is exactly 1. Not nearer: each sample moves it
// 0.11% of the way, and a float near 1 stops moving once that is under half an ulp, 2.6e-5 short.
constexpr float kLand = 1e-4f;

float perSample(float seconds) { return 1.0f - std::exp(-1.0f / (seconds * kRate)); }

// Set when the plugin loads (no guard to take on the audio thread).
const float kAttack = perSample(0.005f), kRelease = perSample(0.4f), kGlide = perSample(0.02f);

// A parameter clamped to its range; NaN (which no comparison catches) becomes `nan`.
float clampOr(float x, float lo, float hi, float nan) { return x >= lo ? (x <= hi ? x : hi) : (x < lo ? lo : nan); }

} // namespace

Space::Space() { reset(); }

// Silent from here: the Reverb starts afresh, nothing has come in.
void Space::reset() {
    reverb_.reset();
    env_ = 0.0f;
    gain_ = 1.0f;
    quietIn_ = quietOut_ = kQuietMax;
}

void Space::set(const Params& p, const Transport& t) {
    rev_ = p.reverb;
    rev_.mix = 1.0f;
    rise_ = clampOr(p.rise, 0.0f, 1.0f, 0.2f);
    t_ = t;
    reverb_.set(rev_, t_);   // now as well: silent() asks the Reverb how far it reaches with these
}

void Space::process(const float* sendL, const float* sendR, float* outL, float* outR, int n) {
    for (int i = 0; i < n; i += kChunk) {
        const int m = std::min(kChunk, n - i);

        // Rise, from the send before the return is written (it may be written over the send).
        float gain[kChunk];
        float env = env_, g = gain_, in = 0.0f;
        bool unity = true;
        for (int k = 0; k < m; ++k) {
            const float level = std::min(std::max(std::fabs(sanitize(sendL[i + k])), std::fabs(sanitize(sendR[i + k]))), kClamp);
            in = std::max(in, level);
            env += (level > env ? kAttack : kRelease) * (level - env);
            const float target = 1.0f - rise_ * std::min(1.0f, env * (1.0f / kFull));
            const float d = target - g;
            g = std::fabs(d) < kLand ? target : g + d * kGlide;
            gain[k] = g;
            unity = unity && g == 1.0f;
        }
        env_ = env < 1e-15f ? 0.0f : env;   // dying away: no denormals
        gain_ = g;

        // The wet, in the Reverb's own chunks; its level before Rise says whether a tail is left.
        std::memmove(outL + i, sendL + i, sizeof(float) * static_cast<size_t>(m));
        std::memmove(outR + i, sendR + i, sizeof(float) * static_cast<size_t>(m));
        reverb_.set(rev_, t_);
        reverb_.process(outL + i, outR + i, m);
        float out = 0.0f;
        for (int k = 0; k < m; ++k) out = std::max(out, std::max(std::fabs(outL[i + k]), std::fabs(outR[i + k])));
        if (!unity) {
            for (int k = 0; k < m; ++k) {
                outL[i + k] *= gain[k];
                outR[i + k] *= gain[k];
            }
        }

        const uint32_t um = static_cast<uint32_t>(m);
        quietIn_ = in > kQuiet ? 0 : std::min(quietIn_ + um, kQuietMax);
        quietOut_ = out > kQuiet ? 0 : std::min(quietOut_ + um, kQuietMax);
        if (t_.playing) t_.beats += m / static_cast<double>(kRate) * t_.bpm / 60.0;
    }
}

bool Space::silent() const {
    const uint32_t reach = static_cast<uint32_t>(reverb_.reachSamples());
    return quietIn_ >= reach && quietOut_ >= reach;
}

} // namespace af
