#include "space.h"
#include "svf.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace af {

namespace {

constexpr float kQuiet = 1e-6f;              // -120 dBFS: under it, nothing is coming in or out
constexpr uint32_t kQuietMax = 1u << 30;     // the quiet counts stop here (6.8 hours, far past any reach)
// The send's envelope at which Rise ducks all it may: -28 dBFS. Measured on the default patch at the
// Init levels (Ground and Bloom 0.7 and the sends 0.4 and 0.5, all squared by the patch map), the
// Rise envelope holds a sustained triad at -25 dBFS (Bloom's -25.4, Ground's -35.6), two chords
// (6 voices) at -23: both duck fully at Rise 1, with 3 dB to spare. (The first guess, -12 dBFS, left
// the triad's wet at 0.77 at Rise 1: Rise barely ducked.) The patch's own gains unsquared (0.7, 0.4,
// 0.5) put the triad at -16 dBFS.
constexpr float kFull = 0.04f;
constexpr float kClamp = 8.0f;               // +18 dBFS: a wild sample mustn't hold the wet down for long
// The gain this near its target is there, so Rise 0 is exactly 1. Not nearer: each sample moves it
// 0.11% of the way, and a float near 1 stops moving once that is under half an ulp, 2.6e-5 short.
constexpr float kLand = 1e-4f;

// Set when the plugin loads (no guard to take on the audio thread).
const float kAttack = smoothCoef(0.005f), kRelease = smoothCoef(0.4f), kGlide = smoothCoef(0.02f);

} // namespace

// Wet only before any set() too: Reverb::Params' own mix is EffectForce's 0.3.
Space::Space() {
    rev_.mix = 1.0f;
    reset();
}

// Silent from here: the Reverb starts afresh (with what it was last given), nothing has come in.
void Space::reset() {
    reverb_.set(rev_, t_);
    reverb_.reset();
    env_ = 0.0f;
    gain_ = 1.0f;
    quietIn_ = quietOut_ = kQuietMax;
}

void Space::set(const Params& p, const Transport& t) {
    rev_ = p.reverb;
    rev_.mix = 1.0f;
    rise_ = clampParam(p.rise, 0.0f, 1.0f, 0.2f);
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

        // The wet, in the Reverb's own chunks; its level before Rise says whether a tail is left
        // (a NaN, which it never returns, would count as one).
        std::memmove(outL + i, sendL + i, sizeof(float) * static_cast<size_t>(m));
        std::memmove(outR + i, sendR + i, sizeof(float) * static_cast<size_t>(m));
        reverb_.set(rev_, t_);
        reverb_.process(outL + i, outR + i, m);
        bool tail = false;
        for (int k = 0; k < m; ++k) tail = tail || !(std::fabs(outL[i + k]) <= kQuiet) || !(std::fabs(outR[i + k]) <= kQuiet);
        if (!unity) {
            for (int k = 0; k < m; ++k) {
                outL[i + k] *= gain[k];
                outR[i + k] *= gain[k];
            }
        }

        const uint32_t um = static_cast<uint32_t>(m);
        quietIn_ = in > kQuiet ? 0 : std::min(quietIn_ + um, kQuietMax);
        quietOut_ = tail ? 0 : std::min(quietOut_ + um, kQuietMax);
        if (t_.playing) t_.beats += m / static_cast<double>(kRate) * t_.bpm / 60.0;
    }

    // Silent: the engine may stop running Space now, for minutes, and a longer predelay set
    // meanwhile would read what went in before the silence: the Reverb forgets that (space.h).
    // While anything sounds a count is 0 and reachSamples() isn't asked.
    if (quietIn_ > 0 && quietOut_ > 0 && silent()) reverb_.forgetInput(quietIn_);
}

bool Space::silent() const {
    const uint32_t reach = static_cast<uint32_t>(reverb_.reachSamples());
    return quietIn_ >= reach && quietOut_ >= reach;
}

} // namespace af
