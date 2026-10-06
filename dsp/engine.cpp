#include "engine.h"

#include "stages.h"

#include <cmath>
#include <cstring>

namespace af {

#ifdef AF_STAGE_TIMING
uint64_t g_stageNs[STG_COUNT] = {};
#endif

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kAttackS = 0.010f;
constexpr float kReleaseS = 0.300f;
constexpr float kVoiceGain = 0.25f;   // a voice at full velocity, -12 dBFS at 0 dB volume: six of them
                                      // pass full scale only where their peaks line up
constexpr float kGlideS = 0.010f;     // the volume's smoothing: a turn of the knob never clicks

float dbToGain(float db) { return db <= -59.5f ? 0.0f : std::pow(10.0f, (db < 12.0f ? db : 12.0f) / 20.0f); }

} // namespace

Engine::Engine(float sampleRate)
    : sr_(sampleRate > 0.0f ? sampleRate : 44100.0f),
      attackStep_(1.0f / (kAttackS * sr_)),
      releaseStep_(1.0f / (kReleaseS * sr_)),
      glide_(1.0f - std::exp(-1.0f / (kGlideS * sr_))) {
    setPatch(Patch{});
    gain_ = target_;
}

void Engine::setPatch(const Patch& p) { target_ = std::isfinite(p.volumeDb) ? dbToGain(p.volumeDb) : 0.0f; }

void Engine::noteOn(int note, int velocity) {
    if (note < 0 || note > 127) return;
    if (velocity <= 0) {
        noteOff(note);
        return;
    }
    // The same key again restarts its own voice; else a free one; else the oldest (a release
    // first, it is on its way out anyway). Its phase and level carry on: no click.
    Voice* v = nullptr;
    for (Voice& c : voices_)
        if (c.note == note) v = &c;
    for (Voice& c : voices_)
        if (!v && c.note < 0) v = &c;
    if (!v) {
        for (int pass = 0; pass < 2 && !v; ++pass)
            for (Voice& c : voices_)
                if ((pass == 1 || c.step < 0.0f) && (!v || c.age < v->age)) v = &c;
    }
    v->note = note;
    v->inc = 440.0f * std::exp2((static_cast<float>(note) - 69.0f) / 12.0f) / sr_;
    v->amp = kVoiceGain * static_cast<float>(velocity > 127 ? 127 : velocity) / 127.0f;
    v->step = attackStep_;
    v->key = true;
    v->pedal = false;
    v->age = ++started_;
}

void Engine::release(Voice& v) {
    v.key = v.pedal = false;
    if (v.note >= 0) v.step = -releaseStep_;
}

void Engine::noteOff(int note) {
    for (Voice& v : voices_)
        if (v.note == note && v.key) {
            if (pedal_) {
                v.key = false;
                v.pedal = true;
            } else {
                release(v);
            }
        }
}

void Engine::sustain(bool down) {
    pedal_ = down;
    if (!down)
        for (Voice& v : voices_)
            if (v.pedal) release(v);
}

void Engine::allNotesOff() {
    for (Voice& v : voices_)
        if (v.key || v.pedal) release(v);
}

void Engine::reset() {
    for (Voice& v : voices_) v = Voice{};
    pedal_ = false;
    gain_ = target_;
}

void Engine::seed(uint32_t s) { seed_ = s ? s : 1u; }

void Engine::setTransport(double bpm, double beats, bool playing, bool beatsValid) {
    bpm_ = bpm > 1.0 ? bpm : 120.0;
    beats_ = beats;
    playing_ = playing;
    beatsValid_ = beatsValid;
}

int Engine::activeVoices() const {
    int n = 0;
    for (const Voice& v : voices_) n += v.note >= 0 ? 1 : 0;
    return n;
}

void Engine::render(float* outL, float* outR, int n) {
    if (n <= 0) return;
    StageClock clock;
    std::memset(outL, 0, sizeof(float) * static_cast<size_t>(n));
    bool any = false;
    for (Voice& v : voices_) {
        if (v.note < 0) continue;
        any = true;
        for (int i = 0; i < n; ++i) {
            v.level += v.step;
            if (v.level >= 1.0f) {
                v.level = 1.0f;
                v.step = 0.0f;
            } else if (v.level <= 0.0f && v.step < 0.0f) {   // released all the way: the voice is free
                v = Voice{};
                break;
            }
            outL[i] += v.amp * v.level * std::sin(kTwoPi * v.phase);
            v.phase += v.inc;
            if (v.phase >= 1.0f) v.phase -= 1.0f;
        }
    }
    // The volume glides per sample and lands exactly (0 at -inf, not a tail of tiny numbers); with
    // nothing sounding it is simply where it is going.
    if (any) {
        for (int i = 0; i < n; ++i) {
            gain_ += glide_ * (target_ - gain_);
            outL[i] *= gain_;
        }
        if (std::fabs(target_ - gain_) < 1e-6f) gain_ = target_;
    } else {
        gain_ = target_;
    }
    if (outR != outL) std::memcpy(outR, outL, sizeof(float) * static_cast<size_t>(n));
    clock.lap(STG_VOICES);
}

} // namespace af
