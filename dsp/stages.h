#pragma once
// From SubForce dsp/stages.h (8846421), namespace sf -> af; the stub's one stage.
// Where a block's time goes, for the profiling build only (-DAF_STAGE_TIMING: `make
// arm-bench-stages`, read by tools/bench.cpp through AmbientForceStageTimes). Engine::render laps
// a clock between its stages; in the normal build StageClock is empty and lap() compiles to
// nothing.
#include <cstdint>
#ifdef AF_STAGE_TIMING
#include <ctime>
#endif

namespace af {

enum Stage : int { STG_VOICES, STG_COUNT };
constexpr const char* kStageNames[STG_COUNT] = {"voices"};

#ifdef AF_STAGE_TIMING
extern uint64_t g_stageNs[STG_COUNT];   // summed over every instance (the bench runs one)

inline uint64_t stageNow() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000u + static_cast<uint64_t>(ts.tv_nsec);
}

struct StageClock {
    uint64_t last = stageNow();
    void lap(int s) {
        const uint64_t t = stageNow();
        g_stageNs[s] += t - last;
        last = t;
    }
};
#else
struct StageClock {
    void lap(int) {}
};
#endif

} // namespace af
