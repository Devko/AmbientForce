#include "ground.h"

#include <algorithm>
#include <cmath>

namespace af {

namespace {

constexpr int kParts = Ground::PT_COUNT;
constexpr float kFloorDb = -60.0f;       // the fade's bottom: silence from here
constexpr float kToneK = 1.41421356f;    // the Tone's 1 / Q: Q 0.707, flat up to the cutoff, no peak
constexpr float kBodyK = 0.25f;          // the formants' 1 / Q: Q 4
constexpr float kF2 = 0.63f;             // the second formant under the first (PolyForce's vowel filter)
// At full Body the dry falls to 0.7 and the formants come in at twice their band-passes' unity
// peak: a harmonic on the first formant ends up 12 dB over one between them, as a sung vowel has
// it. The level moves less than 3 dB either way: the default drone on Cello Tasto loses up to
// 2.9 dB (its energy sits under the formants), on Choir Ah-Oo, a vowel already, it gains up to 2.1.
// Taking more of the dry away thins a low drone out: with the dry down to half, the cello lost
// 5.6 dB.
constexpr float kBodyDry = 0.3f, kBodyWet = 2.0f;
constexpr float kBreathDb = 3.0f, kBreathOct = 1.0f;   // Breath's swing at 1, either way
// The partials' sum, scaled so the loudest Ground there is peaks under 1.5: every partial at 1
// adds up to 5 times one partial's peak where they meet in phase, Breath adds 3 dB at its crest,
// Body its formants. Measured with every partial at 1, level 1, Breath 1, Beat 3 (so the partials
// pass through every alignment) and Width 0, over every table, Body 0, 1/3 and 1, three Color
// intervals and two Tones, 20 s each: the loudest is Square at Body 1 with an 11th, 1.49 (11.9
// before this scale), and the loudest lifetime table Tape Strings, 1.34. The default drone (level
// 0.7, Breath 0) then plays at about -22 dBFS RMS (-19 on Square, -24.5 on Saw); Task 10's level
// matching sets the presets. -18 dB, a power of two, so the scale is exact.
constexpr float kHeadroom = 0.125f;
constexpr float kInvRate = 1.0f / kRate;

// The first two formants (Hz) of a, o and u.
constexpr float kVowel[3][2] = {{800.0f, 1150.0f}, {450.0f, 800.0f}, {350.0f, 600.0f}};

// Color over the root, per interval (m3, M3, 4th, m7, 9th, 11th): Just's 5-limit ratios,
// Pythagorean's stacked fifths (harmony.cpp's tunings, the 9th and 11th an octave up), Equal's
// semitones.
constexpr float kJust[CI_COUNT] = {6.0f / 5.0f, 5.0f / 4.0f, 4.0f / 3.0f, 9.0f / 5.0f, 9.0f / 4.0f, 8.0f / 3.0f};
constexpr float kPythagorean[CI_COUNT] = {32.0f / 27.0f, 81.0f / 64.0f, 4.0f / 3.0f, 16.0f / 9.0f, 9.0f / 4.0f, 8.0f / 3.0f};
constexpr int kSemis[CI_COUNT] = {3, 4, 5, 10, 14, 17};

// Equal-power pan gains for -1..1: unity in the middle, sqrt 2 at the sides (PolyForce's).
void panGains(float pan, float& l, float& r) {
    const float a = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.785398163f;   // 0 .. pi/2
    l = 1.41421356f * sinQuarter(1.570796327f - a);
    r = 1.41421356f * sinQuarter(a);
}

} // namespace

Ground::Ground() {
    set(GroundPatch{}, HarmonyPatch{});
    reset();
}

void Ground::seed(uint32_t s) {
    seed_ = s;
    reset();
}

void Ground::set(const GroundPatch& p, const HarmonyPatch& h) {
    h_ = h;
    mute_ = p.mute;
    table_ = p.table;
    pos_ = p.pos;
    level_ = clampParam(p.level, 0.0f, 1.0f, 0.0f);
    cutoff_ = clampParam(p.cutoffHz, 40.0f, 16000.0f, 2500.0f);
    beat_ = clampParam(p.beatHz, 0.0f, 3.0f, 0.0f);
    gravity_ = clampParam(p.gravityS, 0.0f, 30.0f, 0.0f);
    fade_ = clampParam(p.fadeS, 0.05f, 30.0f, 4.0f);
    body_ = lifeosc::unit(p.body);
    breath_ = lifeosc::unit(p.breath);
    breathHz_ = clampParam(p.breathHz, 0.0f, 5.0f, 0.0f);
    register_ = std::min(std::max(p.registerOct, 1), 3);

    // The tuning as tunedPitch() reads it: out of range is the nearest one.
    const int tuning = std::min(std::max(h.tuning, 0), TU_COUNT - 1);
    const int ci = std::min(std::max(p.colorInterval, 0), CI_COUNT - 1);
    const bool equal = tuning == TU_EQUAL;
    ratio_[PT_SUB] = 0.5f;
    ratio_[PT_ROOT] = 1.0f;
    ratio_[PT_FIFTH] = equal ? std::exp2(7.0f / 12.0f) : 1.5f;
    ratio_[PT_OCTAVE] = 2.0f;
    ratio_[PT_COLOR] = equal ? std::exp2(static_cast<float>(kSemis[ci]) / 12.0f)
                             : (tuning == TU_JUST ? kJust : kPythagorean)[ci];

    const float level[kParts] = {lifeosc::unit(p.sub), lifeosc::unit(p.root), lifeosc::unit(p.fifth),
                                 lifeosc::unit(p.octave), lifeosc::unit(p.color)};
    const float width = lifeosc::unit(p.width);
    for (int i = 0; i < kParts; ++i) {
        float l, r;
        panGains(width * kPan[i], l, r);
        gainL_[i] = kHeadroom * level[i] * l;
        gainR_[i] = kHeadroom * level[i] * r;
    }
    // What of the distance to the goal a full chunk keeps: exp(-t / tau), tau = gravity / 3.
    glideKeep_ = gravity_ > 0.0f ? std::exp(-3.0 * kChunk / (static_cast<double>(gravity_) * kRate)) : 0.0;
    if (target_ >= 0) goal_ = rootPitch(target_);   // a new tuning or register: it glides there
}

double Ground::rootPitch(int note) const { return tunedPitch(h_, 12 * (register_ + 1) + note % 12); }

void Ground::setTarget(int rootNote) {
    if (rootNote < 0) {
        target_ = -1;   // fades out from the next chunk on
        return;
    }
    target_ = rootNote;
    goal_ = rootPitch(rootNote);
    if (!sounding_) {   // from silence: it starts on the note, at the bottom of the fade
        pitch_ = goal_;
        fadeDb_ = kFloorDb;
        sounding_ = true;
    }
}

void Ground::reset() {
    uint32_t r = seed_ * 0x85EBCA6Bu + 0x7F4A7C15u;   // not the scan's own sequence
    if (r == 0) r = 0x7F4A7C15u;                       // xorshift's one stuck state
    xorshift(r);
    scan_.seed(seed_);
    linear_[0].reset(lifeosc::rand01(r));    // the partials in order: Sub, Root, Fifth, Octave, Color
    hermite_[0].reset(lifeosc::rand01(r));
    hermite_[1].reset(lifeosc::rand01(r));
    linear_[1].reset(lifeosc::rand01(r));
    linear_[2].reset(lifeosc::rand01(r));
    breathPhase_ = lifeosc::rand01(r);
    target_ = -1;
    pitch_ = goal_ = 0.0;
    std::fill(curL_, curL_ + kParts, 0.0f);
    std::fill(curR_, curR_ + kParts, 0.0f);
    send_ = 0.0f;
    stop();
}

void Ground::stop() {
    sounding_ = false;
    fresh_ = true;
    fadeDb_ = kFloorDb;
    gain_ = wet_ = 0.0f;
    old_ = nullptr;
    tableFade_ = 0;
    toneSvf_.clear();
    f1Svf_.clear();
    f2Svf_.clear();
}

void Ground::render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR, float spaceSend,
                    int n) {
    spaceSend = clampParam(spaceSend, 0.0f, 1.0f, 0.0f);
    for (int o = 0; o < n && sounding_; o += kChunk)
        chunk(tables.get(table_), outL + o, outR + o, sendL + o, sendR + o, spaceSend, std::min(kChunk, n - o));
}

