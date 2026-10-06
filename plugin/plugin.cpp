// From SubForce plugin/plugin.cpp (8846421), namespace sf -> af; the engine is af::Engine.
// AmbientForce as a VST2 instrument for MPC OS (Force / MPC standalone).
//
// The engine (dsp/engine.h) behind the touchscreen pages generated from surface/surface.py;
// plugin/surface.* decides what every parameter does, and the status line carries a CPU meter
// so the cost can be read on the device.
//
// Threads: processReplacing runs on one of MPC's audio workers (which one changes between
// calls, instances run concurrently); parameters, display text and chunks come from MPC's UI
// side. Host callbacks are only made from processReplacing.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vst2.h"
#include "param_ids.h"
#include "patch_map.h"
#include "state.h"
#include "surface.h"
#include "tables.h"
#include "trace.h"
#include "../dsp/engine.h"
#include "../dsp/stages.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace {

using namespace af;

constexpr size_t kTextCap = 128;   // JUCE reads names/display text into 256 bytes; stay well inside
constexpr int kMaxMidi = 512;
constexpr float kSampleRate = 44100.0f;   // MPC OS always runs 44.1 kHz
constexpr int kScratch = 512;

// Denormals (the tails of decaying filters and envelopes) are slow on the VFP unit. Flush
// them to zero for our own arithmetic only: MPC's callbacks (the song position, automation,
// display updates) run in its worker's own FP mode, which is handed back afterwards.
class FlushDenormals {
public:
    FlushDenormals() {
#if defined(__arm__)
        asm volatile("vmrs %0, fpscr" : "=r"(saved_));
        asm volatile("vmsr fpscr, %0" : : "r"(saved_ | (1u << 24)));   // FZ
#elif defined(__SSE__) || defined(__x86_64__)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040);   // FTZ | DAZ
#endif
    }
    ~FlushDenormals() {
#if defined(__arm__)
        asm volatile("vmsr fpscr, %0" : : "r"(saved_));
#elif defined(__SSE__) || defined(__x86_64__)
        _mm_setcsr(saved_);
#endif
    }
    FlushDenormals(const FlushDenormals&) = delete;
    FlushDenormals& operator=(const FlushDenormals&) = delete;

private:
    uint32_t saved_ = 0;
};

double threadCpuUs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

struct RawMidi {
    int32_t delta;
    uint8_t status, d1, d2;
};

struct Plugin {
    AEffect             fx;          // must stay the first member: MPC hands us &fx back
    audioMasterCallback master = nullptr;
    Surface             surface;
    std::atomic<bool>   panic{false};
    af::Engine          engine{kSampleRate};
    std::string         chunk;       // effGetChunk buffer: must outlive the call

    // audio thread only
    float    snapshot[P_COUNT] = {};
    bool     havePatch = false;   // patch below is built from snapshot
    uint32_t seenWrites = 0;      // the surface's write count that snapshot is current for
    RawMidi  midi[kMaxMidi] = {};
    int      nMidi = 0;
    float    scratch[2][kScratch] = {};
    int      ppqOffset = 0;   // process(): this sub-block starts this many samples into the host's block

    // CPU meter: the audio thread sums its own CPU time against the real-time budget and
    // publishes twice a second; the status line is formatted on the UI thread.
    double           winUs = 0.0, winBudgetUs = 0.0, winPeak = 0.0;
    std::atomic<int> shownVoices{0}, shownAvg{0}, shownPeak{0};   // percent
    int              lastVoices = -1, lastAvg = -1, lastPeak = -1;
};

Plugin* self(AEffect* e) { return static_cast<Plugin*>(e->object); }

// Copies at most cap - 1 bytes, never cutting a UTF-8 character in half.
void copyStr(void* dst, const std::string& s, size_t cap) {
    if (!dst || cap == 0) return;
    size_t n = std::min(s.size(), cap - 1);
    if (n < s.size())
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // s[n] continues a character
    std::memcpy(dst, s.data(), n);
    static_cast<char*>(dst)[n] = 0;
}

