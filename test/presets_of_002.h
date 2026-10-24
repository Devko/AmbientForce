#pragma once
// The 28 factory presets of 0.0.2, by name. M2 must leave them sounding as they did: Air, Weather and Echo are off
// in them (their files name none of M2's parameters). So the checks that hold M2 to 0.0.2 cover these and no
// others: the fingerprints of their phrases (test/preset_test.cpp), the patch map's golden hash, the fields at
// Patch{}'s defaults and the exact-0 checks (test/params_test.cpp). A preset added since sounds Air, Weather
// and Echo; it is held to the level contract and the macro sweeps like every preset, not to 0.0.2, and
// does not join this list. The list is checked against af::kFactoryPresets, so a rename in presets/Factory
// fails there instead of shrinking what is pinned.
#include "factory_presets.h"

#include <cstdio>
#include <cstring>

namespace aft {

inline constexpr const char* kPresetsOf002[] = {
    "Init",
    "Low Tide Hum", "Harbour at 4am", "Fifth Light",
    "Lydian Morning", "Slow Aurora", "Felt Room", "Night Ferry",
    "First Snow", "Glass Orchard", "Tape Bloom", "Sine Garden",
    "Lantern Choir", "Distant Ah", "Cathedral Breath", "Choir in Haze",
    "Felt Keys", "Glass Tines", "Ring Bells", "Bare Strings", "Old Tape", "Sub Monolith", "Pulse Drone",
    "Overtone Choir", "Cluster Fog", "Tonic Halo", "Afterglow", "Frozen Sky",
};
inline constexpr int kNumPresetsOf002 = static_cast<int>(sizeof kPresetsOf002 / sizeof kPresetsOf002[0]);

// Is this factory preset one of 0.0.2's?
inline bool isPresetOf002(const char* name) {
    for (const char* of : kPresetsOf002)
        if (std::strcmp(of, name) == 0) return true;
    return false;
}

// How many of the 28 names are not in af::kFactoryPresets exactly once (each is printed): 0 when none is renamed away.
inline int presetsOf002Gone() {
    int gone = 0;
    for (const char* of : kPresetsOf002) {
        int n = 0;
        for (int i = 0; i < af::kNumFactoryPresets; ++i) n += std::strcmp(af::kFactoryPresets[i].name, of) == 0;
        if (n != 1) {
            std::printf("  0.0.2's preset \"%s\" is %s\n", of, n ? "in the factory presets twice" : "gone (renamed?)");
            ++gone;
        }
    }
    return gone;
}

} // namespace aft
