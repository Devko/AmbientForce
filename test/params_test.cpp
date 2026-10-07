// The parameters as surface.py declares them, against the engine they drive: every default and the text it
// reads, the display formats, the popups, the patch map (the defaults play what a Patch{} plays, every option
// lands on its value), and every sound value at both ends of its range while a chord sounds. That the option
// lists are the engine's names is checked as plugin/patch_map.cpp compiles.
#include "host.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

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
    CHECK(popups == 8);   // Key, Scale, Chord, Memory, the three tables, Space Type
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
    int synthLists = 0;
    for (int i = 0; i < af::P_COUNT; ++i) synthLists += af::PARAM_INFO[i].kind == af::Kind::Synth && af::PARAM_INFO[i].nopts > 0;
    CHECK(memory && synthLists == static_cast<int>(std::size(lists)) + 1);
    // Whole numbers, the taper, the continuous values in their units.
    CHECK(with(af::P_B_BOCT, -2.0f).bloom.bOctave == -2 && with(af::P_B_BOCT, 1.0f).bloom.bOctave == 1);
    CHECK(near(with(af::P_G_LEVEL, 0.5f).ground.level, 0.25f) && with(af::P_B_LEVEL, 0.0f).bloom.level == 0.0f &&
          near(with(af::P_S_RETURN, 1.0f).spaceReturn, 1.0f) && near(with(af::P_G_SPACE, 0.5f).groundSpace, 0.25f) &&
          near(with(af::P_B_SPACE, 0.2f).bloomSpace, 0.04f));
    CHECK(near(with(af::P_G_PAN, -1.0f).groundPan, -1.0f) && near(with(af::P_O_TILT, 0.5f).tilt, 0.5f));
    CHECK(near(with(af::P_S_PREDELAY, 250.0f).space.reverb.predelayMs, 250.0f) &&
          near(with(af::P_G_SWAYRATE, 0.002f).ground.pos.swayHz, 0.002f) &&
          near(with(af::P_B_SWELL, 30.0f).bloom.swellS, 30.0f));
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

} // namespace

void paramsTests() {
    testDefaults();
    testFormats();
    testPopups();
    testPatchMap();
    testExtremes();
}

} // namespace aft
