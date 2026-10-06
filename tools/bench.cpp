// From SubForce tools/bench.cpp (8846421), renamed; the stub engine's cases.
// CPU bench for the built plugin: dlopen()s the .so like MPC does, plays notes at 44.1 kHz /
// 128-frame blocks and times every processReplacing call with the thread's own CPU clock. Same
// verdict rule as sd88me's tools/bench.sh (percent of the 2902 us block): PASS p99 <= 15% and
// max <= 50%, WARN p99 <= 35% and max <= 80%, else FAIL.
//
//   afbench <plugin.so> [-s seconds] [-c cpu]
//
// Cases: idle (no note), the Init patch on a held note, six held notes (every voice), and a new
// note every 50 ms (each one taking the oldest voice). Hermetic: the plugin reads no user folders
// and saves nothing. A profiling build of the plugin (make arm-bench-stages:
// ambientforce_stages.so) also reports where each case's time goes.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../plugin/vst2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <sched.h>
#include <string>
#include <vector>

namespace {

constexpr int kBlock = 128;
constexpr double kBudgetUs = kBlock * 1e6 / 44100.0;   // 2902 us

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

struct Result { double avg, p99, max; };

using StageFn = int (*)(double*, const char**, int);

Result runCase(void* lib, int seconds, const char* name, int mode, StageFn stages) {
    auto entry = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(dlsym(lib, "VSTPluginMain"));
    AEffect* e = entry ? entry(master) : nullptr;
    if (!e) {
        std::fprintf(stderr, "no VSTPluginMain, or it made no plugin\n");
        std::exit(1);
    }
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    std::vector<float> L(kBlock), R(kBlock);
    float* out[2] = {L.data(), R.data()};
    for (int b = 0; b < 32; ++b) e->processReplacing(e, nullptr, out, kBlock);   // the patch settles
    if (stages) {   // the stage counters start over here: only the timed blocks count
        double us[8];
        const char* names[8];
        stages(us, names, 8);
    }
    static const uint8_t chord[] = {36, 43, 48, 52, 55, 60};
    for (int k = 0; k < (mode == 0 ? 0 : mode == 1 ? 1 : 6); ++k) midi(e, 0x90, chord[k], 110);
    const int blocks = seconds * 44100 / kBlock;
    std::vector<double> t(static_cast<size_t>(blocks));
    int note = 0;
    for (int b = 0; b < blocks; ++b) {
        if (mode == 3 && b % 17 == 0) {   // every ~50 ms: a new key, the oldest voice taken
            ++note;
            midi(e, 0x90, static_cast<uint8_t>(36 + (note * 7) % 36), 100);
        }
        const double t0 = cpuUs();
        e->processReplacing(e, nullptr, out, kBlock);
        t[static_cast<size_t>(b)] = cpuUs() - t0;
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    std::vector<double> s = t;
    std::sort(s.begin(), s.end());
    double sum = 0.0;
    for (double v : t) sum += v;
    const Result r{100.0 * sum / blocks / kBudgetUs, 100.0 * s[static_cast<size_t>(blocks * 0.99)] / kBudgetUs,
                   100.0 * s.back() / kBudgetUs};
    const char* verdict = r.p99 <= 15.0 && r.max <= 50.0 ? "PASS" : r.p99 <= 35.0 && r.max <= 80.0 ? "WARN" : "FAIL";
    std::printf("  %-28s avg %5.2f%%  p99 %5.2f%%  max %5.2f%%  %s\n", name, r.avg, r.p99, r.max, verdict);
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
    g_time.sampleRate = 44100.0;
    g_time.tempo = 120.0;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto stages = reinterpret_cast<StageFn>(dlsym(lib, "AmbientForceStageTimes"));
    std::printf("%s, %d s per case, %s\n", argv[1], seconds, stages ? "profiling build" : "plain build");
    const char* names[] = {"idle (no note)", "Init, one held note", "six held notes", "a new note every 50 ms"};
    bool fail = false;
    for (int mode = 0; mode < 4; ++mode) {
        double us[8] = {};
        const char* stageNames[8] = {};
        const Result r = runCase(lib, seconds, names[mode], mode, stages);
        fail = fail || !(r.p99 <= 35.0 && r.max <= 80.0);
        if (stages) {
            const int n = stages(us, stageNames, 8);
            const int blocks = seconds * 44100 / kBlock;
            std::printf("      per block:");
            for (int k = 0; k < n; ++k) std::printf("  %s %.1f us", stageNames[k], us[k] / blocks);
            std::printf("\n");
        }
    }
    dlclose(lib);
    return fail ? 1 : 0;
}
