// From SubForce test/plugin_test.cpp (8846421), sft -> aft; the synth's own checks left out.
// The test suite: the engine on its own (engine_test.cpp), then the whole plugin driven through
// its VST2 entry points the way MPC drives it (128-frame blocks, events with deltaFrames, 0..1
// params). Built with ASan/UBSan by `make test`, for the Force's CPU under qemu by `make test-arm`.
#include "host.h"
#include "../plugin/loader.h"
#include "../plugin/presets.h"
#include "../plugin/sources.h"
#include "../plugin/surface.h"
#include "../plugin/trace.h"
#include "../plugin/wav.h"
#include "../dsp/fields.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Counting locks (processReplacing takes none: testAudioThreadQuiet). pthread_mutex_lock and trylock, which
// std::mutex and every lock of the C++ library come to, are this binary's own and count the locks taken on
// the thread that counts while it counts, then hand on to the real ones (dlsym's next: the sanitizer's, then
// libc's). Under ASan only, where the allocation counter is too.
#if AFT_COUNTS_ALLOCS
#include <dlfcn.h>
#include <pthread.h>
namespace aft {
thread_local bool t_countingLocks = false;
int g_locks = 0;
}
extern "C" int pthread_mutex_lock(pthread_mutex_t* m) noexcept {
    using Fn = int (*)(pthread_mutex_t*);
    static Fn real = nullptr;   // (no guard: dlsym may lock on its first call)
    if (!real) real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pthread_mutex_lock"));
    if (aft::t_countingLocks) ++aft::g_locks;
    return real(m);
}
extern "C" int pthread_mutex_trylock(pthread_mutex_t* m) noexcept {
    using Fn = int (*)(pthread_mutex_t*);
    static Fn real = nullptr;
    if (!real) real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "pthread_mutex_trylock"));
    if (aft::t_countingLocks) ++aft::g_locks;
    return real(m);
}
#endif

namespace aft {
int g_fail = 0, g_pass = 0;

intptr_t hostMaster(AEffect* e, int32_t op, int32_t index, intptr_t, void*, float opt) {
    HostLog* log = e ? static_cast<HostLog*>(e->user) : nullptr;
    if (op == 1) return 2400;   // audioMasterVersion
    if (!log) return 0;
    if (op == vst::audioMasterUpdateDisplay) ++log->updates;
    if (op == vst::audioMasterAutomate && log->record) {
        log->automated[index] = opt;
        ++log->automateCount[index];
    }
    if (op == vst::audioMasterGetTime) {
        if (log->throwOnTime) throw std::runtime_error("the host's transport");
        return reinterpret_cast<intptr_t>(&log->time);
    }
    return 0;
}

std::string fixtureDir() {
    static const std::string dir = [] {
        char tmpl[] = "/tmp/aftest.XXXXXX";
        const char* d = mkdtemp(tmpl);
        return std::string(d ? d : "/tmp/aftest");
    }();
    return dir;
}

std::string sourceDir() { return fixtureDir() + "/sources"; }

double pitchHz(const std::vector<float>& x, size_t from, size_t to) {
    if (to == 0 || to > x.size()) to = x.size();
    double first = -1.0, last = 0.0;
    int n = 0;
    for (size_t i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
            if (first < 0.0) first = t;
            else {
                last = t;
                ++n;
            }
        }
    return n > 0 ? 44100.0 * n / (last - first) : 0.0;
}

// SubForce's test/engine_test.cpp toneAmp (8846421), shared here.
double toneAmp(const std::vector<float>& x, double hz) {
    double re = 0.0, im = 0.0, wsum = 0.0;
    const double n = static_cast<double>(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) / n);
        const double ph = 2.0 * M_PI * hz * static_cast<double>(i) / 44100.0;
        re += w * x[i] * std::cos(ph);
        im += w * x[i] * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

} // namespace aft

