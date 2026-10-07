// The soak test (make soak HOURS=1 [SEED=1]): hours of audio rendered offline on x86 through the
// plugin's own entry points (its sources linked in, as tools/demos.cpp: VSTPluginMain, 128-frame
// blocks, MIDI at sample offsets, parameters by index, the transport), the way a long ambient set
// plays it. From one seeded random sequence:
// - a new chord every 20-90 s: one to three keys (each key its own chord), the new keys down before
//   the old ones go up; a quarter of the changes under the pedal, let up 2-8 s later;
// - Freeze on for 10-60 s every 3-8 minutes;
// - Shimmer every 2-6 minutes: off half the time, else 20-100% at one of its intervals;
// - every 8-16 minutes the next factory preset (the browser's Next button, the way a preset loads
//   on the device; at the end of the list the Init button, and on from there) or another Space
//   mode;
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
// - any trip of the engine's non-finite guard (af::guardTrips(): it zeroes the block and resets the
//   DSP, so its output looks clean, but a trip is a fault in the DSP);
// - any peak over -1 dBFS (kPeakMax);
// - a 10-minute window whose mean |DC| is over -60 dBFS: each 10 s's mean, its magnitude, averaged
//   over the window, for L and R each (an offset that changes sign doesn't cancel out);
// - a 10-minute window whose loudness is more than 6 LU from the first window's.
// The loudness is ITU-R BS.1770's integrated loudness over the window (tools/loudness.h: K-weighting,
// 400 ms blocks every 100 ms, the -70 LUFS absolute and -10 LU relative gates). The gates keep what
// a Stop leaves (a fade, then silence until the transport starts again) from pulling a window down:
// what it compares is how loud the music is while it plays.
// It warns (no fail) when a window had the limiter working (af::limitedSamples()) more than
// kLimitWarn of the time: the presets are levelled well under the ceiling, so a limiter that works
// that much means something is louder than it should be.
//
// It prints every window (loudness and its drift, peak, DC, the limiter's share, what happened in
// it), and the CPU time the plugin took against the audio's length, with the slowest block and the
// event before it (x86, where the host's own hiccups show up there too: it says nothing about the
// device).
//
//   soak [hours] [seed]          AF_SOAK_EVENTS=1: every event as it happens, with its time
#include "../plugin/patch_map.h"
#include "../plugin/surface.h"
#include "../plugin/tables.h"
#include "../plugin/vst2.h"
#include "factory_presets.h"
#include "loudness.h"
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
constexpr int kDcSeg = 441000;                 // 10 s: the DC's piece
constexpr float kPeakMax = 0.8913f;            // -1 dBFS
constexpr double kDcMax = 0.001;               // -60 dBFS
constexpr double kDriftMax = 6.0;              // LU
constexpr double kLimitWarn = 0.05;            // of a window's time
constexpr double kTablesWaitS = 600.0;

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

double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

// --- what a window saw ---------------------------------------------------------------------------
struct Window {
    long long samples = 0;
    double lufs = -100.0;             // set as it closes
    double dcL = 0.0, dcR = 0.0;      // this 10 s's sums
    double dcAbsL = 0.0, dcAbsR = 0.0;   // the 10 s means' magnitudes, summed
    int dcPieces = 0;
    float peak = 0.0f;
    uint32_t limited = 0, guards = 0;   // samples the limiter worked on, the guard's trips
    int chords = 0, freezes = 0, shimmers = 0, presets = 0, modes = 0, stops = 0, suspends = 0;

    void closePieces() {
        const long long inDc = samples % kDcSeg;
        if (inDc >= 44100) {   // a part piece of a second or more counts
            dcAbsL += std::fabs(dcL / static_cast<double>(inDc));
            dcAbsR += std::fabs(dcR / static_cast<double>(inDc));
            ++dcPieces;
        }
    }
    double dc() const { return dcPieces ? std::max(dcAbsL, dcAbsR) / dcPieces : 0.0; }
    double limitShare() const { return samples ? static_cast<double>(limited) / static_cast<double>(samples) : 0.0; }
};

