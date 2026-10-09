// From SubForce plugin/plugin.cpp (8846421), namespace sf -> af; the engine is af::Engine.
// AmbientForce as a VST2 instrument for MPC OS (Force / MPC standalone).
//
// The engine (dsp/engine.h) behind the touchscreen pages generated from surface/surface.py;
// plugin/surface.* decides what every parameter does, and the status line carries a CPU meter
// so the cost can be read on the device (and, for a few seconds, the help line of a control just
// moved or the description of a preset just loaded).
//
// Threads: processReplacing runs on one of MPC's audio workers (which one changes between
// calls, instances run concurrently); parameters, display text and chunks come from MPC's UI
// side. Host callbacks are only made from processReplacing. Each instance has a loader thread
// (plugin/loader.h): Weather's sources are made there, and Keep writes its WAV there.
//
// Weather's source, between the threads (plugin/loader.h has the argument, dsp/engine.h the engine's
// side). Every block, on every path through processReplacing: blockStart(), live(0) read once and given
// to the engine (setWeatherSource(), so Weather has it in every block in which it is audible), and at the
// end blockDone(holds), holds being engine.holdsSource() asked after the block's last render. A block
// always renders whole: the host's transport callback, the one call into MPC before the block's
// blockDone(), is caught where it is made (readTransport: a throw there plays the block on the transport as it last was). A
// block cut short by anything else has the engine reset before its blockDone(), so Weather has let go of
// everything it held and holds is false and true: the loader counts the first block to read a new
// pointer as the one in which Weather copied what it needed from the old, and a block that read it but
// never rendered must not count so with Weather still pointing into the old. Nothing else: no lock, no
// allocation, no free. While MPC isn't calling processReplacing (suspended, or a track it doesn't run)
// no block runs, and the last block's holds says whether Weather still points into its source: false
// (Weather silent), a source picked meanwhile frees the one it replaced at once; true (Weather sounding
// when processing stopped: a Stop resumes it, On Stop keeps or fades it), the replaced source is kept
// until a block has run, and the sources picked after it, which no block saw, go at once. So at most one
// replaced source waits for the track to run again, and it is the one Weather may still need.
//
// Remember and Keep (surface.h): the buttons leave requests. The audio thread gives a Remember to the
// engine before its next block, asking that Weather follow it: one that happens moves Weather onto
// Memory in that very piece (Engine::remember()). Once a control step has applied it, the audio thread
// says what came of it (the status line) and raises the surface's "Memory is the source" (an atomic,
// sticky: Weather stays on Memory); the loader's tick (every 20 ms or so) then only sets the key and the
// stepper's text. The tick takes a Keep and posts its job, which writes the file on the loader's thread
// (keepMemory) and makes the file the source if the player is still on Memory and nothing newer was
// remembered meanwhile. The loader is stopped and joined before anything it calls back into goes (the
// last member, stopLoader): the surface, and the engine's Memory, which Keep reads.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vst2.h"
#include "loader.h"
#include "param_ids.h"
#include "patch_map.h"
#include "paths.h"
#include "sources.h"
#include "state.h"
#include "surface.h"
#include "tables.h"
#include "trace.h"
#include "../dsp/engine.h"
#include "../dsp/stages.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <sys/syscall.h>
#include <unistd.h>

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

// The seconds go through 32 bits (a thread's CPU time won't reach 136 years): on ARM a 64-bit
// time_t becomes a double in libgcc, which returns early for 0, so the meter cost a few dozen
// instructions less a block while the audio thread's CPU time was under a second. That made make
// arm-icount's count depend on how fast the machine ran; a 32-bit one is a single instruction.
double threadCpuUs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(static_cast<uint32_t>(ts.tv_sec)) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

struct RawMidi {
    int32_t delta;
    uint8_t status, d1, d2;
};

