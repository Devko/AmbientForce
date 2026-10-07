// From SubForce test/preset_test.cpp (8846421), sft -> aft; AmbientForce's parameters, RANDOMIZE left out.
// Saved state, presets and the surface: chunk round trips, the preset stepper and buttons, user
// presets, the browser, favorites, stepping (options and popups too), pushing values back to MPC,
// and every factory preset playing.
#include "host.h"
#include "../plugin/presets.h"
#include "factory_presets.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>

namespace aft {
namespace {

// Preset files of our own under the test's preset root: <folder>/<name>.afp for each name.
void presetFiles(const std::string& folder, std::initializer_list<const char*> names) {
    const std::string dir = fixtureDir() + "/presets/" + folder;
    std::filesystem::create_directories(dir);
    float vol = -20.0f;
    for (const char* n : names) std::ofstream(dir + "/" + n + ".afp") << "ambientforce 1\nvolume=" << (vol += 1.0f) << "\n";
}

void testState() {
    std::printf("== saved state\n");
    Host a;
    a.set(af::P_VOLUME, -12.5f);
    const std::string s = a.chunk();
    CHECK(s.compare(0, 15, "ambientforce 1\n") == 0);
    CHECK(s.find("volume=-12.5\n") != std::string::npos);
    // Only the sound is saved: not the browser, the stepper or the readouts.
    CHECK(s.find("fav") == std::string::npos && s.find("cat_") == std::string::npos && s.find("status") == std::string::npos);
    Host b;
    CHECK(b.load(s) == 1);
    for (int i = 0; i < af::P_COUNT; ++i)
        if (af::PARAM_INFO[i].kind == af::Kind::Synth && std::fabs(a.get(i) - b.get(i)) > 1e-5f) {
            std::printf("  %s differs after a round trip\n", af::PARAM_INFO[i].key);
            CHECK(false);
        }
    // Every sound value, each somewhere of its own, comes back from a saved project where it was (options and
    // whole numbers exactly, the rest within the 6 digits a value is saved with).
    {
        Host r;
        uint32_t seed = 777;
        int moved = 0, synth = 0;
        for (int i = 0; i < af::P_COUNT; ++i) {
            if (af::PARAM_INFO[i].kind != af::Kind::Synth) continue;
            ++synth;
            seed = seed * 1664525u + 1013904223u;
            r.setN(i, static_cast<float>(seed >> 8) / 16777216.0f);
            moved += r.get(i) != af::PARAM_INFO[i].def;
        }
        CHECK(moved > synth * 4 / 5);   // a random value now and then lands on the default option
        Host r2;
        CHECK(r2.load(r.chunk()) == 1);
        int same = 0;
        for (int i = 0; i < af::P_COUNT; ++i) {
            if (af::PARAM_INFO[i].kind != af::Kind::Synth) continue;
            if (std::fabs(r.get(i) - r2.get(i)) <= 1e-5f) ++same;
            else std::printf("  %s: %g saved, %g loaded\n", af::PARAM_INFO[i].key, r.value(i), r2.value(i));
        }
        CHECK(same == synth);
    }
    // A project's state changes only what it lists; unknown keys and bad numbers are skipped.
    Host c;
    c.set(af::P_VOLUME, -20.0f);
    CHECK(c.load("ambientforce 1\nnot_a_param=3\nvolume=abc\n") == 1);
    CHECK(std::fabs(c.value(af::P_VOLUME) + 20.0f) < 1e-3f);
    CHECK(c.load("ambientforce 1\nvolume=1,5\n") == 1 && std::fabs(c.value(af::P_VOLUME) + 20.0f) < 1e-3f);
    CHECK(c.load("ambientforce 1\nvolume=-3\n") == 1 && std::fabs(c.value(af::P_VOLUME) + 3.0f) < 1e-3f);
    // Out of range values clamp; BOM and CRLF are fine; other text is refused.
    CHECK(c.load("\xEF\xBB\xBF" "ambientforce 1\r\nvolume=99\r\n") == 1 && std::fabs(c.value(af::P_VOLUME) - 6.0f) < 1e-3f);
    CHECK(c.load("subforce 1\nvolume=0\n") == 0 && c.load("") == 0 && c.load("ambientforce x\n") == 0);
    CHECK(c.e->dispatcher(c.e, vst::effSetChunk, 0, 0, nullptr, 0.0f) == 0);
    // The loaded patch is what plays.
    Host d;
    CHECK(d.load("ambientforce 1\nvolume=-60\n") == 1);
    d.on(57);
    d.run(kBlocksPerSec / 4);
    CHECK(d.run(4) == 0.0f);
    CHECK(d.load("ambientforce 1\nvolume=0\n") == 1);
    d.run(kBlocksPerSec / 4);
    CHECK(d.run(4) > 0.1f);
}

void testPresets() {
    std::printf("== presets\n");
    const std::string root = fixtureDir() + "/presets";
    Host h;
    // INIT loads the Init preset.
    h.set(af::P_VOLUME, -30.0f);
    h.press(af::P_PRE_INIT);
    std::string init;
    CHECK(af::presetText("builtin:Init", init));
    CHECK(std::fabs(h.get(af::P_VOLUME) - af::paramNorm(af::P_VOLUME, std::strtof(init.c_str() + init.find("volume=") + 7, nullptr))) < 1e-5f);
    CHECK(h.display(af::P_PRESET) == "PRESET  Templates / Init");
    // SAVE writes User 001.afp, then User 002, 003; the stepper shows it.
    h.set(af::P_VOLUME, -11.0f);
    h.press(af::P_PRE_SAVE);
    CHECK(std::filesystem::exists(root + "/User/User 001.afp"));
    CHECK(h.display(af::P_PRESET) == "PRESET  User / User 001");
    std::string text;
    CHECK(af::presetText("plugin:User/User 001.afp", text) && text.find("volume=-11\n") != std::string::npos &&
          text.find("preset=") == std::string::npos);
    h.press(af::P_PRE_SAVE);
    h.press(af::P_PRE_SAVE);
    CHECK(std::filesystem::exists(root + "/User/User 003.afp"));
    // Next / previous walk the flat list; the stepper turns one preset per event.
    h.press(af::P_PRE_INIT);
    const auto L = af::presetLibrary().listing();
    const int at = L->find("builtin:Init");
    CHECK(at == 0);   // Init is the first preset of all
    CHECK(static_cast<int>(L->items.size()) >= at + 4);
    h.press(af::P_PRESET_NEXT);
    CHECK(h.display(af::P_PRESET) == "PRESET  " + L->label(L->items[static_cast<size_t>(at + 1)].key));
    h.press(af::P_PRESET_PREV);
    CHECK(h.display(af::P_PRESET) == "PRESET  Templates / Init");
    const float n = h.get(af::P_PRESET);
    h.setN(af::P_PRESET, n + 1.0f / 128.0f);   // one Q-Link detent
    CHECK(h.display(af::P_PRESET) == "PRESET  " + L->label(L->items[static_cast<size_t>(at + 1)].key));
    // A project remembers its preset; a preset file doesn't name one.
    CHECK(h.chunk().find("\npreset=" + L->items[static_cast<size_t>(at + 1)].key + "\n") != std::string::npos);
    // A preset that went missing is ignored: NEXT steps on from where the stepper stood.
    const std::string here = h.display(af::P_PRESET);
    h.load("ambientforce 1\npreset=plugin:User/Gone.afp\n");
    CHECK(h.display(af::P_PRESET) == "PRESET  Gone");
    h.press(af::P_PRESET_NEXT);
    h.run(4);
    CHECK(h.finite && h.display(af::P_PRESET) != "PRESET  Gone" && h.display(af::P_PRESET) != here);

    // PREV at the first preset, NEXT at the last: nothing to load, the edits stay.
    h.press(af::P_PRE_INIT);
    h.set(af::P_VOLUME, -30.0f);
    h.press(af::P_PRESET_PREV);
    CHECK(std::fabs(h.value(af::P_VOLUME) + 30.0f) < 0.01f);
    const std::string lastKey = L->items.back().key;
    h.load("ambientforce 1\npreset=" + lastKey + "\n");
    h.set(af::P_VOLUME, -30.0f);
    h.press(af::P_PRESET_NEXT);
    CHECK(std::fabs(h.value(af::P_VOLUME) + 30.0f) < 0.01f && h.display(af::P_PRESET) == "PRESET  " + L->label(lastKey));
    // A sound from no preset (a project saved without one), then NEXT: the preset after the one the
    // stepper stood on, not the first of all.
    h.press(af::P_PRE_INIT);
    h.press(af::P_PRESET_NEXT);
    h.press(af::P_PRESET_NEXT);   // the third preset
    const std::string third = h.display(af::P_PRESET);
    h.load("ambientforce 1\nvolume=-9\n");
    CHECK(h.display(af::P_PRESET) == "PRESET  -");
    h.press(af::P_PRESET_NEXT);
    CHECK(h.display(af::P_PRESET) == "PRESET  " + L->label(L->items[static_cast<size_t>(at + 3)].key) && third != h.display(af::P_PRESET));

    // User numbers are never reused, even after the newest file is deleted.
    std::filesystem::remove(root + "/User/User 003.afp");
    h.press(af::P_PRE_SAVE);
    CHECK(std::filesystem::exists(root + "/User/User 004.afp") && !std::filesystem::exists(root + "/User/User 003.afp"));

    // A preset file added while MPC runs shows up in a new instance, and when browsing.
    presetFiles("Pads", {"Warm"});
    Host fresh;
    CHECK(af::presetLibrary().listing()->find("plugin:Pads/Warm.afp") >= 0);
    presetFiles("Pads", {"Cold"});
    fresh.setN(af::P_CAT_1, 1.0f);   // a category tap looks at the folders again
    CHECK(af::presetLibrary().listing()->find("plugin:Pads/Cold.afp") >= 0);
    // A file renamed under the listing: the stepper moves past it instead of sticking.
    fresh.press(af::P_PRE_INIT);
    std::filesystem::rename(root + "/Pads/Cold.afp", root + "/Pads/Cool.afp");
    const int items = static_cast<int>(af::presetLibrary().listing()->items.size());
    for (int k = 0; k < items + 2; ++k) fresh.press(af::P_PRESET_NEXT);
    CHECK(fresh.display(af::P_PRESET) != "PRESET  " + L->label("plugin:Pads/Cold.afp"));
}

void testBrowser() {
    std::printf("== preset browser\n");
    presetFiles("Drift", {"Amber", "Birch", "Cedar"});
    Host h;
    h.press(af::P_PRE_INIT);
    h.run(8);
    // Categories: FAVORITES, RECENT, then the factory folders, then the user's (A -> Z).
    CHECK(h.display(af::P_CAT_1) == "FAVORITES" && h.display(af::P_CAT_2) == "RECENT" && h.display(af::P_CAT_3) == "TEMPLATES");
    CHECK(h.get(af::P_CAT_3) > 0.5f);   // Init's category is lit
    CHECK(h.display(af::P_ITEM_1) == "Init" && h.get(af::P_ITEM_1) > 0.5f);
    CHECK(h.display(af::P_ITEM_PAGE) == "PAGE 1 / 1");
    int drift = -1;
    for (int t = 0; t < af::kBrowserCats; ++t)
        if (h.display(af::P_CAT_1 + t) == "DRIFT") drift = af::P_CAT_1 + t;
    CHECK(drift > af::P_CAT_3);
    if (drift < 0) return;
    // A tap on a category shows its presets; a tap on a preset tile loads it.
    h.setN(drift, 1.0f);
    CHECK(h.get(drift) > 0.5f && h.get(af::P_CAT_3) < 0.5f);
    CHECK(h.display(af::P_ITEM_1) == "Amber" && h.display(af::P_ITEM_2) == "Birch" && h.display(af::P_ITEM_3) == "Cedar");
    CHECK(h.display(af::P_ITEM_4).empty());
    h.setN(af::P_ITEM_2, 1.0f);
    CHECK(h.display(af::P_BR_NOW) == "PRESET  Drift / Birch" && h.get(af::P_ITEM_2) > 0.5f);
    CHECK(std::fabs(h.value(af::P_VOLUME) + 18.0f) < 0.01f);   // Birch's volume
    // Favorite: toggled, kept in the data folder, listed under FAVORITES.
    h.setN(af::P_FAV, 1.0f);
    CHECK(h.get(af::P_FAV) > 0.5f);
    CHECK(std::filesystem::exists(fixtureDir() + "/data/preset_favorites.txt"));
    h.setN(af::P_CAT_1, 1.0f);
    CHECK(h.get(af::P_CAT_1) > 0.5f && h.display(af::P_ITEM_1) == "Birch");
    // RECENT: what was loaded, newest first.
    h.setN(af::P_CAT_2, 1.0f);
    CHECK(h.display(af::P_ITEM_1) == "Birch" && h.display(af::P_ITEM_2) == "Init");
    // Random pick: another preset of the category, never the one loaded.
    h.setN(drift, 1.0f);
    for (int k = 0; k < 6; ++k) {
        const std::string before = h.display(af::P_BR_NOW);
        h.press(af::P_RND);
        const std::string now = h.display(af::P_BR_NOW);
        CHECK(now != before && now.compare(0, 15, "PRESET  Drift /") == 0);
    }
    // The plugin pushes the tiles it lit to MPC (audioMasterAutomate) from the audio thread, and
    // tells it to read the texts again.
    h.log.automated.clear();
    h.log.updates = 0;
    h.run(8);
    CHECK(!h.log.automated.empty() && h.log.updates > 0);
}

void testStepping() {
    std::printf("== stepping (Q-Link, data wheel, taps)\n");
    presetFiles("Steps", {"S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8"});
    Host h;
    // An enum moves one option per Q-Link detent, whatever the size of the detent.
    h.set(af::P_H_VOICING, af::VO_SPREAD);
    h.setN(af::P_H_VOICING, h.get(af::P_H_VOICING) - 1.0f / 128.0f);
    CHECK(h.value(af::P_H_VOICING) == af::VO_DROP2);
    // A long list too: a Q-Link turn on Key, as the Force sends it, moves one key per detent (C to D#).
    {
        Turn turn;
        for (int k = 0; k < 3; ++k) h.detent(af::P_H_KEY, +1);
    }
    CHECK(h.value(af::P_H_KEY) == 3.0f && h.display(af::P_H_KEY) == "D#");
    // A tap on an option (its exact value) lands on it.
    h.setN(af::P_H_VOICING, 0.0f);
    CHECK(h.value(af::P_H_VOICING) == af::VO_CLOSE);
    h.setN(af::P_S_MODE, 1.0f);
    CHECK(h.value(af::P_S_MODE) == af::Reverb::ABYSS);
    // The snapped value goes back to MPC.
    h.setN(af::P_G_REG, 0.5f + 0.01f);   // a wheel click off Mid: one step up, to High
    h.log.automated.clear();
    h.run(4);
    CHECK(h.log.automated.count(af::P_G_REG) == 1 && h.log.automated[af::P_G_REG] == 1.0f && h.display(af::P_G_REG) == "High");
    // A popup's list closes when an option is picked ...
    h.setN(af::P_H_SCALE__OPEN, 1.0f);
    CHECK(h.get(af::P_H_SCALE__OPEN) > 0.5f);
    h.setN(af::P_H_SCALE, 3.0f / 11.0f);
    CHECK(h.get(af::P_H_SCALE__OPEN) == 0.0f && h.value(af::P_H_SCALE) == af::SC_LYDIAN);
    // ... and stays open while a Q-Link turns it (as on the device: a turn steps the value, the list stays).
    h.setN(af::P_H_SCALE__OPEN, 1.0f);
    h.detent(af::P_H_SCALE, +1);
    CHECK(h.get(af::P_H_SCALE__OPEN) > 0.5f && h.value(af::P_H_SCALE) == af::SC_MIXOLYDIAN);
    h.setN(af::P_G_TABLE__OPEN, 1.0f);
    h.setN(af::P_G_TABLE, static_cast<float>(af::TB_CHOIR_AH_OO) / (af::TB_COUNT - 1));
    CHECK(h.get(af::P_G_TABLE__OPEN) == 0.0f && h.display(af::P_G_TABLE) == "Choir Ah-Oo");
    // Popup flags are the surface's: never saved.
    CHECK(h.chunk().find("__open") == std::string::npos);
    // Continuous knobs follow MPC as they are.
    h.setN(af::P_VOLUME, 0.37f);
    CHECK(h.get(af::P_VOLUME) == 0.37f);
    // A button reads back 0 (it springs back): every tap arrives as a 1, and each one acts.
    h.press(af::P_PRE_INIT);
    h.log.automated.clear();
    h.press(af::P_PRESET_NEXT);
    h.run(2);
    CHECK(h.log.automated.count(af::P_PRESET_NEXT) == 1 && h.log.automated[af::P_PRESET_NEXT] == 0.0f);
    CHECK(h.get(af::P_PRESET_NEXT) == 0.0f);
    const std::string second = h.display(af::P_PRESET);
    h.press(af::P_PRESET_NEXT);
    CHECK(h.display(af::P_PRESET) != second);
    // A Q-Link turn on the preset stepper, as the Force sends it (the read-back plus 1/128 per
    // detent, a few ms apart): a preset per detent the whole turn long, as many as NEXT taps. (It
    // used to measure each detent from MPC's previous value, and stalled after one preset.)
    {
        Host a;
        a.press(af::P_PRE_INIT);
        const auto L = af::presetLibrary().listing();
        const int at = L->find("builtin:Init");
        auto label = [&L](int k) { return "PRESET  " + L->label(L->items[static_cast<size_t>(k)].key); };
        CHECK(at >= 0 && at + 6 < static_cast<int>(L->items.size()));
        {
            Turn turn;
            for (int k = 0; k < 6; ++k) a.detent(af::P_PRESET, +1);
        }
        CHECK(a.display(af::P_PRESET) == label(at + 6));
        {
            Turn turn;
            for (int k = 0; k < 2; ++k) a.detent(af::P_PRESET, -1);
        }
        CHECK(a.display(af::P_PRESET) == label(at + 4));
        // A data-wheel click (0.01) and a touch drag (0.04) also move one preset each.
        a.setN(af::P_PRESET, a.get(af::P_PRESET) + 0.01f);
        CHECK(a.display(af::P_PRESET) == label(at + 5));
        a.setN(af::P_PRESET, a.get(af::P_PRESET) - 0.04f);
        CHECK(a.display(af::P_PRESET) == label(at + 4));
        // MPC echoing the plugin's own value back moves nothing; the value it reads is pushed.
        a.setN(af::P_PRESET, a.get(af::P_PRESET));
        CHECK(a.display(af::P_PRESET) == label(at + 4));
        a.log.automated.clear();
        a.run(2);
        CHECK(a.log.automated.count(af::P_PRESET) == 0 || std::fabs(a.log.automated[af::P_PRESET] - a.get(af::P_PRESET)) < 1e-6f);
    }
    // A tile tap's release echo (~0.7 s later) is not a second tap; a tap a second later is.
    {
        Host t;
        t.press(af::P_PRE_INIT);   // a preset to favorite
        t.setN(af::P_FAV, 1.0f);
        CHECK(t.get(af::P_FAV) > 0.5f);
        {
            Turn echo(700);
            t.setN(af::P_FAV, 0.0f);
        }
        CHECK(t.get(af::P_FAV) > 0.5f);
        t.setN(af::P_FAV, 0.0f);
        CHECK(t.get(af::P_FAV) < 0.5f);
    }
}

void testFactory() {
    std::printf("== factory presets\n");
    CHECK(af::kNumFactoryPresets >= 1);
    int quiet = 0, loud = 0;
    for (int i = 0; i < af::kNumFactoryPresets; ++i) {
        Host h;
        const std::string key = std::string("builtin:") + af::kFactoryPresets[i].name;
        std::string text;
        CHECK(af::presetText(key, text));
        CHECK(h.load(text) == 1);   // the stepper / browser path is tested above; here, the sound
        h.on(36 + (i * 7) % 24, 100);
        float peak = h.run(kBlocksPerSec);
        h.off(36 + (i * 7) % 24);
        peak = std::max(peak, h.run(kBlocksPerSec / 2));
        if (peak < 0.01f) {
            ++quiet;
            std::printf("  %s: peak %.4f\n", af::kFactoryPresets[i].name, peak);
        }
        if (peak > 1.0f) {
            ++loud;
            std::printf("  %s: peak %.2f\n", af::kFactoryPresets[i].name, peak);
        }
        CHECK(h.finite);
    }
    CHECK(quiet == 0 && loud == 0);
}

} // namespace

void presetTests() {
    testState();
    testPresets();
    testBrowser();
    testStepping();
    testFactory();
}

} // namespace aft
