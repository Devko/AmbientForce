// From PolyForce plugin/loader.cpp (3ad2ac6), pf -> af: see loader.h for what changed.
#include "loader.h"

#include <algorithm>

namespace af {

// --- cache ------------------------------------------------------------------------------------

SourceCache& SourceCache::get() {
    static SourceCache c;
    return c;
}

std::shared_ptr<const SourceBuffer> SourceCache::find(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = items_.find(key);
    if (it == items_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return it->second.source;
}

std::shared_ptr<const SourceBuffer> SourceCache::put(const std::string& key, std::shared_ptr<const SourceBuffer> s) {
    if (!s) return nullptr;
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = items_.find(key);
    if (it != items_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return it->second.source;
    }
    lru_.push_front(key);
    bytes_ += s->bytes();
    items_.emplace(key, Item{s, lru_.begin()});
    evict();
    return s;
}

void SourceCache::evict() {
    auto it = lru_.end();
    while (bytes_ > cap_ && it != lru_.begin()) {
        --it;
        auto found = items_.find(*it);
        if (found->second.source.use_count() > 1) continue;   // someone plays it
        bytes_ -= found->second.source->bytes();
        items_.erase(found);
        it = lru_.erase(it);
    }
}

size_t SourceCache::bytes() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return bytes_;
}

size_t SourceCache::entries() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return items_.size();
}

void SourceCache::setCap(size_t b) {
    std::lock_guard<std::mutex> lk(mtx_);
    cap_ = b;
    evict();
}

void SourceCache::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    items_.clear();
    lru_.clear();
    bytes_ = 0;
}

// --- loader -----------------------------------------------------------------------------------

Loader::Loader(std::vector<SlotType> types) : slots_(types.size()) {
    for (size_t i = 0; i < types.size(); ++i) {
        Slot& s = slots_[i];
        s.type = std::move(types[i]);
        s.want = s.type.fallbackKey;
        int info = 0;
        std::string err;
        // The fallbacks are built-ins (no I/O): ready before the first block.
        auto obj = s.type.fallbackKey.empty() ? nullptr : s.type.load(s.type.fallbackKey, &err, &info);
        s.owned = obj;
        s.info = info;
        s.loadedKey = s.type.fallbackKey;
        s.live.store(obj.get(), std::memory_order_release);
    }
    thread_ = std::thread([this] { run(); });
}

Loader::~Loader() { stop(); }

void Loader::stop() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Loader::setListener(Listener l) {
    std::lock_guard<std::mutex> lk(mtx_);
    listener_ = std::move(l);
}

void Loader::want(int slot, const std::string& key, bool now) {
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Slot& s = slots_[static_cast<size_t>(slot)];
        const std::string k = key.empty() ? s.type.fallbackKey : key;
        // Already wanted: nothing to do. A failed key is retried only on an explicit pick
        // (now), never by a scroll passing over it.
        if (s.want == k && (!s.missing || !now)) return;
        s.want = k;
        s.wantAt = std::chrono::steady_clock::now();
        s.now = now;
        s.missing = false;
        kick_ = true;
    }
    cv_.notify_all();
}

void Loader::post(Job job) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        jobs_.push_back(std::move(job));
        kick_ = true;
    }
    cv_.notify_all();
}

std::string Loader::wanted(int slot) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return slots_[static_cast<size_t>(slot)].want;
}

Loader::View Loader::view(int slot) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Slot& s = slots_[static_cast<size_t>(slot)];
    View v;
    v.key = s.want;
    v.loadedKey = s.loadedKey;
    v.info = s.info;
    v.error = s.error;
    v.state = s.missing ? Missing : (s.want != s.loadedKey ? Loading : Ready);
    v.ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s.doneAt).count();
    return v;
}

bool Loader::busy() const {
    std::lock_guard<std::mutex> lk(mtx_);
    for (const Slot& s : slots_)
        if (!s.missing && s.want != s.loadedKey) return true;
    return false;
}

