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
    groundWant_ = -2;
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
    const Patch was = p_;
    p_ = p;

    harmony_.set(p.harmony);
    ground_.set(p.ground, p.harmony);
    bloom_.set(p.bloom, p.harmony);

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
    ret_.target = p.spaceReturn;
    panGains(p.groundPan, gPanL_.target, gPanR_.target);
    panGains(p.bloomPan, bPanL_.target, bPanR_.target);
    volume_.target = volumeGain(p.volumeDb) * kMakeUp;
    tilt_ = p.tilt;

    // Free's tonic chord: the patch's chord type on the key (a triad under Chord Off).
    HarmonyPatch tonic = p.harmony;
    if (tonic.chord == CH_OFF) tonic.chord = CH_TRIAD;
    freeChord_ = buildChord(tonic, kTonicNote + p.harmony.key);

    changed(was);
    route();
    if (fading_ && p.onStop == OS_CUT) reset();
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
    lastVel_ = vel;
    awake_ = true;
    fading_ = false;   // a fade under way turns round

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

    if (p_.bloom.listen == LI_NOTES) playKey(note, again);
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
    if (was && !playing) stop();
    else if (!was && playing) fading_ = false;   // playing again: a fade turns round
}

void Engine::stop() {
    if (!awake_) return;   // nothing sounds
    if (p_.onStop == OS_CUT) reset();
    else if (p_.onStop == OS_FADE) fading_ = true;
}

void Engine::suspend() { suspended_ = true; }

void Engine::resume(double awayS) {
    if (!suspended_) return;
    suspended_ = false;
    if (awayS >= 0.0 && awayS <= kStopWindowS) stop();
    else reset();   // NaN too
}

void Engine::clearDsp() {
    ground_.reset();
    bloom_.reset();
    space_.reset();
    tiltS_[0] = tiltS_[1] = 0.0f;
    limitD_ = 0.0f;
    limiting_ = false;
    groundWant_ = -2;   // nothing given: the next route() gives Ground its target again
    bloomSync_ = true;
    freePlayed_ = Chord{};
}

// The output's state where it settles with nothing to hear: the glides at their targets, the
// filters empty, the limiter at 1.
void Engine::settle() {
    tiltS_[0] = tiltS_[1] = 0.0f;
    limitD_ = 0.0f;
    limiting_ = false;
    for (Glide* g : {&volume_, &ret_, &gPanL_, &gPanR_, &bPanL_, &bPanR_, &fade_}) g->land();
}

void Engine::reset() {
    clearDsp();
    harmony_.clear();
    std::fill(key_, key_ + 128, K_UP);
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

void Engine::control() {
    harmony_.advance(static_cast<double>(kChunk) / kRate, transport_.bpm);
    route();
    if (fading_) {
        fadeDb_ -= -kFloorDb * static_cast<float>(kChunk) / (kFadeS * kRate);
        if (fadeDb_ <= kFloorDb) {
            reset();
            return;
        }
    } else if (fadeDb_ < 0.0f) {
        fadeDb_ = std::min(0.0f, fadeDb_ + kRecoverDbPerS * static_cast<float>(kChunk) / kRate);
    }
    fade_.target = fadeDb_ < 0.0f ? dbToGain(fadeDb_) : 1.0f;
    fade_.aim(kChunk);
    for (Glide* g : {&volume_, &ret_, &gPanL_, &gPanR_, &bPanL_, &bPanR_}) g->aim(kGlideSamples);
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
    int o = 0;
    while (awake_ && o < n) {
        const int phase = static_cast<int>((samples_ + static_cast<uint64_t>(o)) % kChunk);
        if (phase == 0) {
            control();
            if (!awake_) break;   // the fade just ended
        }
        const int m = std::min(n - o, kChunk - phase);
        clockStrata(samples_ + static_cast<uint64_t>(o));
        if (!piece(outL + o, outR + o, m)) {
            // Not finite: the whole call is zeros and the DSP starts afresh.
            ++guards_;
            g_guardTrips.fetch_add(1, std::memory_order_relaxed);
            clearDsp();
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

// One piece of n <= kChunk samples, all within one control step: the strata into the dry bus and
// the sends, Space, the output. False if a sample came out that isn't finite.
bool Engine::piece(float* L, float* R, int n) {
    const bool g = ground_.audible(), b = bloom_.active() > 0;
    const bool wet = (g && groundSend_ > 0.0f) || (b && bloomSend_ > 0.0f) || !space_.silent() || poison_;
    const size_t bytes = sizeof(float) * static_cast<size_t>(n);
    std::memset(L, 0, bytes);
    std::memset(R, 0, bytes);
    if (!g && !b && !wet) {   // nothing to hear: no DSP
        idle_ = true;
        settle();
        return true;
    }
    idle_ = false;
    StageClock clock;
    std::memset(sendL_, 0, bytes);
    std::memset(sendR_, 0, bytes);

    // A stratum in the middle renders straight into the bus; panned, into its own buffer first.
    // One not rendered has its pan where it goes.
    const auto pan = [n, bytes, L, R](bool on, Glide& pl, Glide& pr, float* xl, float* xr, auto&& draw) {
        if (!on) {
            pl.land();
            pr.land();
        } else if (pl.still() && pr.still() && pl.now == 1.0f && pr.now == 1.0f) {
            draw(L, R);
        } else {
            std::memset(xl, 0, bytes);
            std::memset(xr, 0, bytes);
            draw(xl, xr);
            for (int i = 0; i < n; ++i) {
                L[i] += xl[i] * pl.next();
                R[i] += xr[i] * pr.next();
            }
        }
    };
    pan(g, gPanL_, gPanR_, gL_, gR_,
        [&](float* xl, float* xr) { ground_.render(tables_, xl, xr, sendL_, sendR_, groundSend_, n); });
    clock.lap(STG_GROUND);
    pan(b, bPanL_, bPanR_, bL_, bR_,
        [&](float* xl, float* xr) { bloom_.render(tables_, xl, xr, sendL_, sendR_, bloomSend_, n); });
    clock.lap(STG_BLOOM);

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

int Engine::activeVoices() const { return bloom_.active() + (ground_.audible() ? 1 : 0); }

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
    i.gliding = !(volume_.still() && ret_.still() && gPanL_.still() && gPanR_.still() && bPanL_.still() && bPanR_.still());
    i.spaceDecayS = spaceParams_.reverb.decayS;
    return i;
}

uint32_t guardTrips() { return g_guardTrips.load(std::memory_order_relaxed); }
uint32_t limitedSamples() { return g_limited.load(std::memory_order_relaxed); }

} // namespace af
