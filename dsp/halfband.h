// From EffectForce dsp/halfband.h (c9d09f1), namespace ef -> af, EF_ -> AF_. Here the stereo
// decimator makes Weather's slower levels (dsp/grainsrc.cpp, and Memory as it records); the tests
// named below are the siblings', and test/weather_test.cpp and test/fields_test.cpp measure the
// levels it makes. Added here: FrameDecimator, the stereo decimator over a run of frames as they
// lie, its state in registers (Memory's recording, within its 4k instructions a block).
#pragma once
// 2x up and down: the decimator (SubForce's dsp/halfband.h; there the whole voice runs at
// 88.2 kHz) folds a 2x oversampled signal back to MPC's 44.1 kHz; the interpolator, its mirror
// image, lifts 44.1 kHz to 88.2 kHz (the drive shapes there). Both are the same polyphase IIR
// halfband (two chains of first-order allpasses at the low rate, Laurent de Soras's HIIR
// structure): 8 multiplies per low-rate sample each.
//
// Design: 8 coefficients, transition band 0.0232 of the high rate (passband to 20.0 kHz,
// stopband from 24.1 kHz): passband ripple < 1e-7 dB, stopband >= 85 dB. Coefficients from
// HIIR's PolyphaseIir2Designer formulas (SubForce's tools/halfband_design.py prints them); the
// decimator's response is measured in SubForce's test/engine_test.cpp, the interpolator's (and
// the pair's) in test/drive_test.cpp. Nonlinear phase (an IIR), like any analog filter: up and
// down together delay low frequencies by 2.7 samples, 20 kHz by 5.
//
// The stereo versions at the end run both channels' four chains in the lanes of one f4
// (dsp/simd.h): the same arithmetic, a quarter of the instructions.
#include "simd.h"

namespace af {

// The allpass coefficients: even indices form one chain, odd indices the other.
inline constexpr int kHalfbandCoefs = 8;
inline constexpr float kHalfband[kHalfbandCoefs] = {0.0536154666f, 0.1934367242f, 0.3723159713f, 0.5466893949f,
                                                    0.6930302670f, 0.8067034095f, 0.8940130551f, 0.9658743028f};

class Decimator {
public:
    static constexpr int kCoefs = kHalfbandCoefs;

    // Two high-rate samples, `early` first, become one low-rate sample.
    float process(float early, float late) {
        float a = late, b = early;   // a: the even chain (coefs 0, 2, ...), b: the odd chain (1, 3, ...)
        for (int i = 0; i < kCoefs; i += 2) {
            const float ta = (a - y_[i]) * kHalfband[i] + x_[i];
            const float tb = (b - y_[i + 1]) * kHalfband[i + 1] + x_[i + 1];
            x_[i] = a;
            x_[i + 1] = b;
            y_[i] = a = ta;
            y_[i + 1] = b = tb;
        }
        return 0.5f * (a + b);
    }

    void reset() {
        for (int i = 0; i < kCoefs; ++i) x_[i] = y_[i] = 0.0f;
    }

private:
    float x_[kCoefs] = {}, y_[kCoefs] = {};
};

// HIIR's Upsampler2x: one low-rate sample feeds both chains; the even chain's output is the early
// high-rate sample, the odd chain's the late one. Zero-stuffing would halve the level and the
// halfband's sum of the chains would double it again, so neither appears: passband gain is 1.
// The image of an input at f sits at 44.1 kHz - f: for 0..20 kHz in the stopband, >= 85 dB down.
class Interpolator {
public:
    static constexpr int kCoefs = kHalfbandCoefs;

    // One low-rate sample becomes two high-rate samples, `early` first.
    void process(float in, float& early, float& late) {
        float a = in, b = in;   // a: the even chain (coefs 0, 2, ...), b: the odd chain (1, 3, ...)
        for (int i = 0; i < kCoefs; i += 2) {
            const float ta = (a - y_[i]) * kHalfband[i] + x_[i];
            const float tb = (b - y_[i + 1]) * kHalfband[i + 1] + x_[i + 1];
            x_[i] = a;
            x_[i + 1] = b;
            y_[i] = a = ta;
            y_[i + 1] = b = tb;
        }
        early = a;
        late = b;
    }

    void reset() {
        for (int i = 0; i < kCoefs; ++i) x_[i] = y_[i] = 0.0f;
    }

private:
    float x_[kCoefs] = {}, y_[kCoefs] = {};
};

// The allpass chains of two channels side by side, lanes (left even, left odd, right even,
// right odd): each lane steps exactly as the scalar classes' chains do.
class StereoHalfband {
public:
    void reset() {
        for (int s = 0; s < kStages; ++s) x_[s] = y_[s] = splat(0.0f);
    }

protected:
    static constexpr int kStages = kHalfbandCoefs / 2;

