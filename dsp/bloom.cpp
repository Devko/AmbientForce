#include "bloom.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace af {

namespace {

constexpr float kVoiceGain = 0.25f;      // a voice at full velocity and level: a sine at -12 dBFS
constexpr int kStealSamples = 132;       // 3 ms: a stolen note's fade
constexpr float kVelGlide = 1.0f / 132.0f;   // a note swelling again moves to its new velocity over 3 ms
constexpr float kFloor = 1e-3f;          // -60 dB: where a release ends
constexpr float kBoostInS = 0.2f;        // the handoff's boost ramps in over this
constexpr float kSendOutS = 0.2f;        // and its send fades out over the handoff's last this
constexpr float kLevelS = 0.01f;         // Level and Mute's glide (time constant)
constexpr float kChordSpread = 0.6f;     // of Width: the chord's notes, lowest to highest
constexpr float kUnisonSpread = 0.4f;    // of Width: a voice's two halves around its place
constexpr float kBreathQ = 4.0f;
constexpr float kNoiseRms = 0.57735027f; // white noise, uniform in -1..1
constexpr float kToneRms = 0.70710678f;  // a lifetime frame's (a full-scale sine's): Breath 1's level
constexpr float kInvSqrt2 = 0.70710678f;
constexpr float kLog2e = 1.44269504f;
constexpr float kLog2Thousand = 9.96578428f;   // -60 dB in octaves of amplitude

// cos(pi / 2 u)^2 for u in 0..1: the handoff's fades, 1 to 0.
float cos2(float u) {
    const float c = sinCycle(0.25f + 0.25f * clampf(u, 0.0f, 1.0f));
    return c * c;
}

// Equal-power pan, p -1..1: (L, R) = (cos, sin) of pi / 4 (p + 1).
f2 equalPower(float p, float gain) {
    const float t = 0.125f * (clampf(p, -1.0f, 1.0f) + 1.0f);   // 0..0.25 of a cycle
    return f2{sinCycle(t + 0.25f) * gain, sinCycle(t) * gain};
}

// The handoff's send over its release curve at t seconds into it: the boost ramping from 1 to B,
// then the fade-out.
float sendShape(float t, float boost) {
    return (1.0f + (boost - 1.0f) * std::min(1.0f, t * (1.0f / kBoostInS))) *
           cos2((t - (Bloom::kHandoffS - kSendOutS)) * (1.0f / kSendOutS));
}

// The handoff's B for a release of releaseS: the send over the handoff, the release times
// sendShape(), then carries the energy of the whole release (to -60 dB) as Tail Voice sends it.
// With x = B - 1 that energy is a + 2 b x + c x^2: a, b and c the release's power times f^2, r f^2
// and r^2 f^2 over the handoff (r the ramp 0..1, f the fade-out), here by the midpoint rule in
// 300 steps of 5 ms. At most 4. The whole release's energy is always the larger (it holds the
// handoff's 1.5 s and more), so x >= 0.
float handoffBoostFor(float releaseS) {
    const double k = 2.0 * 6.90775527898 / releaseS;   // the power falls as exp(-k t): -60 dB at releaseS
    const double whole = (1.0 - 1e-6) / k;
    constexpr int kSteps = 300;
    const double dt = static_cast<double>(Bloom::kHandoffS) / kSteps, q = std::exp(-k * dt);
    double e = std::exp(-0.5 * k * dt), a = 0.0, b = 0.0, c = 0.0;
    for (int i = 0; i < kSteps; ++i, e *= q) {
        const float t = static_cast<float>((i + 0.5) * dt);
        const double r = std::min(1.0f, t * (1.0f / kBoostInS));
        const double f = cos2((t - (Bloom::kHandoffS - kSendOutS)) * (1.0f / kSendOutS));
        const double w = e * f * f;
        a += w;
        b += w * r;
        c += w * r * r;
    }
    a *= dt;
    b *= dt;
    c *= dt;
    const double x = (std::sqrt(b * b + c * std::max(0.0, whole - a)) - b) / c;
    return static_cast<float>(std::min(4.0, 1.0 + x));
}

bool zero2(f2 x) { return x[0] == 0.0f && x[1] == 0.0f; }

// The second half's presence Unison and Detune ask for: below 1 cent unison 2 plays as unison 1.
float uniFor(const BloomPatch& p) { return p.unison == 2 && p.detuneCents >= 1.0f ? 1.0f : 0.0f; }

constexpr int kGlideMix = FM_COUNT;   // voiceLoop's Mode while the filter glides: the general mix

// One SVF step (dsp/svf.h's SvfState::tick) with only the output the mode needs, where tick() mixes
// all three: LP the low-pass v2, BP the band v1, HP v0 - k v1 - v2.
template <int Mode>
AF_INLINE f2 svfOut(SvfState& s, f2 v0, const SvfUpdate& a, f2 k) {
    const f2 v3 = v0 - s.ic2;
    const f2 v1 = a.a1 * s.ic1 + a.a2 * v3;
    const f2 v2 = s.ic2 + a.a2 * s.ic1 + a.a3 * v3;
    s.ic1 = v1 + v1 - s.ic1;
    s.ic2 = v2 + v2 - s.ic2;
    if constexpr (Mode == FM_LP) return v2;
    else if constexpr (Mode == FM_BP) return v1;
    else return v0 - k * v1 - v2;
}

// f(std::true_type) or f(std::false_type), f(the mode as a type): run-time choices to template
// arguments.
template <class F>
AF_INLINE void pick(bool b, F f) {
    if (b) f(std::true_type{});
    else f(std::false_type{});
}
template <class F>
AF_INLINE void pickMode(int mode, F f) {
    switch (mode) {
        case FM_LP: f(std::integral_constant<int, FM_LP>{}); break;
        case FM_BP: f(std::integral_constant<int, FM_BP>{}); break;
        case FM_HP: f(std::integral_constant<int, FM_HP>{}); break;
        default: f(std::integral_constant<int, kGlideMix>{}); break;
    }
}

// acc + p0 y[0] (+ p1 y[1]): each SVF lane (a unison half) into its pan. NEON multiplies by a lane
// in place; built from the lanes, the compiler moves them about first.
AF_INLINE f2 panIn(f2 acc, f2 p0, f2 y) {
#if AF_NEON
    return vmla_lane_f32(acc, p0, y, 0);
#else
    return acc + p0 * splat2(y[0]);
#endif
}
AF_INLINE f2 panIn(f2 acc, f2 p0, f2 p1, f2 y) {
#if AF_NEON
    return vmla_lane_f32(vmla_lane_f32(acc, p0, y, 0), p1, y, 1);
#else
    return acc + p0 * splat2(y[0]) + p1 * splat2(y[1]);
#endif
}

} // namespace