std::string statusText(const Plugin* p) {
    char b[80];
    std::snprintf(b, sizeof b, "VOICES %d   CPU %d%%   PEAK %d%%", p->shownVoices.load(), p->shownAvg.load(),
                  p->shownPeak.load());
    return b;
}

// --- parameters (UI thread) ---------------------------------------------------------------

// Nothing may throw into MPC (a preset load reads a file, allocates, parses).
float getParameter(AEffect* e, int32_t i) {
    try {
        return self(e)->surface.get(i);
    } catch (...) {
        return 0.0f;
    }
}

void setParameter(AEffect* e, int32_t i, float v) {
    try {
        Surface& s = self(e)->surface;
        const bool traced = i >= 0 && i < P_COUNT && tracing();
        const float before = traced ? s.get(i) : 0.0f;   // what MPC last read back
        s.set(i, v);
        if (traced)
            trace("%p set %3d %-18s %.4f  read %.4f -> %.4f  \"%s\"", static_cast<void*>(e), static_cast<int>(i),
                  PARAM_INFO[i].key, static_cast<double>(v), static_cast<double>(before), static_cast<double>(s.get(i)),
                  s.display(i).c_str());
    } catch (...) {
    }
}

// --- audio thread -------------------------------------------------------------------------

void handleMidi(Plugin* p, const RawMidi& m) {
    af::Engine& s = p->engine;
    switch (m.status & 0xF0) {
        case 0x90:
            if (m.d2) s.noteOn(m.d1, m.d2);
            else s.noteOff(m.d1);
            break;
        case 0x80: s.noteOff(m.d1); break;
        case 0xB0:
            if (m.d1 == 64) s.sustain(m.d2 >= 64);
            else if (m.d1 == 120) s.reset();
            else if (m.d1 == 121) s.resetControllers();
            else if (m.d1 == 123) s.allNotesOff();
            else s.controller(m.d1, m.d2);
            break;
        case 0xD0: s.aftertouch(static_cast<float>(m.d1) / 127.0f); break;
        case 0xA0: s.polyAftertouch(m.d1, static_cast<float>(m.d2) / 127.0f); break;   // pads send it per key
        case 0xE0:   // -1..1
            s.pitchBend(static_cast<float>((m.d2 << 7 | m.d1) - 8192) / 8192.0f);
            break;
        default: break;
    }
}

struct Transport {
    double bpm = 120.0, beats = 0.0;
    bool   playing = false, valid = false;
};

// MPC's tempo and bar position, for whatever in the engine follows them. A host callback, so in MPC's own
// FP mode.
Transport readTransport(Plugin* p) {
    Transport tr;
    if (!p->master) return tr;
    const intptr_t r = p->master(&p->fx, vst::audioMasterGetTime, 0, vst::kVstTempoValid | vst::kVstPpqPosValid, nullptr, 0.0f);
    if (const VstTimeInfo* t = reinterpret_cast<const VstTimeInfo*>(r)) {
        // A NaN or absurd value would stall or spin anything synced to it: ignore it.
        if ((t->flags & vst::kVstTempoValid) && std::isfinite(t->tempo) && t->tempo >= 1.0 && t->tempo <= 1000.0) tr.bpm = t->tempo;
        tr.valid = (t->flags & vst::kVstPpqPosValid) != 0 && std::isfinite(t->ppqPos) && std::fabs(t->ppqPos) < 1e9;
        tr.beats = tr.valid ? t->ppqPos + p->ppqOffset * tr.bpm / 60.0 / static_cast<double>(kSampleRate) : 0.0;
        tr.playing = (t->flags & vst::kVstTransportPlaying) != 0;
    }
    return tr;
}

