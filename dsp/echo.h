#pragma once
// Echo (docs/CONCEPT.md, "Space"): the instrument's tape delay as a send / return, between the strata
// and Space. The strata's Echo sends sum into it and it returns only the wet: the dry is the strata's
// own, and the engine sets the return's level. Inside is EffectForce's Delay (dsp/delay.h) with
// AmbientForce's Diffuse, at mix 1, run in 32-sample chunks whatever the block, so its glides, wow
// and duck step as they did in EffectForce's rack. set() gives the Delay its targets at once and
// the chunks glide to them, as a set() before each would (it only moves targets; of the transport
// the Delay reads only the tempo). Its duck listens to the send: the repeats sink while the strata
// play into it and bloom in the gaps.
//
// silent(): the send and the wet (before the duck) have both stayed under -120 dBFS for as long as
// anything takes to come round the Delay once (Delay::reachSamples(): the longest read, at the wow's
// depth, plus the diffusers while they run): nothing in flight, no repeat left, so the engine may
// stop running it. A repeat comes round within that, so with feedback 1 it is never silent while
// the repeats sound. The quiet counts saturate: an installation running for days never wraps them.
//
// Wet only from the start: an Echo that has never been set() plays initEcho() at mix 1. Drive and
// Glide stay at initEcho()'s (0 and Tape): M2 doesn't show them.
//
// Cost, in ARM instructions per 128-sample block: counted with a module-level harness (Echo alone,
// noise in, after the lines' first 8 s, two block counts under qemu's instruction-counting plugin,
// the device's compiler flags); Task 8 counts it again in the engine with make arm-icount.
// - M2's worst case (Ping-Pong, wow 1, Diffuse 1, duck 1, feedback 0.9, 1/4. at 120 BPM): 29.2k;
//   29.8k at 1 ms, where the diffusers' sub-runs get shorter. initEcho(): 29.2k as well.
// - The same at Diffuse 0: 25.1k: EffectForce's Delay 24.4k, the level checks and copies the rest.
// M2's budget gives Echo 28k: this is 1.2k (4%) over it, all of it Diffuse's 4.1k. At the plan's
// 0.0245 points of p99 per thousand, about 0.72 points (device: pending).
//
// Real-time rules: the constructor allocates (the Delay's lines, 2.8 MB, and its diffusers, 23 KB);
// reset(), set() and process() don't allocate, lock or throw.
#include "common.h"
#include "delay.h"

#include <cstdint>

namespace af {

class Echo {
public:
    struct Params {
        Delay::Params delay;   // mix is forced to 1 (wet only); drive 0, glide Tape: not exposed in M2
    };

    Echo();   // allocates the Delay's lines (UI thread)
    void reset();
    void set(const Params& p, const Transport& t);
    // sendL/R: the strata's summed Echo sends; out: the wet return (overwritten; it may be the
    // send's own buffers). Any n: the Delay runs in 32-sample chunks.
    void process(const float* sendL, const float* sendR, float* outL, float* outR, int n);
    bool silent() const;   // the repeats are under -120 dBFS and nothing is coming in

private:
    Delay delay_;
    Delay::Params dp_;         // what the Delay is given: the asked, wet only
    Transport t_;
    bool jump_ = true;         // reset() with no set() since: the next process() sets the Delay again
    uint32_t quietIn_ = 0;     // samples the send has stayed under -120 dBFS (saturating)
    uint32_t quietOut_ = 0;    // ... and the wet, before the duck
};

inline constexpr const char* kEchoModeNames[] = {"Stereo", "Ping-Pong", "Mono"};   // Delay::Mode
static_assert(sizeof kEchoModeNames / sizeof kEchoModeNames[0] == Delay::kModes, "a name for every mode");

// Echo's defaults (Init's, though Init sends nothing to it): a dotted quarter synced (450 ms free),
// repeats about 9 dB down a pass (feedback 0.45, and Diffuse's share) through a band of 150 Hz to
// 4.5 kHz, a little tape wow, ducked a little under the strata, diffused a little more each pass.
inline Echo::Params initEcho() {
    Echo::Params p;
    Delay::Params& d = p.delay;
    d.mode = Delay::STEREO;
    d.sync = true;
    d.divBeats = 1.5;   // "1/4."
    d.timeMs = 450.0f;
    d.feedback = 0.45f;
    d.spread = 0.0f;
    d.lowCutHz = 150.0f;
    d.highCutHz = 4500.0f;
    d.wow = 0.3f;
    d.drive = 0.0f;
    d.duck = 0.3f;
    d.diffuse = 0.3f;
    d.mix = 1.0f;       // Echo forces it anyway: a return is wet only
    d.glide = Delay::TAPE;
    return p;
}

} // namespace af
