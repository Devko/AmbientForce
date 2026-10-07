#include "engine.h"

#include "stages.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace af {

#ifdef AF_STAGE_TIMING
uint64_t g_stageNs[STG_COUNT] = {};
#endif

namespace {

constexpr float kTiltPivotHz = 800.0f;
constexpr float kTiltDb = 6.0f;             // the shelf's highs (and, the other way, lows) at tilt 1
constexpr float kTiltStep = 0.029f;         // a control step's tilt glide: the whole range in 50 ms
constexpr float kFloorDb = -60.0f;          // Stop's fade ends here, and resets
constexpr float kLand = 1e-6f;              // a gain this near its target is there
constexpr float kSqrt2 = 1.41421356f;
constexpr int kTonicNote = 48;              // where Free's tonic chord is built (C3 + key)

// Set when the plugin loads (no guard to take on the audio thread). The volume glides 10 ms (a
// one-pole a control step), the limiter's gain 1 ms down and 150 ms back up (a one-pole a sample).
const float kVolumeGlide = 1.0f - std::exp(-static_cast<float>(kChunk) / (0.010f * kRate));
const float kAttack = smoothCoef(0.001f), kRelease = smoothCoef(0.150f);

// The volume knob's gain: off at kVolumeOffDb, at most +12 dB. (common.h's dbToGain is the fast one
// for the DSP; this one is exact and runs only when the patch changes.)
float volumeGain(float db) {
    if (!(db > kVolumeOffDb)) return 0.0f;   // NaN too
    return std::pow(10.0f, (db < 12.0f ? db : 12.0f) / 20.0f);
}

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

// The exponent bits of x: all ones for an infinity or a NaN. Integer work, which no float
// optimisation can fold away, and the block's maximum vectorises.
inline uint32_t exponentOf(float x) {
    uint32_t u;
    std::memcpy(&u, &x, sizeof u);
    return u & 0x7F800000u;
}

// One gain's straight line across a piece of n samples, applied in place (both channels).
void ramp(float* L, float* R, int n, float from, float to) {
    if (from == to) {
        if (from == 1.0f) return;
        for (int i = 0; i < n; ++i) {
            L[i] *= from;
            R[i] *= from;
        }
        return;
    }
    const float step = (to - from) / static_cast<float>(n);
    float g = from;
    for (int i = 0; i < n; ++i) {
        g += step;
        L[i] *= g;
        R[i] *= g;
    }
}

} // namespace

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

// --- the patch ----------------------------------------------------------------------------------

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

    // Space: the tail's hold (Abyss rings four times its Decay already).
    spaceParams_ = p.space;
    float& decay = spaceParams_.reverb.decayS;
    if (p.bloom.tail == TL_SPACE && spaceParams_.reverb.mode != Reverb::ABYSS)
        decay = std::max(decay, clampParam(p.bloom.releaseS, 0.01f, 30.0f, 0.0f));   // Bloom's range
    space_.set(spaceParams_, transport_);

    // The mix: Space's sends only where its return is heard.
    groundSend_ = p.spaceReturn > 0.0f ? p.groundSpace : 0.0f;
    bloomSend_ = p.spaceReturn > 0.0f ? p.bloomSpace : 0.0f;
    ret_.target = p.spaceReturn;
    panGains(p.groundPan, gPanL_.target, gPanR_.target);
    panGains(p.bloomPan, bPanL_.target, bPanR_.target);
    volume_ = volumeGain(p.volumeDb);
    tilt_ = p.tilt;

    // Free's tonic chord: the patch's chord type on the key (a triad under Chord Off).
    HarmonyPatch tonic = p.harmony;
    if (tonic.chord == CH_OFF) tonic.chord = CH_TRIAD;
    freeChord_ = buildChord(tonic, kTonicNote + p.harmony.key);

    // Hold turned off: the latched keys go (to the pedal, while it is down).
    if (was.hold && !p.hold)
        for (int k = 0; k < 128; ++k)
            if (key_[k] == K_HOLD) {
                if (pedal_) key_[k] = K_PEDAL;
                else letGo(k);
            }

    // Bloom's Listen changed: what it followed lets go, what it follows now is looked at afresh.
    if (p.bloom.listen != was.bloom.listen) {
        if (p.bloom.listen == LI_NOTES) bloom_.release(-1);   // the harmony's or the tonic's chord
        if (p.bloom.listen == LI_HARMONY) bloomSync_ = true;
        if (p.bloom.listen == LI_FREE) freePlayed_ = Chord{};
    }
    route();
}

