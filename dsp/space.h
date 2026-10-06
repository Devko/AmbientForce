#pragma once
// Space (docs/CONCEPT.md, "Space"): the instrument's reverb as a send / return. The strata's sends
// sum into it and it returns only the wet: the dry is the strata's own, and the engine sets the
// return's level. Inside is EffectForce's Reverb (dsp/reverb.h) at mix 1, run in its control
// chunks (32 samples) whatever the block.
//
// Rise: the wet ducks under what is coming in and blooms once it stops, so the reverb answers a
// note rather than blurring it. An envelope of the send's louder side (5 ms attack, 400 ms release)
// sets the wet's gain to 1 - rise x min(1, env / 0.25): at Rise 1 a send peaking around -12 dBFS
// or louder mutes the wet, at Rise 0.5 it halves it. (That -12 dBFS is a first guess: Tasks 8 and
// 10 calibrate it against the strata's real send levels.) The gain glides (20 ms) and lands
// exactly, so at Rise 0 Space is the Reverb bit for bit. Only the return ducks, not the network:
// the tail builds under the note as it would without Rise, and is there when the note lets go.
//
// silent(): the send and the wet (before Rise) have both stayed under -120 dBFS for as long as
// anything takes to come through the Reverb (Reverb::reachSamples()): nothing in flight, the tail
// gone, so the engine may stop running it. A frozen tail is never silent while it sounds. The
// counts saturate: an installation running for days never wraps them.
//
// Wet only from the start: a Space that has never been set() plays the Reverb's defaults at mix 1.
//
// Real-time rules: the constructor allocates (the Reverb's buffers); reset(), set() and process()
// don't allocate, lock or throw.
#include "common.h"
#include "reverb.h"

#include <cstdint>

namespace af {

class Space {
public:
    struct Params {
        Reverb::Params reverb;   // mix is forced to 1 (wet only): Space is a return, the dry is elsewhere
        float rise = 0.2f;       // 0..1: the wet ducks under the send's level and blooms after it
    };

    Space();
    void reset();
    void set(const Params& p, const Transport& t);
    // sendL/R: the strata's summed sends; out: the wet return (overwritten; it may be the send's
    // own buffers). Any n: the Reverb runs in its own 32-sample chunks.
    void process(const float* sendL, const float* sendR, float* outL, float* outR, int n);
    bool silent() const;   // the tail is under -120 dBFS and nothing is coming in

private:
    Reverb reverb_;
    Reverb::Params rev_;          // what the Reverb is given: the asked, wet only
    Transport t_;
    float rise_ = 0.2f;
    float env_ = 0.0f;            // the send's envelope
    float gain_ = 1.0f;           // the wet's gain, gliding to where Rise puts it
    uint32_t quietIn_ = 0;        // samples the send has stayed under -120 dBFS (saturating)
    uint32_t quietOut_ = 0;       // ... and the wet
};

} // namespace af