// Suspend and resume reach the audio thread as times (ms): when the host suspended processing (-1:
// it hasn't since the audio thread last looked), and when it said processing resumes (-1: it
// hasn't said). A lock here would be one on the audio thread.
static_assert(std::atomic<long long>::is_always_lock_free, "the suspend times need lock-free 64-bit atomics");

// The trace (plugin/trace.h) is for Phase 0's questions: what MPC sends around Stop, and what MIDI
// reaches the plugin. The audio thread (processReplacing, and effProcessEvents, which MPC may call
// there) never writes it: file I/O and a lock. It leaves notes in lock-free slots, and the host's
// threads trace them at their next call (hostTrace(): a suspend or a resume, a parameter set, a
// display read).

// What the audio thread made of the last suspend: two numbers under a sequence lock.
struct ResumeNote {
    std::atomic<uint32_t> seq{0};        // odd while the audio thread writes; +2 a note
    std::atomic<long long> awayMs{0};    // from the suspend to the resume
    std::atomic<bool> told{false};       // the host said it resumed (else: the first block after)
    std::atomic<uint32_t> traced{0};     // the seq the host's side has traced
};

// The MIDI events that came in while the trace is on, as they came (channel and all): a ring with
// one writer (the audio thread) and one reader (a host thread at a time). Full, an event is dropped
// and counted.
struct MidiLog {
    static constexpr uint32_t kSize = 256;   // a power of two: the counts wrap through it evenly
    RawMidi ev[kSize] = {};
    std::atomic<uint32_t> head{0}, tail{0};   // events written, events read (both wrapping)
    std::atomic<uint32_t> dropped{0};
    std::atomic<bool> draining{false};        // a host thread is reading

    void push(const RawMidi& m) {
        const uint32_t h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) >= kSize) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ev[h % kSize] = m;
        head.store(h + 1, std::memory_order_release);
    }
};

Loader::SlotType tracedSourceSlot();   // Weather's source slot, its loads traced (below)

// MPC's tempo and position, as readTransport() found them for a block.
struct Transport {
    double bpm = 120.0, beats = 0.0;
    bool   playing = false, valid = false;
};

// The loader stopped and joined when it goes: the Plugin's last member, so it goes first, whether the
// Plugin is deleted or its constructor throws (the loader's thread runs from the loader's construction,
// and its listener, tick and jobs call into the surface and the engine).
struct LoaderStop {
    Loader& loader;
    ~LoaderStop() { loader.stop(); }
};

struct Plugin {
    AEffect             fx;          // must stay the first member: MPC hands us &fx back
    audioMasterCallback master = nullptr;
    // Weather's source (above): one slot. Before the surface, which asks it for sources; stopped and
    // joined (stopLoader, the last member) before anything its thread calls back into goes.
    Loader              loader{{tracedSourceSlot()}};
    Surface             surface{loader};
    std::atomic<long long> suspendMs{-1}, resumeMs{-1};
    // The trace's notes (above), and whether it is on, as the host's threads last found it.
    ResumeNote          resumeNote;
    MidiLog             midiLog;
    std::atomic<bool>   tracingOn{false};
    af::Engine          engine{af::sharedTables()};
    std::string         chunk;       // effGetChunk buffer: must outlive the call

    // audio thread only
    float    snapshot[P_COUNT] = {};
    bool     havePatch = false;   // patch below is built from snapshot
    uint32_t seenWrites = 0;      // the surface's write count that snapshot is current for
    bool     memoryOn = false;    // the engine's patch has Weather on Memory (the surface's memorySource())
    bool     rememberWait = false;   // a Remember given to the engine and not yet applied
    uint32_t rememberGen = 0;     // Memory's generation when it was given
    RawMidi  midi[kMaxMidi] = {};
    int      nMidi = 0;
    float    scratch[2][kScratch] = {};
    int      ppqOffset = 0;   // process(): this sub-block starts this many samples into the host's block
    Transport lastTransport;  // the last the host answered (readTransport, if a callback throws)

