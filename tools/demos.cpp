// From SubForce tools/demos.cpp (8846421), renamed; the ambient phrase (tools/phrase.h) in stereo, and
// a look at each preset's sound.
// Demo clips, preset levels and what each factory preset measures, rendered through the plugin's own
// entry points (VSTPluginMain, 128-frame blocks, MIDI with sample offsets), the way MPC plays it.
//
//   demos <outdir>                 every factory preset playing the phrase, as
//                                  <outdir>/NN_<Category>_<Name>.wav (stereo), and all of them back
//                                  to back as <outdir>/tour.wav; prints what each one measures (below)
//   demos --match <dir> <LUFS>     sets volume= in every preset file under <dir> (presets/Factory)
//                                  so its phrase plays at <LUFS> integrated. The engine's limiter
//                                  holds every peak under -1 dBFS; how hard it has to work for that
//                                  is printed (a preset that leans on it is one to fix, not to
//                                  turn down)
//
// The phrase and the loudness: tools/phrase.h. What a preset measures, a map for listening without
// a device (times in the 40 s phrase; the first chord 0..12 s, the second 12..24 s, the release after):
//   LUFS, peak      integrated loudness and the sample peak, dBFS
//   pre-lim, GR     the peak before the limiter, and the most it took off (dB) with the share of
//                   10 ms windows it touched. The volume comes before the limiter and nothing else
//                   depends on it, so a second render 20 dB down, back up by 20 dB, is the output
//                   without the limiter
//   centroid        the spectral centroid (Hz) of the two holds (2..12 s, 14..24 s) and of the
//                   release (24..32 s, 32..40 s). A pad's low fundamentals weigh most: the factory
//                   set runs from about 150 Hz (a drone in fog) to 750 Hz (the brightest Bloom)
//   motion          within the holds (4..12 s and 16..24 s, past the swell): how far the 1 s
//                   loudness moves (LU), and how far the timbre does: the standard deviation of
//                   the energy over 1 kHz against the whole, in 0.5 s frames (dB; the centroid
//                   hardly moves when only the upper harmonics do). The larger hold of each;
//                   both near 0, a static sound
//   wet             the reverb's return against the dry over 4..12 s (dB): a third render with the
//                   Space return at 0 is the dry
//   corr, S/M       L/R correlation and side against mid (dB) over 2..40 s: 1 and -inf are mono
//   tail            after the last key goes up (24 s): seconds until the 400 ms loudness is 20 and
//                   40 dB under where it was ("-": not within the 16 s, a drone holding), and where
//                   it ends (dB)
#include "phrase.h"
#include "factory_presets.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback);

namespace {

using Buf = std::vector<float>;
constexpr double kSr = afl::kSr;
VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

// A fresh instance (a preset means everything it doesn't name at its default) playing the phrase.
void render(const std::string& state, Buf& L, Buf& R) {
    AEffect* e = VSTPluginMain(master);
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(state.size()), const_cast<char*>(state.data()), 0.0f);
    g_time = VstTimeInfo{};
    afl::render(e, g_time, afl::phrase(state), L, R);
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
}

double dbOf(double v) { return 20.0 * std::log10(std::max(v, 1e-9)); }
size_t at(double s) { return static_cast<size_t>(s * kSr); }

// --- spectra -------------------------------------------------------------------------------------
using cd = std::complex<double>;

