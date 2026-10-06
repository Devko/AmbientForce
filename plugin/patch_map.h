// From SubForce plugin/patch_map.h (8846421), namespace sf -> af; the stub engine's Patch.
#pragma once
// MPC's 0..1 parameter values <-> real values, display text, and the engine Patch.
// Ranges, curves, names and options come from surface/surface.py via build/param_ids.h.
#include "param_ids.h"
#include "../dsp/engine.h"

#include <string>

namespace af {

float paramValue(int id, float norm);          // real value (Hz, seconds, semitones, option index...)
float paramNorm(int id, float value);          // inverse, for state text, tests and the bench
std::string paramDisplay(int id, float norm);  // what the knob's value label shows
Patch patchFromParams(const float* norm);      // norm[P_COUNT]

} // namespace af
