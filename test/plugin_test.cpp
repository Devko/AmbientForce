// From SubForce test/plugin_test.cpp (8846421), sft -> aft; the synth's own checks left out.
// The test suite: the engine on its own (engine_test.cpp), then the whole plugin driven through
// its VST2 entry points the way MPC drives it (128-frame blocks, events with deltaFrames, 0..1
// params). Built with ASan/UBSan by `make test`, for the Force's CPU under qemu by `make test-arm`.
#include "host.h"
#include "../plugin/presets.h"
#include "../plugin/surface.h"
#include "../plugin/trace.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace aft {
int g_fail = 0, g_pass = 0;

intptr_t hostMaster(AEffect* e, int32_t op, int32_t index, intptr_t, void*, float opt) {
    HostLog* log = e ? static_cast<HostLog*>(e->user) : nullptr;
    if (op == 1) return 2400;   // audioMasterVersion
    if (!log) return 0;
    if (op == vst::audioMasterUpdateDisplay) ++log->updates;
    if (op == vst::audioMasterAutomate) {
        log->automated[index] = opt;
        ++log->automateCount[index];
    }
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&log->time);
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

} // namespace

long long aft::g_msPerEvent = 1000;

int main() {
    using namespace aft;
    const std::string root = fixtureDir();
    std::filesystem::create_directories(root + "/presets");
    std::filesystem::create_directories(root + "/data");
    setenv("AF_PRESET_ROOTS", (root + "/presets").c_str(), 1);
    setenv("AF_DATA_DIR", (root + "/data").c_str(), 1);
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
    testBasics();
    testGetters();
    testPlay();
    testStop();
    testTrace();
    testStatusLine();
    testProcessLegacy();
    testMidiMapping();
    testSync();
    paramsTests();
    presetTests();
    testStress();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