Bloom::Bloom() {
    set(BloomPatch{}, HarmonyPatch{});
    reset();
}

void Bloom::seed(uint32_t s) {
    seed_ = s;
    reseed();
}

void Bloom::reseed() {
    rng_ = seed_ * 0x9E3779B1u + 0x85EBCA6Bu;
    if (rng_ == 0) rng_ = 0x85EBCA6Bu;   // xorshift's one stuck state
    for (int i = 0; i < kVoices; ++i) v_[i].scan.seed(seed_ * 16u + static_cast<uint32_t>(i));
}

void Bloom::set(const BloomPatch& p, const HarmonyPatch& h) {
    BloomPatch q = p;
    q.level = clampParam(p.level, 0.0f, 1.0f, 0.0f);
    q.cutoffHz = clampParam(p.cutoffHz, 20.0f, 20000.0f, 5000.0f);
    q.reso = clampParam(p.reso, 0.0f, 1.0f, 0.0f);
    q.filterMode = std::min(std::max(p.filterMode, 0), FM_COUNT - 1);
    q.bOctave = std::min(std::max(p.bOctave, -2), 2);
    q.blend = clampParam(p.blend, 0.0f, 1.0f, 0.0f);
    q.couple = std::min(std::max(p.couple, 0), CP_COUNT - 1);
    q.coupleAmt = clampParam(p.coupleAmt, 0.0f, 1.0f, 0.0f);
    q.unison = p.unison >= 2 ? 2 : 1;
    q.detuneCents = clampParam(p.detuneCents, 0.0f, 50.0f, 0.0f);
    q.swellS = clampParam(p.swellS, 0.005f, 30.0f, 2.5f);
    q.releaseS = clampParam(p.releaseS, 0.01f, 30.0f, 6.0f);
    q.velSens = clampParam(p.velSens, 0.0f, 1.0f, 0.0f);
    q.breath = clampParam(p.breath, 0.0f, 1.0f, 0.0f);
    q.tail = p.tail == TL_SPACE ? TL_SPACE : TL_VOICE;
    q.width = clampParam(p.width, 0.0f, 1.0f, 0.0f);

    const bool retune = h.key != h_.key || h.tuning != h_.tuning;
    const bool reinc = q.unison != p_.unison || q.detuneCents != p_.detuneCents || q.bOctave != p_.bOctave;
    panDirty_ = panDirty_ || q.width != p_.width || q.unison != p_.unison;
    const bool refilter =
        q.cutoffHz != p_.cutoffHz || q.reso != p_.reso || q.filterMode != p_.filterMode || fT_.g == 0.0f;
    if (q.releaseS != p_.releaseS || boost_ == 0.0f) boost_ = handoffBoostFor(q.releaseS);
    p_ = q;
    h_ = h;
    h_.strumS = clampParam(h.strumS, 0.0f, 2.0f, 0.0f);   // HarmonyPatch's range

    attackRate_ = 1.0f / (p_.swellS * kRate);
    releaseLog2_ = -kLog2Thousand / (p_.releaseS * kRate);
    lvT_ = p_.mute ? 0.0f : p_.level;
    if (refilter) {
        Filter f;
        f.g = svfG(p_.cutoffHz);
        f.k = 2.0f * exp2Fast(-5.0f * p_.reso);   // 1 / Q, Q = 0.5 x 32^reso
        f.m[0] = p_.filterMode == FM_HP ? 1.0f : 0.0f;
        f.m[1] = p_.filterMode == FM_BP ? 1.0f : p_.filterMode == FM_HP ? -f.k : 0.0f;
        f.m[2] = p_.filterMode == FM_LP ? 1.0f : p_.filterMode == FM_HP ? -1.0f : 0.0f;
        fT_ = f;
    }
    if (retune || reinc)
        for (Voice& v : v_) {
            if (v.stage == ST_FREE) continue;
            if (retune) v.pitch = tunedPitch(h_, v.note);
            tune(v);
        }
}

