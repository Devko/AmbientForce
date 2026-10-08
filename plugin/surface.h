// From SubForce plugin/surface.h (8846421), namespace sf -> af; without RANDOMIZE.
#pragma once
// Surface: what every plugin parameter does when MPC sets it, what it reads back, and what
// its value text says. PolyForce's plugin/surface.{h,cpp} (itself RackForce's, device-proven),
// without the wavetable loader: AmbientForce browses presets only.
//
// Threads:
//   UI thread      get / set / display (MPC polls these freely; they read caches)
//   UI thread      refresh(): recompute the plugin-owned values (stepper, tiles) and texts;
//                  called after every set
//   audio thread   notify(): tell MPC what changed underneath it (audioMasterAutomate for
//                  values, audioMasterUpdateDisplay for text) and snapshot() the sound values.
//                  No allocation, no locks.
//
// MPC sends values rounded to 1/1000 as "what it last read back + a delta": a Q-Link detent
// is 1/128 of the range, a data-wheel click 0.01, a touch drag about 0.04, and a tile tap
// sends a release echo ~0.7 s later. So every stepped parameter (choices, small whole
// numbers, the preset stepper) moves exactly one step per event in MPC's direction, and the
// plugin pushes the snapped value back (stepIndex / stepItem below, RackForce's rules). The
// read-back is the base of every value, within a turn too: a stepper measures each detent from
// its own value, never from MPC's previous one. A button tap toggles its read-back, and a button
// always reads back 0 (it springs back): every 1 is a press, and no release ever follows.
//
// The status line (parameter 0) says what was just done: for kHelpS after a control is moved, its
// help line (surface.py "help"); for kAboutS after a preset loads, its name and description. A
// move is a set from MPC that changes the control's value by more than MPC's own rounding to
// 1/1000 (MPC echoing a value back, even rounded, changes nothing; preset loads and project states
// write through setValue, not set). The UI thread only notes the last move and counts the loads
// (atomics); notify() times them on the audio thread's sample count, decides which line shows
// (another control takes the line only once the one shown has had kHoldS, so automation moving
// several at once doesn't flicker) and pushes audioMasterUpdateDisplay when it changes.
// statusLine() reads what it decided.
#include "library.h"
#include "param_ids.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace af {

class Surface {
public:
    using AutomateFn = void (*)(void* ctx, int index, float value);
    using UpdateFn   = void (*)(void* ctx);

    Surface();

    // --- UI thread ---------------------------------------------------------------
    float       get(int i) const;
    void        set(int i, float n);
    std::string display(int i) const;
    bool        automatable(int i) const;

    // State load: values as saved (no stepping), pushed to MPC by the next blocks.
    void        setValue(int i, float n);
    std::string presetKey() const;         // the preset this sound came from ("" = none)
    void        setPresetKey(const std::string& key);
    void        refresh();

    // Many values changed as one (a preset, a project): the audio thread keeps the
    // previous sound until the batch is complete. Nests; UI thread.
    void        beginBatch();
    void        endBatch();
    struct Batch {
        explicit Batch(Surface& s) : s_(s) { s_.beginBatch(); }
        ~Batch() { s_.endBatch(); }
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        Surface& s_;
    };

    // What the status line shows (UI thread, a cache): a parameter's index (its help line,
    // PARAM_INFO[i].help), kStatusAbout (aboutText()) or kStatusPlugin (the plugin's own line).
    static constexpr int kStatusPlugin = -1;
    static constexpr int kStatusAbout = -2;
    static constexpr double kHelpS = 4.0;    // a help line shows this long after the control's last move
    static constexpr double kAboutS = 6.0;   // a preset's description this long after it loads
    static constexpr double kHoldS = 0.5;    // the least a line shows before another control's takes over
    int         statusLine() const { return status_.load(std::memory_order_acquire); }
    std::string aboutText() const;           // "NAME: its description" (or "NAME") of the last preset loaded

    // --- audio thread ------------------------------------------------------------
    // Once per block of `frames` samples (the status line's clock).
    void        notify(AutomateFn automate, UpdateFn update, void* ctx, int frames);
    // Every parameter's current 0..1 value. False (and `out` untouched) while a batch is
    // being written: keep using the previous snapshot.
    bool        snapshot(float* out) const;
    // Moves on every value write: unchanged since a snapshot, the snapshot is still current.
    uint32_t    writes() const { return changes_.load(std::memory_order_acquire); }