namespace {
using namespace aft;

void testBasics() {
    std::printf("== plugin basics\n");
    Host h;
    CHECK(h.e && h.e->magic == vst::kMagic);
    CHECK(h.e->numParams == af::P_COUNT && h.e->numOutputs == 2 && h.e->numInputs == 0);
    CHECK((h.e->flags & vst::effFlagsIsSynth) && (h.e->flags & vst::effFlagsProgramChunks));
    CHECK(h.e->uniqueID == vst::fourcc("AmFc"));
    char b[256] = {};
    h.e->dispatcher(h.e, vst::effGetEffectName, 0, 0, b, 0.0f);
    CHECK(std::string(b) == "AmbientForce");
    h.e->dispatcher(h.e, vst::effGetVendorString, 0, 0, b, 0.0f);
    CHECK(std::string(b) == "Devko");
    CHECK(h.e->dispatcher(h.e, vst::effCanDo, 0, 0, const_cast<char*>("receiveVstMidiEvent"), 0.0f) == 1);
    CHECK(h.e->dispatcher(h.e, vst::effCanDo, 0, 0, const_cast<char*>("sendVstMidiEvent"), 0.0f) == -1);
    CHECK(h.e->dispatcher(h.e, vst::effGetPlugCategory, 0, 0, nullptr, 0.0f) == vst::kPlugCategSynth);
    // Every parameter has a short, unique name; every sound parameter is automatable.
    bool namesOk = true, autoOk = true;
    std::map<std::string, int> seen;
    for (int i = 0; i < af::P_COUNT; ++i) {
        const std::string n = h.name(i);
        namesOk = namesOk && !n.empty() && n.size() <= 24 && seen.count(n) == 0;
        seen[n] = i;
        const bool automatable = h.e->dispatcher(h.e, vst::effCanBeAutomated, i, 0, nullptr, 0.0f) == 1;
        autoOk = autoOk && automatable == (af::PARAM_INFO[i].kind == af::Kind::Synth);
    }
    CHECK(namesOk && autoOk);
    CHECK(h.display(af::P_VOLUME) == "-6.0 dB");
    h.set(af::P_VOLUME, 0.0f);
    CHECK(h.display(af::P_VOLUME) == "0.0 dB");
    h.set(af::P_VOLUME, -60.0f);
    CHECK(h.display(af::P_VOLUME) == "-inf dB");
    // Out-of-range indices are harmless; NaN and infinities from the host land inside 0..1.
    h.e->setParameter(h.e, -1, 0.5f);
    h.e->setParameter(h.e, af::P_COUNT, 0.5f);
    CHECK(h.e->getParameter(h.e, af::P_COUNT) == 0.0f);
    h.e->setParameter(h.e, af::P_VOLUME, std::nanf(""));
    CHECK(h.get(af::P_VOLUME) == 0.0f);
    h.e->setParameter(h.e, af::P_VOLUME, INFINITY);
    CHECK(h.get(af::P_VOLUME) == 1.0f);
    h.e->setParameter(h.e, af::P_VOLUME, -INFINITY);
    CHECK(h.get(af::P_VOLUME) == 0.0f);
    h.e->setParameter(h.e, af::P_PRESET, std::nanf(""));   // a stepper, a tile, a button: nothing breaks
    h.e->setParameter(h.e, af::P_CAT_3, std::nanf(""));
    h.e->setParameter(h.e, af::P_PRE_SAVE, std::nanf(""));
    h.set(af::P_VOLUME, 0.0f);
    h.on(60);
    h.run(kBlocksPerSec);   // Bloom swells over 2.5 s, Ground fades in over 4
    CHECK(h.finite && rms(h.L) > 0.005);
    h.e->setParameter(h.e, af::P_STATUS, 0.7f);   // the status readout ignores writes
    CHECK(h.get(af::P_STATUS) == 0.0f);
}

// What MPC may ask about any index, in or out of range, at any time: a value in 0..1, a name, a
// text, never a crash (ASan watches the reads).
void testGetters() {
    std::printf("== every getter, every index\n");
    Host h;
    h.on(64);
    h.run(4);
    bool ok = true;
    for (int i = -3; i < af::P_COUNT + 3; ++i) {
        const float v = h.e->getParameter(h.e, i);
        ok = ok && std::isfinite(v) && v >= 0.0f && v <= 1.0f;
        char b[256];
        for (int op : {vst::effGetParamName, vst::effGetParamLabel, vst::effGetParamDisplay}) {
            std::memset(b, 'x', sizeof b);
            h.e->dispatcher(h.e, op, i, 0, b, 0.0f);
            ok = ok && std::memchr(b, 0, sizeof b) != nullptr;   // terminated inside what JUCE reads
        }
        h.e->dispatcher(h.e, vst::effCanBeAutomated, i, 0, nullptr, 0.0f);
        if (i < 0 || i >= af::P_COUNT) {
            h.e->dispatcher(h.e, vst::effGetParamDisplay, i, 0, b, 0.0f);
            ok = ok && b[0] == 0;
        }
    }
    CHECK(ok);
    // Null pointers where MPC passes buffers.
    for (int op : {vst::effGetParamName, vst::effGetParamLabel, vst::effGetParamDisplay, vst::effGetEffectName,
                   vst::effGetVendorString, vst::effGetProductString, vst::effGetProgramName, vst::effCanDo,
                   vst::effGetChunk, vst::effSetChunk, vst::effProcessEvents})
        h.e->dispatcher(h.e, op, 1, 0, nullptr, 0.0f);
    CHECK(h.run(2) > 0.0f && h.finite);
}

void testPlay() {
    std::printf("== notes in, sound out\n");
    Host h;
    CHECK(h.run(8) == 0.0f);   // nothing before a note
    h.on(48, 100, 64);         // starts at sample 64 of the next block
    h.run(1);
    float before = 0.0f, after = 0.0f;
    for (size_t i = 0; i < 64; ++i) before = std::max(before, std::fabs(h.L[i]));
    for (size_t i = 64; i < 128; ++i) after = std::max(after, std::fabs(h.L[i]));
    CHECK(before == 0.0f && after > 0.0f);
    const float peak = h.run(3 * kBlocksPerSec);   // past Bloom's swell (2.5 s)
    std::printf("  the default patch, one key: peak %.3f, RMS %.1f dBFS\n", peak, db(rms(h.L)));
    CHECK(h.finite && peak > 0.05f && peak <= 0.8913f);
    bool stereo = false;
    for (size_t i = 0; i < h.L.size(); ++i) stereo = stereo || h.L[i] != h.R[i];
    CHECK(stereo);   // Width spreads Ground's partials and Bloom's notes
    // The default: Bloom plays the key's triad, Ground the root under it.
    CHECK(h.voices() == 4);
    CHECK(h.display(af::P_STATUS).compare(0, 9, "VOICES 4 ") == 0);
    h.off(48);   // Bloom hands its tails to Space (1.5 s); Ground stays on the harmony (Forever)
    h.run(2 * kBlocksPerSec);
    CHECK(h.voices() == 1 && h.run(4) > 0.0f);
    // All sound off (CC 120) is immediate, and forgets the harmony.
    h.midi(0xB0, 120, 0);
    CHECK(h.run(1) == 0.0f);
    CHECK(h.voices() == 0 && h.run(kBlocksPerSec) == 0.0f);
    // All notes off (CC 123) releases the keys; what the harmony holds goes on sounding.
    h.on(57);
    h.run(kBlocksPerSec);
    h.midi(0xB0, 123, 0);
    CHECK(h.run(1) > 0.0f);
    h.run(2 * kBlocksPerSec);
    CHECK(h.voices() == 1 && h.run(4) > 0.0f);
    CHECK(h.finite);
}

// Stop: MPC suspending and resuming processing, and its transport stopping. On Stop is Fade (8 s).
void testStop() {
    std::printf("== Stop: suspend, resume, the transport\n");
    // A suspend resumed within 250 ms is a Stop: the landscape fades.
    Host p;
    p.on(57);
    p.run(kBlocksPerSec);
    {
        Turn t(100);   // the suspend and the resume 100 ms apart
        p.e->dispatcher(p.e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);
        p.e->dispatcher(p.e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);
    }
    CHECK(p.run(1) > 0.0f);
    p.run(8 * kBlocksPerSec + kBlocksPerSec / 4);
    CHECK(p.run(4) == 0.0f && p.voices() == 0);
    // A longer suspend resets: here a second (the tests' clock moves a second per host event), and
    // the host never says it resumes, so the next block is when it did.
    p.on(57);
    p.run(kBlocksPerSec);
    p.e->dispatcher(p.e, vst::effStopProcess, 0, 0, nullptr, 0.0f);
    CHECK(p.run(1) == 0.0f && p.voices() == 0);
    p.on(57);
    p.run(10);
    p.e->dispatcher(p.e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);
    p.e->dispatcher(p.e, vst::effStartProcess, 0, 0, nullptr, 0.0f);   // a second later
    CHECK(p.run(1) == 0.0f);
    // A resume without a suspend does nothing.
    p.on(57);
    p.run(10);
    p.e->dispatcher(p.e, vst::effStartProcess, 0, 0, nullptr, 0.0f);
    CHECK(p.run(1) > 0.0f);
    // Of two suspends before a resume the first counts, and of two resumes the first: suspended at
    // t0 and again at t0 + 200 ms, resumed at t0 + 300 ms and again at t0 + 400 ms is 300 ms away, a
    // reset (counted from the second suspend it would be 100 ms, a Stop).
    {
        Turn t(100);
        p.e->dispatcher(p.e, vst::effStopProcess, 0, 0, nullptr, 0.0f);       // t0
        {
            Turn later(200);
            p.e->dispatcher(p.e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);   // t0 + 200
        }
        p.e->dispatcher(p.e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);       // t0 + 300
        p.e->dispatcher(p.e, vst::effStartProcess, 0, 0, nullptr, 0.0f);      // t0 + 400
    }
    CHECK(p.run(1) == 0.0f && p.voices() == 0);   // reset
    // The same, the suspends 100 ms apart: 200 ms away, a Stop (fading, not reset).
    p.on(57);
    p.run(10);
    {
        Turn t(100);
        p.e->dispatcher(p.e, vst::effStopProcess, 0, 0, nullptr, 0.0f);       // t0
        p.e->dispatcher(p.e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);      // t0 + 100
        p.e->dispatcher(p.e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);      // t0 + 200
    }
    CHECK(p.run(1) > 0.0f);
    CHECK(p.finite);

    // The transport stopping is a Stop too.
    Host s;
    s.log.time.flags |= vst::kVstTransportPlaying;
    s.on(57);
    s.run(kBlocksPerSec);
    s.off(57);
    s.log.time.flags &= ~vst::kVstTransportPlaying;
    CHECK(s.run(1) > 0.0f);
    s.run(8 * kBlocksPerSec + kBlocksPerSec / 4);
    CHECK(s.run(4) == 0.0f && s.voices() == 0);
    // Playing again wakes nothing; a note does.
    s.log.time.flags |= vst::kVstTransportPlaying;
    CHECK(s.run(kBlocksPerSec) == 0.0f);
    s.on(60);
    CHECK(s.run(kBlocksPerSec) > 0.0f && s.finite);
}

// The trace (Phase 0: what MPC does around Stop, what MIDI it sends): the audio thread only leaves
// notes, and the lines are written at the host's next call, on its own thread.
void testTrace() {
    std::printf("== the trace: the resume and MIDI, written from the host's thread\n");
    if (af::tracing()) {   // on already (a flag file of the developer's): its log is elsewhere
        std::printf("  skipped: the trace is on already\n");
        return;
    }
    const std::string dir = fixtureDir() + "/trace";
    std::filesystem::create_directories(dir);
    setenv("AF_TRACE_DIR", dir.c_str(), 1);
    std::ofstream(dir + "/ambientforce.trace").put('\n');
    const auto wait = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1100)); };
    wait();   // the trace looks for its flag file once a second
    const auto log = [&dir] {
        std::ifstream f(dir + "/ambientforce.log");
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };
    Host h;
    h.on(60);
    h.run(10);
    {
        Turn t(100);
        h.e->dispatcher(h.e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);
        h.e->dispatcher(h.e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);
    }
    h.run(1);   // the audio thread takes the resume
    const std::string before = log();
    h.display(af::P_STATUS);   // a host call
    const std::string after = log();
    CHECK(before.find("suspend at") != std::string::npos && before.find("resumed") == std::string::npos);
    CHECK(after.find("resumed 100 ms after the suspend (the host said so): On Stop") != std::string::npos);
    CHECK(after.find("] resume at") != std::string::npos && after.find("[tid ") != std::string::npos);   // which thread
    // MIDI as it came in (channel 2 here), traced at the host's next call, not from the block.
    h.midi(0x91, 60, 100, 12);
    h.midi(0xB1, 64, 127);
    h.midi(0xE1, 0x00, 0x40);   // the bend's middle
    h.run(1);
    const std::string quiet = log();
    h.display(af::P_STATUS);
    const std::string traced = log();
    CHECK(quiet.find("midi ch 2") == std::string::npos);
    CHECK(traced.find("midi ch 2 note-on 60 vel 100 @12") != std::string::npos &&
          traced.find("midi ch 2 cc 64 127 @0") != std::string::npos &&
          traced.find("midi ch 2 pitch bend 0 @0") != std::string::npos);
    // More than the ring holds between two host calls: the rest dropped and counted.
    for (int i = 0; i < 300; ++i) h.off(60, i % kBlock);
    h.run(1);
    h.display(af::P_STATUS);
    CHECK(log().find("midi: 44 more events dropped") != std::string::npos);
    std::filesystem::remove(dir + "/ambientforce.trace");
    unsetenv("AF_TRACE_DIR");
    wait();   // off again for whatever comes next
}