// --- keys ---------------------------------------------------------------------------------------

void Engine::noteOn(int note, int velocity) {
    if (note < 0 || note > 127) return;
    if (velocity <= 0) {
        noteOff(note);
        return;
    }
    const HarmonyPatch& h = p_.harmony;
    const int mapped = mapInput(h, note);
    if (mapped < 0) return;
    const float vel = static_cast<float>(std::min(velocity, 127)) / 127.0f;
    lastVel_ = vel;
    awake_ = true;
    fading_ = false;   // a fade under way turns round

    // Hold: the keys it latched go once the new chord has started. Under Chord Off the harmony's
    // chord is the keys held, so there they leave the harmony first.
    const bool chordOff = h.chord == CH_OFF;
    if (p_.hold && chordOff)
        for (int k = 0; k < 128; ++k)
            if (k != note && key_[k] == K_HOLD) harmony_.noteOff(k);
    // The same key again (held by the pedal or Hold, or a second note-on): the harmony hears it
    // go down afresh, mapped as now.
    const bool again = key_[note] != K_UP;
    if (again) harmony_.noteOff(note);
    harmony_.noteOn(note, mapped);
    key_[note] = K_DOWN;

    if (p_.bloom.listen == LI_NOTES) {
        const Chord built = buildChord(h, mapped);
        const Chord c = h.leading ? leadFrom(h, prev_, built) : built;
        // A key whose chord came out otherwise this time (the patch changed) lets its old one go
        // first, or its notes would stay with the key.
        if (again && !sameNotes(c, keyChord_[note])) bloom_.release(note);
        bloom_.play(c, note, vel);
        keyChord_[note] = c;
        prev_ = c;
    }
    if (p_.hold)
        for (int k = 0; k < 128; ++k)
            if (k != note && key_[k] == K_HOLD) letGo(k);
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
    transport_.bpm = std::isfinite(bpm) && bpm >= 1.0 ? bpm : 120.0;
    transport_.valid = beatsValid && std::isfinite(beats);
    transport_.beats = transport_.valid ? beats : 0.0;
    transport_.playing = playing;
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
    limit_ = 1.0f;
    groundWant_ = -2;   // nothing given: the next route() gives Ground its target again
    bloomSync_ = true;
    freePlayed_ = Chord{};
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
    fade_ = Glide{};
    groundWant_ = -1;   // Ground's own after its reset
    bloomVersion_ = harmony_.version();
    bloomSync_ = false;
    // The output's glides are where they go: the next sound starts there.
    volumeNow_ = volume_;
    ret_.now = ret_.target;
    gPanL_.now = gPanL_.target;
    gPanR_.now = gPanR_.target;
    bPanL_.now = bPanL_.target;
    bPanR_.now = bPanR_.target;
    tiltNow_ = tilt_;
    tiltFor(tiltNow_);
    idle_ = true;
}

// --- rendering ----------------------------------------------------------------------------------