void runBlock(Plugin* p, float* L, float* R, int n, const Transport& tr) {
    // The sound only changes when a parameter does: then rebuild the patch and hand it to the
    // engine. Looked at only when something was written since the last look.
    const uint32_t writes = p->surface.writes();   // before the snapshot: a later write shows next block
    if (!p->havePatch || writes != p->seenWrites) {
        float fresh[P_COUNT];
        if (p->surface.snapshot(fresh)) {   // mid-preset: false, look again next block
            p->seenWrites = writes;
            if (!p->havePatch || std::memcmp(fresh, p->snapshot, sizeof fresh) != 0) {
                std::memcpy(p->snapshot, fresh, sizeof fresh);
                p->engine.setPatch(patchFromParams(p->snapshot));
                p->havePatch = true;
            }
        }
    }
    if (p->panic.exchange(false)) p->engine.reset();
    p->engine.setTransport(tr.bpm, tr.beats, tr.playing, tr.valid);

    // Events in time order (insertion sort: no allocation; MPC already sends them sorted),
    // each one applied at its own sample.
    for (int i = 1; i < p->nMidi; ++i)
        for (int j = i; j > 0 && p->midi[j].delta < p->midi[j - 1].delta; --j) std::swap(p->midi[j], p->midi[j - 1]);
    int pos = 0;
    for (int i = 0; i < p->nMidi; ++i) {
        const int at = std::clamp(static_cast<int>(p->midi[i].delta), 0, n);
        if (at > pos) {
            p->engine.render(L + pos, R + pos, at - pos);
            pos = at;
        }
        handleMidi(p, p->midi[i]);
    }
    if (n > pos) p->engine.render(L + pos, R + pos, n - pos);
    p->nMidi = 0;
}

void meter(Plugin* p, double us, int n) {
    const double budget = static_cast<double>(n) * 1e6 / kSampleRate;
    p->winUs += us;
    p->winBudgetUs += budget;
    p->winPeak = std::max(p->winPeak, us / budget);
    if (p->winBudgetUs < 500000.0) return;

    const int avg = static_cast<int>(std::lround(100.0 * p->winUs / p->winBudgetUs));
    const int peak = static_cast<int>(std::lround(100.0 * p->winPeak));
    const int voices = p->engine.activeVoices();
    p->winUs = p->winBudgetUs = p->winPeak = 0.0;
    p->shownAvg.store(avg);
    p->shownPeak.store(peak);
    p->shownVoices.store(voices);
    if (avg != p->lastAvg || peak != p->lastPeak || voices != p->lastVoices) {
        p->lastAvg = avg;
        p->lastPeak = peak;
        p->lastVoices = voices;
        if (p->master) p->master(&p->fx, vst::audioMasterUpdateDisplay, 0, 0, nullptr, 0.0f);
    }
}

void hostAutomate(void* ctx, int index, float value) {
    Plugin* p = static_cast<Plugin*>(ctx);
    if (p->master) p->master(&p->fx, vst::audioMasterAutomate, index, 0, nullptr, value);
}

void hostUpdate(void* ctx) {
    Plugin* p = static_cast<Plugin*>(ctx);
    if (p->master) p->master(&p->fx, vst::audioMasterUpdateDisplay, 0, 0, nullptr, 0.0f);
}

void processReplacing(AEffect* e, float** /*in*/, float** out, int32_t n) {
    if (!out || !out[0] || !out[1] || n <= 0) return;
    Plugin* p = self(e);
    const double t0 = threadCpuUs();
    try {
        const Transport tr = readTransport(p);
        FlushDenormals ftz;
        runBlock(p, out[0], out[1], n, tr);
    } catch (...) {   // nothing may throw into MPC: an escaping exception ends the whole process
        std::memset(out[0], 0, sizeof(float) * static_cast<size_t>(n));
        std::memset(out[1], 0, sizeof(float) * static_cast<size_t>(n));
    }
    p->surface.notify(hostAutomate, hostUpdate, p);
    meter(p, threadCpuUs() - t0, n);
}