// One control step, then its n samples.
void Ground::chunk(const Wavetable& want, float* outL, float* outR, float* sendL, float* sendR, float spaceSend, int n) {
    const float dt = static_cast<float>(n) * kInvRate;

    // Gravity: the remaining distance shrinks by exp(-dt / tau) a step, in semitones.
    if (fresh_ || gravity_ <= 0.0f) {
        pitch_ = goal_;
    } else {
        const double keep = n == kChunk ? glideKeep_ : std::exp(-3.0 * n / (static_cast<double>(gravity_) * kRate));
        pitch_ = goal_ + (pitch_ - goal_) * keep;
    }
    fresh_ = false;

    // The fade, in dB; at the bottom on the way out, this chunk ramps to 0 and the drone stops.
    const float fadeStep = 60.0f * dt / fade_;
    bool ending = false;
    if (target_ >= 0) {
        fadeDb_ = std::min(0.0f, fadeDb_ + fadeStep);
    } else {
        fadeDb_ -= fadeStep;
        ending = fadeDb_ <= kFloorDb;
    }

    // Breath: +-3 dB and +-1 octave of cutoff at 1.
    breathPhase_ += static_cast<double>(breathHz_ * dt);
    breathPhase_ -= floorFast(breathPhase_);
    const float b = breath_ > 0.0f ? breath_ * sinCycle(static_cast<float>(breathPhase_)) : 0.0f;
    const float cutoff = clampf(cutoff_ * exp2Fast(kBreathOct * b), 20.0f, 0.45f * kRate);

    const float pos = scan_.step(pos_, dt);

    // The output's gain and the send's, ramped across the chunk from where the last one ended.
    const float g0 = gain_, g1 = mute_ || ending ? 0.0f : level_ * dbToGain(fadeDb_) * dbToGain(kBreathDb * b);
    const bool fromSilence = g0 == 0.0f;   // so whatever else jumps here can't be heard doing it
    const float s0 = fromSilence ? spaceSend : send_, s1 = spaceSend;
    gain_ = g1;
    send_ = s1;
    const float w1 = std::min(1.0f, 3.0f * body_), w0 = fromSilence ? w1 : wet_;
    wet_ = w1;

    // Each partial's pitch: its ratio over the root, and Beat's offset in Hz.
    const float rootHz = noteHz(static_cast<float>(pitch_));
    float inc[kParts];
    for (int i = 0; i < kParts; ++i) inc[i] = std::max(ratio_[i] * rootHz + beat_ * kBeat[i], 0.0f) * kInvRate;

    if (g0 == 0.0f && g1 == 0.0f) {   // level 0 or muted: nothing to hear, nothing read
        table0_ = &want;
        old_ = nullptr;
        tableFade_ = 0;
        linear_[0].skip(want, inc[PT_SUB], pos, n);
        hermite_[0].skip(want, inc[PT_ROOT], pos, n);
        hermite_[1].skip(want, inc[PT_FIFTH], pos, n);
        linear_[1].skip(want, inc[PT_OCTAVE], pos, n);
        linear_[2].skip(want, inc[PT_COLOR], pos, n);
        for (int i = 0; i < kParts; ++i) {
            curL_[i] = gainL_[i];
            curR_[i] = gainR_[i];
        }
        toneSvf_.clear();
        f1Svf_.clear();
        f2Svf_.clear();
        if (ending) stop();
        return;
    }

    // The table: a new one fades in over kTableFade samples, the oscillators as they were at the
    // switch reading the old one under it in step; from silence the new one is simply there. A
    // second change waits for the fade under way to end.
    if (fromSilence) {
        table0_ = &want;
        old_ = nullptr;
        tableFade_ = 0;
    } else if (!old_ && &want != table0_) {
        old_ = table0_;
        table0_ = &want;
        tableFade_ = kTableFade;
        std::copy(hermite_, hermite_ + 2, oldHermite_);
        std::copy(linear_, linear_ + 3, oldLinear_);
    }
    const Wavetable* const from = old_;
    float fade[kChunk];
    if (from) {
        const float done = static_cast<float>(kTableFade - tableFade_);
        for (int j = 0; j < n; ++j) fade[j] = std::min(1.0f, (done + static_cast<float>(j + 1)) * (1.0f / kTableFade));
        tableFade_ -= n;
        if (tableFade_ <= 0) {
            tableFade_ = 0;
            old_ = nullptr;
        }
    }

    // The partials, summed and panned. A partial at level 0 isn't read: it is skipped along.
    const float inv = 1.0f / static_cast<float>(n);
    float accL[kChunk] = {}, accR[kChunk] = {}, x[kChunk], y[kChunk];
    const Wavetable& t = *table0_;
    auto partial = [&](auto& osc, auto& old, int i) {
        const float l1 = gainL_[i], r1 = gainR_[i];
        const float l0 = fromSilence ? l1 : curL_[i], r0 = fromSilence ? r1 : curR_[i];
        curL_[i] = l1;
        curR_[i] = r1;
        if (l0 == 0.0f && r0 == 0.0f && l1 == 0.0f && r1 == 0.0f) {
            osc.skip(t, inc[i], pos, n);
            if (from) old.skip(*from, inc[i], pos, n);
            return;
        }
        osc.render(t, inc[i], pos, x, n);
        if (from) {
            old.render(*from, inc[i], pos, y, n);
            for (int j = 0; j < n; ++j) x[j] = y[j] + fade[j] * (x[j] - y[j]);
        }
        const float dl = (l1 - l0) * inv, dr = (r1 - r0) * inv;
        float gl = l0, gr = r0;
        for (int j = 0; j < n; ++j) {
            gl += dl;
            gr += dr;
            accL[j] += gl * x[j];
            accR[j] += gr * x[j];
        }
    };
    partial(linear_[0], oldLinear_[0], PT_SUB);
    partial(hermite_[0], oldHermite_[0], PT_ROOT);
    partial(hermite_[1], oldHermite_[1], PT_FIFTH);
    partial(linear_[1], oldLinear_[1], PT_OCTAVE);
    partial(linear_[2], oldLinear_[2], PT_COLOR);

    // The filters' targets: Tone, and Body's formants (a, then a -> o -> u).
    const f2 toneG[1] = {splat2(svfG(cutoff))};
    if (fromSilence) tone_.jump(toneG);
    else tone_.to(toneG);
    const bool body = w0 > 0.0f || w1 > 0.0f;
    if (body) {
        const float v = clampf(3.0f * body_ - 1.0f, 0.0f, 2.0f);
        const int k = std::min(static_cast<int>(v), 1);
        const float u = v - static_cast<float>(k);
        const f2 formants[2] = {splat2(svfG(kVowel[k][0] + u * (kVowel[k + 1][0] - kVowel[k][0]))),
                                splat2(svfG(kVowel[k][1] + u * (kVowel[k + 1][1] - kVowel[k][1])))};
        if (fromSilence || w0 == 0.0f) formant_.jump(formants);   // coming in from nothing: no glide to hear
        else formant_.to(formants);
    }

    // Body and Tone, L and R together, into yL / yR. The filters' updates are worked out per
    // sample only while their cutoffs glide. The states and glides run as locals: the members
    // could alias the float buffers, and would be loaded and stored again every sample.
    const f2 kTone = splat2(kToneK), kForm = splat2(kBodyK);
    const f2 lowPass[3] = {splat2(0.0f), splat2(0.0f), splat2(1.0f)};
    const f2 bandPass[3] = {splat2(0.0f), kForm, splat2(0.0f)};   // unity at the formant
    Glide<1, 1> tone = tone_;
    Glide<2, 2> formant = formant_;
    SvfState toneSvf = toneSvf_, f1Svf = f1Svf_, f2Svf = f2Svf_;
    const bool toneGlides = tone.moving(), formantsGlide = body && formant.moving();
    SvfUpdate tu = SvfUpdate::of(tone.cur()[0], kTone);
    SvfUpdate u1 = tu, u2 = tu;
    if (body) {
        u1 = SvfUpdate::of(formant.cur()[0], kForm);
        u2 = SvfUpdate::of(formant.cur()[1], kForm);
    }
    const float dw = (w1 - w0) * inv;
    float w = w0, yL[kChunk], yR[kChunk];
    for (int j = 0; j < n; ++j) {
        f2 y = load2(accL + j, accR + j);
        if (body) {
            w += dw;
            if (formantsGlide) {
                formant.next();
                u1 = SvfUpdate::fast(formant.cur()[0], kForm);
                u2 = SvfUpdate::fast(formant.cur()[1], kForm);
            }
            const f2 v1 = f1Svf.tick(y, u1, bandPass), v2 = f2Svf.tick(y, u2, bandPass);
            y = y * splat2(1.0f - kBodyDry * w) + (v1 + splat2(kF2) * v2) * splat2(kBodyWet * w);
        }
        if (toneGlides) {
            tone.next();
            tu = SvfUpdate::fast(tone.cur()[0], kTone);
        }
        store2(yL + j, yR + j, toneSvf.tick(y, tu, lowPass));
    }
    tone_ = tone;
    formant_ = formant;
    toneSvf.flushTiny();
    toneSvf_ = toneSvf;
    if (body && w1 == 0.0f) {   // faded out: it starts afresh when Body comes back
        f1Svf_.clear();
        f2Svf_.clear();
    } else if (body) {
        f1Svf.flushTiny();
        f2Svf.flushTiny();
        f1Svf_ = f1Svf;
        f2Svf_ = f2Svf;
    }

    // The output and the send, their gains ramping.
    const float dg = (g1 - g0) * inv, ds = (s1 - s0) * inv;
    float g = g0, s = s0;
    for (int j = 0; j < n; ++j) {
        g += dg;
        s += ds;
        const float l = yL[j] * g, r = yR[j] * g;
        outL[j] += l;
        outR[j] += r;
        sendL[j] += l * s;
        sendR[j] += r * s;
    }
    if (ending) stop();
}

} // namespace af
