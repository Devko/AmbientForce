// From SubForce tools/pgo_train.cpp (8846421), renamed; AmbientForce's patches and phrase.
// The profile-guided build's trainer (make arm-plugin with PGO): an instrumented copy of the
// plugin is linked in and plays through VSTPluginMain under qemu-arm, the way MPC drives it
// (128-frame blocks, MIDI at sample offsets, parameters by index, the transport running). An
// ambient phrase (a chord, another taken over under the pedal, a third, the tail) plays through
// every Space mode (Shimmer at every interval, Freeze), every Couple mode at unison 1 and 2, the
// filter modes, the Listen modes with the harmony's memory, the tunings with the Input modes and
// scales, every chord type and voicing with Strum and Leading, Ground's partials, Body, Breath and
// Register, Bloom's tables and motion, Hold with the pedal, more keys than voices, Tilt and the
// limiter, Stop with Fade and Cut, and the host's suspend (a Stop and a reset). Then every factory
// preset (factory_presets.h: new ones are in as they land) plays the phrase on a fresh instance.
// A couple of seconds of audio each, so it stays quick under qemu. The profile only steers the
// compiler (which paths are hot); what the trainer leaves out is still optimised as usual
// (-fprofile-partial-training).
//
// The tables first: the trainer waits until the builder thread the first instance starts has
// published every one, so the voices read lifetime tables, not the one-frame sine every slot plays
// until then.
#include "../plugin/patch_map.h"
#include "../plugin/surface.h"
#include "../plugin/tables.h"
#include "../plugin/vst2.h"
#include "factory_presets.h"
#include "param_ids.h"

#include <cstdio>
#include <ctime>
#include <string>

extern "C" AEffect* VSTPluginMain(audioMasterCallback);

namespace {

using namespace af;

constexpr int kBlock = 128;
constexpr double kSr = 44100.0;
constexpr double kPhraseS = 2.0;   // the phrase's length

VstTimeInfo g_time{};
// The surface's clock (Surface::clock, which the plugin's suspend times read too): a second on at
// every parameter, so no two sets read as one gesture, however fast qemu runs.
long long g_ms = 1;
long long clockMs() { return g_ms; }
long long g_samples = 0;   // rendered, for the summary

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;   // audioMasterVersion
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

struct Player {
    AEffect* e = VSTPluginMain(master);
    float L[kBlock] = {}, R[kBlock] = {};

    Player() { e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f); }
    ~Player() { e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f); }
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    void set(int id, float v) {   // a real value: an option's index, a level, Hz, seconds
        g_ms += 1000;
        e->setParameter(e, id, paramNorm(id, v));
    }
    void press(int id) {          // a button: every 1 is a press
        g_ms += 1000;
        e->setParameter(e, id, 1.0f);
    }
    void midi(uint8_t st, uint8_t d1, uint8_t d2, int delta = 0) {
        VstMidiEvent ev{};
        ev.type = vst::kVstMidiType;
        ev.byteSize = sizeof ev;
        ev.deltaFrames = delta;
        ev.midiData[0] = st;
        ev.midiData[1] = d1;
        ev.midiData[2] = d2;
        VstEvents evs{};
        evs.numEvents = 1;
        evs.events[0] = reinterpret_cast<VstEvent*>(&ev);
        e->dispatcher(e, vst::effProcessEvents, 0, 0, &evs, 0.0f);
    }
    void on(int note, int vel = 96, int delta = 0) { midi(0x90, static_cast<uint8_t>(note), static_cast<uint8_t>(vel), delta); }
    void off(int note, int delta = 0) { midi(0x80, static_cast<uint8_t>(note), 0, delta); }
    void pedal(bool down) { midi(0xB0, 64, down ? 127 : 0); }
    void transport(bool playing) {
        if (playing) g_time.flags |= vst::kVstTransportPlaying;
        else g_time.flags &= ~vst::kVstTransportPlaying;
    }
    void play(double seconds) {
        float* out[2] = {L, R};
        for (int b = 0; b < static_cast<int>(seconds * kSr / kBlock); ++b) {
            e->processReplacing(e, nullptr, out, kBlock);
            g_samples += kBlock;
            if (g_time.flags & vst::kVstTransportPlaying) {
                g_time.samplePos += kBlock;
                g_time.ppqPos += kBlock / kSr * g_time.tempo / 60.0;
            }
        }
    }
    // The host stops processing and starts again `ms` later: within 250 ms a Stop, longer a reset.
    void suspend(long long ms) {
        e->dispatcher(e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);
        g_ms += ms;
        e->dispatcher(e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);
    }
};

