// From PolyForce plugin/loader.h (3ad2ac6), pf -> af. TableCache is SourceCache (Weather's sources, 48 MB)
// and loadTable is loadSource (plugin/sources.h). Changed: the graveyard's rule, which Weather's keeping
// a source between blocks asks more of (below); blockDone() says whether the audio side still holds
// one; post() runs a job on the worker (Keep), and setTick() has it poll every pass (Keep's button);
// an explicit pick of the key already loaded looks at it again; the cache checks a file's size and
// time; rounds() for tests.
#pragma once
// Background loading with a lock-free handoff to the audio thread.
//
// SourceCache  process-wide: key -> shared source, least recently used evicted beyond a memory cap,
//              never one still in use. The same source in two plugin instances is held once (two
//              first loads of one key at the same moment both do the work, and the cache keeps the first).
//              A file's source is kept with the file's size and time and is dropped when they change.
// Loader       one per plugin instance, its own worker thread, a few slots (Weather has one, the
//              source). Per slot:
//                UI thread   want(key)            what the user picked (debounced 150 ms, so
//                                                 scrolling through 50 sources loads one)
//                worker      loads, then publishes: live = new pointer, old one to the graveyard
//                audio       live(slot)           read once at block start
//              The audio thread calls blockStart() before and blockDone() after every block. No locks,
//              frees or allocation on the audio thread. A failed load publishes the slot's fallback
//              (nothing, for a source: live() is nullptr) but keeps the wanted key, so a saved
//              project keeps its reference. An explicit pick (now) of the key that is loaded loads it
//              again, and the load function decides whether anything changed: a source whose file
//              was replaced is swapped in, one that is the same object (the cache found it) is not
//              swapped, and a file that has gone makes the slot Missing. A scroll (not now) over
//              the loaded key does nothing.
//
// When a replaced object is freed. This is the whole of the argument; loader.cpp keeps to it.
//
// What the audio side keeps between blocks matters. PolyForce's oscillators read their table pointer
// anew in each block and kept nothing, so it freed a replaced table once one block had ended since the
// swap, or at once while none was running. Weather keeps its source: its grains point into it from one
// block to the next while it is audible(), and the first render() it is given another source in (or
// none) copies from the old one what each grain still has to read, and only then lets go (dsp/weather.h,
// "A source change"). So the old source is read once more, by the first block that sees the new one.
// PolyForce's rule frees it before that block when the track isn't being processed at the swap (MPC
// may stop calling for a long while), or when the block in progress at the swap ends first.
//
// The audio thread, in each block:   blockStart();  p = live(slot);  ...;  blockDone(holds);
//   p is read once, at the start, and is what the engine gives Weather in that block, in every block in
//   which Weather is audible (the engine renders what is audible).
//   holds says whether anything still points into an object from live() once the block is over. For
//   Weather that is audible(): when it isn't, Weather has let go of its source (silence(), dsp/weather.h)
//   and nothing else keeps one. blockDone() with no argument says true, the safe answer.
//
// The worker swaps at S (the store to live) and at once reads N, the number of blockStart() calls so
// far. A block that began before that read is one of the first N; one that began after it is number
// N + 1 or later, and its live() comes after S, so it sees the new object. So the first block to see the
// new object is one of the first N + 1 at the latest, and no block after that one reads the old object:
// Weather copied what it needed in that block, and holds nothing of it after. The worker frees the old
// object when (graveMayFree below)
//   (a) N + 1 blocks have ended (they end one at a time, in order, so each of those has); or
//   (b) none is running (as many have ended as started) and the last blockDone() said holds == false:
//       nothing is held, and a block that starts after the worker read the counts sees the new object.
// So a block in progress at the swap is waited for, and the block after it too (the worker cannot tell
// which of the first N had read the old pointer), and a caller that never says holds == false is only
// ever in case (a): safe, but then a replaced source stays in memory for as long as no block runs.
//
// And at the swap itself, an object no block can have seen is freed at once (graveNeverSeen below): the
// worker reads the counts before it stores each new pointer (S0 started, then E0 ended), and when it swaps
// that object out again it reads N, the started count after that store. If S0 == E0 == N, no block was
// running as the pointer went in and none started until it went out again, so none can have read it
// (a block reads live() after it starts). That is the track that isn't being processed while the user
// scrolls through the sources: each one picked is freed as the next goes in, not kept until a block has
// run (a source is up to 18.5 MB, outside the cache's cap).
//
// All the counters are sequentially consistent atomics, 64 bits (a count of 32 bits would wrap after 144
// days at 344 blocks a second, and an installation is left on). A block's reads of an object come before
// its blockDone() (the holds flag, then the increment of the ended count), and the worker frees after
// it has read that increment, or a later flag, so a free never races a read; and a block whose
// blockStart() follows the worker's read of the counts follows the store to live in the total order, so
// its live() is the new pointer. The audio thread's share is four atomic operations a block (the start,
// the load of live(), the flag and the increment at the end): no lock, no allocation, no free.
//
// The rule has been checked against every interleaving of the audio thread (a block is six steps) with
// swaps and checks of the worker, by a model in test/sources_test.cpp that calls the two functions
// below; PolyForce's rule, and the never-seen rule without its starting counts, fail it.
//
// A swap leaves the slot as it was if anything in it fails: everything that can throw (the graveyard's
// room, the copy of the key, the error text, the listener's copy) is done before the store to live,
// and nothing after it can.
//
// Jobs. post() runs a function on the worker thread, in order with the loads' rounds: Keep's write
// of 16 s of audio to the SSD, which must not run on the audio or UI thread. A job may throw: it is
// dropped. Jobs still queued when stop() is called are dropped, and none runs after it returns. What
// a job, a load function or a listener captured is destroyed off the loader's lock.
//
// The tick. setTick() gives the worker a function it calls at the start of every pass (about every
// 20 ms, sooner after a want() or a post()), outside the lock, before that pass's jobs: a poll for
// what a thread that may not post() has asked for in an atomic. The plugin's Keep is one: its
// button arrives through setParameter, which a host may call on the audio thread, where post()'s
// lock and allocation may not happen; the tick sees the request and posts Keep's job, which runs in
// the same pass. The tick may post() and want(); it is never called after stop() returns, and a
// throw is ignored.
#include "../dsp/grainsrc.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace af {

