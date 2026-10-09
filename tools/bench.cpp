// From SubForce tools/bench.cpp (8846421), renamed; AmbientForce's cases.
// CPU bench for the built plugin: dlopen()s the .so like MPC does, plays at 44.1 kHz / 128-frame
// blocks and times every processReplacing call with the thread's own CPU clock. Same verdict rule
// as sd88me's tools/bench.sh (percent of the 2902 us block): PASS p99 <= 15% and max <= 50%, WARN
// p99 <= 35% and max <= 80%, else FAIL.
//
//   afbench <plugin.so> [-s seconds] [-c cpu]
//   afbench <plugin.so> --icount <case> <blocks>
//   afbench --cases
//
// Cases, each on a fresh instance set up through its parameters by index (real values, turned into
// MPC's 0..1 by plugin/patch_map.cpp's paramNorm), played for kWarmBlocks (2 s) untimed (the voices
// sounding, the reverb full), then timed for `seconds`:
//   idle         asleep: no note yet
//   init chord   the Init patch, one key held: its triad on Bloom, Ground on the root, Space's Hall
//   drone        Ground only (Bloom muted): every partial at full, Body and Breath; one key held
//   bloom 6x2    Bloom only (Ground muted): Chord Off and six keys, unison 2, Couple FM
//   worst        six keys (Chord Off), let go every kRestrikeBlocks (2 s) for six others (every
//                voice taken from its release), unison 2, FM; Ground at full (every partial, Body,
//                Breath); Sway 1 at 2 Hz on both strata, Bloom's Smear and Breath at 1 (the read
//                position crossing frames all the time: the dearest read); Space Abyss with Shimmer
//                100%, Freeze off; Tilt on
//
// M2's cases play the engine itself (dsp/engine.h, linked into afbench), not the plugin: they were
// written before the plugin had Air's, Weather's and Echo's parameters (M2's Task 9) and Weather's
// sources (Task 10), and still play so (their counts stay comparable). Their patch is the plugin's,
// made from the same parameters by the patch map (patchFromParams), with M2's fields set on it; their keys go to the engine at the block's first sample, as the plugin hands
// over an event at offset 0; the transport is the bench's host's (120 BPM, stopped). What the plugin
// adds around the engine (the parameters' snapshot, MIDI, the meter) isn't counted: `worst engine`
// is `worst` played so, to measure it.
//   air          Air alone (Ground and Bloom muted; Init's Space): Felt at Decay 20, Free at Density 60
//                with Loop on, and Split at 72 with six keys above it struck at the start, so six
//                voices ring from the first block and every generated note steals one (Air's worst)
//   weather      Weather alone (Ground and Bloom muted; Init's Space): 16 grains, Cloud, +12 (level-1
//                reads), To Key Chord, high-pass 200 Hz, tilt +0.3, Duck 0.5, Free, on a 12 s source
//                of noise made as the loader makes one (buildSource)
//   worst engine `worst`, through the engine
//   worst m2     `worst engine` and everything of M2 at its dearest (the plan's budget): Air as in
//                `air` (Harmony; its six keys above Split struck again with each re-strike), Weather as
//                in `weather` but in Stretch, the dearer mode (Harmony, ducking under Bloom), every
//                Echo send open into Echo at Ping-Pong, wow 1, Diffuse 1, duck 1, feedback 0.9 (never
//                silent), its return into the mix and Space, Air and Weather panned, Memory recording
//                the output and a Remember asked with every re-strike. Memory takes one after 2 s of
//                recording (88200 samples) and the re-strikes are 88192 apart, so every second one
//                is taken: the first at the warm-up's end (block 0), then blocks 1378, 2756 and so on;
//                `--icount "worst m2" 1379` less `1378` is a re-strike with its Remember, 689's one
//                refused
//   air strike   a low chord struck on Air (Ground and Bloom muted): Kalimba on the notes, six keys
//                from C1 struck again every kRestrikeBlocks: each strike builds six bursts of a
//                period at note 24 and over (airvoices.h: Air's spikes, bounded, above its share)
//   echo resume  a phrase after a silence: one key on Bloom (Ground muted, Release 0.3 s) for
//                kPhraseBlocks every kRestrikeBlocks into Echo at the worst's settings but 100 ms and
//                feedback 0.3, so Echo falls silent between the phrases and the engine stops running it;
//                each phrase starts it afresh (echo.h: the fresh start's first block, and the reads'
//                zeroing for a delay time after)
// The spikes (a re-strike's block, a strike's, a fresh start's) are a block or ten in the 512 that
// make arm-icount averages: their own blocks are counted by hand, as docs/PERFORMANCE.md says.
//
// The tables: the first instance starts the shared builder thread (plugin/tables.h), which builds
// every table at nice 10, seconds of work on the device, on the bench's own core (a new thread
// inherits the pin). Until a table is published its slots play the sine, which costs what a table
// does but sounds otherwise, and the builder would be timed with the case. So before any case
// afbench opens an instance and waits for the builder to finish: it counts the process's threads
// (/proc/self/task, but the instances' loaders, af-loader) before that instance and waits until the
// count is back there, the builder gone
// and every table published, and says how long that took. Every case then reads real tables, as an
// instance does in a running MPC. No new thread (the builder never started), or one still running
// after kTablesWaitS, fails the bench (the cases still run, for what they are worth). Where /proc
// can't be read it waits kTablesFallbackS instead, and says so.
//
// Hermetic: the plugin reads no user folders and saves nothing (it looks for the trace's flag file
// in a folder that doesn't exist), and AF_FIXED_SEED gives every run the same random numbers (the
// engine cases seed their engine with the same number). A profiling build of the plugin (make
// arm-bench-stages: ambientforce_stages.so) also reports where each plugin case's time goes, in us
// per block: Ground, Bloom, Air, Weather, Echo, Space and the output (dsp/stages.h).
//
// --icount is make arm-icount's half. It runs under a qemu-arm with TCG plugins, whose insn plugin
// counts instructions per vCPU index, modulo 8. In user mode every thread is a vCPU, and a new one
// takes the index after the highest alive, so cpu 0 is the main thread's alone as long as no thread
// gets index 8 (afbench has four at most: main, the helper below, the table builder, the instance's
// loader). It plays the
// case untimed (playCase, as the timed bench does), its 2 s and then <blocks> more, printing nothing
// unless something fails. The count that matters is the main thread's, and the main thread only
// plays. Everything whose instructions depend on the machine runs on a helper thread that main joins:
// the dlopen, the wait for the tables (it polls /proc with a sleep, so how often it looks depends on
// the machine's speed), opening the case's instance and setting its parameters (a parameter set looks
// for the trace's flag file once a second of wall time). A thread of the plugin's alive while the
// blocks play would go uncounted, so there may be none but the instance's loader (plugin/loader.h:
// it lives as long as the instance, and makes Weather's source, which the helper waits for, so it is
// idle while the blocks play; named af-loader, it is left out of the thread counts): the helper looks
// before it goes, and main after the blocks. The main thread's count is then the same on every run, but for a few dozen
// instructions at most: the plugin's CPU meter tells the host when its figures change, looking every
// 0.5 s of audio. Two runs that play different numbers of blocks differ by those blocks: make
// arm-icount plays 256 and 768 and divides the difference by 512. The environment and the arguments
// move both runs' counts alike, by hundreds of instructions; make arm-icount gives them an empty
// environment and paths relative to the repository, so the totals match from one machine to the next
// too. --cases lists the cases, one a line, for it. An engine case's helper builds the tables its
// patch reads (no builder thread) and Weather's source, makes the engine and sets its patch; the .so
// isn't loaded.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../dsp/engine.h"
#include "../plugin/patch_map.h"
#include "../plugin/vst2.h"
#include "param_ids.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <memory>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kBlock = 128;
constexpr double kRate = 44100.0;
constexpr double kBudgetUs = kBlock * 1e6 / kRate;   // 2902 us
constexpr int kWarmBlocks = 689;                     // 2 s: a case plays this long before it is timed
constexpr int kRestrikeBlocks = 689;                 // worst: another six keys every 2 s
constexpr int kPhraseBlocks = 86;                    // echo resume: a phrase of 0.25 s every 2 s
constexpr uint32_t kFixedSeed = 0x9E3779B9u;         // AF_FIXED_SEED's (plugin/plugin.cpp), for the engine cases
constexpr double kTablesWaitS = 300.0;               // the builder's limit (it takes seconds)
constexpr double kTablesFallbackS = 30.0;            // without /proc: this long, and hope

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

