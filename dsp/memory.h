#pragma once
// Memory (docs/CONCEPT.md 5.4): the last 16 s of the instrument's own sound, recorded as it plays, at
// the grains' three levels, so Remember can hand it to Weather as a source (dsp/grainsrc.h) with
// nothing to build. Two rings take turns: one records, the other holds what Remember kept. Weather
// never reads the ring being recorded (plan decision 3), so nothing it plays can feed itself back.
//
// Recording (EffectForce's Grain recording, dsp/grain.cpp): write() stages each sample (one not
// finite, NaN or an infinity, made 0) scaled by 1/2 (6 dB of headroom: the tap may pass 0 dBFS
// before the limiter) and clamped, and records them kBlock (128) frames at a time: level 0 rounded
// to the nearest 16-bit value; each pair of frames (even, odd: the ring's own pairs, where
// Weather's level offsets put them) through the halfband decimator into level 1, each pair of
// those into level 2, from the
// floats before they were rounded, as grainsrc.cpp makes a file's; the guard frames written as the
// ring's first frames are. The decimator is halfband.h's FrameDecimator, StereoDecimator's filter
// over a run of frames as they lie (the same output, measured; half the instructions). Taking 128
// at a time, the decimators always have whole pairs, the rings hold the same bits whatever the
// pieces the signal came in (checked with pieces of 1, 33, 77, 100, 128 and at random), and a block
// costs about the same whether it comes whole or in the engine's pieces. fill(), seal() and
// remember() count the frames staged too (a Remember records them first); nothing else looks at the
// ring recording.
//
// Sleeps: asleep the engine writes nothing, so the ring joins what it recorded last to what comes
// after the wake, and a sleep cuts the sound mid-way (the tap is before the output's fade). seal(),
// at every sleep that keeps the ring (Stop's Cut, the end of its Fade, a long suspend), fades the
// newest kSeamFade frames to 0 (the staged ones before they are decimated, and those in the ring at
// each level over 220, 110 and 55 frames, as a Remember's seam; the decimators' state with them)
// and arms a fade-in over the next kSeamFade frames staged: the join is a dip, never a step. A ring
// that goes a while without write() and without a seal (a host that just stops calling) still joins
// what comes next to what came before.
//
// Remember: the ring recording becomes the remembered source, and recording goes on in the other,
// from its frame 0, empty. Its seam (newest frame against oldest) is faded over 5 ms each side at
// each level (220, 110 and 55 frames; EffectForce's hold fade, sin^2 from 0 at the seam), so a grain
// reading across it hears a dip, never a step; the guard is written after the fade.
// - Full (16 s or more recorded): a source of all its frames, starting at the oldest (origin: the
//   frame the next write would have taken).
// - Not yet full: the frames it holds, rounded down to a multiple of 4 (the levels halve twice),
//   from frame 0 (origin 0), the guard written right after them: Weather wraps at `frames`, so the
//   frames the ring has room for past them are never read.
// - Its gain is 2 / 32768 (the headroom back): Weather plays it at the level it was recorded.
// - Refused (false, nothing changes) under kRememberGapS of recording since the last Remember (a
//   Q-Link turned through Remember presses it twice; plan decision 9), with under kMinS recorded,
//   or while pinned. The gap is counted in samples recorded, so it doesn't move while nothing is.
//
// The ring remembered before is recorded over from the next write(). Its reader must have let go of
// it by then: Weather, given the new remembered() at its next render(), copies what its fading
// grains still need from the old one (weather.h), so the engine renders Weather between remember()
// and the next write() (Task 8: Remember at a control step's start, Memory recording after Space).
// Its GrainSource reads not ready (frames 0) from the Remember on, so a pointer to it kept by mistake
// fades out instead of reading the new recording.
//
// reset() (the engine's reset, its guard): the ring recording starts afresh, empty. The remembered
// ring, its source, the generation and the gap's count stay (plan decision 10).
//
// Threads: everything but pin() and unpin() is the audio thread's (or any one thread's while no
// audio runs). pin() and unpin() are any thread's (Keep writes the remembered ring to a WAV on the
// loader thread): while pinned, Remember is refused, so the remembered ring and its source hold
// still. The audio thread never waits: a Remember that finds the ring pinned returns false, and a
// pin() that finds a Remember under way yields until it is done (microseconds). generation() is
// readable from any thread.
//
// Cost, as ARM instructions (qemu's count, the device's flags, the difference of two run lengths):
// write() 3.3k a 128-sample block given whole, 3.6k given in the engine's pieces of 32 (19k in
// pieces of 1), against the plan's 4k: 0.09 and 0.10 points of p99 at its 0.0268 a thousand
// (device: pending). remember() adds 22.9k to its block (25.2k with 127 frames staged to record
// first), most of it the seam's fades a sample at a time; once in 2 s at most. seal() 12.5k with
// nothing staged (all 385 frames it fades in the rings, a sample at a time), its fade-in 0.5k more
// over the writes after; 7.4k in all with 127 staged; once a sleep.
//
// Real-time rules: both rings (9.9 MB) are allocated in the constructor; nothing allocates, locks or
// throws after it.
#include "grainsrc.h"
#include "halfband.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace af {