// --- the player -----------------------------------------------------------------------------------
struct Soak {
    AEffect* e = nullptr;
    Rng rng;
    int spaceMode = 0;
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
    void press(int id) { e->setParameter(e, id, 1.0f); }   // a button: every 1 is a press
    std::string display(int id) {
        char b[256] = {};
        e->dispatcher(e, vst::effGetParamDisplay, id, 0, b, 0.0f);
        return b;
    }
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

    // The browser's Next; where it doesn't move (the end of the list), Init, and on from there.
    void nextPreset() {
        const std::string was = display(P_PRESET);
        press(P_PRESET_NEXT);
        if (display(P_PRESET) == was) press(P_PRE_INIT);
        spaceMode = static_cast<int>(paramValue(P_S_MODE, e->getParameter(e, P_S_MODE)));
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
            freezeOff = -1;
            mark("Freeze off");
        }
        if (now >= nextFreeze) {
            set(P_S_FREEZE, 1);
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
                nextPreset();
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
    const double hours = argc > 1 && *argv[1] ? std::atof(argv[1]) : 1.0;   // empty: the default
    const uint64_t seed = argc > 2 && *argv[2] ? std::strtoull(argv[2], nullptr, 10) : 1;
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
    bool tables = false;
    while (!tables && wallS() - t0 < kTablesWaitS) {
        tables = true;
        for (const auto& slot : sharedTables().t) tables = tables && slot.load() != nullptr;
        const timespec nap{0, 10 * 1000 * 1000};
        if (!tables) nanosleep(&nap, nullptr);
    }
    std::printf("soak: %.2f h of audio, seed %llu, %d factory presets; ", hours, static_cast<unsigned long long>(seed),
                kNumFactoryPresets);
    if (tables) std::printf("tables built in %.1f s\n", wallS() - t0);
    else std::printf("WARNING: the tables weren't all built in %.0f s, some slots play the sine for a while\n", kTablesWaitS);
    s.press(P_PRE_INIT);   // Init by its button: the browser's Next goes on from there
    s.transport(true);
    s.nextFreeze = s.in(180.0, 480.0);
    s.nextShimmer = s.in(60.0, 240.0);
    s.nextPatch = s.in(480.0, 960.0);
    s.nextStop = s.in(900.0, 1800.0);
    s.nextSuspend = s.in(1200.0, 2400.0);

    const long long total = (static_cast<long long>(hours * 3600.0 * kSr) + kBlock - 1) / kBlock * kBlock;
    std::vector<Window> done;
    Window w;
    loudness::Meter meter;
    float L[kBlock], R[kBlock];
    float* out[2] = {L, R};
    long long nonFinite = 0, overPeak = 0, firstBad = -1;
    float peak = 0.0f;
    double cpu = 0.0, worstBlockS = 0.0;
    long long worstAt = 0, worstAfter = 0;
    const char* worstEvent = "";
    uint32_t limitedWas = limitedSamples(), tripsWas = guardTrips();
    const double wall0 = wallS();
    std::printf("  window          LUFS   drift  peak dBFS  DC dBFS  limit  guard  chords freeze shimmer preset mode stop "
                "suspend\n");
    double reference = 0.0;
    bool fail = false;
    int limitWarnings = 0;
    // A window done: its loudness, its line, its verdict.
    auto close = [&]() {
        w.closePieces();
        w.lufs = meter.integrated();
        meter.clear();
        peak = std::max(peak, w.peak);
        const int index = static_cast<int>(done.size());
        if (index == 0) reference = w.lufs;
        const double drift = w.lufs - reference;
        const bool loud = w.lufs > -70.0 && std::fabs(drift) <= kDriftMax, dcOk = w.dc() <= kDcMax;
        const bool limits = w.limitShare() > kLimitWarn;
        fail = fail || !loud || !dcOk || w.guards > 0;
        limitWarnings += limits;
        const long long from = static_cast<long long>(index) * 600, to = from + w.samples / 44100;
        std::printf("  %2lld:%02lld-%2lld:%02lld   %6.1f  %+5.1f%s  %6.1f    %6.1f%s %5.1f%%%s %4u%s  %6d %6d %7d %6d %4d %4d %7d\n",
                    from / 3600, from / 60 % 60, to / 3600, to / 60 % 60, w.lufs, drift, loud ? " " : "!", db(w.peak),
                    db(w.dc()), dcOk ? " " : "!", 100.0 * w.limitShare(), limits ? "?" : " ", w.guards,
                    w.guards ? "!" : " ", w.chords, w.freezes, w.shimmers, w.presets, w.modes, w.stops, w.suspends);
        std::fflush(stdout);
        done.push_back(w);
        w = Window{};
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
        // The engine's counters, to the window the block started in.
        const uint32_t limited = limitedSamples(), trips = guardTrips();
        w.limited += limited - limitedWas;
        w.guards += trips - tripsWas;
        limitedWas = limited;
        tripsWas = trips;
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
            meter.add(l, r);
            w.dcL += l;
            w.dcR += r;
            ++w.samples;
            if (w.samples % kDcSeg == 0) {
                w.dcAbsL += std::fabs(w.dcL / kDcSeg);
                w.dcAbsR += std::fabs(w.dcR / kDcSeg);
                w.dcL = w.dcR = 0.0;
                ++w.dcPieces;
            }
            if (w.samples == kWindow) close();
        }
        g_samples += kBlock;
    }
    if (w.samples >= 60 * 44100 || done.empty()) close();   // a last part window of a minute or more
    peak = std::max(peak, w.peak);                           // and a shorter one's peak
    uint32_t trips = 0;
    for (const Window& x : done) trips += x.guards;
    trips += w.guards;
    s.e->dispatcher(s.e, vst::effClose, 0, 0, nullptr, 0.0f);

