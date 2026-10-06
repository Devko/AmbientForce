#include "lifetime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace af {
namespace {

constexpr double kPi = 3.14159265358979323846;

// --- life models ------------------------------------------------------------------------------
//
// Frame f shows the note at time t = T * (f / 255)^kLifeCurve. Each harmonic h has the amplitude
//
//   A_h(t) = S_h * life_h(t) * (1 + beat_h(t)) * wobble_h(t) * formant(h, t)
//
// S_h       the start: a tilt in dB per octave, the even harmonics' share, a strong 2nd if asked,
//           a soft high cut.
// life_h    a decay exp(-t / tau_h), tau_h = tau1 / (1 + damp (h - 1)), so the upper harmonics die
//           first; or, for a bloom, a growth (t / T)^(bloom (h - 1)), so they arrive last.
// beat_h    a slow sinusoidal dip, -beat * (1 - cos(2 pi rate_h t + phase_h)) / 2, so a partial
//           pulses the way two strings tuned a hair apart do. Each harmonic its own rate.
// wobble_h  a slow swing of the whole tilt, 10^(wobble sin(2 pi wobbleHz t) log2(h) / 20): the
//           brightness rising and falling together, as a bow's pressure does it.
// formant   floor + F1 + F2, two resonance peaks that may move over the life (Hz at the model's
//           reference pitch).
//
// A table is pitch-free, so its formants sit on fixed harmonics: true at `ref`, shifted with the
// note elsewhere. Each harmonic keeps one phase (pseudo-random, from the model's seed) in every
// frame: the waveform has no big peak where all partials line up, and two neighbouring frames
// crossfade without comb filtering. Then the frame is scaled to the same RMS as all the others.

struct Formant {
    double hz = 0.0;     // centre at the reference pitch
    double gain = 0.0;   // at the centre
    double bw = 100.0;   // Hz, between the half-power points
};

struct LifeModel {
    // The phases' and beat rates' random numbers: fixed per model (its table's place in the
    // browser when it was made), so reordering the browser doesn't change the sound.
    uint32_t seed = 0;
    double length = 10.0;      // T: seconds of the note that the 256 frames span
    // S_h, the spectrum at the start
    double tilt = -6.0;        // dB per octave
    double even = 1.0;         // the even harmonics' gain against the odd ones
    double second = 1.0;       // extra gain on harmonic 2
    double rolloff = 0.0;      // > 0: a soft high cut, exp(-(h - 1) / rolloff)
    uint32_t only = 0;         // non-zero: a mask of the only harmonics (1 << h) that sound
    // life_h
    double tau1 = 0.0;         // s: harmonic 1's decay time (0: no decay)
    double damp = 0.0;
    double bloom = 0.0;        // > 0: growth instead of decay
    // beat_h
    double beat = 0.0;         // depth: a beating harmonic dips to 1 - beat
    double beatLo = 0.0, beatHi = 0.0;   // Hz: the rates, spread over this range
    int beatFrom = 1;          // the first harmonic that beats
    bool beatTogether = false; // every beat starts at its top (strings struck together), else anywhere
    double wobble = 0.0;       // dB per octave: a slow swing of the whole tilt (bow pressure)
    double wobbleHz = 0.0;
    // formant(h, t)
    double ref = 220.0;        // Hz: the pitch the formants are placed for
    // Added to the two peaks everywhere. Their skirts fall slowly (gain / sqrt(1 + z^2), z in half
    // bandwidths: still 1/5 of the peak 5 half bandwidths away), so far from both the gain is the
    // floor plus a little of each. Formants are off when the floor and every gain are 0.
    double floor = 0.0;
    Formant start[2], end[2];  // F1 and F2 at the start of the life and at its end
    double path = 1.0;         // the formants move as (t / T)^path
};

// Felt Piano: a piano with felt between hammer and strings. Dark from the start (-9 dB per
// octave and the felt's soft cut), the 2nd harmonic nearly as strong as the 1st, as in a real
// string's middle register. The upper harmonics die first (damp 0.08), so the 20 s life goes
// from a warm, round attack to a near-sine tail with a trace of the octave. From the 3rd harmonic
// up the partials beat at 0.3-0.9 Hz, starting together as the struck strings of a unison do.
// The beating dips 2 dB: deeper, and early on it would brighten the sound faster than the
// damping darkens it (tau1 1.5 s sets that pace), and the tone would no longer age one way.
LifeModel feltPiano() {
    LifeModel m;
    m.seed = 0;
    m.length = 20.0;
    m.tilt = -9.0;
    m.second = 2.0;          // -3 dB under the fundamental instead of the tilt's -9
    m.rolloff = 24.0;
    m.tau1 = 1.5;
    m.damp = 0.08;
    m.beat = 0.2;
    m.beatLo = 0.3;
    m.beatHi = 0.9;
    m.beatFrom = 3;
    m.beatTogether = true;
    return m;
}

// Celesta: struck steel bars over wooden resonators. Odd-heavy and bright at the strike (-6 dB
// per octave, the even harmonics 10 dB down: a hollow, glassy ping); damp 0.25 takes the upper
// harmonics away within the first second, leaving the resonator's near-sine for the tail.
LifeModel celesta() {
    LifeModel m;
    m.seed = 1;
    m.length = 8.0;
    m.tilt = -6.0;
    m.even = 0.3;
    m.tau1 = 1.2;
    m.damp = 0.25;
    return m;
}

// Glass Harmonica: rubbed glass bowls. Harmonics 1, 2, 3 and 5 only, steep (-11 dB per octave: an
// almost pure tone with a faint overtone halo), barely decaying; the overtones beat slowly
// (0.1-0.3 Hz, 4 dB deep) against the steady fundamental, as the glass's partials drift.
LifeModel glassHarmonica() {
    LifeModel m;
    m.seed = 2;
    m.length = 30.0;
    m.tilt = -11.0;
    m.only = 1u << 1 | 1u << 2 | 1u << 3 | 1u << 5;
    m.tau1 = 30.0;
    m.damp = 0.02;
    m.beat = 0.35;
    m.beatLo = 0.1;
    m.beatHi = 0.3;
    m.beatFrom = 2;
    return m;
}

// Cello Tasto: bowed over the fingerboard, the soft, flute-like way. A bowed string's saw with
// every harmonic, but steep (-12 dB per octave); the body's wood and air resonances at 300 Hz
// and 1 kHz, each lifting its harmonics by about 4 dB (placed for C3, the middle of a cello's
// range); the bow pressure swinging the brightness by +-1 dB per octave at 0.2 Hz; the stroke
// starting with a little more bite and settling darker as it goes on (the 10th harmonic 8 dB
// down by the end).
LifeModel celloTasto() {
    LifeModel m;
    m.seed = 3;
    m.length = 12.0;
    m.tilt = -12.0;
    m.tau1 = 6.0;
    m.damp = 0.05;
    m.wobble = 1.0;
    m.wobbleHz = 0.2;
    m.ref = 130.8;
    m.floor = 0.3;
    m.start[0] = m.end[0] = {300.0, 1.0, 150.0};
    m.start[1] = m.end[1] = {1000.0, 1.0, 400.0};
    return m;
}

// Choir Ah-Oo: voices singing "ah" that close to "oo" over the life. A glottal source (-6 dB per
// octave, softened far up as a voice is) through two formants moving from ah (F1 800 Hz, F2
// 1150 Hz) to oo (350 Hz, 600 Hz, F2 weaker), placed for A3; the mouth closes quickly at first,
// then slower. Every partial beats a little (0.2-0.6 Hz), as a section of voices never sings one
// pitch.
LifeModel choirAhOo() {
    LifeModel m;
    m.seed = 4;
    m.length = 16.0;
    m.tilt = -6.0;
    m.rolloff = 24.0;
    m.beat = 0.25;
    m.beatLo = 0.2;
    m.beatHi = 0.6;
    m.ref = 220.0;
    m.floor = 0.05;
    m.start[0] = {800.0, 1.0, 160.0};
    m.start[1] = {1150.0, 0.6, 160.0};
    m.end[0] = {350.0, 1.0, 120.0};
    m.end[1] = {600.0, 0.35, 130.0};
    m.path = 0.6;
    return m;
}

// Reed Organ: free reeds blown by bellows. Reedy and steady: mostly odd harmonics, the evens ~16 dB
// down, -6 dB per octave with a soft cut far up; no decay while the bellows blow, only a little
// beating (0.5-1.2 Hz) between the reeds of a rank.
LifeModel reedOrgan() {
    LifeModel m;
    m.seed = 5;
    m.length = 10.0;
    m.tilt = -6.0;
    m.even = 0.15;
    m.rolloff = 48.0;
    m.beat = 0.12;
    m.beatLo = 0.5;
    m.beatHi = 1.2;
    return m;
}

// Sine Bloom: a decay run backwards. Frame 0 is a pure sine; each harmonic grows in as
// (t / T)^(0.6 (h - 1)), the upper ones later, until the last frame is a soft saw (-7.5 dB per
// octave with a soft cut). Spread over the table: at frame 128 the 2nd harmonic is 14 dB under
// the fundamental, at frame 192 10 dB (the 5th 28 dB).
LifeModel sineBloom() {
    LifeModel m;
    m.seed = 6;
    m.length = 20.0;
    m.tilt = -7.5;
    m.rolloff = 32.0;
    m.bloom = 0.6;
    return m;
}

// Tape Strings: a string section as a tape replay keyboard plays it. A saw (-6 dB per octave),
// the tape's high loss as a soft cut, the top mellowing over the life (the 12th harmonic 7 dB
// down by the end); every partial beats gently (0.3-1.0 Hz), the ensemble's many slightly
// different bows.
LifeModel tapeStrings() {
    LifeModel m;
    m.seed = 7;
    m.length = 10.0;
    m.tilt = -6.0;
    m.rolloff = 36.0;
    m.tau1 = 4.0;
    m.damp = 0.03;
    m.beat = 0.3;
    m.beatLo = 0.3;
    m.beatHi = 1.0;
    return m;
}

LifeModel (*const kModels[])() = {feltPiano, celesta, glassHarmonica, celloTasto,
                                   choirAhOo, reedOrgan, sineBloom, tapeStrings};
static_assert(sizeof kModels / sizeof kModels[0] == TB_SINE, "one model per lifetime table");

// A pseudo-random number in [0, 1) per (model's seed, harmonic, use): the same table on every
// build and every machine (splitmix64; no library generator, whose sequence may differ between
// versions).
double rnd(uint32_t seed, int h, int use) {
    uint64_t x = static_cast<uint64_t>(seed) << 40 ^ static_cast<uint64_t>(use) << 32 ^ static_cast<uint64_t>(h);
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return static_cast<double>(x >> 11) * (1.0 / 9007199254740992.0);
}

// What a harmonic keeps for the whole life.
struct Partial {
    double start = 0.0;    // S_h
    double decay = 0.0;    // 1 / tau_h
    double beatW = 0.0;    // rad/s; 0: no beating
    double beatPh = 0.0;
    double sinPh = 0.0, cosPh = 1.0;   // its phase
    double octave = 0.0;   // log2 h
    double hz = 0.0;       // at the reference pitch
};

std::vector<Partial> partials(const LifeModel& m) {
    std::vector<Partial> ps(kMaxHarmonic);
    for (int h = 1; h < kMaxHarmonic; ++h) {
        Partial& p = ps[static_cast<size_t>(h)];
        const bool sounds = !m.only || (h < 32 && (m.only >> h & 1u));
        if (!sounds) continue;
        p.octave = std::log2(static_cast<double>(h));
        p.hz = h * m.ref;
        p.start = std::pow(10.0, m.tilt * p.octave / 20.0) * (h % 2 ? 1.0 : m.even) * (h == 2 ? m.second : 1.0);
        if (m.rolloff > 0.0) p.start *= std::exp(-(h - 1) / m.rolloff);
        if (m.tau1 > 0.0) p.decay = (1.0 + m.damp * (h - 1)) / m.tau1;
        if (m.beat > 0.0 && h >= m.beatFrom) {
            p.beatW = 2.0 * kPi * (m.beatLo + (m.beatHi - m.beatLo) * rnd(m.seed, h, 1));
            p.beatPh = m.beatTogether ? 0.0 : 2.0 * kPi * rnd(m.seed, h, 2);
        }
        const double ph = 2.0 * kPi * rnd(m.seed, h, 0);
        p.sinPh = std::sin(ph);
        p.cosPh = std::cos(ph);
    }
    return ps;
}

// A resonance's magnitude at f: 1 at its centre, 1/sqrt(2) half a bandwidth away (PolyForce's peak()).
double peak(double f, const Formant& r) {
    const double z = (f - r.hz) / (0.5 * r.bw);
    return r.gain / std::sqrt(1.0 + z * z);
}

// Frame at time t: amplitudes, then scaled so the harmonics' energy is a unit sine's (RMS 1/sqrt(2)).
void spectrumAt(const LifeModel& m, const std::vector<Partial>& ps, double t, std::vector<double>& amp, Spectrum& s) {
    const double u = std::min(t / m.length, 1.0);
    const bool formants = m.floor > 0.0 || m.start[0].gain > 0.0 || m.start[1].gain > 0.0 || m.end[0].gain > 0.0 ||
                          m.end[1].gain > 0.0;
    Formant f[2];
    if (formants) {
        const double x = std::pow(u, m.path);
        for (int k = 0; k < 2; ++k) {
            const Formant &a = m.start[k], &b = m.end[k];
            f[k].hz = a.hz > 0.0 && b.hz > 0.0 ? a.hz * std::pow(b.hz / a.hz, x) : a.hz;   // glides in pitch
            f[k].gain = a.gain + (b.gain - a.gain) * x;
            f[k].bw = a.bw + (b.bw - a.bw) * x;
        }
    }
    const double tilt = m.wobble * std::sin(2.0 * kPi * m.wobbleHz * t);   // dB per octave, now

    double energy = 0.0;
    for (int h = 1; h < kMaxHarmonic; ++h) {
        const Partial& p = ps[static_cast<size_t>(h)];
        double a = p.start;
        if (a > 0.0) {
            if (m.bloom > 0.0) a *= h == 1 ? 1.0 : std::pow(u, m.bloom * (h - 1));
            const double d = t * p.decay;
            a *= d < 600.0 ? std::exp(-d) : 0.0;   // exp(-600) = 1e-261, nothing at any level: skip the exp
            if (p.beatW > 0.0) a *= 1.0 - m.beat * 0.5 * (1.0 - std::cos(p.beatW * t + p.beatPh));
            if (tilt != 0.0) a *= std::pow(10.0, tilt * p.octave / 20.0);
            if (formants) a *= m.floor + peak(p.hz, f[0]) + peak(p.hz, f[1]);
        }
        amp[static_cast<size_t>(h)] = a;
        energy += a * a;
    }
    const double g = energy > 0.0 ? 1.0 / std::sqrt(energy) : 0.0;
    for (int h = 1; h < kMaxHarmonic; ++h) {
        double a = g * amp[static_cast<size_t>(h)];
        if (a < 1e-10) a = 0.0;   // -200 dB: nothing, and no denormals in the FFT
        const Partial& p = ps[static_cast<size_t>(h)];
        s.a[static_cast<size_t>(h)] = a * p.sinPh;   // a sin(2 pi h x + ph) = a sin(ph) cos + a cos(ph) sin
        s.b[static_cast<size_t>(h)] = a * p.cosPh;
    }
}

double timeOf(int frame, double length) {
    return length * std::pow(static_cast<double>(frame) / (kLifeFrames - 1), kLifeCurve);
}

bool cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(std::memory_order_relaxed); }

