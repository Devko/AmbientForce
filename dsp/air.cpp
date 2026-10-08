#include "air.h"

#include "svf.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace af {

namespace {
constexpr int kNoteLow = 24, kNoteHigh = 108;   // Air's voices' range (airvoices.h)
constexpr float kPanCentre = 66.0f, kPanSpan = 42.0f;   // a played note's pan: 24 hard left .. 108 hard right
} // namespace

Air::Air() {
    set(AirPatch{}, HarmonyPatch{});
    reset();
}

void Air::seed(uint32_t s) {
    seed_ = s;
    voices_.seed(s * 0x2545F491u + 0x6A09E667u);   // not the generator's sequence
    gen_.seed(s);
    reset();
}

void Air::set(const AirPatch& p, const HarmonyPatch& h) {
    voices_.set(p.voice, h);
    gen_.set(p.gen, h);
    width_ = clampParam(p.voice.width, 0.0f, 1.0f, 0.0f);
    velSens_ = clampParam(p.velSens, 0.0f, 1.0f, 0.0f);
    level_ = clampParam(p.level, 0.0f, 1.0f, 0.0f);
    mute_ = p.mute;
    loop_ = p.gen.loop;
    density_ = p.gen.density > 0.0f;   // NaN: the generator's 0
}

void Air::setChord(const Chord& c, bool generate) {
    gen_.setChord(c, generate);
    generate_ = generate;
}

void Air::setTransport(double bpm, double beats, bool locked) { gen_.setTransport(bpm, beats, locked); }

void Air::reset() {
    voices_.reset();
    gen_.reset();
    rng_ = seed_ * 0x9E3779B1u + 0x7F4A7C15u;
    if (rng_ == 0) rng_ = 0x7F4A7C15u;   // xorshift's one stuck state
    xorshift(rng_);
    levelNow_ = levelTo_ = mute_ ? 0.0f : level_;
    levelStep_ = 0.0f;
    send_ = -1.0f;
}

bool Air::audible() const { return voices_.active() > 0 && (levelNow_ > 0.0f || (!mute_ && level_ > 0.0f)); }

bool Air::generating() const { return !mute_ && level_ > 0.0f && ((generate_ && density_) || loop_); }

float Air::playedPan(int note) const {
    return width_ * std::min(1.0f, std::max(-1.0f, (static_cast<float>(note) - kPanCentre) / kPanSpan));
}

float Air::playedVel(float vel) const { return 1.0f - velSens_ + velSens_ * clampParam(vel, 0.0f, 1.0f, 0.0f); }

void Air::strike(int note, float vel, float pan, bool played) {
    voices_.strike(note, vel, pan);
    ++strikes_;
    if (hook_) hook_(hookCtx_, note, vel, pan, played);
}

void Air::play(int note, float vel) {
    while (note < kNoteLow) note += 12;
    while (note > kNoteHigh) note -= 12;
    strike(note, playedVel(vel), playedPan(note), true);
    gen_.played(note, vel, 0);
}

void Air::render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR, float spaceSend, int n) {
    for (int o = 0; o < n; o += kMaxBlock)
        block(tables, outL + o, outR + o, sendL + o, sendR + o, spaceSend, std::min(kMaxBlock, n - o));
}

void Air::block(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR, float spaceSend, int n) {
    if (n <= 0) return;
    spaceSend = clampParam(spaceSend, 0.0f, 1.0f, 0.0f);
    if (send_ < 0.0f) send_ = spaceSend;

    // The level's glide across this render, a straight line from where it is.
    const float g0 = levelNow_, target = mute_ ? 0.0f : level_;
    if (target != levelTo_) {
        levelTo_ = target;
        levelStep_ = (target - levelNow_) / static_cast<float>(kLevelGlide);
    }
    if (levelNow_ != target) {   // (a step under half an ulp lands, as past the target)
        const float x = levelNow_ + levelStep_ * static_cast<float>(n);
        levelNow_ = x == levelNow_ || (levelStep_ > 0.0f ? x >= target : x <= target) ? target : x;
    }
    const float s0 = send_, inv = 1.0f / static_cast<float>(n);
    const float dg = (levelNow_ - g0) * inv, ds = (spaceSend - s0) * inv;
    send_ = spaceSend;

    const int k = gen_.step(n, ev_, kMaxEvents);
    if (k == 0 && voices_.active() == 0) return;   // nothing rings, nothing struck: no bus to mix

    std::memset(busL_, 0, sizeof(float) * static_cast<size_t>(n));
    std::memset(busR_, 0, sizeof(float) * static_cast<size_t>(n));
    int pos = 0;
    for (int e = 0; e < k; ++e) {
        const AirEvent& v = ev_[e];
        const int at = std::min(std::max(v.offset, pos), n - 1);
        if (at > pos) {
            voices_.render(tables, busL_ + pos, busR_ + pos, junkL_ + pos, junkR_ + pos, 0.0f, at - pos);
            pos = at;
        }
        if (v.played) strike(v.note, playedVel(v.vel), playedPan(v.note), true);
        else strike(v.note, v.vel, width_ * (2.0f * lifeosc::rand01(rng_) - 1.0f), false);
    }
    if (n > pos) voices_.render(tables, busL_ + pos, busR_ + pos, junkL_ + pos, junkR_ + pos, 0.0f, n - pos);

    // The bus into the dry and the send: the level, and the send after it, each a straight line.
    for (int i = 0; i < n; ++i) {
        const float f = static_cast<float>(i + 1);
        const float g = g0 + dg * f, s = g * (s0 + ds * f);
        outL[i] += busL_[i] * g;
        outR[i] += busR_[i] * g;
        sendL[i] += busL_[i] * s;
        sendR[i] += busR_[i] * s;
    }
}

} // namespace af