    // CPU meter: the audio thread sums its own CPU time against the real-time budget and
    // publishes twice a second; the status line is formatted on the UI thread.
    double           winUs = 0.0, winBudgetUs = 0.0, winPeak = 0.0;
    std::atomic<int> shownVoices{0}, shownAvg{0}, shownPeak{0};   // percent
    int              lastVoices = -1, lastAvg = -1, lastPeak = -1;

    LoaderStop       stopLoader{loader};   // the last member (above)

    Plugin();
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;
    void tick();                   // the loader's thread, every pass
    void keep();                   // the loader's thread, a job
};

Plugin* self(AEffect* e) { return static_cast<Plugin*>(e->object); }

// The trace never costs a host call its work: it allocates and locks, so it may throw, and what it
// throws stays here. Each host-side line names the thread (Phase 0: which of MPC's threads makes
// which call).
#define AF_TRACE_QUIETLY(...)   \
    do {                        \
        try {                   \
            trace(__VA_ARGS__); \
        } catch (...) {         \
        }                       \
    } while (0)

bool tracingQuietly() {
    try {
        return tracing();
    } catch (...) {
        return false;
    }
}

long threadId() { return static_cast<long>(syscall(SYS_gettid)); }

// --- Weather's source, Remember and Keep on the loader's thread ------------------------------------

// sources.h's slot, each load traced from the loader's thread: the key, what came of it, how long it
// took and how big the source is.
Loader::SlotType tracedSourceSlot() {
    Loader::SlotType t = sourceSlotType();
    t.load = [load = t.load](const std::string& key, std::string* err, int* info) {
        const auto t0 = std::chrono::steady_clock::now();
        std::shared_ptr<const void> obj = load(key, err, info);
        if (tracingQuietly()) {
            const long long ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            const size_t bytes = obj ? static_cast<const SourceBuffer*>(obj.get())->bytes() : 0;
            AF_TRACE_QUIETLY("[tid %ld] source %s: %s in %lld ms, %zu bytes", threadId(), key.c_str(),
                             obj ? "loaded" : err && !err->empty() ? err->c_str() : "failed", ms, bytes);
        }
        return obj;
    };
    return t;
}

Plugin::Plugin() {
    // The loader's thread: a load published (the stepper's text, Memory's flag); every pass, what the buttons
    // asked for. Then Weather's first source, Init's, as a scroll (debounced): the project MPC sets right after
    // it creates the instance names its own, and the default isn't made for nothing.
    loader.setListener([this](int, const std::string&, bool) { surface.sourceLoaded(); });
    loader.setTick([this] { tick(); });
    surface.setSourceKey(defaultSourceKey(), false);
}

// Every pass of the loader's thread: a Remember the audio thread saw happen (Weather already on Memory) sets
// the key memory: and the stepper's text, unless the player picked another source since; and a Keep pressed
// is posted from here (its job runs in this same pass), so it is never posted from a thread that may be the
// audio thread. Whatever throws is tried again at the next pass (the key is looked at anew; a Keep whose job
// couldn't be posted isn't left marked as writing).
void Plugin::tick() {
    surface.followRemember();
    if (surface.takeKeep()) {
        try {
            loader.post([this] { keep(); });
        } catch (...) {
            surface.keepDone();
            throw;
        }
    }
}

