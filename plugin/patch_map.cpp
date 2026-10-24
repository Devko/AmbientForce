// From SubForce plugin/patch_map.cpp (8846421), namespace sf -> af; the M1 parameters and their formats.
#include "patch_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace af {

namespace {

// The options whose values are the patch map's, not an engine enum's: their names and what they become.
constexpr const char* kMemoryNames[] = {"Off", "1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars", "32 Bars", "64 Bars",
                                        "Forever"};
constexpr int kMemoryBars[] = {0, 1, 2, 4, 8, 16, 32, 64, -1};   // HarmonyPatch::memoryBars: off, bars, forever
static_assert(sizeof kMemoryNames / sizeof *kMemoryNames == sizeof kMemoryBars / sizeof *kMemoryBars, "Memory");
// Ground Reg and Air Reg: GroundPatch::registerOct 1, 2, 3; AirGenPatch::registerOct 4, 5, 6.
constexpr const char* kRegisterNames[] = {"Low", "Mid", "High"};
constexpr const char* kUnisonNames[] = {"1", "2"};                 // BloomPatch::unison
constexpr const char* kOnOff[] = {"Off", "On"};                    // a switch: 0 off, 1 on
// Echo Div's options are kDelayDivs' names: the table holds the beats beside them, so its names are gathered here.
struct DelayDivNames {
    const char* n[kNumDelayDivs];
    constexpr DelayDivNames() : n() {
        for (int i = 0; i < kNumDelayDivs; ++i) n[i] = kDelayDivs[i].name;
    }
};
constexpr DelayDivNames kDelayDivNames;

// The family's audio taper: a level knob's 0..1 to a gain, so the knob's middle is about -12 dB.
float taper(float knob) { return knob * knob; }

// Every option list of surface.py against the names of the values it stands for, entry by entry, as this
// compiles: a list in another order, with an option too many or too few, or a new one nobody checks, fails
// the build. (The names live beside the engine's enums: dsp/harmony.h and the others.)
struct OptionList {
    int id;
    const char* const* names;
    int count;
};
template <int N>
constexpr OptionList list(int id, const char* const (&names)[N]) { return {id, names, N}; }

constexpr OptionList kOptionLists[] = {
    list(P_H_KEY, kKeyNames), list(P_H_SCALE, kScaleNames), list(P_H_TUNING, kTuningNames),
    list(P_H_INPUT, kInputNames), list(P_H_CHORD, kChordNames), list(P_H_VOICING, kVoicingNames),
    list(P_H_LEADING, kOnOff), list(P_H_MEMORY, kMemoryNames), list(P_H_HOLD, kOnOff),
    list(P_H_ONSTOP, kOnStopNames),
    list(P_G_LISTEN, kListenNames), list(P_G_MUTE, kOnOff), list(P_G_TABLE, kTableNames),
    list(P_G_COLINT, kColorIntervalNames), list(P_G_REG, kRegisterNames),
    list(P_B_LISTEN, kListenNames), list(P_B_MUTE, kOnOff), list(P_B_FMODE, kFilterModeNames),
    list(P_B_TABLE, kTableNames), list(P_B_TABLEB, kTableNames), list(P_B_COUPLE, kCoupleNames),
    list(P_B_UNISON, kUnisonNames), list(P_B_TAIL, kTailNames),
    list(P_S_MODE, Reverb::kModeNames), list(P_S_FREEZE, kOnOff), list(P_S_SHINT, Reverb::kIntervalNames),
    list(P_G_BREATHSYNC, kSyncNames), list(P_G_BREATHDIV, kBarDivNames), list(P_G_SWAYSYNC, kSyncNames),
    list(P_G_SWAYDIV, kBarDivNames), list(P_B_SWAYSYNC, kSyncNames), list(P_B_SWAYDIV, kBarDivNames),
    list(P_E_MODE, kEchoModeNames), list(P_E_SYNC, kSyncNames), list(P_E_DIV, kDelayDivNames.n),
    list(P_A_LISTEN, kListenNames), list(P_A_MUTE, kOnOff), list(P_A_SOUND, kAirSoundNames),
    list(P_A_PATTERN, kAirPatternNames), list(P_A_REG, kRegisterNames), list(P_A_LOOP, kOnOff),
    list(P_A_LOOPSYNC, kSyncNames), list(P_A_LOOPDIV, kBarDivNames),
    list(P_W_LISTEN, kListenNames), list(P_W_MUTE, kOnOff), list(P_W_MODE, kWeatherModeNames),
    list(P_W_TOKEY, kToKeyNames), list(P_W_MEMTAP, kMemoryTapNames),
};

constexpr bool sameText(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

// The first sound parameter whose options aren't its list's names (or that has no list here), -1 when
// every one is: the compiler's note on the failed assertion below shows the index (param_ids.h's order).
constexpr int firstUnlike() {
    for (int i = 0; i < P_COUNT; ++i) {
        if (PARAM_INFO[i].kind != Kind::Synth || PARAM_INFO[i].nopts == 0) continue;
        bool same = false;
        for (const OptionList& l : kOptionLists) {
            if (l.id != i) continue;
            same = l.count == PARAM_INFO[i].nopts;
            for (int k = 0; same && k < l.count; ++k) same = sameText(PARAM_INFO[i].opts[k], l.names[k]);
        }
        if (!same) return i;
    }
    return -1;
}

} // namespace

static_assert(firstUnlike() == -1, "surface.py's option list for this parameter is not the engine's names (kOptionLists)");
static_assert(PARAM_SPECS[P_B_BOCT].lo == -2.0f && PARAM_SPECS[P_B_BOCT].hi == 2.0f, "BloomPatch::bOctave -2..+2");
static_assert(PARAM_SPECS[P_W_GRAINS].lo == 1.0f && PARAM_SPECS[P_W_GRAINS].hi == static_cast<float>(Weather::kGrains),
              "Grains runs from 1 to Weather::kGrains");
static_assert(PARAM_SPECS[P_A_MOTIF].lo == 3.0f && PARAM_SPECS[P_A_MOTIF].hi == static_cast<float>(AirGen::kMotifMax),
              "Motif runs from 3 to AirGen::kMotifMax");
// A saved project stores its values by key, but MPC stores them by index: the parameters of 0.0.2 keep theirs (the
// sound ones to b_swaydiv's popup flag), M2's follow (its sound values, then Weather's Source stepper, Remember and
// Keep), and the preset stepper and the browser move up behind them.
static_assert(P_B_SWAYDIV__OPEN == 97 && P_E_MODE == 98 && P_W_MEMTAP == 159 && P_W_SOURCE == 160 && P_W_KEEP == 164 &&
                  P_PRESET == 165,
              "MPC stores values by index: a parameter moved or added here moves saved projects' values (surface.py)");
static_assert(PARAM_SPECS[P_E_DIV].hi == static_cast<float>(kNumDelayDivs - 1), "an option for every Echo division");

float paramValue(int id, float n) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    n = n > 0.0f ? (n < 1.0f ? n : 1.0f) : 0.0f;   // NaN-safe (std::clamp passes NaN through)
    switch (s.curve) {
        case Curve::Lin:  return s.lo + n * (s.hi - s.lo);
        case Curve::Log:  return s.lo * std::pow(s.hi / s.lo, n);
        case Curve::Int:  return std::round(s.lo + n * (s.hi - s.lo));
        case Curve::Enum: return std::round(n * s.hi);   // lo = 0, hi = options - 1
        case Curve::Pow:  return s.hi * n * n * n;       // 0..hi, fine near 0 (times that may be 0)
        default:          return 0.0f;
    }
}

