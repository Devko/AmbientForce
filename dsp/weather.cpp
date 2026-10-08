// Weather: see weather.h. Its grains from EffectForce dsp/grain.cpp (d94bde0) and dsp/grain.h
// (f78d36a), namespace ef -> af: the voice and its envelope (ramp, release, window4), the staged
// reads (render -> renderVoice, readVoice, cubic2), and Cloud's and Stretch's spawning (grains ->
// startGrains, spawn), reading a fixed 16-bit source instead of a live float recording. The
// recording, room() and guard() (a fixed source has no young or old end), Mosaic, Stutter, Arp,
// the grid and the feedback stayed behind. Changed for the 16-bit samples and the budget: the
// cubic takes its taps' differences as integers as they widen, the window of a grain not released
// runs on a recurrence, and the fractions and envelopes are staged once a sample (two to a vector
// as they load). Stream, To Key, the anchor's wander, the gate, Duck, the tilt and high-pass and the
// copies of an old source are Weather's own.
#include "weather.h"

#include "simd.h"
#include "svf.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// On the device a read's address fits a 32-bit lane, so the staged reads hold addresses (one
// instruction less a read); elsewhere they hold offsets into the ring, in int16s.
#if AF_NEON && UINTPTR_MAX == 0xFFFFFFFFu
#define AF_TAP_ADDRESSES 1
#else
#define AF_TAP_ADDRESSES 0
#endif