// Keep (sources.h keepMemory), a job on the loader's thread: what Memory remembered written to the SSD as the
// next Memory NNN.wav; the status line says which, or why not. The file becomes Weather's source only if the
// player is still on Memory and nothing newer has been remembered since the write began: the same audio,
// now by a key a project keeps. Otherwise what the player chose meanwhile (another source, a newer Remember)
// stays.
void Plugin::keep() {
    struct Done {   // however it ends, the next Keep may come
        Surface& s;
        ~Done() { s.keepDone(); }
    } done{surface};
    const void* instance = &fx;
    if (engine.memory().generation() == 0) {   // nothing remembered yet (once remembered, Memory keeps it)
        surface.say(MSG_KEEP_NOTHING);
        if (tracingQuietly()) AF_TRACE_QUIETLY("%p [tid %ld] keep: nothing remembered", instance, threadId());
        return;
    }
    std::string err;
    const uint32_t generation = engine.memory().generation();   // (a Remember before the copy: the file is newer, kept as it is)
    const std::string key = keepMemory(engine.memory(), &err);
    if (tracingQuietly())
        AF_TRACE_QUIETLY("%p [tid %ld] keep: %s%s", instance, threadId(), key.empty() ? "failed: " : "",
                         key.empty() ? err.c_str() : resolveKey(key, sourceRoots()).c_str());
    if (key.empty()) {
        surface.say(MSG_KEEP_FAILED);
        return;
    }
    const size_t space = key.rfind(' ');   // "ssd:AmbientForce/Memories/Memory 007.wav": 7
    surface.say(MSG_KEPT, space == std::string::npos ? 0u : static_cast<uint32_t>(std::strtoul(key.c_str() + space + 1, nullptr, 10)));
    surface.replaceSourceKey(kMemoryKey, key, [this, generation] { return engine.memory().generation() == generation; });
}

// The audio thread's note of a resume (runBlock).
void noteResume(Plugin* p, long long awayMs, bool told) {
    ResumeNote& n = p->resumeNote;
    const uint32_t s = n.seq.load(std::memory_order_relaxed);
    n.seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    n.awayMs.store(awayMs, std::memory_order_relaxed);
    n.told.store(told, std::memory_order_relaxed);
    n.seq.store(s + 2, std::memory_order_release);
}

// One MIDI event as the trace reads it: "midi ch 2 note-on 60 vel 100 @12" (@: its sample in the block).
void traceMidi(const void* fx, long tid, const RawMidi& m) {
    const int d1 = m.d1, d2 = m.d2;
    char what[48];
    switch (m.status & 0xF0) {
        case 0x80: std::snprintf(what, sizeof what, "note-off %d vel %d", d1, d2); break;
        case 0x90: std::snprintf(what, sizeof what, "note-on %d vel %d", d1, d2); break;
        case 0xA0: std::snprintf(what, sizeof what, "poly pressure %d %d", d1, d2); break;
        case 0xB0: std::snprintf(what, sizeof what, "cc %d %d", d1, d2); break;
        case 0xC0: std::snprintf(what, sizeof what, "program %d", d1); break;
        case 0xD0: std::snprintf(what, sizeof what, "pressure %d", d1); break;
        case 0xE0: std::snprintf(what, sizeof what, "pitch bend %d", ((d2 & 0x7F) << 7 | (d1 & 0x7F)) - 8192); break;
        default:
            trace("%p [tid %ld] midi %02X %d %d @%d", fx, tid, m.status, d1, d2, static_cast<int>(m.delta));
            return;
    }
    trace("%p [tid %ld] midi ch %d %s @%d", fx, tid, (m.status & 0x0F) + 1, what, static_cast<int>(m.delta));
}