float paramNorm(int id, float v) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    float n = 0.0f;
    switch (s.curve) {
        case Curve::Log: n = v > 0.0f ? std::log(v / s.lo) / std::log(s.hi / s.lo) : 0.0f; break;
        case Curve::Pow: n = v > 0.0f && s.hi > 0.0f ? std::cbrt(v / s.hi) : 0.0f; break;
        case Curve::Lin:
        case Curve::Int:
        case Curve::Enum: n = s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f; break;
        default: break;
    }
    return std::isfinite(n) ? std::clamp(n, 0.0f, 1.0f) : 0.0f;   // values come from saved state text too
}

std::string paramDisplay(int id, float n) {
    if (id < 0 || id >= P_COUNT) return {};
    const float v = paramValue(id, n);
    char b[32];
    switch (PARAM_SPECS[id].fmt) {
        case Fmt::Enum: {
            const int i = static_cast<int>(v);
            return i >= 0 && i < PARAM_INFO[id].nopts ? PARAM_INFO[id].opts[i] : "";
        }
        case Fmt::Percent: std::snprintf(b, sizeof b, "%.0f%%", v * 100.0f); break;
        case Fmt::Bipolar:
            std::snprintf(b, sizeof b, std::fabs(v) < kBipolarZero ? "0%%" : "%+.0f%%", v * 100.0f);
            break;
        // Unit changes where the rounded text would reach the next unit ("1000 Hz" is "1.00 kHz").
        case Fmt::Hz:
            if (v < 999.5f) std::snprintf(b, sizeof b, "%.0f Hz", v);
            else std::snprintf(b, sizeof b, v < 9995.0f ? "%.2f kHz" : "%.1f kHz", v / 1000.0f);
            break;
        case Fmt::Hz2: std::snprintf(b, sizeof b, "%.2f Hz", v); break;   // Beat: the rate dialled in, to 0.01 Hz
        case Fmt::Time:   // seconds
            if (v < 0.00005f) return "0 ms";
            if (v < 0.00995f) std::snprintf(b, sizeof b, "%.1f ms", v * 1000.0f);
            else if (v < 0.9995f) std::snprintf(b, sizeof b, "%.0f ms", v * 1000.0f);
            else std::snprintf(b, sizeof b, v < 9.995f ? "%.2f s" : "%.1f s", v);
            break;
        case Fmt::Ms:     // milliseconds (the predelay)
            if (v < 0.05f) return "0 ms";
            std::snprintf(b, sizeof b, v < 9.95f ? "%.1f ms" : "%.0f ms", v);
            break;
        // A slow rate reads better as how long one cycle takes: 0.05 Hz is "20 s", 0.002 Hz "8.3 min".
        case Fmt::Period: {
            if (v >= 0.995f || v <= 0.0f) {
                std::snprintf(b, sizeof b, "%.2f Hz", v);
                break;
            }
            const float t = 1.0f / v;
            if (t < 9.95f) std::snprintf(b, sizeof b, "%.1f s", t);
            else if (t < 59.5f) std::snprintf(b, sizeof b, "%.0f s", t);
            else std::snprintf(b, sizeof b, "%.1f min", t / 60.0f);
            break;
        }
        case Fmt::Cents:
            if (v < 0.05f) return "0 ct";
            std::snprintf(b, sizeof b, v < 9.95f ? "%.1f ct" : "%.0f ct", v);
            break;
        case Fmt::Oct: {
            const long o = std::lround(v);
            if (o == 0) return "0 Oct";
            std::snprintf(b, sizeof b, "%+ld Oct", o);
            break;
        }
        case Fmt::Pan: {   // PolyForce's: C, L50, R100
            const long p = std::lround(std::fabs(v) * 100.0f);
            if (p == 0) return "C";
            std::snprintf(b, sizeof b, "%c%ld", v < 0.0f ? 'L' : 'R', p);
            break;
        }
        case Fmt::Semi: std::snprintf(b, sizeof b, v == 0.0f ? "0 st" : "%+.0f st", v); break;
        case Fmt::PerMin:   // Air Density: notes a minute; "off" where the patch map makes it 0 (kAirDensityOff)
            if (v < kAirDensityOff) return "off";
            std::snprintf(b, sizeof b, v < 9.95f ? "%.1f /min" : "%.0f /min", v);
            break;
        case Fmt::Note: {   // Split: a MIDI note as its name and octave, C4 = 60; 0 is off
            const long n = std::lround(v);
            if (n <= 0) return "Off";
            std::snprintf(b, sizeof b, "%s%ld", kKeyNames[n % 12], n / 12 - 1);
            break;
        }
        case Fmt::OctRange: std::snprintf(b, sizeof b, "%.1f Oct", v); break;   // Air Range: octaves, not signed
        case Fmt::Count: std::snprintf(b, sizeof b, "%.0f", v); break;
        case Fmt::Db:
            if (v <= kVolumeOffDb) return "-inf dB";   // where the engine's output is off
            std::snprintf(b, sizeof b, "%.1f dB", std::fabs(v) < 0.05f ? 0.0f : v);   // never "-0.0 dB"
            break;
        case Fmt::LfoHz: std::snprintf(b, sizeof b, v < 0.995f ? "%.2f Hz" : (v < 9.95f ? "%.1f Hz" : "%.0f Hz"), v); break;
        default: return {};
    }
    return b;
}

