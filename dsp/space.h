#pragma once
// Space (docs/CONCEPT.md, "Space"): the instrument's reverb as a send / return. The strata's sends
// sum into it and it returns only the wet: the dry is the strata's own, and the engine sets the
// return's level. Inside is EffectForce's Reverb (dsp/reverb.h) at mix 1, run in its control
// chunks (32 samples) whatever the block.
//
// Rise: the wet ducks under what is coming in and blooms once it stops, so the reverb answers a
// note rather than blurring it. An envelope of the send's louder side (5 ms attack, 400 ms release)
// sets the wet's gain to 1 - rise x min(1, env / 0.04): at Rise 1 a send at -28 dBFS or louder
// mutes the wet, at Rise 0.5 it halves it. -28 dBFS is under a pad's send at the Init levels (the
// knobs' defaults, squared): a triad's envelope sits at -25 dBFS, two chords' at -23 (space.cpp).
// Ground's drone alone, at -36, ducks a little over half. The gain glides (20 ms) and lands
// exactly, so at Rise 0 Space is the Reverb bit for bit. Only the return ducks, not the network:
// the tail builds under the note as it would without Rise, and is there when the note lets go.
//
// silent(): the send and the wet (before Rise) have both stayed under -120 dBFS for as long as
// anything takes to come through the Reverb (Reverb::reachSamples()): nothing in flight, the tail
// gone, so the engine may stop running it. A frozen tail is never silent while it sounds. The
// counts saturate: an installation running for days never wraps them.
//
// Silent doesn't mean empty: the Reverb's predelay still holds what went in before the quiet
// stretch, and a longer predelay set while the engine skips Space would read it (the reach grows,
// so silent() turns false and the engine runs Space again, with no note played): a ghost of the
// last notes, at their level, however long ago they were. So once a silence begins, the Reverb
// forgets it (Reverb::forgetInput(): nothing cleared, nothing else changed), once a silence. A
// silence begins at the end of a process() call, or at a set(): the reach counts the larger of
// where the size, the mode and the predelay are and where they are going, and a set() that turns
// one back while it moves shortens it, so the engine's next look finds Space silent with no
// process() since. Nothing the network wrote before the quiet stretch is read again
// (Reverb::reachSamples()); what it wrote during it can come out at very short decays (there).
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
    void forgetIfSilent();

    Reverb reverb_;
    Reverb::Params rev_;          // what the Reverb is given: the asked, wet only
    Transport t_;
    float rise_ = 0.2f;
    float env_ = 0.0f;            // the send's envelope
    float gain_ = 1.0f;           // the wet's gain, gliding to where Rise puts it
    uint32_t quietIn_ = 0;        // samples the send has stayed under -120 dBFS (saturating)
    uint32_t quietOut_ = 0;       // ... and the wet
    bool held_ = false;           // a send came in since the Reverb last forgot its input
};

} // namespace af
