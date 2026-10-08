#include "engine.h"

#include "stages.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace af {

#ifdef AF_STAGE_TIMING
uint64_t g_stageNs[STG_COUNT] = {};
#endif

namespace {

std::atomic<uint32_t> g_guardTrips{0}, g_limited{0};   // guardTrips(), limitedSamples()

constexpr float kTiltStep = TiltShelf::kStep;   // a control step's tilt glide (svf.h's shelf, Weather's too)
constexpr float kFloorDb = -60.0f;          // Stop's fade ends here, and resets
constexpr float kLimitLand = 1e-3f;         // the limiter's release lands on 1 from here (0.009 dB)
constexpr int kGlideSamples = 441;          // the volume, the return and the pans glide 10 ms
constexpr float kSqrt2 = 1.41421356f;
constexpr int kTonicNote = 48;              // where Free's tonic chord is built (C3 + key)
constexpr uint32_t kAbsMask = 0x7FFFFFFFu, kInfBits = 0x7F800000u;

// Set when the plugin loads (no guard to take on the audio thread): the limiter's gain moves 1 ms
// down and 150 ms back up (a one-pole a sample).
const float kAttack = smoothCoef(0.001f), kRelease = smoothCoef(0.150f);

// The volume knob's gain: off at kVolumeOffDb, at most +12 dB. (common.h's dbToGain is the fast one
// for the DSP; this one is exact and runs only when the patch changes.)
float volumeGain(float db) {
    if (!(db > kVolumeOffDb)) return 0.0f;   // NaN too
    return std::pow(10.0f, (db < 12.0f ? db : 12.0f) / 20.0f);
}

// The make-up gain (engine.h, the output), set when the plugin loads.
const float kMakeUp = std::pow(10.0f, Engine::kMakeUpDb / 20.0f);

// A stratum's pan, -1..1: equal power, unity in the middle (exactly, so a centred stratum is its
// own dry bit for bit), sqrt 2 at the sides, where the other side is exactly 0 (as Ground pans its
// partials).
void panGains(float pan, float& l, float& r) {
    pan = clampParam(pan, -1.0f, 1.0f, 0.0f);
    if (pan == 0.0f) {
        l = r = 1.0f;
    } else if (pan <= -1.0f) {
        l = kSqrt2;
        r = 0.0f;
    } else if (pan >= 1.0f) {
        l = 0.0f;
        r = kSqrt2;
    } else {
        const float a = (pan + 1.0f) * 0.785398163f;   // 0 .. pi/2
        l = kSqrt2 * std::cos(a);
        r = kSqrt2 * std::sin(a);
    }
}

bool sameNotes(const Chord& a, const Chord& b) {
    if (a.n != b.n) return false;
    for (int i = 0; i < a.n; ++i)
        if (a.notes[i] != b.notes[i]) return false;
    return true;
}

// |x|'s bits: for finite floats they order as the magnitudes do, and an infinity or a NaN is
// kInfBits or more. Integer work, which no float optimisation can fold away; a block's maximum
// vectorises, and gives both the guard's answer and the limiter's peak.
inline uint32_t absBits(float x) {
    uint32_t u;
    std::memcpy(&u, &x, sizeof u);
    return u & kAbsMask;
}

} // namespace

// --- glides -------------------------------------------------------------------------------------

void Engine::Glide::aim(int samples) {
    if (target == to) return;
    to = target;
    step = (to - now) / static_cast<float>(samples);
    if (step == 0.0f) now = to;
}

// A step under half an ulp of `now` doesn't move it (a target change of a couple of hundred ulps
// over 441 samples): there, as past the target, it lands.
inline float Engine::Glide::next() {
    if (step != 0.0f) {
        const float x = now + step;
        if (x == now || (step > 0.0f ? x >= to : x <= to)) {
            now = to;
            step = 0.0f;
        } else {
            now = x;
        }
    }
    return now;
}

void Engine::Glide::land() {
    now = to = target;
    step = 0.0f;
}

