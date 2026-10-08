// The parameters as surface.py declares them, against the engine they drive: every default and the text it
// reads, the display formats, the popups, the help lines, the patch map (the defaults play what a Patch{} plays,
// every option lands on its value, M2's strata off in every factory preset), the macros (at 0 nothing, else their
// own fields one way, in range), and every sound value at both ends of its range while a chord sounds, Air,
// Weather and Echo on for the new ones. That the option lists are the engine's names is checked as
// plugin/patch_map.cpp compiles.
#include "host.h"
#include "factory_presets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace aft {
namespace {

std::string shown(int id, float value) { return af::paramDisplay(id, af::paramNorm(id, value)); }

// Every sound parameter's default, as it reads: what surface.py declares, printed in its format. A new
// parameter must be listed (the count is checked), so its default's text is looked at once.
const struct {
    const char* key;
    const char* text;
} kDefaults[] = {
    {"volume", "-6.0 dB"},
    {"h_key", "C"}, {"h_scale", "Major"}, {"h_tuning", "Just"}, {"h_input", "Snap"}, {"h_chord", "Triad"},
    {"h_voicing", "Open"}, {"h_leading", "On"}, {"h_strum", "0 ms"}, {"h_memory", "Forever"}, {"h_hold", "Off"},
    {"h_onstop", "Fade"},
    {"g_listen", "Harmony"}, {"g_mute", "Off"}, {"g_level", "70%"}, {"g_cutoff", "2.50 kHz"},
    {"g_table", "Cello Tasto"}, {"g_age", "50%"}, {"g_sway", "30%"}, {"g_swayrate", "20 s"}, {"g_beat", "0.30 Hz"},
    {"g_gravity", "6.00 s"}, {"g_fade", "4.00 s"}, {"g_sub", "30%"}, {"g_root", "100%"}, {"g_fifth", "50%"},
    {"g_oct", "25%"}, {"g_color", "0%"}, {"g_colint", "9th"}, {"g_reg", "Mid"}, {"g_body", "0%"},
    {"g_breath", "30%"}, {"g_space", "40%"}, {"g_width", "50%"}, {"g_pan", "C"},
    {"b_listen", "Notes"}, {"b_mute", "Off"}, {"b_level", "70%"}, {"b_cutoff", "5.00 kHz"}, {"b_reso", "10%"},
    {"b_fmode", "LP"}, {"b_table", "Felt Piano"}, {"b_age", "60%"}, {"b_sway", "25%"}, {"b_swayrate", "14 s"},
    {"b_smear", "10%"}, {"b_tableb", "Sine"}, {"b_boct", "0 Oct"}, {"b_blend", "0%"}, {"b_couple", "Mix"},
    {"b_camt", "0%"}, {"b_unison", "1"}, {"b_detune", "8.0 ct"}, {"b_swell", "2.50 s"}, {"b_release", "6.00 s"},
    {"b_vel", "40%"}, {"b_breath", "5%"}, {"b_tail", "Space"}, {"b_space", "50%"}, {"b_width", "60%"},
    {"b_pan", "C"},
    {"s_mode", "Hall"}, {"s_size", "60%"}, {"s_decay", "8.00 s"}, {"s_predelay", "30 ms"}, {"s_damp", "6.00 kHz"},
    {"s_lowcut", "120 Hz"}, {"s_mod", "40%"}, {"s_width", "100%"}, {"s_freeze", "Off"}, {"s_shimmer", "0%"},
    {"s_shint", "+12"}, {"s_rise", "20%"}, {"s_return", "80%"},
    {"o_tilt", "0%"},
    {"m_horizon", "0%"}, {"m_motion", "0%"}, {"m_glow", "0%"}, {"m_density", "0%"},
    {"g_breathrate", "14 s"}, {"g_breathsync", "Free"}, {"g_breathdiv", "8 Bars"}, {"g_swaysync", "Free"},
    {"g_swaydiv", "8 Bars"}, {"b_swaysync", "Free"}, {"b_swaydiv", "8 Bars"},
    // M2: Echo, Air, Weather. Off in Init: the levels and the Echo sends at 0.
    {"e_mode", "Stereo"}, {"e_sync", "Sync"}, {"e_time", "450 ms"}, {"e_div", "1/4."}, {"e_feedback", "45%"},
    {"e_lowcut", "150 Hz"}, {"e_highcut", "4.50 kHz"}, {"e_wow", "30%"}, {"e_duck", "30%"}, {"e_diffuse", "30%"},
    {"e_return", "70%"}, {"e_space", "30%"}, {"g_echo", "0%"}, {"b_echo", "0%"},
    {"a_listen", "Harmony"}, {"a_mute", "Off"}, {"a_level", "0%"}, {"a_tone", "6.00 kHz"}, {"a_sound", "Glass"},
    {"a_decay", "4.00 s"}, {"a_density", "12 /min"}, {"a_pattern", "Constellation"}, {"a_reg", "Mid"},
    {"a_range", "2.0 Oct"}, {"a_gravity", "60%"}, {"a_motif", "5"}, {"a_mutate", "30%"}, {"a_loop", "Off"},
    {"a_looplen", "16.0 s"}, {"a_loopsync", "Free"}, {"a_loopdiv", "4 Bars"}, {"a_rubato", "20%"}, {"a_vel", "50%"},
    {"a_width", "70%"}, {"a_space", "50%"}, {"a_echo", "0%"}, {"a_pan", "C"}, {"h_split", "Off"},
    {"w_listen", "Free"}, {"w_mute", "Off"}, {"w_level", "0%"}, {"w_mode", "Cloud"}, {"w_position", "50%"},
    {"w_drift", "20%"}, {"w_spray", "30%"}, {"w_size", "250 ms"}, {"w_grains", "8"}, {"w_pitch", "0 st"},
    {"w_tokey", "Off"}, {"w_reverse", "0%"}, {"w_width", "70%"}, {"w_tilt", "0%"}, {"w_hp", "20 Hz"},
    {"w_duck", "0%"}, {"w_space", "30%"}, {"w_echo", "0%"}, {"w_pan", "C"}, {"w_memtap", "Output"},
};

void testDefaults() {
    std::printf("== parameters: every default and its text\n");
    int synth = 0, listed = 0;
    for (int i = 0; i < af::P_COUNT; ++i) {
        if (af::PARAM_INFO[i].kind != af::Kind::Synth) continue;
        ++synth;
        // 0..1 -> value -> 0..1 lands where it started (state text stores values), and reads the same.
        const float n = af::PARAM_INFO[i].def, v = af::paramValue(i, n), back = af::paramNorm(i, v);
        const std::string text = af::paramDisplay(i, n);
        const char* want = nullptr;
        for (const auto& d : kDefaults)
            if (std::strcmp(d.key, af::PARAM_INFO[i].key) == 0) want = d.text;
        listed += want != nullptr;
        const bool ok = std::fabs(back - n) < 1e-5f && af::paramDisplay(i, back) == text && want && text == want;
        if (!ok) std::printf("  %s: default %g reads \"%s\", expected \"%s\"\n", af::PARAM_INFO[i].key, v, text.c_str(), want ? want : "(not listed)");
        CHECK(ok);
    }
    CHECK(listed == synth && synth == static_cast<int>(std::size(kDefaults)));
    // What MPC reads from a fresh instance.
    Host h;
    CHECK(h.display(af::P_G_CUTOFF) == "2.50 kHz" && h.display(af::P_G_TABLE) == "Cello Tasto" &&
          h.display(af::P_B_SWAYRATE) == "14 s");
}

void testFormats() {
    std::printf("== parameters: display formats\n");
    // period: a slow rate as how long one cycle takes; from 1 Hz up, the rate.
    CHECK(shown(af::P_G_SWAYRATE, 0.002f) == "8.3 min" && shown(af::P_B_SWAYRATE, 1.0f / 60.0f) == "1.0 min");
    CHECK(shown(af::P_G_SWAYRATE, 0.05f) == "20 s" && shown(af::P_G_SWAYRATE, 0.5f) == "2.0 s");
    CHECK(shown(af::P_G_SWAYRATE, 1.0f) == "1.00 Hz" && shown(af::P_G_SWAYRATE, 2.0f) == "2.00 Hz");
    // hz2: Beat to 0.01 Hz.
    CHECK(shown(af::P_G_BEAT, 0.0f) == "0.00 Hz" && shown(af::P_G_BEAT, 1.25f) == "1.25 Hz" &&
          shown(af::P_G_BEAT, 3.0f) == "3.00 Hz");
    // cents, oct, ms, pan.
    CHECK(shown(af::P_B_DETUNE, 0.0f) == "0 ct" && shown(af::P_B_DETUNE, 12.4f) == "12 ct" &&
          shown(af::P_B_DETUNE, 50.0f) == "50 ct");
    CHECK(shown(af::P_B_BOCT, -2.0f) == "-2 Oct" && shown(af::P_B_BOCT, 1.0f) == "+1 Oct" &&
          shown(af::P_B_BOCT, 2.0f) == "+2 Oct");
    CHECK(shown(af::P_S_PREDELAY, 0.0f) == "0 ms" && shown(af::P_S_PREDELAY, 5.0f) == "5.0 ms" &&
          shown(af::P_S_PREDELAY, 250.0f) == "250 ms");
    CHECK(shown(af::P_G_PAN, -0.4f) == "L40" && shown(af::P_B_PAN, 1.0f) == "R100");
    CHECK(shown(af::P_O_TILT, -1.0f) == "-100%" && shown(af::P_O_TILT, 0.5f) == "+50%");
    // Times: ms under a second, two decimals to 10 s, one above; Hz into kHz.
    CHECK(shown(af::P_S_DECAY, 0.1f) == "100 ms" && shown(af::P_B_RELEASE, 0.01f) == "10 ms" &&
          shown(af::P_B_RELEASE, 12.34f) == "12.3 s" && shown(af::P_B_RELEASE, 30.0f) == "30.0 s");
    CHECK(shown(af::P_G_CUTOFF, 40.0f) == "40 Hz" && shown(af::P_G_CUTOFF, 16000.0f) == "16.0 kHz" &&
          shown(af::P_B_CUTOFF, 20.0f) == "20 Hz");
    CHECK(shown(af::P_H_STRUM, 2.0f) == "2.00 s" && shown(af::P_G_GRAVITY, 30.0f) == "30.0 s");
    // permin: Air Density in notes a minute, "off" where the patch map makes it 0 (kAirDensityOff).
    CHECK(shown(af::P_A_DENSITY, 0.0f) == "off" && shown(af::P_A_DENSITY, 0.04f) == "off" &&
          shown(af::P_A_DENSITY, 0.06f) == "0.1 /min" && shown(af::P_A_DENSITY, 5.0f) == "5.0 /min" &&
          shown(af::P_A_DENSITY, 12.0f) == "12 /min" && shown(af::P_A_DENSITY, 60.0f) == "60 /min");
    // note: Split as a note's name and octave, C4 = 60; 0 is off.
    CHECK(shown(af::P_H_SPLIT, 0.0f) == "Off" && shown(af::P_H_SPLIT, 60.0f) == "C4" &&
          shown(af::P_H_SPLIT, 61.0f) == "C#4" && shown(af::P_H_SPLIT, 72.0f) == "C5" &&
          shown(af::P_H_SPLIT, 127.0f) == "G9" && shown(af::P_H_SPLIT, 1.0f) == "C#-1");
    // octr: Air Range in octaves, not signed.
    CHECK(shown(af::P_A_RANGE, 0.5f) == "0.5 Oct" && shown(af::P_A_RANGE, 2.0f) == "2.0 Oct" &&
          shown(af::P_A_RANGE, 3.0f) == "3.0 Oct");
    // The rest of M2's formats are old ones: ms for Echo Time, semitones, counts, times.
    CHECK(shown(af::P_E_TIME, 1.0f) == "1.0 ms" && shown(af::P_E_TIME, 2000.0f) == "2000 ms" &&
          shown(af::P_W_PITCH, -24.0f) == "-24 st" && shown(af::P_W_PITCH, 12.0f) == "+12 st" &&
          shown(af::P_W_GRAINS, 16.0f) == "16" && shown(af::P_A_MOTIF, 3.0f) == "3" &&
          shown(af::P_W_SIZE, 0.02f) == "20 ms" && shown(af::P_W_SIZE, 2.0f) == "2.00 s" &&
          shown(af::P_A_LOOPLEN, 120.0f) == "120.0 s" && shown(af::P_A_DECAY, 0.1f) == "100 ms");
}


// Every popup's open flag belongs to a sound parameter's list, and is named after it. (The lists themselves are
// held to the engine's names, entry by entry, as plugin/patch_map.cpp compiles.)
void testPopups() {
    std::printf("== parameters: popups\n");
    int popups = 0;
    for (int i = 0; i < af::P_COUNT; ++i) {
        if (af::PARAM_INFO[i].kind != af::Kind::Popup) continue;
        ++popups;
        const int of = af::PARAM_INFO[i].popupOf;
        CHECK(of >= 0 && af::PARAM_INFO[of].kind == af::Kind::Synth && af::PARAM_INFO[of].nopts > 2 &&
              std::string(af::PARAM_INFO[i].name) == std::string(af::PARAM_INFO[of].name) + " List");
    }
    // Key, Scale, Chord, Memory, the three tables, Space Type, the three Divs; M2's Echo Div, Air Sound, Air Pattern
    // and Loop Div.
    CHECK(popups == 15);
}

// --- the patch, field by field ----------------------------------------------------------------------

// Every number in a Patch, by name, as a float (an int or bool exactly): two patches are alike when these are, bit
// for bit. A field added to Patch is added here: those of 0.0.2 in fields(), M2's in newFields().
struct Field {
    const char* name;
    float v;
};
std::vector<Field> fields(const af::Patch& p) {
    const af::HarmonyPatch& h = p.harmony;
    const af::GroundPatch& g = p.ground;
    const af::BloomPatch& b = p.bloom;
    const af::Reverb::Params& r = p.space.reverb;
    auto f = [](int v) { return static_cast<float>(v); };
    return {
        {"volumeDb", p.volumeDb}, {"tilt", p.tilt}, {"hold", f(p.hold)}, {"onStop", f(p.onStop)},
        {"key", f(h.key)}, {"scale", f(h.scale)}, {"tuning", f(h.tuning)}, {"input", f(h.input)}, {"chord", f(h.chord)},
        {"voicing", f(h.voicing)}, {"leading", f(h.leading)}, {"strumS", h.strumS}, {"memoryBars", f(h.memoryBars)},
        {"g.listen", f(g.listen)}, {"g.mute", f(g.mute)}, {"g.level", g.level}, {"g.cutoffHz", g.cutoffHz},
        {"g.table", f(g.table)}, {"g.age", g.pos.age}, {"g.sway", g.pos.sway}, {"g.swayHz", g.pos.swayHz},
        {"g.smear", g.pos.smear}, {"g.beatHz", g.beatHz}, {"g.gravityS", g.gravityS}, {"g.fadeS", g.fadeS},
        {"g.sub", g.sub}, {"g.root", g.root}, {"g.fifth", g.fifth}, {"g.octave", g.octave}, {"g.color", g.color},
        {"g.colorInterval", f(g.colorInterval)}, {"g.registerOct", f(g.registerOct)}, {"g.body", g.body},
        {"g.breath", g.breath}, {"g.breathHz", g.breathHz}, {"g.breathBeats", g.breathBeats},
        {"g.swayBeats", g.pos.swayBeats}, {"g.width", g.width},
        {"groundSpace", p.groundSpace}, {"groundPan", p.groundPan},
        {"b.listen", f(b.listen)}, {"b.mute", f(b.mute)}, {"b.level", b.level}, {"b.cutoffHz", b.cutoffHz},
        {"b.reso", b.reso}, {"b.filterMode", f(b.filterMode)}, {"b.table", f(b.table)}, {"b.age", b.pos.age},
        {"b.sway", b.pos.sway}, {"b.swayHz", b.pos.swayHz}, {"b.smear", b.pos.smear},
        {"b.swayBeats", b.pos.swayBeats}, {"b.tableB", f(b.tableB)},
        {"b.bOctave", f(b.bOctave)}, {"b.blend", b.blend}, {"b.couple", f(b.couple)}, {"b.coupleAmt", b.coupleAmt},
        {"b.unison", f(b.unison)}, {"b.detuneCents", b.detuneCents}, {"b.swellS", b.swellS},
        {"b.releaseS", b.releaseS}, {"b.velSens", b.velSens}, {"b.breath", b.breath}, {"b.tail", f(b.tail)},
        {"b.width", b.width}, {"bloomSpace", p.bloomSpace}, {"bloomPan", p.bloomPan},
        {"r.mode", f(r.mode)}, {"r.size", r.size}, {"r.decayS", r.decayS}, {"r.predelayMs", r.predelayMs},
        {"r.dampHz", r.dampHz}, {"r.lowCutHz", r.lowCutHz}, {"r.mod", r.mod}, {"r.width", r.width},
        {"r.freeze", f(r.freeze)}, {"r.mix", r.mix}, {"r.shimmer", r.shimmer}, {"r.shimmerInterval", f(r.shimmerInterval)},
        {"rise", p.space.rise}, {"spaceReturn", p.spaceReturn},
    };
}

// M2's fields: Air, Weather and Echo, the strata's Echo sends, the memory tap and the split.
std::vector<Field> newFields(const af::Patch& p) {
    const af::AirPatch& a = p.air;
    const af::AirGenPatch& ag = a.gen;
    const af::WeatherPatch& w = p.weather;
    const af::Delay::Params& d = p.echo.delay;
    auto f = [](int v) { return static_cast<float>(v); };
    return {
        {"groundEcho", p.groundEcho}, {"bloomEcho", p.bloomEcho},
        {"a.listen", f(a.listen)}, {"a.mute", f(a.mute)}, {"a.level", a.level}, {"a.velSens", a.velSens},
        {"a.sound", f(a.voice.sound)}, {"a.toneHz", a.voice.toneHz}, {"a.decayS", a.voice.decayS},
        {"a.width", a.voice.width}, {"a.density", ag.density}, {"a.pattern", f(ag.pattern)},
        {"a.registerOct", f(ag.registerOct)}, {"a.rangeOct", ag.rangeOct}, {"a.gravity", ag.gravity},
        {"a.motif", f(ag.motif)}, {"a.mutate", ag.mutate}, {"a.loop", f(ag.loop)}, {"a.loopS", ag.loopS},
        {"a.loopBeats", ag.loopBeats}, {"a.rubato", ag.rubato}, {"airSpace", p.airSpace}, {"airEcho", p.airEcho},
        {"airPan", p.airPan},
        {"w.listen", f(w.listen)}, {"w.mute", f(w.mute)}, {"w.level", w.level}, {"w.memory", f(w.memory)},
        {"w.mode", f(w.mode)}, {"w.position", w.position}, {"w.drift", w.drift}, {"w.spray", w.spray},
        {"w.sizeS", w.sizeS}, {"w.grains", f(w.grains)}, {"w.pitch", w.pitch}, {"w.toKey", f(w.toKey)},
        {"w.reverse", w.reverse}, {"w.width", w.width}, {"w.tilt", w.tilt}, {"w.hpHz", w.hpHz}, {"w.duck", w.duck},
        {"weatherSpace", p.weatherSpace}, {"weatherEcho", p.weatherEcho}, {"weatherPan", p.weatherPan},
        {"e.mode", f(d.mode)}, {"e.sync", f(d.sync)}, {"e.timeMs", d.timeMs}, {"e.divBeats", static_cast<float>(d.divBeats)},
        {"e.feedback", d.feedback}, {"e.spread", d.spread}, {"e.lowCutHz", d.lowCutHz}, {"e.highCutHz", d.highCutHz},
        {"e.wow", d.wow}, {"e.drive", d.drive}, {"e.duck", d.duck}, {"e.mix", d.mix}, {"e.glide", f(d.glide)},
        {"e.diffuse", d.diffuse}, {"echoReturn", p.echoReturn}, {"echoSpace", p.echoSpace},
        {"memoryTap", f(p.memoryTap)}, {"split", f(p.split)},
    };
}
// Every field: 0.0.2's and M2's. The macros' checks hold all of them (a macro moves only its own, bit for bit).
std::vector<Field> allFields(const af::Patch& p) {
    std::vector<Field> all = fields(p);
    const std::vector<Field> more = newFields(p);
    all.insert(all.end(), more.begin(), more.end());
    return all;
}

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// The fields' bits folded into one number (FNV-1a over each float's four bytes, in order).
uint64_t foldFields(uint64_t h, const std::vector<Field>& f) {
    for (const Field& x : f) {
        uint32_t bits;
        std::memcpy(&bits, &x.v, sizeof bits);
        for (int k = 0; k < 4; ++k) {
            h ^= (bits >> (8 * k)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
    return h;
}

bool near(float a, float b) { return std::fabs(a - b) <= 1e-4f * std::max(1.0f, std::fabs(b)); }

void testPatchMap() {
    std::printf("== the patch map\n");
    float norm[af::P_COUNT];
    for (int i = 0; i < af::P_COUNT; ++i) norm[i] = af::PARAM_INFO[i].def;
    // The defaults play what the engine's own Patch{} plays: one set of numbers, surface.py's following the
    // engine's headers (the levels and sends there the default knobs squared).
    const af::Patch p = af::patchFromParams(norm), d;
    CHECK(near(p.volumeDb, d.volumeDb) && near(p.tilt, d.tilt) && p.hold == d.hold && p.onStop == d.onStop);
    const af::HarmonyPatch &h = p.harmony, &dh = d.harmony;
    CHECK(h.key == dh.key && h.scale == dh.scale && h.tuning == dh.tuning && h.input == dh.input &&
          h.chord == dh.chord && h.voicing == dh.voicing && h.leading == dh.leading && near(h.strumS, dh.strumS) &&
          h.memoryBars == dh.memoryBars);
    const af::GroundPatch &g = p.ground, &dg = d.ground;
    CHECK(g.listen == dg.listen && g.mute == dg.mute && near(g.level, dg.level) && near(g.cutoffHz, dg.cutoffHz) &&
          g.table == dg.table && near(g.pos.age, dg.pos.age) && near(g.pos.sway, dg.pos.sway) &&
          near(g.pos.swayHz, dg.pos.swayHz) && near(g.pos.smear, dg.pos.smear) && near(g.beatHz, dg.beatHz) &&
          near(g.gravityS, dg.gravityS) && near(g.fadeS, dg.fadeS));
    CHECK(near(g.sub, dg.sub) && near(g.root, dg.root) && near(g.fifth, dg.fifth) && near(g.octave, dg.octave) &&
          near(g.color, dg.color) && g.colorInterval == dg.colorInterval && g.registerOct == dg.registerOct &&
          near(g.body, dg.body) && near(g.breath, dg.breath) && near(g.breathHz, dg.breathHz) &&
          near(g.width, dg.width) && near(p.groundSpace, d.groundSpace) && near(p.groundPan, d.groundPan));
    // Free, as Ground always ran: the Breath Rate's default is the knob's middle and reads back as Ground's own 0.07 Hz
    // bit for bit, so no preset that leaves it breathes any other way; nothing synced.
    CHECK(af::PARAM_INFO[af::P_G_BREATHRATE].def == 0.5f && g.breathHz == dg.breathHz && g.breathHz == 0.07f &&
          g.breathBeats == 0.0f && dg.breathBeats == 0.0f && g.pos.swayBeats == 0.0f && p.bloom.pos.swayBeats == 0.0f);
    const af::BloomPatch &b = p.bloom, &db = d.bloom;
    CHECK(b.listen == db.listen && b.mute == db.mute && near(b.level, db.level) && near(b.cutoffHz, db.cutoffHz) &&
          near(b.reso, db.reso) && b.filterMode == db.filterMode && b.table == db.table && near(b.pos.age, db.pos.age) &&
          near(b.pos.sway, db.pos.sway) && near(b.pos.swayHz, db.pos.swayHz) && near(b.pos.smear, db.pos.smear));
    CHECK(b.tableB == db.tableB && b.bOctave == db.bOctave && near(b.blend, db.blend) && b.couple == db.couple &&
          near(b.coupleAmt, db.coupleAmt) && b.unison == db.unison && near(b.detuneCents, db.detuneCents) &&
          near(b.swellS, db.swellS) && near(b.releaseS, db.releaseS) && near(b.velSens, db.velSens) &&
          near(b.breath, db.breath) && b.tail == db.tail && near(b.width, db.width) &&
          near(p.bloomSpace, d.bloomSpace) && near(p.bloomPan, d.bloomPan));
    const af::Reverb::Params &r = p.space.reverb, &dr = d.space.reverb;
    CHECK(r.mode == dr.mode && near(r.size, dr.size) && near(r.decayS, dr.decayS) && near(r.predelayMs, dr.predelayMs) &&
          near(r.dampHz, dr.dampHz) && near(r.lowCutHz, dr.lowCutHz) && near(r.mod, dr.mod) && near(r.width, dr.width) &&
          r.freeze == dr.freeze && near(r.shimmer, dr.shimmer) && r.shimmerInterval == dr.shimmerInterval &&
          near(p.space.rise, d.space.rise) && near(p.spaceReturn, d.spaceReturn));
    // M2's: every field of Air, Weather and Echo, the sends, the tap and the split, as Patch{} has them.
    {
        const auto a = newFields(p), b = newFields(d);
        bool ok = a.size() == b.size() && a.size() > 60;
        for (size_t i = 0; ok && i < a.size(); ++i)
            if (!near(a[i].v, b[i].v)) {
                std::printf("  %s: the default knobs give %g, Patch{} has %g\n", a[i].name, a[i].v, b[i].v);
                ok = false;
            }
        CHECK(ok);
        // Off in Init, exactly: the levels and every Echo send are 0, not nearly.
        CHECK(p.air.level == 0.0f && p.weather.level == 0.0f && p.groundEcho == 0.0f && p.bloomEcho == 0.0f &&
              p.airEcho == 0.0f && p.weatherEcho == 0.0f && p.split == -1 && !p.weather.memory);
    }

    // Away from the defaults.
    auto with = [&norm](int id, float value) {
        float n[af::P_COUNT];
        std::copy(norm, norm + af::P_COUNT, n);
        n[id] = af::paramNorm(id, value);
        return af::patchFromParams(n);
    };
    // Every option of every list lands on its value in the Patch: option k is value first + k.
    const struct {
        int id;
        int (*field)(const af::Patch&);
        int first;
    } lists[] = {
        {af::P_H_KEY, [](const af::Patch& q) { return q.harmony.key; }, 0},
        {af::P_H_SCALE, [](const af::Patch& q) { return q.harmony.scale; }, 0},
        {af::P_H_TUNING, [](const af::Patch& q) { return q.harmony.tuning; }, 0},
        {af::P_H_INPUT, [](const af::Patch& q) { return q.harmony.input; }, 0},
        {af::P_H_CHORD, [](const af::Patch& q) { return q.harmony.chord; }, 0},
        {af::P_H_VOICING, [](const af::Patch& q) { return q.harmony.voicing; }, 0},
        {af::P_H_LEADING, [](const af::Patch& q) { return static_cast<int>(q.harmony.leading); }, 0},
        {af::P_H_HOLD, [](const af::Patch& q) { return static_cast<int>(q.hold); }, 0},
        {af::P_H_ONSTOP, [](const af::Patch& q) { return q.onStop; }, 0},
        {af::P_G_LISTEN, [](const af::Patch& q) { return q.ground.listen; }, 0},
        {af::P_G_MUTE, [](const af::Patch& q) { return static_cast<int>(q.ground.mute); }, 0},
        {af::P_G_TABLE, [](const af::Patch& q) { return q.ground.table; }, 0},
        {af::P_G_COLINT, [](const af::Patch& q) { return q.ground.colorInterval; }, 0},
        {af::P_G_REG, [](const af::Patch& q) { return q.ground.registerOct; }, 1},
        {af::P_B_LISTEN, [](const af::Patch& q) { return q.bloom.listen; }, 0},
        {af::P_B_MUTE, [](const af::Patch& q) { return static_cast<int>(q.bloom.mute); }, 0},
        {af::P_B_FMODE, [](const af::Patch& q) { return q.bloom.filterMode; }, 0},
        {af::P_B_TABLE, [](const af::Patch& q) { return q.bloom.table; }, 0},
        {af::P_B_TABLEB, [](const af::Patch& q) { return q.bloom.tableB; }, 0},
        {af::P_B_COUPLE, [](const af::Patch& q) { return q.bloom.couple; }, 0},
        {af::P_B_UNISON, [](const af::Patch& q) { return q.bloom.unison; }, 1},
        {af::P_B_TAIL, [](const af::Patch& q) { return q.bloom.tail; }, 0},
        {af::P_S_MODE, [](const af::Patch& q) { return q.space.reverb.mode; }, 0},
        {af::P_S_FREEZE, [](const af::Patch& q) { return static_cast<int>(q.space.reverb.freeze); }, 0},
        {af::P_S_SHINT, [](const af::Patch& q) { return q.space.reverb.shimmerInterval; }, 0},
        {af::P_E_MODE, [](const af::Patch& q) { return q.echo.delay.mode; }, 0},
        {af::P_E_SYNC, [](const af::Patch& q) { return static_cast<int>(q.echo.delay.sync); }, 0},
        {af::P_A_LISTEN, [](const af::Patch& q) { return q.air.listen; }, 0},
        {af::P_A_MUTE, [](const af::Patch& q) { return static_cast<int>(q.air.mute); }, 0},
        {af::P_A_SOUND, [](const af::Patch& q) { return q.air.voice.sound; }, 0},
        {af::P_A_PATTERN, [](const af::Patch& q) { return q.air.gen.pattern; }, 0},
        {af::P_A_REG, [](const af::Patch& q) { return q.air.gen.registerOct; }, 4},
        {af::P_A_LOOP, [](const af::Patch& q) { return static_cast<int>(q.air.gen.loop); }, 0},
        {af::P_W_LISTEN, [](const af::Patch& q) { return q.weather.listen; }, 0},
        {af::P_W_MUTE, [](const af::Patch& q) { return static_cast<int>(q.weather.mute); }, 0},
        {af::P_W_MODE, [](const af::Patch& q) { return q.weather.mode; }, 0},
        {af::P_W_TOKEY, [](const af::Patch& q) { return q.weather.toKey; }, 0},
        {af::P_W_MEMTAP, [](const af::Patch& q) { return q.memoryTap; }, 0},
    };
    for (const auto& l : lists) {
        bool ok = true;
        for (int k = 0; k < af::PARAM_INFO[l.id].nopts; ++k) ok = ok && l.field(with(l.id, static_cast<float>(k))) == l.first + k;
        if (!ok) std::printf("  %s: an option lands on another value\n", af::PARAM_INFO[l.id].key);
        CHECK(ok);
    }
    // Memory's options are bars: Off 0, Forever -1. With it, every list is above.
    const int bars[] = {0, 1, 2, 4, 8, 16, 32, 64, -1};
    bool memory = af::PARAM_INFO[af::P_H_MEMORY].nopts == 9;
    for (int k = 0; memory && k < 9; ++k) memory = with(af::P_H_MEMORY, static_cast<float>(k)).harmony.memoryBars == bars[k];
    // Free / Sync and the divisions: free is 0, synced the division's quarter notes (1/4 = 1 .. 64 bars = 256).
    const struct {
        int sync, div;
        float (*field)(const af::Patch&);
    } synced[] = {
        {af::P_G_BREATHSYNC, af::P_G_BREATHDIV, [](const af::Patch& q) { return q.ground.breathBeats; }},
        {af::P_G_SWAYSYNC, af::P_G_SWAYDIV, [](const af::Patch& q) { return q.ground.pos.swayBeats; }},
        {af::P_B_SWAYSYNC, af::P_B_SWAYDIV, [](const af::Patch& q) { return q.bloom.pos.swayBeats; }},
        {af::P_A_LOOPSYNC, af::P_A_LOOPDIV, [](const af::Patch& q) { return q.air.gen.loopBeats; }},
    };
    bool divs = af::kNumBarDivs == 9 && af::kBarDivBeats[0] == 1.0f && af::kBarDivBeats[2] == 4.0f && af::kBarDivBeats[8] == 256.0f;
    for (const auto& c : synced) {
        divs = divs && af::PARAM_INFO[c.div].nopts == af::kNumBarDivs;
        for (int k = 0; divs && k < af::kNumBarDivs; ++k) {
            float n[af::P_COUNT];
            std::copy(norm, norm + af::P_COUNT, n);
            n[c.div] = af::paramNorm(c.div, static_cast<float>(k));
            divs = divs && c.field(af::patchFromParams(n)) == 0.0f;   // Free: the division waits
            n[c.sync] = 1.0f;
            divs = divs && c.field(af::patchFromParams(n)) == af::kBarDivBeats[k];
        }
    }
    CHECK(divs);
    int synthLists = 0;
    for (int i = 0; i < af::P_COUNT; ++i) synthLists += af::PARAM_INFO[i].kind == af::Kind::Synth && af::PARAM_INFO[i].nopts > 0;
    // Echo Div: option k is kDelayDivs[k], quarter notes (sync or not: the division waits, as Loop Div does not).
    bool echoDivs = af::PARAM_INFO[af::P_E_DIV].nopts == af::kNumDelayDivs;
    for (int k = 0; echoDivs && k < af::kNumDelayDivs; ++k)
        echoDivs = with(af::P_E_DIV, static_cast<float>(k)).echo.delay.divBeats == af::kDelayDivs[k].beats;
    CHECK(echoDivs && std::string(af::kDelayDivs[12].name) == "1/4." && af::kDelayDivs[12].beats == 1.5);
    // Every division menu spells the bars alike: "1 Bar" (Echo's, the LFO list's, the strata's bar divisions).
    CHECK(std::string(af::kDelayDivs[15].name) == "1 Bar" && std::string(af::kLfoDivs[9].name) == "1 Bar" &&
          std::string(af::kBarDivNames[2]) == "1 Bar" && std::string(af::kLfoDivs[10].name) == "2 Bars");
    // Memory and Echo Div are the two lists that are neither a plain enum nor a Free / Sync pair.
    CHECK(memory && synthLists == static_cast<int>(std::size(lists)) + 2 + 2 * static_cast<int>(std::size(synced)));
    // Whole numbers, the taper, the continuous values in their units.
    CHECK(with(af::P_B_BOCT, -2.0f).bloom.bOctave == -2 && with(af::P_B_BOCT, 1.0f).bloom.bOctave == 1);
    CHECK(near(with(af::P_G_LEVEL, 0.5f).ground.level, 0.25f) && with(af::P_B_LEVEL, 0.0f).bloom.level == 0.0f &&
          near(with(af::P_S_RETURN, 1.0f).spaceReturn, 1.0f) && near(with(af::P_G_SPACE, 0.5f).groundSpace, 0.25f) &&
          near(with(af::P_B_SPACE, 0.2f).bloomSpace, 0.04f));
    CHECK(near(with(af::P_G_PAN, -1.0f).groundPan, -1.0f) && near(with(af::P_O_TILT, 0.5f).tilt, 0.5f));
    CHECK(near(with(af::P_S_PREDELAY, 250.0f).space.reverb.predelayMs, 250.0f) &&
          near(with(af::P_G_SWAYRATE, 0.002f).ground.pos.swayHz, 0.002f) &&
          near(with(af::P_B_SWELL, 30.0f).bloom.swellS, 30.0f));

    // M2. The levels and sends of the new strata and of Echo are knobs with the taper, like the others.
    CHECK(near(with(af::P_A_LEVEL, 0.5f).air.level, 0.25f) && near(with(af::P_W_LEVEL, 0.5f).weather.level, 0.25f) &&
          near(with(af::P_A_SPACE, 0.2f).airSpace, 0.04f) && near(with(af::P_W_SPACE, 0.2f).weatherSpace, 0.04f) &&
          near(with(af::P_G_ECHO, 0.5f).groundEcho, 0.25f) && near(with(af::P_B_ECHO, 1.0f).bloomEcho, 1.0f) &&
          near(with(af::P_A_ECHO, 0.5f).airEcho, 0.25f) && near(with(af::P_W_ECHO, 0.5f).weatherEcho, 0.25f) &&
          near(with(af::P_E_RETURN, 0.5f).echoReturn, 0.25f) && near(with(af::P_E_SPACE, 0.5f).echoSpace, 0.25f));
    // Their ranges in their units: the ends of Echo, Air and Weather land where the engine's headers say.
    CHECK(near(with(af::P_E_TIME, 1.0f).echo.delay.timeMs, 1.0f) && near(with(af::P_E_TIME, 2000.0f).echo.delay.timeMs, 2000.0f) &&
          near(with(af::P_E_LOWCUT, 20.0f).echo.delay.lowCutHz, 20.0f) &&
          near(with(af::P_E_HIGHCUT, 20000.0f).echo.delay.highCutHz, 20000.0f) &&
          near(with(af::P_E_FEEDBACK, 1.0f).echo.delay.feedback, 1.0f) &&
          near(with(af::P_A_TONE, 200.0f).air.voice.toneHz, 200.0f) && near(with(af::P_A_DECAY, 20.0f).air.voice.decayS, 20.0f) &&
          near(with(af::P_A_RANGE, 0.5f).air.gen.rangeOct, 0.5f) && near(with(af::P_A_LOOPLEN, 120.0f).air.gen.loopS, 120.0f) &&
          with(af::P_A_MOTIF, 3.0f).air.gen.motif == 3 && with(af::P_A_MOTIF, 8.0f).air.gen.motif == 8 &&
          near(with(af::P_W_SIZE, 0.02f).weather.sizeS, 0.02f) && near(with(af::P_W_SIZE, 2.0f).weather.sizeS, 2.0f) &&
          with(af::P_W_GRAINS, 1.0f).weather.grains == 1 && with(af::P_W_GRAINS, 16.0f).weather.grains == 16 &&
          near(with(af::P_W_PITCH, -24.0f).weather.pitch, -24.0f) && near(with(af::P_W_PITCH, 24.0f).weather.pitch, 24.0f) &&
          near(with(af::P_W_HP, 2000.0f).weather.hpHz, 2000.0f) && near(with(af::P_W_TILT, -1.0f).weather.tilt, -1.0f) &&
          near(with(af::P_A_PAN, -1.0f).airPan, -1.0f) && near(with(af::P_W_PAN, 1.0f).weatherPan, 1.0f));
    // Split: 0 is off (-1 for the engine), else the note itself.
    CHECK(with(af::P_H_SPLIT, 0.0f).split == -1 && with(af::P_H_SPLIT, 1.0f).split == 1 &&
          with(af::P_H_SPLIT, 72.0f).split == 72 && with(af::P_H_SPLIT, 127.0f).split == 127);
    // Air Density: under what the knob reads "off" at the engine gets 0; from there up, the density itself.
    CHECK(with(af::P_A_DENSITY, 0.0f).air.gen.density == 0.0f && with(af::P_A_DENSITY, 0.04f).air.gen.density == 0.0f &&
          near(with(af::P_A_DENSITY, 0.06f).air.gen.density, 0.06f) && near(with(af::P_A_DENSITY, 60.0f).air.gen.density, 60.0f));
    // A loop on the bars is synced to the division, free at Loop Length; Air Reg Low is octave 4.
    {
        float n[af::P_COUNT];
        std::copy(norm, norm + af::P_COUNT, n);
        n[af::P_A_LOOPDIV] = af::paramNorm(af::P_A_LOOPDIV, 2.0f);   // 1 Bar
        const af::Patch free = af::patchFromParams(n);
        n[af::P_A_LOOPSYNC] = 1.0f;
        const af::Patch bar = af::patchFromParams(n);
        CHECK(free.air.gen.loopBeats == 0.0f && bar.air.gen.loopBeats == 4.0f && near(bar.air.gen.loopS, free.air.gen.loopS));
    }
    // The source of Weather is not a knob: no parameter sets weather.memory (the plugin does, with the loader).
    CHECK(!with(af::P_W_MEMTAP, 0.0f).weather.memory && !with(af::P_W_LEVEL, 1.0f).weather.memory);
}

// MPC stores a project's values by index, so a parameter moved or reordered moves saved projects' values (the state's
// keys are no help to MPC's own storage). The first 98 (to b_swaydiv's popup flag) are 0.0.2's, in its order: their keys
// and newlines, FNV-1a-64 (the standard offset basis) over the lot, taken from 0.0.2's params.json; M2's follow them,
// and the preset stepper and the browser come after those (patch_map.cpp holds the numbers as it compiles).
void testIndices() {
    std::printf("== parameter indices\n");
    uint64_t h = 14695981039346656037ull;
    for (int i = 0; i < 98; ++i) {
        for (const char* c = af::PARAM_INFO[i].key; *c; ++c) {
            h ^= static_cast<unsigned char>(*c);
            h *= 1099511628211ull;
        }
        h ^= '\n';
        h *= 1099511628211ull;
    }
    if (h != 0x5b9e4ab6d140675bull) std::printf("  the first 98 keys fold to %016llx\n", static_cast<unsigned long long>(h));
    CHECK(h == 0x5b9e4ab6d140675bull);
    CHECK(af::P_B_SWAYDIV__OPEN == 97 && af::P_E_MODE == 98 && af::P_W_MEMTAP == 159 && af::P_PRESET == 160);
}

// Each of M2's parameters owns the Patch fields it sets, and nothing else. Every one moved alone, from its default to the
// end farthest from it, with the rest at their defaults: exactly its fields change (bit for bit) and every other field
// of the Patch stays as it was. That finds a knob that sets nothing, two that are swapped, and one that moves another's
// field too, which the defaults (many are alike: 30%, 70%, 0) cannot.
void testOwnership() {
    std::printf("== every M2 parameter sets its own Patch fields\n");
    // key -> the field newFields() calls it (nullptr: none).
    static const struct {
        const char* key;
        const char* field;
    } owns[] = {
        {"e_mode", "e.mode"}, {"e_sync", "e.sync"}, {"e_time", "e.timeMs"}, {"e_div", "e.divBeats"},
        {"e_feedback", "e.feedback"}, {"e_lowcut", "e.lowCutHz"}, {"e_highcut", "e.highCutHz"}, {"e_wow", "e.wow"},
        {"e_duck", "e.duck"}, {"e_diffuse", "e.diffuse"}, {"e_return", "echoReturn"}, {"e_space", "echoSpace"},
        {"g_echo", "groundEcho"}, {"b_echo", "bloomEcho"},
        {"a_listen", "a.listen"}, {"a_mute", "a.mute"}, {"a_level", "a.level"}, {"a_tone", "a.toneHz"},
        {"a_sound", "a.sound"}, {"a_decay", "a.decayS"}, {"a_density", "a.density"}, {"a_pattern", "a.pattern"},
        {"a_reg", "a.registerOct"}, {"a_range", "a.rangeOct"}, {"a_gravity", "a.gravity"}, {"a_motif", "a.motif"},
        {"a_mutate", "a.mutate"}, {"a_loop", "a.loop"}, {"a_looplen", "a.loopS"}, {"a_loopsync", "a.loopBeats"},
        {"a_loopdiv", nullptr},   // Free: the division waits for Sync (the synced checks in testPatchMap move both)
        {"a_rubato", "a.rubato"}, {"a_vel", "a.velSens"}, {"a_width", "a.width"}, {"a_space", "airSpace"},
        {"a_echo", "airEcho"}, {"a_pan", "airPan"}, {"h_split", "split"},
        {"w_listen", "w.listen"}, {"w_mute", "w.mute"}, {"w_level", "w.level"}, {"w_mode", "w.mode"},
        {"w_position", "w.position"}, {"w_drift", "w.drift"}, {"w_spray", "w.spray"}, {"w_size", "w.sizeS"},
        {"w_grains", "w.grains"}, {"w_pitch", "w.pitch"}, {"w_tokey", "w.toKey"}, {"w_reverse", "w.reverse"},
        {"w_width", "w.width"}, {"w_tilt", "w.tilt"}, {"w_hp", "w.hpHz"}, {"w_duck", "w.duck"},
        {"w_space", "weatherSpace"}, {"w_echo", "weatherEcho"}, {"w_pan", "weatherPan"}, {"w_memtap", "memoryTap"},
    };
    // The fields no knob sets: Echo's Spread, Drive, Mix and Glide (initEcho()'s) and where Weather's source is Memory.
    static const char* const unexposed[] = {"e.spread", "e.drive", "e.mix", "e.glide", "w.memory"};

    float norm[af::P_COUNT];
    for (int i = 0; i < af::P_COUNT; ++i) norm[i] = af::PARAM_INFO[i].def;
    const auto base = newFields(af::patchFromParams(norm));
    // Every field is some knob's or listed as unexposed, once.
    bool covered = base.size() == std::size(owns) - 1 + std::size(unexposed);
    for (const Field& f : base) {
        int n = 0;
        for (const auto& o : owns) n += o.field && std::strcmp(o.field, f.name) == 0;
        for (const char* u : unexposed) n += std::strcmp(u, f.name) == 0;
        if (n != 1) std::printf("  field %s is claimed %d times\n", f.name, n);
        covered = covered && n == 1;
    }
    CHECK(covered);
    // Every parameter M2 added has a row, and every row a parameter.
    int synth = 0, rows = 0;
    bool listed = true, changed = true;
    for (int i = af::P_E_MODE; i <= af::P_W_MEMTAP; ++i) {
        if (af::PARAM_INFO[i].kind != af::Kind::Synth) continue;
        ++synth;
        const char* key = af::PARAM_INFO[i].key;
        const char* want = nullptr;
        int found = 0;
        for (const auto& o : owns)
            if (std::strcmp(o.key, key) == 0) {
                want = o.field;
                ++found;
            }
        if (found != 1) std::printf("  %s: %d rows\n", key, found);
        listed = listed && found == 1;
        rows += found;
        float n[af::P_COUNT];
        std::copy(norm, norm + af::P_COUNT, n);
        n[i] = af::PARAM_INFO[i].def < 0.5f ? 1.0f : 0.0f;
        const auto now = newFields(af::patchFromParams(n));
        for (size_t f = 0; f < now.size(); ++f) {
            const bool owned = want && std::strcmp(want, now[f].name) == 0;
            if (owned == !sameBits(now[f].v, base[f].v)) continue;
            std::printf("  %s %s %s (%g, was %g)\n", key, owned ? "does not set" : "also sets", now[f].name, now[f].v, base[f].v);
            changed = false;
        }
    }
    CHECK(listed && changed && synth == 58 && rows == synth && std::size(owns) == 58);
}

// The help lines (surface.py "help", shown on the status line after a move): every control a hand moves has one,
// "NAME: what it does", the name as MPC shows it in capitals; tiles, readouts and popup flags have none. That each
// fits the status line is surface.py's check.
void testHelp() {
    std::printf("== parameters: help lines\n");
    int with = 0;
    bool ok = true;
    for (int i = 0; i < af::P_COUNT; ++i) {
        const af::ParamInfo& p = af::PARAM_INFO[i];
        const bool moved = p.kind == af::Kind::Synth || p.kind == af::Kind::Ui || p.kind == af::Kind::Stepper ||
                           p.kind == af::Kind::Button || p.kind == af::Kind::Toggle;
        std::string name = p.name;
        for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const bool good = moved ? p.help && std::string(p.help).compare(0, name.size() + 2, name + ": ") == 0 &&
                                      std::strlen(p.help) > name.size() + 12
                                : p.help == nullptr;
        if (!good) std::printf("  %s: help %s\n", p.key, p.help ? p.help : "(none)");
        ok = ok && good;
        with += p.help != nullptr;
    }
    CHECK(ok && with > 80);
}

// --- the macros ----------------------------------------------------------------------------------

// The fields each macro owns, the way each one goes as the macro rises (+1 up, -1 down), and the range it must
// stay in (the parameter's own; a level or send 0..1 as a gain).
struct Owned {
    const char* field;
    int dir;
    float lo, hi;
};
constexpr float lo(int id) { return af::PARAM_SPECS[id].lo; }
constexpr float hi(int id) { return af::PARAM_SPECS[id].hi; }
struct MacroSpec {
    int id;
    std::vector<Owned> owned;
};
std::vector<MacroSpec> macroSpecs() {
    using namespace af;
    return {
        {P_M_HORIZON, {{"groundSpace", 1, 0, 1}, {"bloomSpace", 1, 0, 1}, {"g.level", -1, 0, 1},
                       {"b.level", -1, 0, 1}, {"r.decayS", 1, lo(P_S_DECAY), hi(P_S_DECAY)},
                       {"r.predelayMs", 1, lo(P_S_PREDELAY), hi(P_S_PREDELAY)}, {"g.cutoffHz", -1, lo(P_G_CUTOFF), hi(P_G_CUTOFF)},
                       {"b.cutoffHz", -1, lo(P_B_CUTOFF), hi(P_B_CUTOFF)}, {"r.dampHz", -1, lo(P_S_DAMP), hi(P_S_DAMP)},
                       {"rise", 1, 0, 1},
                       // M2: the sends of Air, Weather and Echo, and their levels (far steps the dry back)
                       {"airSpace", 1, 0, 1}, {"weatherSpace", 1, 0, 1}, {"groundEcho", 1, 0, 1}, {"bloomEcho", 1, 0, 1},
                       {"airEcho", 1, 0, 1}, {"weatherEcho", 1, 0, 1}, {"a.level", -1, 0, 1}, {"w.level", -1, 0, 1}}},
        {P_M_MOTION, {{"g.sway", 1, 0, 1}, {"b.sway", 1, 0, 1}, {"g.swayHz", 1, lo(P_G_SWAYRATE), hi(P_G_SWAYRATE)},
                      {"b.swayHz", 1, lo(P_B_SWAYRATE), hi(P_B_SWAYRATE)}, {"b.smear", 1, 0, 1}, {"g.breath", 1, 0, 1},
                      {"g.beatHz", 1, lo(P_G_BEAT), hi(P_G_BEAT)}, {"g.breathHz", 1, lo(P_G_BREATHRATE), hi(P_G_BREATHRATE)},
                      {"a.rubato", 1, 0, 1}, {"a.mutate", 1, 0, 1}, {"w.drift", 1, 0, 1}, {"e.wow", 1, 0, 1}}},
        {P_M_GLOW, {{"g.cutoffHz", 1, lo(P_G_CUTOFF), hi(P_G_CUTOFF)}, {"b.cutoffHz", 1, lo(P_B_CUTOFF), hi(P_B_CUTOFF)},
                    {"tilt", 1, lo(P_O_TILT), hi(P_O_TILT)}, {"r.dampHz", 1, lo(P_S_DAMP), hi(P_S_DAMP)},
                    {"r.shimmer", 1, 0, 1}, {"g.body", -1, 0, 1}, {"volumeDb", 1, lo(P_VOLUME), hi(P_VOLUME)},
                    {"a.toneHz", 1, lo(P_A_TONE), hi(P_A_TONE)}, {"w.tilt", 1, lo(P_W_TILT), hi(P_W_TILT)},
                    {"e.highCutHz", 1, lo(P_E_HIGHCUT), hi(P_E_HIGHCUT)}}},
        {P_M_DENSITY, {{"g.sub", 1, 0, 1}, {"g.octave", 1, 0, 1}, {"g.color", 1, 0, 1},
                       {"b.detuneCents", 1, lo(P_B_DETUNE), hi(P_B_DETUNE)}, {"b.breath", 1, 0, 1},
                       {"strumS", -1, lo(P_H_STRUM), hi(P_H_STRUM)}, {"volumeDb", -1, lo(P_VOLUME), hi(P_VOLUME)},
                       {"a.density", 1, lo(P_A_DENSITY), hi(P_A_DENSITY)}, {"w.grains", 1, lo(P_W_GRAINS), hi(P_W_GRAINS)}}},
    };
}

af::Macros only(int id, float x) {
    af::Macros m;
    (id == af::P_M_HORIZON ? m.horizon : id == af::P_M_MOTION ? m.motion : id == af::P_M_GLOW ? m.glow : m.density) = x;
    return m;
}

// Three presets with Air, Weather and Echo sounding, for the macros' checks to bend: the factory presets have them
// off (levels and sends at 0, which a macro leaves at 0), so on them the new fields never move. In the middle of the
// ranges, at the top (every bend meets its clamp from below) and near the bottom (from above); the knobs' real values.
std::vector<std::vector<float>> strataOn() {
    const struct {
        int id;
        float mid, top, low;
    } values[] = {
        {af::P_A_LEVEL, 0.6f, 1.0f, 0.1f}, {af::P_W_LEVEL, 0.6f, 1.0f, 0.1f}, {af::P_A_SPACE, 0.5f, 1.0f, 0.1f},
        {af::P_W_SPACE, 0.3f, 1.0f, 0.1f}, {af::P_G_ECHO, 0.5f, 1.0f, 0.1f}, {af::P_B_ECHO, 0.5f, 1.0f, 0.1f},
        {af::P_A_ECHO, 0.5f, 1.0f, 0.1f}, {af::P_W_ECHO, 0.5f, 1.0f, 0.1f}, {af::P_A_TONE, 3000.0f, 16000.0f, 200.0f},
        {af::P_A_DENSITY, 20.0f, 60.0f, 0.5f}, {af::P_A_RUBATO, 0.3f, 1.0f, 0.0f}, {af::P_A_MUTATE, 0.4f, 1.0f, 0.0f},
        {af::P_W_DRIFT, 0.4f, 1.0f, 0.0f}, {af::P_W_GRAINS, 6.0f, 16.0f, 1.0f}, {af::P_W_TILT, -0.2f, 1.0f, -1.0f},
        {af::P_E_HIGHCUT, 4500.0f, 20000.0f, 500.0f}, {af::P_E_WOW, 0.3f, 1.0f, 0.0f},
    };
    std::vector<std::vector<float>> out;
    for (int which = 0; which < 3; ++which) {
        std::vector<float> norm(af::P_COUNT);
        for (int i = 0; i < af::P_COUNT; ++i) norm[static_cast<size_t>(i)] = af::PARAM_INFO[i].def;
        for (const auto& v : values)
            norm[static_cast<size_t>(v.id)] = af::paramNorm(v.id, which == 0 ? v.mid : which == 1 ? v.top : v.low);
        out.push_back(std::move(norm));
    }
    return out;
}

void testMacros() {
    std::printf("== the macros\n");
    // Every factory preset, as the plugin loads it: with its macros at 0 (as saved) it plays the knobs' patch bit for
    // bit, through patchFromParams as the audio thread builds it.
    std::vector<std::vector<float>> presets;   // each one's 0..1
    bool same = true, newOff = true;
    int asBefore = 0;
    uint64_t golden = 1469598103934665603ull;
    for (int k = 0; k < af::kNumFactoryPresets; ++k) {
        Host h;
        CHECK(h.load(af::kFactoryPresets[k].text) == 1);
        std::vector<float> norm(af::P_COUNT);
        for (int i = 0; i < af::P_COUNT; ++i) norm[static_cast<size_t>(i)] = h.get(i);
        for (int id : {af::P_M_HORIZON, af::P_M_MOTION, af::P_M_GLOW, af::P_M_DENSITY})
            CHECK(af::paramValue(id, norm[static_cast<size_t>(id)]) == 0.0f);
        // A preset that leaves Breath Rate and the Syncs alone breathes and sways as it always did: Ground's own
        // 0.07 Hz, bit for bit, nothing on the bars.
        const std::string text = af::kFactoryPresets[k].text;
        if (text.find("g_breathrate=") == std::string::npos && text.find("sync=") == std::string::npos) {
            const af::Patch q = af::patchFromKnobs(norm.data());
            const bool free = sameBits(q.ground.breathHz, af::GroundPatch{}.breathHz) && q.ground.breathBeats == 0.0f &&
                              q.ground.pos.swayBeats == 0.0f && q.bloom.pos.swayBeats == 0.0f;
            if (!free) std::printf("  %s: not as it breathed before Breath Rate\n", af::kFactoryPresets[k].name);
            CHECK(free);
            ++asBefore;
        }
        const af::Patch knobs = af::patchFromKnobs(norm.data());
        const auto a = allFields(af::patchFromParams(norm.data())), b = allFields(knobs);
        for (size_t f = 0; f < a.size(); ++f)
            if (!sameBits(a[f].v, b[f].v)) {
                std::printf("  %s: %s %g with the macros at 0, %g without\n", af::kFactoryPresets[k].name, a[f].name, a[f].v, b[f].v);
                same = false;
            }
        golden = foldFields(golden, fields(knobs));
        // M2's fields are Patch{}'s (the presets of 0.0.2 name none of its parameters), the new strata and every Echo
        // send exactly off.
        const auto fresh = newFields(af::Patch{}), mine = newFields(knobs);
        for (size_t f = 0; f < mine.size(); ++f)
            if (!near(mine[f].v, fresh[f].v)) {
                std::printf("  %s: %s is %g, not Patch{}'s %g\n", af::kFactoryPresets[k].name, mine[f].name, mine[f].v, fresh[f].v);
                newOff = false;
            }
        newOff = newOff && knobs.air.level == 0.0f && knobs.weather.level == 0.0f && knobs.groundEcho == 0.0f &&
                 knobs.bloomEcho == 0.0f && knobs.airEcho == 0.0f && knobs.weatherEcho == 0.0f;
        presets.push_back(std::move(norm));
    }
    CHECK(same && newOff && presets.size() == static_cast<size_t>(af::kNumFactoryPresets) && asBefore >= af::kNumFactoryPresets - 2);
    // The 28 presets give 0.0.2's Patch, bit for bit, apart from M2's fields (just checked at their defaults): the
    // 0.0.2 fields of all of them folded into one number, taken from 0.0.2's patch map (the merge of Tasks 1-8 before
    // this task; the patch map did not change in them). x86 only: the arithmetic behind a field (pow, log, exp) is
    // libm's, and the device's flags fuse multiply-adds.
#if !defined(__arm__)
    CHECK(golden == 0x6c3af5238ae84bbaull);
#endif

    // Air, Weather and Echo are off in every factory preset, and a macro brings none of them in: a level or an Echo send
    // at 0 is 0 (exactly) under every macro, alone at five positions each and in the 16 corners of all four. With a
    // send open the same macros do move it (the sweep below, on the presets with the new strata on).
    {
        std::vector<af::Macros> tries;
        for (int id : {af::P_M_HORIZON, af::P_M_MOTION, af::P_M_GLOW, af::P_M_DENSITY})
            for (float x : {-1.0f, -0.5f, 0.25f, 0.5f, 1.0f}) tries.push_back(only(id, x));
        for (int corner = 0; corner < 16; ++corner) {
            af::Macros m;
            m.horizon = corner & 1 ? 1.0f : -1.0f;
            m.motion = corner & 2 ? 1.0f : -1.0f;
            m.glow = corner & 4 ? 1.0f : -1.0f;
            m.density = corner & 8 ? 1.0f : -1.0f;
            tries.push_back(m);
        }
        bool stays = true;
        for (const auto& norm : presets) {
            const af::Patch knobs = af::patchFromKnobs(norm.data());
            for (const af::Macros& m : tries) {
                af::Patch p = knobs;
                af::applyMacros(p, m);
                stays = stays && p.air.level == 0.0f && p.weather.level == 0.0f && p.groundEcho == 0.0f &&
                        p.bloomEcho == 0.0f && p.airEcho == 0.0f && p.weatherEcho == 0.0f;
            }
        }
        CHECK(stays);
    }
    const std::vector<std::vector<float>> on = strataOn();
    for (const auto& norm : on) presets.push_back(norm);   // after the factory presets: the sweep takes them too

    // Each macro swept from -1 to +1 on every factory preset and on the three presets with the new strata on: its own
    // fields each one way, inside their range; nothing else moves (bit for bit); and each field it owns moves on some
    // preset.
    for (const MacroSpec& spec : macroSpecs()) {
        std::vector<int> movedOn(spec.owned.size(), 0);
        bool others = true, monotonic = true, inRange = true;
        for (const auto& norm : presets) {
            const af::Patch knobs = af::patchFromKnobs(norm.data());
            const auto base = allFields(knobs);
            std::vector<float> last(spec.owned.size(), 0.0f);
            for (int s = -10; s <= 10; ++s) {
                af::Patch p = knobs;
                af::applyMacros(p, only(spec.id, static_cast<float>(s) / 10.0f));
                const auto now = allFields(p);
                for (size_t f = 0; f < now.size(); ++f) {
                    size_t o = 0;
                    while (o < spec.owned.size() && std::strcmp(spec.owned[o].field, now[f].name) != 0) ++o;
                    if (o == spec.owned.size()) {   // not this macro's: untouched
                        if (!sameBits(now[f].v, base[f].v)) {
                            std::printf("  %s at %+.1f moves %s\n", af::PARAM_INFO[spec.id].name, s / 10.0, now[f].name);
                            others = false;
                        }
                        continue;
                    }
                    const Owned& w = spec.owned[o];
                    if (now[f].v < w.lo || now[f].v > w.hi) {
                        std::printf("  %s at %+.1f: %s %g outside %g..%g\n", af::PARAM_INFO[spec.id].name, s / 10.0, w.field, now[f].v, w.lo, w.hi);
                        inRange = false;
                    }
                    if (s > -10 && (now[f].v - last[o]) * static_cast<float>(w.dir) < 0.0f) {
                        std::printf("  %s at %+.1f: %s turns back (%g after %g)\n", af::PARAM_INFO[spec.id].name, s / 10.0, w.field, now[f].v, last[o]);
                        monotonic = false;
                    }
                    if (s == 0 && !sameBits(now[f].v, base[f].v)) monotonic = false;   // 0: as the knobs have it
                    if (!sameBits(now[f].v, base[f].v)) ++movedOn[o];
                    last[o] = now[f].v;
                }
            }
        }
        bool allMove = true;
        for (size_t o = 0; o < spec.owned.size(); ++o)
            if (!movedOn[o]) {
                std::printf("  %s never moves %s\n", af::PARAM_INFO[spec.id].name, spec.owned[o].field);
                allMove = false;
            }
        CHECK(others && monotonic && inRange && allMove);
    }

    // The amounts, on the middle one of the presets with the new strata on (Air Level 0.6, Tone 3 kHz, Density 20 a
    // minute, Grains 6, every send at 0.5, Wow 0.3, Rubato 0.3, Mutate 0.4, Drift 0.4, Weather Tilt -0.2, Echo High Cut
    // 4.5 kHz): the numbers of the plan's Task 11, each at the ends (Density also at -50%).
    {
        const af::Patch mid = af::patchFromKnobs(on[0].data());
        const auto bent = [&mid](int id, float x) {
            af::Patch p = mid;
            af::applyMacros(p, only(id, x));
            return p;
        };
        const af::Patch hFar = bent(af::P_M_HORIZON, 1.0f), hNear = bent(af::P_M_HORIZON, -1.0f);
        const float s = mid.groundEcho;   // 0.25: every send at 0.5, a gain
        CHECK(near(mid.airEcho, 0.25f) && near(mid.air.level, 0.36f) && near(mid.airSpace, 0.25f));
        // Sends: x 2^(0.5 h) far, x 2^h near; the four Echo sends and both Space sends alike. Levels: x 2^(-0.5 h) far only.
        CHECK(near(hFar.airSpace, s * std::sqrt(2.0f)) && near(hFar.groundEcho, s * std::sqrt(2.0f)) &&
              near(hFar.bloomEcho, s * std::sqrt(2.0f)) && near(hFar.airEcho, s * std::sqrt(2.0f)) &&
              near(hFar.weatherEcho, s * std::sqrt(2.0f)) && near(hFar.weatherSpace, mid.weatherSpace * std::sqrt(2.0f)));
        CHECK(near(hNear.airSpace, s * 0.5f) && near(hNear.groundEcho, s * 0.5f) && near(hNear.bloomEcho, s * 0.5f) &&
              near(hNear.airEcho, s * 0.5f) && near(hNear.weatherEcho, s * 0.5f) && near(hNear.weatherSpace, mid.weatherSpace * 0.5f));
        CHECK(near(hFar.air.level, mid.air.level / std::sqrt(2.0f)) && near(hFar.weather.level, mid.weather.level / std::sqrt(2.0f)) &&
              hNear.air.level == mid.air.level && hNear.weather.level == mid.weather.level);
        // Echo's own return and its Space send are not sends into it: Horizon leaves them.
        CHECK(hFar.echoReturn == mid.echoReturn && hFar.echoSpace == mid.echoSpace && hNear.echoReturn == mid.echoReturn &&
              hNear.echoSpace == mid.echoSpace);
        // Motion: toward 1 by 60%, 50%, 70%, 50%; toward 0 at still, all the way.
        const af::Patch mMove = bent(af::P_M_MOTION, 1.0f), mStill = bent(af::P_M_MOTION, -1.0f);
        CHECK(near(mMove.air.gen.rubato, 0.3f + 0.7f * 0.6f) && near(mMove.air.gen.mutate, 0.4f + 0.6f * 0.5f) &&
              near(mMove.weather.drift, 0.4f + 0.6f * 0.7f) && near(mMove.echo.delay.wow, 0.3f + 0.7f * 0.5f));
        CHECK(mStill.air.gen.rubato == 0.0f && mStill.air.gen.mutate == 0.0f && mStill.weather.drift == 0.0f &&
              mStill.echo.delay.wow == 0.0f);
        // Glow: Air Tone x 2^(2 g), Weather Tilt + 0.3 g, Echo High Cut x 2^g.
        const af::Patch gBright = bent(af::P_M_GLOW, 1.0f), gDark = bent(af::P_M_GLOW, -1.0f);
        CHECK(near(gBright.air.voice.toneHz, 12000.0f) && near(gBright.weather.tilt, 0.1f) && near(gBright.echo.delay.highCutHz, 9000.0f));
        CHECK(near(gDark.air.voice.toneHz, 750.0f) && near(gDark.weather.tilt, -0.5f) && near(gDark.echo.delay.highCutHz, 2250.0f));
        // Density: Air Density and Grains x 2^d thick, x (1 + d) sparse, Grains rounded and never under 1.
        const af::Patch dThick = bent(af::P_M_DENSITY, 1.0f), dHalf = bent(af::P_M_DENSITY, -0.5f), dSparse = bent(af::P_M_DENSITY, -1.0f);
        CHECK(near(dThick.air.gen.density, 40.0f) && dThick.weather.grains == 12 && near(dHalf.air.gen.density, 10.0f) &&
              dHalf.weather.grains == 3 && dSparse.air.gen.density == 0.0f && dSparse.weather.grains == 1);
        // At the top of its range Air Density stays 60 and Grains 16 (thick), and Tone 16 kHz, Weather Tilt 1, High
        // Cut 20 kHz (bright): capped, not wrapped or overshot.
        af::Patch top = af::patchFromKnobs(on[1].data());
        af::applyMacros(top, af::Macros{0.0f, 0.0f, 1.0f, 1.0f});
        CHECK(top.air.gen.density == 60.0f && top.weather.grains == 16 && top.air.voice.toneHz == 16000.0f &&
              top.weather.tilt == 1.0f && top.echo.delay.highCutHz == 20000.0f);
        // And at the bottom, dark and sparse: Tone 200 Hz, Weather Tilt -1, High Cut 500 Hz, Grains 1.
        af::Patch low = af::patchFromKnobs(on[2].data());
        af::applyMacros(low, af::Macros{0.0f, 0.0f, -1.0f, -1.0f});
        CHECK(low.air.voice.toneHz == 200.0f && low.weather.tilt == -1.0f && low.echo.delay.highCutHz == 500.0f &&
              low.weather.grains == 1 && low.air.gen.density == 0.0f);
    }

    // Off is off under the macros, heard. Air, Weather and Echo are off in every factory preset, and the new bends move
    // their other fields all the same (Air Tone, Rubato and Mutate, Weather Drift and Tilt, Echo Wow and High Cut, the
    // Space sends of a silent Air, Grains): none of it may be heard. A chord, two seconds through the engine itself
    // with the macros at +-1 in four mixes: the patch as the macros make it and the same patch with every field the new
    // bends touch put back as the knobs have it give the same samples, bit for bit. (The control: let Air in, and the
    // render differs, so the comparison can hear a stratum.)
    {
        static af::TableSet tables;   // the sine in every slot: it is the engine's arithmetic that is compared
        const auto play = [](const af::Patch& p, std::vector<float>& L, std::vector<float>& R) {
            af::Engine e(tables);
            e.seed(1);
            e.setPatch(p);
            for (int note : {48, 55, 64, 67}) e.noteOn(note, 100);
            const size_t n = 2 * static_cast<size_t>(af::kRate);
            L.assign(n, 0.0f);
            R.assign(n, 0.0f);
            for (size_t b = 0; b < n; b += 128) e.render(&L[b], &R[b], static_cast<int>(std::min<size_t>(128, n - b)));
        };
        // p with everything the new bends touch as k has it: the old bends alone.
        const auto oldBends = [](af::Patch p, const af::Patch& k) {
            p.air = k.air;
            p.weather = k.weather;
            p.echo = k.echo;
            p.airSpace = k.airSpace;
            p.weatherSpace = k.weatherSpace;
            p.groundEcho = k.groundEcho;
            p.bloomEcho = k.bloomEcho;
            p.airEcho = k.airEcho;
            p.weatherEcho = k.weatherEcho;
            return p;
        };
#if defined(__arm__)
        // qemu is slow (a 40 s phrase takes 7.6 s): the five presets testFactory plays there, two mixes.
        const std::vector<std::string> only = {"Init", "Harbour at 4am", "Fifth Light", "Sine Garden", "Choir in Haze"};
        const std::initializer_list<int> mixes = {15, 0};
#else
        const std::vector<std::string> only;   // every one
        const std::initializer_list<int> mixes = {15, 0, 5, 10};
#endif
        int compared = 0, played = 0;
        bool silent = true, control = false;
        for (int i = 0; i < af::kNumFactoryPresets; ++i) {
            if (!only.empty() && std::find(only.begin(), only.end(), std::string(af::kFactoryPresets[i].name)) == only.end()) continue;
            ++played;
            const af::Patch k = af::patchFromKnobs(presets[static_cast<size_t>(i)].data());
            for (int mix : mixes) {   // Horizon, Motion, Glow, Density: bit set = +1, clear = -1
                af::Macros m;
                m.horizon = mix & 1 ? 1.0f : -1.0f;
                m.motion = mix & 2 ? 1.0f : -1.0f;
                m.glow = mix & 4 ? 1.0f : -1.0f;
                m.density = mix & 8 ? 1.0f : -1.0f;
                af::Patch bent = k;
                af::applyMacros(bent, m);
                const af::Patch old = oldBends(bent, k);
                std::vector<float> aL, aR, bL, bR;
                play(bent, aL, aR);
                play(old, bL, bR);
                const bool same = aL == bL && aR == bR;
                if (!same) std::printf("  %s, macros %d: the new bends are heard\n", af::kFactoryPresets[i].name, mix);
                silent = silent && same;
                ++compared;
                if (played == 1 && mix == 15) {
                    af::Patch air = old;   // Air let in, with keys for it above the split
                    air.air.level = 0.5f;
                    air.split = 60;
                    std::vector<float> cL, cR;
                    play(air, cL, cR);
                    control = cL != bL || cR != bR;
                }
            }
        }
        CHECK(silent && control && played == (only.empty() ? af::kNumFactoryPresets : static_cast<int>(only.size())) &&
              compared == played * static_cast<int>(mixes.size()));
    }

    // Bloom Tone moves with Glow and Horizon only on a low-pass: on a band-pass or a high-pass its cutoff picks a band
    // (Overtone Choir's whistle), and stays bit for bit where the preset has it at either end of either macro.
    for (int mode : {af::FM_LP, af::FM_BP, af::FM_HP}) {
        af::Patch knobs;
        knobs.bloom.filterMode = mode;
        knobs.bloom.cutoffHz = 1200.0f;
        bool still = true;
        for (int id : {af::P_M_GLOW, af::P_M_HORIZON})
            for (float x : {-1.0f, 1.0f}) {
                af::Patch p = knobs;
                af::applyMacros(p, only(id, x));
                still = still && sameBits(p.bloom.cutoffHz, knobs.bloom.cutoffHz);
            }
        if (still != (mode != af::FM_LP)) std::printf("  Bloom Tone on %s: %s\n", af::kFilterModeNames[mode], still ? "still" : "moved");
        CHECK(still == (mode != af::FM_LP));
    }

    // Under half a percent a macro is 0 (kBipolarZero), as its knob reads: at 0.501 of its range (+0.2%) the patch is
    // the knobs' bit for bit, at 0.503 (+0.6%) it moves. And over every float from 0.497 to 0.503 (Glow's; the four
    // share the code), the knob shows "0%" exactly where the macro is 0.
    for (int start = 0; start < 2; ++start) {   // Init, and the presets with the new strata on (their bends live in the dead zone too)
        Host d;
        std::vector<float> norm(af::P_COUNT);
        for (int i = 0; i < af::P_COUNT; ++i) norm[static_cast<size_t>(i)] = start == 0 ? d.get(i) : on[0][static_cast<size_t>(i)];
        const auto same = [&norm]() {
            const auto a = allFields(af::patchFromParams(norm.data())), b = allFields(af::patchFromKnobs(norm.data()));
            for (size_t f = 0; f < a.size(); ++f)
                if (!sameBits(a[f].v, b[f].v)) return false;
            return true;
        };
        bool dead = true, alive = true, agree = true;
        for (int id : {af::P_M_HORIZON, af::P_M_MOTION, af::P_M_GLOW, af::P_M_DENSITY}) {
            float& n = norm[static_cast<size_t>(id)];
            n = 0.501f;
            dead = dead && same();
            n = 0.503f;
            alive = alive && !same();
            for (n = 0.497f; id == af::P_M_GLOW && n <= 0.503f; n = std::nextafter(n, 1.0f)) {
                const af::Macros m = af::macrosFromParams(norm.data());
                const float v = id == af::P_M_HORIZON ? m.horizon : id == af::P_M_MOTION ? m.motion
                                : id == af::P_M_GLOW ? m.glow : m.density;
                if ((af::paramDisplay(id, n) == "0%") != (v == 0.0f)) {
                    std::printf("  %s at %.9g: shows %s, the macro %g\n", af::PARAM_INFO[id].name, n,
                                af::paramDisplay(id, n).c_str(), v);
                    agree = false;
                    break;
                }
            }
            n = 0.5f;
        }
        CHECK(dead && alive && agree);
    }

    // Through the plugin: a macro bends the sound, not the knobs. The parameters it bends read and show as they were.
    Host h;
    const float cutoff = h.get(af::P_B_CUTOFF);
    const std::string shown = h.display(af::P_B_CUTOFF);
    h.set(af::P_M_GLOW, 1.0f);
    h.run(2);
    CHECK(h.display(af::P_M_GLOW) == "+100%" && h.get(af::P_B_CUTOFF) == cutoff && h.display(af::P_B_CUTOFF) == shown);
    h.set(af::P_M_HORIZON, -0.5f);
    CHECK(h.display(af::P_M_HORIZON) == "-50%");
}

// Each sound value at 0 and at 1 while a chord sounds, a quarter of a second each, one after another and back
// between (Swell at its shortest, so the chord sounds from the start): every sample finite.
void testExtremes() {
    std::printf("== every sound value at both ends, a chord sounding\n");
    Host h;
    h.set(af::P_B_SWELL, 0.005f);
    for (int note : {48, 55, 64, 67}) h.on(note, 100);
    h.run(kBlocksPerSec / 4);
    bool finite = true;
    float worst = 0.0f;
    for (int i = 0; i < af::P_COUNT; ++i) {
        if (af::PARAM_INFO[i].kind != af::Kind::Synth) continue;
        const float was = h.get(i);
        for (float end : {0.0f, 1.0f}) {
            h.setN(i, end);
            worst = std::max(worst, h.run(kBlocksPerSec / 4));
            if (!h.finite) std::printf("  %s at %g: not finite\n", af::PARAM_INFO[i].key, end);
            finite = finite && h.finite;
            h.finite = true;
        }
        h.setN(i, was);
    }
    worst = std::max(worst, h.run(kBlocksPerSec / 2));
    CHECK(finite && h.finite);
    std::printf("  peak %.2f\n", worst);
}

// M2's sound values at both ends with every stratum and Echo on: Air generating at 60 a minute in a loop, the keys
// above Split striking it, every send into Echo open at a feedback that holds, a chord under it. (Weather has no
// source until the loader gives it one: the engine's tests play it over fields and Memory.) Each new value at 0 and at
// 1 for a quarter of a second, one after another and back between: every sample finite, and the output under the
// limiter's ceiling.
void testExtremesM2() {
    std::printf("== every M2 sound value at both ends, every stratum and Echo on\n");
    Host h;
    h.set(af::P_B_SWELL, 0.005f);
    for (int id : {af::P_A_LEVEL, af::P_W_LEVEL, af::P_G_ECHO, af::P_B_ECHO, af::P_A_ECHO, af::P_W_ECHO, af::P_E_RETURN,
                   af::P_E_SPACE})
        h.setN(id, 1.0f);
    h.set(af::P_A_DENSITY, 60.0f);
    h.set(af::P_A_LOOP, 1.0f);
    h.set(af::P_A_DECAY, 20.0f);
    h.set(af::P_E_FEEDBACK, 0.9f);
    h.set(af::P_E_DIFFUSE, 1.0f);
    h.set(af::P_H_SPLIT, 72.0f);
    for (int note : {48, 55, 64, 67, 76}) h.on(note, 100);
    h.run(kBlocksPerSec / 2);
    bool finite = true;
    float worst = 0.0f;
    int swept = 0;
    for (int i = af::P_E_MODE; i <= af::P_W_MEMTAP; ++i) {
        if (af::PARAM_INFO[i].kind != af::Kind::Synth) continue;
        ++swept;
        const float was = h.get(i);
        for (float end : {0.0f, 1.0f}) {
            h.setN(i, end);
            worst = std::max(worst, h.run(kBlocksPerSec / 4));
            if (!h.finite) std::printf("  %s at %g: not finite\n", af::PARAM_INFO[i].key, end);
            finite = finite && h.finite;
            h.finite = true;
        }
        h.setN(i, was);
    }
    worst = std::max(worst, h.run(kBlocksPerSec / 2));
    CHECK(swept == 58 && finite && h.finite && worst <= 0.8913f);   // the limiter's ceiling
    std::printf("  %d values, peak %.2f\n", swept, worst);
}

} // namespace

void paramsTests() {
    testDefaults();
    testFormats();
    testPopups();
    testHelp();
    testIndices();
    testPatchMap();
    testOwnership();
    testMacros();
    testExtremes();
    testExtremesM2();
}

} // namespace aft