bool buildLife(int id, Wavetable& t, const std::atomic<bool>* cancel) {
    const LifeModel m = kModels[id]();
    const std::vector<Partial> ps = partials(m);
    std::vector<double> amp(kMaxHarmonic, 0.0);
    Spectrum a, b;
    const FrameBuilder fb;
    t.data.reserve(static_cast<size_t>(kLifeFrames) * kFrameStride);
    t.scale.reserve(kLifeFrames);
    for (int f = 0; f < kLifeFrames; f += 2) {   // two frames per inverse FFT
        if (cancelled(cancel)) return false;
        spectrumAt(m, ps, timeOf(f, m.length), amp, a);
        spectrumAt(m, ps, timeOf(f + 1, m.length), amp, b);
        fb.add(t, a, &b);
    }
    return true;
}

// --- the digital waves: the exact Fourier series (PolyForce's), band-limited per level like any table

void buildDigital(int id, Wavetable& t) {
    Spectrum s;
    for (int h = 1; h < kMaxHarmonic; ++h) {
        const size_t i = static_cast<size_t>(h);
        const bool odd = h % 2 == 1;
        switch (id) {
            case TB_SINE: s.b[i] = h == 1 ? 1.0 : 0.0; break;
            case TB_TRIANGLE: s.b[i] = odd ? 8.0 / (kPi * kPi * h * h) * (((h - 1) / 2) % 2 ? -1.0 : 1.0) : 0.0; break;
            case TB_SAW: s.b[i] = 2.0 / (kPi * h) * (odd ? 1.0 : -1.0); break;
            default: s.b[i] = odd ? 4.0 / (kPi * h) : 0.0; break;   // TB_SQUARE
        }
    }
    const FrameBuilder fb;
    fb.add(t, s);
}