// The phrase: a key (its chord) held, a second taken over under the pedal (legato, mid-block), a
// third, the pedal up and the tail.
void phrase(Player& p, double s = kPhraseS) {
    p.on(57, 90);
    p.play(s * 0.3);
    p.pedal(true);
    p.on(62, 80, 37);
    p.off(57, 40);
    p.play(s * 0.25);
    p.on(53, 105);
    p.off(62, 3);
    p.play(s * 0.2);
    p.pedal(false);
    p.off(53);
    p.play(s * 0.25);
}

// Until the builder thread has published every table (a few seconds under qemu); false after 10 min.
bool waitForTables() {
    for (int tries = 0; tries < 12000; ++tries) {
        bool all = true;
        for (const auto& slot : sharedTables().t) all = all && slot.load() != nullptr;
        if (all) return true;
        const timespec nap{0, 50 * 1000 * 1000};
        nanosleep(&nap, nullptr);
    }
    return false;
}

int patches = 0;

void spaces(Player& p) {   // every mode, Shimmer at every interval on the later ones, Freeze in Haze
    for (int m = 0; m < Reverb::kModes; ++m) {
        p.set(P_S_MODE, m);
        p.set(P_S_SIZE, 0.2f + 0.15f * m);
        p.set(P_S_DECAY, 2.0f + 4.0f * m);
        p.set(P_S_SHIMMER, m >= 2 ? 0.3f + 0.1f * m : 0.0f);
        p.set(P_S_SHINT, m % Reverb::kIntervals);
        p.set(P_S_RISE, m % 2 ? 0.7f : 0.0f);
        p.set(P_S_PREDELAY, 10.0f * m);
        p.set(P_S_LOWCUT, m % 2 ? 300.0f : 20.0f);   // 20: off
        p.set(P_S_WIDTH, 1.0f - 0.15f * m);
        p.on(55);
        p.play(kPhraseS * 0.5);
        if (m == Reverb::HAZE) {
            p.set(P_S_FREEZE, 1);
            p.play(kPhraseS * 0.5);
            p.set(P_S_FREEZE, 0);
        }
        p.off(55);
        phrase(p, kPhraseS * 0.75);
        ++patches;
    }
}

void couples(Player& p) {   // Mix, FM, AM, Ring, each at unison 1 and 2, B at an octave
    p.set(P_B_BLEND, 0.5f);
    p.set(P_B_CAMT, 0.6f);
    p.set(P_B_TABLEB, TB_TRIANGLE);
    for (int c = 0; c < CP_COUNT; ++c)
        for (int u = 0; u < 2; ++u) {
            p.set(P_B_COUPLE, c);
            p.set(P_B_UNISON, u);
            p.set(P_B_BOCT, c % 2 ? 1 : -1);
            p.set(P_B_DETUNE, u ? 12.0f : 0.0f);
            phrase(p, kPhraseS * 0.6);
            ++patches;
        }
    p.set(P_B_DETUNE, 0.5f);   // under a cent: unison 2 plays as one
    phrase(p, kPhraseS * 0.5);
    ++patches;
}

void filters(Player& p) {   // LP, BP, HP with resonance, the Tone moving
    p.set(P_B_RESO, 0.6f);
    for (int f = 0; f < FM_COUNT; ++f) {
        p.set(P_B_FMODE, f);
        p.set(P_B_CUTOFF, 600.0f);
        p.on(60);
        p.play(kPhraseS * 0.3);
        p.set(P_B_CUTOFF, 4000.0f);
        p.play(kPhraseS * 0.3);
        p.off(60);
        p.play(kPhraseS * 0.2);
        ++patches;
    }
}

