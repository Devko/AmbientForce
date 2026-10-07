// The soak test (make soak HOURS=1 [SEED=1]): hours of audio rendered offline on x86 through the
// plugin's own entry points (its sources linked in, as tools/demos.cpp: VSTPluginMain, 128-frame
// blocks, MIDI at sample offsets, parameters by index, the transport), the way a long ambient set
// plays it. From one seeded random sequence:
// - a new chord every 20-90 s: one to three keys (each key its own chord), the new keys down before
//   the old ones go up; a quarter of the changes under the pedal, let up 2-8 s later;
// - Freeze on for 10-60 s every 3-8 minutes;
// - Shimmer every 2-6 minutes: off half the time, else 20-100% at one of its intervals;
// - every 8-16 minutes the next factory preset (as a project chunk: every parameter's default,
//   then the preset's lines, which is what loading the preset does) or another Space mode;
// - every 15-30 minutes the transport stops (On Stop) and starts again 3-20 s later, at a new
//   tempo, with a new chord;
// - every 20-40 minutes the host suspends processing and resumes it, within the Stop window (On
//   Stop applies) or seconds later (a reset), and a new chord follows.
// The surface's clock (Surface::clock, which the plugin's suspend times read too) runs with the
// audio, and AF_FIXED_SEED fixes the plugin's own random numbers: a run plays the same samples
// every time.
//
// It fails on:
// - any sample that isn't finite;
// - any peak over -1 dBFS (kPeakMax);
// - a 10-minute window whose mean |DC| is over -60 dBFS: each 10 s's mean, its magnitude, averaged
//   over the window, for L and R each (an offset that changes sign doesn't cancel out);
// - a 10-minute window whose loudness is more than 6 LU from the first window's.
// The loudness is ITU-R BS.1770's integrated loudness over the window (tools/demos.cpp's measure:
// K-weighting, 400 ms blocks every 100 ms, the -70 LUFS absolute and -10 LU relative gates). The
// gates keep what a Stop leaves (a fade, then silence until the transport starts again) from
// pulling a window down: what it compares is how loud the music is while it plays.
//
// It prints every window (loudness and its drift, peak, DC, what happened in it), and the CPU time
// the plugin took against the audio's length, with the slowest block and the event before it (x86,
// where the host's own hiccups show up there too: it says nothing about the device).
//
//   soak [hours] [seed]          AF_SOAK_EVENTS=1: every event as it happens, with its time
#include "../plugin/patch_map.h"
#include "../plugin/surface.h"
#include "../plugin/tables.h"
#include "../plugin/vst2.h"
#include "factory_presets.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback);

