// From SubForce plugin/state.h (8846421), namespace sf -> af, unchanged otherwise.
#pragma once
// The plugin's state text, shared by projects (effGetChunk / effSetChunk) and preset files.
//
// "ambientforce 1": key=value lines of REAL values (Hz, seconds, semitones, option index) for every
// sound parameter, plus, in a project, the preset it came from. Survives parameters being added
// or reordered AND ranges changing (a 0..1 value would silently move when a range does).
#include "surface.h"

#include <string>

namespace af {

constexpr int kStateVersion = 1;

std::string saveState(const Surface& s, bool asPreset);
bool isStateText(const std::string& text);   // "ambientforce <version >= 1>" (a UTF-8 BOM allowed)
// A preset starts from the defaults (what it doesn't say is the default); a project's state
// only overrides what it lists. False if it isn't AmbientForce state.
bool loadState(Surface& s, const std::string& text, bool asPreset);

} // namespace af