void listens(Player& p) {   // Notes, Harmony and Free on both strata, the memory short and forever
    static const int kModes[][3] = {{LI_NOTES, LI_NOTES, 0}, {LI_HARMONY, LI_HARMONY, 1}, {LI_FREE, LI_FREE, 8},
                                    {LI_NOTES, LI_HARMONY, 2}, {LI_HARMONY, LI_FREE, 8}};
    p.transport(true);
    for (const auto& m : kModes) {
        p.set(P_G_LISTEN, m[0]);
        p.set(P_B_LISTEN, m[1]);
        p.set(P_H_MEMORY, m[2]);
        phrase(p);
        p.set(P_H_KEY, (m[2] * 5) % 12);   // Free moves with the key
        p.play(kPhraseS * 0.3);
        ++patches;
    }
}

void tunings(Player& p) {   // each tuning with an Input mode and a scale
    static const int kScales[] = {SC_MINOR, SC_LYDIAN, SC_HIRAJOSHI, SC_MAJ_PENT, SC_WHOLE_TONE, SC_CHROMATIC};
    for (int t = 0; t < 6; ++t) {
        p.set(P_H_TUNING, t % TU_COUNT);
        p.set(P_H_INPUT, (t + 1) % IN_COUNT);
        p.set(P_H_SCALE, kScales[t]);
        p.set(P_H_KEY, (t * 7) % 12);
        phrase(p, kPhraseS * 0.6);
        ++patches;
    }
}

void chords(Player& p) {   // every chord type in a voicing, strummed, Leading on and off
    p.set(P_H_STRUM, 0.25f);
    for (int c = 0; c < CH_COUNT; ++c) {
        p.set(P_H_CHORD, c);
        p.set(P_H_VOICING, c % VO_COUNT);
        p.set(P_H_LEADING, c % 3 ? 1 : 0);
        p.on(48 + 2 * c);
        p.play(kPhraseS * 0.25);
        p.on(53 + c, 70);
        p.off(48 + 2 * c);
        p.play(kPhraseS * 0.25);
        p.off(53 + c);
    }
    p.set(P_H_STRUM, 0.0f);
    p.play(kPhraseS * 0.3);
    ++patches;
}

void grounds(Player& p) {   // every partial, the colour's intervals, Body's vowels, Breath, the registers
    for (int id : {P_G_SUB, P_G_ROOT, P_G_FIFTH, P_G_OCT, P_G_COLOR}) p.set(id, 0.9f);
    p.set(P_G_BREATH, 0.8f);
    p.set(P_G_BEAT, 1.5f);
    p.set(P_G_GRAVITY, 1.0f);
    p.set(P_G_WIDTH, 1.0f);
    p.set(P_G_PAN, -0.4f);
    for (int k = 0; k < 3; ++k) {
        p.set(P_G_REG, k);
        p.set(P_G_BODY, 0.2f + 0.35f * k);
        p.set(P_G_COLINT, 2 * k + 1);
        p.set(P_G_TABLE, k == 1 ? TB_SAW : TB_CHOIR_AH_OO);
        p.set(P_G_CUTOFF, 300.0f + 2000.0f * k);
        phrase(p, kPhraseS * 0.7);
        ++patches;
    }
}

void blooms(Player& p) {   // the tables, the scan's motion, Breath, Tail Voice, Width and Pan
    static const int kTables[] = {TB_CELESTA, TB_GLASS_HARMONICA, TB_TAPE_STRINGS, TB_SQUARE};
    for (int k = 0; k < 4; ++k) {
        p.set(P_B_TABLE, kTables[k]);
        p.set(P_B_AGE, 0.2f * k);
        p.set(P_B_SWAY, 0.3f * k);
        p.set(P_B_SWAYRATE, 0.05f + 0.5f * k);
        p.set(P_B_SMEAR, 0.25f * k);
        p.set(P_B_BREATH, 0.15f * k);
        p.set(P_B_TAIL, k % 2);
        p.set(P_B_SWELL, 0.05f + 0.4f * k);
        p.set(P_B_RELEASE, 0.5f + 2.0f * k);
        p.set(P_B_WIDTH, 0.3f * k);
        p.set(P_B_PAN, 0.2f * k - 0.3f);
        p.set(P_B_VEL, 0.3f * k);
        phrase(p, kPhraseS * 0.7);
        ++patches;
    }
}