void Bloom::reset() {
    for (Voice& v : v_) v = Voice{};
    reseed();
    played_ = 0;
    lv_ = lvT_;
    send_ = 0.0f;
    fNow_ = fT_;
    const SvfUpdate u = SvfUpdate::of(splat2(fNow_.g), splat2(fNow_.k));
    a1_ = u.a1[0];
    a2_ = u.a2[0];
    a3_ = u.a3[0];
    glide_ = false;
    panDirty_ = true;
    tabA_ = tabB_ = oldA_ = oldB_ = nullptr;
    tableFade_ = 0;
}

// --- owners and voices ------------------------------------------------------------------------

bool Bloom::held(const Voice& v) {
    uint32_t any = 0;
    for (uint32_t w : v.owners) any |= w;
    return any != 0;
}

void Bloom::clearOwners(Voice& v) {
    for (uint32_t& w : v.owners) w = 0;
}

Bloom::Voice* Bloom::holding(int note) {
    for (Voice& v : v_)
        if (held(v) && heldNote(v) == note) return &v;
    return nullptr;
}

Bloom::Voice* Bloom::releasing(int note) {
    for (Voice& v : v_)
        if (v.next < 0 && (v.stage == ST_RELEASE || v.stage == ST_HANDOFF) && v.note == note) return &v;
    return nullptr;
}

Bloom::Voice& Bloom::allocate(uint32_t taken) {
    const auto open = [&](const Voice& v) { return !((taken >> (&v - v_)) & 1u); };
    for (Voice& v : v_)
        if (open(v) && v.stage == ST_FREE && v.next < 0) return v;
    Voice* best = nullptr;
    for (Voice& v : v_)
        if (open(v) && v.next < 0 && (v.stage == ST_RELEASE || v.stage == ST_HANDOFF || v.stage == ST_STEAL) &&
            (!best || v.loud < best->loud))
            best = &v;
    if (best) return *best;
    for (Voice& v : v_)
        if (open(v) && (!best || v.age < best->age)) best = &v;
    return best ? *best : v_[0];   // a chord has at most kVoices notes: never all taken
}

void Bloom::steal(Voice& v) {
    if (v.stage != ST_FREE && v.stage != ST_STEAL) {   // a fade already under way goes on
        for (int h = 0; h < 2; ++h) {
            v.stealP[h] = v.P[h];
            v.stealS[h] = v.S[h];
        }
        v.stealLoud = v.loud;
        v.stealLv = lv_;
        v.stealLeft = kStealSamples;
        v.stage = ST_STEAL;
    }
    v.next = -1;
    v.retrig = false;
    clearOwners(v);
}

void Bloom::add(int note, int owner, float vel, float spread, int wait, uint32_t& taken) {
    const int bit = ownerBit(owner);
    if (Voice* v = holding(note)) {   // sounding or about to: it only gains an owner
        addOwner(*v, bit);
        taken |= 1u << (v - v_);
        return;
    }
    Voice* v = releasing(note);
    if (v) {   // swells again where it is, keeping its place
        v->retrig = true;
        v->nextSpread = v->spread;
    } else {
        v = &allocate(taken);
        steal(*v);
        v->retrig = false;
        v->nextSpread = spread;
    }
    v->next = note;
    v->wait = wait;
    v->nextVel = vel;
    clearOwners(*v);
    addOwner(*v, bit);
    v->age = ++played_;
    taken |= 1u << (v - v_);
}

int Bloom::strumWait(int k, int n) const {
    if (k <= 0 || h_.strumS <= 0.0f) return 0;
    return static_cast<int>(std::lround(static_cast<double>(k) * h_.strumS * kRate / std::max(1, n - 1)));
}

void Bloom::play(const Chord& c, int owner, float vel) {
    vel = clampParam(vel, 0.0f, 1.0f, 0.0f);
    const int n = std::min(std::max(c.n, 0), kChordMax);
    uint32_t taken = 0;
    for (int k = 0; k < n; ++k) {
        const int note = c.notes[k];
        if (note < 0 || note > 127) continue;
        const float spread = n > 1 ? 2.0f * static_cast<float>(k) / static_cast<float>(n - 1) - 1.0f : 0.0f;
        add(note, owner, vel, spread, strumWait(k, n), taken);
    }
}

void Bloom::letGo(Voice& v) {
    if (v.next >= 0) {   // not started yet: it never will (a note swelling again goes on releasing)
        v.next = -1;
        v.retrig = false;
        return;
    }
    if (v.stage != ST_ATTACK && v.stage != ST_SUSTAIN) return;
    // Only with a send open is there a Space to hand the tail to (send_: the last render's).
    if (p_.tail == TL_SPACE && p_.releaseS > kHandoffS && send_ > 0.0f) {
        v.stage = ST_HANDOFF;
        v.th = 0.0f;
        v.boost = boost_;
    } else {
        v.stage = ST_RELEASE;
    }
}

void Bloom::release(int owner) {
    const int bit = ownerBit(owner);
    for (Voice& v : v_) {
        if (!owns(v, bit)) continue;
        v.owners[bit >> 5] &= ~(1u << (bit & 31));
        if (!held(v)) letGo(v);
    }
}