// A host thread's call, after its own work: whether the trace is on (for the audio thread's next
// events), then what the audio thread left since the last call. Whatever the trace throws ends it
// here; the ring is left free for the next call (a scope guard), its events read so far gone.
void hostTrace(Plugin* p) {
    try {
        const bool on = tracing();
        p->tracingOn.store(on, std::memory_order_relaxed);
        const void* fx = &p->fx;
        const long tid = threadId();

        // MIDI: one reader at a time; another host thread at it now leaves it to that one.
        MidiLog& log = p->midiLog;
        if (!log.draining.exchange(true, std::memory_order_acquire)) {
            struct Unlock {
                std::atomic<bool>& flag;
                ~Unlock() { flag.store(false, std::memory_order_release); }
            } unlock{log.draining};
            const uint32_t h = log.head.load(std::memory_order_acquire);
            for (uint32_t t = log.tail.load(std::memory_order_relaxed); t != h; ++t) {
                const RawMidi m = log.ev[t % MidiLog::kSize];
                log.tail.store(t + 1, std::memory_order_release);
                if (on) traceMidi(fx, tid, m);
            }
            const uint32_t lost = log.dropped.exchange(0, std::memory_order_relaxed);
            if (on && lost) trace("%p [tid %ld] midi: %u more events dropped (the trace's ring was full)", fx, tid, lost);
        }

        // The resume.
        ResumeNote& n = p->resumeNote;
        const uint32_t s = n.seq.load(std::memory_order_acquire);
        uint32_t done = n.traced.load(std::memory_order_relaxed);
        if (s == done || (s & 1u)) return;   // nothing new, or being written: the next call
        const long long away = n.awayMs.load(std::memory_order_relaxed);
        const bool told = n.told.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (n.seq.load(std::memory_order_relaxed) != s) return;   // written over meanwhile: the next call
        if (!n.traced.compare_exchange_strong(done, s)) return;   // another host thread has it
        if (on)
            trace("%p [tid %ld] resumed %lld ms after the suspend (%s): %s", fx, tid, away,
                  told ? "the host said so" : "the first block after",
                  away >= 0 && static_cast<double>(away) * 1e-3 <= Engine::kStopWindowS ? "On Stop" : "reset");
    } catch (...) {
    }
}

// The host stopped processing (effMainsChanged 0, effStopProcess), or said it starts again
// (effMainsChanged 1, effStartProcess). The first suspend counts from when it came; a suspend after
// a resume the audio thread hasn't seen yet starts over (the latest one wins: a reset covers a
// Stop). What MPC sends on Stop is a Phase 0 question: the trace shows it.
// The suspend's length is read on the surface's clock (the tests' own, when they set one).
void suspended(Plugin* p) {
    const long long now = Surface::nowMs();
    if (p->suspendMs.load() < 0 || p->resumeMs.load() >= 0) {
        p->resumeMs.store(-1);
        p->suspendMs.store(now);
    }
    if (tracingQuietly()) AF_TRACE_QUIETLY("%p [tid %ld] suspend at %lld ms", static_cast<void*>(&p->fx), threadId(), now);
    hostTrace(p);
}
void resumed(Plugin* p) {
    const long long now = Surface::nowMs();
    if (p->suspendMs.load() >= 0 && p->resumeMs.load() < 0) p->resumeMs.store(now);
    if (tracingQuietly()) AF_TRACE_QUIETLY("%p [tid %ld] resume at %lld ms", static_cast<void*>(&p->fx), threadId(), now);
    hostTrace(p);
}

// Copies at most cap - 1 bytes, never cutting a UTF-8 character in half.
void copyStr(void* dst, const std::string& s, size_t cap) {
    if (!dst || cap == 0) return;
    size_t n = std::min(s.size(), cap - 1);
    if (n < s.size())
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // s[n] continues a character
    std::memcpy(dst, s.data(), n);
    static_cast<char*>(dst)[n] = 0;
}

