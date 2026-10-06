// From SubForce tools/demos.cpp (8846421), renamed; one phrase for every preset, no filter sweep.
// Demo clips and preset levels, rendered through the plugin's own entry points (VSTPluginMain,
// 128-frame blocks, MIDI with sample offsets), the way MPC plays it.
//
//   demos <outdir>                 every factory preset playing the phrase, as
//                                  <outdir>/NN_<Category>_<Name>.wav, and all of them back to back
//                                  as <outdir>/tour.wav; prints each one's loudness
//   demos --match <dir> <LUFS>     sets volume= in every preset file under <dir> (presets/Factory)
//                                  so its phrase plays at <LUFS> integrated (ITU-R BS.1770 / EBU
//                                  R128: K-weighting, 400 ms blocks, -70 LUFS and -10 LU gates), or
//                                  lower where it would peak over kPeakCap
//
// The phrase, for now the same for every preset: two held chords, then their release. 120 BPM.
// The files are stereo as the plugin plays: the loudness is that of the stereo pair.
#include "../plugin/vst2.h"
#include "factory_presets.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
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

constexpr double kSr = 44100.0;
constexpr double kPeakCap = -1.0;   // dBFS: no factory preset peaks over it on its phrase
constexpr int kBlock = 128;
VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

struct Ev {
    double beat;      // when, in quarter notes
    uint8_t st, d1, d2;
};

// note, start beat, length in beats, velocity
void note(std::vector<Ev>& ev, int n, double at, double len, int vel) {
    ev.push_back({at, 0x90, static_cast<uint8_t>(n), static_cast<uint8_t>(vel)});
    ev.push_back({at + len, 0x80, static_cast<uint8_t>(n), 0});
}

std::vector<Ev> phrase(double& beats) {
    std::vector<Ev> ev;
    for (int n : {48, 55, 60, 64}) note(ev, n, 0.0, 7.5, 100);
    for (int n : {45, 52, 57, 60}) note(ev, n, 8.0, 7.5, 100);
    beats = 16.0;
    std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.beat < b.beat; });
    return ev;
}

std::vector<float> render(const std::string& state) {
    AEffect* e = VSTPluginMain(master);
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(state.size()), const_cast<char*>(state.data()), 0.0f);
    double beats = 0.0;
    const std::vector<Ev> ev = phrase(beats);
    const double spb = kSr * 60.0 / 120.0;   // samples per beat
    const size_t total = static_cast<size_t>((beats + 1.5) * spb);   // + the release
    std::vector<float> out((total / kBlock + 1) * kBlock);
    std::vector<float> R(kBlock);
    size_t next = 0;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid | vst::kVstTransportPlaying;
    for (size_t pos = 0; pos < out.size(); pos += kBlock) {
        g_time.ppqPos = static_cast<double>(pos) / spb;
        std::vector<VstMidiEvent> mev;
        while (next < ev.size() && ev[next].beat * spb < static_cast<double>(pos + kBlock)) {
            VstMidiEvent m{};
            m.type = vst::kVstMidiType;
            m.byteSize = sizeof m;
            m.deltaFrames = std::max(0, static_cast<int>(std::lround(ev[next].beat * spb)) - static_cast<int>(pos));
            m.deltaFrames = std::min(m.deltaFrames, kBlock - 1);
            m.midiData[0] = static_cast<char>(ev[next].st);
            m.midiData[1] = static_cast<char>(ev[next].d1);
            m.midiData[2] = static_cast<char>(ev[next].d2);
            mev.push_back(m);
            ++next;
        }
        if (!mev.empty()) {
            std::vector<char> buf(sizeof(VstEvents) + mev.size() * sizeof(VstEvent*));
            auto* evs = reinterpret_cast<VstEvents*>(buf.data());
            evs->numEvents = static_cast<int32_t>(mev.size());
            for (size_t k = 0; k < mev.size(); ++k) evs->events[k] = reinterpret_cast<VstEvent*>(&mev[k]);
            e->dispatcher(e, vst::effProcessEvents, 0, 0, evs, 0.0f);
        }
        float* o[2] = {&out[pos], R.data()};
        e->processReplacing(e, nullptr, o, kBlock);
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    out.resize(total);
    return out;
}