void Bloom::releaseAll() {
    for (Voice& v : v_) {
        if (!held(v)) continue;
        clearOwners(v);
        letGo(v);
    }
}

void Bloom::moveTo(const Chord& c, float vel) {
    vel = clampParam(vel, 0.0f, 1.0f, 0.0f);
    const int n = std::min(std::max(c.n, 0), kChordMax);
    const auto in = [&](int note) {
        for (int k = 0; k < n; ++k)
            if (c.notes[k] == note) return true;
        return false;
    };
    for (Voice& v : v_)
        if (held(v) && !in(heldNote(v))) {
            clearOwners(v);
            letGo(v);
        }
    // What is held already stays as it is; the rest is new, and only the new notes strum.
    int fresh[kChordMax], where[kChordMax], m = 0;
    uint32_t taken = 0;
    for (int k = 0; k < n; ++k) {
        const int note = c.notes[k];
        if (note < 0 || note > 127) continue;
        if (Voice* v = holding(note)) {
            addOwner(*v, ownerBit(-1));
            taken |= 1u << (v - v_);
        } else if (std::find(fresh, fresh + m, note) == fresh + m) {
            fresh[m] = note;
            where[m++] = k;
        }
    }
    for (int j = 0; j < m; ++j) {
        const float spread = n > 1 ? 2.0f * static_cast<float>(where[j]) / static_cast<float>(n - 1) - 1.0f : 0.0f;
        add(fresh[j], -1, vel, spread, strumWait(j, m), taken);
    }
}

// --- a voice starting ---------------------------------------------------------------------------

void Bloom::tune(Voice& v) {
    const double hz = 440.0 * std::exp2((v.pitch - 69.0) / 12.0);
    const double c = hz / kRate, o = std::exp2(static_cast<double>(p_.bOctave));
    // Worked out here once, for every render to share. The first half goes down by half the detune
    // as the second comes in (uni 0..1); the second is always up by half (unheard at uni 0).
    const double a0 = c * std::exp2(-v.uni * p_.detuneCents / 2400.0), a1 = c * std::exp2(p_.detuneCents / 2400.0);
    v.incA[0] = static_cast<float>(a0);
    v.incA[1] = static_cast<float>(a1);
    v.incB[0] = static_cast<float>(a0 * o);
    v.incB[1] = static_cast<float>(a1 * o);
    // The breath's band-pass at the note: unity peak (k v1), scaled so its RMS for the white
    // noise is a tone's (the band holds pi f / (Q rate) of the noise's power).
    const double f = std::min(std::max(hz, 10.0), 0.45 * kRate);
    const double g = std::tan(3.14159265358979 * f / kRate), k = 1.0 / kBreathQ;
    const double a = 1.0 / (1.0 + g * (g + k));
    v.bpA1 = static_cast<float>(a);
    v.bpA2 = static_cast<float>(g * a);
    v.bpA3 = static_cast<float>(g * g * a);
    v.bpNorm = static_cast<float>(k * kToneRms / (kNoiseRms * std::sqrt(3.14159265358979 * f / (kBreathQ * kRate))));
}

void Bloom::placePan(Voice& v) {
    // The second half at uni / sqrt 2 and the first at what keeps their power 1 (the halves drift
    // apart in phase, so their powers add), each moving out from the voice's place as it comes in.
    const float w = p_.width, at = kChordSpread * w * v.spread, apart = kUnisonSpread * w * v.uni;
    const float g1 = v.uni * kInvSqrt2, g0 = std::sqrt(1.0f - g1 * g1);
    v.pan[0] = equalPower(at - apart, g0);
    v.pan[1] = g1 > 0.0f ? equalPower(at + apart, g1) : splat2(0.0f);
    v.bk = 1.0f / (g0 + g1);   // the same breath in both lanes adds up coherently
}

void Bloom::begin(Voice& v) {
    if (v.retrig && (v.stage == ST_RELEASE || v.stage == ST_HANDOFF) && v.note == v.next) {
        // Swells again from where its release has it: the S-curve's point at that level. A
        // handoff's dry fade is folded into the level (the dry goes on from where it is), and what
        // its send had beyond the dry fades out over 3 ms, from where it is too.
        if (v.stage == ST_HANDOFF) {
            const float dry = cos2(v.th * (1.0f / kHandoffS));
            v.rest = v.env * (v.sendMul - dry) + v.rest * static_cast<float>(v.restLeft) * (1.0f / kStealSamples);
            v.restLeft = kStealSamples;
            v.env *= dry;
            v.sendMul = 1.0f;
        }
        v.stage = ST_ATTACK;
        v.x = std::acos(clampf(1.0f - 2.0f * v.env, -1.0f, 1.0f)) * (1.0f / kPi);
        v.vel = v.nextVel;
        v.velStep = (v.vel - v.velNow) * kVelGlide;
        v.next = -1;
        v.retrig = false;
        return;
    }
    if (v.stage != ST_FREE) return;   // a steal's fade still going
    v.note = v.next;
    v.next = -1;
    v.retrig = false;
    v.stage = ST_ATTACK;
    v.x = 0.0f;
    v.env = 0.0f;
    v.vel = v.velNow = v.nextVel;
    v.velStep = 0.0f;
    v.th = 0.0f;
    v.sendMul = 1.0f;
    v.rest = 0.0f;
    v.restLeft = 0;
    v.fading = false;   // it starts on the new tables
    v.spread = v.nextSpread;
    v.uni = uniFor(p_);
    v.pitch = tunedPitch(h_, v.note);
    tune(v);
    placePan(v);
    // A and B in step (the Couple modes hear their phases). The second half a quarter of A's cycle
    // later, B by the same time (a quarter of a cycle at its octave), so it is the first half
    // delayed: its sum with the first never depends on the seed.
    const double later = 0.25 * std::exp2(static_cast<double>(p_.bOctave));
    v.oa[0].reset(0.0f);
    v.ob[0].reset(0.0f);
    v.oa[1].reset(0.25f);
    v.ob[1].reset(static_cast<float>(later - std::floor(later)));
    v.svf.clear();
    v.bpIc1 = v.bpIc2 = 0.0f;
}