void keysAndStops(Player& p) {
    // Hold, with the pedal over it.
    p.set(P_H_HOLD, 1);
    phrase(p);
    p.on(64);
    p.off(64);
    p.play(kPhraseS * 0.3);
    p.set(P_H_HOLD, 0);
    ++patches;
    // More keys than voices: Chord Off, nine keys under the pedal.
    p.set(P_H_CHORD, CH_OFF);
    p.pedal(true);
    for (int k = 0; k < 9; ++k) {
        p.on(45 + 3 * k, 60 + 6 * k, 11 * k);
        p.play(0.1);
        p.off(45 + 3 * k);
    }
    p.play(kPhraseS * 0.3);
    p.pedal(false);
    p.play(kPhraseS * 0.2);
    p.set(P_H_CHORD, CH_TRIAD);
    ++patches;
    // Tilt both ways, and the volume at the top: the limiter at work.
    p.set(P_O_TILT, 0.8f);
    p.set(P_VOLUME, 6.0f);
    p.set(P_B_LEVEL, 1.0f);
    p.set(P_G_LEVEL, 1.0f);
    for (int k : {48, 52, 55, 59, 62, 64}) p.on(k, 127);
    p.play(kPhraseS * 0.4);
    p.set(P_O_TILT, -0.8f);
    p.play(kPhraseS * 0.3);
    for (int k : {48, 52, 55, 59, 62, 64}) p.off(k);
    p.set(P_VOLUME, -6.0f);
    p.set(P_O_TILT, 0.0f);
    ++patches;
    // Stop: Fade, turned round by a note; Cut; the host's suspend, short (a Stop) and long (a reset).
    p.transport(true);
    p.set(P_H_ONSTOP, OS_FADE);
    p.on(57);
    p.play(kPhraseS * 0.3);
    p.transport(false);
    p.play(kPhraseS * 0.4);
    p.on(60);
    p.play(kPhraseS * 0.2);
    p.off(57);
    p.off(60);
    p.set(P_H_ONSTOP, OS_CUT);
    p.transport(true);
    p.play(0.1);
    p.transport(false);
    p.play(0.1);
    p.transport(true);
    p.set(P_H_ONSTOP, OS_FADE);
    p.on(55);
    p.play(kPhraseS * 0.2);
    p.suspend(100);
    p.play(kPhraseS * 0.2);
    p.on(55);
    p.suspend(3000);
    p.play(0.1);
    p.midi(0xB0, 123, 0);   // all notes off: the harmony plays on
    p.play(0.1);
    p.midi(0xB0, 120, 0);   // all sound off
    p.play(0.1);
    // Mutes and levels at 0: the strata skipped.
    p.set(P_G_MUTE, 1);
    p.set(P_B_LEVEL, 0.0f);
    phrase(p, kPhraseS * 0.5);
    p.set(P_G_MUTE, 0);
    p.set(P_B_MUTE, 1);
    p.set(P_B_LEVEL, 0.7f);
    phrase(p, kPhraseS * 0.5);
    p.set(P_B_MUTE, 0);
    ++patches;
}

} // namespace

int main() {
    Surface::clock = clockMs;
    g_time.sampleRate = kSr;
    g_time.tempo = 96.0;
    g_time.timeSigNumerator = g_time.timeSigDenominator = 4;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;
    {
        Player p;   // the first instance starts the builder thread
        if (!waitForTables()) std::printf("pgo trainer: the tables weren't built in 10 minutes; training on the sine\n");
        phrase(p);   // Init
        ++patches;
        spaces(p);
        p.press(P_PRE_INIT);
        couples(p);
        filters(p);
        p.press(P_PRE_INIT);
        listens(p);
        p.press(P_PRE_INIT);
        tunings(p);
        chords(p);
        p.press(P_PRE_INIT);
        grounds(p);
        blooms(p);
        p.press(P_PRE_INIT);
        keysAndStops(p);
    }
    // The factory presets, as users will mostly play them: each on a fresh instance (a project
    // chunk only sets what it lists; a preset means everything else at its default).
    g_time.flags |= vst::kVstTransportPlaying;
    for (int i = 0; i < kNumFactoryPresets; ++i) {
        Player f;
        const std::string text = kFactoryPresets[i].text;
        f.e->dispatcher(f.e, vst::effSetChunk, 0, static_cast<intptr_t>(text.size()), const_cast<char*>(text.data()), 0.0f);
        phrase(f, kPhraseS * 1.25);
        ++patches;
    }
    std::printf("pgo trainer: %d patches, %.0f s of audio\n", patches, static_cast<double>(g_samples) / kSr);
    return 0;
}
