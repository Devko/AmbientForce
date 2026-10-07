#include "ground.h"

#include <algorithm>
#include <cmath>

namespace af {

namespace {

constexpr int kParts = Ground::PT_COUNT;
constexpr float kFloorDb = -60.0f;       // the fade's bottom: off from here
constexpr float kToneK = 1.41421356f;    // the Tone's 1 / Q: Q 0.707, flat up to the cutoff, no peak
constexpr float kBodyK = 0.25f;          // the formants' 1 / Q: Q 4
constexpr float kF2 = 0.63f;             // the second formant under the first (PolyForce's vowel filter)
// At full Body the dry falls to 0.75 and the formants come in at 0.9 of their band-passes' unity
// peak: a harmonic on a formant is lifted 4.7 dB at most (the second formant's skirt included),
// and stands 7 dB over one between the formants, so the vowel is there without Body making a
// harmonic much louder than the dry had it. The level stays within 2.5 dB under the dry's: over
// the tables the default drone loses 1.6 to 2.5 dB at Body 1/3 (most of its energy sits under the
// formants) and 0 to 2.3 dB at Body 1, where Choir Ah-Oo, a vowel already, loses nothing. Twice
// the formants over 0.7 of the dry made a stronger vowel but lifted a harmonic on a formant by up
// to 9 dB, and the loudest peak from 1.26 to 1.95 (Square, B in register 3, Body 1).
constexpr float kBodyDry = 0.25f, kBodyWet = 0.9f;
constexpr float kBreathDb = 3.0f, kBreathOct = 1.0f;   // Breath's swing at 1, either way
// The partials' sum, scaled so the loudest Ground there is peaks under 1.5: every partial at 1
// adds up to 5 times one partial's peak where they meet in phase, Breath adds 3 dB at its crest,
// Body lifts what sits on its formants. Measured with every partial at 1, level 1, Breath 1 at
// 0.5 Hz, Beat 3 (so the partials pass through their alignments) and Width 0, over every table,
// the 12 pitch classes in registers 1 to 3, the 6 Color intervals and Body 0, 1/3, 0.5, 2/3, 0.85
// and 1, 8 s each: the loudest is Square, B in register 3 with a 4th at Body 1, 1.26; the loudest
// lifetime table Felt Piano, 1.20 (F# in register 3, m7, Body 1); at Body 0 nothing passes 1.06.
// (The linear reads moved none of these by a thousandth.) The default drone (level 0.7, Breath 0,
// G2) then plays at -22.2 dBFS RMS on Cello Tasto, -21.2 to -22.9 on the lifetime tables, -19.1
// on Square and -24.9 on Saw; Task 10's level matching sets the presets. -18 dB, a power of two,
// so the scale is exact; up to about 0.145 would keep the worst under 1.5.
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

// Equal-power pan gains for -1..1: unity in the middle (exactly, so L and R are equal there), sqrt 2
// at the sides (PolyForce's).
void panGains(float pan, float& l, float& r) {
    if (pan == 0.0f) {
        l = r = 1.0f;
        return;
    }
    const float a = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.785398163f;   // 0 .. pi/2
    l = 1.41421356f * sinQuarter(1.570796327f - a);
    r = 1.41421356f * sinQuarter(a);
}

int pitchClass(int note) { return (note % 12 + 12) % 12; }

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
    const bool retune = h.key != h_.key || h.tuning != h_.tuning;
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
    breathHz_ = clampParam(p.breathHz, 0.0f, 10.0f, 0.0f);
    breathBeats_ = clampParam(p.breathBeats, 0.0f, 1024.0f, 0.0f);
    wantRegister_ = std::min(std::max(p.registerOct, 1), 3);
    if (!sounding_) register_ = wantRegister_;   // nothing to dip: the next start uses it