namespace {

using namespace af;

constexpr double kSr = 44100.0;
constexpr int kBlock = 128;
constexpr long long kWindow = 600LL * 44100;   // 10 minutes
constexpr int kSeg = 4410;                     // 100 ms: the loudness blocks' step
constexpr int kDcSeg = 441000;                 // 10 s: the DC's piece
constexpr float kPeakMax = 0.8913f;            // -1 dBFS
constexpr double kDcMax = 0.001;               // -60 dBFS
constexpr double kDriftMax = 6.0;              // LU

VstTimeInfo g_time{};
long long g_samples = 0;     // rendered
long long g_awayMs = 0;      // the host's suspends, which take time but render nothing
long long clockMs() { return g_samples * 1000 / 44100 + g_awayMs + 1; }

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;   // audioMasterVersion
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

double cpuS() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

double wallS() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

struct Rng {   // splitmix64
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uni(double a, double b) { return a + (b - a) * static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    int pick(int n) { return static_cast<int>(next() % static_cast<uint64_t>(n)); }
    bool chance(double p) { return uni(0.0, 1.0) < p; }
};

// --- loudness: ITU-R BS.1770-4 (tools/demos.cpp's) ---------------------------------------------
struct Biquad {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// K-weighting for 44.1 kHz (libebur128's formulas): the head's high shelf, then the RLB high-pass.
struct KWeight {
    Biquad shelf, hp;
    KWeight() {
        double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        double K = std::tan(M_PI * f0 / kSr), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        double a0 = 1.0 + K / Q + K * K;
        shelf = {(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
        f0 = 38.13547087602444;
        Q = 0.5003270373238773;
        K = std::tan(M_PI * f0 / kSr);
        a0 = 1.0 + K / Q + K * K;
        hp = {1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    }
    double run(double x) { return hp.run(shelf.run(x)); }
};

double loudOf(double ms) { return -0.691 + 10.0 * std::log10(std::max(ms, 1e-20)); }

// Integrated loudness from the window's 100 ms sums of L^2 + R^2 (K-weighted): 400 ms blocks every
// 100 ms, gated. -100: nothing over the absolute gate.
double integrated(const std::vector<double>& seg) {
    std::vector<double> z;
    for (size_t j = 0; j + 4 <= seg.size(); ++j) z.push_back((seg[j] + seg[j + 1] + seg[j + 2] + seg[j + 3]) / (4.0 * kSeg));
    double sum = 0.0;
    int n = 0;
    for (double v : z)
        if (loudOf(v) > -70.0) sum += v, ++n;
    if (!n) return -100.0;
    const double rel = loudOf(sum / n) - 10.0;
    sum = 0.0;
    n = 0;
    for (double v : z)
        if (loudOf(v) > -70.0 && loudOf(v) > rel) sum += v, ++n;
    return n ? loudOf(sum / n) : -100.0;
}

double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

// --- what a window saw ---------------------------------------------------------------------------
struct Window {
    long long samples = 0;
    std::vector<double> seg;          // K-weighted L^2 + R^2 per 100 ms
    double segAcc = 0.0;
    double dcL = 0.0, dcR = 0.0;      // this 10 s's sums
    double dcAbsL = 0.0, dcAbsR = 0.0;   // the 10 s means' magnitudes, summed
    int dcPieces = 0;
    float peak = 0.0f;
    int chords = 0, freezes = 0, shimmers = 0, presets = 0, modes = 0, stops = 0, suspends = 0;

    void closePieces() {
        const long long inSeg = samples % kSeg, inDc = samples % kDcSeg;
        if (inSeg > 0) seg.push_back(segAcc * kSeg / static_cast<double>(inSeg));   // a part block, as if whole
        if (inDc >= 44100) {                       // a part piece of a second or more counts
            dcAbsL += std::fabs(dcL / static_cast<double>(inDc));
            dcAbsR += std::fabs(dcR / static_cast<double>(inDc));
            ++dcPieces;
        }
    }
    double dc() const { return dcPieces ? std::max(dcAbsL, dcAbsR) / dcPieces : 0.0; }
};

// --- the player -----------------------------------------------------------------------------------
struct Soak {
    AEffect* e = nullptr;
    Rng rng;
    std::string defaults;             // a chunk of every sound parameter at its default
    int preset = 0;
    int spaceMode = 0;
    bool frozen = false;
    std::vector<int> held;            // the keys down (ours: the engine may have let them go)
    std::vector<VstMidiEvent> pending;   // this block's MIDI

    // When, in samples (-1: not due).
    long long nextChord = 0, pedalUp = -1, nextFreeze = 0, freezeOff = -1, nextShimmer = 0, nextPatch = 0,
              nextStop = 0, restart = -1, nextSuspend = 0;

    // The last event and when (the slowest block's report names it); every one printed with
    // AF_SOAK_EVENTS set.
    const char* last = "the start";
    long long lastAt = 0;
    bool verbose = false;

    long long in(double lo, double hi) { return g_samples + static_cast<long long>(rng.uni(lo, hi) * kSr); }
    void mark(const char* what) {
        last = what;
        lastAt = g_samples;
        if (verbose) std::printf("    %9.1f s  %s\n", static_cast<double>(g_samples) / kSr, what);
    }

    void set(int id, float v) { e->setParameter(e, id, paramNorm(id, v)); }
    void midi(uint8_t st, uint8_t d1, uint8_t d2, int delta) {
        VstMidiEvent m{};
        m.type = vst::kVstMidiType;
        m.byteSize = sizeof m;
        m.deltaFrames = std::clamp(delta, 0, kBlock - 1);
        m.midiData[0] = st;
        m.midiData[1] = d1;
        m.midiData[2] = d2;
        pending.push_back(m);
    }
    void send() {
        if (pending.empty()) return;
        std::vector<char> buf(sizeof(VstEvents) + pending.size() * sizeof(VstEvent*));
        auto* evs = reinterpret_cast<VstEvents*>(buf.data());
        evs->numEvents = static_cast<int32_t>(pending.size());
        for (size_t k = 0; k < pending.size(); ++k) evs->events[k] = reinterpret_cast<VstEvent*>(&pending[k]);
        e->dispatcher(e, vst::effProcessEvents, 0, 0, evs, 0.0f);
        pending.clear();
    }

    void loadPreset(int i) {
        const FactoryPreset& p = kFactoryPresets[i];
        std::string text = p.text;
        const size_t body = text.find('\n');
        text = defaults + (body == std::string::npos ? "" : text.substr(body + 1)) + "preset=builtin:" + p.name + "\n";
        e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(text.size()), const_cast<char*>(text.data()), 0.0f);
        spaceMode = static_cast<int>(paramValue(P_S_MODE, e->getParameter(e, P_S_MODE)));
        frozen = paramValue(P_S_FREEZE, e->getParameter(e, P_S_FREEZE)) > 0.5f;
    }

    // One to three keys, each its own chord: the new ones down, then the old ones up.
    void chord(Window& w) {
        const int n = rng.chance(0.6) ? 1 : rng.chance(0.75) ? 2 : 3;
        std::vector<int> keys;
        int k = 45 + rng.pick(20);
        for (int i = 0; i < n; ++i, k += 3 + rng.pick(7)) keys.push_back(k);
        if (pedalUp < 0 && rng.chance(0.25)) {
            midi(0xB0, 64, 127, 0);
            pedalUp = in(2.0, 8.0);
        }
        int at = rng.pick(64);
        for (int key : keys) midi(0x90, static_cast<uint8_t>(key), static_cast<uint8_t>(60 + rng.pick(51)), at += rng.pick(8));
        for (int key : held)
            if (std::find(keys.begin(), keys.end(), key) == keys.end()) midi(0x80, static_cast<uint8_t>(key), 0, at + 1);
        held = keys;
        ++w.chords;
        mark(n == 1 ? "a chord (one key)" : n == 2 ? "a chord (two keys)" : "a chord (three keys)");
        nextChord = in(20.0, 90.0);
    }

    void transport(bool playing) {
        if (playing) {
            g_time.flags |= vst::kVstTransportPlaying;
            g_time.tempo = 60.0 + 10.0 * rng.pick(7);
        } else {
            g_time.flags &= ~vst::kVstTransportPlaying;
        }
    }

    // What falls due before this block.
    void events(Window& w) {
        const long long now = g_samples;
        if (restart >= 0 && now >= restart) {   // the transport starts again, with a chord
            restart = -1;
            transport(true);
            mark("the transport started");
            chord(w);
        }
        if (restart < 0 && now >= nextChord) chord(w);
        if (pedalUp >= 0 && now >= pedalUp) {
            midi(0xB0, 64, 0, rng.pick(kBlock));
            pedalUp = -1;
            mark("the pedal up");
        }
        if (freezeOff >= 0 && now >= freezeOff) {
            set(P_S_FREEZE, 0);
            frozen = false;
            freezeOff = -1;
            mark("Freeze off");
        }
        if (now >= nextFreeze) {
            set(P_S_FREEZE, 1);
            frozen = true;
            freezeOff = in(10.0, 60.0);
            ++w.freezes;
            mark("Freeze on");
            nextFreeze = in(180.0, 480.0);
        }
        if (now >= nextShimmer) {
            const bool off = rng.chance(0.5);
            set(P_S_SHIMMER, off ? 0.0f : static_cast<float>(rng.uni(0.2, 1.0)));
            set(P_S_SHINT, rng.pick(Reverb::kIntervals));
            ++w.shimmers;
            mark(off ? "Shimmer off" : "Shimmer on");
            nextShimmer = in(120.0, 360.0);
        }
        if (now >= nextPatch) {
            if (rng.chance(0.5)) {
                preset = (preset + 1) % kNumFactoryPresets;
                loadPreset(preset);
                ++w.presets;
                mark("a preset");
            } else {
                spaceMode = (spaceMode + 1 + rng.pick(Reverb::kModes - 1)) % Reverb::kModes;
                set(P_S_MODE, spaceMode);
                ++w.modes;
                mark(Reverb::kModeNames[spaceMode]);
            }
            nextPatch = in(480.0, 960.0);
        }
        if (restart < 0 && now >= nextStop) {
            transport(false);
            restart = in(3.0, 20.0);
            ++w.stops;
            mark("the transport stopped");
            nextStop = in(900.0, 1800.0);
        }
        if (now >= nextSuspend) {   // effMainsChanged as MPC sends it; the time away renders nothing
            send();
            e->dispatcher(e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);
            const bool brief = rng.chance(0.5);
            g_awayMs += brief ? 100 : static_cast<long long>(rng.uni(1.0, 10.0) * 1000.0);
            e->dispatcher(e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);
            ++w.suspends;
            mark(brief ? "a suspend of 100 ms (a Stop)" : "a suspend of seconds (a reset)");
            nextSuspend = in(1200.0, 2400.0);
            nextChord = std::min(nextChord, in(0.5, 3.0));
        }
        send();
    }
};

} // namespace

int main(int argc, char** argv) {
    const double hours = argc > 1 ? std::atof(argv[1]) : 1.0;
    const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    if (!(hours > 0.0) || hours > 1000.0) {
        std::fprintf(stderr, "usage: %s [hours] [seed]\n", argv[0]);
        return 2;
    }
    // Hermetic: no user folders, nothing saved; the plugin's random numbers fixed.
    setenv("AF_PRESET_ROOTS", "/nonexistent-afsoak", 1);
    setenv("AF_DATA_DIR", "", 1);
    setenv("AF_FIXED_SEED", "1", 1);
    Surface::clock = clockMs;
    g_time.sampleRate = kSr;
    g_time.timeSigNumerator = g_time.timeSigDenominator = 4;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;

    Soak s;
    s.rng.s = seed;
    s.verbose = std::getenv("AF_SOAK_EVENTS") != nullptr;
    s.e = VSTPluginMain(master);
    s.e->dispatcher(s.e, vst::effOpen, 0, 0, nullptr, 0.0f);
    // The tables first (the builder thread the instance started), so the whole run reads them.
    const double t0 = wallS();
    for (bool all = false; !all && wallS() - t0 < 600.0;) {
        all = true;
        for (const auto& slot : sharedTables().t) all = all && slot.load() != nullptr;
        const timespec nap{0, 10 * 1000 * 1000};
        if (!all) nanosleep(&nap, nullptr);
    }
    std::printf("soak: %.2f h of audio, seed %llu, %d factory presets; tables built in %.1f s\n", hours,
                static_cast<unsigned long long>(seed), kNumFactoryPresets, wallS() - t0);
    s.defaults = "ambientforce 1\n";
    for (int i = 0; i < P_COUNT; ++i)
        if (PARAM_INFO[i].kind == Kind::Synth)
            s.defaults += std::string(PARAM_INFO[i].key) + "=" + std::to_string(paramValue(i, PARAM_INFO[i].def)) + "\n";
    s.loadPreset(0);
    s.transport(true);
    s.nextFreeze = s.in(180.0, 480.0);
    s.nextShimmer = s.in(60.0, 240.0);
    s.nextPatch = s.in(480.0, 960.0);
    s.nextStop = s.in(900.0, 1800.0);
    s.nextSuspend = s.in(1200.0, 2400.0);

    const long long total = (static_cast<long long>(hours * 3600.0 * kSr) + kBlock - 1) / kBlock * kBlock;
    std::vector<Window> done;
    Window w;
    KWeight kl, kr;
    float L[kBlock], R[kBlock];
    float* out[2] = {L, R};
    long long nonFinite = 0, overPeak = 0, firstBad = -1;
    float peak = 0.0f;
    double cpu = 0.0, worstBlockS = 0.0;
    long long worstAt = 0, worstAfter = 0;
    const char* worstEvent = "";
    const double wall0 = wallS();
    auto close = [&](Window& x) {
        x.closePieces();
        done.push_back(x);
        x = Window{};
    };
    std::printf("  window          LUFS   drift   peak dBFS   DC dBFS   chords freeze shimmer preset mode stop suspend\n");
    double reference = 0.0;
    bool fail = false;
    auto report = [&](const Window& x, int index) {
        const double lufs = integrated(x.seg), drift = index == 0 ? 0.0 : lufs - reference;
        if (index == 0) reference = lufs;
        const bool loud = lufs > -70.0 && std::fabs(drift) <= kDriftMax, dcOk = x.dc() <= kDcMax;
        fail = fail || !loud || !dcOk;
        const long long from = static_cast<long long>(index) * 600, to = from + x.samples / 44100;
        std::printf("  %2lld:%02lld-%2lld:%02lld   %6.1f  %+5.1f%s   %6.1f      %6.1f%s    %4d %6d %7d %6d %4d %4d %7d\n",
                    from / 3600, from / 60 % 60, to / 3600, to / 60 % 60, lufs, drift, loud ? " " : "!", db(x.peak),
                    db(x.dc()), dcOk ? " " : "!", x.chords, x.freezes, x.shimmers, x.presets, x.modes, x.stops,
                    x.suspends);
        std::fflush(stdout);
    };
    while (g_samples < total) {
        s.events(w);
        const double c0 = cpuS();
        s.e->processReplacing(s.e, nullptr, out, kBlock);
        const double took = cpuS() - c0;
        cpu += took;
        if (took > worstBlockS) {
            worstBlockS = took;
            worstAt = g_samples;
            worstEvent = s.last;
            worstAfter = g_samples - s.lastAt;
        }
        if (g_time.flags & vst::kVstTransportPlaying) {
            g_time.samplePos += kBlock;
            g_time.ppqPos += kBlock / kSr * g_time.tempo / 60.0;
        }
        for (int i = 0; i < kBlock; ++i) {
            const float l = L[i], r = R[i];
            if (!std::isfinite(l) || !std::isfinite(r)) {
                if (firstBad < 0) firstBad = g_samples + i;
                ++nonFinite;
                continue;
            }
            const float a = std::max(std::fabs(l), std::fabs(r));
            if (a > kPeakMax) {
                if (firstBad < 0) firstBad = g_samples + i;
                ++overPeak;
            }
            w.peak = std::max(w.peak, a);
            const double yl = kl.run(l), yr = kr.run(r);
            w.segAcc += yl * yl + yr * yr;
            w.dcL += l;
            w.dcR += r;
            ++w.samples;
            if (w.samples % kSeg == 0) {
                w.seg.push_back(w.segAcc);
                w.segAcc = 0.0;
            }
            if (w.samples % kDcSeg == 0) {
                w.dcAbsL += std::fabs(w.dcL / kDcSeg);
                w.dcAbsR += std::fabs(w.dcR / kDcSeg);
                w.dcL = w.dcR = 0.0;
                ++w.dcPieces;
            }
            if (w.samples == kWindow) {
                peak = std::max(peak, w.peak);
                close(w);
                report(done.back(), static_cast<int>(done.size()) - 1);
            }
        }
        g_samples += kBlock;
    }
    if (w.samples >= 60 * 44100 || done.empty()) {   // a last part window of a minute or more
        peak = std::max(peak, w.peak);
        close(w);
        report(done.back(), static_cast<int>(done.size()) - 1);
    }
    s.e->dispatcher(s.e, vst::effClose, 0, 0, nullptr, 0.0f);

    const double audioS = static_cast<double>(g_samples) / kSr, wall = wallS() - wall0;
    int events[7] = {};
    for (const Window& x : done) {
        events[0] += x.chords, events[1] += x.freezes, events[2] += x.shimmers, events[3] += x.presets;
        events[4] += x.modes, events[5] += x.stops, events[6] += x.suspends;
    }
    std::printf("  %d chords, %d freezes, %d shimmer changes, %d presets, %d Space modes, %d stops, %d suspends\n",
                events[0], events[1], events[2], events[3], events[4], events[5], events[6]);
    std::printf("  peak %.2f dBFS (limit %.2f), %lld samples over it, %lld not finite\n", db(peak), db(kPeakMax), overPeak,
                nonFinite);
    std::printf("  CPU: the plugin %.1f s for %.2f h of audio (%.0fx real time, %.2f%% of a block on average, the "
                "slowest block %.0f us, at %.1f s, %.2f s after %s); %.1f s in all\n",
                cpu, audioS / 3600.0, audioS / std::max(cpu, 1e-9), 100.0 * cpu / audioS, worstBlockS * 1e6,
                static_cast<double>(worstAt) / kSr, static_cast<double>(worstAfter) / kSr, worstEvent, wall);
    fail = fail || nonFinite > 0 || overPeak > 0;
    if (firstBad >= 0) std::printf("  first bad sample at %.3f s\n", static_cast<double>(firstBad) / kSr);
    std::printf("soak: %s (non-finite: none; peaks <= -1 dBFS; mean |DC| <= -60 dBFS; loudness within %.0f LU of the "
                "first window)\n",
                fail ? "FAIL" : "PASS", kDriftMax);
    return fail ? 1 : 0;
}
