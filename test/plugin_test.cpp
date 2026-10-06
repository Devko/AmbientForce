// From SubForce test/plugin_test.cpp (8846421), sft -> aft; the synth's own checks left out.
// The test suite: the engine on its own (engine_test.cpp), then the whole plugin driven through
// its VST2 entry points the way MPC drives it (128-frame blocks, events with deltaFrames, 0..1
// params). Built with ASan/UBSan by `make test`, for the Force's CPU under qemu by `make test-arm`.
#include "host.h"
#include "../plugin/surface.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
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
    h.run(kBlocksPerSec / 4);
    CHECK(h.finite && rms(h.L) > 0.01);
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
    float before = 0.0f;
    for (int i = 0; i < 64; ++i) before = std::max(before, std::fabs(h.L[static_cast<size_t>(i)]));
    CHECK(before == 0.0f && std::fabs(h.L[127]) > 0.0f);
    const float peak = h.run(kBlocksPerSec);
    CHECK(h.finite && peak > 0.05f && peak < 1.0f);
    bool mono = true;
    for (size_t i = 0; i < h.L.size(); ++i) mono = mono && h.L[i] == h.R[i];
    CHECK(mono);
    CHECK(h.voices() == 1);
    CHECK(h.display(af::P_STATUS).compare(0, 9, "VOICES 1 ") == 0);
    h.off(48);
    h.run(kBlocksPerSec / 2);   // the release is 300 ms
    CHECK(h.run(4) == 0.0f);
    CHECK(h.voices() == 0);

    // Pitch: A3 is 220 Hz, A4 440 Hz.
    Host p;
    p.on(57, 100);
    p.run(kBlocksPerSec / 4);
    p.run(kBlocksPerSec);
    CHECK(std::fabs(pitchHz(p.L) - 220.0) < 0.1);
    p.off(57);
    p.on(69, 100);
    p.run(kBlocksPerSec / 2);   // past the release of the 220 Hz
    p.run(kBlocksPerSec);
    CHECK(std::fabs(pitchHz(p.L) - 440.0) < 0.2);
    // The volume: 0 dB is 6 dB over the default -6 dB.
    const double quiet = rms(p.L);
    p.set(af::P_VOLUME, 0.0f);
    p.run(kBlocksPerSec / 4);
    p.run(kBlocksPerSec / 2);
    CHECK(std::fabs(20.0 * std::log10(rms(p.L) / quiet) - 6.0) < 0.1);

    // All sound off (CC 120) is immediate; all notes off (CC 123) releases.
    p.midi(0xB0, 120, 0);
    CHECK(p.run(1) == 0.0f);
    p.on(57);
    p.run(10);
    p.midi(0xB0, 123, 0);
    CHECK(p.run(1) > 0.0f);
    p.run(kBlocksPerSec / 2);
    CHECK(p.run(2) == 0.0f);
    // Suspend (MPC stopping the track) silences too.
    p.on(57);
    p.run(10);
    p.e->dispatcher(p.e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);
    CHECK(p.run(1) == 0.0f);
    p.on(57);
    p.run(10);
    p.e->dispatcher(p.e, vst::effStopProcess, 0, 0, nullptr, 0.0f);
    CHECK(p.run(1) == 0.0f);
    CHECK(p.finite);
}

void testProcessLegacy() {
    std::printf("== process() (accumulating)\n");
    Host a, b;
    a.on(45, 110, 300);   // one 1024-frame call (split into 512-frame sub-blocks inside)
    std::vector<float> L(1024, 0.25f), R(1024, 0.25f);   // process() adds to what is there
    float* out[2] = {L.data(), R.data()};
    a.e->process(a.e, nullptr, out, 1024);
    // The same note at the same sample through processReplacing in 128-frame blocks: sample 300 is
    // in the third block, 44 frames in.
    std::vector<float> L2(1024), R2(1024);
    for (int k = 0; k < 8; ++k) {
        if (k == 2) b.on(45, 110, 300 - 256);
        float* o[2] = {L2.data() + k * 128, R2.data() + k * 128};
        b.e->processReplacing(b.e, nullptr, o, 128);
    }
    double before = 0.0, diff = 0.0, energy = 0.0;
    for (int i = 0; i < 300; ++i) before = std::max(before, std::fabs(static_cast<double>(L[i]) - 0.25));
    for (int i = 0; i < 1024; ++i) {
        diff = std::max(diff, std::fabs(static_cast<double>(L[i]) - 0.25 - L2[i]));
        energy += std::fabs(L2[i]);
    }
    CHECK(before == 0.0 && energy > 1.0);
    CHECK(diff < 1e-6);   // block sizes don't change the sound
}

void testMidiMapping() {
    std::printf("== MIDI: velocity, the pedal, CC 121\n");
    // Velocity 0 is a note-off; velocity scales the level.
    Host v;
    v.on(57, 127);
    v.run(kBlocksPerSec / 4);
    const double loud = rms(v.L);
    v.on(57, 0);
    v.run(kBlocksPerSec / 2);
    CHECK(v.run(2) == 0.0f);
    v.on(57, 64);
    v.run(kBlocksPerSec / 4);
    CHECK(std::fabs(rms(v.L) / loud - 64.0 / 127.0) < 0.01);
    // CC 64 holds the note after its key goes up.
    Host h;
    h.on(57);
    h.midi(0xB0, 64, 127);
    h.off(57);
    h.run(kBlocksPerSec / 2);
    CHECK(rms(h.L) > 0.02);
    h.midi(0xB0, 64, 0);
    h.run(kBlocksPerSec / 2);
    CHECK(h.run(2) == 0.0f);
    // CC 121 (reset all controllers) puts the pedal back up: what it held is released.
    h.midi(0xB0, 64, 127);
    h.on(60);
    h.off(60);
    h.run(kBlocksPerSec / 2);
    CHECK(rms(h.L) > 0.02);
    h.midi(0xB0, 121, 0);
    h.run(kBlocksPerSec / 2);
    CHECK(h.run(2) == 0.0f);
    // Bend, the mod wheel and pressure reach the engine and change nothing yet.
    h.on(69);
    h.midi(0xE0, 0x7F, 0x7F);
    h.midi(0xB0, 1, 127);
    h.midi(0xD0, 100, 0);
    h.midi(0xA0, 69, 100);
    h.run(kBlocksPerSec / 4);
    h.run(kBlocksPerSec / 2);
    CHECK(std::fabs(pitchHz(h.L) - 440.0) < 0.2);
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
    h.run(kBlocksPerSec);
    CHECK(h.run(2) == 0.0f);
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
    CHECK(worst < 2.0f);
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
    engineTests();
    tablesTests();   // early: it starts, stops and restarts the shared builder itself, from no instance alive
    reverbTests();
    testBasics();
    testGetters();
    testPlay();
    testProcessLegacy();
    testMidiMapping();
    presetTests();
    testStress();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
