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
// before would come back. So once Echo has been silent() since something last went through the
// Delay, the next process() starts the Delay afresh, whether or not the engine skipped any calls
// in between. Silent can come at the end of a process(), or at a set(): set() works the reach out
// again from where the heads have glided to, so a time gliding down, or Diffuse landing on 0, can
// make Echo silent there, and the engine skip it from that block. The fresh start: Delay::clear()
// hides the lines' old samples from every read and empties the cuts and the limiter, the diffusers
// are cleared the next time they run, and the Delay jumps to the latest set(), as it would have
// glided there had it run on. The quiet count is full again, as after reset(): woken by a setting
// alone, Echo is silent again at the end of that call unless something comes in. Once a silence:
// until something has gone through the Delay again, it isn't started afresh again.
//
// The duck's envelope is kept across the fresh start. It follows the send, quiet since before the
// silence began, so it has fallen on its own, and it still says how loud the send has just been:
// emptied, it let a quiet phrase's first repeats after a short silence through 15 dB louder than a
// Delay that ran on. Where the engine skipped Echo it holds what it had when the skipping
// began: fallen (250 ms release) for the reach at least, where a Delay run on through a long
// pause would have let it fall to nothing.
//
// Wet only from the start: an Echo that has never been set() plays initEcho() at mix 1. Drive and
// Glide stay at initEcho()'s (0 and Tape): M2 doesn't show them.
//
// Cost, in ARM instructions per 128-sample block: counted with a module-level harness (Echo alone,
// noise in, after the lines' first 8 s; the device's compiler flags without the profile, under the
// qemu and insn plugin make arm-icount counts with, 768 blocks less 256); Task 8 counts it again in
// the engine.
// - M2's worst case (Ping-Pong, wow 1, Diffuse 1, duck 1, feedback 0.9, 1/4. at 120 BPM): 30.0k;
//   30.5k at 1 ms, where the diffusers' sub-runs get shorter. initEcho(): 30.0k as well.
// - The same at Diffuse 0: 25.9k: EffectForce's Delay 25.1k, the send's level check and the copies
//   the rest. The wet's level check, skipped while the send brings anything, took 0.4k more.
// - After a fresh start (a silence, or reset()), at the worst case: the first block 36.1k, most of
//   the extra the diffusers' 23 KB clear; then 30.9k a block for as long as the delay time (750 ms
//   here), the reads' old samples zeroed ahead of them, one or two fills a run; then 30.0k again.
// M2's budget gives Echo 28k: this is 2.0k (7%) over it, less than Diffuse's 4.1k. At the plan's
// 0.0268 points of p99 per thousand, about 0.80 points; the first block after a silence 0.97
// (device: pending). Task 8's bench should count a phrase that starts after a silence, so that
// those blocks are in its p99.
//
// Real-time rules: the constructor allocates (the Delay's lines, 2.8 MB, and its diffusers, 23 KB);
// reset(), set() and process() don't allocate, lock or throw. A fresh start costs the blocks above
// once a silence, not every block.
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
    bool asleep_ = false;      // ... and silent() since (at a process()'s end, or a set()): the next
                               // process() starts it afresh
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