// The status line (plugin/surface.h): for 4 s after a move the control's help line, for 6 s after a preset loads
// its name and description, else the voices and the CPU; MPC told to read it again within 4 blocks of a change.
// Timed in audio blocks: the tests' clock only moves with host events.
std::string upperOf(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void testStatusLine() {
    std::printf("== the status line: help lines and preset descriptions\n");
    const auto meter = [](Host& h) { return h.display(af::P_STATUS).compare(0, 7, "VOICES ") == 0; };
    // `blocks` blocks: was MPC told to read the texts again?
    const auto told = [](Host& h, int blocks) {
        h.log.updates = 0;
        h.run(blocks);
        return h.log.updates > 0;
    };
    // Block by block until the status line reads otherwise (at most `max`): was MPC told then, or in the 4 blocks
    // after? (While a help line shows, its text is the only thing on the line that changes.)
    const auto toldWhenItChanges = [](Host& h, int max) {
        const std::string was = h.display(af::P_STATUS);
        for (int b = 0; b < max; ++b) {
            h.log.updates = 0;
            h.run(1);
            if (h.display(af::P_STATUS) == was) continue;
            for (int k = 0; k < 4 && !h.log.updates; ++k) h.run(1);
            return h.log.updates > 0;
        }
        return false;
    };
    const std::string bloomAge = af::PARAM_INFO[af::P_B_AGE].help, groundAge = af::PARAM_INFO[af::P_G_AGE].help;
    CHECK(bloomAge.compare(0, 11, "BLOOM AGE: ") == 0);
    Host h;
    h.run(8);
    CHECK(meter(h));
    // A move: its help line at once, and MPC told within 4 blocks.
    h.set(af::P_B_AGE, 0.3f);
    CHECK(told(h, 4) && h.display(af::P_STATUS) == bloomAge);
    // 4 s from the move, then the meter again, MPC told again.
    h.run(static_cast<int>(3.9 * kBlocksPerSec));
    CHECK(h.display(af::P_STATUS) == bloomAge);
    CHECK(toldWhenItChanges(h, kBlocksPerSec / 5) && meter(h));
    // MPC sending a value back moves nothing: no help. A stepped list likewise, until a step steps.
    h.setN(af::P_B_AGE, h.get(af::P_B_AGE));
    h.setN(af::P_H_SCALE, h.get(af::P_H_SCALE));
    h.run(8);
    CHECK(meter(h));
    h.detent(af::P_H_SCALE, +1);
    h.run(4);
    CHECK(h.display(af::P_STATUS) == af::PARAM_INFO[af::P_H_SCALE].help && h.value(af::P_H_SCALE) == 1.0f);
    // The same control moved again and again (a slow turn, automation) keeps its line, 4 s from its last move.
    bool kept = true;
    for (int k = 0; k < 6; ++k) {
        h.set(af::P_B_AGE, 0.1f + 0.1f * static_cast<float>(k));
        h.run(kBlocksPerSec);
        kept = kept && h.display(af::P_STATUS) == bloomAge;
    }
    h.run(5 * kBlocksPerSec / 2);   // 3.5 s from the last move
    CHECK(kept && h.display(af::P_STATUS) == bloomAge);
    h.run(kBlocksPerSec / 2 + 8);
    CHECK(meter(h));
    // A second control moved just after the first: shown once the first has had half a second.
    h.set(af::P_B_AGE, 0.5f);
    h.run(2);
    h.set(af::P_G_AGE, 0.6f);
    h.run(2);
    CHECK(h.display(af::P_STATUS) == bloomAge);
    h.run(kBlocksPerSec / 2);
    CHECK(h.display(af::P_STATUS) == groundAge);
    h.run(5 * kBlocksPerSec);
    CHECK(meter(h));
    // Two controls moving every block, in turns (automation): the line changes no more often than every half
    // second. (Moved in the same order every block, the second one's line simply stays.)
    int changes = 0;
    std::string was = h.display(af::P_STATUS);
    for (int b = 0; b < 3 * kBlocksPerSec; ++b) {
        const float v = b % 2 ? 0.3f : 0.4f;
        h.setN(b % 2 ? af::P_B_AGE : af::P_G_AGE, v);
        h.setN(b % 2 ? af::P_G_AGE : af::P_B_AGE, v);
        h.run(1);
        const std::string now = h.display(af::P_STATUS);
        changes += now != was;
        was = now;
    }
    std::printf("  two controls automated for 3 s: the line changed %d times\n", changes);
    CHECK(changes >= 4 && changes <= 7);   // the first move at once, then every 0.5 s at most
    h.run(5 * kBlocksPerSec);
    CHECK(meter(h));
    // A value echoed back rounded to MPC's 1/1000 (at most 0.0005 off) is no move; MPC's smallest move, 0.001, is one.
    h.setN(af::P_B_AGE, h.get(af::P_B_AGE) + 0.0005f);
    h.setN(af::P_B_AGE, h.get(af::P_B_AGE) - 0.0005f);
    h.run(8);
    CHECK(meter(h));
    h.setN(af::P_B_AGE, h.get(af::P_B_AGE) + 0.001f);
    h.run(4);
    CHECK(h.display(af::P_STATUS) == bloomAge);
    h.run(5 * kBlocksPerSec);
    CHECK(meter(h));

    // A preset loaded: "NAME: its description" for 6 s, over any help, never the help of the values it set.
    std::string init;
    CHECK(af::presetText("builtin:Init", init) && !af::presetAbout(init).empty());
    h.set(af::P_G_AGE, 0.2f);
    h.run(2);
    h.press(af::P_PRE_INIT);
    CHECK(told(h, 4) && h.display(af::P_STATUS) == "INIT: " + af::presetAbout(init));
    // MPC sending back a value the preset set, rounded to its 1/1000 (Init's volume, 0.81818 as 0.818), is no move:
    // the description stays.
    const float volume = h.get(af::P_VOLUME), echo = std::round(volume * 1000.0f) / 1000.0f;
    h.setN(af::P_VOLUME, echo);
    h.run(kBlocksPerSec * 3 / 5);
    CHECK(echo != volume && h.display(af::P_STATUS) == "INIT: " + af::presetAbout(init));
    h.run(static_cast<int>(5.3 * kBlocksPerSec));
    CHECK(h.display(af::P_STATUS) == "INIT: " + af::presetAbout(init));
    CHECK(toldWhenItChanges(h, kBlocksPerSec / 5) && meter(h));
    // NEXT loads the next preset and says which; PREV at the first preset loads nothing and shows its help.
    const auto L = af::presetLibrary().listing();
    CHECK(L->find("builtin:Init") == 0 && L->items.size() > 1);
    std::string next;
    CHECK(af::presetText(L->items[1].key, next));
    h.press(af::P_PRESET_NEXT);
    h.run(4);
    CHECK(h.display(af::P_STATUS) == upperOf(L->items[1].name) + ": " + af::presetAbout(next));
    h.press(af::P_PRESET_PREV);
    h.run(4);
    CHECK(h.display(af::P_STATUS) == "INIT: " + af::presetAbout(init));
    h.run(kBlocksPerSec);
    h.press(af::P_PRESET_PREV);   // at the first preset: nothing to load
    h.run(kBlocksPerSec / 2 + 4);
    CHECK(h.display(af::P_STATUS) == af::PARAM_INFO[af::P_PRESET_PREV].help);
    // A project's state isn't a preset load: no description, no help.
    h.run(5 * kBlocksPerSec);
    CHECK(h.load(h.chunk()) == 1);
    h.run(8);
    CHECK(meter(h) && h.finite);
}

void testProcessLegacy() {
    std::printf("== process() (accumulating)\n");
    Host a, b;
    a.on(45, 110, 300);   // 1024-frame calls (split into 512-frame sub-blocks inside)
    constexpr int kCalls = 16;
    std::vector<float> L(1024 * kCalls, 0.25f), R(1024 * kCalls, 0.25f);   // process() adds to what is there
    for (int c = 0; c < kCalls; ++c) {
        float* out[2] = {L.data() + c * 1024, R.data() + c * 1024};
        a.e->process(a.e, nullptr, out, 1024);
    }
    // The same note at the same sample through processReplacing in 128-frame blocks: sample 300 is
    // in the third block, 44 frames in.
    std::vector<float> L2(1024 * kCalls), R2(1024 * kCalls);
    for (int k = 0; k < 8 * kCalls; ++k) {
        if (k == 2) b.on(45, 110, 300 - 256);
        float* o[2] = {L2.data() + k * 128, R2.data() + k * 128};
        b.e->processReplacing(b.e, nullptr, o, 128);
    }
    double before = 0.0, diff = 0.0, energy = 0.0;
    for (int i = 0; i < 300; ++i) before = std::max(before, std::fabs(static_cast<double>(L[i]) - 0.25));
    for (size_t i = 0; i < L.size(); ++i) {
        diff = std::max(diff, std::fabs(static_cast<double>(L[i]) - 0.25 - L2[i]));
        diff = std::max(diff, std::fabs(static_cast<double>(R[i]) - 0.25 - R2[i]));
        energy += std::fabs(L2[i]);
    }
    CHECK(before == 0.0 && energy > 1.0);
    CHECK(diff < 1e-6);   // block sizes don't change the sound
}

// Two hosts against each other, sample for sample: the same seed, the same events.
double maxDiff(const Host& a, const Host& b) {
    double d = 0.0;
    for (size_t i = 0; i < a.L.size() && i < b.L.size(); ++i)
        d = std::max(d, std::max(std::fabs(static_cast<double>(a.L[i]) - b.L[i]), std::fabs(static_cast<double>(a.R[i]) - b.R[i])));
    return d;
}

void testMidiMapping() {
    std::printf("== MIDI: velocity, the pedal, CC 121\n");
    // Velocity 0 is a note-off: Bloom lets go, Ground stays on the harmony.
    Host v;
    v.on(57, 127);
    v.run(kBlocksPerSec);
    CHECK(v.voices() == 4);
    v.on(57, 0);
    v.run(2 * kBlocksPerSec);
    CHECK(v.voices() == 1);
    // Velocity scales Bloom (Vel 0.4): a soft key is quieter than a hard one, Ground the same.
    Host loud, soft;
    loud.on(57, 127);
    soft.on(57, 32);
    loud.run(3 * kBlocksPerSec);
    soft.run(3 * kBlocksPerSec);
    std::printf("  velocity 127 against 32: %+.1f dB\n", db(rms(loud.L) / rms(soft.L)));
    CHECK(rms(loud.L) > 1.2 * rms(soft.L));
    // CC 64 holds the chord after its key goes up, until the pedal comes up.
    Host h;
    h.on(57);
    h.midi(0xB0, 64, 127);
    h.off(57);
    h.run(2 * kBlocksPerSec);
    CHECK(h.voices() == 4);
    h.midi(0xB0, 64, 0);
    h.run(2 * kBlocksPerSec);
    CHECK(h.voices() == 1);
    // CC 121 (reset all controllers) puts the pedal back up: what it held is released.
    h.midi(0xB0, 64, 127);
    h.on(60);
    h.off(60);
    h.run(kBlocksPerSec);
    CHECK(h.voices() == 4);
    h.midi(0xB0, 121, 0);
    h.run(2 * kBlocksPerSec);
    CHECK(h.voices() == 1);
    // Bend, the mod wheel and pressure reach the engine and change nothing yet.
    Host x, y;
    x.on(69);
    y.on(69);
    x.midi(0xE0, 0x7F, 0x7F);
    x.midi(0xB0, 1, 127);
    x.midi(0xD0, 100, 0);
    x.midi(0xA0, 69, 100);
    x.run(kBlocksPerSec);
    y.run(kBlocksPerSec);
    CHECK(maxDiff(x, y) == 0.0 && rms(x.L) > 0.0);
}

// Ground's Breath synced to 1 Bar, through the plugin with MPC playing at 120 BPM (a bar is 2 s): at its top once a
// bar, on the downbeat, and on the bar again after the transport jumps; with MPC stopped, on at the tempo. Free
// breath and sway don't listen to the transport at all.
void testSync() {
    std::printf("== synced cycles: Breath on the bar\n");
    constexpr long kBar = 88200;   // samples: 4 beats at 120 BPM
    // The RMS of both channels over 100 ms around sample `at` of the last run, in dB.
    const auto level = [](const Host& h, long at) {
        double e = 0.0;
        int n = 0;
        for (long i = std::max(0L, at - 2205); i < std::min(static_cast<long>(h.L.size()), at + 2205); ++i, ++n)
            e += static_cast<double>(h.L[static_cast<size_t>(i)]) * h.L[static_cast<size_t>(i)] +
                 static_cast<double>(h.R[static_cast<size_t>(i)]) * h.R[static_cast<size_t>(i)];
        return n ? 10.0 * std::log10(e / (2.0 * n) + 1e-20) : -200.0;
    };
    // Where, within a bar, the breath tops over `bars` bars of the last run from sample `from`: the phase of the
    // level's swing at the bar's rate (a sine fitted to it every 10 ms), as samples after `from` (0..kBar).
    const auto top = [&level](const Host& h, long from, int bars) {
        double c = 0.0, s = 0.0;
        for (long i = 0; i < bars * kBar; i += 441) {
            const double l = level(h, from + i), w = 2.0 * M_PI * static_cast<double>(i) / kBar;
            c += l * std::cos(w);
            s += l * std::sin(w);
        }
        double t = std::atan2(s, c) / (2.0 * M_PI);
        if (t < 0.0) t += 1.0;
        return static_cast<long>(std::lround(t * kBar)) % kBar;
    };
    // How far apart two places in the bar are, either way round, in ms.
    const auto apart = [](long a, long b) {
        long d = ((a - b) % kBar + kBar) % kBar;
        if (d > kBar / 2) d = kBar - d;
        return static_cast<double>(d) / 44.1;
    };
    Host h;
    // The drone alone and steady (Bloom muted, no sway, no beating, no reverb), breathing fully, once a bar.
    h.set(af::P_B_MUTE, 1.0f);
    h.set(af::P_G_SWAY, 0.0f);
    h.set(af::P_G_BEAT, 0.0f);
    h.set(af::P_S_RETURN, 0.0f);
    h.set(af::P_G_FADE, 0.05f);
    h.set(af::P_G_BREATH, 1.0f);
    h.set(af::P_G_BREATHSYNC, 1.0f);
    h.set(af::P_G_BREATHDIV, 2.0f);   // 1 Bar
    h.set(af::P_H_ONSTOP, static_cast<float>(af::OS_KEEP));
    CHECK(h.display(af::P_G_BREATHDIV) == "1 Bar" && h.display(af::P_G_BREATHSYNC) == "Sync");
    h.log.time.flags |= vst::kVstTransportPlaying;
    h.on(48);
    h.run(kBlocksPerSec);   // in, at beat 2
    // Four bars from the downbeat of beat 4: the top on every downbeat, the bottom half a bar on, 4 dB under or more.
    const double startBeat = h.log.time.ppqPos;
    h.run(static_cast<int>(10.5 * 44100.0 / kBlock));
    const long downbeat = std::lround((4.0 - startBeat) * 0.5 * 44100.0);
    double depth = 99.0;
    for (int k = 0; k < 4; ++k)
        depth = std::min(depth, level(h, downbeat + k * kBar) - level(h, downbeat + k * kBar + kBar / 2));
    const double off = apart(top(h, downbeat, 4), 0);
    std::printf("  1 bar at 120 BPM: the top %.0f ms from the downbeat, %.1f dB over the bar's middle\n", off, depth);
    CHECK(off <= 30.0 && depth >= 4.0 && h.finite);
    // MPC jumps to beat 101, a beat past a downbeat: the tops land on beats 104, 108, ..., 1.5 s on and every 2 s.
    h.log.time.ppqPos = 101.0;
    h.run(static_cast<int>(8.5 * 44100.0 / kBlock));
    const double jumped = apart(top(h, kBar, 3), 3 * kBar / 4);   // from beat 105, once the breath has glided there
    std::printf("  after a jump to beat 101: the top %.0f ms from beat 104's place in the bar\n", jumped);
    CHECK(jumped <= 30.0);
    // Stopped (On Stop: Keep), it breathes on at the tempo from where it was: the tops stay on the same grid.
    const double stopBeat = h.log.time.ppqPos;
    h.log.time.flags &= ~vst::kVstTransportPlaying;
    h.run(static_cast<int>(8.5 * 44100.0 / kBlock));
    const long next = std::lround(std::fmod(4.0 - std::fmod(stopBeat, 4.0), 4.0) * 0.5 * 44100.0);
    const double stopped = apart(top(h, 0, 4), next);
    std::printf("  stopped: the top %.0f ms from where the bar would be\n", stopped);
    CHECK(stopped <= 30.0 && h.finite && rms(h.L) > 0.001);

    // Free (the default): the breath and both sways ignore the transport, sample for sample, whatever its tempo and
    // position, playing or not.
    Host x, y;
    x.log.time.flags |= vst::kVstTransportPlaying;
    y.log.time.flags |= vst::kVstTransportPlaying;
    y.log.time.tempo = 87.0;
    y.log.time.ppqPos = 13.37;
    for (Host* k : {&x, &y}) {
        k->set(af::P_G_BREATH, 1.0f);
        k->set(af::P_G_BREATHRATE, 0.4f);
        k->set(af::P_H_ONSTOP, static_cast<float>(af::OS_KEEP));
        k->on(48);
        k->on(55);
    }
    x.run(2 * kBlocksPerSec);
    y.run(2 * kBlocksPerSec);
    CHECK(maxDiff(x, y) == 0.0 && rms(x.L) > 0.001);
    x.log.time.flags &= ~vst::kVstTransportPlaying;
    x.run(kBlocksPerSec);
    y.run(kBlocksPerSec);
    CHECK(maxDiff(x, y) == 0.0);
}

void testStress() {
    std::printf("== stress: floods, extremes, random values\n");
    Host h;
    // More events than the queue holds: the note-offs still get through.
    for (int r = 0; r < 4; ++r) {
        for (int i = 0; i < 600; ++i) h.on(36 + i % 60, 1 + i % 127, i % 128);
        for (int i = 0; i < 60; ++i) h.off(36 + i, kBlock - 1);   // after every note-on of the block
        h.run(1);
    }
    h.run(2 * kBlocksPerSec);
    CHECK(h.voices() == 1);   // Bloom let every key go; Ground stays on the harmony
    // Every sound parameter at random values, notes going: finite and bounded.
    uint32_t s = 12345;
    auto rnd = [&s] {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s >> 8) / 16777216.0f;
    };
    float worst = 0.0f;
    for (int round = 0; round < 40; ++round) {
        for (int i = 0; i < af::P_COUNT; ++i)
            if (af::PARAM_INFO[i].kind == af::Kind::Synth && i != af::P_VOLUME) h.setN(i, rnd());
        h.set(af::P_VOLUME, 0.0f);
        // One rnd() per statement: the order a call's arguments are evaluated in is the compiler's
        // choice (x86 and ARM differ), and both builds must play the same notes.
        const int note = 24 + static_cast<int>(rnd() * 84);
        const int vel = 1 + static_cast<int>(rnd() * 126);
        h.on(note, vel);
        const auto lsb = static_cast<uint8_t>(rnd() * 127);
        const auto msb = static_cast<uint8_t>(rnd() * 127);
        h.midi(0xE0, lsb, msb);
        h.midi(0xB0, 1, static_cast<uint8_t>(rnd() * 127));
        h.midi(0xD0, static_cast<uint8_t>(rnd() * 127), 0);
        worst = std::max(worst, h.run(20));
        if (rnd() < 0.5f) h.midi(0xB0, 123, 0);
    }
    CHECK(h.finite);
    std::printf("  peak over 40 random patches: %.2f\n", worst);
    CHECK(worst <= 0.8913f);   // the limiter's ceiling
}