    // The ratios, for the tuning as tunedPitch() reads it (out of range: the nearest one).
    const int tuning = std::min(std::max(h.tuning, 0), TU_COUNT - 1);
    const int ci = std::min(std::max(p.colorInterval, 0), CI_COUNT - 1);
    if (tuning * CI_COUNT + ci != ratiosFor_) {
        ratiosFor_ = tuning * CI_COUNT + ci;
        const bool equal = tuning == TU_EQUAL;
        ratio_[PT_SUB] = 0.5f;
        ratio_[PT_ROOT] = 1.0f;
        ratio_[PT_FIFTH] = equal ? std::exp2(7.0f / 12.0f) : 1.5f;
        ratio_[PT_OCTAVE] = 2.0f;
        ratio_[PT_COLOR] = equal ? std::exp2(static_cast<float>(kSemis[ci]) / 12.0f)
                                 : (tuning == TU_JUST ? kJust : kPythagorean)[ci];
    }
    const float width = lifeosc::unit(p.width);
    if (width != widthFor_) {
        widthFor_ = width;
        for (int i = 0; i < kParts; ++i) panGains(width * kPan[i], panL_[i], panR_[i]);
    }
    const float level[kParts] = {lifeosc::unit(p.sub), lifeosc::unit(p.root), lifeosc::unit(p.fifth),
                                 lifeosc::unit(p.octave), lifeosc::unit(p.color)};
    for (int i = 0; i < kParts; ++i) {
        gainL_[i] = kHeadroom * level[i] * panL_[i];
        gainR_[i] = kHeadroom * level[i] * panR_[i];
    }
    // What of the distance to the goal a full step keeps: exp(-t / tau), tau = gravity / 3.
    if (gravity_ != gravityFor_) {
        gravityFor_ = gravity_;
        glideKeep_ = gravity_ > 0.0f ? std::exp(-3.0 * kChunk / (static_cast<double>(gravity_) * kRate)) : 0.0;
    }
    if (retune && note_ >= 0) goal_ = tunedPitch(h_, note_);   // a new key or tuning: it glides there
}

void Ground::setTarget(int rootNote) {
    if (rootNote < 0) {
        target_ = -1;   // fades out from the next step on
        return;
    }
    target_ = rootNote;
    const int pc = pitchClass(rootNote);
    if (!sounding_) {   // from off: in Register's octave, on the note, at the bottom of the fade
        note_ = base() + pc;
        pitch_ = goal_ = tunedPitch(h_, note_);
        fadeDb_ = kFloorDb;
        sounding_ = true;
        return;
    }
    if (pitchClass(note_) == pc) return;   // where it is, or gliding there already
    // The two places of the pitch class within base - 6 .. base + 17; the nearer to the root now.
    const int lo = base() - 6 + pitchClass(pc - (base() - 6));
    const double dLo = std::fabs(tunedPitch(h_, lo) - pitch_), dHi = std::fabs(tunedPitch(h_, lo + 12) - pitch_);
    note_ = dHi < dLo ? lo + 12 : lo;
    goal_ = tunedPitch(h_, note_);
}

void Ground::reset() {
    uint32_t r = seed_ * 0x85EBCA6Bu + 0x7F4A7C15u;   // not the scan's own sequence
    if (r == 0) r = 0x7F4A7C15u;                       // xorshift's one stuck state
    xorshift(r);
    scan_.seed(seed_);
    for (TableOscLinear& o : osc_) o.reset(lifeosc::rand01(r));   // Sub, Root, Fifth, Octave, Color
    breathPhase_ = breathOwn_ = lifeosc::rand01(r);
    target_ = -1;
    std::fill(curL_, curL_ + kParts, 0.0f);
    std::fill(curR_, curR_ + kParts, 0.0f);
    send_ = 0.0f;
    stop();
}

void Ground::stop() {
    sounding_ = false;
    fresh_ = true;
    note_ = -1;
    pitch_ = goal_ = 0.0;
    register_ = wantRegister_;
    dip_ = 1.0f;
    fadeDb_ = kFloorDb;
    gain_ = wet_ = 0.0f;
    fadeFrom_ = nullptr;
    tableFade_ = 0;
    toneHz_ = -1.0f;
    toneSvf_.clear();
    f1Svf_.clear();
    f2Svf_.clear();
}

void Ground::shiftRegister() {
    const int octaves = wantRegister_ - register_;
    register_ = wantRegister_;
    note_ += 12 * octaves;
    pitch_ += 12.0 * octaves;            // a glide under way goes on from the new octave
    goal_ = tunedPitch(h_, note_);       // the old goal, octaves away: the tunings repeat by octaves
}

void Ground::render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR, float spaceSend,
                    int n) {
    spaceSend = clampParam(spaceSend, 0.0f, 1.0f, 0.0f);
    for (int o = 0; o < n && sounding_; o += kChunk)
        chunk(tables.get(table_), outL + o, outR + o, sendL + o, sendR + o, spaceSend, std::min(kChunk, n - o));
}

