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
// depth, plus the diffusers while they run), counted in 32-sample chunks: nothing in flight, no
// repeat left, so the engine may stop running it. A repeat comes round within that, so with
// feedback 1 it is never silent while the repeats sound. The wet's level isn't measured while the
// send brings anything: silent() is false then whatever the wet is. The quiet count saturates: an
// installation running for days never wraps it.
//
// Silent doesn't mean empty: the Delay's lines still hold what went in up to 8 s before, at ages
// past the reach, where a longer time (or deeper wow, or a wider spread) would read it. Set while
// the engine skips Echo, such a change makes silent() false (the reach grows), and what went in
// before would come back. So the first process() after a call that ended silent() starts the
// Delay afresh, whether or not the engine skipped any in between: Delay::reset() hides the lines'
// old samples from every read and empties the cuts, the limiter and the duck, the diffusers are
// cleared the next time they run (a 23 KB clear, in that block), and the Delay jumps to the latest
// set(), as it would have glided there had it run on. Once a silence: until something has gone
// through the Delay again, it isn't started afresh again.
//
// Wet only from the start: an Echo that has never been set() plays initEcho() at mix 1. Drive and
// Glide stay at initEcho()'s (0 and Tape): M2 doesn't show them.
//
// Cost, in ARM instructions per 128-sample block: counted with a module-level harness (Echo alone,
// noise in, after the lines' first 8 s, two block counts under qemu's instruction-counting plugin,
// the device's compiler flags); Task 8 counts it again in the engine with make arm-icount. Since
// then the wet's level check is skipped while the send brings anything (with noise in, always:
// about 0.5k less) and the checks for a fresh start add a few a call, so each figure below is a
// little high until it is counted again.
// - M2's worst case (Ping-Pong, wow 1, Diffuse 1, duck 1, feedback 0.9, 1/4. at 120 BPM): 29.2k
//   (count pending); 29.8k at 1 ms, where the diffusers' sub-runs get shorter (count pending).
//   initEcho(): 29.2k as well (count pending).
// - The same at Diffuse 0: 25.1k (count pending): EffectForce's Delay 24.4k, the level checks and
//   copies the rest.
// M2's budget gives Echo 28k: this is 1.2k (4%) over it (count pending), all of it Diffuse's 4.1k.
// At the plan's 0.0245 points of p99 per thousand, about 0.72 points (count pending; device:
// pending).
//
// Real-time rules: the constructor allocates (the Delay's lines, 2.8 MB, and its diffusers, 23 KB);
// reset(), set() and process() don't allocate, lock or throw. The fresh start after a silence
// costs a few stores, and the 23 KB clear in the first block that runs the diffusers: once a
// silence, not every block.
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
    bool held_ = false;        // something has gone through the Delay since it last started afresh
    bool asleep_ = false;      // ... and the last process() ended silent(): the next starts it afresh
    uint32_t quiet_ = 0;       // samples the send and the wet (before the duck) have both stayed
                               // under -120 dBFS (saturating)
};

inline constexpr const char* kEchoModeNames[] = {"Stereo", "Ping-Pong", "Mono"};   // Delay::Mode
static_assert(sizeof kEchoModeNames / sizeof kEchoModeNames[0] == Delay::kModes, "a name for every mode");

// Echo's defaults (Init's, though Init sends nothing to it): a dotted quarter synced (450 ms free),
// the first repeats about 9 dB down a pass (feedback 0.45, and Diffuse's share, which falls away as
// the tail narrows: delay.h) through a band of 150 Hz to 4.5 kHz, a little tape wow, ducked a
// little under the strata, diffused a little more each pass.
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
