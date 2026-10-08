#pragma once
// Air (docs/CONCEPT.md 5.3), the stratum: Air's generator (dsp/airgen.h) driving Air's voices
// (dsp/airvoices.h). The generator says when and which note; Air strikes it on the voices at its
// sample, with a pan and a velocity, and puts the stratum's level, mute and send on what they play.
// The engine decides what Air hears (Listen, Split): the chord it generates from (setChord) and the
// notes the player gives it (play).
//
// - Generated notes (and a loop's replays of them) are struck at the velocity the generator gives
//   them, each at a pan drawn within +-Width (AirVoicePatch::width), evenly, from Air's own random
//   numbers (seeded: the same seed and the same calls play the same samples).
// - A note the player gives Air (Notes, or a key at or above Split) is struck at once, at the next
//   render's first sample, and passed to the generator (played(): Echo's motif learns it, a loop
//   records it). Its pan follows its pitch, low left to high right: Width x (note - 66) / 42, so the
//   voices' range, 24 to 108, spans the field. Its velocity goes into its level through Vel, as
//   Bloom's does: 1 - Vel + Vel x vel (Vel 0: every note as if struck hard). A note outside 24..108
//   is moved there by octaves, as the generator moves its replays. A loop's replay of a player's
//   note (AirEvent::played) is struck the same way.
// - Level and Mute: `level` is a gain (the patch map squares the knob). The level and the mute glide
//   in a straight line over 10 ms from where they are, as Weather's do; the send is taken after the
//   level, at spaceSend, gliding across a render from the last render's (the first after reset()
//   takes it as given). Both are applied here, after the voices: AirVoices renders into Air's own
//   bus with no send of its own (its spaceSend 0), and Air mixes that bus into the strata's.
//
// When the engine may skip it. audible(): a voice ringing, and the level up or still gliding down
// (as Ground's: a muted Air is rendered until its glide has reached 0). generating(): the level up
// (not muted) and the generator may strike: it generates (setChord's `generate`) at a Density above
// 0, or Loop is on (a loop replays whether or not it generates). The engine renders Air while either
// is true. Skipped, Air stands still: its voices' rings and the generator's clock pause where they
// are (a loop synced to the bar finds its place again from the clock the engine gives it). So a
// muted Air, or one at level 0, costs nothing and generates nothing; brought back, it goes on.
//
// Control rate: render() cuts its n samples at each event's offset and renders the voices up to it
// before striking it, so every note starts on its own sample; the voices cut those pieces into their
// own control steps (airvoices.h). The level's glide is a straight line across each render.
//
// Cost, in ARM instructions per 128-sample block (make arm-icount, the bench's engine cases): in M2's
// worst case Air adds 38.4k (Felt ringing in six voices at Decay 20, Density 60, Loop on, its six
// keys struck again every 2 s, panned and sent to Echo; the worst case less Air), against the plan's
// 32k for ringing (the voices' own 29.7k, airvoices.h; about 1 point of p99 at the plan's 0.0268 a
// thousand, device: pending). Air's own work on top of the voices and the generator is the bus:
// zeroed, then mixed into the dry and the send; with no voice ringing only the generator steps.
//
// Real-time rules: everything is fixed-size (the voices', the generator's, the bus). Nothing
// allocates, locks or throws after the constructor.
#include "airgen.h"
#include "airvoices.h"
#include "common.h"
#include "harmony.h"
#include "lifetime.h"

#include <cstdint>

namespace af {

struct AirPatch {
    int listen = LI_HARMONY;   // the engine's
    bool mute = false;
    float level = 0.0f;        // a gain (the knob squared): Init has Air asleep
    float velSens = 0.5f;      // 0..1: played notes' velocity into their level
    AirVoicePatch voice;
    AirGenPatch gen;
};

class Air {
public:
    static constexpr int kMaxEvents = 16;   // events a step of the generator may give (more wait, airgen.h)
    static constexpr int kMaxBlock = 128;   // render's n is cut into pieces of at most this
    static constexpr int kLevelGlide = 441; // 10 ms: the level and the mute glide over this

    Air();
    // The random numbers (the voices' noise, the generator's draws, the pans), then reset().
    void seed(uint32_t s);
    void set(const AirPatch& p, const HarmonyPatch& h);
    void setChord(const Chord& c, bool generate);  // AirGen's
    void play(int note, float vel);                // a note the player gave Air (Notes, Split): struck now and played()
    void setTransport(double bpm, double beats, bool locked);
    // Every voice silent at once, the generator back at its seed's start (airgen.h: the chord and the
    // clock stay), the level where it is aimed. The patch stays.
    void reset();
    // Steps the generator, strikes its events at their offsets; adds the dry into outL/outR and the
    // send at spaceSend into sendL/sendR. Any n (cut into pieces of kMaxBlock).
    void render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR,
                float spaceSend, int n);
    bool audible() const;                          // a voice ringing, or the level still ramping down
    bool generating() const;                       // the level up and the generator may strike (above)

    // For the tests and the engine's Info: strikes so far (generated, replayed and played), the
    // voices, and a hook called with every strike (nullptr: none; the plugin never sets one).
    uint64_t strikes() const { return strikes_; }
    const AirVoices& voices() const { return voices_; }
    using StrikeHook = void (*)(void* ctx, int note, float vel, float pan, bool played);
    void setStrikeHook(StrikeHook hook, void* ctx) {
        hook_ = hook;
        hookCtx_ = ctx;
    }

private:
    void block(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR, float spaceSend, int n);
    void strike(int note, float vel, float pan, bool played);
    float playedPan(int note) const;
    float playedVel(float vel) const;

    AirVoices voices_;
    AirGen gen_;
    float width_ = 0.7f, velSens_ = 0.5f, level_ = 0.0f;
    bool mute_ = false;
    bool generate_ = false;      // setChord's
    bool density_ = false, loop_ = false;   // set()'s: Density above 0, Loop on
    uint32_t seed_ = 1, rng_ = 1;   // the generated notes' pans
    uint64_t strikes_ = 0;
    float levelNow_ = 0.0f, levelTo_ = 0.0f, levelStep_ = 0.0f;   // the level's glide, a step a sample
    float send_ = -1.0f;         // the last render's spaceSend (-1: none since reset())
    StrikeHook hook_ = nullptr;
    void* hookCtx_ = nullptr;
    AirEvent ev_[kMaxEvents] = {};
    float busL_[kMaxBlock] = {}, busR_[kMaxBlock] = {};     // the voices' dry
    float junkL_[kMaxBlock] = {}, junkR_[kMaxBlock] = {};   // their send at 0 (never read)
};

} // namespace af
