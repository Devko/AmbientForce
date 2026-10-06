// From SubForce tools/pgo_train.cpp (8846421), renamed; the stub engine's phrases.
// The profile-guided build's trainer (make arm-plugin with PGO): an instrumented copy of the
// plugin is linked in and plays through VSTPluginMain under qemu-arm, the way MPC drives it:
// single notes, chords on every voice and past them (stealing), the pedal, and the factory
// presets. The profile only steers the compiler (which paths are hot); what the trainer leaves
// out is still optimised as usual (-fprofile-partial-training).
#include "../plugin/vst2.h"
#include "factory_presets.h"
#include "param_ids.h"

#include <cstdio>
#include <string>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback);

namespace {

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;   // audioMasterVersion
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

void midi(AEffect* e, uint8_t st, uint8_t d1, uint8_t d2) {
    VstMidiEvent ev{};
    ev.type = vst::kVstMidiType;
    ev.byteSize = sizeof ev;
    ev.midiData[0] = st;
    ev.midiData[1] = d1;
    ev.midiData[2] = d2;
    VstEvents evs{};
    evs.numEvents = 1;
    evs.events[0] = reinterpret_cast<VstEvent*>(&ev);
    e->dispatcher(e, vst::effProcessEvents, 0, 0, &evs, 0.0f);
}

// A short phrase: a note, a chord of `voices` keys held with the pedal, a release.
void phrase(AEffect* e, std::vector<float>& L, std::vector<float>& R, int voices, int blocks) {
    float* out[2] = {L.data(), R.data()};
    midi(e, 0x90, 48, 90);
    for (int b = 0; b < blocks; ++b) e->processReplacing(e, nullptr, out, 128);
    midi(e, 0x80, 48, 0);
    midi(e, 0xB0, 64, 127);
    for (int k = 0; k < voices; ++k) {
        midi(e, 0x90, static_cast<uint8_t>(48 + 4 * k), static_cast<uint8_t>(70 + 5 * k));
        for (int b = 0; b < blocks / 4; ++b) e->processReplacing(e, nullptr, out, 128);
        midi(e, 0x80, static_cast<uint8_t>(48 + 4 * k), 0);
    }
    midi(e, 0xB0, 64, 0);
    for (int b = 0; b < blocks; ++b) e->processReplacing(e, nullptr, out, 128);
}

} // namespace

int main() {
    g_time.sampleRate = 44100.0;
    g_time.tempo = 120.0;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;
    std::vector<float> L(128), R(128);
    int patches = 0;
    AEffect* e = VSTPluginMain(master);
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    for (int voices = 1; voices <= 9; voices += 4) {   // one, five, and more keys than voices
        e->setParameter(e, af::P_VOLUME, 0.6f + 0.1f * static_cast<float>(voices % 3));
        phrase(e, L, R, voices, 60);
        ++patches;
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    // The factory presets, as users will mostly play them: each on a fresh instance (a project
    // chunk only sets what it lists; a preset means everything else at its default).
    for (int i = 0; i < af::kNumFactoryPresets; ++i) {
        AEffect* f = VSTPluginMain(master);
        f->dispatcher(f, vst::effOpen, 0, 0, nullptr, 0.0f);
        const std::string text = af::kFactoryPresets[i].text;
        f->dispatcher(f, vst::effSetChunk, 0, static_cast<intptr_t>(text.size()), const_cast<char*>(text.data()), 0.0f);
        phrase(f, L, R, 6, 50);
        f->dispatcher(f, vst::effClose, 0, 0, nullptr, 0.0f);
        ++patches;
    }
    std::printf("pgo trainer: %d patches\n", patches);
    return 0;
}