// --- control rate -------------------------------------------------------------------------------

void Bloom::control(int m) {
    const float dt = static_cast<float>(m) * (1.0f / kRate);
    lv_ = lvT_ + (lv_ - lvT_) * exp2Fast(-static_cast<float>(m) * kLog2e / (kLevelS * kRate));
    if (std::fabs(lv_ - lvT_) < 1e-5f) lv_ = lvT_;
    const float sens = p_.velSens;
    for (Voice& v : v_) {
        if (v.next >= 0 && v.wait <= 0) begin(v);
        for (int h = 0; h < 2; ++h) {
            v.P0[h] = v.P[h];
            v.S0[h] = v.S[h];
        }
        v.live = v.stage != ST_FREE;
        if (!v.live) continue;
        const float ut = uniFor(p_);
        if (v.uni != ut) {   // the second half glides in or out over 3 ms: its gain, place and detune
            const float du = static_cast<float>(m) * (1.0f / kStealSamples);
            v.uni = ut > v.uni ? std::min(ut, v.uni + du) : std::max(ut, v.uni - du);
            tune(v);
            placePan(v);
        } else if (panDirty_) {
            placePan(v);
        }
        // dry: the handoff's fade on the dry; rest: the send beyond the dry, as a level like env
        // (a handoff's, or what one left when it swelled again, fading out over 3 ms).
        float dry = 1.0f, rest = 0.0f;
        if (v.restLeft > 0) {
            v.restLeft = std::max(0, v.restLeft - m);
            rest = v.rest * static_cast<float>(v.restLeft) * (1.0f / kStealSamples);
            if (v.restLeft == 0) v.rest = 0.0f;
        }
        bool ends = false;
        switch (v.stage) {
            case ST_ATTACK:
                v.x += static_cast<float>(m) * attackRate_;
                if (v.x >= 1.0f) {
                    v.stage = ST_SUSTAIN;
                    v.env = 1.0f;
                } else {
                    v.env = 0.5f - 0.5f * sinCycle(0.25f + 0.5f * v.x);   // 0.5 - 0.5 cos(pi x)
                }
                break;
            case ST_SUSTAIN: v.env = 1.0f; break;
            case ST_RELEASE:
                v.env *= exp2Fast(static_cast<float>(m) * releaseLog2_);
                ends = v.env < kFloor;
                break;
            case ST_HANDOFF: {
                v.env *= exp2Fast(static_cast<float>(m) * releaseLog2_);
                v.th += dt;
                ends = v.th >= kHandoffS || v.env < kFloor;
                dry = cos2(v.th * (1.0f / kHandoffS));
                v.sendMul = sendShape(v.th, v.boost);
                rest += v.env * (v.sendMul - dry);
                break;
            }
            default:   // ST_STEAL
                v.stealLeft -= m;
                ends = v.stealLeft <= 0;
                break;
        }
        if (v.velStep != 0.0f) {
            v.velNow += v.velStep * static_cast<float>(m);
            if ((v.velStep > 0.0f) == (v.velNow >= v.vel)) {
                v.velNow = v.vel;
                v.velStep = 0.0f;
            }
        }
        if (v.stage == ST_STEAL) {
            // The fade, and the level's move since it began (Mute during a steal).
            const float f = ends || v.stealLv <= 0.0f
                                ? 0.0f
                                : static_cast<float>(v.stealLeft) * (1.0f / kStealSamples) * lv_ / v.stealLv;
            for (int h = 0; h < 2; ++h) {
                v.P[h] = v.stealP[h] * splat2(f);
                v.S[h] = v.stealS[h] * splat2(f);
            }
            v.loud = v.stealLoud * f;
        } else {
            const float g = ends ? 0.0f : kVoiceGain * (1.0f - sens + sens * v.velNow) * lv_;
            for (int h = 0; h < 2; ++h) {
                v.P[h] = v.pan[h] * splat2(g * v.env * dry);
                v.S[h] = v.pan[h] * splat2(g * rest);
            }
            v.loud = g * v.env * dry;
        }
        if (ends) {
            v.stage = ST_FREE;   // this step still renders its fade to 0
            v.note = -1;
            v.env = 0.0f;
            v.sendMul = 1.0f;
            v.rest = 0.0f;
            v.restLeft = 0;
            v.retrig = false;   // a note waiting to swell again here now starts afresh
        }
        v.pos = v.scan.step(p_.pos, dt);
    }
    panDirty_ = false;
}

