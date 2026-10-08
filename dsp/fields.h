#pragma once
// Weather's procedural fields (docs/CONCEPT.md 10): the factory sources, computed, never sampled, so
// no sample data ships. Eight in M2 (plan decision 6); Vinyl Dust, Tape Hiss, Room Tone and Distant
// City come with M4. Each is rendered on the loader thread when Weather wants it (not at load like
// the tables), then made a source by buildSource() (dsp/grainsrc.h): looped, at -20 dBFS RMS, with
// its slower levels.
//
// Every field is 12 s plus the loop's fade (kLoopFadeS), so buildSource's loop is exactly 12 s. It
// is stereo with its channels decorrelated: each channel's noise its own, events panned at constant
// power with up to 0.4 ms between the ears (the far side later; the crickets, steady tones, without
// it). Its periodic parts divide 12 s (the loop) and its pitched parts are on C (plan decision 5:
// every source's root is C for To Key). A second is rendered first and dropped, so the beds, the
// filters and the wanders are settled when the field begins and events of the second before ring on
// into it. A click's peak is its level, whatever its band (a wide band would pass more of its
// burst); the parts of a field are mixed by their RMS, at the levels below.
// - Rain on Roof: drops as a Poisson process, 300 a second, each a decaying click (a burst of noise,
//   0.2-0.8 ms, rising over 0.1 ms) through a band-pass at 1-4 kHz (Q 2-6, log-uniform and uniform
//   at random), at levels within 18 dB, over a low rumble of brown noise under 250 Hz, 7 dB under them.
// - Light Rain: 60 drops a second, higher (2.5-7 kHz, Q 3-8) and softer (bursts of 0.8-2 ms that
//   rise over 0.6 ms, levels within 12 dB), over a quiet pink bed under 6 kHz, 5 dB under them.
// - Wind High: white noise through a band-pass (Q 3) whose centre wanders 800-3000 Hz on slow
//   smoothed random walks (a shared one, and one each side), its level in gusts (a walk of its own,
//   2 to 6 s a step), the centre rising a little with them.
// - Wind Low: brown noise through a low-pass (Q 0.8) wandering 150-600 Hz, slow gusts (3 to 8 s).
// - Surf: pink noise in swells every 6 s (two to the loop, the second a little smaller), each a rise
//   of 2.4 s and a fall of 3.6 s (sin^2 up, cos^2 down), from 16 dB under the crest, the low-pass
//   over it opening from 400 Hz to 6 kHz with it. The loop's seam sits in a trough.
// - Stream: bubbles, 35 a second: sine chirps rising a fifth (exponentially) in 20-60 ms from
//   C-pentatonic pitches between C5 and C7, over band-passed noise (white about a wandering
//   900-1600 Hz, Q 0.7, and pink under 800 Hz), 8 dB under them.
// - Embers: sparse sharp crackles, 15 a second (each one to three clicks within 2 ms, through a
//   band-pass at 2-8 kHz, at levels within 24 dB), over a soft hiss (400 Hz to 7 kHz) and a low
//   warmth (brown noise under 200 Hz). Crackles that sparse carry little of the RMS, so the hiss is
//   8 dB over theirs: still about 20 dB under the loudest crackles' peaks (after the knee below).
// - Night: two crickets of FM chirps (the modulator at the carrier, its index falling over each
//   pulse), on C8 (4186 Hz, left) and G7 (3136 Hz, right, 3 dB softer), in trains of three 25 ms
//   pulses, a train every 0.5 s and every 0.75 s, taking turns (their peaks would add), over a quiet
//   low bed 6 dB under them. No train crosses the loop's seam (the crossfade would add a train to
//   itself).
//
// The level: buildSource sets the RMS to -20 dBFS unless the peak would pass 0 dBFS, so a field's
// peaks must stay within 20 dB of its RMS. Its peaks are rounded by a soft knee (tanh) that starts at
// 6.5 times its RMS (16.3 dB) and never reaches 9 times (19.1 dB), so buildSource's level is the
// RMS's. Everything under the knee is untouched (in Wind, Surf and Night nothing or a few frames
// reach it), but the impulsive fields' loudest transients are squashed, by as much as their peak
// stood over 19.1 dB before it: before the knee Embers' peak is 28.4 dB over its RMS (its loudest
// crackles lose up to about 9 dB), Light Rain's 25.4 (about 6), Rain on Roof's 22.1 (3), Stream's
// 21.3 (2). The frames over the knee: Light Rain 0.6% (its RMS 0.14 dB less), Rain on Roof 0.26%,
// Embers 0.14%, Stream 0.14%.
//
// Cost, at load time on the loader thread: 27 to 45 ms a field on x86 (-O2; 70 to 130 under ASan,
// checked under 1 s), and as ARM instructions (qemu's count, the device's flags) 202M (Wind High) to
// 359M (Stream) a field, Rain on Roof 298M, buildSource's 122M of it each time (device: pending).
#include "grainsrc.h"

#include <memory>

namespace af {

enum FieldId : int { FD_RAIN_ROOF, FD_LIGHT_RAIN, FD_WIND_HIGH, FD_WIND_LOW, FD_SURF, FD_STREAM, FD_EMBERS,
                     FD_NIGHT, FD_COUNT };
inline constexpr const char* kFieldNames[] = {"Rain on Roof", "Light Rain", "Wind High", "Wind Low", "Surf",
                                              "Stream", "Embers", "Night"};
static_assert(sizeof kFieldNames / sizeof *kFieldNames == FD_COUNT, "a name per field");
constexpr float kFieldS = 12.0f;   // every field loops after 12 s
// Load time (the loader thread): the field rendered and made a source (buildSource). Deterministic
// for a build (each field its own seed): the same samples every time. Not across machines: x86 and
// ARM round their library functions and fused operations apart, and their sources differ by 1 in 70
// to 346 of a field's 1.85M 16-bit samples (the review's measure; no event lands elsewhere), so no
// test may pin a field's bits across platforms. Allocates; tens of ms on x86. nullptr for an id
// out of range.
std::unique_ptr<SourceBuffer> renderField(int id);

} // namespace af