// --- Weather's source, Remember and Keep through the plugin (plugin/surface.h, plugin.cpp) ---------------

// A WAV of a sine in the test's source roots, listed at once.
void makeWav(const std::string& path, double seconds, double hz) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    const int frames = static_cast<int>(seconds * 44100.0);
    std::vector<int16_t> pcm(2 * static_cast<size_t>(frames));
    for (size_t i = 0; i < static_cast<size_t>(frames); ++i)
        pcm[2 * i] = pcm[2 * i + 1] =
            static_cast<int16_t>(std::lround(8000.0 * std::sin(2.0 * M_PI * hz * static_cast<double>(i) / 44100.0)));
    std::string err;
    CHECK(af::writeWav(path, pcm.data(), frames, &err));
    af::rescanSources();
}

std::string sourceText(Host& h) { return h.display(af::P_W_SOURCE); }

// How many cached sources something still holds (a loader's slot or its graveyard): a cap of nothing drops every
// other one (the cache never drops one in use), and the cap goes back to its 48 MB.
size_t sourcesInUse() {
    af::SourceCache& c = af::SourceCache::get();
    c.setCap(0);
    const size_t n = c.entries();
    c.setCap(48u << 20);
    return n;
}

// The loader's thread held where a hook calls (a Keep's write, a source's load), the test's to let go.
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool in = false, open = false;
    std::string key;   // a load hook holds only this key's load
    void hold() {
        std::unique_lock<std::mutex> lk(m);
        in = true;
        cv.notify_all();
        cv.wait(lk, [this] { return open; });
    }
    bool entered() {   // the loader's thread is held (within 10 s)
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::seconds(AFT_COUNTS_ALLOCS ? 10 : 100), [this] { return in; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(m);
            open = true;
        }
        cv.notify_all();
    }
};
void holdKeepWrite(void* ctx, int stage) {
    if (stage == af::KS_WRITE) static_cast<Gate*>(ctx)->hold();
}
void holdLoad(void* ctx, const std::string& key) {
    Gate* g = static_cast<Gate*>(ctx);
    if (key == g->key) g->hold();
}
// A load of `key` held for the scope's life (let go, and the hook taken away, at its end).
struct HeldLoad {
    Gate gate;
    explicit HeldLoad(const std::string& key) {
        gate.key = key;
        af::setLoadHook(holdLoad, &gate);
    }
    ~HeldLoad() {
        gate.release();
        af::setLoadHook(nullptr, nullptr);
    }
};
// A raw tap on a button (Host::press waits for Weather's source, which a held load keeps from coming).
void tap(Host& h, int id) { h.setN(id, 1.0f); }
// Weather's part of the last run (its 220 Hz, A3's, which no field has, and a test WAV's 1 kHz) against its RMS.
struct Heard {
    double rms, a220, k1;
};
Heard heard(Host& h, int blocks) {
    h.run(blocks);
    const double r = rms(h.L);
    return {r, r > 0.0 ? toneAmp(h.L, 220.0) / r : 0.0, r > 0.0 ? toneAmp(h.L, 1000.0) / r : 0.0};
}
// Only Weather heard: Ground and Bloom muted, the reverb's return closed, Weather up.
void weatherAlone(Host& h) {
    h.set(af::P_G_MUTE, 1.0f);
    h.set(af::P_B_MUTE, 1.0f);
    h.set(af::P_S_RETURN, 0.0f);
    h.set(af::P_W_LEVEL, 1.0f);
}