void fft(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const cd w = std::polar(1.0, -2.0 * afl::kPi / static_cast<double>(len));
        for (size_t i = 0; i < n; i += len) {
            cd t = 1.0;
            for (size_t k = 0; k < len / 2; ++k, t *= w) {
                const cd u = a[i + k], v = a[i + k + len / 2] * t;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

constexpr size_t kFft = 4096;

// The mid's power spectrum over [from, to), Hann frames of kFft every half frame, summed.
std::vector<double> powerSpectrum(const Buf& L, const Buf& R, size_t from, size_t to) {
    std::vector<double> p(kFft / 2, 0.0);
    std::vector<cd> a(kFft);
    for (size_t s = from; s + kFft <= to; s += kFft / 2) {
        for (size_t i = 0; i < kFft; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * afl::kPi * static_cast<double>(i) / kFft);
            a[i] = 0.5 * (L[s + i] + R[s + i]) * w;
        }
        fft(a);
        for (size_t k = 0; k < kFft / 2; ++k) p[k] += std::norm(a[k]);
    }
    return p;
}

// Hz, over 20 Hz .. 16 kHz; 0 for silence.
double centroid(const std::vector<double>& p) {
    double num = 0.0, den = 0.0;
    for (size_t k = 0; k < p.size(); ++k) {
        const double f = static_cast<double>(k) * kSr / kFft;
        if (f < 20.0 || f > 16000.0) continue;
        num += f * p[k];
        den += p[k];
    }
    return den > 1e-12 ? num / den : 0.0;
}

// --- what a preset measures ------------------------------------------------------------------------
struct Look {
    double lufs = 0.0, peak = 0.0, rawPeak = 0.0, gr = 0.0, grShare = 0.0;
    double cent[4] = {};
    double levelMove = 0.0, brightMove = 0.0;
    double wet = 0.0, corr = 0.0, sideMid = 0.0;
    double t20 = -1.0, t40 = -1.0, end = 0.0;
};

// The limiter's work: y the output, raw the output without it (20 dB down, back up), 10 ms windows.
// From 0.2 s: a new instance's volume glides in from the default's -6 dB, and one 20 dB down glides
// from there differently for its first few ms.
void limiterWork(const Buf& L, const Buf& R, const Buf& rawL, const Buf& rawR, Look& k) {
    const size_t win = at(0.01);
    int touched = 0, windows = 0;
    for (size_t s = at(0.2); s + win <= L.size(); s += win, ++windows) {
        float y = 0.0f, x = 0.0f;
        for (size_t i = s; i < s + win; ++i) {
            y = std::max(y, std::max(std::fabs(L[i]), std::fabs(R[i])));
            x = std::max(x, std::max(std::fabs(rawL[i]), std::fabs(rawR[i])));
        }
        if (y < 1e-4f) continue;
        const double gr = dbOf(x) - dbOf(y);
        k.gr = std::max(k.gr, gr);
        if (gr > 0.1) ++touched;
    }
    k.grShare = windows ? 100.0 * touched / windows : 0.0;
}

// The energy over 1 kHz against the whole (dB); -100 for silence.
double brightness(const std::vector<double>& p) {
    double hi = 0.0, all = 0.0;
    for (size_t k = 0; k < p.size(); ++k) {
        const double f = static_cast<double>(k) * kSr / kFft;
        if (f < 20.0 || f > 16000.0) continue;
        all += p[k];
        if (f >= 1000.0) hi += p[k];
    }
    return all > 1e-12 ? 10.0 * std::log10(std::max(hi, 1e-30) / all) : -100.0;
}

// Within the holds, past the swell: the 1 s loudness's range, the 0.5 s brightness's spread (dB).
void motion(const Buf& L, const Buf& R, Look& k) {
    for (double from : {4.0, 16.0}) {
        const Buf l(L.begin() + static_cast<long>(at(from)), L.begin() + static_cast<long>(at(from + 8.0)));
        const Buf r(R.begin() + static_cast<long>(at(from)), R.begin() + static_cast<long>(at(from + 8.0)));
        const std::vector<double> z = afl::blockPowers(l, r, 1.0);
        double lo = 1e9, hi = -1e9;
        for (double v : z) {
            lo = std::min(lo, afl::loudnessOf(v));
            hi = std::max(hi, afl::loudnessOf(v));
        }
        k.levelMove = std::max(k.levelMove, hi - lo);
        std::vector<double> b;
        for (double t = from; t + 0.5 <= from + 8.0; t += 0.5) b.push_back(brightness(powerSpectrum(L, R, at(t), at(t) + kFft)));
        double mean = 0.0, var = 0.0;
        for (double v : b) mean += v / static_cast<double>(b.size());
        for (double v : b) var += (v - mean) * (v - mean) / static_cast<double>(b.size());
        k.brightMove = std::max(k.brightMove, std::sqrt(var));
    }
}

// After the last key goes up: the 400 ms loudness against where it was in the second before.
void tail(const Buf& L, const Buf& R, Look& k) {
    const std::vector<double> z = afl::blockPowers(L, R);   // block i covers i * 0.1 s .. + 0.4 s
    auto level = [&z](size_t i) { return afl::loudnessOf(z[std::min(i, z.size() - 1)]); };
    double held = 0.0;
    for (size_t i = 230; i < 237; ++i) held += std::pow(10.0, level(i) / 10.0) / 7.0;   // 23.0 .. 24.0 s
    const double ref = 10.0 * std::log10(std::max(held, 1e-20));
    for (size_t i = 240; i < z.size(); ++i) {
        const double t = static_cast<double>(i) * 0.1 + 0.4 - 2.0 * afl::kHoldS;   // the block's end, after the keys
        if (k.t20 < 0.0 && level(i) < ref - 20.0) k.t20 = t;
        if (k.t40 < 0.0 && level(i) < ref - 40.0) k.t40 = t;
    }
    k.end = level(z.size() - 1) - ref;
}

Look look(const std::string& text, const Buf& L, const Buf& R) {
    Look k;
    k.lufs = afl::lufs(L, R);
    k.peak = dbOf(afl::peakOf(L, R));
    // 20 dB down: under the limiter's knee, it only looks (unless a preset is 20 dB over the ceiling).
    const float down = std::pow(10.0f, -20.0f / 20.0f);
    char vol[48];
    std::snprintf(vol, sizeof vol, "\nvolume=%.2f\n", afl::presetValue(text, af::P_VOLUME) - 20.0);
    Buf qL, qR, dL, dR;
    render(text + vol, qL, qR);
    render(text + vol + "s_return=0\n", dL, dR);   // the dry (the last line naming a key wins)
    Buf rawL(qL.size()), rawR(qR.size());
    for (size_t i = 0; i < qL.size(); ++i) {
        rawL[i] = qL[i] / down;
        rawR[i] = qR[i] / down;
    }
    k.rawPeak = dbOf(afl::peakOf(rawL, rawR));
    limiterWork(L, R, rawL, rawR, k);
    const double spans[4][2] = {{2.0, 12.0}, {14.0, 24.0}, {24.0, 32.0}, {32.0, 40.0}};
    for (int s = 0; s < 4; ++s) k.cent[s] = centroid(powerSpectrum(L, R, at(spans[s][0]), at(spans[s][1])));
    motion(L, R, k);
    double wet = 0.0, dry = 0.0;
    for (size_t i = at(4.0); i < at(12.0); ++i) {
        const double wl = qL[i] - dL[i], wr = qR[i] - dR[i];
        wet += wl * wl + wr * wr;
        dry += static_cast<double>(dL[i]) * dL[i] + static_cast<double>(dR[i]) * dR[i];
    }
    k.wet = 10.0 * std::log10(std::max(wet, 1e-20) / std::max(dry, 1e-20));
    double lr = 0.0, ll = 0.0, rr = 0.0, side = 0.0, mid = 0.0;
    for (size_t i = at(2.0); i < L.size(); ++i) {
        lr += static_cast<double>(L[i]) * R[i];
        ll += static_cast<double>(L[i]) * L[i];
        rr += static_cast<double>(R[i]) * R[i];
        side += (static_cast<double>(L[i]) - R[i]) * (static_cast<double>(L[i]) - R[i]);
        mid += (static_cast<double>(L[i]) + R[i]) * (static_cast<double>(L[i]) + R[i]);
    }
    k.corr = lr / std::sqrt(std::max(ll * rr, 1e-30));
    k.sideMid = 10.0 * std::log10(std::max(side, 1e-20) / std::max(mid, 1e-20));
    tail(L, R, k);
    return k;
}

std::string seconds(double t) {
    char b[16];
    if (t < 0.0) return "-";
    std::snprintf(b, sizeof b, "%.1f", t);
    return b;
}

void printHeader() {
    std::printf("%-26s %5s %5s %7s %8s  %-19s %9s %5s %5s %5s  %-14s %s\n", "preset", "LUFS", "peak", "pre-lim",
                "GR (%)", "centroid: holds rel", "motion", "wet", "corr", "S/M", "tail -20 -40 end", "volume");
}

void printLook(const std::string& name, const Look& k, float volume, const char* note = "") {
    char gr[24], cent[48], move[24];
    std::snprintf(gr, sizeof gr, "%.1f (%.0f)", k.gr, k.grShare);
    std::snprintf(cent, sizeof cent, "%4.0f %4.0f %4.0f %4.0f", k.cent[0], k.cent[1], k.cent[2], k.cent[3]);
    std::snprintf(move, sizeof move, "%.1f/%.1f", k.levelMove, k.brightMove);
    std::printf("%-26s %5.1f %5.1f %7.1f %8s  %-19s %9s %5.1f %5.2f %5.1f  %4s %4s %4.0f  %5.1f%s\n", name.c_str(), k.lufs,
                k.peak, k.rawPeak, gr, cent, move, k.wet, k.corr, k.sideMid, seconds(k.t20).c_str(), seconds(k.t40).c_str(),
                k.end, volume, note);
}

// "Drone Low Tide Hum": the category cut to five letters, for the table.
std::string label(const std::string& category, const std::string& name) { return category.substr(0, 5) + " " + name; }

void writeWav(const std::string& path, const Buf& L, const Buf& R) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&f](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&f](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(L.size() * 4);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(44100);
    u32(44100 * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(bytes);
    auto s16 = [](float v) {
        return static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)));
    };
    for (size_t i = 0; i < L.size(); ++i) {
        u16(s16(L[i]));
        u16(s16(R[i]));
    }
}

