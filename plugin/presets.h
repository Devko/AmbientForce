// From SubForce plugin/presets.h (8846421), namespace sf -> af, unchanged otherwise.
#pragma once
// Presets as files, plus the factory set built in.
//
//   *.afp under <plugin dir>/Presets and /media/AkaiForce/AmbientForce Presets (AF_PRESET_ROOTS),
//   plus the factory presets (presets/Factory/<category>/, embedded at build time: one category
//   per folder, keys "builtin:<name>"). A preset file is the plugin's own state text
//   ("ambientforce 1" + key=value lines), so a preset and a project restore the same way. Saving
//   writes <first root>/User/User NNN.afp: there is no text entry on the device, so names are
//   numbered.
#include "library.h"

#include <string>

namespace af {

bool presetText(const std::string& key, std::string& out);   // factory or file
// A preset's one-line description: its "about=" line (the first; "" when there is none), which the
// status line shows when it loads. Not a parameter: the state ignores it, and saving never writes one.
std::string presetAbout(const std::string& text);
// Claims the next user preset file (created empty: highest number + 1) and its key. "" if
// there is no preset root. The caller writes it, or removes it on failure.
std::string nextUserPreset(std::string* key);

} // namespace af
