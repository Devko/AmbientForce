// The process-wide table library and its builder thread (tables.h).
#include "tables.h"
#include "trace.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

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
    std::mutex mtx;   // one start or release at a time
    std::thread thread;
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};

    ~Builder();
};

Builder& builder() {
    static Builder b;
    return b;
}

std::atomic<int> live{0};   // plugin instances alive

// In id order, each table published as soon as it is built; one already published (kept by a
// release while instances were alive) stays as it is, since it may be being read. One that can't
// be built (out of memory) leaves its slot on the sine. Nothing may escape a thread: that would
// end MPC (std::terminate), so even the trace of a failure has its own catch: it allocates and
// locks, and may throw too when memory is short.
void run(Builder* b) {
    TableSet& set = sharedTables();
    for (int id = 0; id < TB_COUNT && !b->stop.load(std::memory_order_relaxed); ++id) {
        if (set.t[id].load(std::memory_order_acquire)) continue;
        try {
            std::unique_ptr<Wavetable> t(new Wavetable);
            if (buildTable(id, *t, &b->stop)) set.t[id].store(t.release(), std::memory_order_release);
        } catch (...) {
            try {
                trace("tables: could not build %s, its slots play the sine", tableName(id));
            } catch (...) {
            }
        }
    }
}

// The builder always stops. The tables are freed only with no instance alive, each slot back to
// the sine before its table goes: whatever reads a slot afterwards gets the sine, never a freed
// table.
void release(Builder& b) {
    std::lock_guard<std::mutex> lk(b.mtx);
    b.stop.store(true, std::memory_order_relaxed);
    if (b.thread.joinable()) b.thread.join();
    if (live.load(std::memory_order_acquire) == 0)
        for (auto& slot : sharedTables().t) delete slot.exchange(nullptr, std::memory_order_acq_rel);
    b.stop.store(false, std::memory_order_relaxed);
    b.started.store(false, std::memory_order_release);
}

Builder::~Builder() {
    try {
        release(*this);
    } catch (...) {   // a lock or join that fails: leave the tables rather than throw into MPC
    }
}

} // namespace

TableSet& sharedTables() {
    static TableSet set;   // atomics only: nothing to destroy at exit, while an audio thread may still read
    return set;
}

void ensureTablesBuilding() {
    try {
        Builder& b = builder();
        if (b.started.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lk(b.mtx);
        if (b.started.load(std::memory_order_relaxed)) return;
        sineTable();   // the fallback, built here and never on the audio thread
        b.thread = std::thread(run, &b);
        b.started.store(true, std::memory_order_release);
    } catch (const std::system_error& e) {   // the traces in these catches have their own (run() says why)
        try {
            trace("tables: no builder thread (%s), every slot plays the sine", e.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            trace("tables: no builder thread, every slot plays the sine");
        } catch (...) {
        }
    }
}

void releaseTables() {
    try {
        release(builder());
    } catch (...) {
        try {
            trace("tables: could not stop the builder");
        } catch (...) {
        }
    }
}

void instanceOpened() { live.fetch_add(1, std::memory_order_acq_rel); }
void instanceClosed() { live.fetch_sub(1, std::memory_order_acq_rel); }
int liveInstances() { return live.load(std::memory_order_acquire); }

} // namespace af