// One control step, then its n samples: the partials into a stereo sum, Body and Tone, the gains.
//
// Two kinds of "from silence" meet here. fresh_: the drone starts from off, and its first step puts
// the root on its goal (no glide from wherever it was last). s.fromSilence: the output's gain ended
// the last step at 0 (a start, a dip's bottom, a mute or level 0 lifted), so nothing that jumps in
// this step can be heard doing it: the partials' gains, Body's share, its formants and the table go
// straight to where they are aimed.
void Ground::chunk(const Wavetable& want, float* outL, float* outR, float* sendL, float* sendR, float spaceSend, int n) {
    const Step s = control(spaceSend, n);
    if (s.g0 == 0.0f && s.g1 == 0.0f) {
        silent(s, want);
    } else {
        float accL[kChunk] = {}, accR[kChunk] = {}, yL[kChunk], yR[kChunk];
        partials(s, want, accL, accR);
        filters(s, accL, accR, yL, yR);
        output(s, yL, yR, outL, outR, sendL, sendR);
    }
    if (s.ending) stop();
}

Ground::Step Ground::control(float spaceSend, int n) {
    Step s;
    s.n = n;
    const float dt = static_cast<float>(n) * kInvRate;

    // Register: a change dips the drone out; at the bottom (or at once, when nothing is heard) the
    // root moves by the octaves, and it comes back.
    if (wantRegister_ != register_ && (dip_ <= 0.0f || (dip_ >= 1.0f && gain_ == 0.0f))) shiftRegister();
    const float dipStep = static_cast<float>(n) / static_cast<float>(kDip);
    dip_ = wantRegister_ != register_ ? std::max(0.0f, dip_ - dipStep) : std::min(1.0f, dip_ + dipStep);

    // Gravity: the remaining distance shrinks by exp(-dt / tau) a step, in semitones.
    if (fresh_ || gravity_ <= 0.0f) {
        pitch_ = goal_;
    } else {
        const double keep = n == kChunk ? glideKeep_ : std::exp(-3.0 * n / (static_cast<double>(gravity_) * kRate));
        pitch_ = goal_ + (pitch_ - goal_) * keep;
    }
    fresh_ = false;

    // The fade, in dB; at the bottom on the way out, this step ramps to 0 and the drone is off.
    const float fadeStep = 60.0f * dt / fade_;
    if (target_ >= 0) {
        fadeDb_ = std::min(0.0f, fadeDb_ + fadeStep);
    } else {
        fadeDb_ -= fadeStep;
        s.ending = fadeDb_ <= kFloorDb;
    }

    // Breath: +-3 dB and +-1 octave of cutoff at 1. The Tone's coefficients for this step. Its own
    // phase moves on at breathHz all the while; it breathes on that (Free) or on the clock's at this
    // step's end, a quarter cycle on (Sync: the top on the downbeat; the division doubled past
    // kMaxSyncBreathHz), pulled there (pullPhase): a lock, a jump or a switch glides.
    clock_.advance(n);
    breathOwn_ += static_cast<double>(breathHz_ * dt);
    breathOwn_ -= floorFast(breathOwn_);
    if (breathBeats_ > 0.0f) {
        const double beats = clock_.cycleBeats(static_cast<double>(breathBeats_), kMaxSyncBreathHz);
        breathPhase_ = pullPhase(breathPhase_, clock_.cycles(beats, dt), clock_.phase(beats, 0.25), dt);
    } else {
        breathPhase_ = pullPhase(breathPhase_, static_cast<double>(breathHz_ * dt), breathOwn_, dt);
    }
    const float b = breath_ > 0.0f ? breath_ * sinCycle(static_cast<float>(breathPhase_)) : 0.0f;
    const float cutoff = clampf(cutoff_ * exp2Fast(kBreathOct * b), 20.0f, 0.45f * kRate);
    if (cutoff != toneHz_) {
        const float g = svfG(cutoff), jump = exp2Fast(kToneJump);
        if (gain_ > 0.0f && toneHz_ > 0.0f && (cutoff > toneHz_ * jump || cutoff * jump < toneHz_)) {
            s.toneGlides = true;   // a jump of the knob: across the step, evenly in octaves
            s.toneG0 = toneG_;
            s.toneRate = std::pow(g / toneG_, 1.0f / static_cast<float>(n));
        }
        toneHz_ = cutoff;
        toneG_ = g;
        toneUpdate_ = SvfUpdate::of(splat2(g), splat2(kToneK));
    }

    s.pos = scan_.step(pos_, dt, &clock_);

    // The output's gain and the send's, ramped across the step from where the last one ended.
    s.g0 = gain_;
    s.g1 = mute_ || s.ending ? 0.0f : level_ * dbToGain(fadeDb_) * dbToGain(kBreathDb * b) * dip_;
    s.fromSilence = s.g0 == 0.0f;
    s.s0 = s.fromSilence ? spaceSend : send_;
    s.s1 = spaceSend;
    gain_ = s.g1;
    send_ = s.s1;
    s.w1 = std::min(1.0f, 3.0f * body_);
    s.w0 = s.fromSilence ? s.w1 : wet_;
    wet_ = s.w1;

    // Each partial's pitch: its ratio over the root, and Beat's offset in Hz.
    const float rootHz = noteHz(static_cast<float>(pitch_));
    for (int i = 0; i < kParts; ++i) s.inc[i] = std::max(ratio_[i] * rootHz + beat_ * kBeat[i], 0.0f) * kInvRate;
    return s;
}