    void        seed(uint32_t s) { rng_ = s ? s : 1u; }   // RND's random numbers

    static int  kFine;   // ranges with this many steps or more follow MPC's value
    // Milliseconds for telling gestures apart (null: the steady clock). Tests set one that only
    // moves when they say, so stepping doesn't depend on how fast the machine is.
    static long long (*clock)();
    static long long nowMs();   // clock(), or the steady clock when none is set (the plugin's suspends read it too)

private:
    struct Category {
        std::string name;
        std::vector<std::string> keys;
    };
    void apply(int i, float n);
    int  stepIndex(int i, float n, int count, int cur);
    int  stepItem(float n, int normRange, int items, int cur);   // one item per event
    bool toggleBounce(int i, bool on);
    void browserAction(int i);
    // FAVORITES, RECENT, then the library's categories (from L, the listing in use).
    std::vector<Category> categories(const Listing& L) const;
    void loadPreset(const std::string& key);
    void savePreset();
    void touch(int i);   // the player moved control i: its help line, if it has one
    void tickStatus(int frames);   // audio thread: which line the status shows (above)
    int  stepperCur(int i, const Listing& L, const std::string& key) const;   // where a stepper stands

    // UI-thread-only stepping state (RackForce's).
    long long lastSentMs_[P_COUNT] = {};
    float     lastN_[P_COUNT] = {};
    long long toggleMs_[P_COUNT] = {};
    bool      toggleOn_[P_COUNT] = {};

    // What each parameter should read vs what MPC last saw.
    std::atomic<float>    want_[P_COUNT];
    std::atomic<float>    shown_[P_COUNT];
    std::atomic<bool>     release_[P_COUNT];
    std::atomic<uint32_t> textGen_{0};
    std::atomic<uint32_t> batchSeq_{0};    // odd while a batch is being written (a seqlock)
    std::atomic<uint32_t> changes_{0};     // bumped by every write to want_ / shown_ (notify's cue)
    std::atomic<int>      batchDepth_{0};

    // Browser state + text cache (refresh writes, the UI thread reads).
    mutable std::mutex       mtx_;
    std::vector<std::string> texts_;
    size_t                   textHash_ = 0;
    int                      brCat_ = 2;       // index into categories(): starts on the first factory category
    int                      catPage_ = 0, itemPage_ = 0;
    std::string              followed_;        // the preset key the browser last followed
    std::string              presetKey_;
    std::vector<std::string> tileKeys_;        // what each preset tile holds now
    std::vector<int>         catTiles_;        // which category each category tile holds
    uint32_t                 rng_ = 0x2545F491u;
    std::string              about_;           // aboutText()

    // The status line: what the UI thread notes, and what the audio thread made of it.
    static constexpr int     kTouchBits = 10;  // touched_: (moves << kTouchBits) | the control's index
    static_assert(P_COUNT < (1 << kTouchBits), "a control's index must fit touched_'s low bits");
    std::atomic<uint32_t>    moves_{0};        // moves so far
    std::atomic<uint32_t>    touched_{0};      // the last one (0: none yet)
    std::atomic<uint32_t>    loads_{0};        // preset loads so far (about_ is the last one's)
    std::atomic<int>         status_{kStatusPlugin};

    // Every write of a value MPC should see goes through here.
    void put(int i, float v) {
        want_[i].store(v, std::memory_order_relaxed);
        changes_.fetch_add(1, std::memory_order_release);
    }

    // Audio-thread state.
    uint32_t scanned_ = 0xffffffffu;   // changes_ at the last complete notify pass
    bool     scanPending_ = true;
    uint32_t textSeen_ = 0;
    int      cursor_ = 0;
    int      sinceText_ = 0;
    // The status line's (tickStatus): samples so far, what was last seen of the UI's notes, the line
    // shown (status_), since when and until when, and a move waiting for kHoldS to pass.
    uint64_t now_ = 0;
    uint32_t touchedSeen_ = 0, loadsSeen_ = 0;
    int      line_ = kStatusPlugin, pending_ = -1;
    uint64_t lineSince_ = 0, lineUntil_ = 0, pendingAt_ = 0;
    bool     statusChanged_ = false;   // MPC must read the status text again
};

} // namespace af
