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
// The engine's Patch from every parameter's 0..1 (norm[P_COUNT]): the knobs' patch (patchFromKnobs), bent by the
// macros (applyMacros). Audio thread: no allocation.
Patch patchFromParams(const float* norm);
// The knobs alone, the macros left out. Levels and sends get the audio taper (gain = knob^2); options become the
// engine's enums; Memory becomes bars (0 off, -1 forever).
Patch patchFromKnobs(const float* norm);

// The four macros (surface.py m_*), each -1..+1. They bend the Patch the knobs make, not the knobs: each one
// moves its fields monotonically with its value, every result clamped to its parameter's range, and a macro at
// exactly 0 touches nothing, so a preset with its macros at 0 plays bit for bit as saved.
struct Macros {
    float horizon = 0.0f;   // near and dry (-1) .. far and vast (+1)
    float motion = 0.0f;    // still .. drifting
    float glow = 0.0f;      // dark .. bright
    float density = 0.0f;   // sparse .. thick
};
// A bipolar control (a macro, Tilt) nearer 0 than this shows "0%", and a macro there is 0: one
// number for both, so the knob never reads 0% while the macro bends the preset, nor the other way.
constexpr float kBipolarZero = 0.005f;
Macros macrosFromParams(const float* norm);
void applyMacros(Patch& p, const Macros& m);

} // namespace af