std::string slug(const std::string& s) {
    std::string o;
    for (char c : s) o += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return o;
}

// "volume=..." replaced, or added right after the header line, in a preset file's text.
std::string withVolume(const std::string& text, float db) {
    char line[48];
    std::snprintf(line, sizeof line, "volume=%.1f", db);
    const bool has = text.find("\nvolume=") != std::string::npos;
    std::istringstream in(text);
    std::string out, l;
    bool header = true;
    while (std::getline(in, l)) {
        if (has && l.compare(0, 7, "volume=") == 0) l = line;
        out += l + "\n";
        if (header && !has) out += std::string(line) + "\n";
        header = false;
    }
    return out;
}

int match(const char* dir, double target) {
    int off = 0;
    std::vector<std::filesystem::path> files;
    for (const auto& cat : std::filesystem::directory_iterator(dir))
        if (cat.is_directory())
            for (const auto& f : std::filesystem::directory_iterator(cat.path()))
                if (f.path().extension() == ".afp") files.push_back(f.path());
    std::sort(files.begin(), files.end());
    printHeader();
    for (const auto& path : files) {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        Buf L, R;
        float vol = afl::presetValue(text, af::P_VOLUME);
        text = withVolume(text, vol);
        render(text, L, R);
        double l = afl::lufs(L, R);
        // The volume comes after everything but the limiter: a pass lands on the target (to the
        // volume's 0.1 dB) unless the limiter takes some of the change, which the next pass makes up.
        for (int pass = 0; pass < 4 && std::fabs(target - l) >= 0.05; ++pass) {
            const float want = std::round(static_cast<float>(vol + (target - l)) * 10.0f) / 10.0f;
            const float now = std::clamp(want, -30.0f, 6.0f);
            if (now == vol) break;   // at a stop, or as near as 0.1 dB gets
            vol = now;
            text = withVolume(text, vol);
            render(text, L, R);
            l = afl::lufs(L, R);
        }
        const Look k = look(text, L, R);
        const bool miss = std::fabs(l - target) > afl::kTolLu;
        off += miss;
        std::string cat = path.parent_path().filename().string(), name = path.stem().string();
        for (std::string* t : {&cat, &name}) {   // "02_Low_Tide_Hum" -> "Low Tide Hum", as the browser shows them
            t->erase(0, t->find('_') + 1);
            std::replace(t->begin(), t->end(), '_', ' ');
        }
        printLook(label(cat, name), k, vol, miss ? "  OFF TARGET" : "");
        std::ofstream(path) << text;
    }
    return off ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    setenv("AF_FIXED_SEED", "1", 1);   // the same random numbers on every run: the levels match
    if (!afl::waitForTables()) {
        std::fprintf(stderr, "demos: the tables weren't built\n");
        return 1;
    }
    if (argc == 4 && !std::strcmp(argv[1], "--match")) return match(argv[2], std::atof(argv[3]));
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <outdir> | --match <preset dir> <LUFS>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    std::filesystem::create_directories(dir);
    printHeader();
    Buf tourL, tourR;
    for (int i = 0; i < af::kNumFactoryPresets; ++i) {
        const af::FactoryPreset& p = af::kFactoryPresets[i];
        Buf L, R;
        render(p.text, L, R);
        char name[160];
        std::snprintf(name, sizeof name, "%s/%02d_%s_%s.wav", dir.c_str(), i + 1, slug(p.category).c_str(), slug(p.name).c_str());
        writeWav(name, L, R);
        printLook(label(p.category, p.name), look(p.text, L, R), afl::presetValue(p.text, af::P_VOLUME));
        tourL.insert(tourL.end(), L.begin(), L.end());
        tourR.insert(tourR.end(), R.begin(), R.end());
        tourL.insert(tourL.end(), 22050, 0.0f);
        tourR.insert(tourR.end(), 22050, 0.0f);
    }
    writeWav(dir + "/tour.wav", tourL, tourR);
    return 0;
}