    const double audioS = static_cast<double>(g_samples) / kSr, wall = wallS() - wall0;
    int events[7] = {};
    for (const Window& x : done) {
        events[0] += x.chords, events[1] += x.freezes, events[2] += x.shimmers, events[3] += x.presets;
        events[4] += x.modes, events[5] += x.stops, events[6] += x.suspends;
    }
    std::printf("  %d chords, %d freezes, %d shimmer changes, %d presets, %d Space modes, %d stops, %d suspends\n",
                events[0], events[1], events[2], events[3], events[4], events[5], events[6]);
    std::printf("  peak %.2f dBFS (limit %.2f), %lld samples over it, %lld not finite, %u guard trips\n", db(peak),
                db(kPeakMax), overPeak, nonFinite, trips);
    if (limitWarnings)
        std::printf("  WARNING: %d windows had the limiter working over %.0f%% of the time (marked ?)\n", limitWarnings,
                    100.0 * kLimitWarn);
    std::printf("  CPU: the plugin %.1f s for %.2f h of audio (%.0fx real time, %.2f%% of a block on average, the "
                "slowest block %.0f us, at %.1f s, %.2f s after %s); %.1f s in all\n",
                cpu, audioS / 3600.0, audioS / std::max(cpu, 1e-9), 100.0 * cpu / audioS, worstBlockS * 1e6,
                static_cast<double>(worstAt) / kSr, static_cast<double>(worstAfter) / kSr, worstEvent, wall);
    fail = fail || nonFinite > 0 || overPeak > 0 || trips > 0;
    if (firstBad >= 0) std::printf("  first bad sample at %.3f s\n", static_cast<double>(firstBad) / kSr);
    std::printf("soak: %s (non-finite: none; guard trips: none; peaks <= -1 dBFS; mean |DC| <= -60 dBFS; loudness "
                "within %.0f LU of the first window)\n",
                fail ? "FAIL" : "PASS", kDriftMax);
    return fail ? 1 : 0;
}
