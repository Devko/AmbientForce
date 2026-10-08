#pragma once
// What Weather's grains read (docs/CONCEPT.md 5.4): a fixed source, never the sound being made, so
// nothing Weather plays can feed itself back (plan decision 3). A procedural field or a WAV is made
// one here, at load time; Memory (dsp/memory.h) fills the same layout as it records.
//
// The layout: stereo, 16 bit (half the memory of floats, and plenty under grains at -20 dBFS RMS),
// interleaved L R, at three rates. Level 0 is the source at 44.1 kHz; level 1 is level 0 through
// the halfband decimator (dsp/halfband.h) at half the rate, level 2 level 1's at a quarter. A grain
// pitched up reads a slower level, so what it would shift past 22 kHz was filtered out first
// (EffectForce's Grain buffer, which records the same three). Every level loops: the kGuard frames
// after its end repeat its start (cyclically, if the level is shorter than that), so the reads of
// one control step never have to wrap; the reader wraps once a step.
//
// Level L's frame m stands for level-0 time 2^L m + 1 - tau (level 1) or 3 - 3 tau (level 2), tau
// being the decimator's delay at low frequencies (3.19 input samples): the reader takes that into
// account, as EffectForce's does, so a grain on another level reads the same moment.
//
// buildSource() makes a source of any signal: the loop, the level, the levels. It allocates and
// takes tens of ms for a minute of audio, so it runs on a loader thread, never the audio thread.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace af {

// A Weather source: stereo, 16 bit, at three rates (EffectForce's levels: a grain pitched up reads a
// decimated copy, so what it would shift past 22 kHz was filtered out first). Interleaved L R. Each
// level loops: GrainSource::kGuard frames after its end repeat its start, so a call's reads never wrap.
// Ready only with all three levels and frames a multiple of 4: Weather has no fallback, since level
// 0 read at up to 4 frames a sample would alias and outrun the copies it takes at a source change.
// buildSource() makes all three, and Memory records all three.
struct GrainSource {
    static constexpr int kLevels = 3;
    static constexpr int kGuard = 256;
    int frames = 0;            // level 0's frames, a multiple of 4; level k holds frames >> k (then the guard)
    int origin = 0;            // level 0's frame where the source starts (Memory's oldest); 0 for a file
    float gain = 1.0f / 32768.0f;          // a sample's value as a float
    const int16_t* level[kLevels] = {};    // nullptr: empty (not ready)
    bool ready() const { return frames > 0 && frames % 4 == 0 && level[0] && level[1] && level[2]; }
};

// A source that owns its samples (a field or a WAV, built at load time, never on the audio thread).
// Not copyable: src points into data.
struct SourceBuffer {
    GrainSource src;                       // points into data
    std::vector<int16_t> data[GrainSource::kLevels];
    size_t bytes() const;
    SourceBuffer() = default;
    SourceBuffer(const SourceBuffer&) = delete;
    SourceBuffer& operator=(const SourceBuffer&) = delete;
};

// The signal (44.1 kHz, `frames` samples a channel) made a source: crossfaded into a seamless loop
// over its last kLoopFadeS (equal power, so the loop is frames - fade long), normalised to
// kSourceRmsDb RMS with its peak at most 0 dBFS, decimated twice by the halfband. Allocates.
//
// - The loop: its first `fade` frames are the start fading in (sin) under the frames past the loop's
//   end fading out (cos), so the last frame runs on into the first as the signal itself ran on.
//   Equal power (the plan's choice) suits material that differs at the two ends: a DC, or a
//   signal alike at both, swells by up to 3 dB across the fade (sin + cos reaches sqrt 2 half
//   way), and a tone in antiphase at the two ends dips there.
//   The loop is frames - fade long, rounded down to a multiple of 4 (the decimators halve twice),
//   the up to 3 frames over joining the fade. A signal under 0.5 s fades over about half of itself,
//   at most the loop less a frame (7 frames: a loop of 4 faded over 3). If that leaves under 4
//   frames to loop (a signal of 1 to 6 frames), the loop is its first 4 frames, unfaded, padded
//   with silence when it has fewer.
// - The level: one gain for both channels, the loop's RMS (both channels together) at
//   kSourceRmsDb unless its peak would pass 0 dBFS (then the peak at full scale). Silence stays
//   silence. A sample that isn't finite counts as 0.
// - The levels: each decimator first runs over the loop's last 4096 frame pairs (round and round a
//   shorter one), so it starts the real pass where the loop's own end leaves it: the slow levels
//   loop seamlessly too.
// Deterministic: the same signal makes the same samples. nullptr for no signal (frames <= 0).
constexpr float kLoopFadeS = 0.25f;
constexpr float kSourceRmsDb = -20.0f;
std::unique_ptr<SourceBuffer> buildSource(const float* L, const float* R, int frames);

} // namespace af