void Loader::publish(Slot& s, std::shared_ptr<const void> obj, const std::string& loadedKey) {
    std::shared_ptr<const void> old = std::move(s.owned);
    s.owned = std::move(obj);
    s.loadedKey = loadedKey;
    s.live.store(s.owned.get(), std::memory_order_seq_cst);
    // The count of blocks started is read AFTER the swap: every block that can have loaded the old
    // pointer is in it (loader.h).
    if (old) graveyard_.push_back({std::move(old), started_.load(std::memory_order_seq_cst)});
}

void Loader::run() {
    std::unique_lock<std::mutex> lk(mtx_);
    while (!quit_) {
        // Nothing may leave this thread (std::terminate would end MPC): an out-of-memory
        // here is a round skipped; the next one, 20 ms on, tries again.
        try {
            cv_.wait_for(lk, std::chrono::milliseconds(20), [this] { return quit_ || kick_; });
            kick_ = false;
            if (quit_) break;

            // Jobs first, outside the lock, one at a time (a job may post another).
            while (!jobs_.empty() && !quit_) {
                Job job = std::move(jobs_.front());
                jobs_.pop_front();
                lk.unlock();
                try {
                    job();
                } catch (...) {
                }
                job = nullptr;   // what it captured goes here, off the lock
                lk.lock();
            }
            if (quit_) break;

            // Free what no block can still be reading (loader.h): the blocks started when the object
            // was swapped out, and one more, have ended; or none is running and the audio side holds
            // nothing. Counts read in this order: started, then ended, then holds.
            const uint64_t started = started_.load(std::memory_order_seq_cst);
            const uint64_t ended = ended_.load(std::memory_order_seq_cst);
            const bool idle = started == ended && !holds_.load(std::memory_order_seq_cst);
            std::vector<std::shared_ptr<const void>> dead;
            graveyard_.erase(std::remove_if(graveyard_.begin(), graveyard_.end(),
                                            [&](Grave& g) {
                                                if (!idle && ended < g.started + 1) return false;
                                                dead.push_back(std::move(g.obj));
                                                return true;
                                            }),
                             graveyard_.end());

            const auto now = std::chrono::steady_clock::now();
            for (size_t i = 0; i < slots_.size(); ++i) {
                Slot& s = slots_[i];
                if (s.want == s.loadedKey || s.missing) continue;
                if (!s.now && now - s.wantAt < std::chrono::milliseconds(kDebounceMs)) continue;
                const std::string key = s.want;
                const LoadFn load = s.type.load;
                const std::string fallback = s.type.fallbackKey;
                lk.unlock();
                dead.clear();   // free outside the lock too
                std::string err;
                int info = 0;
                std::shared_ptr<const void> obj;
                try {   // an exception here would end MPC (std::terminate): a throw is a failed load
                    obj = load(key, &err, &info);
                } catch (...) {
                    obj = nullptr;
                    err = "could not load";
                }
                bool ok = obj != nullptr;
                if (!ok && !fallback.empty()) {
                    std::string ignored;
                    try {
                        obj = load(fallback, &ignored, &info);
                    } catch (...) {
                        obj = nullptr;
                    }
                }
                lk.lock();
                if (s.want != key) {   // picked something else meanwhile: next round loads that
                    dead.push_back(std::move(obj));   // (freed off the lock, at the next unlock)
                    continue;
                }
                publish(s, std::move(obj), ok ? key : fallback);
                s.missing = !ok;
                s.error = ok ? "" : err;
                s.info = info;
                s.doneAt = std::chrono::steady_clock::now();
                loads_.fetch_add(1);
                Listener l = listener_;
                lk.unlock();
                try {
                    if (l) l(static_cast<int>(i), key, ok);
                } catch (...) {
                }
                lk.lock();
            }
            lk.unlock();
            dead.clear();
            lk.lock();
            rounds_.fetch_add(1);
        } catch (...) {
            if (!lk.owns_lock()) lk.lock();
        }
    }
    graveyard_.clear();   // after the audio thread is gone (effClose joins us first)
    jobs_.clear();        // never run: what they captured goes here, on the worker
}

} // namespace af