// Runs on the audio thread (plugin.cpp, when a value changed): no allocation, just arithmetic.
Patch patchFromParams(const float* norm) {
    Patch p = patchFromKnobs(norm);
    applyMacros(p, macrosFromParams(norm));
    return p;
}

Macros macrosFromParams(const float* norm) {
    // Under half a percent is 0, as the knob reads ("0%", kBipolarZero): a knob turned back by hand
    // lands within MPC's 1/1000 of the middle, and the preset should play as saved then (not, say, a
    // shimmer of 0.0006 woken up).
    auto at = [norm](int id) {
        const float v = paramValue(id, norm[id]);
        return std::fabs(v) < kBipolarZero ? 0.0f : v;
    };
    Macros m;
    m.horizon = at(P_M_HORIZON);
    m.motion = at(P_M_MOTION);
    m.glow = at(P_M_GLOW);
    m.density = at(P_M_DENSITY);
    return m;
}

// --- the macros ------------------------------------------------------------------------------------
// Fixed and relative: each macro moves the fields it owns from wherever the preset has them, the same way in
// every preset. Times, rates and frequencies move in octaves (x 2^(k x)), depths toward their ends, levels and
// sends as gains (their knobs squared, 0..1). A field at 0 that a macro scales stays 0 (a send, a
// partial, Body, Beat, Detune, Breath, Air's and Weather's levels, every Echo send, Air Density), so what a
// preset switched off stays off: no macro brings a stratum or an Echo send in from nothing, and a preset of
// 0.0.2, which has Air, Weather and Echo off, sounds the same at every macro position as it did. (Air Rubato
// and Mutate, Weather Drift and Echo Wow are depths and do move where nothing plays: nothing is heard of it.)
// Where two macros share a field (Horizon and Glow both move the tones) they apply one after the other, each
// clamping. Far steps the dry back, and dark and thick take a little off the volume, so the level stays near
// the preset's (test/preset_test.cpp: none leans on the limiter or drops far).
namespace {

float clampTo(int id, float v) { return std::clamp(v, PARAM_SPECS[id].lo, PARAM_SPECS[id].hi); }
float gain01(float g) { return std::clamp(g, 0.0f, 1.0f); }   // a level or send: the knob's 0..1, squared
float octaves(float v, float oct) { return v * std::exp2(oct); }
// x -1..1: v moved toward lo at -1 by `down` of the way there, toward hi at +1 by `up` of the way.
float toward(float v, float x, float lo, float hi, float down, float up) {
    return x < 0.0f ? v + (v - lo) * x * down : v + (hi - v) * x * up;
}
// The volume moved by db, so a macro keeps the level near where the preset has it; off stays off.
void trim(Patch& p, float db) {
    if (p.volumeDb > kVolumeOffDb) p.volumeDb = clampTo(P_VOLUME, p.volumeDb + db);
}
// x -1..1: v scaled down to 0 at -1, up by 2^(oct x) above 0 (capped at hi).
float fadeOrScale(float v, float x, float oct, float hi) {
    return x < 0.0f ? v * (1.0f + x) : std::min(hi, octaves(v, oct * x));
}

// Horizon, near (-1) to far (+1): the wet up and the dry back, the reverb longer, later and darker, blooming
// after the note. Near takes the sends down 6 dB and leaves Space Level alone: on a preset that is nearly all
// reverb (Frozen Sky) more took it 11 LU down. A Tone moves only on Bloom's low-pass: a band-pass's or
// high-pass's Tone picks a band, not a brightness (Overtone Choir's whistle), and moving it changes the sound
// more than the level allows.
constexpr float kHorizonSendNear = 1.0f;    // the sends (Space and Echo): x 2^h near (to -6 dB) ...
constexpr float kHorizonSendFar = 0.5f;     // ... x 2^(0.5 h) far (to +3 dB)
constexpr float kHorizonDryOct = 0.5f;      // toward far only: the dry levels down to x 2^-0.5 (-3 dB)
constexpr float kHorizonDecayOct = 1.3f;    // Space Decay x 2^(1.3 h): 0.41x..2.46x
constexpr float kHorizonPredelayMs = 50.0f; // Pre-Delay + 50 h ms
constexpr float kHorizonToneOct = -0.5f;    // both Tones and Space Damp, x 2^(-0.5 h): half an octave darker far
constexpr float kHorizonRiseUp = 0.6f;      // Rise toward 1 by 60% far, toward 0 near
// Air's and Weather's sends into Space, and all four Echo sends (Ground's, Bloom's, Air's, Weather's), move by
// the same octaves as Ground's and Bloom's Space sends; Air's and Weather's levels step back far like theirs.
// Motion, still (-1) to moving (+1).
constexpr float kMotionSwayUp = 0.8f;       // both Sways toward 1 by 80% (toward 0 still)
constexpr float kMotionRateOct = 2.0f;      // both Rates and Breath Rate x 2^(2 m): a quarter .. 4x (free only)
constexpr float kMotionSmearUp = 0.6f;      // Bloom Smear toward 1 by 60%
constexpr float kMotionBreathUp = 0.7f;     // Ground Breath toward 1 by 70%
constexpr float kMotionBeatOct = 1.5f;      // Ground Beat x 2^(1.5 m) moving (capped at 3 Hz), to 0 still
constexpr float kMotionRubatoUp = 0.6f;     // Air Rubato toward 1 by 60% (toward 0 still)
constexpr float kMotionMutateUp = 0.5f;     // Air Mutate toward 1 by 50% (toward 0 still)
constexpr float kMotionDriftUp = 0.7f;      // Weather Drift toward 1 by 70% (toward 0 still)
constexpr float kMotionWowUp = 0.5f;        // Echo Wow toward 1 by 50% (toward 0 still)
// Glow, dark (-1) to bright (+1).
constexpr float kGlowToneOct = 2.0f;        // both Tones x 2^(2 g): two octaves either way (Bloom's on LP only)
constexpr float kGlowTilt = 0.3f;           // Tilt + 0.3 g
constexpr float kGlowDampOct = 1.0f;        // Space Damp x 2^g
constexpr float kGlowShimmerUp = 0.35f;     // bright only: Shimmer + 0.35 g (an upward interval only)
constexpr float kGlowBodyOct = -1.0f;       // Ground Body x 2^-g: half toward bright, twice (darker vowels) toward dark
constexpr float kGlowDarkTrimDb = 1.5f;     // dark only: the volume down 1.5 dB at -1 (the low shelf and Body add level)
constexpr float kGlowAirToneOct = 2.0f;     // Air Tone x 2^(2 g): two octaves either way, like the other Tones
constexpr float kGlowWeatherTilt = 0.3f;    // Weather Tilt + 0.3 g, like the output Tilt
constexpr float kGlowEchoCutOct = 1.0f;     // Echo High Cut x 2^g: an octave either way
// Density, sparse (-1) to thick (+1).
constexpr float kDensityPartialOct = 1.0f;  // Ground Sub, Octave, Color x (1 + d) sparse (0 at -1), x 2^d thick
constexpr float kDensityDetuneOct = 1.0f;   // Bloom Detune: to 0 sparse (unison 2 plays as 1), x 2^d thick
constexpr float kDensityBreathOct = 1.0f;   // Bloom Breath: to 0 sparse, x 2^d thick
constexpr float kDensityStrumOct = 1.0f;    // Strum, sparse: x 2^(-d) + 0.6 s x (-d) (one by one); thick: down to a quarter
constexpr float kDensityStrumS = 0.6f;
constexpr float kDensityStrumThick = 0.75f;
constexpr float kDensityAirOct = 1.0f;      // Air Density to 0 sparse, x 2^d thick (capped at 60 a minute)
constexpr float kDensityGrainsOct = 1.0f;   // Weather Grains to 1 sparse, x 2^d thick (rounded, 1..16)
constexpr float kDensityThickTrimDb = 1.0f; // thick only: the volume down 1 dB at +1 (the partials add level)

} // namespace