// Check 1: the stepper walks sourceKeys() one per detent, each source named at once and loaded (a field within
// 2 s); a turn inside the loader's debounce loads only where it stops (a step is a scroll); a file deleted under
// its key shows MISSING at its next pick, its key kept.
void testSourceStepper() {
    std::printf("== Weather's source: the stepper\n");
    makeWav(sourceDir() + "/plugin/Weather/Creek.wav", 2.0, 330.0);
    Host h;
    CHECK(showsSource(h, "Rain on Roof"));   // Init's
    const std::vector<std::string> keys = af::sourceKeys();
    CHECK(keys.size() == static_cast<size_t>(af::FD_COUNT) + 2 && keys[0] == af::defaultSourceKey() &&
          keys[af::FD_COUNT] == af::kMemoryKey && keys.back() == "plugin:Weather/Creek.wav");
    bool named = true, loaded = true;
    long long slowest = 0;
    for (size_t k = 1; k < keys.size(); ++k) {
        h.detent(af::P_W_SOURCE, +1);
        const std::string name = af::sourceName(keys[k]), now = sourceText(h);
        const bool memory = keys[k] == af::kMemoryKey;
        if (now != (memory ? name : name + " ...")) std::printf("  %s, the step: \"%s\"\n", keys[k].c_str(), now.c_str());
        named = named && now == (memory ? name : name + " ...");
        const auto t0 = std::chrono::steady_clock::now();
        const bool in = showsSource(h, name, 2000);
        slowest = std::max<long long>(slowest, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now() - t0).count());
        if (!in) std::printf("  %s: \"%s\"\n", keys[k].c_str(), sourceText(h).c_str());
        loaded = loaded && in;
    }
    std::printf("  %zu sources a detent each, the slowest in %lld ms\n", keys.size(), slowest);
    CHECK(named && loaded);
    h.detent(af::P_W_SOURCE, +1);   // the end: nothing further
    CHECK(sourceText(h) == "Creek");
    h.detent(af::P_W_SOURCE, -1);
    CHECK(sourceText(h) == "Memory");
    h.press(af::P_W_SOURCE_NEXT);   // an arrow: a pick, the same way
    CHECK(showsSource(h, "Creek") && h.finite);

    // Four files the cache hasn't seen, turned through faster than the debounce: one is made, where the turn stops.
    // (A machine too slow to turn four detents within the debounce, qemu at its worst, may make more.)
    for (int k = 1; k <= 4; ++k) makeWav(sourceDir() + "/plugin/Weather/Pebble " + std::to_string(k) + ".wav", 1.0, 200.0 * k);
    af::sourceKeys();   // the listing made before the turn, as a stepper's first look would
    const int made = af::sourcesMade();
    const auto turn = std::chrono::steady_clock::now();
    for (int k = 0; k < 4; ++k) h.detent(af::P_W_SOURCE, +1);
    const long long turnMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - turn).count();
    CHECK(sourceText(h) == "Pebble 4 ...");
    CHECK(showsSource(h, "Pebble 4"));
    std::this_thread::sleep_for(std::chrono::milliseconds(AFT_COUNTS_ALLOCS ? 200 : 2000));   // anything else in flight lands
    std::printf("  a turn through four new files in %lld ms: %d made\n", turnMs, af::sourcesMade() - made);
    CHECK(turnMs >= af::Loader::kDebounceMs ? af::sourcesMade() - made >= 1 : af::sourcesMade() == made + 1);

    // A file deleted under its key: what plays stays, and the next pick of the key (the project loaded again)
    // finds it gone: MISSING, the key kept.
    h.press(af::P_W_SOURCE_PREV);
    h.press(af::P_W_SOURCE_PREV);
    h.press(af::P_W_SOURCE_PREV);
    h.press(af::P_W_SOURCE_PREV);
    CHECK(showsSource(h, "Creek"));
    std::filesystem::remove(sourceDir() + "/plugin/Weather/Creek.wav");
    af::rescanSources();
    const std::string project = h.chunk();
    CHECK(project.find("\nw_source=plugin:Weather/Creek.wav\n") != std::string::npos);
    CHECK(h.load(project) == 1);
    CHECK(showsSource(h, "MISSING Creek") && h.chunk().find("\nw_source=plugin:Weather/Creek.wav\n") != std::string::npos);
    h.run(8);
    CHECK(h.finite);
    for (int k = 1; k <= 4; ++k) std::filesystem::remove(sourceDir() + "/plugin/Weather/Pebble " + std::to_string(k) + ".wav");
    af::rescanSources();
}