namespace af {

namespace {

// --- reads (EffectForce's, the samples 16 bit) -------------------------------------------------

AF_INLINE f2 loadPair(const float* p) {
#if AF_NEON
    return vld1_f32(p);
#else
    return f2{p[0], p[1]};
#endif
}
AF_INLINE void storePair(float* p, f2 v) {
#if AF_NEON
    vst1_f32(p, v);
#else
    p[0] = v[0];
    p[1] = v[1];
#endif
}

// The 4-point, 3rd-order Hermite between x0 and x1 at t (common.h's hermite()), on both sides of an
// interleaved stereo ring, in de Soras's arrangement (EffectForce's cubicOf):
//   y = x0 + t (c1 + t (a t - b)),   c1 = d1 / 2,   a = c1 + 2 d2 + d3 / 2,   b = a + c1 + d2,
// with d1 = x1 - x[-1], d2 = x0 - x1, d3 = x2 - x0. On 16-bit taps the differences are taken as
// the samples widen, and the conversion to float halves d1 and d3 itself (a fixed-point
// conversion); every sum of them is a whole or half number, exact in float. So the 16-bit samples
// cost about what floats would. Two samples at once: q and r point at their windows' x[-1] left
// sample (L-1 R-1 L0 R0 L1 R1 L2 R2), t is their fractions (t0 t0 t1 t1); the result L R of the
// first, then of the second. On NEON one load per window; a zip of the two as 32-bit pairs (a
// frame a lane) puts each tap's frames of both side by side.
AF_INLINE f4 cubic2(const int16_t* q, const int16_t* r, f4 t) {
#if AF_NEON
    const uint32x4x2_t z = vzipq_u32(vreinterpretq_u32_s16(vld1q_s16(q)), vreinterpretq_u32_s16(vld1q_s16(r)));
    // (x-1 | x0), (x1 | x2): each half both windows' frame, L R and L R.
    const int16x8_t lo = vreinterpretq_s16_u32(z.val[0]), hi = vreinterpretq_s16_u32(z.val[1]);
    const int16x4_t x0 = vget_high_s16(lo), x1 = vget_low_s16(hi);
    const f4 c1 = vcvtq_n_f32_s32(vsubl_s16(x1, vget_low_s16(lo)), 1);
    const f4 d2 = vcvtq_f32_s32(vsubl_s16(x0, x1)), h3 = vcvtq_n_f32_s32(vsubl_s16(vget_high_s16(hi), x0), 1);
    const f4 a = vaddq_f32(vaddq_f32(c1, h3), vaddq_f32(d2, d2)), b = vaddq_f32(vaddq_f32(a, c1), d2);
    return vfmaq_f32(vcvtq_f32_s32(vmovl_s16(x0)), t, vfmsq_f32(c1, t, vfmsq_f32(b, t, a)));
#else
    const auto f = [](int16_t v) { return static_cast<float>(v); };
    const f4 xm1{f(q[0]), f(q[1]), f(r[0]), f(r[1])}, x0{f(q[2]), f(q[3]), f(r[2]), f(r[3])};
    const f4 x1{f(q[4]), f(q[5]), f(r[4]), f(r[5])}, x2{f(q[6]), f(q[7]), f(r[6]), f(r[7])};
    const f4 c1 = splat(0.5f) * (x1 - xm1), d2 = x0 - x1, h3 = splat(0.5f) * (x2 - x0);
    const f4 a = (c1 + h3) + (d2 + d2), b = (a + c1) + d2;
    return x0 + t * (c1 - t * (b - t * a));
#endif
}

// A grain's two samples (y: L R, L R) into the sum: its own side's gain and, panned, the other
// side's (Cross), times the envelope (e0 e0 e1 e1).
template <bool Cross>
AF_INLINE f4 addGrain(f4 acc, f4 y, f4 gd, f4 gx, f4 env) {
#if AF_NEON
    f4 g = vmulq_f32(gd, y);
    if (Cross) g = vfmaq_f32(g, gx, vrev64q_f32(y));
    return vfmaq_f32(acc, g, env);
#else
    f4 g = gd * y;
    if (Cross) g += gx * f4{y[1], y[0], y[3], y[2]};
    return acc + g * env;
#endif
}

// Two values, each twice: (p[0] p[0] p[1] p[1]). On NEON one load fills each half with one of them.
AF_INLINE f4 loadTwice(const float* p) {
#if AF_NEON
    const float32x2x2_t d = vld2_dup_f32(p);
    return vcombine_f32(d.val[0], d.val[1]);
#else
    return f4{p[0], p[0], p[1], p[1]};
#endif
}

// The envelope's gain from its ramp a (0..1): sin^2(pi / 2 a). sin as fastmath.h's sinQuarter, its
// last coefficient moved (2.7557e-6 -> 2.6949e-6) so it reaches exactly 1 at pi / 2 instead of
// overshooting by 4e-6 (error 2.6e-7 throughout): a release starting at a ramp a hair under 1
// would otherwise step the gain up by 7e-6. Never over 1.
constexpr float kS3 = -1.666666667e-1f, kS5 = 8.333333333e-3f, kS7 = -1.984126984e-4f, kS9 = 2.694884624e-6f;
AF_INLINE f4 sinPoly(f4 x) {   // |x| <= pi / 2
    const f4 x2 = x * x;
    return x * (splat(1.0f) + x2 * (splat(kS3) + x2 * (splat(kS5) + x2 * (splat(kS7) + x2 * splat(kS9)))));
}
AF_INLINE f4 window4(f4 a) {
    const f4 s = sinPoly(a * splat(1.57079633f));
    const f4 w = min4(s * s, splat(1.0f));
#if AF_NEON
    return vbslq_f32(vcgeq_f32(a, splat(1.0f)), splat(1.0f), w);   // exactly 1 when sustained
#else
    return a >= splat(1.0f) ? splat(1.0f) : w;
#endif
}
// sin(x) for x in -3/2 pi .. 3/2 pi, folded onto -pi/2 .. pi/2 (sin(pi - x) = sin x).
AF_INLINE f4 sinWide(f4 x) {
    const f4 half = splat(1.57079633f), pi = splat(3.14159265f);
#if AF_NEON
    x = vbslq_f32(vcgtq_f32(x, half), vsubq_f32(pi, x), x);
    x = vbslq_f32(vcltq_f32(x, vnegq_f32(half)), vsubq_f32(vnegq_f32(pi), x), x);
#else
    x = x > half ? pi - x : x;
    x = x < -half ? -pi - x : x;
#endif
    return sinPoly(x);
}

// floor() for |x| < 2^31 (a ring position), without the library or floorFast's range check.
AF_INLINE int floorInt(double x) {
    const int i = static_cast<int>(x);
    return static_cast<double>(i) > x ? i - 1 : i;
}

// std::ceil, which is a libm call on ARMv7.
double ceilFast(double x) { return -floorFast(-x); }

// x into [0, size).
double wrapTo(double x, double size) {
    const double r = x - floorFast(x / size) * size;
    return r >= size ? r - size : (r < 0.0 ? 0.0 : r);
}

// --- constants ----------------------------------------------------------------------------------

constexpr double kSr = 44100.0;
// Level L's frame m stands for level-0 time 2^L m + kOffset[L]: the decimator takes the pair
// (2m, 2m + 1) and delays low frequencies by 3.19 input samples (its allpasses' delay at DC).
constexpr double kTau = 3.19;
constexpr double kOffset[3] = {0.0, 1.0 - kTau, 3.0 - 3.0 * kTau};
constexpr double kLevel1 = 1.26, kLevel2 = 2.2;   // a grain faster than these reads level 1, 2
constexpr int kOpen = 1 << 24;                    // a grain's release end until it is released
constexpr float kDetune = 6.0f;                   // Cloud's random detune at Spray 1, +- cents
constexpr double kStretchSpeed = 0.125;           // Stretch's head: 1/8 of real time
constexpr float kStreamMinS = 0.2f;               // Stream's grains are at least this long
// A transposed Stream grain's level: two uncorrelated neighbours overlap at 3/4 of their power.
const float kStreamPitched = std::sqrt(4.0f / 3.0f);
constexpr float kFloorDb = -60.0f;                // the gate's bottom: closed from here
constexpr float kGateDbPerSample = 60.0f / (Weather::kGateS * 44100.0f);
constexpr float kLevelGlide = 441.0f;             // the level and the mute glide 10 ms
constexpr float kHpOffHz = 20.0f;                 // the high-pass at its bottom is off
constexpr float kHpOctaves = 0.25f;               // its glide: 1/4 octave a 32-sample step at most
constexpr float kDuckLaw = 16.0f;
constexpr float kDuckClamp = 8.0f;                // +18 dBFS: the most Duck's envelope takes in
// Duck's envelope: a 50 ms time constant up, 60 dB in 1.5 s down; per sample in exp2Fast's terms
// (the coefficient for n samples is 1 - 2^(-n c)).
constexpr float kDuckUp = 1.0f / (0.05f * 44100.0f * 0.693147181f);
constexpr float kDuckDown = 6.90775528f / (1.5f * 44100.0f * 0.693147181f);   // ln 1000 / 1.5 s
// The wander: Drift 1 crosses the source in kDriftSpanS; its direction glides (kDirS) toward a
// new target every 1 to 3 s.
constexpr float kDriftSpanS = 20.0f, kDirS = 0.5f;
constexpr int kExactSteps = 16;                   // the window's recurrence starts exactly afresh this often
constexpr int kCopyMargin = 48;                   // frames either side of a grain's last reads: a step's reach
constexpr int kCopyStride = Weather::kCopyFrames + 64;
constexpr uint32_t kSeedMix = 0x9E3779B1u;

int levelOf(double speed) { return speed > kLevel2 ? 2 : (speed > kLevel1 ? 1 : 0); }

bool sameSource(const GrainSource& a, const GrainSource& b) {
    return a.frames == b.frames && a.origin == b.origin && a.gain == b.gain && a.level[0] == b.level[0] &&
           a.level[1] == b.level[1] && a.level[2] == b.level[2];
}

// The envelope's ramp at the voice's own time t.
template <class V>
float ramp(const V& v, int t) {
    const float tf = static_cast<float>(t);
    float a = std::min(tf * v.slope, (static_cast<float>(v.end) - tf) * v.slope);
    a = std::min(a, std::min((static_cast<float>(v.relEnd) - tf) * v.relSlope, 1.0f));
    return std::max(a, 0.0f);
}

} // namespace

Weather::Weather() : copy_(static_cast<size_t>(2 * kGrains * kCopyStride), 0) {
    set(WeatherPatch{}, HarmonyPatch{});
    reset();
}

void Weather::seed(uint32_t s) {
    seed_ = s;
    reset();
}

float Weather::uniform() { return static_cast<float>(xorshift(rng_) >> 8) * (1.0f / 16777216.0f); }

void Weather::set(const WeatherPatch& p, const HarmonyPatch& h) {
    mute_ = p.mute;
    level_ = clampParam(p.level, 0.0f, 1.0f, 0.0f);
    const int mode = std::min(std::max(p.mode, 0), WM_COUNT - 1);
    if (mode != mode_) {   // every grain fades out, the new mode starts at once
        mode_ = mode;
        dropAll_ = !closed_;
        begin();
    }
    position_ = clampParam(p.position, 0.0f, 1.0f, 0.5f);
    drift_ = clampParam(p.drift, 0.0f, 1.0f, 0.0f);
    spray_ = clampParam(p.spray, 0.0f, 1.0f, 0.0f);
    sizeS_ = clampParam(p.sizeS, 0.02f, 2.0f, 0.25f);
    grains_ = std::min(std::max(p.grains, 1), kGrains);
    pitch_ = clampParam(p.pitch, -24.0f, 24.0f, 0.0f);
    toKey_ = std::min(std::max(p.toKey, 0), TK_COUNT - 1);
    reverse_ = clampParam(p.reverse, 0.0f, 1.0f, 0.0f);
    width_ = clampParam(p.width, 0.0f, 1.0f, 0.0f);
    tilt_ = clampParam(p.tilt, -1.0f, 1.0f, 0.0f);
    hpHz_ = clampParam(p.hpHz, kHpOffHz, 2000.0f, kHpOffHz);
    duck_ = clampParam(p.duck, 0.0f, 1.0f, 0.0f);
    if (grains_ != grainsFor_) {
        grainsFor_ = grains_;
        cloudAmp_ = 1.0f / std::sqrt(0.375f * static_cast<float>(grains_));
    }
    // The scale's pitch classes, for To Key Scale (and Chord without a chord).
    const int key = ((h.key % 12) + 12) % 12, scale = std::min(std::max(h.scale, 0), SC_COUNT - 1);
    nKeyPcs_ = scaleSize(scale);
    for (int d = 0; d < nKeyPcs_; ++d) keyPcs_[d] = (key + scaleStep(scale, d)) % 12;
    // Nothing to hear (closed, or the level or the mute at 0 and the gain already down there):
    // everything where it is aimed, so no render() can glide it back up (only set() or gate() makes
    // it audible again), and no grain or source kept (silence()).
    if (!audible()) {
        levelNow_ = levelTo_ = mute_ ? 0.0f : level_;
        levelStep_ = 0.0f;
        tiltNow_ = tilt_;
        hpNow_ = hpHz_;
        silence();
    }
}

void Weather::setChord(uint16_t pcs) {
    nChordPcs_ = 0;
    for (int pc = 0; pc < 12; ++pc)
        if ((pcs >> pc) & 1) chordPcs_[nChordPcs_++] = pc;
}

void Weather::gate(bool on) {
    gateOn_ = on;
    if (on && closed_) {   // from silence: the modes start from the anchor afresh
        closed_ = false;
        ending_ = false;
        gateDb_ = kFloorDb;
        gain_ = gain0_ = 0.0f;
        begin();
    }
}

void Weather::reset() {
    uint32_t r = seed_ * kSeedMix + 0x7F4A7C15u;
    if (r == 0) r = 0x7F4A7C15u;   // xorshift's one stuck state
    xorshift(r);
    rng_ = r;
    silence();
    now_ = 0;
    begin();
    dropAll_ = false;
    wander_ = dir_ = dirTarget_ = dirLeftS_ = 0.0f;
    anchor_ = 0.0;
    gateOn_ = false;
    closed_ = true;
    ending_ = false;
    gateDb_ = kFloorDb;
    levelNow_ = levelTo_ = mute_ ? 0.0f : level_;
    levelStep_ = 0.0f;
    gain_ = gain0_ = send_ = send0_ = 0.0f;
    duckEnv_ = 0.0f;
    duckGain_ = 1.0f;
    tiltNow_ = tilt_;
    hpNow_ = hpHz_;
    tiltFor_ = hpFor_ = -1.0f;   // worked out afresh at the first step
}

// Nothing more to hear (the gate closed, the level or the mute glided to 0, a reset): every grain
// stops and the source is forgotten. The engine may skip Weather from here on, and the loader or a
// Remember may free the source meanwhile, so nothing may keep a pointer into it: a grain still
// reading it would be copied from it at the next source change. The next render() with something
// to hear takes its source afresh (changeSource: the modes from the anchor, a grain at once).
void Weather::silence() {
    for (Voice& v : voice_) {
        v.on = false;
        v.data = nullptr;
    }
    tiltS_[0] = tiltS_[1] = hpS_[0] = hpS_[1] = 0.0f;
    src_ = GrainSource{};
    hasSrc_ = false;
}

// Stream and Stretch start from the anchor afresh from here, and the next grain comes at once.
void Weather::begin() {
    began_ = now_;
    nextSpawn_ = static_cast<double>(now_);
    stream_ = SJ_NONE;
}

int Weather::grainsOn() const {
    int n = 0;
    for (const Voice& v : voice_) n += v.on ? 1 : 0;
    return n;
}

// --- the source and releases ----------------------------------------------------------------------

// Fades a voice out over `fade` samples from the next step's first sample on (its own time v.t
// there). The release starts at the envelope's value there and only falls faster than a fade
// under way (a fade-out ending sooner is left alone).
void Weather::release(Voice& v, int fade) {
    const int at = v.t;
    if (at <= 0) {   // it hadn't started
        v.on = false;
        return;
    }
    if (std::min(v.end, v.relEnd) - at <= fade) return;
    v.relSlope = ramp(v, at) / static_cast<float>(fade);
    v.relEnd = at + fade;
}

// Another source, or none. Every grain fades out over kModeFade; one still reading the old source
// reads a copy of what it has left to read from here on (its positions over the rest of its life,
// with a step's reach either side), so the old source may go as soon as this call returns. The
// modes start from the anchor afresh, and the next grain comes at once.
void Weather::changeSource(const GrainSource* src) {
    for (int i = 0; i < kGrains; ++i) {
        Voice& v = voice_[i];
        if (!v.on) continue;
        release(v, kModeFade);
        if (!v.on || v.copied) continue;
        const double p0 = v.start + v.t * v.rate, p1 = v.start + std::min(v.end, v.relEnd) * v.rate;
        const double lo = floorFast(std::min(p0, p1)) - kCopyMargin;
        const int n = std::min(kCopyFrames, static_cast<int>(floorFast(std::max(p0, p1)) - lo) + kCopyMargin + 4);
        int16_t* const to = copy_.data() + 2 * static_cast<size_t>(i) * kCopyStride;
        int f = static_cast<int>(wrapTo(lo, v.frames));
        for (int done = 0; done < n;) {
            const int run = std::min(n - done, v.frames - f);
            std::memcpy(to + 2 * done, v.data + 2 * f, sizeof(int16_t) * 2 * static_cast<size_t>(run));
            done += run;
            f = 0;
        }
        v.start -= lo;
        v.data = to;
        v.frames = kCopyFrames;
        v.copied = true;
    }
    hasSrc_ = src != nullptr;
    src_ = src ? *src : GrainSource{};
    begin();
}

// --- rendering ----------------------------------------------------------------------------------

void Weather::render(const GrainSource* src, float duckPeak, float* outL, float* outR, float* sendL, float* sendR,
                     float spaceSend, int n) {
    if (n <= 0) return;
    // The source is looked at only with something to hear: silent, Weather holds none (silence()),
    // and no grain starts.
    if (audible()) {
        const bool ready = src && src->ready() && src->frames >= 4;
        if (ready != hasSrc_ || (ready && !sameSource(*src, src_))) changeSource(ready ? src : nullptr);
    }
    if (closed_) return;
    spaceSend_ = clampParam(spaceSend, 0.0f, 1.0f, 0.0f);
    if (dropAll_) {
        for (Voice& v : voice_)
            if (v.on) release(v, kModeFade);
        dropAll_ = false;
    }

    // Duck: the envelope takes this call's peak; its gain glides across the call.
    const float pk = std::min(std::fabs(sanitize(duckPeak)), kDuckClamp), nf = static_cast<float>(n);
    const float k = pk > duckEnv_ ? kDuckUp : kDuckDown;
    duckEnv_ += (pk - duckEnv_) * (1.0f - exp2Fast(-nf * k));
    if (duckEnv_ < 1e-20f) duckEnv_ = 0.0f;
    const float d0 = duckGain_, d1 = duck_ > 0.0f ? 1.0f / (1.0f + kDuckLaw * duck_ * duckEnv_) : 1.0f;
    const float dStep = (d1 - d0) / nf;
    duckGain_ = d1;

    for (int o = 0; o < n && !closed_; o += kChunk) {
        const int m = std::min(kChunk, n - o);
        control(m);
        step(m, d0 + dStep * static_cast<float>(o), dStep, outL + o, outR + o, sendL + o, sendR + o);
        now_ += static_cast<uint64_t>(m);
        if (ending_) {   // the gate's bottom: every grain stops
            silence();
            closed_ = true;
            ending_ = false;
            gain_ = gain0_ = 0.0f;
            levelNow_ = levelTo_ = mute_ ? 0.0f : level_;
            levelStep_ = 0.0f;
        } else if (!audible()) {   // the level or the mute has glided to 0: the same
            silence();
        }
    }
}

// One control step of m <= kChunk samples: the gate, the level's glide, the gains, the wander and
// the anchor, the tilt's and the high-pass's glides and coefficients.
void Weather::control(int m) {
    const float n = static_cast<float>(m), dt = n * (1.0f / 44100.0f);
    if (gateOn_) gateDb_ = std::min(0.0f, gateDb_ + kGateDbPerSample * n);
    else gateDb_ -= kGateDbPerSample * n;
    ending_ = !gateOn_ && gateDb_ <= kFloorDb;

    // The level and the mute: a straight line over 10 ms from wherever they were.
    const float target = mute_ ? 0.0f : level_;
    if (target != levelTo_) {
        levelTo_ = target;
        levelStep_ = (target - levelNow_) / kLevelGlide;
    }
    if (levelNow_ != target) {   // (a step under half an ulp lands, as past the target)
        const float x = levelNow_ + levelStep_ * n;
        levelNow_ = x == levelNow_ || (levelStep_ > 0.0f ? x >= target : x <= target) ? target : x;
    }
    gain0_ = gain_;
    gain_ = ending_ ? 0.0f : levelNow_ * dbToGain(std::max(gateDb_, kFloorDb));
    send0_ = gain0_ == 0.0f ? spaceSend_ : send_;
    send_ = spaceSend_;

    // The wander: its direction glides to a target drawn every 1 to 3 s, and turns back at +-0.5.
    dirLeftS_ -= dt;
    if (dirLeftS_ <= 0.0f) {
        const float u1 = uniform(), u2 = uniform(), u3 = uniform();
        dirTarget_ = (u1 < 0.5f ? -1.0f : 1.0f) * (0.5f + 0.5f * u2);
        dirLeftS_ = 1.0f + 2.0f * u3;
    }
    dir_ += (dirTarget_ - dir_) * (dt / kDirS);
    wander_ += drift_ * drift_ / kDriftSpanS * dir_ * dt;
    if (wander_ > 0.5f || wander_ < -0.5f) {
        wander_ = std::copysign(1.0f, wander_) - wander_;
        dir_ = -dir_;
        dirTarget_ = -dirTarget_;
    }
    const double share = static_cast<double>(position_) + static_cast<double>(wander_);
    anchor_ = share - floorFast(share);

    // The tilt glides as the output's does (the whole range in 50 ms); the high-pass by octaves.
    if (tiltNow_ != tilt_) {
        const float step = TiltShelf::kStep * n * (1.0f / kChunk);
        tiltNow_ = std::fabs(tilt_ - tiltNow_) <= step ? tilt_ : tiltNow_ + std::copysign(step, tilt_ - tiltNow_);
    }
    if (tiltNow_ != tiltFor_) {
        tiltFor_ = tiltNow_;
        const TiltShelf s = TiltShelf::of(tiltNow_);
        tiltHigh_ = s.high;
        tiltLow_ = s.low;
        tiltG_ = s.G;
    }
    if (hpNow_ != hpHz_) {
        const float by = exp2Fast(kHpOctaves * n * (1.0f / kChunk)), up = hpHz_ / hpNow_;
        hpNow_ = up > by ? hpNow_ * by : (up * by < 1.0f ? hpNow_ / by : hpHz_);
    }
    if (hpNow_ != hpFor_) {
        hpFor_ = hpNow_;
        const float g = tanFast(kPi * hpNow_ / kRate);
        hpG_ = g / (1.0f + g);
    }
}

// A grain's transposition: Pitch (plus detune), or To Key's pitch class nearest Pitch.
float Weather::transposition(float detune, float u) {
    if (toKey_ == TK_OFF) return pitch_ + detune;
    const bool chord = toKey_ == TK_CHORD && nChordPcs_ > 0;
    const int* const pcs = chord ? chordPcs_ : keyPcs_;
    const int n = chord ? nChordPcs_ : nKeyPcs_;
    const int q = pcs[std::min(n - 1, static_cast<int>(u * static_cast<float>(n)))];
    const int base = static_cast<int>(floorFast(pitch_ + 0.5f));
    int d = ((q - base) % 12 + 12) % 12;   // 0..11 above base, then -6..+5
    if (d > 5) d -= 12;
    const int t = base + d;
    return static_cast<float>(t > 24 ? t - 12 : t);
}

// The grains due in this step (its first sample now_), at the sample their interval puts them on.
// Every draw first, the same ones whatever the mode and whether or not a grain finds a voice.
void Weather::startGrains(int m) {
    const double now = static_cast<double>(now_);
    if (!hasSrc_) {   // none start; the first comes as soon as there is a source
        nextSpawn_ = now + m;
        return;
    }
    const double frames = static_cast<double>(src_.frames);
    if (mode_ == WM_STREAM) {
        // Exactly half a grain apart, each from where the read point has got to.
        const int length = 2 * std::max(1, static_cast<int>(std::max(sizeS_, kStreamMinS) * 0.5f * kRate + 0.5f));
        const int hop = length / 2;
        if (nextSpawn_ < now - hop) nextSpawn_ = now;
        while (nextSpawn_ < now + m) {
            const int k = nextSpawn_ <= now ? 0 : static_cast<int>(ceilFast(nextSpawn_ - now));
            if (k >= m) break;
            for (int i = 0; i < 5; ++i) uniform();
            const float u6 = uniform();
            nextSpawn_ += hop;
            const double since = static_cast<double>(now_ + static_cast<uint64_t>(k) - began_);
            const float semis = transposition(0.0f, u6);
            // At 1 only reading on exactly where the last grain does: at rate 1, after one at rate
            // 1, the anchor unmoved (no Drift, no new Position). Otherwise the two are uncorrelated.
            const bool joins =
                semis == 0.0f && (stream_ == SJ_NONE || (stream_ == SJ_JOINS && anchor_ == streamAnchor_));
            stream_ = semis == 0.0f ? SJ_JOINS : SJ_SHIFTED;
            streamAnchor_ = anchor_;
            spawn(k, anchor_ * frames + since, semis, false, length, 0.0f, joins ? 1.0f : kStreamPitched);
        }
        return;
    }
    // Cloud and Stretch: intervals of length / grains, jittered (Cloud +-30%, Stretch +-15%).
    const bool stretch = mode_ == WM_STRETCH;
    const int length = std::max(static_cast<int>(sizeS_ * kRate + 0.5f), 2);
    const double interval = length / static_cast<double>(grains_);
    const double behind = (0.005 + 0.06 * spray_) * kSr;                         // Stretch's scatter
    const double scatter = std::min(static_cast<double>(spray_ * spray_) * 2.0 * kSr, 0.5 * frames);   // Cloud's
    if (nextSpawn_ < now - interval) nextSpawn_ = now;
    while (nextSpawn_ < now + m) {
        const int k = nextSpawn_ <= now ? 0 : static_cast<int>(ceilFast(nextSpawn_ - now));
        if (k >= m) break;
        const float u1 = uniform(), u2 = uniform(), u3 = uniform(), u4 = uniform(), u5 = uniform(), u6 = uniform();
        nextSpawn_ += interval * (stretch ? 0.85 + 0.3 * u1 : 0.7 + 0.6 * u1);
        const float semis = transposition(stretch ? 0.0f : kDetune * 0.01f * spray_ * (2.0f * u2 - 1.0f), u6);
        double pos;
        if (stretch) {   // the head, 1/8 of the time since Stretch began on from the anchor; just behind it
            const double since = static_cast<double>(now_ + static_cast<uint64_t>(k) - began_);
            pos = anchor_ * frames + since * kStretchSpeed - behind * u4;
        } else {
            pos = anchor_ * frames + scatter * (2.0 * u4 - 1.0);
        }
        spawn(k, pos, semis, u3 < reverse_, length, width_ * (2.0f * u5 - 1.0f), cloudAmp_);
    }
}

// A new voice at sample k of the step, at `pos` (level-0 frames from the source's start), `semis`
// away, a Hann window `length` samples long. Panned: the side away from the pan loses gain at
// constant power (sqrt 2 at the near side when hard over), and the grain's own stereo narrows
// toward mono as it moves out, so a hard-panned grain keeps both channels' material (EffectForce's
// law).
void Weather::spawn(int k, double pos, float semis, bool back, int length, float pan, float amp) {
    Voice* v = nullptr;
    for (Voice& c : voice_)
        if (!c.on) {
            v = &c;
            break;
        }
    if (!v) return;
    if (hook_) hook_(hookCtx_, semis);
    const double speed = static_cast<double>(exp2Fast(semis * (1.0f / 12.0f)));
    int level = levelOf(speed);
    while (level > 0 && !src_.level[level]) --level;   // (a source without its slower levels)
    const double scale = static_cast<double>(1 << level);
    const int frames = src_.frames >> level;
    v->start = wrapTo((src_.origin + pos - kOffset[level]) / scale, frames);
    v->rate = (back ? -speed : speed) / scale;
    v->data = src_.level[level];
    v->frames = frames;
    v->t = -k;
    v->end = length;
    v->relEnd = kOpen;
    v->slope = 2.0f / static_cast<float>(length);   // up for half its length, down for the other half
    v->relSlope = 1.0f;
    v->phi = static_cast<float>(3.14159265358979323846 / length);
    const double half = std::sin(2.0 * 3.14159265358979323846 / length);
    v->k = static_cast<float>(4.0 * half * half);
    v->dk = static_cast<float>(2.0 * half);
    v->exactIn = 0;
    const float pc = clampf(pan, -1.0f, 1.0f), mid = 0.5f * std::fabs(pc), g = amp * src_.gain;
    float gl = 1.0f, gr = 1.0f;
    if (pc != 0.0f) {
        const float th = (pc + 1.0f) * (0.25f * kPi);   // 0 .. pi/2
        gl = 1.41421356f * sinQuarter(0.5f * kPi - th);
        gr = 1.41421356f * sinQuarter(th);
    }
    v->gd[0] = g * gl * (1.0f - mid);
    v->gd[1] = g * gr * (1.0f - mid);
    v->gx[0] = g * gl * mid;
    v->gx[1] = g * gr * mid;
    v->on = true;
    v->copied = false;
}

// One step of m samples: the grains due, every grain into acc_, then the tilt, the high-pass and the
// gains (the step's ramp times Duck's, d0 at the step's start, dStep a sample) into the dry and the
// send. A step with nothing to hear reads nothing: any grain moves on unread and the filters start
// afresh (once nothing more can be heard, render() stops every grain: silence()).
void Weather::step(int m, float d0, float dStep, float* outL, float* outR, float* sendL, float* sendR) {
    startGrains(m);
    const bool heard = gain0_ > 0.0f || gain_ > 0.0f;
    bool any = false;
    if (heard) std::fill(acc_, acc_ + 2 * (m + 4), 0.0f);   // (and the room past it: see readVoice)
    for (Voice& v : voice_)
        if (v.on) {
            if (heard) renderVoice(v, m);
            any = true;
            v.t += m;
            if (v.t >= std::min(v.end, v.relEnd)) v.on = false;
        }
    const bool tilt = tiltNow_ != 0.0f, hp = hpNow_ > kHpOffHz;
    if (!tilt) tiltS_[0] = tiltS_[1] = 0.0f;
    if (!hp) hpS_[0] = hpS_[1] = 0.0f;
    if (!heard) {
        tiltS_[0] = tiltS_[1] = hpS_[0] = hpS_[1] = 0.0f;
        return;
    }
    if (!any && tiltS_[0] == 0.0f && tiltS_[1] == 0.0f && hpS_[0] == 0.0f && hpS_[1] == 0.0f) return;   // silence

    // The filters, in place over acc_ (L R side by side), their states as locals.
    if (tilt || hp) {
        f2 ts = f2{tiltS_[0], tiltS_[1]}, hs = f2{hpS_[0], hpS_[1]};
        const f2 tG = splat2(tiltG_), tHi = splat2(tiltHigh_), tD = splat2(tiltLow_ - tiltHigh_), hG = splat2(hpG_);
        for (int j = 0; j < m; ++j) {
            f2 x = loadPair(acc_ + 2 * j);
            if (tilt) {
                const f2 v = (x - ts) * tG, lp = v + ts;
                ts = lp + v;
                x = tHi * x + tD * lp;
            }
            if (hp) {
                const f2 v = (x - hs) * hG, lp = v + hs;
                hs = lp + v;
                x = x - lp;
            }
            storePair(acc_ + 2 * j, x);
        }
        tiltS_[0] = std::fabs(ts[0]) < 1e-20f ? 0.0f : ts[0];
        tiltS_[1] = std::fabs(ts[1]) < 1e-20f ? 0.0f : ts[1];
        hpS_[0] = std::fabs(hs[0]) < 1e-20f ? 0.0f : hs[0];
        hpS_[1] = std::fabs(hs[1]) < 1e-20f ? 0.0f : hs[1];
    }

    // The gains, ramped across the step, times Duck's across the call; four samples at a time, the
    // sum taken apart into L and R as it loads.
    const float inv = 1.0f / static_cast<float>(m);
    const float dg = (gain_ - gain0_) * inv, ds = (send_ - send0_) * inv;
    int j = 0;
    const f4 lane = f4{1.0f, 2.0f, 3.0f, 4.0f};
    for (; j + 4 <= m; j += 4) {
        const f4 at = splat(static_cast<float>(j)) + lane;
        const f4 g = (splat(gain0_) + splat(dg) * at) * (splat(d0) + splat(dStep) * at);
        const f4 s = g * (splat(send0_) + splat(ds) * at);
#if AF_NEON
        const float32x4x2_t y = vld2q_f32(acc_ + 2 * j);
        const f4 l = y.val[0], r = y.val[1];
#else
        const f4 l{acc_[2 * j], acc_[2 * j + 2], acc_[2 * j + 4], acc_[2 * j + 6]};
        const f4 r{acc_[2 * j + 1], acc_[2 * j + 3], acc_[2 * j + 5], acc_[2 * j + 7]};
#endif
        store4(outL + j, load4(outL + j) + l * g);
        store4(outR + j, load4(outR + j) + r * g);
        store4(sendL + j, load4(sendL + j) + l * s);
        store4(sendR + j, load4(sendR + j) + r * s);
    }
    for (; j < m; ++j) {
        const float at = static_cast<float>(j + 1);
        const float g = (gain0_ + dg * at) * (d0 + dStep * at), s = g * (send0_ + ds * at);
        outL[j] += acc_[2 * j] * g;
        outR[j] += acc_[2 * j + 1] * g;
        sendL[j] += acc_[2 * j] * s;
        sendR[j] += acc_[2 * j + 1] * s;
    }
}

// The reads, a group of four samples a turn, two samples a vector (L R of each). Every group is
// whole: a grain's envelope is 0 past its end (and acc_ has room past the step's), so a group
// running over adds nothing.
template <bool Cross>
AF_INLINE void Weather::readVoice(const Voice& v, int k0, int groups) {
#if AF_NEON
    const f2 gdh = vld1_f32(v.gd), gxh = vld1_f32(v.gx);
    const f4 gd = vcombine_f32(gdh, gdh), gx = vcombine_f32(gxh, gxh);
#else
    const f4 gd = f4{v.gd[0], v.gd[1], v.gd[0], v.gd[1]}, gx = f4{v.gx[0], v.gx[1], v.gx[0], v.gx[1]};
#endif
    const auto tap = [&v](int i) {
#if AF_TAP_ADDRESSES
        (void)v;
        return reinterpret_cast<const int16_t*>(static_cast<uintptr_t>(static_cast<uint32_t>(i)));
#else
        return v.data + i;
#endif
    };
    float* acc = acc_ + 2 * k0;
    const float* st = stage_;
    const int* ix = idx_;
    for (int g = 0; g < groups; ++g, acc += 8, st += 8, ix += 4) {
        // (One pair read and added before the next is read: two in flight would run out of registers.)
        const f4 y0 = cubic2(tap(ix[0]), tap(ix[1]), loadTwice(st));
        store4(acc, addGrain<Cross>(load4(acc), y0, gd, gx, loadTwice(st + 4)));
        const f4 y1 = cubic2(tap(ix[2]), tap(ix[3]), loadTwice(st + 2));
        store4(acc + 4, addGrain<Cross>(load4(acc + 4), y1, gd, gx, loadTwice(st + 6)));
    }
}

// A voice's part of a step: its positions and envelope four samples at a time, then the reads.
// The positions go by the step (EffectForce's segment): its first position (in double) as a whole
// frame `base` and a float offset that stays at least 1 across the step (so truncation is the
// floor), base moved into the ring once (the guard frames past its end take what runs over; a
// voice that has run past the ring's end is moved back by whole rings, so that is rare).
//
// The envelope of a grain not released is sin^2(pi t / L) itself, the Hann window the ramps make.
// sin(pi t / L) moves on four samples at a time by a recurrence, in Reinsch's form: with d the step
// just taken, d -= k s, s += d, k = 4 sin^2(2 pi / L) (= 2 - 2 cos(4 pi / L), but a small number
// keeps its precision in float where 2 cos(...) near 2 would lose it): two instructions a vector.
// A whole step leaves s and d where the next one starts, so they are kept; every kExactSteps steps
// (and whenever a step isn't whole) they are worked out exactly again (sinWide), so the recurrence
// never runs more than 128 turns from an exact start (a grain over a constant traces the Hann
// within 3e-6: checked). A grain released (a mode or source change: rare, 20 ms) takes the general
// ramp, the release's line below the window's, through window4.
void Weather::renderVoice(Voice& v, int m) {
    const int t0 = v.t;
    const int k0 = std::max(0, -t0);
    const int k1 = std::min(m, std::min(v.end, v.relEnd) - t0);
    if (k1 <= k0) return;
    const int groups = (k1 - k0 + 3) >> 2;
    const double p = v.start + static_cast<double>(t0) * v.rate;   // where the step starts
    // (Backwards, two frames below: the last read's offset is 1 in exact arithmetic, a hair under
    // it in float, which must not truncate to 0.)
    const int whole = v.rate < 0.0 ? floorInt(p + (kChunk - 1) * v.rate) - 2 : floorInt(p) - 1;
    int ib = whole;   // into the ring in integers
    if (ib >= v.frames || ib < 0) {
        ib = (ib % v.frames + v.frames) % v.frames;
        v.start -= static_cast<double>(whole - ib);   // whole rings: the same samples from here on
    }
    const f4 lane = f4{0.0f, 1.0f, 2.0f, 3.0f}, four = splat(4.0f);
    const f4 fv = splat(static_cast<float>(p - whole)), rv = splat(static_cast<float>(v.rate));
#if AF_TAP_ADDRESSES
    const int32_t at0 = static_cast<int32_t>(reinterpret_cast<uintptr_t>(v.data) + 4u * static_cast<uint32_t>(ib - 1));
    const i4 first = i4{at0, at0, at0, at0};
#else
    const i4 first = i4{2 * (ib - 1), 2 * (ib - 1), 2 * (ib - 1), 2 * (ib - 1)};
#endif
    f4 at = splat(static_cast<float>(k0)) + lane;   // the samples' places in the step
    // A group's positions: each read's first int16 (its address on the device) and its fraction.
    const auto positions = [&](int g) {
        const f4 o = fv + at * rv;
        const i4 io = __builtin_convertvector(o, i4);
        store4(stage_ + 8 * g, o - __builtin_convertvector(io, f4));
#if AF_TAP_ADDRESSES
        const i4 ix = first + (io << 2);   // 4 bytes a frame
        vst1q_s32(idx_ + 4 * g, ix);
#else
        const i4 ix = first + io + io;
        std::memcpy(idx_ + 4 * g, &ix, sizeof ix);
#endif
        at += four;
    };
    const f4 tau = splat(static_cast<float>(t0 + k0)) + lane;   // the voice's own times
    if (v.relEnd == kOpen) {
        const f4 k = splat(v.k);
        f4 s, d;
        const bool full = k0 == 0 && k1 == kChunk;   // a whole step, inside the grain
        if (v.exactIn > 0 && full) {
            s = load4(v.sin);
            d = load4(v.dsin);
        } else {
            // s at t, and d = s(t) - s(t - 4) = 2 sin(2 phi) cos(phi (t - 2)): a small number
            // worked out as one, not as the difference of two near ones.
            const f4 ph = splat(v.phi);
            s = sinWide(ph * tau);
            d = splat(v.dk) * sinWide(splat(1.57079633f) - ph * (tau - splat(2.0f)));
        }
        for (int g = 0; g < groups; ++g) {
            positions(g);
            store4(stage_ + 8 * g + 4, s * s);
            d -= k * s;
            s += d;
        }
        if (full) {   // eight vectors on: where the next step starts
            store4(v.sin, s);
            store4(v.dsin, d);
            v.exactIn = v.exactIn > 0 ? v.exactIn - 1 : kExactSteps - 1;
        } else {
            v.exactIn = 0;
        }
    } else {
        const f4 sl = splat(v.slope), relS = splat(v.relSlope);
        const f4 endF = splat(static_cast<float>(v.end)), relF = splat(static_cast<float>(v.relEnd));
        f4 t = tau;
        for (int g = 0; g < groups; ++g, t += four) {
            positions(g);
            const f4 a = max4(min4(min4(t * sl, (endF - t) * sl), (relF - t) * relS), splat(0.0f));
            store4(stage_ + 8 * g + 4, window4(a));
        }
    }
    for (int j = k1 - k0; j < 4 * groups; ++j) stage_[8 * (j >> 2) + 4 + (j & 3)] = 0.0f;   // past its end
    if (v.gx[0] != 0.0f || v.gx[1] != 0.0f) readVoice<true>(v, k0, groups);
    else readVoice<false>(v, k0, groups);
}

} // namespace af
