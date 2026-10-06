// From SubForce plugin/state.cpp (8846421), namespace sf -> af, unchanged otherwise.
#include "state.h"

#include "patch_map.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace af {
namespace {
constexpr const char* kMagic = "ambientforce ";

// Numbers in the C locale whatever the process's is (a "0,5" would misread every preset).
std::string number(float v) {
    char b[32];
    const auto r = std::to_chars(b, b + sizeof b, v, std::chars_format::general, 6);   // 6 digits: 333 Hz, not 332.9999
    return std::string(b, r.ptr);
}

bool parse(const std::string& s, float& out) {
    size_t a = 0, e = s.size();
    while (a < e && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (e > a && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    if (a < e && s[a] == '+') ++a;
    float v = 0.0f;
    const auto r = std::from_chars(s.data() + a, s.data() + e, v);
    if (r.ec != std::errc() || r.ptr != s.data() + e || !std::isfinite(v)) return false;
    out = v;
    return true;
}
} // namespace

bool isStateText(const std::string& text) {
    size_t at = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;   // a UTF-8 byte-order mark (a text editor's)
    if (text.compare(at, std::strlen(kMagic), kMagic) != 0) return false;
    at += std::strlen(kMagic);
    int version = 0;
    const auto r = std::from_chars(text.data() + at, text.data() + text.size(), version);
    return r.ec == std::errc() && version >= 1;
}

std::string saveState(const Surface& s, bool asPreset) {
    std::string out = std::string(kMagic) + std::to_string(kStateVersion) + "\n";
    for (int i = 0; i < P_COUNT; ++i) {
        if (PARAM_INFO[i].kind != Kind::Synth) continue;
        out += PARAM_INFO[i].key;
        out += '=';
        out += number(paramValue(i, s.get(i)));
        out += '\n';
    }
    if (!asPreset && !s.presetKey().empty()) out += "preset=" + s.presetKey() + "\n";
    return out;
}

bool loadState(Surface& s, const std::string& textIn, bool asPreset) {
    if (!isStateText(textIn)) return false;
    const std::string text = textIn.compare(0, 3, "\xEF\xBB\xBF") == 0 ? textIn.substr(3) : textIn;
    Surface::Batch batch(s);   // the audio thread never plays a half-loaded sound
    if (asPreset)
        for (int i = 0; i < P_COUNT; ++i)
            if (PARAM_INFO[i].kind == Kind::Synth) s.setValue(i, PARAM_INFO[i].def);
    // A preset is complete (what it doesn't name is the default); a project changes only what it lists.
    std::string preset;
    size_t at = text.find('\n');
    while (at != std::string::npos && at + 1 < text.size()) {
        const size_t end = text.find('\n', at + 1);
        std::string line = text.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
        at = end;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "preset") {
            preset = val;
            continue;
        }
        for (int i = 0; i < P_COUNT; ++i)
            if (PARAM_INFO[i].kind == Kind::Synth && key == PARAM_INFO[i].key) {
                float v = 0.0f;
                if (parse(val, v)) s.setValue(i, paramNorm(i, v));
                break;
            }
    }
    if (!asPreset) s.setPresetKey(preset);   // a project without one came from no preset
    s.refresh();
    return true;
}

} // namespace af