void Bloom::filterFor(int m) {
    if (fNow_ == fT_) {
        glide_ = false;
        return;
    }
    // g evenly in octaves, k and the mix in straight lines, landing exactly on the last sample;
    // every sample a real filter between the two (dsp/svf.h).
    glide_ = true;
    const float inv = 1.0f / static_cast<float>(m);
    const float r = fT_.g / fNow_.g;
    const float gr = r > 0.0f && r < 1e30f ? std::pow(r, inv) : 1.0f;
    const float dk = (fT_.k - fNow_.k) * inv;
    float dm[3];
    for (int j = 0; j < 3; ++j) dm[j] = (fT_.m[j] - fNow_.m[j]) * inv;
    Filter f = fNow_;
    for (int i = 0; i < m; ++i) {
        if (i == m - 1) {
            f = fT_;
        } else {
            f.g *= gr;
            f.k += dk;
            for (int j = 0; j < 3; ++j) f.m[j] += dm[j];
        }
        const float a1 = 1.0f / (1.0f + f.g * (f.g + f.k));
        ga1_[i] = a1;
        ga2_[i] = f.g * a1;
        ga3_[i] = f.g * f.g * a1;
        for (int j = 0; j < 3; ++j) gm_[j][i] = f.m[j];
    }
    fNow_ = fT_;
    a1_ = ga1_[m - 1];
    a2_ = ga2_[m - 1];
    a3_ = ga3_[m - 1];
}

// --- audio --------------------------------------------------------------------------------------

// One voice's m samples from o: its two halves (a0, a1) plus the breath through the SVF (the two
// lanes), panned into the dry bus and, in a handoff, the send beyond it.
template <bool Unison, bool Breath, int Mode, bool Send>
void Bloom::voiceLoop(Voice& v, const float* a0, const float* a1, int o, int m) {
    const f2 inv = splat2(1.0f / static_cast<float>(m));
    f2 p0 = v.P0[0], p1 = v.P0[1], s0 = v.S0[0], s1 = v.S0[1];
    const f2 dp0 = (v.P[0] - p0) * inv, dp1 = (v.P[1] - p1) * inv;
    const f2 ds0 = (v.S[0] - s0) * inv, ds1 = (v.S[1] - s1) * inv;
    SvfState svf = v.svf;
    const SvfUpdate cu{splat2(a1_), splat2(a2_), splat2(a3_)};
    const f2 k = splat2(fNow_.k);
    float ic1 = v.bpIc1, ic2 = v.bpIc2;
    const float b1 = v.bpA1, b2 = v.bpA2, b3 = v.bpA3, bk = Breath ? p_.breath * v.bpNorm * v.bk : 0.0f;
    f2* bus = bus_ + o;
    f2* sx = sendX_ + o;
    const float* nz = noise_ + o;
    for (int i = 0; i < m; ++i) {
        // The halves side by side (unison 1: the one in both lanes; lane 1 is never heard).
        f2 x = Unison ? load2(a0 + i, a1 + i) : splat2(a0[i]);
        if constexpr (Breath) {
            const float v3 = nz[i] - ic2;
            const float v1 = b1 * ic1 + b2 * v3;
            const float v2 = ic2 + b2 * ic1 + b3 * v3;
            ic1 = v1 + v1 - ic1;
            ic2 = v2 + v2 - ic2;
            x += splat2(v1 * bk);
        }
        f2 y;
        if constexpr (Mode == kGlideMix) {
            const SvfUpdate u{splat2(ga1_[i]), splat2(ga2_[i]), splat2(ga3_[i])};
            const f2 mm[3] = {splat2(gm_[0][i]), splat2(gm_[1][i]), splat2(gm_[2][i])};
            y = svf.tick(x, u, mm);
        } else {
            y = svfOut<Mode>(svf, x, cu, k);
        }
        p0 += dp0;
        if constexpr (Unison) {
            p1 += dp1;
            bus[i] = panIn(bus[i], p0, p1, y);
        } else {
            bus[i] = panIn(bus[i], p0, y);
        }
        if constexpr (Send) {
            s0 += ds0;
            if constexpr (Unison) {
                s1 += ds1;
                sx[i] = panIn(sx[i], s0, s1, y);
            } else {
                sx[i] = panIn(sx[i], s0, y);
            }
        }
    }
    svf.flushTiny();
    v.svf = svf;
    v.bpIc1 = std::fabs(ic1) < 1e-20f ? 0.0f : ic1;
    v.bpIc2 = std::fabs(ic2) < 1e-20f ? 0.0f : ic2;
}