enum MemoryTap : int { MT_STRATA, MT_OUTPUT, MT_COUNT };
inline constexpr const char* kMemoryTapNames[] = {"Strata", "Output"};
static_assert(sizeof kMemoryTapNames / sizeof *kMemoryTapNames == MT_COUNT, "a name per tap");

class Memory {
public:
    static constexpr float kSeconds = 16.0f;
    static constexpr float kRememberGapS = 2.0f;   // a Remember sooner after the last is ignored
    static constexpr float kMinS = 0.5f;           // and one with less than this recorded
    static constexpr int kFrames = 705600;         // a ring's level-0 frames: 16 s, a multiple of 4
    static constexpr int kSeamFade = 220;          // the seam's fade each side at level 0 (5 ms)

    Memory();                                    // allocates both rings (UI thread): 9.9 MB
    Memory(const Memory&) = delete;              // (the sources point into it)
    Memory& operator=(const Memory&) = delete;
    void reset();                                // the ring recording starts afresh; the remembered one stays
    // Audio thread: n samples of the tap (any n; the engine's pieces), scaled by 1/2 into 16 bit
    // (6 dB of headroom), clamped; a sample not finite is 0.
    void write(const float* L, const float* R, int n);
    // Audio thread, at a sleep that keeps the ring: its newest 5 ms faded to 0 and the next 5 ms
    // written to fade in, so what the wake records joins it with a dip (above: Sleeps).
    void seal();
    // Audio thread: the ring recording becomes the remembered source (its seam faded over 5 ms each
    // side, its guard written), and recording goes on in the other, empty. False (nothing changes)
    // within kRememberGapS of the last, with under kMinS recorded, or while pinned.
    bool remember();
    const GrainSource* remembered() const;       // nullptr: nothing remembered yet
    float fill() const;                          // 0..1: how much of 16 s the ring recording holds
    uint32_t generation() const;                 // +1 at every Remember (Keep says which it wrote)
    // Any thread (Keep, on the loader thread): while pinned, Remember is refused, so the remembered
    // ring can be read safely. pin() returns false (and holds no pin) if nothing is remembered; it
    // holds the gate while it looks, so a Remember in those few instructions is refused, as one
    // while Keep writes is (plan decision 9). Pins count: each pin() that returned true takes one
    // unpin().
    bool pin();
    void unpin();

private:
    static constexpr int kBlock = 128;           // the frames recorded at a time (staged until then)

    struct Ring {
        std::vector<int16_t> level[GrainSource::kLevels];   // interleaved L R, then the guard
        GrainSource src;                                    // points into level; frames 0 until remembered
    };

    void restart();
    void flush();

    Ring ring_[2];
    int rec_ = 0;                        // the ring recording
    std::atomic<int> rem_{-1};           // the ring remembered; -1 none
    // The recording: level-0 frames it holds (up to kFrames, the staged ones too), the frames
    // staged in x_ and not yet in the ring, each level's next frame, the decimators.
    int written_ = 0, staged_ = 0;
    int w_[GrainSource::kLevels] = {};
    FrameDecimator down1_, down2_;
    int rise_ = kSeamFade;               // the next frame of a seal's fade-in; kSeamFade: none under way
    int64_t since_;                      // frames recorded since the last Remember (saturating; 64 bit)
    std::atomic<uint32_t> gen_{0};
    std::atomic<int> gate_{0};           // 0 free; -1 a Remember under way; > 0 pins held
    float fade_[GrainSource::kLevels][kSeamFade] = {};   // the seam's gains, nearest the seam first

    // The frames staged (L R floats, kBlock of them and four over for a group running past), then
    // at a flush each level's in turn.
    alignas(16) float x_[2 * (kBlock + 4)] = {};
};

} // namespace af