static_assert(std::atomic<uint64_t>::is_always_lock_free, "the block counts are read and written by the audio thread");

// The graveyard's rules as functions of the counters. `started` and `ended` are the counts of blockStart()
// and blockDone() calls, `holds` the last blockDone()'s flag, `n` the started count read right after the
// swap that took the object out.
inline bool graveMayFree(uint64_t started, uint64_t ended, bool holds, uint64_t n) {
    return (started == ended && !holds) || ended >= n + 1;
}
// An object put in with `s0` blocks started and `e0` ended before the store, and taken out with `n`
// started after the store that took it out: no block can have read it.
inline bool graveNeverSeen(uint64_t s0, uint64_t e0, uint64_t n) { return s0 == e0 && n == s0; }

// A file's size and modification time (ns): what a cached source was made from.
struct FileStamp {
    bool valid = false;
    uint64_t size = 0;
    int64_t mtimeNs = 0;
    bool operator==(const FileStamp& o) const { return valid == o.valid && size == o.size && mtimeNs == o.mtimeNs; }
};

class SourceCache {
public:
    static SourceCache& get();
    // With a stamp: a cached source made from a file of another size or time is dropped from the cache
    // (whoever holds it goes on playing it) and is not found; a stamp that is not valid (the file is gone)
    // drops it too. Without one, whatever is cached.
    std::shared_ptr<const SourceBuffer> find(const std::string& key, const FileStamp* stamp = nullptr);
    // Inserts unless already there; returns the cached copy (a racing second load keeps the first).
    // The one returned is in use (the caller holds it), so a source over the cap by itself stays.
    // Nothing is changed if it throws.
    std::shared_ptr<const SourceBuffer> put(const std::string& key, std::shared_ptr<const SourceBuffer> s,
                                            const FileStamp& stamp = {});
    size_t bytes() const;
    size_t entries() const;
    void setCap(size_t bytes);   // default 48 MB (about two minutes of stereo: two 60 s WAVs)
    void clear();                // tests

private:
    void evict();   // with mtx_ held
    struct Item {
        std::shared_ptr<const SourceBuffer> source;
        std::list<std::string>::iterator lru;
        FileStamp stamp;
    };
    mutable std::mutex mtx_;
    std::map<std::string, Item> items_;
    std::list<std::string> lru_;   // front = most recently used
    size_t bytes_ = 0;
    size_t cap_ = 48u << 20;
};

