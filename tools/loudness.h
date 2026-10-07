#pragma once
// Loudness for the tools: ITU-R BS.1770-4's integrated loudness (EBU R128's), at 44.1 kHz. The
// K-weighting (the head's high shelf, then the RLB high-pass: libebur128's formulas), 400 ms blocks
// every 100 ms, the -70 LUFS absolute and -10 LU relative gates; stereo, both channels at weight 1
// (a mono signal is fed as the same sample on both, as the plugin plays it).
//
//   loudness::Meter m;
//   for (...) m.add(l, r);
//   double lufs = m.integrated();   // everything added since the last clear()
//
// clear() forgets the blocks but keeps the filters' state, so a stream can be measured in windows
// (tools/soak.cpp's ten minutes) without a filter starting over at every boundary.
#include <algorithm>
#include <cmath>
#include <vector>

namespace loudness {

constexpr double kRate = 44100.0;
constexpr int kStep = 4410;   // 100 ms: the blocks' hop (a block is four steps)

struct Biquad {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct KWeight {
    Biquad shelf{}, hp{};
    KWeight() {
        double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        double K = std::tan(M_PI * f0 / kRate), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        double a0 = 1.0 + K / Q + K * K;
        shelf = {(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
        f0 = 38.13547087602444;
        Q = 0.5003270373238773;
        K = std::tan(M_PI * f0 / kRate);
        a0 = 1.0 + K / Q + K * K;
        hp = {1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    }
    double run(double x) { return hp.run(shelf.run(x)); }
};

// A block's loudness from its mean square (the channels' summed).
inline double lufsOf(double meanSquare) { return -0.691 + 10.0 * std::log10(std::max(meanSquare, 1e-20)); }

class Meter {
public:
    void add(float l, float r) {
        const double yl = kl_.run(l), yr = kr_.run(r);
        acc_ += yl * yl + yr * yr;
        if (++n_ == kStep) {
            steps_.push_back(acc_);
            acc_ = 0.0;
            n_ = 0;
        }
    }

    // LUFS, gated; -100 when no block passes the absolute gate. A part step at the end counts as if
    // it were whole.
    double integrated() const {
        std::vector<double> s = steps_;
        if (n_ > 0) s.push_back(acc_ * kStep / n_);
        std::vector<double> z;   // the 400 ms blocks' mean squares
        for (size_t j = 0; j + 4 <= s.size(); ++j) z.push_back((s[j] + s[j + 1] + s[j + 2] + s[j + 3]) / (4.0 * kStep));
        double sum = 0.0;
        int n = 0;
        for (double v : z)
            if (lufsOf(v) > -70.0) sum += v, ++n;
        if (!n) return -100.0;
        const double rel = lufsOf(sum / n) - 10.0;
        sum = 0.0;
        n = 0;
        for (double v : z)
            if (lufsOf(v) > -70.0 && lufsOf(v) > rel) sum += v, ++n;
        return n ? lufsOf(sum / n) : -100.0;
    }

    void clear() {
        steps_.clear();
        acc_ = 0.0;
        n_ = 0;
    }

private:
    KWeight kl_, kr_;
    std::vector<double> steps_;   // K-weighted L^2 + R^2 summed over each 100 ms
    double acc_ = 0.0;
    int n_ = 0;
};

} // namespace loudness
