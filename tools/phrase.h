#pragma once
// The demo phrase the factory presets are matched on, and what a render of it must meet:
// tools/demos.cpp renders and level-matches them with these, test/preset_test.cpp checks that they
// stay matched.
//
// The phrase, for an ambient instrument: one chord held 12 s, a second chord held 12 s, then 16 s
// of release, 40 s in all at 120 BPM, the transport playing. Each preset plays it in its own key and
// scale, so it always sounds as the preset is meant to, and the same way, so the levels compare:
// - the first chord on the tonic, in the octave from C3 to B3 (MIDI 48..59, under middle C: where a
//   pad's left hand plays, and where a Spread voicing's bass, an octave down, is still a bass);
// - the second on IV, a fourth above (in Lydian, whose IV is diminished, on II, its own colour; in
//   the other scales on the tone nearest a fourth above, the lower on a tie);
// - one key per chord, the preset's Chord type building on it; with Chord Off three keys, an open
//   triad: the root, the scale's tone nearest a fifth over it (7 semitones) and the one nearest a
//   major third (4), an octave up: a tenth (the lower on a tie, as for IV);
// - keys mapped back through Input: As Played and Snap get the scale's own notes, Degrees the
//   white keys that play the degrees (a scale of fewer than seven tones runs on into the next
//   octave's C, harmony.h's mapInput);
// - velocity 100; the first chord's keys go up at 12 s in the same sample as the second's go
//   down, the ups first (a key the two share is struck again rather than held through).
// The loudness is measured over the whole 40 s: tools/loudness.h's integrated loudness of the stereo
// pair (ITU-R BS.1770 / EBU R128).
#include "loudness.h"
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
// A factory preset is matched by its volume, not by the limiter, which only holds the peaks: on its
// phrase the limiter may take at most this off (`make preset-levels` fails past it) and work on at
// most this share of the samples (test/preset_test.cpp, af::limitedSamples()).
constexpr double kMaxGainReductionDb = 1.0;
constexpr double kMaxLimitedShare = 0.01;

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
    // The degree over `from` (within its octave) whose tone is nearest `semis` above it, the lower
    // on a tie.
    auto nearest = [&h, size](int from, int semis) {
        int best = from;
        auto off = [&](int d) { return std::abs(af::scaleStep(h.scale, d) - af::scaleStep(h.scale, from) - semis); };
        for (int d = from + 1; d < from + size; ++d)
            if (off(d) < off(best)) best = d;
        return best;
    };
    // The second chord's degree: IV, in Lydian II, in the other scales the tone nearest a fourth.
    int second = nearest(0, 5);
    if (size == 7) second = h.scale == af::SC_LYDIAN ? 1 : 3;
    // A degree's key: the scale's note over the tonic, or (Degrees) the white key that plays it.
    constexpr int lowC = 48;   // C3: the tonic's octave
    auto keyOf = [&h, size](int degree) {
        if (h.input != af::IN_DEGREES) return lowC + h.key + af::scaleStep(h.scale, degree);
        if (h.scale == af::SC_CHROMATIC) return lowC + degree;   // Degrees under Chromatic: as played, moved by the key
        static constexpr int kWhite[7] = {0, 2, 4, 5, 7, 9, 11};   // the scales other than Chromatic: 5..7 tones
        return lowC + kWhite[degree % size] + 12 * (degree / size);
    };
    auto chord = [&](int degree) {
        std::vector<int> keys{keyOf(degree)};
        if (h.chord == af::CH_OFF) {
            keys.push_back(keyOf(nearest(degree, 7)));              // the fifth
            keys.push_back(keyOf(nearest(degree, 4) + size));       // the third, an octave up: the tenth
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

// Weather's source loaded (plugin/surface.h): the Source stepper's text no longer ends in " ..." (the
// source is in, or it is Memory, or MISSING). A phrase played before then would have Weather come in
// whenever the instance's loader thread got there, and no two renders would be alike. Every tool that
// renders a preset waits for it after loading one. False if not within `limitS` (ten times that on ARM,
// where the tools run under qemu).
inline bool waitForSource(AEffect* e, double limitS = 60.0) {
#if defined(__arm__)
    limitS *= 10.0;
#endif
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        char b[256] = {};
        e->dispatcher(e, vst::effGetParamDisplay, af::P_W_SOURCE, 0, b, 0.0f);
        const std::string text = b;
        if (text.size() < 4 || text.compare(text.size() - 4, 4, " ...") != 0) return true;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > limitS) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Plays the phrase through an open instance (the preset already loaded) the way MPC does: 128-frame
// blocks, each event at its sample, the transport playing at 120 BPM. `time` is what the host's
// callback answers audioMasterGetTime with. L and R: the 40 s. Weather's source first (waitForSource).
inline void render(AEffect* e, VstTimeInfo& time, const std::vector<Event>& ev, std::vector<float>& L, std::vector<float>& R) {
    waitForSource(e);
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

// --- what a render measures -----------------------------------------------------------------------

// Integrated loudness of the stereo pair, LUFS (-100: silence).
inline double lufs(const std::vector<float>& L, const std::vector<float>& R) {
    loudness::Meter m;
    for (size_t i = 0; i < L.size(); ++i) m.add(L[i], R[i]);
    return m.integrated();
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
