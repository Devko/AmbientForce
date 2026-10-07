#pragma once
// The demo phrase and the loudness the factory presets are matched by: tools/demos.cpp renders and
// level-matches them with these, test/preset_test.cpp checks that they stay matched. (EffectForce's
// tools/loudness.h does the same for its presets.)
//
// The phrase, for an ambient instrument: one chord held 12 s, a second chord held 12 s, then 16 s
// of release, 40 s in all at 120 BPM, the transport playing. Each preset plays it in its own key and
// scale, so it always sounds as the preset is meant to, and the same way, so the levels compare:
// - the first chord on the tonic, in the octave from C3 to B3 (MIDI 48..59, under middle C: where a
//   pad's left hand plays, and where a Spread voicing's bass, an octave down, is still a bass);
// - the second on IV, a fourth above (in Lydian, whose IV is diminished, on II, its own colour; in
//   the five- and six-tone scales on the tone a fourth above, or the nearest one under it);
// - one key per chord, the preset's Chord type building on it; with Chord Off three keys, the
//   triad's root, fifth and tenth (an open triad);
// - keys mapped back through Input: As Played and Snap get the scale's own notes, Degrees the
//   white keys of the degrees;
// - velocity 100; the first chord's keys go up at 12 s in the same sample as the second's go
//   down, the ups first (a key the two share is struck again rather than held through).
// The loudness is measured over the whole 40 s: ITU-R BS.1770 / EBU R128 integrated loudness of the
// stereo pair (K-weighting as libebur128 derives it, 400 ms blocks every 100 ms, the -70 LUFS and
// -10 LU gates).
#include "../dsp/harmony.h"
#include "../plugin/patch_map.h"
#include "../plugin/tables.h"
#include "../plugin/vst2.h"
#include "param_ids.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace afl {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kBlock = 128;               // MPC's block
constexpr double kBpm = 120.0;
constexpr double kHoldS = 12.0;           // each chord
constexpr double kReleaseS = 16.0;        // after the second
constexpr double kPhraseS = 2.0 * kHoldS + kReleaseS;
constexpr int kVelocity = 100;
constexpr double kTargetLufs = -16.0;     // the family's level (CONCEPT.md 10)
constexpr double kTolLu = 1.0;            // what test/preset_test.cpp allows either side of it
constexpr double kPeakCap = 0.89125;      // -1 dBFS: no factory preset passes it on its phrase

// The real value a preset's text gives the parameter, or its default: as the plugin reads it
// (plugin/state.cpp: the last line naming it wins; clamped and rounded by the patch map).
inline float presetValue(const std::string& text, int id) {
    const std::string key = std::string("\n") + af::PARAM_INFO[id].key + "=";
    float norm = af::PARAM_INFO[id].def;
    for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 1))
        norm = af::paramNorm(id, std::strtof(text.c_str() + at + key.size(), nullptr));
    return af::paramValue(id, norm);
}

struct Event {
    size_t at;            // sample
    uint8_t status, note, velocity;
};