    f4 run(f4 a) {
        for (int s = 0; s < kStages; ++s) {
            const f4 c = f4{kHalfband[2 * s], kHalfband[2 * s + 1], kHalfband[2 * s], kHalfband[2 * s + 1]};
            const f4 t = (a - y_[s]) * c + x_[s];
            x_[s] = a;
            y_[s] = a = t;
        }
        return a;
    }

private:
    f4 x_[kStages] = {}, y_[kStages] = {};
};

class StereoInterpolator : public StereoHalfband {
public:
    // One low-rate sample per channel becomes (left early, left late, right early, right late).
    f4 process(float left, float right) {
#if AF_NEON
        return run(vcombine_f32(vdup_n_f32(left), vdup_n_f32(right)));
#else
        return run(f4{left, left, right, right});
#endif
    }
};

class StereoDecimator : public StereoHalfband {
public:
#if AF_NEON
    // Two frames (L R)(L R), early first, become one (L R): process()'s arithmetic, the lanes
    // arranged in registers instead of assembled from scalars.
    float32x2_t processPair(float32x4_t frames) {
        const float32x2x2_t t = vtrn_f32(vget_low_f32(frames), vget_high_f32(frames));   // (Le Ll), (Re Rl)
        const f4 a = run(vrev64q_f32(vcombine_f32(t.val[0], t.val[1])));
        return vmul_f32(vpadd_f32(vget_low_f32(a), vget_high_f32(a)), vdup_n_f32(0.5f));
    }
#endif
    // (left early, left late, right early, right late) become one low-rate sample per channel.
    void process(f4 high, float& left, float& right) {
#if AF_NEON
        const f4 a = run(vrev64q_f32(high));   // the even chains take the late samples
        const float32x2_t sum = vpadd_f32(vget_low_f32(a), vget_high_f32(a));
        left = 0.5f * vget_lane_f32(sum, 0);
        right = 0.5f * vget_lane_f32(sum, 1);
#else
        const f4 a = run(f4{high[1], high[0], high[3], high[2]});
        left = 0.5f * (a[0] + a[1]);
        right = 0.5f * (a[2] + a[3]);
#endif
    }
};

// StereoDecimator's filter over a run of frames as they lie in memory (L R L R ..., Memory's
// recording): the same chains with the same coefficients, lane for lane, but the lanes in the
// frames' own order, (L early, R early, L late, R late): the odd chains (which take the early
// samples), then the even ones, so a pair loads as it lies and needs no rearranging. Through a run
// the state stays in registers: each stage's input is the stage before's output (StereoDecimator
// keeps both; here x_[s + 1] is y_[s], so only the first stage's input is kept), and a stage's new
// output takes the register of the value it no longer needs, which moves every register's role on
// by one a pair; six pairs bring them back, so the loop takes six at a time and moves nothing.
// About 15 ARM instructions a pair, where a loop of StereoDecimator::processPair() takes 28 (the
// compiler's output for the device). test/fields_test.cpp checks it gives StereoDecimator's output.
class FrameDecimator {
public:
    void reset() {
        x0_ = splat(0.0f);
        for (f4& y : y_) y = splat(0.0f);
    }

    // `pairs` pairs of frames (L R L R, early first) from `in`, each into one frame (L R) at `out`,
    // which may be `in` (written behind what is read).
    void process(const float* in, int pairs, float* out) {
        const f4 c0 = coefs(0), c1 = coefs(1), c2 = coefs(2), c3 = coefs(3);
        // r1..r5 hold the first stage's input and the four stages' outputs; r0 is free.
        f4 r0 = splat(0.0f), r1 = x0_, r2 = y_[0], r3 = y_[1], r4 = y_[2], r5 = y_[3];
        // One pair: its frames into A; the stages' outputs over the first's input (X), then over
        // the outputs before (Y0 .. Y2). After it A is the input, X .. Y2 the outputs, Y3 free.
        const auto pair = [&](f4& A, f4& X, f4& Y0, f4& Y1, f4& Y2, const f4& Y3) {
            A = load4(in);
            in += 4;
            X = (A - Y0) * c0 + X;
            Y0 = (X - Y1) * c1 + Y0;
            Y1 = (Y0 - Y2) * c2 + Y1;
            Y2 = (Y1 - Y3) * c3 + Y2;
#if AF_NEON
            vst1_f32(out, vmul_f32(vadd_f32(vget_low_f32(Y2), vget_high_f32(Y2)), vdup_n_f32(0.5f)));
#else
            out[0] = 0.5f * (Y2[0] + Y2[2]);
            out[1] = 0.5f * (Y2[1] + Y2[3]);
#endif
            out += 2;
        };
        int p = 0;
        for (; p + 6 <= pairs; p += 6) {
            pair(r0, r1, r2, r3, r4, r5);
            pair(r5, r0, r1, r2, r3, r4);
            pair(r4, r5, r0, r1, r2, r3);
            pair(r3, r4, r5, r0, r1, r2);
            pair(r2, r3, r4, r5, r0, r1);
            pair(r1, r2, r3, r4, r5, r0);
        }
        for (; p < pairs; ++p) {
            pair(r0, r1, r2, r3, r4, r5);
            r5 = r4;
            r4 = r3;
            r3 = r2;
            r2 = r1;
            r1 = r0;
        }
        x0_ = r1;
        y_[0] = r2;
        y_[1] = r3;
        y_[2] = r4;
        y_[3] = r5;
    }

private:
    static constexpr int kStages = kHalfbandCoefs / 2;
    static_assert(kStages == 4, "four stages, in registers");
    // A stage's coefficients in the lanes' order: the odd chain's twice, then the even chain's.
    static f4 coefs(int s) { return f4{kHalfband[2 * s + 1], kHalfband[2 * s + 1], kHalfband[2 * s], kHalfband[2 * s]}; }

    f4 x0_ = {}, y_[kStages] = {};
};

} // namespace af