const char* const kNames[TB_COUNT] = {"Felt Piano", "Celesta", "Glass Harmonica", "Cello Tasto",
                                      "Choir Ah-Oo", "Reed Organ", "Sine Bloom", "Tape Strings",
                                      "Sine", "Triangle", "Saw", "Square"};

} // namespace

const char* tableName(int id) { return id >= 0 && id < TB_COUNT ? kNames[id] : ""; }

bool isLifetime(int id) { return id >= 0 && id < TB_SINE; }

bool buildTable(int id, Wavetable& out, const std::atomic<bool>* cancel) {
    if (id < 0 || id >= TB_COUNT || cancelled(cancel)) return false;
    Wavetable t;
    t.name = kNames[id];
    if (isLifetime(id)) {
        if (!buildLife(id, t, cancel)) return false;
    } else {
        buildDigital(id, t);   // one frame: not worth a look at `cancel`
    }
    t.id = newTableId();
    out = std::move(t);
    return true;
}

const Wavetable& sineTable() {
    // Lives for the process, by design: never destroyed, as an audio thread may still be reading it
    // while the process exits. An unload of the module leaves its 18 KB behind.
    static const Wavetable* const sine = [] {
        std::unique_ptr<Wavetable> t(new Wavetable);
        buildTable(TB_SINE, *t);
        return t.release();
    }();
    return *sine;
}

const Wavetable& TableSet::get(int id) const {
    const Wavetable* p = id >= 0 && id < TB_COUNT ? t[id].load(std::memory_order_acquire) : nullptr;
    return p ? *p : sineTable();
}

} // namespace af