// The status line: for a few seconds after a move, a preset load, a Remember or a Keep, what the surface says
// (the control's help line, the preset's description, what came of it: plugin/surface.h); else the voices and
// the CPU meter.
std::string statusText(const Plugin* p) {
    const int line = p->surface.statusLine();
    if (line >= 0 && line < P_COUNT && PARAM_INFO[line].help) return PARAM_INFO[line].help;
    if (line == Surface::kStatusAbout) return p->surface.aboutText();
    if (line == Surface::kStatusMessage) return p->surface.messageText();
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
        const bool traced = i >= 0 && i < P_COUNT && tracingQuietly();
        const float before = traced ? s.get(i) : 0.0f;   // what MPC last read back
        s.set(i, v);
        if (traced)
            AF_TRACE_QUIETLY("%p [tid %ld] set %3d %-18s %.4f  read %.4f -> %.4f  \"%s\"", static_cast<void*>(e), threadId(),
                             static_cast<int>(i), PARAM_INFO[i].key, static_cast<double>(v), static_cast<double>(before),
                             static_cast<double>(s.get(i)), s.display(i).c_str());
        hostTrace(self(e));
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

// MPC's tempo and bar position, for whatever in the engine follows them. A host callback, so in MPC's own
// FP mode. A callback that throws is caught here, so the block still renders whole (processReplacing:
// a block that read Weather's source must give it to Weather), on the transport as the host last gave it
// (a throw is no Stop).
Transport readTransport(Plugin* p) {
    Transport tr;
    if (!p->master) return tr;
    intptr_t r = 0;
    try {
        r = p->master(&p->fx, vst::audioMasterGetTime, 0, vst::kVstTempoValid | vst::kVstPpqPosValid, nullptr, 0.0f);
    } catch (...) {
        return p->lastTransport;
    }
    if (const VstTimeInfo* t = reinterpret_cast<const VstTimeInfo*>(r)) {
        // A NaN or absurd value would stall or spin anything synced to it: ignore it.
        if ((t->flags & vst::kVstTempoValid) && std::isfinite(t->tempo) && t->tempo >= 1.0 && t->tempo <= 1000.0) tr.bpm = t->tempo;
        tr.valid = (t->flags & vst::kVstPpqPosValid) != 0 && std::isfinite(t->ppqPos) && std::fabs(t->ppqPos) < 1e9;
        tr.beats = tr.valid ? t->ppqPos + p->ppqOffset * tr.bpm / 60.0 / static_cast<double>(kSampleRate) : 0.0;
        tr.playing = (t->flags & vst::kVstTransportPlaying) != 0;
    }
    p->lastTransport = tr;
    return tr;
}

void runBlock(Plugin* p, float* L, float* R, int n, const Transport& tr, const GrainSource* source) {
    // The sound only changes when a parameter does, or Weather's source moves to Memory or away: then
    // rebuild the patch and hand it to the engine. Looked at only when something was written since the
    // last look.
    const uint32_t writes = p->surface.writes();   // before the snapshot: a later write shows next block
    const bool memory = p->surface.memorySource();
    if (!p->havePatch || writes != p->seenWrites || memory != p->memoryOn) {
        float fresh[P_COUNT];
        if (p->surface.snapshot(fresh)) {   // mid-preset: false, look again next block
            p->seenWrites = writes;
            if (!p->havePatch || memory != p->memoryOn || std::memcmp(fresh, p->snapshot, sizeof fresh) != 0) {
                std::memcpy(p->snapshot, fresh, sizeof fresh);
                Patch patch = patchFromParams(p->snapshot);
                patch.weather.memory = memory;   // no knob's: the source's (plugin/surface.h)
                p->engine.setPatch(patch);
                p->memoryOn = memory;
                p->havePatch = true;
            }
        }
    }
    // Weather's source, from the loader (processReplacing), in every block: the engine gives it to Weather
    // unless Weather plays Memory.
    p->engine.setWeatherSource(source);
    // Remember, pressed since the last block: the engine applies it at its next control step (in this block,
    // awake; at once, asleep), and one that happens moves Weather onto Memory in that piece. What came of it
    // is said once it has.
    if (p->surface.takeRemember()) {
        if (!p->rememberWait) p->rememberGen = p->engine.memory().generation();
        p->rememberWait = true;
        p->engine.remember(true);
    }
    // Processing resumed after a suspend: the engine applies On Stop (back within 250 ms) or
    // resets. Without a resume from the host, this block is when it resumed.
    const long long went = p->suspendMs.exchange(-1);
    if (went >= 0) {
        long long back = p->resumeMs.exchange(-1);
        const bool told = back >= 0;
        if (!told) back = Surface::nowMs();
        noteResume(p, back - went, told);   // for the trace, written on a host thread
        p->engine.suspend();
        p->engine.resume(static_cast<double>(back - went) * 1e-3);
    }
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

    // A Remember applied: the status line says what came of it. One that happened has moved Weather onto
    // Memory (the engine's patch): the surface's flag says so from here, so the patches made after keep it,
    // and the loader's tick sets the key memory: (a string, under the surface's lock; followRemember()).
    if (p->rememberWait && !p->engine.remembering()) {
        p->rememberWait = false;
        const Memory& m = p->engine.memory();
        if (m.generation() != p->rememberGen) {
            const GrainSource* g = m.remembered();
            const uint64_t frames = g ? static_cast<uint64_t>(g->frames) : 0u;
            p->surface.say(MSG_REMEMBERED, static_cast<uint32_t>((frames * 10u + 22050u) / 44100u));   // tenths of a second
            p->surface.rememberedHere();
            p->memoryOn = true;
        } else {
            p->surface.say(m.refusal() == Memory::RF_EMPTY ? MSG_REMEMBER_EMPTY : MSG_REMEMBER_SOON);
        }
    }
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
    if (!out || !out[0] || !out[1] || n <= 0) return;   // no block started: none to end
    Plugin* p = self(e);
    const double t0 = threadCpuUs();
    // The block on the loader's count (above): from here exactly one blockDone(), whatever happens.
    p->loader.blockStart();
    const void* live = p->loader.live(0);   // read once, at the block's start
    bool holds = true;
    try {
        const Transport tr = readTransport(p);   // (a host's throw caught there: the block renders whole)
        FlushDenormals ftz;
        runBlock(p, out[0], out[1], n, tr, sourceOf(live));
        holds = p->engine.holdsSource();    // after the block's last render (Weather may let go in it)
    } catch (...) {   // nothing may throw into MPC: an escaping exception ends the whole process
        std::memset(out[0], 0, sizeof(float) * static_cast<size_t>(n));
        std::memset(out[1], 0, sizeof(float) * static_cast<size_t>(n));
        // Cut short, the block may not have given Weather the source it read: Weather lets go of all it
        // holds (silence(): its pointers dropped, nothing read), so nothing does once the block ends.
        p->engine.reset();
        holds = p->engine.holdsSource();
    }
    p->loader.blockDone(holds);
    try {   // the host's callbacks, after the block's end: a throw from MPC goes no further either
        p->surface.notify(hostAutomate, hostUpdate, p, n);
        meter(p, threadCpuUs() - t0, n);
    } catch (...) {
    }
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

// Audio thread (MPC may send events there): no trace, no allocation; the trace's ring takes every
// MIDI event, as it came, while the trace is on.
void onMidi(Plugin* p, const VstEvents* evs) {
    if (!evs) return;
    const bool logged = p->tracingOn.load(std::memory_order_relaxed);
    for (int32_t i = 0; i < evs->numEvents; ++i) {
        const VstEvent* ev = evs->events[i];
        if (!ev || ev->type != vst::kVstMidiType) continue;
        const auto* me = reinterpret_cast<const VstMidiEvent*>(ev);
        if (logged)
            p->midiLog.push({me->deltaFrames, static_cast<uint8_t>(me->midiData[0]), static_cast<uint8_t>(me->midiData[1]),
                             static_cast<uint8_t>(me->midiData[2])});
        if (p->nMidi >= kMaxMidi) continue;
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
            hostTrace(p);
            return 0;
        case vst::effSetSampleRate:   // MPC OS is fixed at 44.1 kHz; the engine is built for it
        case vst::effSetBlockSize: return 1;
        case vst::effMainsChanged:
            if (val == 0) suspended(p);
            else resumed(p);
            return 1;
        case vst::effStopProcess: suspended(p); return 0;
        case vst::effStartProcess: resumed(p); return 0;
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
    af::instanceOpened();   // the first starts the shared table builds; until effClose the tables stay
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
        af::sineTable();   // every slot's fallback, built here and never on the audio thread
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