// memory: is the engine's: the Source stepper says "Memory" (never MISSING, never loading), and the loader is
// never asked for it, so the source it had is still there to go back to, at once.
void testMemoryKey() {
    std::printf("== Weather's source: Memory is the engine's\n");
    Host h;
    CHECK(showsSource(h, "Rain on Roof"));
    CHECK(h.load("ambientforce 1\nw_source=memory:\n") == 1 && sourceText(h) == "Memory");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));   // past the debounce, many of the loader's passes
    h.run(4);
    CHECK(sourceText(h) == "Memory" && h.chunk().find("\nw_source=memory:\n") != std::string::npos);
    CHECK(h.load("ambientforce 1\nw_source=builtin:Rain on Roof\n") == 1 && sourceText(h) == "Rain on Roof");
}

// Check 3, and the button's way to the engine: a Remember pressed through setParameter reaches the engine at the
// next block (the audio thread's), says what came of it, and makes Memory Weather's source; Weather turned up then
// plays what was remembered. Refused, it says why and leaves the source.
void testRemember() {
    std::printf("== Remember through the plugin\n");
    Host a;   // nothing heard yet
    CHECK(showsSource(a, "Rain on Roof"));
    a.press(af::P_W_REMEMBER);
    a.run(1);
    CHECK(a.display(af::P_STATUS) == "REMEMBER: nothing heard yet");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(sourceText(a) == "Rain on Roof");

    // A3 (220 Hz, Bloom's root, Ground under it) for 3 s, then Remember; c the same without.
    Host h, c;
    for (Host* x : {&h, &c}) {
        CHECK(showsSource(*x, "Rain on Roof"));
        x->on(57);
        x->run(3 * kBlocksPerSec);
    }
    h.press(af::P_W_REMEMBER);
    h.run(1);
    CHECK(h.display(af::P_STATUS) == "REMEMBER: the last 3.0 s are Weather's source");
    CHECK(showsSource(h, "Memory"));
    h.press(af::P_W_REMEMBER);   // within 2 s of the last
    h.run(1);
    CHECK(h.display(af::P_STATUS) == "REMEMBER: again in a moment" && sourceText(h) == "Memory");
    // The rest silent, Weather up: from Memory, A3's 220 Hz (no field has it); c plays Rain on Roof.
    double tone[2] = {}, level[2] = {};
    int k = 0;
    for (Host* x : {&h, &c}) {
        x->set(af::P_G_MUTE, 1.0f);
        x->set(af::P_B_MUTE, 1.0f);
        x->set(af::P_S_RETURN, 0.0f);
        x->set(af::P_W_LEVEL, 1.0f);
        x->run(kBlocksPerSec);
        x->run(2 * kBlocksPerSec);
        level[k] = rms(x->L);
        tone[k] = toneAmp(x->L, 220.0);
        ++k;
    }
    std::printf("  Weather from Memory: %.1f dBFS RMS, 220 Hz at %.2f of it; from Rain on Roof: %.1f dBFS, %.3f\n",
                db(level[0]), tone[0] / level[0], db(level[1]), tone[1] / level[1]);
    CHECK(h.finite && c.finite && level[0] > 0.003 && level[1] > 0.003);
    CHECK(tone[0] > 0.1 * level[0] && tone[1] < 0.03 * level[1]);

    // The move to Memory is the audio thread's, in the Remember's own block: with the loader's thread held in a
    // load (its tick can't run), Weather plays Memory all the same, and the key follows once the thread is free
    // (a Remember after a pick is the choice: the file that comes in meanwhile doesn't take Weather from Memory).
    makeWav(sourceDir() + "/plugin/Weather/Held.wav", 1.0, 1000.0);
    Host m;
    CHECK(showsSource(m, "Rain on Roof"));
    m.on(57);
    m.run(3 * kBlocksPerSec);
    {
        HeldLoad held("plugin:Weather/Held.wav");
        m.loadRaw("ambientforce 1\nw_source=plugin:Weather/Held.wav\n");
        CHECK(held.gate.entered());
        tap(m, af::P_W_REMEMBER);
        m.run(1);
        CHECK(m.display(af::P_STATUS) == "REMEMBER: the last 3.0 s are Weather's source");
        weatherAlone(m);
        m.run(kBlocksPerSec);
        const Heard w = heard(m, 2 * kBlocksPerSec);
        std::printf("  Remembered with the loader's thread held: Weather's 220 Hz at %.2f of it, \"%s\"\n", w.a220,
                    sourceText(m).c_str());
        CHECK(w.rms > 0.003 && w.a220 > 0.1 && sourceText(m) == "Held ...");
    }
    CHECK(showsSource(m, "Memory"));
    const Heard after = heard(m, kBlocksPerSec);
    CHECK(after.a220 > 0.1 && after.k1 < 0.05 && m.finite);
}

// Leaving Memory: with something remembered, Weather plays it on until the source picked has loaded (then that
// source); with nothing remembered there is nothing to hold, and Weather goes straight to the source the loader
// has, rather than silence for the whole load.
void testLeavingMemory() {
    std::printf("== Weather's source: leaving Memory\n");
    makeWav(sourceDir() + "/plugin/Weather/Held.wav", 1.0, 1000.0);
    {
        Host a;   // nothing remembered
        CHECK(showsSource(a, "Rain on Roof"));
        weatherAlone(a);
        a.on(57);
        a.run(kBlocksPerSec);
        CHECK(a.load("ambientforce 1\nw_source=memory:\n") == 1);
        a.run(kBlocksPerSec / 2);   // (the grains fading)
        const Heard onMemory = heard(a, kBlocksPerSec);
        af::SourceCache::get().clear();   // Held.wav to be read: its load held
        HeldLoad held("plugin:Weather/Held.wav");
        a.loadRaw("ambientforce 1\nw_source=plugin:Weather/Held.wav\n");
        CHECK(held.gate.entered());
        const Heard loading = heard(a, kBlocksPerSec);
        std::printf("  nothing remembered: on Memory %.1f dBFS, while Held.wav loads %.1f dBFS (the field the loader has)\n",
                    db(onMemory.rms), db(loading.rms));
        CHECK(onMemory.rms < 1e-4 && loading.rms > 0.003 && loading.k1 < 0.05);
    }
    {
        Host b;   // something remembered
        CHECK(showsSource(b, "Rain on Roof"));
        b.on(57);
        b.run(3 * kBlocksPerSec);
        tap(b, af::P_W_REMEMBER);
        b.run(1);
        CHECK(showsSource(b, "Memory"));
        weatherAlone(b);
        b.run(kBlocksPerSec);
        af::SourceCache::get().clear();
        Heard loading{}, loaded{};
        {
            HeldLoad held("plugin:Weather/Held.wav");
            b.loadRaw("ambientforce 1\nw_source=plugin:Weather/Held.wav\n");
            CHECK(held.gate.entered());
            loading = heard(b, kBlocksPerSec);
        }
        CHECK(showsSource(b, "Held"));
        b.run(kBlocksPerSec / 2);
        loaded = heard(b, kBlocksPerSec);
        std::printf("  remembered: while Held.wav loads 220 Hz at %.2f and 1 kHz at %.2f; loaded, %.2f and %.2f\n", loading.a220,
                    loading.k1, loaded.a220, loaded.k1);
        CHECK(loading.a220 > 0.1 && loading.k1 < 0.05 && loaded.k1 > 0.1 && loaded.a220 < 0.05 && b.finite);
    }
}