// Level 0 or muted: nothing to hear, nothing read. The oscillators are skipped along and the
// filters start afresh when it is heard again.
void Ground::silent(const Step& s, const Wavetable& want) {
    table0_ = &want;
    fadeFrom_ = nullptr;
    tableFade_ = 0;
    for (int i = 0; i < kParts; ++i) {
        osc_[i].skip(want, s.inc[i], s.pos, s.n);
        curL_[i] = gainL_[i];
        curR_[i] = gainR_[i];
    }
    toneSvf_.clear();
    f1Svf_.clear();
    f2Svf_.clear();
}

// The partials, summed and panned into accL / accR. A partial at level 0 isn't read: it is skipped
// along. A new table fades in over kTableFade samples, the oscillators as they were at the switch
// reading the old one under it in step; from silence the new one is simply there. A second change
// waits for the fade under way to end.
void Ground::partials(const Step& s, const Wavetable& want, float* accL, float* accR) {
    const int n = s.n;
    if (s.fromSilence) {
        table0_ = &want;
        fadeFrom_ = nullptr;
        tableFade_ = 0;
    } else if (!fadeFrom_ && &want != table0_) {
        fadeFrom_ = table0_;
        table0_ = &want;
        tableFade_ = kTableFade;
        std::copy(osc_, osc_ + kParts, fadeOsc_);
    }
    const Wavetable* const from = fadeFrom_;
    float fade[kChunk];
    if (from) {
        const float done = static_cast<float>(kTableFade - tableFade_);
        for (int j = 0; j < n; ++j) fade[j] = std::min(1.0f, (done + static_cast<float>(j + 1)) * (1.0f / kTableFade));
        tableFade_ -= n;
        if (tableFade_ <= 0) {
            tableFade_ = 0;
            fadeFrom_ = nullptr;
        }
    }

    const Wavetable& t = *table0_;
    const float inv = 1.0f / static_cast<float>(n);
    float x[kChunk], old[kChunk];
    for (int i = 0; i < kParts; ++i) {
        const float l1 = gainL_[i], r1 = gainR_[i];
        const float l0 = s.fromSilence ? l1 : curL_[i], r0 = s.fromSilence ? r1 : curR_[i];
        curL_[i] = l1;
        curR_[i] = r1;
        if (l0 == 0.0f && r0 == 0.0f && l1 == 0.0f && r1 == 0.0f) {
            osc_[i].skip(t, s.inc[i], s.pos, n);
            if (from) fadeOsc_[i].skip(*from, s.inc[i], s.pos, n);
            continue;
        }
        osc_[i].render(t, s.inc[i], s.pos, x, n);
        if (from) {
            fadeOsc_[i].render(*from, s.inc[i], s.pos, old, n);
            for (int j = 0; j < n; ++j) x[j] = old[j] + fade[j] * (x[j] - old[j]);
        }
        const float dl = (l1 - l0) * inv, dr = (r1 - r0) * inv;
        float gl = l0, gr = r0;
        for (int j = 0; j < n; ++j) {
            gl += dl;
            gr += dr;
            accL[j] += gl * x[j];
            accR[j] += gr * x[j];
        }
    }
}