// Legacy accumulating entry point (MPC uses processReplacing): sub-blocks of kScratch, each
// with its own events and its own place on the host's timeline.
void process(AEffect* e, float** in, float** out, int32_t n) {
    if (!out || !out[0] || !out[1]) return;
    Plugin* p = self(e);
    RawMidi all[kMaxMidi];
    const int nAll = p->nMidi;
    std::copy(p->midi, p->midi + nAll, all);
    for (int32_t pos = 0; pos < n; pos += kScratch) {
        const int32_t m = std::min<int32_t>(kScratch, n - pos);
        const bool last = pos + m >= n;
        p->nMidi = 0;
        for (int i = 0; i < nAll; ++i) {
            const int32_t d = std::max<int32_t>(all[i].delta, 0);
            if ((d >= pos && d < pos + m) || (last && d >= pos + m)) {
                p->midi[p->nMidi] = all[i];
                p->midi[p->nMidi++].delta = d - pos;
            }
        }
        p->ppqOffset = pos;
        float* tmp[2] = {p->scratch[0], p->scratch[1]};
        processReplacing(e, in, tmp, m);
        for (int32_t i = 0; i < m; ++i) {
            out[0][pos + i] += tmp[0][i];
            out[1][pos + i] += tmp[1][i];
        }
    }
    p->ppqOffset = 0;
}

// The last kEndReserve slots only take what ends notes (note-off, pedal, all notes/sound off):
// a flood of note-ons or controllers must not leave a note stuck by crowding out its note-off.
constexpr int kEndReserve = 64;

void onMidi(Plugin* p, const VstEvents* evs) {
    if (!evs) return;
    for (int32_t i = 0; i < evs->numEvents && p->nMidi < kMaxMidi; ++i) {
        const VstEvent* ev = evs->events[i];
        if (!ev || ev->type != vst::kVstMidiType) continue;
        const auto* me = reinterpret_cast<const VstMidiEvent*>(ev);
        const uint8_t st = static_cast<uint8_t>(me->midiData[0]);
        const uint8_t d1 = static_cast<uint8_t>(me->midiData[1] & 0x7F), d2 = static_cast<uint8_t>(me->midiData[2] & 0x7F);
        const int type = st & 0xF0;
        const bool ends = type == 0x80 || (type == 0x90 && d2 == 0) || (type == 0xB0 && (d1 == 64 || d1 == 120 || d1 == 121 || d1 == 123));
        if (!ends && p->nMidi >= kMaxMidi - kEndReserve) continue;
        p->midi[p->nMidi++] = {me->deltaFrames, st, d1, d2};
    }
}

intptr_t dispatch(Plugin* p, int32_t op, int32_t idx, intptr_t val, void* ptr) {
    const bool validIdx = idx >= 0 && idx < P_COUNT;
    switch (op) {
        case vst::effOpen: return 1;
        case vst::effClose: delete p; af::instanceClosed(); return 1;
        case vst::effGetProgram: return 0;
        case vst::effGetProgramName: copyStr(ptr, kPlugName, 24); return 0;
        case vst::effGetPlugCategory: return vst::kPlugCategSynth;
        case vst::effGetEffectName:
        case vst::effGetProductString: copyStr(ptr, kPlugName, 32); return 1;
        case vst::effGetVendorString: copyStr(ptr, kPlugVendor, 32); return 1;
        case vst::effGetVendorVersion: return kPlugVersion;
        case vst::effGetVstVersion: return 2400;
        case vst::effCanBeAutomated: return p->surface.automatable(idx) ? 1 : 0;
        case vst::effGetParamName: copyStr(ptr, validIdx ? PARAM_INFO[idx].name : "", kTextCap); return 0;
        case vst::effGetParamLabel: copyStr(ptr, "", 8); return 0;
        case vst::effGetParamDisplay:
            if (!validIdx) copyStr(ptr, "", kTextCap);
            else if (idx == P_STATUS) copyStr(ptr, statusText(p), kTextCap);
            else copyStr(ptr, p->surface.display(idx), kTextCap);
            return 0;
        case vst::effSetSampleRate:   // MPC OS is fixed at 44.1 kHz; the engine is built for it
        case vst::effSetBlockSize: return 1;
        case vst::effMainsChanged:
            if (val == 0) p->panic.store(true);   // suspended: silence when processing resumes
            return 1;
        case vst::effStopProcess: p->panic.store(true); return 0;
        case vst::effProcessEvents: onMidi(p, static_cast<const VstEvents*>(ptr)); return 1;
        case vst::effCanDo: {
            const char* s = static_cast<const char*>(ptr);
            if (!s) return -1;
            return !std::strcmp(s, "receiveVstEvents") || !std::strcmp(s, "receiveVstMidiEvent") ? 1 : -1;
        }
        case vst::effGetChunk:
            if (!ptr) return 0;
            p->chunk = saveState(p->surface, false);
            *static_cast<void**>(ptr) = const_cast<char*>(p->chunk.c_str());
            return static_cast<intptr_t>(p->chunk.size() + 1);
        case vst::effSetChunk: {
            if (!ptr || val <= 0 || val > (1 << 20)) return 0;   // a state is ~4 KB; more is not ours
            std::string s(static_cast<const char*>(ptr), static_cast<size_t>(val));
            while (!s.empty() && s.back() == '\0') s.pop_back();
            return loadState(p->surface, s, false) ? 1 : 0;
        }
        default: return 0;
    }
}