// Check 4, and Keep's in-flight flag: with nothing remembered nothing is written and the status line says so; with
// something, Memory 001.wav appears on the SSD, the source becomes it, and a project saved then reloads it from the
// file. A Keep pressed while one writes is ignored (queued, it would write the same audio again as Memory 003).
// No SSD: it says it couldn't write.
void testKeep() {
    std::printf("== Keep through the plugin\n");
    const std::string memories = sourceDir() + "/ssd/AmbientForce/Memories";
    std::error_code ec;
    std::filesystem::remove_all(memories, ec);
    Host a;
    a.press(af::P_W_KEEP);
    CHECK(statusSays(a, "KEEP: nothing remembered yet") && !std::filesystem::exists(memories + "/Memory 001.wav"));

    Host h;
    h.on(57);
    h.run(3 * kBlocksPerSec);
    h.press(af::P_W_REMEMBER);
    h.run(1);
    CHECK(showsSource(h, "Memory"));
    h.press(af::P_W_KEEP);
    CHECK(statusSays(h, "KEEP: Memory 001 on the SSD") && std::filesystem::exists(memories + "/Memory 001.wav"));
    CHECK(showsSource(h, "Memory 001"));
    const std::string key = "ssd:AmbientForce/Memories/Memory 001.wav";
    const std::string project = h.chunk();
    CHECK(project.find("\nw_source=" + key + "\n") != std::string::npos);
    af::WavData w;
    std::string err;
    CHECK(af::readWav(memories + "/Memory 001.wav", w, 60.0f, &err) && std::labs(static_cast<long>(w.l.size()) - 3 * kBlocksPerSec * kBlock) <= 4);
    // Reloaded from the file, not from the cache.
    af::SourceCache::get().clear();
    const int made = af::sourcesMade();
    {
        Host r;
        CHECK(r.load(project) == 1 && showsSource(r, "Memory 001") && af::sourcesMade() == made + 1);
    }

    // A Keep pressed while one writes: ignored. (Back on Memory first: Keep's file becomes the source only in
    // place of Memory.)
    CHECK(h.load("ambientforce 1\nw_source=memory:\n") == 1);
    Gate gate;
    af::setKeepHook(holdKeepWrite, &gate);
    h.press(af::P_W_KEEP);
    CHECK(gate.entered());
    h.press(af::P_W_KEEP);
    h.run(2);
    h.press(af::P_W_KEEP);
    gate.release();
    CHECK(statusSays(h, "KEEP: Memory 002 on the SSD"));
    std::this_thread::sleep_for(std::chrono::milliseconds(AFT_COUNTS_ALLOCS ? 300 : 3000));   // many of the loader's passes
    h.run(4);
    af::setKeepHook(nullptr, nullptr);
    CHECK(std::filesystem::exists(memories + "/Memory 002.wav") && !std::filesystem::exists(memories + "/Memory 003.wav"));
    CHECK(showsSource(h, "Memory 002"));

    // No SSD.
    const char* had = std::getenv("AF_SOURCE_ROOTS");
    const std::string roots = had ? had : "";
    setenv("AF_SOURCE_ROOTS", (sourceDir() + "/plugin:" + sourceDir() + "/no-ssd").c_str(), 1);
    af::rescanSources();
    h.press(af::P_W_KEEP);
    CHECK(statusSays(h, "KEEP: couldn't write to the SSD"));
    setenv("AF_SOURCE_ROOTS", roots.c_str(), 1);
    af::rescanSources();
    CHECK(h.finite && !std::filesystem::exists(memories + "/Memory 003.wav"));
}

// Keep's file becomes the source only in place of Memory, and only if nothing newer was remembered while it was
// written: a source picked during the write stays, and so does Memory after a Remember during the write.
void testKeepSwitch() {
    std::printf("== Keep: what the player chose meanwhile stays\n");
    const auto kept = [](const std::string& s) {
        return s.compare(0, 13, "KEEP: Memory ") == 0 && s.size() > 24 && s.compare(s.size() - 11, 11, " on the SSD") == 0;
    };
    {
        Host h;
        h.on(57);
        h.run(3 * kBlocksPerSec);
        tap(h, af::P_W_REMEMBER);
        h.run(1);
        CHECK(showsSource(h, "Memory"));
        Gate gate;
        af::setKeepHook(holdKeepWrite, &gate);
        tap(h, af::P_W_KEEP);
        CHECK(gate.entered());
        h.loadRaw("ambientforce 1\nw_source=builtin:Surf\n");   // picked while the file is written
        gate.release();
        CHECK(waitFor([&] {
            h.run(1);
            return kept(h.display(af::P_STATUS));
        }));
        af::setKeepHook(nullptr, nullptr);
        CHECK(showsSource(h, "Surf"));
        std::this_thread::sleep_for(std::chrono::milliseconds(AFT_COUNTS_ALLOCS ? 200 : 2000));
        CHECK(sourceText(h) == "Surf" && h.chunk().find("\nw_source=builtin:Surf\n") != std::string::npos);
    }
    {
        Host g;
        g.on(57);
        g.run(3 * kBlocksPerSec);
        tap(g, af::P_W_REMEMBER);
        g.run(1);
        CHECK(showsSource(g, "Memory"));
        Gate gate;
        af::setKeepHook(holdKeepWrite, &gate);
        tap(g, af::P_W_KEEP);
        CHECK(gate.entered());
        g.run(5 * kBlocksPerSec / 2);   // 2.5 s more: a Remember may come again (and the ring is let go before the write)
        tap(g, af::P_W_REMEMBER);
        g.run(1);
        CHECK(g.display(af::P_STATUS).compare(0, 19, "REMEMBER: the last ") == 0);
        gate.release();
        CHECK(waitFor([&] {
            g.run(1);
            return kept(g.display(af::P_STATUS));
        }));
        af::setKeepHook(nullptr, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(AFT_COUNTS_ALLOCS ? 200 : 2000));
        CHECK(sourceText(g) == "Memory" && g.chunk().find("\nw_source=memory:\n") != std::string::npos && g.finite);
    }
}

// The Source stepper after the list has changed under its key: twenty files listed before it since the value was
// pushed, a detent to the right goes one on from the key, to the right; and a name too long for the stepper is cut
// to fit (fitText; surface.py checks the cases, params_test holds fitText to them).
void testStepperListing() {
    std::printf("== Weather's source: the stepper over a list that changed\n");
    makeWav(sourceDir() + "/plugin/Weather/Bb.wav", 1.0, 300.0);
    makeWav(sourceDir() + "/plugin/Weather/Cc.wav", 1.0, 400.0);
    Host h;
    CHECK(h.load("ambientforce 1\nw_source=plugin:Weather/Bb.wav\n") == 1 && sourceText(h) == "Bb");
    for (int k = 0; k < 20; ++k) {
        char name[64];
        std::snprintf(name, sizeof name, "/plugin/Weather/A%02d.wav", k);
        makeWav(sourceDir() + name, 0.6, 500.0);
    }
    h.detent(af::P_W_SOURCE, +1);
    std::printf("  one detent right from Bb, twenty files listed before it: \"%s\"\n", sourceText(h).c_str());
    CHECK(sourceText(h) == "Cc ...");
    h.detent(af::P_W_SOURCE, -1);
    CHECK(showsSource(h, "Bb"));
    // A long name, cut.
    const std::string longName = "Rain on the tin roof of the boathouse";
    makeWav(sourceDir() + "/plugin/Weather/" + longName + ".wav", 1.0, 600.0);
    CHECK(h.load("ambientforce 1\nw_source=plugin:Weather/" + longName + ".wav\n") == 1);
    const std::string shown = sourceText(h);
    std::printf("  \"%s\" shows as \"%s\"\n", longName.c_str(), shown.c_str());
    CHECK(shown == af::fitText("", longName, "", af::kSourceTextRoom) && shown.size() < longName.size() &&
          shown.compare(shown.size() - 2, 2, "..") == 0 && longName.compare(0, shown.size() - 2, shown, 0, shown.size() - 2) == 0);
    for (int k = 0; k < 20; ++k) {
        char name[64];
        std::snprintf(name, sizeof name, "/plugin/Weather/A%02d.wav", k);
        std::filesystem::remove(sourceDir() + name);
    }
    for (const char* f : {"Bb", "Cc", "Rain on the tin roof of the boathouse"})
        std::filesystem::remove(sourceDir() + "/plugin/Weather/" + f + ".wav");
    af::rescanSources();
}

// Every block ends on the loader's count (plugin.cpp: one blockDone() for every blockStart(), with what Weather
// holds), so the loader frees a replaced source when it may: here the host's transport callback throwing, and a
// suspend with Weather silent and with Weather sounding.
void testSourceBlocks() {
    std::printf("== Weather's source: every block on the loader's count\n");
    // Long enough for the loader to have made many passes (each frees what it may): what is still in use then
    // is kept. What may be freed is waited for.
    const auto passes = [] { std::this_thread::sleep_for(std::chrono::milliseconds(AFT_COUNTS_ALLOCS ? 150 : 1500)); };
    const auto inUse = [](size_t n) { return waitFor([n] { return sourcesInUse() == n; }, 2000); };
    af::SourceCache::get().clear();
    {
        // Weather sounding on Surf, Stream picked, then the host's transport callback throwing for three blocks.
        // A block cut short there would have read Stream without giving it to Weather, still on Surf, and told
        // the loader it was done: Surf freed (here by the cache's cap, taken down), and the next block's copy from
        // Surf a read of freed memory (ASan stops the run). The throw is caught where it is made, so each block
        // plays whole, on the transport as it last was: Weather sounds on, Surf is let go of in the first.
        Host w;
        w.set(af::P_W_LEVEL, 1.0f);
        CHECK(w.load("ambientforce 1\nw_source=builtin:Surf\n") == 1 && showsSource(w, "Surf"));
        w.on(60);
        w.run(kBlocksPerSec);
        CHECK(w.load("ambientforce 1\nw_source=builtin:Stream\n") == 1 && showsSource(w, "Stream"));
        w.log.throwOnTime = true;
        const float during = w.run(3);
        w.log.throwOnTime = false;
        passes();
        const size_t n = sourcesInUse();   // what isn't held is dropped from the cache: freed
        const float after = w.run(kBlocksPerSec / 4);
        std::printf("  the host throwing for three blocks: peak %.3f in them, %zu sources in use, then peak %.3f\n", during,
                    n, after);
        CHECK(during > 0.0f && after > 0.0f && n == 1 && w.finite);
    }
    {
        Host h;   // Weather silent (level 0): its blocks hold nothing, the throw changes none of that
        CHECK(h.load("ambientforce 1\nw_source=builtin:Surf\n") == 1 && showsSource(h, "Surf"));
        h.run(4);
        h.log.throwOnTime = true;
        CHECK(h.run(3) == 0.0f && h.finite);
        h.log.throwOnTime = false;
        CHECK(h.load("ambientforce 1\nw_source=builtin:Stream\n") == 1 && showsSource(h, "Stream"));
        CHECK(inUse(1));
    }
    {
        Host q;   // suspended, Weather silent: a source picked meanwhile frees the one before at once
        CHECK(q.load("ambientforce 1\nw_source=builtin:Surf\n") == 1 && showsSource(q, "Surf"));
        q.run(4);
        q.e->dispatcher(q.e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);
        CHECK(q.load("ambientforce 1\nw_source=builtin:Stream\n") == 1 && showsSource(q, "Stream"));
        CHECK(inUse(1));
        q.e->dispatcher(q.e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);
    }
    {
        Host w;   // suspended with Weather sounding: the source it reads stays until a block has run
        w.set(af::P_W_LEVEL, 1.0f);
        CHECK(w.load("ambientforce 1\nw_source=builtin:Surf\n") == 1 && showsSource(w, "Surf"));
        w.on(60);
        w.run(kBlocksPerSec);
        w.e->dispatcher(w.e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);
        CHECK(w.load("ambientforce 1\nw_source=builtin:Stream\n") == 1 && showsSource(w, "Stream"));
        passes();
        CHECK(sourcesInUse() == 2);
        w.e->dispatcher(w.e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);
        w.run(2);
        CHECK(inUse(1) && w.finite);
    }
}

