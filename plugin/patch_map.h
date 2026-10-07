// From SubForce plugin/patch_map.h (8846421), namespace sf -> af.
#pragma once
// MPC's 0..1 parameter values <-> real values, display text, and the engine Patch.
// Ranges, curves, names and options come from surface/surface.py via build/param_ids.h.
#include "param_ids.h"
#include "../dsp/engine.h"

#include <string>

namespace af {

float paramValue(int id, float norm);          // real value (Hz, seconds, semitones, option index...)
float paramNorm(int id, float value);          // inverse, for state text, tests and the bench
// What the knob's value label shows, in the parameter's format (surface.py fmt): "2.50 kHz", "20 s" (a slow
// rate as its period), "0.30 Hz", "8.0 ct", "+1 Oct", "L40", an option's name...
std::string paramDisplay(int id, float norm);
// The engine's Patch from every parameter's 0..1 (norm[P_COUNT]). Levels and sends get the audio taper (gain =
// knob^2); options become the engine's enums; Memory becomes bars (0 off, -1 forever). Audio thread: no allocation.
Patch patchFromParams(const float* norm);

} // namespace af