// The process's threads but the instances' loaders (named af-loader: plugin/loader.h, alive as long as
// their instance, idle once its source is in); -1 if /proc can't say.
int threads() {
    DIR* d = opendir("/proc/self/task");
    if (!d) return -1;
    int n = 0;
    while (const dirent* t = readdir(d)) {
        if (t->d_name[0] == '.') continue;
        char path[sizeof "/proc/self/task//comm" + sizeof t->d_name], name[32] = {};
        std::snprintf(path, sizeof path, "/proc/self/task/%s/comm", t->d_name);
        if (FILE* f = std::fopen(path, "r")) {
            if (!std::fgets(name, sizeof name, f)) name[0] = 0;
            std::fclose(f);
        }
        if (std::strncmp(name, "af-loader", 9) != 0) ++n;
    }
    closedir(d);
    return n;
}

// Weather's source in (the Source stepper's text no longer "...": in, Memory or MISSING), as the tools
// wait for it (tools/phrase.h): before it is, the instance's loader thread is at work, and its publish
// would land somewhere in the blocks (a display update: not the same count on every run). Not in
// after kTablesWaitS fails the bench (g_sourceLate; the case still runs, for what it is worth).
bool g_sourceLate = false;
void waitForSource(AEffect* e) {
    const double t0 = wallS();
    for (;;) {
        char b[256] = {};
        e->dispatcher(e, vst::effGetParamDisplay, af::P_W_SOURCE, 0, b, 0.0f);
        const size_t n = std::strlen(b);
        if (n < 4 || std::strcmp(b + n - 4, " ...") != 0) return;
        if (wallS() - t0 > kTablesWaitS) {
            std::printf("  FAIL: Weather's source wasn't in after %.0f s: its publish lands in the blocks\n", kTablesWaitS);
            g_sourceLate = true;
            return;
        }
        const timespec nap{0, 5 * 1000 * 1000};
        nanosleep(&nap, nullptr);
    }
}