void applyMacros(Patch& p, const Macros& m) {
    GroundPatch& g = p.ground;
    BloomPatch& b = p.bloom;
    Reverb::Params& r = p.space.reverb;
    AirPatch& a = p.air;
    WeatherPatch& w = p.weather;
    Delay::Params& e = p.echo.delay;
    if (m.horizon != 0.0f) {
        const float h = std::clamp(m.horizon, -1.0f, 1.0f);
        const float send = h * (h < 0.0f ? kHorizonSendNear : kHorizonSendFar);
        p.groundSpace = gain01(octaves(p.groundSpace, send));
        p.bloomSpace = gain01(octaves(p.bloomSpace, send));
        p.airSpace = gain01(octaves(p.airSpace, send));
        p.weatherSpace = gain01(octaves(p.weatherSpace, send));
        p.groundEcho = gain01(octaves(p.groundEcho, send));
        p.bloomEcho = gain01(octaves(p.bloomEcho, send));
        p.airEcho = gain01(octaves(p.airEcho, send));
        p.weatherEcho = gain01(octaves(p.weatherEcho, send));
        const float back = -kHorizonDryOct * std::max(h, 0.0f);
        g.level = gain01(octaves(g.level, back));
        b.level = gain01(octaves(b.level, back));
        a.level = gain01(octaves(a.level, back));
        w.level = gain01(octaves(w.level, back));
        r.decayS = clampTo(P_S_DECAY, octaves(r.decayS, kHorizonDecayOct * h));
        r.predelayMs = clampTo(P_S_PREDELAY, r.predelayMs + kHorizonPredelayMs * h);
        g.cutoffHz = clampTo(P_G_CUTOFF, octaves(g.cutoffHz, kHorizonToneOct * h));
        if (b.filterMode == FM_LP) b.cutoffHz = clampTo(P_B_CUTOFF, octaves(b.cutoffHz, kHorizonToneOct * h));
        r.dampHz = clampTo(P_S_DAMP, octaves(r.dampHz, kHorizonToneOct * h));
        p.space.rise = toward(p.space.rise, h, 0.0f, 1.0f, 1.0f, kHorizonRiseUp);
    }
    if (m.motion != 0.0f) {
        const float x = std::clamp(m.motion, -1.0f, 1.0f);
        g.pos.sway = toward(g.pos.sway, x, 0.0f, 1.0f, 1.0f, kMotionSwayUp);
        b.pos.sway = toward(b.pos.sway, x, 0.0f, 1.0f, 1.0f, kMotionSwayUp);
        g.pos.swayHz = clampTo(P_G_SWAYRATE, octaves(g.pos.swayHz, kMotionRateOct * x));
        b.pos.swayHz = clampTo(P_B_SWAYRATE, octaves(b.pos.swayHz, kMotionRateOct * x));
        g.breathHz = clampTo(P_G_BREATHRATE, octaves(g.breathHz, kMotionRateOct * x));   // synced cycles keep their bars
        b.pos.smear = toward(b.pos.smear, x, 0.0f, 1.0f, 1.0f, kMotionSmearUp);
        g.breath = toward(g.breath, x, 0.0f, 1.0f, 1.0f, kMotionBreathUp);
        g.beatHz = fadeOrScale(g.beatHz, x, kMotionBeatOct, PARAM_SPECS[P_G_BEAT].hi);
        a.gen.rubato = toward(a.gen.rubato, x, 0.0f, 1.0f, 1.0f, kMotionRubatoUp);
        a.gen.mutate = toward(a.gen.mutate, x, 0.0f, 1.0f, 1.0f, kMotionMutateUp);
        w.drift = toward(w.drift, x, 0.0f, 1.0f, 1.0f, kMotionDriftUp);
        e.wow = toward(e.wow, x, 0.0f, 1.0f, 1.0f, kMotionWowUp);
    }
    if (m.glow != 0.0f) {
        const float x = std::clamp(m.glow, -1.0f, 1.0f);
        g.cutoffHz = clampTo(P_G_CUTOFF, octaves(g.cutoffHz, kGlowToneOct * x));
        if (b.filterMode == FM_LP) b.cutoffHz = clampTo(P_B_CUTOFF, octaves(b.cutoffHz, kGlowToneOct * x));
        p.tilt = clampTo(P_O_TILT, p.tilt + kGlowTilt * x);
        r.dampHz = clampTo(P_S_DAMP, octaves(r.dampHz, kGlowDampOct * x));
        if (r.shimmerInterval != Reverb::DOWN_OCTAVE)   // a shimmer an octave down darkens: left as it is
            r.shimmer = std::min(1.0f, r.shimmer + kGlowShimmerUp * std::max(x, 0.0f));
        g.body = clampTo(P_G_BODY, octaves(g.body, kGlowBodyOct * x));
        a.voice.toneHz = clampTo(P_A_TONE, octaves(a.voice.toneHz, kGlowAirToneOct * x));
        w.tilt = clampTo(P_W_TILT, w.tilt + kGlowWeatherTilt * x);
        e.highCutHz = clampTo(P_E_HIGHCUT, octaves(e.highCutHz, kGlowEchoCutOct * x));
        trim(p, kGlowDarkTrimDb * std::min(x, 0.0f));
    }
    if (m.density != 0.0f) {
        const float x = std::clamp(m.density, -1.0f, 1.0f);
        g.sub = fadeOrScale(g.sub, x, kDensityPartialOct, 1.0f);
        g.octave = fadeOrScale(g.octave, x, kDensityPartialOct, 1.0f);
        g.color = fadeOrScale(g.color, x, kDensityPartialOct, 1.0f);
        b.detuneCents = fadeOrScale(b.detuneCents, x, kDensityDetuneOct, PARAM_SPECS[P_B_DETUNE].hi);
        b.breath = fadeOrScale(b.breath, x, kDensityBreathOct, 1.0f);
        a.gen.density = fadeOrScale(a.gen.density, x, kDensityAirOct, PARAM_SPECS[P_A_DENSITY].hi);
        // Under the knob's "off" mark is off, as the knob has it: a rate of a few notes an hour would keep Air awake.
        if (a.gen.density < kAirDensityOff) a.gen.density = 0.0f;
        w.grains = std::clamp(static_cast<int>(std::lround(fadeOrScale(static_cast<float>(w.grains), x, kDensityGrainsOct,
                                                                       PARAM_SPECS[P_W_GRAINS].hi))),
                              static_cast<int>(PARAM_SPECS[P_W_GRAINS].lo), static_cast<int>(PARAM_SPECS[P_W_GRAINS].hi));
        float& strum = p.harmony.strumS;
        strum = clampTo(P_H_STRUM, x < 0.0f ? octaves(strum, -kDensityStrumOct * x) - kDensityStrumS * x
                                            : strum * (1.0f - kDensityStrumThick * x));
        trim(p, -kDensityThickTrimDb * std::max(x, 0.0f));
    }
}