// The phrase as MIDI for a preset (its text), in the order to send it.
inline std::vector<Event> phrase(const std::string& text) {
    af::HarmonyPatch h;
    h.key = static_cast<int>(presetValue(text, af::P_H_KEY));
    h.scale = static_cast<int>(presetValue(text, af::P_H_SCALE));
    h.input = static_cast<int>(presetValue(text, af::P_H_INPUT));
    h.chord = static_cast<int>(presetValue(text, af::P_H_CHORD));
    const int size = af::scaleSize(h.scale);
    // The second chord's degree.
    int second = 3;
    if (h.scale == af::SC_LYDIAN) second = 1;
    else if (h.scale == af::SC_CHROMATIC) second = 5;
    else if (size != 7) {
        second = 0;
        for (int d = 1; d < size; ++d)
            if (std::abs(af::scaleStep(h.scale, d) - 5) < std::abs(af::scaleStep(h.scale, second) - 5)) second = d;
    }
    // A degree's key: the scale's note over the tonic, or (Degrees) the white key that plays it.
    constexpr int lowC = 48;   // C3: the tonic's octave
    auto keyOf = [&h](int degree) {
        if (h.input != af::IN_DEGREES) return lowC + h.key + af::scaleStep(h.scale, degree);
        if (h.scale == af::SC_CHROMATIC) return lowC + degree;   // Degrees under Chromatic: as played, moved by the key
        static constexpr int kWhite[7] = {0, 2, 4, 5, 7, 9, 11};
        return lowC + kWhite[degree % 7] + 12 * (degree / 7);
    };
    auto chord = [&](int degree) {
        std::vector<int> keys{keyOf(degree)};
        if (h.chord == af::CH_OFF) {
            keys.push_back(keyOf(degree + 4));          // the fifth
            keys.push_back(keyOf(degree + 2 + size));   // the tenth
        }
        return keys;
    };
    const auto at = [](double s) { return static_cast<size_t>(std::lround(s * kSr)); };
    std::vector<Event> ev;
    const std::vector<int> one = chord(0), two = chord(second);
    for (int k : one) ev.push_back({0, 0x90, static_cast<uint8_t>(k), kVelocity});
    for (int k : one) ev.push_back({at(kHoldS), 0x80, static_cast<uint8_t>(k), 0});
    for (int k : two) ev.push_back({at(kHoldS), 0x90, static_cast<uint8_t>(k), kVelocity});
    for (int k : two) ev.push_back({at(2.0 * kHoldS), 0x80, static_cast<uint8_t>(k), 0});
    return ev;
}

// Every table of the library built: a preset rendered before then would play the sine in a slot
// whose table isn't there yet (plugin/tables.h), and no two renders would be alike. False if they
// aren't built within `limitS` (the builder is started if it isn't running).
inline bool waitForTables(double limitS = 300.0) {
    af::ensureTablesBuilding();
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        bool all = true;
        for (int id = 0; id < af::TB_COUNT; ++id) all = all && af::sharedTables().t[id].load(std::memory_order_acquire);
        if (all) return true;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > limitS) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// Plays the phrase through an open instance (the preset already loaded) the way MPC does: 128-frame
// blocks, each event at its sample, the transport playing at 120 BPM. `time` is what the host's
// callback answers audioMasterGetTime with. L and R: the 40 s.
inline void render(AEffect* e, VstTimeInfo& time, const std::vector<Event>& ev, std::vector<float>& L, std::vector<float>& R) {
    const size_t total = static_cast<size_t>(kPhraseS * kSr);
    L.assign((total + kBlock - 1) / kBlock * kBlock, 0.0f);
    R.assign(L.size(), 0.0f);
    time.sampleRate = kSr;
    time.tempo = kBpm;
    time.timeSigNumerator = time.timeSigDenominator = 4;
    time.flags |= vst::kVstTempoValid | vst::kVstPpqPosValid | vst::kVstTransportPlaying;
    const double samplesPerBeat = kSr * 60.0 / kBpm;
    std::vector<VstMidiEvent> mev;
    std::vector<char> buf;
    size_t next = 0;
    for (size_t pos = 0; pos < L.size(); pos += kBlock) {
        time.samplePos = static_cast<double>(pos);
        time.ppqPos = static_cast<double>(pos) / samplesPerBeat;
        mev.clear();
        for (; next < ev.size() && ev[next].at < pos + kBlock; ++next) {
            VstMidiEvent m{};
            m.type = vst::kVstMidiType;
            m.byteSize = sizeof m;
            m.deltaFrames = static_cast<int32_t>(ev[next].at - std::min(ev[next].at, pos));
            m.midiData[0] = static_cast<char>(ev[next].status);
            m.midiData[1] = static_cast<char>(ev[next].note);
            m.midiData[2] = static_cast<char>(ev[next].velocity);
            mev.push_back(m);
        }
        if (!mev.empty()) {
            buf.assign(sizeof(VstEvents) + mev.size() * sizeof(VstEvent*), 0);
            auto* evs = reinterpret_cast<VstEvents*>(buf.data());
            evs->numEvents = static_cast<int32_t>(mev.size());
            for (size_t k = 0; k < mev.size(); ++k) evs->events[k] = reinterpret_cast<VstEvent*>(&mev[k]);
            e->dispatcher(e, vst::effProcessEvents, 0, 0, evs, 0.0f);
        }
        float* out[2] = {&L[pos], &R[pos]};
        e->processReplacing(e, nullptr, out, kBlock);
    }
    L.resize(total);
    R.resize(total);
}