// --- loudness: ITU-R BS.1770-4 (both channels the same signal) -------------------------------
struct Biquad {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

double lufs(const std::vector<float>& x) {
    // K-weighting for 44.1 kHz (libebur128's formulas): the head's high shelf, then the RLB high-pass.
    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = std::tan(M_PI * f0 / kSr), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    Biquad shelf{(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    f0 = 38.13547087602444;
    Q = 0.5003270373238773;
    K = std::tan(M_PI * f0 / kSr);
    a0 = 1.0 + K / Q + K * K;
    Biquad hp{1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    std::vector<double> sq(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        const double y = hp.run(shelf.run(x[i]));
        sq[i] = y * y;
    }
    const size_t block = static_cast<size_t>(0.4 * kSr), hop = block / 4;
    std::vector<double> z;
    for (size_t at = 0; at + block <= sq.size(); at += hop) {
        double s = 0.0;
        for (size_t i = at; i < at + block; ++i) s += sq[i];
        z.push_back(2.0 * s / static_cast<double>(block));   // two channels
    }
    auto loud = [](double ms) { return -0.691 + 10.0 * std::log10(std::max(ms, 1e-20)); };
    double sum = 0.0;
    int n = 0;
    for (double v : z)
        if (loud(v) > -70.0) sum += v, ++n;
    if (!n) return -100.0;
    const double rel = loud(sum / n) - 10.0;
    sum = 0.0;
    n = 0;
    for (double v : z)
        if (loud(v) > -70.0 && loud(v) > rel) sum += v, ++n;
    return n ? loud(sum / n) : -100.0;
}

float peakOf(const std::vector<float>& x) {
    float p = 0.0f;
    for (float v : x) p = std::max(p, std::fabs(v));
    return p;
}

void writeWav(const std::string& path, const std::vector<float>& x) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&f](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&f](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(x.size() * 2);   // per channel
    f.write("RIFF", 4);
    u32(36 + 2 * bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);   // stereo, both channels the same: what MPC gets
    u32(44100);
    u32(44100 * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(2 * bytes);
    for (float v : x) {
        const uint16_t s = static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)));
        u16(s);
        u16(s);
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

float volumeOf(const std::string& text) {   // what the preset sets, or the parameter's default
    const size_t at = text.find("\nvolume=");
    if (at != std::string::npos) return std::strtof(text.c_str() + at + 8, nullptr);
    const af::ParamSpec& v = af::PARAM_SPECS[af::P_VOLUME];
    return v.lo + af::PARAM_INFO[af::P_VOLUME].def * (v.hi - v.lo);
}

} // namespace

int main(int argc, char** argv) {
    setenv("AF_FIXED_SEED", "1", 1);   // the same random numbers on every run: the levels match
    g_time.sampleRate = kSr;
    g_time.tempo = 120.0;
    if (argc == 4 && !std::strcmp(argv[1], "--match")) {
        const double target = std::atof(argv[3]);
        for (const auto& cat : std::filesystem::directory_iterator(argv[2])) {
            if (!cat.is_directory()) continue;
            for (const auto& f : std::filesystem::directory_iterator(cat.path())) {
                if (f.path().extension() != ".afp") continue;
                std::ifstream in(f.path());
                std::stringstream ss;
                ss << in.rdbuf();
                std::string text = ss.str();
                // The volume is after everything: two passes land on the target.
                for (int pass = 0; pass < 2; ++pass) {
                    const std::vector<float> x = render(text);
                    const double l = lufs(x);
                    // The loudness target, unless that would peak over kPeakCap.
                    const double room = kPeakCap - 20.0 * std::log10(std::max(peakOf(x), 1e-9f));
                    const float want = static_cast<float>(volumeOf(text) + std::min(target - l, room));
                    const float vol = std::clamp(want, -30.0f, 6.0f);
                    if (pass == 1 && vol != want)
                        std::printf("  %s: needs %.1f dB, the volume stops at %.1f\n", f.path().filename().c_str(), want, vol);
                    text = withVolume(text, vol);
                    if (pass == 1) {
                        const std::vector<float> y = render(text);
                        std::printf("%-40s %6.1f LUFS  peak %6.1f dBFS  volume %5.1f\n", f.path().filename().c_str(), lufs(y),
                                    20.0 * std::log10(std::max(peakOf(y), 1e-9f)), vol);
                    }
                }
                std::ofstream(f.path()) << text;
            }
        }
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <outdir> | --match <preset dir> <LUFS>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    std::filesystem::create_directories(dir);
    std::vector<float> tour;
    for (int i = 0; i < af::kNumFactoryPresets; ++i) {
        const af::FactoryPreset& p = af::kFactoryPresets[i];
        const std::vector<float> x = render(p.text);
        char name[160];
        std::snprintf(name, sizeof name, "%s/%02d_%s_%s.wav", dir.c_str(), i + 1, slug(p.category).c_str(), slug(p.name).c_str());
        writeWav(name, x);
        std::printf("%-14s %-20s %6.1f LUFS  peak %6.1f dBFS\n", p.category, p.name, lufs(x),
                    20.0 * std::log10(std::max(peakOf(x), 1e-9f)));
        tour.insert(tour.end(), x.begin(), x.end());
        tour.insert(tour.end(), 22050, 0.0f);
    }
    writeWav(dir + "/tour.wav", tour);
    return 0;
}