Patch patchFromKnobs(const float* norm) {
    auto V = [norm](int id) { return paramValue(id, norm[id]); };
    auto I = [&V](int id) { return static_cast<int>(V(id)); };   // options and whole numbers: rounded already
    auto On = [&I](int id) { return I(id) != 0; };
    // A synced cycle's length in quarter notes, 0 when it runs free (Free/Sync and its division).
    auto synced = [&On, &I](int sync, int div) { return On(sync) ? kBarDivBeats[I(div)] : 0.0f; };
    Patch p;
    p.volumeDb = V(P_VOLUME);
    p.tilt = V(P_O_TILT);

    HarmonyPatch& h = p.harmony;
    h.key = I(P_H_KEY);
    h.scale = I(P_H_SCALE);
    h.tuning = I(P_H_TUNING);
    h.input = I(P_H_INPUT);
    h.chord = I(P_H_CHORD);
    h.voicing = I(P_H_VOICING);
    h.leading = On(P_H_LEADING);
    h.strumS = V(P_H_STRUM);
    h.memoryBars = kMemoryBars[I(P_H_MEMORY)];
    p.hold = On(P_H_HOLD);
    p.onStop = I(P_H_ONSTOP);

    GroundPatch& g = p.ground;
    g.listen = I(P_G_LISTEN);
    g.mute = On(P_G_MUTE);
    g.level = taper(V(P_G_LEVEL));
    g.cutoffHz = V(P_G_CUTOFF);
    g.table = I(P_G_TABLE);
    g.pos.age = V(P_G_AGE);
    g.pos.sway = V(P_G_SWAY);
    g.pos.swayHz = V(P_G_SWAYRATE);
    g.beatHz = V(P_G_BEAT);
    g.gravityS = V(P_G_GRAVITY);
    g.fadeS = V(P_G_FADE);
    g.sub = V(P_G_SUB);
    g.root = V(P_G_ROOT);
    g.fifth = V(P_G_FIFTH);
    g.octave = V(P_G_OCT);
    g.color = V(P_G_COLOR);
    g.colorInterval = I(P_G_COLINT);
    g.registerOct = I(P_G_REG) + 1;   // Low, Mid, High: C1, C2, C3
    g.body = V(P_G_BODY);
    g.breath = V(P_G_BREATH);
    g.breathHz = V(P_G_BREATHRATE);
    g.breathBeats = synced(P_G_BREATHSYNC, P_G_BREATHDIV);
    g.pos.swayBeats = synced(P_G_SWAYSYNC, P_G_SWAYDIV);
    g.width = V(P_G_WIDTH);
    p.groundSpace = taper(V(P_G_SPACE));
    p.groundPan = V(P_G_PAN);

    BloomPatch& b = p.bloom;
    b.listen = I(P_B_LISTEN);
    b.mute = On(P_B_MUTE);
    b.level = taper(V(P_B_LEVEL));
    b.cutoffHz = V(P_B_CUTOFF);
    b.reso = V(P_B_RESO);
    b.filterMode = I(P_B_FMODE);
    b.table = I(P_B_TABLE);
    b.pos.age = V(P_B_AGE);
    b.pos.sway = V(P_B_SWAY);
    b.pos.swayHz = V(P_B_SWAYRATE);
    b.pos.smear = V(P_B_SMEAR);
    b.pos.swayBeats = synced(P_B_SWAYSYNC, P_B_SWAYDIV);
    b.tableB = I(P_B_TABLEB);
    b.bOctave = I(P_B_BOCT);
    b.blend = V(P_B_BLEND);
    b.couple = I(P_B_COUPLE);
    b.coupleAmt = V(P_B_CAMT);
    b.unison = I(P_B_UNISON) + 1;
    b.detuneCents = V(P_B_DETUNE);
    b.swellS = V(P_B_SWELL);
    b.releaseS = V(P_B_RELEASE);
    b.velSens = V(P_B_VEL);
    b.breath = V(P_B_BREATH);
    b.tail = I(P_B_TAIL);
    b.width = V(P_B_WIDTH);
    p.bloomSpace = taper(V(P_B_SPACE));
    p.bloomPan = V(P_B_PAN);

    // M2. The strata's levels and sends are knobs with the audio taper, like Ground's; every default is the
    // engine's (Patch{}), which has the new strata off.
    p.groundEcho = taper(V(P_G_ECHO));
    p.bloomEcho = taper(V(P_B_ECHO));

    AirPatch& a = p.air;
    a.listen = I(P_A_LISTEN);
    a.mute = On(P_A_MUTE);
    a.level = taper(V(P_A_LEVEL));
    a.velSens = V(P_A_VEL);
    a.voice.sound = I(P_A_SOUND);
    a.voice.toneHz = V(P_A_TONE);
    a.voice.decayS = V(P_A_DECAY);
    a.voice.width = V(P_A_WIDTH);
    AirGenPatch& ag = a.gen;
    const float density = V(P_A_DENSITY);
    ag.density = density < kAirDensityOff ? 0.0f : density;   // what the knob reads "off" at is off
    ag.pattern = I(P_A_PATTERN);
    ag.registerOct = I(P_A_REG) + 4;   // Low, Mid, High: the tonic in octave 4, 5, 6 (C4 = 60)
    ag.rangeOct = V(P_A_RANGE);
    ag.gravity = V(P_A_GRAVITY);
    ag.motif = I(P_A_MOTIF);
    ag.mutate = V(P_A_MUTATE);
    ag.loop = On(P_A_LOOP);
    ag.loopS = V(P_A_LOOPLEN);
    ag.loopBeats = synced(P_A_LOOPSYNC, P_A_LOOPDIV);
    ag.rubato = V(P_A_RUBATO);
    p.airSpace = taper(V(P_A_SPACE));
    p.airEcho = taper(V(P_A_ECHO));
    p.airPan = V(P_A_PAN);
    p.split = I(P_H_SPLIT) == 0 ? -1 : I(P_H_SPLIT);   // 0 is Off

    WeatherPatch& w = p.weather;
    w.listen = I(P_W_LISTEN);
    w.mute = On(P_W_MUTE);
    w.level = taper(V(P_W_LEVEL));
    w.mode = I(P_W_MODE);
    w.position = V(P_W_POSITION);
    w.drift = V(P_W_DRIFT);
    w.spray = V(P_W_SPRAY);
    w.sizeS = V(P_W_SIZE);
    w.grains = I(P_W_GRAINS);
    w.pitch = V(P_W_PITCH);
    w.toKey = I(P_W_TOKEY);
    w.reverse = V(P_W_REVERSE);
    w.width = V(P_W_WIDTH);
    w.tilt = V(P_W_TILT);
    w.hpHz = V(P_W_HP);
    w.duck = V(P_W_DUCK);
    // w.memory (the source is Memory's remembered 16 s) comes with the source, not with a knob.
    p.weatherSpace = taper(V(P_W_SPACE));
    p.weatherEcho = taper(V(P_W_ECHO));
    p.weatherPan = V(P_W_PAN);
    p.memoryTap = I(P_W_MEMTAP);

    // Echo: initEcho()'s Delay with the knobs' fields; Spread, Drive and Glide stay as it has them, and Echo
    // forces the mix to wet only.
    Delay::Params& d = p.echo.delay;
    d.mode = I(P_E_MODE);
    d.sync = On(P_E_SYNC);
    d.timeMs = V(P_E_TIME);
    d.divBeats = kDelayDivs[I(P_E_DIV)].beats;
    d.feedback = V(P_E_FEEDBACK);
    d.lowCutHz = V(P_E_LOWCUT);
    d.highCutHz = V(P_E_HIGHCUT);
    d.wow = V(P_E_WOW);
    d.duck = V(P_E_DUCK);
    d.diffuse = V(P_E_DIFFUSE);
    p.echoReturn = taper(V(P_E_RETURN));
    p.echoSpace = taper(V(P_E_SPACE));

    Reverb::Params& r = p.space.reverb;
    r.mode = I(P_S_MODE);
    r.size = V(P_S_SIZE);
    r.decayS = V(P_S_DECAY);
    r.predelayMs = V(P_S_PREDELAY);
    r.dampHz = V(P_S_DAMP);
    r.lowCutHz = V(P_S_LOWCUT);
    r.mod = V(P_S_MOD);
    r.width = V(P_S_WIDTH);
    r.freeze = On(P_S_FREEZE);
    r.shimmer = V(P_S_SHIMMER);
    r.shimmerInterval = I(P_S_SHINT);
    p.space.rise = V(P_S_RISE);
    p.spaceReturn = taper(V(P_S_RETURN));
    return p;
}

} // namespace af
