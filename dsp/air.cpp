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
    setMix(1.0f, 1.0f, 0.0f);
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
    level_.aim(p.mute ? 0.0f : clampParam(p.level, 0.0f, 1.0f, 0.0f), kLevelGlide);
    loop_ = p.gen.loop;
    density_ = p.gen.density > 0.0f;   // NaN: the generator's 0
    letGoIfDown();   // set to 0 where the level already is: nothing glides down to let them go
}

void Air::setMix(float panL, float panR, float echoSend) {
    panL_.aim(panL, kLevelGlide);
    panR_.aim(panR, kLevelGlide);
    echo_.aim(clampParam(echoSend, 0.0f, 1.0f, 0.0f), kLevelGlide);
    if (!audible() && !generating()) {   // not rendered: nothing to glide over
        panL_.land();
        panR_.land();
        echo_.land();
    }
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
    for (LineGlide* g : {&level_, &panL_, &panR_, &echo_}) g->land();
    send_ = -1.0f;
}

void Air::letGoIfDown() {
    if (level_.now == 0.0f && level_.to == 0.0f && voices_.active() > 0) voices_.reset();
}

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

bool Air::render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR, float spaceSend,
                 float* echoL, float* echoR, int n) {
    bool sounded = false;
    for (int o = 0; o < n; o += kMaxBlock) {
        const int m = std::min(kMaxBlock, n - o);
        sounded = block(tables, outL + o, outR + o, sendL + o, sendR + o, spaceSend, echoL ? echoL + o : nullptr,
                        echoR ? echoR + o : nullptr, m) ||
                  sounded;
    }
    return sounded;
}

bool Air::block(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR, float spaceSend,
                float* echoL, float* echoR, int n) {
    if (n <= 0) return false;
    spaceSend = clampParam(spaceSend, 0.0f, 1.0f, 0.0f);
    if (send_ < 0.0f) send_ = spaceSend;
    const float s0 = send_;
    send_ = spaceSend;
    // The glides across this render, each a straight line from where it is.
    g0_ = level_.now;
    pl0_ = panL_.now;
    pr0_ = panR_.now;
    e0_ = echo_.now;
    level_.move(n);
    panL_.move(n);
    panR_.move(n);
    echo_.move(n);

    const int k = gen_.step(n, ev_, kMaxEvents);
    bool sounded = false;
    if (k > 0 || voices_.active() > 0) {
        // The voices into their bus up to each event, the event struck on its sample, then the rest.
        int pos = 0;
        for (int e = 0; e < k; ++e) {
            const AirEvent& v = ev_[e];
            const int at = std::min(std::max(v.offset, pos), n - 1);
            if (at > pos) {
                sounded = voices_.renderBus(tables, pos, at - pos) || sounded;
                pos = at;
            }
            if (v.played) strike(v.note, playedVel(v.vel), playedPan(v.note), true);
            else strike(v.note, v.vel, width_ * (2.0f * lifeosc::rand01(rng_) - 1.0f), false);
        }
        if (n > pos) sounded = voices_.renderBus(tables, pos, n - pos) || sounded;
        if (sounded) mix(outL, outR, sendL, sendR, echoL, echoR, s0, spaceSend, n);
    }
    letGoIfDown();   // glided down to 0 in this render: silent from here, every voice goes
    return sounded;
}

// The voices' bus into the dry (the level and the pan), the Space send (the level and spaceSend) and
// the Echo send (the level and its send), one pass. Each gain a straight line across the render, or
// taken whole while nothing glides.
void Air::mix(float* outL, float* outR, float* sendL, float* sendR, float* echoL, float* echoR, float s0, float s1, int n) {
    const f2* b = voices_.bus();
    const float g1 = level_.now, pl1 = panL_.now, pr1 = panR_.now, e1 = echo_.now;
    if (g0_ == g1 && pl0_ == pl1 && pr0_ == pr1 && e0_ == e1 && s0 == s1) {
        const float gl = g1 * pl1, gr = g1 * pr1, gs = g1 * s1, ge = g1 * e1;
        int i = 0;
#if AF_NEON
        const float* const bf = reinterpret_cast<const float*>(b);   // a vector type aliases its element type
        for (; i + 4 <= n; i += 4) {
            const float32x4x2_t d = vld2q_f32(bf + 2 * i);
            vst1q_f32(outL + i, vmlaq_n_f32(vld1q_f32(outL + i), d.val[0], gl));
            vst1q_f32(outR + i, vmlaq_n_f32(vld1q_f32(outR + i), d.val[1], gr));
            vst1q_f32(sendL + i, vmlaq_n_f32(vld1q_f32(sendL + i), d.val[0], gs));
            vst1q_f32(sendR + i, vmlaq_n_f32(vld1q_f32(sendR + i), d.val[1], gs));
            if (echoL) {
                vst1q_f32(echoL + i, vmlaq_n_f32(vld1q_f32(echoL + i), d.val[0], ge));
                vst1q_f32(echoR + i, vmlaq_n_f32(vld1q_f32(echoR + i), d.val[1], ge));
            }
        }
#endif
        for (; i < n; ++i) {
            const float l = b[i][0], r = b[i][1];
            outL[i] += l * gl;
            outR[i] += r * gr;
            sendL[i] += l * gs;
            sendR[i] += r * gs;
            if (echoL) {
                echoL[i] += l * ge;
                echoR[i] += r * ge;
            }
        }
        return;
    }
    const float inv = 1.0f / static_cast<float>(n);
    const float dg = (g1 - g0_) * inv, dl = (pl1 - pl0_) * inv, dr = (pr1 - pr0_) * inv, de = (e1 - e0_) * inv,
                ds = (s1 - s0) * inv;
    for (int i = 0; i < n; ++i) {
        const float f = static_cast<float>(i + 1);
        const float g = g0_ + dg * f, l = b[i][0], r = b[i][1];
        outL[i] += l * (g * (pl0_ + dl * f));
        outR[i] += r * (g * (pr0_ + dr * f));
        const float gs = g * (s0 + ds * f);
        sendL[i] += l * gs;
        sendR[i] += r * gs;
        if (echoL) {
            const float ge = g * (e0_ + de * f);
            echoL[i] += l * ge;
            echoR[i] += r * ge;
        }
    }
}

} // namespace af