// A whole number, all of s and in an int's range: false for "", "10abc" or "1e3".
bool number(const char* s, int& v) {
    char* end = nullptr;
    errno = 0;
    const long long n = std::strtoll(s, &end, 10);
    if (end == s || *end || errno || n < INT_MIN || n > INT_MAX) return false;
    v = static_cast<int>(n);
    return true;
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
// published), saying how long that took. False when no builder thread appeared (the instance starts
// it before VSTPluginMain returns) or it was still running after kTablesWaitS. For --icount
// (`counting`) it says nothing when all is well, and /proc that can't be read fails too: a table
// published while the blocks play would change what they cost from run to run.
bool waitForTables(void* lib, bool counting) {
    const int before = threads();
    const double t0 = wallS();
    AEffect* e = openPlugin(lib);
    const auto napUntil = [t0](double s) {
        const timespec nap{0, 20 * 1000 * 1000};
        while (wallS() - t0 < s) nanosleep(&nap, nullptr);
    };
    bool ok = true;
    if (before <= 0 && counting) {
        std::printf("  FAIL: /proc/self/task can't be read, so the table builder can't be waited for\n");
        ok = false;
    } else if (before <= 0) {
        std::printf("  WARNING: /proc/self/task can't be read: waiting %.0f s for the tables instead\n", kTablesFallbackS);
        napUntil(kTablesFallbackS);
    } else if (threads() <= before) {
        std::printf("  FAIL: no table builder thread appeared: the cases play the sine fallback\n");
        ok = false;
    } else {
        const timespec nap{0, 20 * 1000 * 1000};
        while (threads() > before && wallS() - t0 < kTablesWaitS) nanosleep(&nap, nullptr);
        ok = threads() <= before;
        if (ok && !counting) std::printf("  tables built in %.1f s (the builder thread gone before the first case)\n", wallS() - t0);
        else if (!ok) std::printf("  FAIL: the table builder was still running after %.0f s: the cases time it too\n", kTablesWaitS);
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);   // the tables stay until the .so is unloaded
    return ok;
}

enum Case : int { C_IDLE, C_INIT, C_DRONE, C_BLOOM, C_WORST, C_AIR, C_WEATHER, C_WORST_ENGINE, C_WORST_M2,
                  C_AIR_STRIKE, C_ECHO_RESUME, C_COUNT };
constexpr const char* kCaseNames[C_COUNT] = {"idle", "init chord", "drone", "bloom 6x2", "worst", "air", "weather",
                                             "worst engine", "worst m2", "air strike", "echo resume"};
bool engineCase(int c) { return c >= C_AIR; }
bool worstLike(int c) { return c == C_WORST || c == C_WORST_ENGINE || c == C_WORST_M2; }

// Two six-key chords for Chord Off (a voice a key): white keys, so Snap keeps every one apart, and
// no key in both, so a re-strike takes every voice from its release.
constexpr uint8_t kSix[2][6] = {{48, 52, 55, 59, 62, 67}, {50, 53, 57, 60, 64, 69}};
constexpr int kAirSplit = 72;
constexpr uint8_t kAirKeys[6] = {72, 74, 76, 77, 79, 81};   // over Split: Air's alone
constexpr uint8_t kLowKeys[6] = {24, 26, 28, 29, 31, 33};   // a low chord from C1, white keys

// A case's parameters by index, in real values, through `set(id, value)`: the plugin's (setParameter)
// or, for an engine case, an array of them the patch map turns into the engine's Patch.
template <class Set>
void setUpParams(Set&& set, int c) {
    using namespace af;
    const bool worst = worstLike(c);
    if (c == C_DRONE || worst) {   // Ground at full
        for (int id : {P_G_SUB, P_G_ROOT, P_G_FIFTH, P_G_OCT, P_G_COLOR}) set(id, 1.0f);
        set(P_G_BODY, 0.5f);
        set(P_G_BREATH, 0.6f);
    }
    if (c == C_DRONE) set(P_B_MUTE, 1);
    if (c == C_BLOOM || c == C_ECHO_RESUME) set(P_G_MUTE, 1);
    if (c == C_AIR || c == C_WEATHER || c == C_AIR_STRIKE) {   // M2's strata alone
        set(P_G_MUTE, 1);
        set(P_B_MUTE, 1);
    }
    if (c == C_BLOOM || worst) {   // six voices of unison 2, B frequency-modulating A
        set(P_H_CHORD, CH_OFF);
        set(P_B_UNISON, 1);            // the option "2"
        set(P_B_COUPLE, CP_FM);
        set(P_B_CAMT, 0.5f);
        set(P_B_BLEND, 0.5f);
    }
    if (worst) {   // the scan moving fast and jittering: every read crosses frames
        for (int id : {P_G_SWAY, P_B_SWAY, P_B_SMEAR, P_B_BREATH}) set(id, 1.0f);
        set(P_G_SWAYRATE, 2.0f);
        set(P_B_SWAYRATE, 2.0f);
        set(P_S_MODE, Reverb::ABYSS);
        set(P_S_SHIMMER, 1.0f);
        set(P_S_FREEZE, 0);
        set(P_O_TILT, 0.3f);
    }
    if (c == C_ECHO_RESUME) set(P_B_RELEASE, 0.3f);
}

void setUp(AEffect* e, int c) { setUpParams([e](int id, float v) { set(e, id, v); }, c); }

// An engine case's Patch: the plugin's for its parameters (the patch map's, as an instance makes it),
// then M2's fields, which the plugin doesn't map yet (M2's Task 9).
af::Patch enginePatch(int c) {
    using namespace af;
    float norm[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) norm[i] = PARAM_INFO[i].def;
    setUpParams([&norm](int id, float v) { norm[id] = paramNorm(id, v); }, c);
    Patch p = patchFromParams(norm);
    if (c == C_AIR || c == C_WORST_M2) {   // Air at its dearest: Felt ringing, six voices, Density 60, Loop
        p.air.level = 0.5f * 0.5f;
        p.air.listen = c == C_AIR ? LI_FREE : LI_HARMONY;
        p.air.voice.sound = AS_FELT;
        p.air.voice.decayS = 20.0f;
        p.air.gen.density = 60.0f;
        p.air.gen.loop = true;
        p.split = kAirSplit;
    }
    if (c == C_WEATHER || c == C_WORST_M2) {   // Weather at its dearest
        p.weather.level = 0.5f * 0.5f;
        p.weather.listen = c == C_WEATHER ? LI_FREE : LI_HARMONY;
        p.weather.grains = Weather::kGrains;
        // The plan's case (weather) is Cloud; M2's worst takes Stretch, the dearer (1.2k here).
        p.weather.mode = c == C_WORST_M2 ? WM_STRETCH : WM_CLOUD;
        p.weather.pitch = 12.0f;
        p.weather.toKey = TK_CHORD;
        p.weather.hpHz = 200.0f;
        p.weather.tilt = 0.3f;
        p.weather.duck = 0.5f;
    }
    if (c == C_WORST_M2 || c == C_ECHO_RESUME) {   // Echo at its dearest
        Delay::Params& d = p.echo.delay;
        d.mode = Delay::PING_PONG;
        d.wow = d.diffuse = d.duck = 1.0f;
        d.feedback = 0.9f;
        p.echoReturn = 0.7f * 0.7f;
        p.echoSpace = 0.3f * 0.3f;
        p.groundEcho = p.bloomEcho = p.airEcho = p.weatherEcho = 0.5f * 0.5f;
    }
    if (c == C_WORST_M2) {   // panned: Air and Weather into buffers of their own; Memory records
        p.airPan = 0.3f;
        p.weatherPan = -0.3f;
        p.memoryTap = MT_OUTPUT;
    }
    if (c == C_ECHO_RESUME) {   // short, so the repeats fall silent between the phrases
        p.echo.delay.sync = false;
        p.echo.delay.timeMs = 100.0f;
        p.echo.delay.feedback = 0.3f;
    }
    if (c == C_AIR_STRIKE) {   // Air on the notes, Kalimba
        p.air.level = 0.5f * 0.5f;
        p.air.listen = LI_NOTES;
        p.air.voice.sound = AS_KALIMBA;
    }
    return p;
}

// The case's keys going down (or up), at a block's first sample.
template <class Key>
void keys(Key&& key, int c, int chord, bool down) {
    switch (c) {
        case C_INIT:
        case C_DRONE:
        case C_WEATHER:
        case C_ECHO_RESUME: key(60, down); break;
        case C_BLOOM:
        case C_WORST:
        case C_WORST_ENGINE:
            for (uint8_t k : kSix[chord]) key(k, down);
            break;
        case C_WORST_M2:
            for (uint8_t k : kSix[chord]) key(k, down);
            for (uint8_t k : kAirKeys) key(k, down);
            break;
        case C_AIR:
            for (uint8_t k : kAirKeys) key(k, down);
            break;
        case C_AIR_STRIKE:
            for (uint8_t k : kLowKeys) key(k, down);
            break;
        default: break;
    }
}

// Case c's instance, set up through its parameters: the host's side of a case.
AEffect* openCase(void* lib, int c) {
    AEffect* e = openPlugin(lib);
    setUp(e, c);
    waitForSource(e);
    return e;
}

// An engine case's engine, with what it reads: the tables its patch names (built here, every other
// slot the sine) and Weather's source (12 s of noise, made a source as the loader makes one).
struct EngineCase {
    af::TableSet tables;
    std::unique_ptr<af::Wavetable> built[af::TB_COUNT];
    std::unique_ptr<af::SourceBuffer> source;
    std::unique_ptr<af::Engine> engine;
};

std::unique_ptr<EngineCase> openEngineCase(int c) {
    auto x = std::make_unique<EngineCase>();
    const af::Patch p = enginePatch(c);
    af::sineTable();   // the fallback, built before any render
    for (int id : {p.ground.table, p.bloom.table, p.bloom.tableB, static_cast<int>(af::TB_FELT_PIANO)}) {
        if (id < 0 || id >= af::TB_COUNT || x->built[id]) continue;
        x->built[id] = std::make_unique<af::Wavetable>();
        if (af::buildTable(id, *x->built[id])) x->tables.t[id].store(x->built[id].get());
    }
    const int frames = 12 * static_cast<int>(kRate);
    std::vector<float> L(static_cast<size_t>(frames)), R(L.size());
    uint32_t r = 12345u;
    for (size_t i = 0; i < L.size(); ++i) {
        L[i] = af::randBipolar(r);
        R[i] = af::randBipolar(r);
    }
    x->source = af::buildSource(L.data(), R.data(), frames);
    x->engine = std::make_unique<af::Engine>(x->tables);
    x->engine->seed(kFixedSeed);
    x->engine->setPatch(p);
    x->engine->setWeatherSource(x->source ? &x->source->src : nullptr);
    return x;
}

// Plays case c: its keys, the 2 s (kWarmBlocks) and then `blocks` more, the worst cases letting their
// six keys go for the other six before every kRestrikeBlocks-th block (and the low chord struck again
// there), echo resume's phrase starting there and ending kPhraseBlocks later. Each block is
// block(b, render), b counted from -kWarmBlocks, which calls render() once: the timed bench times it,
// --icount only renders. key(note, down) is the host's: MIDI into the plugin, or straight into the
// engine. The one way to play a case, so the counts and the timings are always of the same thing.
template <class Key, class Remember, class Render, class Block>
void playCase(int c, int blocks, Key&& key, Remember&& remember, Render&& render, Block&& block) {
    if (c != C_IDLE) keys(key, c, 0, true);
    int chord = 0;
    const bool restrikes = worstLike(c) || c == C_AIR_STRIKE;
    for (int b = -kWarmBlocks; b < blocks; ++b) {
        const int at = b + kWarmBlocks;
        if (restrikes && at > 0 && at % kRestrikeBlocks == 0) {
            keys(key, c, chord, false);
            chord ^= 1;
            keys(key, c, chord, true);
            if (c == C_WORST_M2) remember();   // taken every second time (the header says why)
        }
        if (c == C_ECHO_RESUME && at > 0 && at % kRestrikeBlocks == 0) keys(key, c, 0, true);
        if (c == C_ECHO_RESUME && at % kRestrikeBlocks == kPhraseBlocks) keys(key, c, 0, false);
        block(b, render);
    }
}

// The plugin's way: MIDI at offset 0 before the block, processReplacing.
template <class Block>
void playPlugin(AEffect* e, int c, int blocks, Block&& block) {
    std::vector<float> L(kBlock), R(kBlock);
    float* out[2] = {L.data(), R.data()};
    playCase(
        c, blocks, [e](uint8_t k, bool down) { midi(e, down ? 0x90 : 0x80, k, down ? 100 : 0); }, [] {},
        [e, &out] { e->processReplacing(e, nullptr, out, kBlock); }, block);
}

// The engine's way: the keys at the block's first sample, the bench's host's transport once a block.
template <class Block>
void playEngine(af::Engine& e, int c, int blocks, Block&& block) {
    std::vector<float> L(kBlock), R(kBlock);
    playCase(
        c, blocks, [&e](uint8_t k, bool down) { down ? e.noteOn(k, 100) : e.noteOff(k); }, [&e] { e.remember(); },
        [&e, &L, &R] {
            e.setTransport(120.0, 0.0, false, true);
            e.render(L.data(), R.data(), kBlock);
        },
        block);
}

struct Result { double avg, p99, max; };

using StageFn = int (*)(double*, const char**, int);

// The stage table holds the plugin's stages (dsp/stages.h); an engine case reports none.
Result runCase(void* lib, int seconds, int c, StageFn stages) {
    const int blocks = seconds * static_cast<int>(kRate) / kBlock;
    std::vector<double> t(static_cast<size_t>(blocks));
    const auto timed = [&](int b, const auto& render) {
        if (b == 0 && stages) {   // the stage counters start over here: only the timed blocks count
            double us[8];
            const char* names[8];
            stages(us, names, 8);
        }
        const double t0 = cpuUs();
        render();
        if (b >= 0) t[static_cast<size_t>(b)] = cpuUs() - t0;
    };
    if (engineCase(c)) {
        const std::unique_ptr<EngineCase> x = openEngineCase(c);
        playEngine(*x->engine, c, blocks, timed);
    } else {
        AEffect* e = openCase(lib, c);
        playPlugin(e, c, blocks, timed);
        e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    }
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

// --icount: case c played for its 2 s and then `blocks` blocks on the main thread, and nothing else
// there (the header says why). A helper thread, standing in for the host's own threads, loads the
// plugin, waits for the tables and opens the case (Weather's source in); main joins it and plays. No
// thread but main may be alive while the blocks play (but the instance's idle loader, left out of the
// counts), since only main's count is read: the
// helper looks once the builder has gone, main once the blocks are over (sooner would race the
// helper's own exit, and waiting it out would make main's count depend on the machine again). 0
// when all went well.
int icount(const char* so, int c, int blocks) {
    void* lib = nullptr;
    AEffect* e = nullptr;
    std::unique_ptr<EngineCase> x;   // an engine case's
    int alone = -1;   // the process's threads with main and the helper alone (qemu's own count too)
    bool ok = false;
    const char* const others = "  FAIL: a thread besides main is alive while the blocks play: what it does goes uncounted\n";
    std::thread host([&] {
        alone = threads();
        if (engineCase(c)) {
            x = openEngineCase(c);
            ok = alone > 0;
        } else {
            lib = dlopen(so, RTLD_NOW | RTLD_LOCAL);
            if (!lib) {
                std::fprintf(stderr, "dlopen: %s\n", dlerror());
                return;
            }
            ok = waitForTables(lib, true);
            e = openCase(lib, c);
            ok = ok && !g_sourceLate;
        }
        if (ok && threads() != alone) {
            std::printf("%s", others);
            ok = false;
        }
    });
    host.join();
    if (!ok) return 1;   // the count would be over the sine fallback, or miss a thread's work

    const auto play = [](int, const auto& render) { render(); };
    if (x) playEngine(*x->engine, c, blocks, play);
    else playPlugin(e, c, blocks, play);
    if (threads() != alone - 1) {   // the helper gone, and nothing else
        std::printf("%s", others);
        return 1;
    }
    if (e) e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    if (lib) dlclose(lib);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--cases")) {
        for (const char* name : kCaseNames) std::printf("%s\n", name);
        return 0;
    }
    const auto usage = [argv] {
        std::fprintf(stderr, "usage: %s <plugin.so> [-s seconds] [-c cpu]\n"
                             "       %s <plugin.so> --icount <case> <blocks>   (blocks >= 1)\n"
                             "       %s --cases                                (the cases' names)\n", argv[0], argv[0], argv[0]);
        return 2;
    };
    if (argc < 2) return usage();
    // Anything else fails rather than being let go: a mistyped --icount would run the timed bench.
    int counted = -1, countBlocks = 0;   // --icount: the case and how many blocks after its 2 s
    int seconds = 3, cpu = 1;
    if (argc > 2 && !std::strcmp(argv[2], "--icount")) {
        for (int c = 0; argc == 5 && c < C_COUNT; ++c)
            if (!std::strcmp(argv[3], kCaseNames[c])) counted = c;
        if (counted < 0 || !number(argv[4], countBlocks) || countBlocks < 1) return usage();
    } else {
        for (int i = 2; i < argc; i += 2) {
            int v = 0;
            if (i + 1 == argc || !number(argv[i + 1], v)) return usage();
            if (!std::strcmp(argv[i], "-s")) seconds = std::max(1, v);
            else if (!std::strcmp(argv[i], "-c")) cpu = v;
            else return usage();
        }
    }
    if (cpu >= 0 && counted < 0) {   // -c -1: don't pin (x86 runs, CI)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0) std::printf("(could not pin to cpu %d)\n", cpu);
    }
    // Hermetic: no user folders (presets, Weather's WAVs), nothing saved, no trace (its flag file
    // looked for where there is none), the same random numbers every run.
    setenv("AF_PRESET_ROOTS", "/nonexistent-afbench", 1);
    setenv("AF_SOURCE_ROOTS", "/nonexistent-afbench:/nonexistent-afbench", 1);
    setenv("AF_DATA_DIR", "", 1);
    setenv("AF_TRACE_DIR", "/nonexistent-afbench", 1);
    setenv("AF_FIXED_SEED", "1", 1);
    g_time.sampleRate = kRate;
    g_time.tempo = 120.0;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;
    if (counted >= 0) return icount(argv[1], counted, countBlocks);

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto stages = reinterpret_cast<StageFn>(dlsym(lib, "AmbientForceStageTimes"));
    std::printf("%s, %d s per case, %s\n", argv[1], seconds, stages ? "profiling build" : "plain build");
    bool fail = !waitForTables(lib, false);
    for (int c = 0; c < C_COUNT; ++c) {
        double us[8] = {};
        const char* stageNames[8] = {};
        const Result r = runCase(lib, seconds, c, engineCase(c) ? nullptr : stages);
        fail = fail || !(r.p99 <= 35.0 && r.max <= 80.0);
        if (stages && !engineCase(c)) {
            const int n = stages(us, stageNames, 8);
            const int blocks = seconds * static_cast<int>(kRate) / kBlock;
            std::printf("      per block:");
            for (int k = 0; k < n; ++k) std::printf("  %s %.1f us", stageNames[k], us[k] / blocks);
            std::printf("\n");
        }
    }
    dlclose(lib);
    return fail || g_sourceLate ? 1 : 0;
}
