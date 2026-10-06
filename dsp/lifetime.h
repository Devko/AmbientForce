#pragma once
// The table library: lifetime tables and the digital waves, all computed (no sample data ships).
//
// A lifetime table is 256 single-cycle frames, each one moment of an instrument's note from the
// attack (frame 0) to the deep tail (frame 255). The frames come from additive life models
// (lifetime.cpp): every harmonic has its own start level, decay, beating and formant path. A table
// is pitch-free (the oscillator plays it at any note), so where a model has formants they sit on
// fixed harmonics, true at the model's reference pitch.
//
// Every lifetime frame has the same RMS (a full-scale sine's): the table carries timbre, not
// level, so Age 1 (the deep tail) is as loud as Age 0. The digital waves keep their Fourier
// series' own levels (a square is louder than a sine, as on an analog synth).
#include "wavetable.h"

#include <atomic>

namespace af {

constexpr int kLifeFrames = 256;

// The table library, in browser order: lifetime tables first, then the digital waves (one frame each).
enum TableId : int { TB_FELT_PIANO, TB_CELESTA, TB_GLASS_HARMONICA, TB_CELLO_TASTO, TB_CHOIR_AH_OO,
                     TB_REED_ORGAN, TB_SINE_BLOOM, TB_TAPE_STRINGS,
                     TB_SINE, TB_TRIANGLE, TB_SAW, TB_SQUARE, TB_COUNT };

const char* tableName(int id);             // "Felt Piano", ..., "Square"; "" out of range
bool isLifetime(int id);                   // the first 8
// Load time, not real time: allocates, ~1e8 flops. `cancel`, if given, is looked at between pairs
// of frames: once it is set the build gives up and returns false, `out` untouched.
bool buildTable(int id, Wavetable& out, const std::atomic<bool>* cancel = nullptr);
// One frame, built on its first call (a thread-safe static, never destroyed): the fallback of
// every slot. Call it once before audio starts (ensureTablesBuilding does), so the audio thread
// never builds it.
const Wavetable& sineTable();

// What a slot reads: published by the plugin's builder thread, read by the audio thread.
struct TableSet {
    std::atomic<const Wavetable*> t[TB_COUNT] = {};
    const Wavetable& get(int id) const;   // the table if built, else sineTable()
};

// Frame f of a lifetime table shows the note at time T * (f / 255)^kLifeCurve (T: the model's
// length), so frames 0..63 cover the first tenth of the note.
constexpr double kLifeCurve = 1.661;   // (64/255)^1.661 = 0.1

} // namespace af