void Bloom::renderVoice(Voice& v, int o, int m, const float* fade) {
    float a0[kChunk], a1[kChunk], scratch[2 * kChunk];
    const Wavetable& ta = *tabA_;
    const Wavetable& tb = *tabB_;
    // The second half is heard while its gains, at either end of the step, are: unison 2, and the
    // steps that glide it in or out. Else it is only skipped along, so that it comes back in at
    // the same quarter cycle from the first (at Detune 0, where they share a pitch).
    const bool uni = !zero2(v.P0[1]) || !zero2(v.P[1]) || !zero2(v.S0[1]) || !zero2(v.S[1]);
    const int cp = p_.couple;
    const float amt = p_.coupleAmt, blend = p_.blend;
    renderCoupled(v.oa[0], ta, v.incA[0], v.ob[0], tb, v.incB[0], v.pos, cp, amt, blend, a0, scratch, m);
    if (uni) {
        renderCoupled(v.oa[1], ta, v.incA[1], v.ob[1], tb, v.incB[1], v.pos, cp, amt, blend, a1, scratch, m);
    } else {
        v.oa[1].skip(ta, v.incA[1], v.pos, m);
        v.ob[1].skip(tb, v.incB[1], v.pos, m);
    }
    if (fade && v.fading) {   // new tables: the old pair under the new one, fading out
        float was[kChunk];
        const Wavetable& oa = *oldA_;
        const Wavetable& ob = *oldB_;
        renderCoupled(v.oldA[0], oa, v.incA[0], v.oldB[0], ob, v.incB[0], v.pos, cp, amt, blend, was, scratch, m);
        for (int i = 0; i < m; ++i) a0[i] = was[i] + fade[i] * (a0[i] - was[i]);
        if (uni) {
            renderCoupled(v.oldA[1], oa, v.incA[1], v.oldB[1], ob, v.incB[1], v.pos, cp, amt, blend, was, scratch, m);
            for (int i = 0; i < m; ++i) a1[i] = was[i] + fade[i] * (a1[i] - was[i]);
        } else {
            v.oldA[1].skip(oa, v.incA[1], v.pos, m);
            v.oldB[1].skip(ob, v.incB[1], v.pos, m);
        }
    }
    const bool breath = p_.breath > 0.0f;
    const bool send = sendOn_ && (!zero2(v.S0[0]) || !zero2(v.S[0]) || !zero2(v.S0[1]) || !zero2(v.S[1]));
    const int mode = glide_ ? kGlideMix : p_.filterMode;
    pick(uni, [&](auto u) {
        pick(breath, [&](auto b) {
            pickMode(mode, [&](auto f) {
                pick(send, [&](auto s) {
                    constexpr bool U = decltype(u)::value, B = decltype(b)::value, S = decltype(s)::value;
                    voiceLoop<U, B, decltype(f)::value, S>(v, a0, a1, o, m);
                });
            });
        });
    });
}

void Bloom::render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR, float spaceSend,
                   int n) {
    for (int o = 0; o < n; o += kMaxBlock)
        renderBlock(tables, outL + o, outR + o, sendL + o, sendR + o, spaceSend, std::min(kMaxBlock, n - o));
}

void Bloom::renderBlock(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR, float spaceSend,
                        int n) {
    if (n <= 0) return;
    spaceSend = clampParam(spaceSend, 0.0f, 1.0f, 0.0f);
    bool any = false, sounding = false;
    for (const Voice& v : v_) {
        any = any || v.stage != ST_FREE || v.next >= 0;
        sounding = sounding || v.stage != ST_FREE;
    }
    const Wavetable* ta = &tables.get(p_.table);
    const Wavetable* tb = &tables.get(p_.tableB);
    if (!sounding || !tabA_) {   // nothing to glide or fade from: level, filter, send and tables are where they go
        lv_ = lvT_;
        fNow_ = fT_;
        const SvfUpdate u = SvfUpdate::of(splat2(fNow_.g), splat2(fNow_.k));
        a1_ = u.a1[0];
        a2_ = u.a2[0];
        a3_ = u.a3[0];
        send_ = spaceSend;
        tabA_ = ta;
        tabB_ = tb;
        oldA_ = oldB_ = nullptr;
        tableFade_ = 0;
    } else if (tableFade_ == 0 && (ta != tabA_ || tb != tabB_)) {
        // New tables fade in over kTableFade, every sounding voice's oscillators as they are now
        // reading the old ones under them in step. A second change waits for this fade to end.
        oldA_ = tabA_;
        oldB_ = tabB_;
        tabA_ = ta;
        tabB_ = tb;
        tableFade_ = kTableFade;
        for (Voice& v : v_) {
            v.fading = v.stage != ST_FREE;
            if (!v.fading) continue;
            for (int h = 0; h < 2; ++h) {
                v.oldA[h] = v.oa[h];
                v.oldB[h] = v.ob[h];
            }
        }
    }
    if (!any) return;

    std::fill(bus_, bus_ + n, splat2(0.0f));
    // Only a voice handing off (or fading out of a handoff for a steal) sends beyond its dry, and
    // a handoff starts between renders, never in one.
    sendOn_ = false;
    for (const Voice& v : v_) sendOn_ = sendOn_ || v.stage == ST_HANDOFF || !zero2(v.S[0]) || !zero2(v.S[1]);
    if (sendOn_) std::fill(sendX_, sendX_ + n, splat2(0.0f));
    if (p_.breath > 0.0f)
        for (int i = 0; i < n; ++i) noise_[i] = randBipolar(rng_);
    bool heard = false;
    for (int o = 0; o < n;) {
        // A step ends where a strum's start or a steal's fade does, so a note starts on its sample.
        int m = std::min(kChunk, n - o);
        for (const Voice& v : v_) {
            if (v.next >= 0 && v.wait > 0) m = std::min(m, v.wait);
            if (v.stage == ST_STEAL && v.stealLeft > 0) m = std::min(m, v.stealLeft);
        }
        const float lvWas = lv_;
        control(m);
        filterFor(m);
        float fade[kChunk];   // the new tables' share, while they fade in
        if (tableFade_ > 0) {
            const float done = static_cast<float>(kTableFade - tableFade_);
            for (int j = 0; j < m; ++j)
                fade[j] = std::min(1.0f, (done + static_cast<float>(j + 1)) * (1.0f / kTableFade));
        }
        if (lvWas > 0.0f || lv_ > 0.0f)
            for (Voice& v : v_)
                if (v.live) {
                    renderVoice(v, o, m, tableFade_ > 0 ? fade : nullptr);
                    heard = true;
                }
        if (tableFade_ > 0) {
            tableFade_ -= m;
            if (tableFade_ <= 0) {
                tableFade_ = 0;
                oldA_ = oldB_ = nullptr;
                for (Voice& v : v_) v.fading = false;
            }
        }
        for (Voice& v : v_)
            if (v.next >= 0 && v.wait > 0) v.wait -= m;
        o += m;
    }
    if (!heard) {
        send_ = spaceSend;
        return;
    }
    const float ds = (spaceSend - send_) / static_cast<float>(n);
    float s = send_;
    if (sendOn_) {
        for (int i = 0; i < n; ++i) {
            s += ds;
            const f2 d = bus_[i], x = (d + sendX_[i]) * splat2(s);
            outL[i] += d[0];
            outR[i] += d[1];
            sendL[i] += x[0];
            sendR[i] += x[1];
        }
    } else {
        for (int i = 0; i < n; ++i) {
            s += ds;
            const f2 d = bus_[i], x = d * splat2(s);
            outL[i] += d[0];
            outR[i] += d[1];
            sendL[i] += x[0];
            sendR[i] += x[1];
        }
    }
    send_ = spaceSend;
}