void Engine::tiltFor(float t) {
    tiltHigh_ = exp2Fast(t * kTiltDb * 0.166096404744f);   // log2(10) / 20
    tiltLow_ = 1.0f / tiltHigh_;
    // The pole at pivot x sqrt(high / low), the zero at pivot / that: the shelf's middle (0 dB)
    // on the pivot. A TPT one-pole, so the highs reach tiltHigh_ exactly at Nyquist.
    const float g = svfG(kTiltPivotHz * tiltHigh_);
    tiltG_ = g / (1.0f + g);
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
        if (!piece(outL + o, outR + o, m)) {
            // Not finite: the whole call is zeros and the DSP starts afresh.
            ++guards_;
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

// One piece of n <= kChunk samples, all within one control step: the strata into the dry bus and
// the sends, Space, the output. False if a sample came out that isn't finite.
bool Engine::piece(float* L, float* R, int n) {
    const bool g = ground_.audible(), b = bloom_.active() > 0;
    const bool wet = (g && groundSend_ > 0.0f) || (b && bloomSend_ > 0.0f) || !space_.silent() || poison_;
    const size_t bytes = sizeof(float) * static_cast<size_t>(n);
    std::memset(L, 0, bytes);
    std::memset(R, 0, bytes);
    if (!g && !b && !wet) {   // nothing to hear: no DSP, and the output's state where it settles
        idle_ = true;
        tiltS_[0] = tiltS_[1] = 0.0f;
        limit_ = 1.0f;
        volumeNow_ = volume_;
        fade_.now = fade_.target;
        ret_.now = ret_.target;
        gPanL_.now = gPanL_.target;
        gPanR_.now = gPanR_.target;
        bPanL_.now = bPanL_.target;
        bPanR_.now = bPanR_.target;
        return true;
    }
    idle_ = false;
    StageClock clock;
    std::memset(sendL_, 0, bytes);
    std::memset(sendR_, 0, bytes);

    // A stratum in the middle renders straight into the bus; panned, into its own buffer first.
    const auto pan = [n, bytes, L, R](Glide& pl, Glide& pr, float* xl, float* xr, auto&& draw) {
        if (pl.now == 1.0f && pl.target == 1.0f && pr.now == 1.0f && pr.target == 1.0f) {
            draw(L, R);
            return;
        }
        std::memset(xl, 0, bytes);
        std::memset(xr, 0, bytes);
        draw(xl, xr);
        const float sl = (pl.target - pl.now) / static_cast<float>(n), sr = (pr.target - pr.now) / static_cast<float>(n);
        float gl = pl.now, gr = pr.now;
        for (int i = 0; i < n; ++i) {
            gl += sl;
            gr += sr;
            L[i] += xl[i] * gl;
            R[i] += xr[i] * gr;
        }
        pl.now = pl.target;
        pr.now = pr.target;
    };
    if (g)
        pan(gPanL_, gPanR_, gL_, gR_,
            [&](float* xl, float* xr) { ground_.render(tables_, xl, xr, sendL_, sendR_, groundSend_, n); });
    clock.lap(STG_GROUND);
    if (b)
        pan(bPanL_, bPanR_, bL_, bR_,
            [&](float* xl, float* xr) { bloom_.render(tables_, xl, xr, sendL_, sendR_, bloomSend_, n); });
    clock.lap(STG_BLOOM);

    if (wet) {
        space_.process(sendL_, sendR_, sendL_, sendR_, n);   // the wet, over the sends
        if (poison_) {
            sendL_[0] = std::numeric_limits<float>::quiet_NaN();
            poison_ = false;
        }
        const float step = (ret_.target - ret_.now) / static_cast<float>(n);
        float r = ret_.now;
        for (int i = 0; i < n; ++i) {
            r += step;
            L[i] += r * sendL_[i];
            R[i] += r * sendR_[i];
        }
        ret_.now = ret_.target;
    } else {
        ret_.now = ret_.target;
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

    const float v0 = volumeNow_;
    float v1 = v0 + (volume_ - v0) * kVolumeGlide;
    if (std::fabs(volume_ - v1) < kLand) v1 = volume_;
    ramp(L, R, n, v0, v1);
    volumeNow_ = v1;

    uint32_t e = 0;
    for (int i = 0; i < n; ++i) e = std::max(e, std::max(exponentOf(L[i]), exponentOf(R[i])));
    if (e == 0x7F800000u) return false;

    // The limiter: the gain computer only where a peak passes the knee or the gain is still down.
    float pk = 0.0f;
    for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
    if (pk > kKnee || limit_ < 1.0f) {
        constexpr float w = kCeiling - kKnee, invW = 1.0f / w;
        float gain = limit_;
        for (int i = 0; i < n; ++i) {
            const float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
            const float want = a > kKnee ? kKnee / a : 1.0f;
            gain += (want - gain) * (want < gain ? kAttack : kRelease);
            float l = L[i] * gain, r = R[i] * gain;
            // What the attack lets through: a soft clip from the knee up to the ceiling.
            if (std::fabs(l) > kKnee) l = std::copysign(kKnee + w * softclip((std::fabs(l) - kKnee) * invW), l);
            if (std::fabs(r) > kKnee) r = std::copysign(kKnee + w * softclip((std::fabs(r) - kKnee) * invW), r);
            L[i] = l;
            R[i] = r;
        }
        limit_ = 1.0f - gain < kLand ? 1.0f : gain;
    }

    ramp(L, R, n, fade_.now, fade_.target);
    fade_.now = fade_.target;
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
    i.limiterGain = limit_;
    i.spaceDecayS = spaceParams_.reverb.decayS;
    return i;
}

} // namespace af
