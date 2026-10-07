// From SubForce plugin/patch_map.cpp (8846421), namespace sf -> af; the M1 parameters and their formats.
#include "patch_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace af {

namespace {

// Memory's options (surface.py h_memory) as HarmonyPatch::memoryBars: Off, the bars, Forever.
constexpr int kMemoryBars[] = {0, 1, 2, 4, 8, 16, 32, 64, -1};

// The family's audio taper: a level knob's 0..1 to a gain, so the knob's middle is about -12 dB.
float taper(float knob) { return knob * knob; }

constexpr int opts(int id) { return PARAM_INFO[id].nopts; }

} // namespace

// Option lists in surface.py against the engine's enums: a list with an option too many or too few doesn't
// compile. The order test/params_test.cpp checks: the tables' names against tableName(), the others by name.
static_assert(opts(P_H_KEY) == 12, "surface.py h_key: the 12 pitch classes, C first");
static_assert(opts(P_H_SCALE) == SC_COUNT && opts(P_H_TUNING) == TU_COUNT && opts(P_H_INPUT) == IN_COUNT &&
                  opts(P_H_CHORD) == CH_COUNT && opts(P_H_VOICING) == VO_COUNT,
              "surface.py harmony lists must match dsp/harmony.h");
static_assert(opts(P_H_MEMORY) == sizeof kMemoryBars / sizeof kMemoryBars[0], "surface.py h_memory vs kMemoryBars");
static_assert(opts(P_H_ONSTOP) == OS_COUNT, "surface.py h_onstop must match dsp/engine.h OnStop");
static_assert(opts(P_G_LISTEN) == LI_COUNT && opts(P_B_LISTEN) == LI_COUNT, "surface.py Listen must match dsp/harmony.h");
static_assert(opts(P_G_TABLE) == TB_COUNT && opts(P_B_TABLE) == TB_COUNT && opts(P_B_TABLEB) == TB_COUNT,
              "surface.py TABLES must match dsp/lifetime.h TableId");
static_assert(opts(P_G_COLINT) == CI_COUNT, "surface.py g_colint must match dsp/ground.h ColorInterval");
static_assert(opts(P_G_REG) == 3, "surface.py g_reg: GroundPatch::registerOct 1..3");
static_assert(opts(P_B_FMODE) == FM_COUNT && opts(P_B_TAIL) == TL_COUNT, "surface.py b_fmode / b_tail vs dsp/bloom.h");
static_assert(opts(P_B_COUPLE) == CP_COUNT, "surface.py b_couple must match dsp/lifeosc.h Couple");
static_assert(opts(P_B_UNISON) == 2, "surface.py b_unison: BloomPatch::unison 1..2");
static_assert(PARAM_SPECS[P_B_BOCT].lo == -2.0f && PARAM_SPECS[P_B_BOCT].hi == 2.0f, "BloomPatch::bOctave -2..+2");
static_assert(opts(P_S_MODE) == Reverb::kModes && opts(P_S_SHINT) == Reverb::kIntervals,
              "surface.py s_mode / s_shint must match dsp/reverb.h");
// Off / On lists: index 0 is Off.
static_assert(opts(P_H_LEADING) == 2 && opts(P_H_HOLD) == 2 && opts(P_G_MUTE) == 2 && opts(P_B_MUTE) == 2 &&
                  opts(P_S_FREEZE) == 2,
              "on / off lists");

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
            std::snprintf(b, sizeof b, std::fabs(v) < 0.005f ? "0%%" : "%+.0f%%", v * 100.0f);
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
    auto V = [norm](int id) { return paramValue(id, norm[id]); };
    auto I = [&V](int id) { return static_cast<int>(V(id)); };   // options and whole numbers: rounded already
    auto On = [&I](int id) { return I(id) != 0; };
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
