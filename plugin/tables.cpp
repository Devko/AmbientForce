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

// The builder thread, owned by a static so that it is stopped and joined before this code goes
// away: MPC unloads the plugin when its last instance is removed, and a detached thread still
// building would then run on in unmapped code. The static's destructor runs at that unload (and
// at process exit); the builder looks at `stop` between tables, so the wait is at most one build.
struct Builder {
    std::mutex mtx;   // one start at a time
    std::thread thread;
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};

    ~Builder() {
        stop.store(true, std::memory_order_relaxed);
        if (thread.joinable()) thread.join();
    }
};

Builder& builder() {
    static Builder b;
    return b;
}

// In id order, each table published as soon as it is built. One that can't be built (out of
// memory) leaves its slot on the sine. Nothing may escape a thread: that would end MPC.
void run(Builder* b) {
    TableSet& set = sharedTables();
    for (int id = 0; id < TB_COUNT && !b->stop.load(std::memory_order_relaxed); ++id) {
        try {
            std::unique_ptr<Wavetable> t(new Wavetable);
            if (buildTable(id, *t)) set.t[id].store(t.release(), std::memory_order_release);
        } catch (...) {
            trace("tables: could not build %s, its slots play the sine", tableName(id));
        }
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
    } catch (const std::system_error& e) {
        trace("tables: no builder thread (%s), every slot plays the sine", e.what());
    } catch (...) {
        trace("tables: no builder thread, every slot plays the sine");
    }
}

} // namespace af