// Body and Tone, L and R together, into yL / yR. The formants' and the Tone's updates are worked
// out per sample only while they glide. The states and the glide run as locals: the members could alias the float
// buffers, and would be loaded and stored again every sample.
void Ground::filters(const Step& s, const float* accL, const float* accR, float* yL, float* yR) {
    const int n = s.n;
    const bool body = s.w0 > 0.0f || s.w1 > 0.0f;
    const f2 kForm = splat2(kBodyK);
    if (body) {   // the formants' targets: a, then a -> o -> u
        const float v = clampf(3.0f * body_ - 1.0f, 0.0f, 2.0f);
        const int k = std::min(static_cast<int>(v), 1);
        const float u = v - static_cast<float>(k);
        const f2 formants[2] = {splat2(svfG(kVowel[k][0] + u * (kVowel[k + 1][0] - kVowel[k][0]))),
                                splat2(svfG(kVowel[k][1] + u * (kVowel[k + 1][1] - kVowel[k][1])))};
        if (s.fromSilence || s.w0 == 0.0f) formant_.jump(formants);   // coming in from nothing: no glide to hear
        else formant_.to(formants);
    }
    const f2 lowPass[3] = {splat2(0.0f), splat2(0.0f), splat2(1.0f)};
    const f2 bandPass[3] = {splat2(0.0f), kForm, splat2(0.0f)};   // unity at the formant
    Glide<2, 2> formant = formant_;
    SvfState toneSvf = toneSvf_, f1Svf = f1Svf_, f2Svf = f2Svf_;
    const f2 kTone = splat2(kToneK);
    SvfUpdate tu = s.toneGlides ? SvfUpdate::of(splat2(s.toneG0), kTone) : toneUpdate_;
    float toneG = s.toneG0;
    const bool glides = body && formant.moving();
    SvfUpdate u1 = tu, u2 = tu;
    if (body) {
        u1 = SvfUpdate::of(formant.cur()[0], kForm);
        u2 = SvfUpdate::of(formant.cur()[1], kForm);
    }
    const float dw = (s.w1 - s.w0) / static_cast<float>(n);
    float w = s.w0;
    for (int j = 0; j < n; ++j) {
        f2 v = load2(accL + j, accR + j);
        if (body) {
            w += dw;
            if (glides) {
                formant.next();
                u1 = SvfUpdate::fast(formant.cur()[0], kForm);
                u2 = SvfUpdate::fast(formant.cur()[1], kForm);
            }
            const f2 b1 = f1Svf.tick(v, u1, bandPass), b2 = f2Svf.tick(v, u2, bandPass);
            v = v * splat2(1.0f - kBodyDry * w) + (b1 + splat2(kF2) * b2) * splat2(kBodyWet * w);
        }
        if (s.toneGlides) {   // lands on the step's own coefficients at its last sample
            toneG *= s.toneRate;
            tu = j == n - 1 ? toneUpdate_ : SvfUpdate::fast(splat2(toneG), kTone);
        }
        store2(yL + j, yR + j, toneSvf.tick(v, tu, lowPass));
    }
    formant_ = formant;
    toneSvf.flushTiny();
    toneSvf_ = toneSvf;
    if (body && s.w1 == 0.0f) {   // faded out: it starts afresh when Body comes back
        f1Svf_.clear();
        f2Svf_.clear();
    } else if (body) {
        f1Svf.flushTiny();
        f2Svf.flushTiny();
        f1Svf_ = f1Svf;
        f2Svf_ = f2Svf;
    }
}

// The output and the send, their gains ramping across the step.
void Ground::output(const Step& s, const float* yL, const float* yR, float* outL, float* outR, float* sendL,
                    float* sendR) {
    const float inv = 1.0f / static_cast<float>(s.n);
    const float dg = (s.g1 - s.g0) * inv, ds = (s.s1 - s.s0) * inv;
    float g = s.g0, sg = s.s0;
    for (int j = 0; j < s.n; ++j) {
        g += dg;
        sg += ds;
        const float l = yL[j] * g, r = yR[j] * g;
        outL[j] += l;
        outR[j] += r;
        sendL[j] += l * sg;
        sendR[j] += r * sg;
    }
}

} // namespace af