// A gain across a piece, in place on both channels.
void Engine::Glide::apply(float* L, float* R, int n) {
    if (step == 0.0f) {
        if (now == 1.0f) return;
        for (int i = 0; i < n; ++i) {
            L[i] *= now;
            R[i] *= now;
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        const float g = next();
        L[i] *= g;
        R[i] *= g;
    }
}

// --- the patch ----------------------------------------------------------------------------------

Engine::Engine(const TableSet& tables) : tables_(tables) {
    setPatch(Patch{});
    reset();
}

void Engine::seed(uint32_t s) {
    ground_.seed(s);   // a reset of Ground: it is given its target again
    bloom_.seed(s * 0x9E3779B1u + 0x7F4A7C15u);   // not Ground's sequence
    air_.seed(s * 0x85EBCA6Bu + 0xC2B2AE35u);     // nor either's
    weather_.seed(s * 0x27D4EB2Fu + 0x165667B1u);
    groundWant_ = -2;
    weatherGate_ = false;   // Weather's reset closed it: the route opens it again if it should be
    airSync_ = true;
    route();
}

void Engine::setPatch(const Patch& in) {
    Patch p = in;
    p.tilt = clampParam(p.tilt, -1.0f, 1.0f, 0.0f);
    p.onStop = std::min(std::max(p.onStop, 0), OS_COUNT - 1);
    p.ground.listen = std::min(std::max(p.ground.listen, 0), LI_COUNT - 1);
    p.bloom.listen = std::min(std::max(p.bloom.listen, 0), LI_COUNT - 1);
    p.groundSpace = clampParam(p.groundSpace, 0.0f, 1.0f, 0.0f);
    p.bloomSpace = clampParam(p.bloomSpace, 0.0f, 1.0f, 0.0f);
    p.spaceReturn = clampParam(p.spaceReturn, 0.0f, 1.0f, 0.0f);
    p.harmony.key = std::min(std::max(p.harmony.key, 0), 11);
    p.air.listen = std::min(std::max(p.air.listen, 0), LI_COUNT - 1);
    p.weather.listen = std::min(std::max(p.weather.listen, 0), LI_COUNT - 1);
    for (float* s : {&p.groundEcho, &p.bloomEcho, &p.airSpace, &p.airEcho, &p.weatherSpace, &p.weatherEcho,
                     &p.echoReturn, &p.echoSpace})
        *s = clampParam(*s, 0.0f, 1.0f, 0.0f);
    p.memoryTap = std::min(std::max(p.memoryTap, 0), MT_COUNT - 1);
    if (p.split < 0 || p.split > 127) p.split = -1;
    const Patch was = p_;
    p_ = p;

    harmony_.set(p.harmony);
    ground_.set(p.ground, p.harmony);
    bloom_.set(p.bloom, p.harmony);
    air_.set(p.air, p.harmony);
    echo_.set(p.echo, transport_);
    echoBpm_ = transport_.bpm;

    // Space: the tail's hold. Abyss rings four times its Decay, so a quarter of the Release does.
    spaceParams_ = p.space;
    float& decay = spaceParams_.reverb.decayS;
    if (p.bloom.tail == TL_SPACE) {
        const float release = clampParam(p.bloom.releaseS, 0.01f, 30.0f, 0.0f);   // Bloom's range
        decay = std::max(decay, spaceParams_.reverb.mode == Reverb::ABYSS ? 0.25f * release : release);
    }
    space_.set(spaceParams_, transport_);

    // The mix: Space's sends only where its return is heard, and Bloom's only where Space can carry
    // a tail (a frozen reverb takes no input: there Bloom releases as Tail Voice).
    groundSend_ = p.spaceReturn > 0.0f ? p.groundSpace : 0.0f;
    bloomSend_ = p.spaceReturn > 0.0f && !p.space.reverb.freeze ? p.bloomSpace : 0.0f;
    airSend_ = p.spaceReturn > 0.0f ? p.airSpace : 0.0f;
    weatherSend_ = p.spaceReturn > 0.0f ? p.weatherSpace : 0.0f;
    ret_.target = p.spaceReturn;
    panGains(p.groundPan, gPanL_.target, gPanR_.target);
    panGains(p.bloomPan, bPanL_.target, bPanR_.target);
    // Echo: the strata's sends only where its return is heard, in the mix or through Space; its own
    // send into Space only where Space's return is. Air and Weather take their pan and Echo send
    // themselves (setMix: one pass over their own output).
    const bool echoHeard = p.echoReturn > 0.0f || (p.echoSpace > 0.0f && p.spaceReturn > 0.0f);
    gEcho_.target = echoHeard ? p.groundEcho : 0.0f;
    bEcho_.target = echoHeard ? p.bloomEcho : 0.0f;
    float panL = 1.0f, panR = 1.0f;
    panGains(p.airPan, panL, panR);
    air_.setMix(panL, panR, echoHeard ? p.airEcho : 0.0f);
    echoRet_.target = p.echoReturn;
    echoSpace_.target = p.spaceReturn > 0.0f ? p.echoSpace : 0.0f;
    volume_.target = volumeGain(p.volumeDb) * kMakeUp;
    tilt_ = p.tilt;

    // Free's tonic chord: the patch's chord type on the key (a triad under Chord Off).
    HarmonyPatch tonic = p.harmony;
    if (tonic.chord == CH_OFF) tonic.chord = CH_TRIAD;
    freeChord_ = buildChord(tonic, kTonicNote + p.harmony.key);

    changed(was);
    // Weather's gate before its set(), on purpose: a patch that closes the gate (Listen on the notes
    // with no key held, a Hold let go) and raises the level at once (a preset loaded) leaves Weather
    // silent, where set() first would play the gate's fade out of a cloud its Listen doesn't want
    // (weather.h: within one call order counts).
    routeWeather();
    weather_.set(p.weather, p.harmony);
    panGains(p.weatherPan, panL, panR);
    weather_.setMix(panL, panR, echoHeard ? p.weatherEcho : 0.0f);
    route();
    if (fading_ && p.onStop == OS_CUT) sleep(false);
}

// What a patch change does to what is sounding, besides the settings themselves.
void Engine::changed(const Patch& was) {
    // Hold turned off: the latched keys go (to the pedal, while it is down).
    if (was.hold && !p_.hold)
        for (int k = 0; k < 128; ++k)
            if (key_[k] == K_HOLD) {
                if (pedal_) key_[k] = K_PEDAL;
                else letGo(k);
            }

    // Bloom's Listen: what it followed lets go, what it follows now is looked at afresh. Back on the
    // notes, the keys held (by a finger, the pedal or Hold) play their chords again, oldest first,
    // so Bloom and Ground agree; then the harmony's or the tonic's chord goes, after, so the notes
    // they share carry on.
    if (p_.bloom.listen != was.bloom.listen) {
        switch (p_.bloom.listen) {
            case LI_NOTES:
                for (uint64_t after = 0;;) {
                    int k = -1;
                    for (int j = 0; j < 128; ++j)
                        if (key_[j] != K_UP && keyAge_[j] > after && (k < 0 || keyAge_[j] < keyAge_[k])) k = j;
                    if (k < 0) break;
                    after = keyAge_[k];
                    playKey(k, true);
                }
                bloom_.release(-1);
                break;
            case LI_HARMONY: bloomSync_ = true; break;
            default: freePlayed_ = Chord{}; break;
        }
    }

    // On Stop changed while a fade is under way: Keep turns it round, as a note-on does (Cut, once
    // the routes are done: setPatch() resets).
    if (fading_ && p_.onStop == OS_KEEP) fading_ = false;
}

// --- keys ---------------------------------------------------------------------------------------

void Engine::noteOn(int note, int velocity) {
    if (note < 0 || note > 127) return;
    if (velocity <= 0) {
        noteOff(note);
        return;
    }
    const int mapped = mapInput(p_.harmony, note);
    if (mapped < 0) return;
    const float vel = static_cast<float>(std::min(velocity, 127)) / 127.0f;
    awake_ = true;
    fading_ = false;   // a fade under way turns round

    // Air takes the player's notes only while it can be heard (its level above 0, not muted): an Air
    // that is off isn't rendered, and notes struck into it would ring on from where they stood when
    // it was turned up, long after they were played (and keep the meter's voices up meanwhile).
    const bool airOn = !p_.air.mute && p_.air.level > 0.0f;

    // Split: a key at or above it plays Air alone (its mapped note), whatever Air's Listen. It wakes
    // the engine, as any note does (Free strata start), but enters nothing else: not the harmony,
    // Bloom, Ground or Weather's notes. It is never held, so its note-off finds nothing to let go.
    if (p_.split >= 0 && note >= p_.split) {
        if (airOn) air_.play(mapped, vel);
        route();
        return;
    }
    lastVel_ = vel;

    // Hold: a note-on with no other finger on a key starts a new chord, and the keys Hold latched go
    // once it has started (play() before release(): the notes the two share carry on). With a
    // finger still down, the key joins the chord. Under Chord Off the harmony's chord is the keys
    // held, so for a new chord the latched keys leave the harmony first, or they would stay in it.
    bool fresh = p_.hold;
    for (int k = 0; k < 128 && fresh; ++k) fresh = k == note || key_[k] != K_DOWN;
    const bool latchedOut = fresh && p_.harmony.chord == CH_OFF;
    if (latchedOut)
        for (int k = 0; k < 128; ++k)
            if (k != note && key_[k] == K_HOLD) harmony_.noteOff(k);
    // The same key again (held by the pedal or Hold, or a second note-on): the harmony hears it go
    // down afresh, mapped as now.
    const bool again = key_[note] != K_UP;
    if (again) harmony_.noteOff(note);
    makeRoom(latchedOut);
    harmony_.noteOn(note, mapped);
    key_[note] = K_DOWN;
    mapped_[note] = mapped;
    keyVel_[note] = vel;
    keyAge_[note] = ++keysPressed_;
    keysDirty_ = true;

    if (p_.bloom.listen == LI_NOTES) playKey(note, again);
    // Air on the notes plays every key, unless Split is on: then only the keys above it (above).
    if (airOn && p_.air.listen == LI_NOTES && p_.split < 0) air_.play(mapped, vel);
    if (fresh) releaseLatched(note);
    route();
}

void Engine::noteOff(int note) {
    if (note < 0 || note > 127 || key_[note] != K_DOWN) return;
    if (p_.hold) {
        key_[note] = K_HOLD;
    } else if (pedal_) {
        key_[note] = K_PEDAL;
    } else {
        letGo(note);
        route();
    }
}

// Bloom (Notes) plays the key's chord: the chord on its mapped note, led from the chord before when
// Leading is on (with Chord Off the single note). A key whose chord comes out otherwise than it
// last played (the patch changed) lets the old one go first, or its notes would stay with the key.
void Engine::playKey(int key, bool again) {
    const HarmonyPatch& h = p_.harmony;
    const Chord built = buildChord(h, mapped_[key]);
    const Chord c = h.leading ? leadFrom(h, prev_, built) : built;
    if (again && !sameNotes(c, keyChord_[key])) bloom_.release(key);
    bloom_.play(c, key, keyVel_[key]);
    keyChord_[key] = c;
    prev_ = c;
}

void Engine::releaseLatched(int except) {
    for (int k = 0; k < 128; ++k)
        if (k != except && key_[k] == K_HOLD) letGo(k);
}

// The harmony keeps Harmony::kHeldMax keys. Full, the oldest key the pedal or Hold keeps goes, so a
// pedal held down through a long phrase doesn't freeze the harmony; with only fingers on keys, a
// new one isn't heard there (Bloom still plays it). latchedOut: the latched keys have left the
// harmony already, and letting one go makes no room.
void Engine::makeRoom(bool latchedOut) {
    if (harmony_.held() < Harmony::kHeldMax) return;
    int oldest = -1;
    for (int k = 0; k < 128; ++k)
        if ((key_[k] == K_PEDAL || (key_[k] == K_HOLD && !latchedOut)) && (oldest < 0 || keyAge_[k] < keyAge_[oldest]))
            oldest = k;
    if (oldest >= 0) letGo(oldest);
}

void Engine::letGo(int key) {
    bloom_.release(key);   // in Harmony and Free modes Bloom has no notes of a key's: nothing
    harmony_.noteOff(key);
    key_[key] = K_UP;
    keysDirty_ = true;
}

void Engine::sustain(bool down) {
    pedal_ = down;
    if (down) return;
    for (int k = 0; k < 128; ++k)
        if (key_[k] == K_PEDAL) letGo(k);
    route();
}

void Engine::allNotesOff() {
    for (int k = 0; k < 128; ++k)
        if (key_[k] != K_UP) letGo(k);
    route();
}

// --- routes -------------------------------------------------------------------------------------

void Engine::route() {
    int g = -1;
    if (awake_) {
        switch (p_.ground.listen) {
            case LI_NOTES: g = harmony_.lowestHeld(); break;
            case LI_HARMONY: g = harmony_.current().root; break;
            default: g = kTonicNote + p_.harmony.key; break;
        }
    }
    if (g != groundWant_) {
        groundWant_ = g;
        ground_.setTarget(g);
    }
    routeAir();
    routeWeather();
    if (!awake_) return;
    if (p_.bloom.listen == LI_HARMONY) {
        if (bloomSync_ || harmony_.version() != bloomVersion_) {
            bloomSync_ = false;
            bloomVersion_ = harmony_.version();
            const Chord& c = harmony_.current();
            if (c.root < 0) bloom_.releaseAll();
            else bloom_.moveTo(c, lastVel_);
        }
    } else if (p_.bloom.listen == LI_FREE) {
        if (!sameNotes(freeChord_, freePlayed_)) {
            freePlayed_ = freeChord_;
            bloom_.moveTo(freeChord_, lastVel_);
        }
    }
}

// Air's chord and whether it generates. Notes: the harmony's chord (what a loop's replays move to),
// no generating; Air plays the keys themselves. Harmony: the harmony's chord, generating while there
// is one (a forgotten harmony: AirGen keeps its tones, and stops). Free: the tonic chord, generating
// from the first note. Asleep: nothing. Given again only when it changes, and only while Air can be
// heard (its level above 0, not muted): every chord change works the generator's candidates and
// motifs out again (a re-strike of six keys under Chord Off is twelve of them), for nothing while
// Air is off, as in Init. Turned up, it is given the chord there is then, if that isn't the one it
// was last given.
void Engine::routeAir() {
    if (p_.air.mute || !(p_.air.level > 0.0f)) return;
    Chord c;
    bool gen = false;
    if (awake_) {
        switch (p_.air.listen) {
            case LI_NOTES: c = harmony_.current(); break;
            case LI_HARMONY:
                c = harmony_.current();
                gen = c.root >= 0;
                break;
            default:
                c = freeChord_;
                gen = true;
                break;
        }
    }
    if (airSync_ || gen != airGenerate_ || c.root != airChord_.root || c.pcs != airChord_.pcs || !sameNotes(c, airChord_)) {
        airSync_ = false;
        airChord_ = c;
        airGenerate_ = gen;
        air_.setChord(c, gen);
    }
}

// Weather's gate and To Key's chord. Notes: open while any key is held (a finger, the pedal or Hold;
// not a key above Split, which is Air's), on the held notes' pitch classes. Harmony: open while the
// harmony has a chord, on its pitch classes. Free: open from the first note until Stop puts the
// engine to sleep, on the tonic chord's. Shut, Weather keeps the chord it had while its gate fades.
// The gate is given only when it changes: a gate(false) on a silent Weather closes it at once
// (weather.h), and the engine keeps Weather's own state.
void Engine::routeWeather() {
    bool want = false;
    uint16_t pcs = weatherPcs_;
    if (awake_) {
        switch (p_.weather.listen) {
            case LI_NOTES:
                if (keysDirty_) {
                    keysDirty_ = false;
                    heldPcs_ = 0;
                    anyHeld_ = false;
                    for (int k = 0; k < 128; ++k)
                        if (key_[k] != K_UP) {
                            anyHeld_ = true;
                            heldPcs_ = static_cast<uint16_t>(heldPcs_ | 1u << (mapped_[k] % 12));
                        }
                }
                want = anyHeld_;
                pcs = heldPcs_;
                break;
            case LI_HARMONY: {
                const Chord& c = harmony_.current();
                want = c.root >= 0;
                if (want) pcs = c.pcs;
                break;
            }
            default:
                want = true;
                pcs = freeChord_.pcs;
                break;
        }
    }
    if (want && pcs != weatherPcs_) {
        weatherPcs_ = pcs;
        weather_.setChord(pcs);
    }
    if (want != weatherGate_) {
        weatherGate_ = want;
        weather_.gate(want);
    }
}

// --- Stop, suspend, reset -----------------------------------------------------------------------

void Engine::setTransport(double bpm, double beats, bool playing, bool beatsValid) {
    const bool was = transport_.playing;
    // The strata's count moves on to this block's first sample at the tempo it ran at, then takes
    // MPC's position if MPC plays.
    beats_ = beatsAt(samples_);
    beatsAt_ = samples_;
    transport_.bpm = std::isfinite(bpm) && bpm >= 1.0 ? bpm : 120.0;
    transport_.valid = beatsValid && std::isfinite(beats);
    transport_.beats = transport_.valid ? beats : 0.0;
    transport_.playing = playing;
    if (transport_.playing && transport_.valid) beats_ = transport_.beats;
    space_.set(spaceParams_, transport_);
    if (transport_.bpm != echoBpm_) {   // of the transport the Delay reads only the tempo
        echo_.set(p_.echo, transport_);
        echoBpm_ = transport_.bpm;
    }
    if (was && !playing) stop();
    else if (!was && playing) fading_ = false;   // playing again: a fade turns round
}

void Engine::stop() {
    if (!awake_) return;   // nothing sounds
    if (p_.onStop == OS_CUT) sleep(false);
    else if (p_.onStop == OS_FADE) fading_ = true;
}

void Engine::suspend() { suspended_ = true; }

void Engine::resume(double awayS) {
    if (!suspended_) return;
    suspended_ = false;
    if (awayS >= 0.0 && awayS <= kStopWindowS) stop();
    else sleep(false);   // NaN too
}

// Every DSP state silent and empty: the strata, Echo, Space, the output's filters; with
// `restartMemory` (the guard, CC 120) Memory's ring recording starts afresh too, without it (a
// sleep that keeps the ring) Memory seals it: the sound it cut off fades out there, and what the
// wake records fades in (memory.h: Sleeps). What Memory remembered stays either way (plan
// decision 10: only a new Remember replaces it).
void Engine::clearDsp(bool restartMemory) {
    ground_.reset();
    bloom_.reset();
    air_.reset();
    weather_.reset();
    echo_.reset();
    if (restartMemory) memory_.reset();
    else memory_.seal();
    space_.reset();
    tiltS_[0] = tiltS_[1] = 0.0f;
    limitD_ = 0.0f;
    limiting_ = false;
    groundWant_ = -2;   // nothing given: the next route() gives Ground its target again
    bloomSync_ = true;
    freePlayed_ = Chord{};
    airSync_ = true;
    weatherGate_ = false;   // Weather's reset closed it: the next route() opens it if Listen wants it
}

// The output's state where it settles with nothing to hear: the glides at their targets, the
// filters empty, the limiter at 1.
void Engine::settle() {
    tiltS_[0] = tiltS_[1] = 0.0f;
    limitD_ = 0.0f;
    limiting_ = false;
    for (Glide* g : {&volume_, &ret_, &gPanL_, &gPanR_, &bPanL_, &bPanR_, &gEcho_, &bEcho_, &echoRet_, &echoSpace_, &fade_})
        g->land();
}

void Engine::reset() { sleep(true); }

// Silence now, the harmony and the keys forgotten, asleep: CC 120 and reset() (restartMemory: the
// ring recording starts afresh), Stop's Cut, the end of its Fade and a long suspend (the last 16 s
// stay recorded, so Remember after a Stop still finds what was played: the author's decision;
// sealed where the sleep cut them, clearDsp()).
void Engine::sleep(bool restartMemory) {
    clearDsp(restartMemory);
    harmony_.clear();
    std::fill(key_, key_ + 128, K_UP);
    keysDirty_ = true;
    pedal_ = false;
    prev_ = Chord{};
    awake_ = false;
    fading_ = false;
    fadeDb_ = 0.0f;
    fade_.target = 1.0f;
    groundWant_ = -1;   // Ground's own after its reset
    bloomVersion_ = harmony_.version();
    bloomSync_ = false;
    settle();
    tiltNow_ = tilt_;
    tiltFor(tiltNow_);
    idle_ = true;
}

// --- rendering ----------------------------------------------------------------------------------

// The shelf's gains and its one-pole for t (svf.h's TiltShelf, which Weather shares).
void Engine::tiltFor(float t) {
    const TiltShelf s = TiltShelf::of(t);
    tiltHigh_ = s.high;
    tiltLow_ = s.low;
    tiltG_ = s.G;
}

// Memory's ring recording becomes the remembered source. Weather, if it reads Memory, sees the new
// source at its next render(), before Memory writes into the ring it leaves (the old remembered
// one): its fading grains copy what they still read from it there (weather.h).
void Engine::applyRemember() {
    rememberAsked_ = false;
    memory_.remember();
}

void Engine::control() {
    // Remember at the control step's start, before the piece's strata: Weather (on Memory) renders
    // next, sees the new source and copies what its fading grains still read from the old ring; only
    // then does Memory write (after Weather in piece()), recording over that ring. Applied after Weather had
    // rendered, the copies could be taken from frames already recorded over: a click (memory.h).
    if (rememberAsked_) applyRemember();
    harmony_.advance(static_cast<double>(kChunk) / kRate, transport_.bpm);
    route();
    if (fading_) {
        fadeDb_ -= -kFloorDb * static_cast<float>(kChunk) / (kFadeS * kRate);
        if (fadeDb_ <= kFloorDb) {
            sleep(false);
            return;
        }
    } else if (fadeDb_ < 0.0f) {
        fadeDb_ = std::min(0.0f, fadeDb_ + kRecoverDbPerS * static_cast<float>(kChunk) / kRate);
    }
    fade_.target = fadeDb_ < 0.0f ? dbToGain(fadeDb_) : 1.0f;
    fade_.aim(kChunk);
    for (Glide* g : {&volume_, &ret_, &gPanL_, &gPanR_, &bPanL_, &bPanR_, &gEcho_, &bEcho_, &echoRet_, &echoSpace_})
        g->aim(kGlideSamples);
    if (tiltNow_ != tilt_) {
        tiltNow_ = std::fabs(tilt_ - tiltNow_) <= kTiltStep ? tilt_ : tiltNow_ + std::copysign(kTiltStep, tilt_ - tiltNow_);
        tiltFor(tiltNow_);
    }
}

// The control steps sit on the sample count's multiples of kChunk, asleep or awake, so the host's
// blocks (MPC's 128, or any multiple of kChunk) never cut one: only a MIDI event does, at its
// sample, and the same events play the same samples whatever the block size.
void Engine::render(float* outL, float* outR, int n) {
    if (n <= 0) return;
    // Asleep no control step runs and nothing is recorded: a Remember applies at once (after a Stop
    // it finds the last 16 s still recorded).
    if (rememberAsked_ && !awake_) applyRemember();
    int o = 0;
    while (awake_ && o < n) {
        const int phase = static_cast<int>((samples_ + static_cast<uint64_t>(o)) % kChunk);
        if (phase == 0) {
            control();
            if (!awake_) break;   // the fade just ended
        }
        const int m = std::min(n - o, kChunk - phase);
        clockStrata(samples_ + static_cast<uint64_t>(o));
        if (!piece(outL + o, outR + o, m, samples_ + static_cast<uint64_t>(o))) {
            // Not finite: the whole call is zeros and the DSP starts afresh.
            ++guards_;
            g_guardTrips.fetch_add(1, std::memory_order_relaxed);
            clearDsp(true);
            o = 0;
            break;
        }
        o += m;
    }
    if (o < n) {   // asleep (from the start, or from where a fade ended), or the guard
        std::memset(outL + o, 0, sizeof(float) * static_cast<size_t>(n - o));
        std::memset(outR + o, 0, sizeof(float) * static_cast<size_t>(n - o));
        if (!awake_) idle_ = true;
    }
    samples_ += static_cast<uint64_t>(n);
}

// The strata's synced cycles (Breath, the sways) run on one beat count, the engine's: MPC's position
// at the block's first sample while it plays, and on from where it was at the tempo while it is
// stopped; moved on at the tempo by the samples since. It runs on the sample count, asleep or awake,
// so every stratum, sounding or silent, finds it where the grid is: a stratum's own clock only moved
// while it was rendered. A tempo change applies from the block that brings it.
double Engine::beatsAt(uint64_t at) const {
    const double since = static_cast<double>(static_cast<int64_t>(at - beatsAt_));
    return beats_ + since * transport_.bpm / (60.0 * static_cast<double>(kRate));
}

// Each piece's first sample: the strata's clocks set to the count there.
void Engine::clockStrata(uint64_t at) {
    const double beats = beatsAt(at);
    ground_.setTransport(transport_.bpm, beats);
    bloom_.setTransport(transport_.bpm, beats);
}

// One piece of n <= kChunk samples from sample `at`, all within one control step: the strata into
// the dry bus, the Space sends and the Echo bus; Echo; Space; Memory's tap; the output. False if a
// sample came out that isn't finite.
bool Engine::piece(float* L, float* R, int n, uint64_t at) {
    // What may sound: Air while a voice rings or its generator may strike; Echo while a stratum
    // that may sound sends to it, or it isn't silent; Space while anything sends to it. Air's send
    // and Echo's into Space are looked at once they have run (below): Air's generator may strike
    // nothing, and Space needn't run for it.
    const auto sending = [](const Glide& s) { return !(s.still() && s.now == 0.0f); };
    const bool g = ground_.audible(), b = bloom_.active() > 0;
    const bool a = air_.audible() || air_.generating(), w = weather_.audible();
    const bool echo = (g && sending(gEcho_)) || (b && sending(bEcho_)) || (a && air_.echoing()) ||
                      (w && weather_.echoing()) || !echo_.silent() || poisonEcho_;
    const bool spaceIn = (g && groundSend_ > 0.0f) || (b && bloomSend_ > 0.0f) || (w && weatherSend_ > 0.0f) ||
                         !space_.silent() || poison_;
    const size_t bytes = sizeof(float) * static_cast<size_t>(n);
    std::memset(L, 0, bytes);
    std::memset(R, 0, bytes);
    if (!g && !b && !a && !w && !echo && !spaceIn) {   // nothing to hear: no DSP
        idle_ = true;
        settle();
        return true;
    }
    idle_ = false;
    StageClock clock;
    std::memset(sendL_, 0, bytes);
    std::memset(sendR_, 0, bytes);
    bool echoBus = false, echoIn = false;   // the Echo bus zeroed this piece; something sent into it
    const auto openEcho = [&] {
        if (echoBus) return;
        std::memset(echoL_, 0, bytes);
        std::memset(echoR_, 0, bytes);
        echoBus = true;
    };

    // Ground and Bloom: centred and sending nothing to Echo, straight into the bus. Panned, sending
    // to Echo, or wanted apart (Bloom's dry for Weather's Duck), into their own buffer first: then
    // into the bus at the pan (0.0.2's loop, a sample at a time, so a panned stratum is bit for bit
    // what it was), and into the Echo bus at the send (the dry before its pan, decision 2). One not
    // rendered has its pan and send where they go.
    const auto mix = [&](bool on, Glide& pl, Glide& pr, Glide& es, float* xl, float* xr, bool apart, auto&& draw) {
        if (!on) {
            pl.land();
            pr.land();
            es.land();
            return;
        }
        const bool toEcho = sending(es);
        const bool centred = pl.still() && pr.still() && pl.now == 1.0f && pr.now == 1.0f;
        if (!toEcho && !apart && centred) {
            draw(L, R);
            return;
        }
        std::memset(xl, 0, bytes);
        std::memset(xr, 0, bytes);
        draw(xl, xr);
        if (centred) {   // what a gain of 1 gives, bit for bit
            for (int i = 0; i < n; ++i) {
                L[i] += xl[i];
                R[i] += xr[i];
            }
        } else {
            for (int i = 0; i < n; ++i) {
                L[i] += xl[i] * pl.next();
                R[i] += xr[i] * pr.next();
            }
        }
        if (!toEcho) return;
        openEcho();
        if (es.still()) {
            const float e = es.now;
            for (int i = 0; i < n; ++i) {
                echoL_[i] += xl[i] * e;
                echoR_[i] += xr[i] * e;
            }
        } else {
            for (int i = 0; i < n; ++i) {
                const float e = es.next();
                echoL_[i] += xl[i] * e;
                echoR_[i] += xr[i] * e;
            }
        }
        echoIn = true;
    };
    mix(g, gPanL_, gPanR_, gEcho_, gL_, gR_, false,
        [&](float* xl, float* xr) { ground_.render(tables_, xl, xr, sendL_, sendR_, groundSend_, n); });
    clock.lap(STG_GROUND);
    const bool duck = w && p_.weather.duck > 0.0f;   // Weather hears Bloom's dry peak
    mix(b, bPanL_, bPanR_, bEcho_, bL_, bR_, duck,
        [&](float* xl, float* xr) { bloom_.render(tables_, xl, xr, sendL_, sendR_, bloomSend_, n); });
    float bloomPeak = 0.0f;
    if (duck && b)
        for (int i = 0; i < n; ++i) bloomPeak = std::max(bloomPeak, std::max(std::fabs(bL_[i]), std::fabs(bR_[i])));
    clock.lap(STG_BLOOM);

    // Air and Weather each in one pass over their own output (air.h, weather.h: setMix()): the dry at
    // their level and pan straight into the bus, their Space and Echo sends before the pan. Air is
    // given the strata's beat count only when it renders (skipped, its clock stands).
    bool airSounded = false;
    if (a) {
        air_.setTransport(transport_.bpm, beatsAt(at), true);   // the one count, locked always (airgen.h)
        const bool toEcho = air_.echoing();
        if (toEcho) openEcho();
        airSounded = air_.render(tables_, L, R, sendL_, sendR_, airSend_, toEcho ? echoL_ : nullptr,
                                 toEcho ? echoR_ : nullptr, n);
        echoIn = echoIn || (toEcho && airSounded);
    }
    clock.lap(STG_AIR);
    if (w) {
        // Memory's remembered 16 s, or the plugin's source (holdsSource()).
        const GrainSource* src = p_.weather.memory ? memory_.remembered() : weatherSrc_;
        weatherHolds_ = !p_.weather.memory && src && src->ready();
        const bool toEcho = weather_.echoing();
        if (toEcho) openEcho();
        weather_.render(src, bloomPeak, L, R, sendL_, sendR_, weatherSend_, toEcho ? echoL_ : nullptr,
                        toEcho ? echoR_ : nullptr, n);
        echoIn = echoIn || toEcho;
    }
    clock.lap(STG_WEATHER);
    // Memory's tap: the strata's dry here (after Weather has rendered, and taken its copies of the
    // old ring if a Remember came at this step's start), or the dry and the returns before the tilt
    // (below). A sample that isn't finite Memory takes as 0, and the guard then starts its ring afresh.
    if (p_.memoryTap == MT_STRATA) {
        memory_.write(L, R, n);
        clock.lap(STG_OUT);
    }

    // Echo: its return into the mix and into Space's send. Skipped (silent, nothing sent), it is told
    // for how long when it runs again, so its duck falls as far as it would have (echo.h).
    bool echoToSpace = false;
    if (echoIn || !echo_.silent() || poisonEcho_) {
        openEcho();
        if (at > echoTo_) echo_.rest(at - echoTo_);
        echo_.process(echoL_, echoR_, echoL_, echoR_, n);   // the wet, over the bus
        echoTo_ = at + static_cast<uint64_t>(n);
        ++echoRuns_;
        if (poisonEcho_) {
            echoL_[0] = std::numeric_limits<float>::quiet_NaN();
            poisonEcho_ = false;
        }
        const auto add = [n](Glide& gain, const float* xl, const float* xr, float* l, float* r) {
            if (gain.still()) {
                const float k = gain.now;
                for (int i = 0; i < n; ++i) {
                    l[i] += k * xl[i];
                    r[i] += k * xr[i];
                }
            } else {
                for (int i = 0; i < n; ++i) {
                    const float k = gain.next();
                    l[i] += k * xl[i];
                    r[i] += k * xr[i];
                }
            }
        };
        add(echoRet_, echoL_, echoR_, L, R);
        echoToSpace = sending(echoSpace_);
        if (echoToSpace) add(echoSpace_, echoL_, echoR_, sendL_, sendR_);
    } else {
        echoRet_.land();   // no wet to glide over
        echoSpace_.land();
    }
    clock.lap(STG_ECHO);

    const bool wet = spaceIn || (airSounded && airSend_ > 0.0f) || echoToSpace;
    if (wet) {
        space_.process(sendL_, sendR_, sendL_, sendR_, n);   // the wet, over the sends
        if (poison_) {
            sendL_[0] = std::numeric_limits<float>::quiet_NaN();
            poison_ = false;
        }
        for (int i = 0; i < n; ++i) {
            const float r = ret_.next();
            L[i] += r * sendL_[i];
            R[i] += r * sendR_[i];
        }
    } else {
        ret_.land();   // no wet to glide over
    }
    clock.lap(STG_SPACE);
    if (p_.memoryTap == MT_OUTPUT) memory_.write(L, R, n);   // Memory last (above)
    const bool finite = output(L, R, n);
    clock.lap(STG_OUT);
    return finite;
}

// Tilt, volume, the guard's look, the limiter, Stop's fade: in place over the piece.
bool Engine::output(float* L, float* R, int n) {
    if (tiltNow_ != 0.0f) {
        const float G = tiltG_, hi = tiltHigh_, d = tiltLow_ - tiltHigh_;
        float* ch[2] = {L, R};
        for (int c = 0; c < 2; ++c) {
            float* x = ch[c];
            float s = tiltS_[c];
            for (int i = 0; i < n; ++i) {
                const float v = (x[i] - s) * G, lp = v + s;
                s = lp + v;
                x[i] = hi * x[i] + d * lp;
            }
            tiltS_[c] = std::fabs(s) < 1e-20f ? 0.0f : s;
        }
    } else {
        tiltS_[0] = tiltS_[1] = 0.0f;
    }
    volume_.apply(L, R, n);

    // One pass for the guard and the limiter: the largest |sample|'s bits.
    uint32_t top = 0;
    for (int i = 0; i < n; ++i) top = std::max(top, std::max(absBits(L[i]), absBits(R[i])));
    if (top >= kInfBits) return false;
    float pk;
    std::memcpy(&pk, &top, sizeof pk);

    // The limiter: the gain computer only where a peak passes the knee or the gain is still down.
    // It runs on the gain's distance under 1 (d, and the wanted one wd): near 1 a float gain can't
    // take the release's small steps, a float distance can.
    limiting_ = pk > kKnee || limitD_ > 0.0f;
    if (limiting_) {
        constexpr float w = kCeiling - kKnee, invW = 1.0f / w;
        float d = limitD_;
        for (int i = 0; i < n; ++i) {
            const float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
            const float wd = a > kKnee ? 1.0f - kKnee / a : 0.0f;
            d += (wd - d) * (wd > d ? kAttack : kRelease);
            const float gain = 1.0f - d;
            float l = L[i] * gain, r = R[i] * gain;
            // What the attack lets through: a soft clip from the knee up to the ceiling.
            if (std::fabs(l) > kKnee) l = std::copysign(kKnee + w * softclip((std::fabs(l) - kKnee) * invW), l);
            if (std::fabs(r) > kKnee) r = std::copysign(kKnee + w * softclip((std::fabs(r) - kKnee) * invW), r);
            L[i] = l;
            R[i] = r;
        }
        // Released all but 0.1% with nothing over the knee: back at 1, the next piece only looks.
        limitD_ = pk <= kKnee && d < kLimitLand ? 0.0f : d;
        g_limited.fetch_add(static_cast<uint32_t>(n), std::memory_order_relaxed);
    }

    fade_.apply(L, R, n);
    return true;
}

// --- what the tests and the meter see ----------------------------------------------------------

int Engine::activeVoices() const {
    return bloom_.active() + air_.voices().active() + (ground_.audible() ? 1 : 0);
}

Engine::Info Engine::info() const {
    Info i;
    i.awake = awake_;
    i.idle = idle_;
    i.fading = fading_;
    i.fadeDb = fadeDb_;
    i.suspended = suspended_;
    i.groundTarget = ground_.target();
    i.groundAudible = ground_.audible();
    i.bloomActive = bloom_.active();
    i.harmonyRoot = harmony_.current().root;
    i.harmonyVersion = harmony_.version();
    i.guards = guards_;
    i.samples = samples_;
    i.limiterGain = 1.0f - limitD_;
    i.limiting = limiting_;
    i.gliding = false;
    for (const Glide* g : {&volume_, &ret_, &gPanL_, &gPanR_, &bPanL_, &bPanR_, &gEcho_, &bEcho_, &echoRet_, &echoSpace_})
        i.gliding = i.gliding || !g->still();
    i.spaceDecayS = spaceParams_.reverb.decayS;
    i.airActive = air_.voices().active();
    i.airStrikes = air_.strikes();
    i.weatherAudible = weather_.audible();
    i.weatherGate = weatherGate_;
    i.echoRuns = echoRuns_;
    i.echoSilent = echo_.silent();
    i.remembered = memory_.remembered() != nullptr;
    i.memoryFill = memory_.fill();
    i.memoryGeneration = memory_.generation();
    return i;
}

uint32_t guardTrips() { return g_guardTrips.load(std::memory_order_relaxed); }
uint32_t limitedSamples() { return g_limited.load(std::memory_order_relaxed); }

} // namespace af