// Check 5: an instance closed while its loader is making a source joins it cleanly (ASan: nothing leaked, nothing
// read after it was freed): held in a load, the close waits for the load and goes once it is let go; held in a
// Keep's write, likewise (on the device MPC's thread waits as long as the SSD takes: device-pending); and forty
// opened and closed while loading, one after another and twenty at once.
void testSourceLifetime() {
    std::printf("== Weather's source: instances closed while loading\n");
    for (int k = 0; k < 20; ++k) makeWav(sourceDir() + "/plugin/Weather/Lot " + std::to_string(k) + ".wav", 2.0, 100.0 + k);
    af::SourceCache::get().clear();
    const auto waitsThenGoes = [](Host* h, Gate& gate) {   // closed on another thread: it waits for the gate, then goes
        std::atomic<bool> closed{false};
        std::thread closer([&] {
            delete h;
            closed = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const bool waited = !closed.load();
        gate.release();
        closer.join();
        return waited && closed.load();
    };
    {
        HeldLoad held("plugin:Weather/Lot 0.wav");
        Host* h = new Host;
        h->loadRaw("ambientforce 1\nw_source=plugin:Weather/Lot 0.wav\n");
        CHECK(held.gate.entered());
        CHECK(waitsThenGoes(h, held.gate));
    }
    {
        Host* k = new Host;
        k->on(57);
        k->run(kBlocksPerSec);
        tap(*k, af::P_W_REMEMBER);
        k->run(1);
        Gate gate;
        af::setKeepHook(holdKeepWrite, &gate);
        tap(*k, af::P_W_KEEP);
        CHECK(gate.entered());
        CHECK(waitsThenGoes(k, gate));
        af::setKeepHook(nullptr, nullptr);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < 20; ++k) {
        Host h;
        h.loadRaw("ambientforce 1\nw_source=plugin:Weather/Lot " + std::to_string(k) + ".wav\n");
        h.run(1);
    }
    {
        std::vector<std::unique_ptr<Host>> hs;
        af::SourceCache::get().clear();
        for (int k = 0; k < 20; ++k) {
            hs.push_back(std::make_unique<Host>());
            hs.back()->loadRaw("ambientforce 1\nw_source=plugin:Weather/Lot " + std::to_string(19 - k) + ".wav\n");
            hs.back()->run(1);
        }
    }
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  40 instances opened and closed mid-load in %lld ms\n", ms);
    CHECK(ms < 60000);
    for (int k = 0; k < 20; ++k) std::filesystem::remove(sourceDir() + "/plugin/Weather/Lot " + std::to_string(k) + ".wav");
    af::rescanSources();
}

// Check 6: nothing in processReplacing allocates or locks, whatever the loader, Remember and Keep do around it (the
// loader's handoff is atomics only; the buttons leave atomics; the messages are atomics). Counted on the thread
// playing the audio thread (allocations and pthread mutex locks, above), with the host keeping nothing of what it
// is pushed. (Waits: the same calls, none of which waits on anything.)
void testAudioThreadQuiet() {
    std::printf("== processReplacing: nothing allocated, nothing locked\n");
#if AFT_COUNTS_ALLOCS
    CHECK(hookAllocations());
    Host h;
    h.log.record = false;
    float L[kBlock], R[kBlock];
    float* out[2] = {L, R};
    int allocs = 0, blocks = 0;
    g_locks = 0;
    {   // the counter counts: a lock taken while it counts is seen
        std::mutex probe;
        t_countingLocks = true;
        probe.lock();
        probe.unlock();
        t_countingLocks = false;
        CHECK(g_locks == 1);
        g_locks = 0;
    }
    const auto play = [&](int n) {   // n blocks, counted
        countAllocations();
        t_countingLocks = true;
        for (int b = 0; b < n; ++b) h.e->processReplacing(h.e, nullptr, out, kBlock);
        t_countingLocks = false;
        allocs += allocationsCounted();
        blocks += n;
    };
    const auto until = [&](const std::function<bool()>& f) {
        return waitFor([&] {
            play(2);
            return f();
        });
    };
    h.set(af::P_W_LEVEL, 1.0f);
    h.set(af::P_W_ECHO, 0.5f);
    h.on(57);
    play(kBlocksPerSec);
    CHECK(until([&] { return sourceText(h) == "Rain on Roof"; }));
    h.press(af::P_W_SOURCE_NEXT);   // a source swapped under Weather as it plays
    CHECK(until([&] { return sourceText(h) == "Light Rain"; }));
    play(kBlocksPerSec);
    h.press(af::P_W_REMEMBER);   // Remember, and Weather moved to Memory
    CHECK(until([&] { return sourceText(h) == "Memory"; }));
    play(kBlocksPerSec);
    h.press(af::P_W_KEEP);   // Keep writing on the loader's thread, then Weather moved to the file
    CHECK(until([&] { return sourceText(h).compare(0, 8, "Memory 0") == 0 && sourceText(h).find("...") == std::string::npos; }));
    play(kBlocksPerSec);
    h.load("ambientforce 1\nw_source=builtin:Surf\n");   // away from the file
    CHECK(until([&] { return sourceText(h) == "Surf"; }));
    play(kBlocksPerSec / 2);
    std::printf("  %d blocks: %d allocations, %d locks\n", blocks, allocs, g_locks);
    CHECK(allocs == 0 && g_locks == 0);
#else
    std::printf("  (counted under ASan: make test)\n");
#endif
}

} // namespace

long long aft::g_msPerEvent = 1000;

int main() {
    using namespace aft;
    const std::string root = fixtureDir();
    std::filesystem::create_directories(root + "/presets");
    std::filesystem::create_directories(root + "/data");
    std::filesystem::create_directories(sourceDir() + "/plugin/Weather");
    std::filesystem::create_directories(sourceDir() + "/ssd");
    setenv("AF_PRESET_ROOTS", (root + "/presets").c_str(), 1);
    setenv("AF_DATA_DIR", (root + "/data").c_str(), 1);
    setenv("AF_SOURCE_ROOTS", (sourceDir() + "/plugin:" + sourceDir() + "/ssd").c_str(), 1);   // never the real SSD
    setenv("AF_FIXED_SEED", "1", 1);   // every instance the same random numbers: they are compared
    // Host events a second apart unless a test says otherwise (aft::Turn): stepping never
    // mistakes two of them for one turn, whatever the machine's speed (and qemu's).
    af::Surface::clock = [] {
        static long long t = 0;
        return t += g_msPerEvent;
    };

    harmonyTests();
    airgenTests();
    engineTests();
    tablesTests();   // early: it starts, stops and restarts the shared builder itself, from no instance alive
    lifeoscTests();
    groundTests();
    bloomTests();
    airvoicesTests();
    reverbTests();
    echoTests();
    weatherTests();
    fieldsTests();
    sourcesTests();
    testBasics();
    testGetters();
    testPlay();
    testStop();
    testTrace();
    testStatusLine();
    testProcessLegacy();
    testMidiMapping();
    testSync();
    testSourceStepper();
    testStepperListing();
    testMemoryKey();
    testRemember();
    testLeavingMemory();
    testKeep();
    testKeepSwitch();
    testSourceBlocks();
    testSourceLifetime();
    testAudioThreadQuiet();
    paramsTests();
    presetTests();
    testStress();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
