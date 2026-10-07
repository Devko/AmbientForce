// From SubForce tools/bench.cpp (8846421), renamed; AmbientForce's cases.
// CPU bench for the built plugin: dlopen()s the .so like MPC does, plays at 44.1 kHz / 128-frame
// blocks and times every processReplacing call with the thread's own CPU clock. Same verdict rule
// as sd88me's tools/bench.sh (percent of the 2902 us block): PASS p99 <= 15% and max <= 50%, WARN
// p99 <= 35% and max <= 80%, else FAIL.
//
//   afbench <plugin.so> [-s seconds] [-c cpu]
//
// Cases, each on a fresh instance set up through its parameters by index (real values, turned into
// MPC's 0..1 by plugin/patch_map.cpp's paramNorm), played for kWarmS untimed (the voices sounding,
// the reverb full), then timed for `seconds`:
//   idle         asleep: no note yet
//   init chord   the Init patch, one key held: its triad on Bloom, Ground on the root, Space's Hall
//   drone        Ground only (Bloom muted): every partial at full, Body and Breath; one key held
//   bloom 6x2    Bloom only (Ground muted): Chord Off and six keys, unison 2, Couple FM
//   worst        six keys (Chord Off), let go every kRestrikeS for six others (every voice taken
//                from its release), unison 2, FM; Ground at full (every partial, Body, Breath);
//                Space Abyss with Shimmer 100%, Freeze off; Tilt on
//
// The tables: the first instance starts the shared builder thread (plugin/tables.h), which builds
// every table at nice 10, seconds of work on the device, on the bench's own core (a new thread
// inherits the pin). Until a table is published its slots play the sine, which costs what a table
// does but sounds otherwise, and the builder would be timed with the case. So before any case
// afbench opens an instance and waits for the builder to finish: it counts the process's threads
// (/proc/self/task) before that instance and waits until the count is back there, the builder gone
// and every table published, and says how long that took. Every case then reads real tables, as an
// instance does in a running MPC.
//
// Hermetic: the plugin reads no user folders and saves nothing. A profiling build of the plugin
// (make arm-bench-stages: ambientforce_stages.so) also reports where each case's time goes, in us
// per block: Ground, Bloom, Space and the output (dsp/stages.h).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../plugin/patch_map.h"
#include "../plugin/vst2.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <sched.h>
#include <string>
#include <vector>

