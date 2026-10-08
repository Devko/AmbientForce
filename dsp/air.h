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
// - Level, Mute, pan and the sends: `level` is a gain (the patch map squares the knob); the pan is
//   the engine's (its equal-power gains, setMix()), and so is the Echo send. The level and the mute,
//   the pan and the Echo send glide in a straight line over 10 ms from where they are, as Weather's
//   do; the Space send is taken at spaceSend, gliding across a render from the last render's (the
//   first after reset() takes it as given). Air applies them all in one pass over its voices' bus
//   (AirVoices::renderBus()): the dry at the level and the pan into the strata's bus, and before the
//   pan (plan decision 2) the Space send at the level and spaceSend, the Echo send at the level and
//   the Echo send. While nothing glides the gains are taken whole.
//
// When the engine may skip it. audible(): a voice ringing, and the level up or still gliding down
// (as Ground's: a muted Air is rendered until its glide has reached 0). generating(): the level up
// (not muted) and the generator may strike: it generates (setChord's `generate`) at a Density above
// 0, or Loop is on (a loop replays whether or not it generates). The engine renders Air while either
// is true. Once the level has glided down to 0 (or is set to 0 while it is there) Air lets every
// voice go at once, silent already: an Air muted or turned down holds no note to bring back, nothing
// rings on in the meter's count, and turned up again it starts afresh (the author's decision: a
// note struck a minute ago coming back when unmuted was a surprise, not a feature). Skipped, the
// generator's clock stands where it was (a loop synced to the bar finds its place again from the
// clock the engine gives it before each render). So a muted Air, or one at level 0, costs nothing
// and generates nothing.
//
// Control rate: render() cuts its n samples at each event's offset and renders the voices up to it
// before striking it, so every note starts on its own sample; the voices cut those pieces into their
// own control steps (airvoices.h). The glides are straight lines across each render.
//
// Cost, in ARM instructions per 128-sample block (make arm-icount, the bench's engine cases): see
// dsp/engine.h, which counts Air in M2's worst case (Felt ringing in six voices at Decay 20, Density
// 60, Loop on, its six keys struck again every 2 s, panned and sent to Echo) against the plan's 32k
// for ringing (the voices' own 29.7k, airvoices.h). With no voice ringing only the generator steps.
//
// Real-time rules: everything is fixed-size (the voices', the generator's). Nothing allocates, locks
// or throws after the constructor.
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
    // The stratum's pan, as its left and right gains (the engine's law), and its Echo send (a gain):
    // they glide from the next render. Set while Air isn't rendered, they are there at once.
    void setMix(float panL, float panR, float echoSend);
    // Every voice silent at once, the generator back at its seed's start (airgen.h: the chord and the
    // clock stay), the level, the pan and the Echo send where they are aimed. The patch stays.
    void reset();
    // Steps the generator, strikes its events at their offsets, and adds, in one pass: the dry at the
    // level and the pan into outL/outR; the Space send at the level and spaceSend into sendL/sendR;
    // the Echo send at the level and its send into echoL/echoR (nullptr: none, only while !echoing()).
    // Any n (cut into pieces of kMaxBlock). True if a voice sounded in it.
    bool render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR, float spaceSend,
                float* echoL, float* echoR, int n);
    // A voice ringing, and the level up or still gliding down.
    bool audible() const { return voices_.active() > 0 && (level_.now > 0.0f || level_.to > 0.0f); }
    // The level up (not muted) and the generator may strike (above).
    bool generating() const { return level_.to > 0.0f && ((generate_ && density_) || loop_); }
    // The Echo send above 0, or gliding: render() wants the Echo bus.
    bool echoing() const { return echo_.now > 0.0f || echo_.to > 0.0f; }

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
    bool block(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR, float spaceSend,
               float* echoL, float* echoR, int n);
    void mix(float* outL, float* outR, float* sendL, float* sendR, float* echoL, float* echoR, float s0, float s1, int n);
    void strike(int note, float vel, float pan, bool played);
    float playedPan(int note) const;
    float playedVel(float vel) const;
    void letGoIfDown();   // the level at 0 and staying there: every voice goes

    AirVoices voices_;
    AirGen gen_;
    float width_ = 0.7f, velSens_ = 0.5f;
    bool generate_ = false;      // setChord's
    bool density_ = false, loop_ = false;   // set()'s: Density above 0, Loop on
    uint32_t seed_ = 1, rng_ = 1;   // the generated notes' pans
    uint64_t strikes_ = 0;
    LineGlide level_;            // the level (0 muted), and the start of this render's line
    LineGlide panL_, panR_, echo_;   // the pan's gains and the Echo send
    float g0_ = 0.0f, pl0_ = 0.0f, pr0_ = 0.0f, e0_ = 0.0f;   // where each was at the render's start
    float send_ = -1.0f;         // the last render's spaceSend (-1: none since reset())
    StrikeHook hook_ = nullptr;
    void* hookCtx_ = nullptr;
    AirEvent ev_[kMaxEvents] = {};
};

} // namespace af
