// From SubForce test/host.h (8846421), sft -> aft; without the synth's bare voice.
#pragma once
// A fake MPC: drives the plugin through its VST2 entry points the way MPC does (128-frame
// blocks, events with deltaFrames, 0..1 params), records what the plugin pushes back
// (audioMasterAutomate, audioMasterUpdateDisplay) and serves transport time.
#include "check.h"
#include "signal.h"
#include "../plugin/vst2.h"
#include "../plugin/patch_map.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback master);

namespace aft {

constexpr int kBlock = 128;
constexpr int kBlocksPerSec = 44100 / kBlock;

struct HostLog {
    int updates = 0;
    std::map<int, float> automated;   // index -> last value pushed
    std::map<int, int> automateCount;
    VstTimeInfo time{};
};

intptr_t hostMaster(AEffect* e, int32_t op, int32_t index, intptr_t, void*, float opt);

struct Host {
    AEffect* e;
    HostLog log;
    std::vector<float> L, R;   // last run, all samples
    bool finite = true;

    Host() : e(VSTPluginMain(hostMaster)) {
        e->user = &log;
        log.time.sampleRate = 44100.0;
        log.time.tempo = 120.0;
        log.time.timeSigNumerator = log.time.timeSigDenominator = 4;
        log.time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;
        e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    }
    ~Host() { e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    void set(int id, float value) { e->setParameter(e, id, af::paramNorm(id, value)); }
    void setN(int id, float n) { e->setParameter(e, id, n); }
    float get(int id) { return e->getParameter(e, id); }
    float value(int id) { return af::paramValue(id, get(id)); }

    // A tap on a button, as a Force sends it: MPC toggles the value it last read back. A button
    // reads back 0 (it springs back), so a tap is a single 1, never followed by a release.
    void press(int id) { e->setParameter(e, id, get(id) > 0.5f ? 0.0f : 1.0f); }
    // One Q-Link detent (dir +1 / -1), as a Force sends it: the value MPC last read back plus
    // 1/128 of the range, rounded to 1/1000 (MPC OS 3.9.1, measured by sd88me/mpc-vst-plugins,
    // docs/NOTES.md "Input probe"). Several in a Turn are one gesture.
    void detent(int id, int dir) {
        e->setParameter(e, id, std::round((get(id) + static_cast<float>(dir) / 128.0f) * 1000.0f) / 1000.0f);
    }

    void midi(uint8_t st, uint8_t d1, uint8_t d2, int delta = 0) {
        VstMidiEvent ev{};
        ev.type = vst::kVstMidiType;
        ev.byteSize = sizeof ev;
        ev.deltaFrames = delta;
        ev.midiData[0] = st;
        ev.midiData[1] = d1;
        ev.midiData[2] = d2;
        VstEvents evs{};
        evs.numEvents = 1;
        evs.events[0] = reinterpret_cast<VstEvent*>(&ev);
        e->dispatcher(e, vst::effProcessEvents, 0, 0, &evs, 0.0f);
    }
    void on(int note, int vel = 100, int delta = 0) { midi(0x90, static_cast<uint8_t>(note), static_cast<uint8_t>(vel), delta); }
    void off(int note, int delta = 0) { midi(0x80, static_cast<uint8_t>(note), 0, delta); }

    // Renders `blocks` blocks (the transport advances if playing); returns the peak |sample|.
    float run(int blocks) {
        L.assign(static_cast<size_t>(blocks) * kBlock, 0.0f);
        R.assign(L.size(), 0.0f);
        float peak = 0.0f;
        for (int b = 0; b < blocks; ++b) {
            float* out[2] = {&L[static_cast<size_t>(b) * kBlock], &R[static_cast<size_t>(b) * kBlock]};
            e->processReplacing(e, nullptr, out, kBlock);
            if (log.time.flags & vst::kVstTransportPlaying) {
                log.time.samplePos += kBlock;
                log.time.ppqPos += kBlock / 44100.0 * log.time.tempo / 60.0;
            }
        }
        for (size_t i = 0; i < L.size(); ++i) {
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) finite = false;
            peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
        }
        return peak;
    }

    std::string display(int id) {
        char b[256] = {};
        e->dispatcher(e, vst::effGetParamDisplay, id, 0, b, 0.0f);
        return b;
    }
    std::string name(int id) {
        char b[256] = {};
        e->dispatcher(e, vst::effGetParamName, id, 0, b, 0.0f);
        return b;
    }
    int voices() {   // from the status line ("VOICES n ..."), published every 0.5 s
        run(kBlocksPerSec / 2 + 2);
        return std::atoi(display(af::P_STATUS).c_str() + 7);
    }

    std::string chunk() {
        void* data = nullptr;
        const intptr_t size = e->dispatcher(e, vst::effGetChunk, 0, 0, &data, 0.0f);
        return size > 0 && data ? std::string(static_cast<const char*>(data)) : std::string();
    }
    intptr_t load(const std::string& s) {
        return e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(s.size()), const_cast<char*>(s.data()), 0.0f);
    }
};

std::string fixtureDir();   // per-run temp folder (removed at exit)

// The surface's clock moves this far per host event (plugin_test.cpp): a second, so separate
// events never read as one gesture, whatever the machine's speed.
extern long long g_msPerEvent;
// Within a Turn, events come a few ms apart, as a Q-Link turn's detents or a tile's release echo.
struct Turn {
    explicit Turn(long long ms = 5) : was(g_msPerEvent) { g_msPerEvent = ms; }
    ~Turn() { g_msPerEvent = was; }
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;
    long long was;
};

void engineTests();      // engine_test.cpp: the engine on its own (the stub: sines, voices, release)
void reverbTests();      // reverb_test.cpp: the Reverb and Space on their own
void harmonyTests();     // harmony_test.cpp: the harmony brain on its own (scales, chords, memory)
void tablesTests();      // tables_test.cpp: the table library, the life models, the builder thread
void lifeoscTests();     // lifeosc_test.cpp: the lifetime oscillator on its own (read, glide, sway, smear, couple)
void groundTests();      // ground_test.cpp: Ground on its own (partials, beat, gravity, fade, gains, stability)
void presetTests();      // preset_test.cpp: state, presets, the browser, stepping

// Signal helpers (plugin_test.cpp; rms() and the module tests' own are in signal.h).
double pitchHz(const std::vector<float>& x, size_t from = 0, size_t to = 0);   // rising zero crossings
// The amplitude of the `hz` component of x (44.1 kHz): a Hann-windowed correlation.
double toneAmp(const std::vector<float>& x, double hz);

} // namespace aft
