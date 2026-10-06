// The process-wide table library and its builder thread (tables.h).
#include "tables.h"
#include "trace.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace af {
namespace {

// The builder thread, owned by a static whose destructor runs when MPC unloads the plugin (when
// its last instance is removed) and at process exit. It stops and joins the builder first: a
// detached thread still building would run on in unmapped code. Then, with no instance alive, it
// frees the tables, so removing the last instance and inserting one again (a project change)
// doesn't leave ~38 MB behind each time. A host that exits while an instance still renders
// keeps its tables to the end: the OS takes them back. The builder looks at `stop` between pairs
// of frames, so the wait is a few ms.
//
// The unload itself was tried by hand: a loader that dlopens build/ambientforce.so, makes and
// closes an instance and dlcloses it, mid-build (dlclose took 1 ms) and after all tables were
// built (2 ms; resident memory flat over four such cycles). The tests link the plugin
// statically, so they go through releaseTables() instead.
struct Builder {
    std::mutex mtx;            // the count, every start and every release: one at a time
    std::thread thread;
    bool started = false;      // under mtx
    bool closed = false;       // under mtx: torn down (unload, exit), nothing starts again
    int live = 0;              // under mtx: plugin instances alive
    std::atomic<bool> stop{false};

    ~Builder();
};

Builder& builder() {
    static Builder b;
    return b;
}

// Traces from a catch: trace() allocates and locks, so it may throw too when memory is short, and
// nothing may escape into MPC (from the builder thread that would be std::terminate).
#define AF_TRACE_QUIETLY(...)   \
    do {                        \
        try {                   \
            trace(__VA_ARGS__); \
        } catch (...) {         \
        }                       \
    } while (0)

// In id order, each table published as soon as it is built; one already published (kept by a
// release while instances were alive) stays as it is, since it may be being read. One that can't
// be built (out of memory) leaves its slot on the sine.
void run(Builder* b) {
    // Nice 10, this thread only (on Linux a thread is a task of its own): the builds take a core for
    // seconds at every load of the plugin (4-5 s under emulation), on the cores the UI runs on.
    // MPC's audio workers are SCHED_FIFO on cores of their own anyway. If it fails: normal priority.
    (void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
    TableSet& set = sharedTables();
    for (int id = 0; id < TB_COUNT && !b->stop.load(std::memory_order_relaxed); ++id) {
        if (set.t[id].load(std::memory_order_acquire)) continue;
        try {
            std::unique_ptr<Wavetable> t(new Wavetable);
            if (buildTable(id, *t, &b->stop)) set.t[id].store(t.release(), std::memory_order_release);
        } catch (...) {
            AF_TRACE_QUIETLY("tables: could not build %s, its slots play the sine", tableName(id));
        }
    }
}

// With b.mtx held. Never throws.
void startLocked(Builder& b) {
    if (b.started || b.closed) return;
    try {
        sineTable();   // the fallback: VSTPluginMain built it already, a test's call may be the first
        b.thread = std::thread(run, &b);
        b.started = true;
    } catch (const std::system_error& e) {
        AF_TRACE_QUIETLY("tables: no builder thread (%s), every slot plays the sine", e.what());
    } catch (...) {
        AF_TRACE_QUIETLY("tables: no builder thread, every slot plays the sine");
    }
}

// With b.mtx held. The builder always stops. The tables are freed only with no instance alive,
// each slot back to the sine before its table goes: whatever reads a slot afterwards gets the
// sine, never a freed table.
void releaseLocked(Builder& b) {
    b.stop.store(true, std::memory_order_relaxed);
    if (b.thread.joinable()) b.thread.join();
    if (b.live == 0)
        for (auto& slot : sharedTables().t) delete slot.exchange(nullptr, std::memory_order_acq_rel);
    b.stop.store(false, std::memory_order_relaxed);
    b.started = false;
}

Builder::~Builder() {
    try {
        std::lock_guard<std::mutex> lk(mtx);
        closed = true;
        releaseLocked(*this);
    } catch (...) {   // a join that fails: let the thread go rather than std::terminate in MPC
        if (thread.joinable()) thread.detach();
    }
}

} // namespace

TableSet& sharedTables() {
    static TableSet set;   // atomics only: nothing to destroy at exit, while an audio thread may still read
    return set;
}

void instanceOpened() {
    try {
        Builder& b = builder();
        std::lock_guard<std::mutex> lk(b.mtx);
        if (b.closed) return;
        ++b.live;
        startLocked(b);
    } catch (...) {
        AF_TRACE_QUIETLY("tables: could not count an instance");
    }
}

void instanceClosed() {
    try {
        Builder& b = builder();
        std::lock_guard<std::mutex> lk(b.mtx);
        if (b.live > 0) --b.live;
    } catch (...) {
        AF_TRACE_QUIETLY("tables: could not count an instance");
    }
}

int liveInstances() {
    try {
        Builder& b = builder();
        std::lock_guard<std::mutex> lk(b.mtx);
        return b.live;
    } catch (...) {
        return -1;
    }
}

void ensureTablesBuilding() {
    try {
        Builder& b = builder();
        std::lock_guard<std::mutex> lk(b.mtx);
        startLocked(b);
    } catch (...) {
        AF_TRACE_QUIETLY("tables: no builder thread, every slot plays the sine");
    }
}

void releaseTables() {
    try {
        Builder& b = builder();
        std::lock_guard<std::mutex> lk(b.mtx);
        releaseLocked(b);
    } catch (...) {
        AF_TRACE_QUIETLY("tables: could not stop the builder");
    }
}

} // namespace af
