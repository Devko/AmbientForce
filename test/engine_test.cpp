// The engine on its own, for now the stub: silence before a note, a sine at the note's pitch,
// the release, six voices and the seventh note taking the oldest, the pedal, reset.
#include "host.h"
#include "../dsp/engine.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace aft {
namespace {

using af::Engine;

// Renders n samples (in 128-frame blocks, as the plugin does); returns the left channel and
// checks the right one is the same.
std::vector<float> render(Engine& e, size_t n, bool* same = nullptr) {
    std::vector<float> L(n), R(n);
    for (size_t b = 0; b < n; b += 128) {
        const int m = static_cast<int>(std::min<size_t>(128, n - b));
        e.render(&L[b], &R[b], m);
    }
    if (same) *same = L == R;
    return L;
}

float peakOf(const std::vector<float>& x) {
    float p = 0.0f;
    for (float v : x) p = std::max(p, std::fabs(v));
    return p;
}

double hzOf(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

void testSilence() {
    std::printf("== engine: silence\n");
    Engine e;
    e.setPatch(af::Patch{0.0f});
    bool same = false;
    const std::vector<float> x = render(e, 44100, &same);
    CHECK(peakOf(x) == 0.0f && same);   // exactly 0, not just quiet
    CHECK(e.activeVoices() == 0);
}

void testNote() {
    std::printf("== engine: a note\n");
    Engine e;
    e.setPatch(af::Patch{0.0f});
    e.noteOn(69, 100);
    render(e, 4410);   // past the attack
    bool same = false;
    const std::vector<float> x = render(e, 16384, &same);
    const double at = toneAmp(x, 440.0), lo = toneAmp(x, 430.0), hi = toneAmp(x, 450.0);
    std::printf("  440 Hz %.3f, 20 log(440 / 430) %.1f dB, 20 log(440 / 450) %.1f dB\n", at,
                20.0 * std::log10(at / lo), 20.0 * std::log10(at / hi));
    CHECK(20.0 * std::log10(at / lo) >= 20.0 && 20.0 * std::log10(at / hi) >= 20.0);
    CHECK(std::fabs(at - 0.25 * 100.0 / 127.0) < 0.01);   // a voice's level, at 0 dB
    CHECK(same && e.activeVoices() == 1);
    // The volume: -6 dB halves it; -60 is off.
    e.setPatch(af::Patch{-6.0206f});
    render(e, 4410);
    CHECK(std::fabs(toneAmp(render(e, 16384), 440.0) / at - 0.5) < 0.005);
    e.setPatch(af::Patch{-60.0f});
    render(e, 22050);
    CHECK(peakOf(render(e, 4410)) == 0.0f);
    // Velocity 0 is a note-off.
    e.setPatch(af::Patch{0.0f});
    e.noteOn(69, 0);
    render(e, 22050);
    CHECK(e.activeVoices() == 0);
}

void testRelease() {
    std::printf("== engine: attack and release\n");
    Engine e;
    e.setPatch(af::Patch{0.0f});
    e.noteOn(60, 127);
    const std::vector<float> a = render(e, 4410);
    float first = 0.0f, rest = 0.0f;
    for (size_t i = 0; i < 176; ++i) first = std::max(first, std::fabs(a[i]));   // 4 ms into the 10 ms attack
    for (size_t i = 441; i < a.size(); ++i) rest = std::max(rest, std::fabs(a[i]));
    CHECK(first < 0.45f * rest && rest > 0.24f && rest <= 0.25f);
    e.noteOff(60);
    const std::vector<float> r = render(e, 22050);   // 0.5 s
    float tail = 0.0f, mid = 0.0f;
    for (size_t i = 0; i < 4410; ++i) mid = std::max(mid, std::fabs(r[i]));
    for (size_t i = r.size() - 441; i < r.size(); ++i) tail = std::max(tail, std::fabs(r[i]));
    CHECK(mid > 0.1f);      // still sounding 0.1 s into the 300 ms release
    CHECK(tail < 1e-6f);    // gone 0.5 s after the note-off
    CHECK(e.activeVoices() == 0);
}

void testVoices() {
    std::printf("== engine: six voices, the seventh steals\n");
    const int notes[] = {48, 52, 55, 60, 64, 67, 72};
    Engine e;
    e.setPatch(af::Patch{0.0f});
    for (int k = 0; k < 6; ++k) e.noteOn(notes[k], 100);
    render(e, 4410);
    CHECK(e.activeVoices() == 6);
    std::vector<float> x = render(e, 16384);
    bool all = true;
    for (int k = 0; k < 6; ++k) all = all && toneAmp(x, hzOf(notes[k])) > 0.15;
    CHECK(all && toneAmp(x, hzOf(notes[6])) < 0.01);
    CHECK(peakOf(x) < 6 * 0.25f);
    // A seventh note takes the oldest voice (the first note's): still six.
    e.noteOn(notes[6], 100);
    render(e, 4410);
    CHECK(e.activeVoices() == 6);
    x = render(e, 16384);
    all = true;
    for (int k = 1; k < 7; ++k) all = all && toneAmp(x, hzOf(notes[k])) > 0.15;
    CHECK(all && toneAmp(x, hzOf(notes[0])) < 0.01);
    // A key struck again keeps its own voice.
    e.noteOn(notes[3], 100);
    render(e, 4410);
    CHECK(e.activeVoices() == 6);
    // A releasing voice goes before a held one.
    e.noteOff(notes[5]);
    e.noteOn(80, 100);
    render(e, 4410);
    x = render(e, 16384);
    CHECK(toneAmp(x, hzOf(notes[1])) > 0.15 && toneAmp(x, hzOf(notes[5])) < 0.01 && toneAmp(x, hzOf(80)) > 0.15);
    // All notes off releases them all.
    e.allNotesOff();
    render(e, 22050);
    CHECK(e.activeVoices() == 0 && peakOf(render(e, 128)) == 0.0f);
}

void testPedalAndReset() {
    std::printf("== engine: the pedal, reset\n");
    Engine e;
    e.setPatch(af::Patch{0.0f});
    e.sustain(true);
    e.noteOn(60, 100);
    e.noteOff(60);
    render(e, 22050);
    CHECK(e.activeVoices() == 1 && peakOf(render(e, 4410)) > 0.1f);   // held by the pedal
    e.sustain(false);
    render(e, 22050);
    CHECK(e.activeVoices() == 0);
    // A key still down when the pedal comes up keeps sounding.
    e.sustain(true);
    e.noteOn(62, 100);
    e.sustain(false);
    render(e, 22050);
    CHECK(e.activeVoices() == 1);
    // Reset: silent at once, the pedal up.
    e.sustain(true);
    e.reset();
    CHECK(e.activeVoices() == 0 && peakOf(render(e, 128)) == 0.0f);
    e.noteOn(64, 100);
    e.noteOff(64);
    render(e, 22050);
    CHECK(e.activeVoices() == 0);
    // Out-of-range notes and a bad volume are refused, not played.
    e.noteOn(-1, 100);
    e.noteOn(128, 100);
    e.noteOff(500);
    e.setPatch(af::Patch{std::nanf("")});
    render(e, 128);
    e.noteOn(60, 100);
    const std::vector<float> x = render(e, 4410);
    CHECK(e.activeVoices() == 1 && peakOf(x) == 0.0f);
}

} // namespace

void engineTests() {
    testSilence();
    testNote();
    testRelease();
    testVoices();
    testPedalAndReset();
}

} // namespace aft