namespace {

constexpr int kBlock = 128;
constexpr double kRate = 44100.0;
constexpr double kBudgetUs = kBlock * 1e6 / kRate;   // 2902 us
constexpr int kWarmBlocks = 689;                     // 2 s: a case plays this long before it is timed
constexpr int kRestrikeBlocks = 689;                 // worst: another six keys every 2 s
constexpr double kTablesWaitS = 300.0;               // the builder's limit (it takes seconds)

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;   // audioMasterVersion
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

double cpuUs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

double wallS() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// The process's threads; -1 if /proc can't say.
int threads() {
    DIR* d = opendir("/proc/self/task");
    if (!d) return -1;
    int n = 0;
    while (const dirent* t = readdir(d))
        if (t->d_name[0] != '.') ++n;
    closedir(d);
    return n;
}

void midi(AEffect* e, uint8_t st, uint8_t d1, uint8_t d2) {
    VstMidiEvent ev{};
    ev.type = vst::kVstMidiType;
    ev.byteSize = sizeof ev;
    ev.midiData[0] = st;
    ev.midiData[1] = d1;
    ev.midiData[2] = d2;
    VstEvents evs{};
    evs.numEvents = 1;
    evs.events[0] = reinterpret_cast<VstEvent*>(&ev);
    e->dispatcher(e, vst::effProcessEvents, 0, 0, &evs, 0.0f);
}

// A parameter by index, in its real value (an option's index, a level 0..1, Hz, seconds).
void set(AEffect* e, int id, float v) { e->setParameter(e, id, af::paramNorm(id, v)); }

AEffect* openPlugin(void* lib) {
    auto entry = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(dlsym(lib, "VSTPluginMain"));
    AEffect* e = entry ? entry(master) : nullptr;
    if (!e) {
        std::fprintf(stderr, "no VSTPluginMain, or it made no plugin\n");
        std::exit(1);
    }
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    return e;
}

// The first instance, and the builder thread it starts: back when the thread has gone (every table
// published). The seconds that took; -1 when it didn't finish in kTablesWaitS, or /proc can't tell.
double waitForTables(void* lib) {
    const int before = threads();
    const double t0 = wallS();
    AEffect* e = openPlugin(lib);
    double took = -1.0;
    if (before > 0) {
        while (wallS() - t0 < kTablesWaitS) {
            if (threads() <= before) {
                took = wallS() - t0;
                break;
            }
            const timespec nap{0, 20 * 1000 * 1000};
            nanosleep(&nap, nullptr);
        }
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);   // the tables stay until the .so is unloaded
    return took;
}

enum Case : int { C_IDLE, C_INIT, C_DRONE, C_BLOOM, C_WORST, C_COUNT };
constexpr const char* kCaseNames[C_COUNT] = {"idle", "init chord", "drone", "bloom 6x2", "worst"};

// Two six-key chords for Chord Off (a voice a key): white keys, so Snap keeps every one apart, and
// no key in both, so a re-strike takes every voice from its release.
constexpr uint8_t kSix[2][6] = {{48, 52, 55, 59, 62, 67}, {50, 53, 57, 60, 64, 69}};

void setUp(AEffect* e, int c) {
    using namespace af;
    if (c == C_DRONE || c == C_WORST) {   // Ground at full
        for (int id : {P_G_SUB, P_G_ROOT, P_G_FIFTH, P_G_OCT, P_G_COLOR}) set(e, id, 1.0f);
        set(e, P_G_BODY, 0.5f);
        set(e, P_G_BREATH, 0.6f);
    }
    if (c == C_DRONE) set(e, P_B_MUTE, 1);
    if (c == C_BLOOM) set(e, P_G_MUTE, 1);
    if (c == C_BLOOM || c == C_WORST) {   // six voices of unison 2, B frequency-modulating A
        set(e, P_H_CHORD, CH_OFF);
        set(e, P_B_UNISON, 1);            // the option "2"
        set(e, P_B_COUPLE, CP_FM);
        set(e, P_B_CAMT, 0.5f);
        set(e, P_B_BLEND, 0.5f);
    }
    if (c == C_WORST) {
        set(e, P_S_MODE, Reverb::ABYSS);
        set(e, P_S_SHIMMER, 1.0f);
        set(e, P_S_FREEZE, 0);
        set(e, P_O_TILT, 0.3f);
    }
}

void keys(AEffect* e, int c, int chord, bool down) {
    const uint8_t st = down ? 0x90 : 0x80, vel = down ? 100 : 0;
    if (c == C_INIT || c == C_DRONE) midi(e, st, 60, vel);
    if (c == C_BLOOM || c == C_WORST)
        for (uint8_t k : kSix[chord]) midi(e, st, k, vel);
}

struct Result { double avg, p99, max; };

using StageFn = int (*)(double*, const char**, int);

Result runCase(void* lib, int seconds, int c, StageFn stages) {
    AEffect* e = openPlugin(lib);
    setUp(e, c);
    std::vector<float> L(kBlock), R(kBlock);
    float* out[2] = {L.data(), R.data()};
    if (c != C_IDLE) keys(e, c, 0, true);
    const int blocks = seconds * static_cast<int>(kRate) / kBlock;
    std::vector<double> t(static_cast<size_t>(blocks));
    int chord = 0;
    for (int b = -kWarmBlocks; b < blocks; ++b) {
        if (c == C_WORST && b > -kWarmBlocks && (b + kWarmBlocks) % kRestrikeBlocks == 0) {
            keys(e, c, chord, false);
            chord ^= 1;
            keys(e, c, chord, true);
        }
        if (b == 0 && stages) {   // the stage counters start over here: only the timed blocks count
            double us[8];
            const char* names[8];
            stages(us, names, 8);
        }
        const double t0 = cpuUs();
        e->processReplacing(e, nullptr, out, kBlock);
        if (b >= 0) t[static_cast<size_t>(b)] = cpuUs() - t0;
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    std::vector<double> s = t;
    std::sort(s.begin(), s.end());
    double sum = 0.0;
    for (double v : t) sum += v;
    const Result r{100.0 * sum / blocks / kBudgetUs, 100.0 * s[static_cast<size_t>(blocks * 0.99)] / kBudgetUs,
                   100.0 * s.back() / kBudgetUs};
    const char* verdict = r.p99 <= 15.0 && r.max <= 50.0 ? "PASS" : r.p99 <= 35.0 && r.max <= 80.0 ? "WARN" : "FAIL";
    std::printf("  %-28s avg %5.2f%%  p99 %5.2f%%  max %5.2f%%  %s\n", kCaseNames[c], r.avg, r.p99, r.max, verdict);
    return r;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugin.so> [-s seconds] [-c cpu]\n", argv[0]);
        return 2;
    }
    int seconds = 3, cpu = 1;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "-s")) seconds = std::max(1, std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "-c")) cpu = std::atoi(argv[i + 1]);
    }
    if (cpu >= 0) {   // -c -1: don't pin (x86 runs, CI)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0) std::printf("(could not pin to cpu %d)\n", cpu);
    }
    // Hermetic: no user folders, nothing saved.
    setenv("AF_PRESET_ROOTS", "/nonexistent-afbench", 1);
    setenv("AF_DATA_DIR", "", 1);
    g_time.sampleRate = kRate;
    g_time.tempo = 120.0;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto stages = reinterpret_cast<StageFn>(dlsym(lib, "AmbientForceStageTimes"));
    std::printf("%s, %d s per case, %s\n", argv[1], seconds, stages ? "profiling build" : "plain build");
    const double tables = waitForTables(lib);
    if (tables >= 0.0) std::printf("  tables built in %.1f s (the builder thread gone before the first case)\n", tables);
    else std::printf("  WARNING: the table builder hadn't finished after %.0f s (or /proc/self/task can't tell):\n"
                     "  the cases may play the sine fallback and time the builder too\n", kTablesWaitS);
    bool fail = false;
    for (int c = 0; c < C_COUNT; ++c) {
        double us[8] = {};
        const char* stageNames[8] = {};
        const Result r = runCase(lib, seconds, c, stages);
        fail = fail || !(r.p99 <= 35.0 && r.max <= 80.0);
        if (stages) {
            const int n = stages(us, stageNames, 8);
            const int blocks = seconds * static_cast<int>(kRate) / kBlock;
            std::printf("      per block:");
            for (int k = 0; k < n; ++k) std::printf("  %s %.1f us", stageNames[k], us[k] / blocks);
            std::printf("\n");
        }
    }
    dlclose(lib);
    return fail ? 1 : 0;
}