class Loader {
public:
    // Loads `key` (worker thread). `info` gets something to show (frames); null = failed.
    using LoadFn = std::function<std::shared_ptr<const void>(const std::string& key, std::string* err, int* info)>;
    struct SlotType {
        LoadFn load;
        std::string fallbackKey;   // "": nothing (live() is nullptr until something loads)
    };
    enum State : int { Ready, Loading, Missing };
    struct View {
        std::string key;         // wanted
        std::string loadedKey;   // what live() holds (the fallback's key when missing)
        State state = Ready;
        int info = 0;
        std::string error;
        long long ageMs = 0;     // since the last publish (how long MISSING has been showing)
    };
    // slot, key: called on the worker after every publish (not under any lock).
    using Listener = std::function<void(int slot, const std::string& key, bool ok)>;
    using Job = std::function<void()>;

    explicit Loader(std::vector<SlotType> types);
    ~Loader();
    void stop();   // joins the worker; the listener is never called, and no job run, after this returns
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    void setListener(Listener l);   // before the first want()
    void setTick(Job tick);         // the worker's poll, every pass (above)
    // now: skip the debounce, and look again at a key that is loaded or failed (a pick, not a scroll).
    void want(int slot, const std::string& key, bool now);
    std::string wanted(int slot) const;
    View view(int slot) const;
    bool busy() const;              // some slot isn't at its wanted key yet
    void post(Job job);             // any thread but the audio thread; runs on the worker
    int loads() const { return loads_.load(); }     // completed loads (tests)
    int rounds() const { return rounds_.load(); }   // passes the worker has made (tests)

    const void* live(int slot) const { return slots_[static_cast<size_t>(slot)].live.load(std::memory_order_seq_cst); }
    void blockStart() { started_.fetch_add(1, std::memory_order_seq_cst); }
    void blockDone(bool holds = true) {
        holds_.store(holds, std::memory_order_seq_cst);
        ended_.fetch_add(1, std::memory_order_seq_cst);
    }

    static constexpr int kDebounceMs = 150;
    // The worker's name (/proc's comm, top's): the bench waits for the table builder to go by counting
    // threads, and an instance's loader, which lives as long as the instance, isn't the builder.
    static constexpr const char* kThreadName = "af-loader";

private:
    // The counts before the store that put the slot's object in (see graveNeverSeen).
    struct Seen {
        uint64_t s0 = 0, e0 = 0;
    };
    struct Slot {
        SlotType type;
        std::string want, loadedKey, error;
        std::chrono::steady_clock::time_point wantAt{}, doneAt{};
        bool now = false, missing = false, reload = false;
        int info = 0;
        std::shared_ptr<const void> owned;
        Seen seen;
        std::atomic<const void*> live{nullptr};
    };
    struct Grave {
        std::shared_ptr<const void> obj;
        uint64_t started;   // blocks started, counted right after the swap
    };
    using Dead = std::vector<std::shared_ptr<const void>>;
    void run();
    // mtx_ held. Throws, if at all, before anything is changed. An object no block can have seen goes
    // to `dead` (to be freed off the lock), the rest to the graveyard.
    void publish(Slot& s, std::shared_ptr<const void> obj, const std::string& loadedKey, Dead& dead);

    std::vector<Slot> slots_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool quit_ = false, kick_ = false;
    std::vector<Grave> graveyard_;
    std::deque<Job> jobs_;
    std::atomic<uint64_t> started_{0}, ended_{0};
    std::atomic<bool> holds_{false};
    std::atomic<int> loads_{0}, rounds_{0};
    Listener listener_;
    Job tick_;
    std::thread thread_;
};

} // namespace af