// --- loudness: ITU-R BS.1770-4 -----------------------------------------------------------------
struct Biquad {
    double b0, b1, b2, a1, a2, z1 = 0.0, z2 = 0.0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// K-weighting at 44.1 kHz (libebur128's formulas): the head's high shelf, then the RLB high-pass.
inline Biquad kShelf() {
    const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    const double k = std::tan(kPi * f0 / kSr), vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    return {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
            2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
}
inline Biquad kHighpass() {
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double k = std::tan(kPi * f0 / kSr), a0 = 1.0 + k / q + k * k;
    return {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
}

// The K-weighted power (both channels summed) of every 400 ms block, a block every 100 ms.
inline std::vector<double> blockPowers(const std::vector<float>& L, const std::vector<float>& R, double blockS = 0.4) {
    Biquad s[2] = {kShelf(), kShelf()}, hp[2] = {kHighpass(), kHighpass()};
    std::vector<double> sq(L.size());
    for (size_t i = 0; i < L.size(); ++i) {
        const double l = hp[0].run(s[0].run(L[i])), r = hp[1].run(s[1].run(R[i]));
        sq[i] = l * l + r * r;
    }
    const size_t block = static_cast<size_t>(blockS * kSr), hop = static_cast<size_t>(0.1 * kSr);
    std::vector<double> z;
    double sum = 0.0;
    for (size_t i = 0; i < std::min(block, sq.size()); ++i) sum += sq[i];
    for (size_t at = 0; at + block <= sq.size(); at += hop) {
        if (at > 0) {   // slide the window by a hop
            for (size_t i = at - hop; i < at; ++i) sum -= sq[i];
            for (size_t i = at - hop + block; i < at + block; ++i) sum += sq[i];
        }
        z.push_back(std::max(sum, 0.0) / static_cast<double>(block));
    }
    return z;
}

inline double loudnessOf(double power) { return -0.691 + 10.0 * std::log10(std::max(power, 1e-20)); }

// Integrated loudness, LUFS (-100: silence).
inline double lufs(const std::vector<float>& L, const std::vector<float>& R) {
    const std::vector<double> z = blockPowers(L, R);
    double sum = 0.0;
    int n = 0;
    for (double v : z)
        if (loudnessOf(v) > -70.0) sum += v, ++n;
    if (!n) return -100.0;
    const double gate = loudnessOf(sum / n) - 10.0;
    sum = 0.0;
    n = 0;
    for (double v : z)
        if (loudnessOf(v) > -70.0 && loudnessOf(v) > gate) sum += v, ++n;
    return n ? loudnessOf(sum / n) : -100.0;
}

inline float peakOf(const std::vector<float>& L, const std::vector<float>& R) {
    float p = 0.0f;
    for (size_t i = 0; i < L.size(); ++i) p = std::max(p, std::max(std::fabs(L[i]), std::fabs(R[i])));
    return p;
}

inline bool allFinite(const std::vector<float>& L, const std::vector<float>& R) {
    for (size_t i = 0; i < L.size(); ++i)
        if (!std::isfinite(L[i]) || !std::isfinite(R[i])) return false;
    return true;
}

} // namespace afl