intptr_t dispatcher(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float /*opt*/) {
    try {
        return dispatch(self(e), op, idx, val, ptr);
    } catch (...) {
        return 0;
    }
}

// Every instance its own random numbers (the engine's, the browser's RND): two layered
// instances would otherwise play the same random motion, +6 dB instead of +3. AF_FIXED_SEED: the
// same every time (the tests compare instances sample for sample).
uint32_t instanceSeed(const void* p) {
    static std::atomic<uint32_t> count{0};
    const char* fixed = std::getenv("AF_FIXED_SEED");
    if (fixed && *fixed) return 0x9E3779B9u;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)) ^ static_cast<uint32_t>(ts.tv_nsec) ^
                 (count.fetch_add(1) * 0x9E3779B9u);
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h ? h : 1u;
}

AEffect* createPlugin(audioMasterCallback master) {
    Plugin* p = new Plugin();
    af::instanceOpened();   // until effClose: the shared tables stay while it may read them
    p->master = master;
    const uint32_t seed = instanceSeed(p);
    p->engine.seed(seed);
    p->surface.seed(seed * 0x2545F491u + 1u);

    AEffect* e = &p->fx;
    std::memset(e, 0, sizeof(*e));
    e->magic            = vst::kMagic;
    e->dispatcher       = dispatcher;
    e->process          = process;
    e->setParameter     = setParameter;
    e->getParameter     = getParameter;
    e->processReplacing = processReplacing;
    e->numParams        = P_COUNT;
    e->numInputs        = 0;
    e->numOutputs       = 2;
    e->flags            = vst::effFlagsCanReplacing | vst::effFlagsIsSynth | vst::effFlagsProgramChunks;
    e->uniqueID         = kPlugUid;
    e->version          = kPlugVersion;
    e->object           = p;
    return e;
}

} // namespace

// Nothing may throw into MPC: a failed creation reports "no plugin" instead.
extern "C" __attribute__((visibility("default"))) AEffect* VSTPluginMain(audioMasterCallback master) {
    try {
        af::ensureTablesBuilding();   // the first instance starts the process-wide table builds
        return createPlugin(master);
    } catch (...) {
        return nullptr;
    }
}

#ifdef AF_STAGE_TIMING
// The profiling build only (make arm-bench-stages): the engine's time per render stage since
// the last call, in microseconds, with the stages' names; tools/bench.cpp reads it with dlsym.
// Returns the number of stages written.
extern "C" __attribute__((visibility("default"))) int AmbientForceStageTimes(double* us, const char** names, int max) {
    if (!us || !names || max < af::STG_COUNT) return 0;   // all stages or none (each read starts them over)
    for (int i = 0; i < af::STG_COUNT; ++i) {
        us[i] = static_cast<double>(af::g_stageNs[i]) * 1e-3;
        names[i] = af::kStageNames[i];
        af::g_stageNs[i] = 0;
    }
    return af::STG_COUNT;
}
#endif