// --- what the tests see -------------------------------------------------------------------------

int Bloom::active() const {
    int n = 0;
    for (const Voice& v : v_) n += v.stage != ST_FREE || v.next >= 0 ? 1 : 0;
    return n;
}

float Bloom::handoffBoost() const {
    float b = 0.0f;
    bool any = false;
    for (const Voice& v : v_)
        if (v.stage == ST_HANDOFF) {
            b = std::max(b, v.sendMul);
            any = true;
        }
    return any ? b : 1.0f;
}

bool Bloom::checkInvariants() const {
    const auto finite2 = [](f2 x) { return std::isfinite(x[0]) && std::isfinite(x[1]); };
    for (int i = 0; i < kVoices; ++i) {
        const Voice& v = v_[i];
        const bool sounding = v.stage == ST_ATTACK || v.stage == ST_SUSTAIN;
        if (v.stage < ST_FREE || v.stage > ST_STEAL) return false;
        if (held(v) != (sounding || v.next >= 0)) return false;   // owners exactly while it holds a note
        if (v.stage == ST_FREE) {
            if (v.note != -1 || !zero2(v.P[0]) || !zero2(v.P[1]) || !zero2(v.S[0]) || !zero2(v.S[1])) return false;
        } else if (v.note < 0 || v.note > 127) {
            return false;
        }
        if (v.next >= 0) {
            if (v.next > 127 || v.wait < 0) return false;
            const bool swells = (v.stage == ST_RELEASE || v.stage == ST_HANDOFF) && v.note == v.next;
            if (v.retrig ? !swells : !(v.stage == ST_FREE || v.stage == ST_STEAL)) return false;
        } else if (v.retrig) {
            return false;
        }
        if (!(v.env >= 0.0f && v.env <= 1.0f) || !(v.uni >= 0.0f && v.uni <= 1.0f)) return false;
        if (v.stage == ST_ATTACK && !(v.x >= 0.0f && v.x < 1.0f)) return false;
        if (v.stage == ST_STEAL && !(v.stealLeft > 0 && v.stealLeft <= kStealSamples)) return false;
        if (v.stage == ST_HANDOFF && !(v.th >= 0.0f && v.th < kHandoffS && v.boost >= 1.0f && v.boost <= 4.0f))
            return false;
        if (v.restLeft < 0 || v.restLeft > kStealSamples || !std::isfinite(v.rest)) return false;
        for (int h = 0; h < 2; ++h)
            if (!finite2(v.P[h]) || !finite2(v.S[h])) return false;
        if (held(v))   // no two voices hold one note
            for (int j = i + 1; j < kVoices; ++j)
                if (held(v_[j]) && heldNote(v_[j]) == heldNote(v)) return false;
    }
    return tableFade_ >= 0 && tableFade_ <= kTableFade && (tableFade_ == 0) == (oldA_ == nullptr);
}

Bloom::VoiceView Bloom::voice(int i) const {
    VoiceView w;
    if (i < 0 || i >= kVoices) return w;
    const Voice& v = v_[i];
    w.stage = v.stage;
    w.note = v.stage != ST_FREE ? v.note : -1;
    w.next = v.next;
    w.env = v.stage != ST_FREE ? v.env : 0.0f;
    return w;
}

} // namespace af
