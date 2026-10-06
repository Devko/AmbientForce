// The harmony brain on its own: the scales, input mapping, diatonic chords, voicings, voice
// leading, tunings and the harmony memory. Pure logic, so every expected number is worked out by
// hand (see docs/plans/2026-10-06-m1-first-light.md, Task 3).
#include "check.h"
#include "host.h"
#include "../dsp/harmony.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace aft {
namespace {

using af::Chord;
using af::Harmony;
using af::HarmonyPatch;

HarmonyPatch patch(int key, int scale, int chord = af::CH_TRIAD, int voicing = af::VO_CLOSE) {
    HarmonyPatch h;
    h.key = key;
    h.scale = scale;
    h.chord = chord;
    h.voicing = voicing;
    return h;
}

// The chord's notes are exactly `want`; prints what it got when they aren't.
bool notesAre(const Chord& c, std::initializer_list<int> want) {
    bool ok = c.n == static_cast<int>(want.size());
    int i = 0;
    for (int w : want) ok = ok && c.notes[i++] == w;
    if (!ok) {
        std::printf("  got");
        for (int k = 0; k < c.n && k < af::kChordMax; ++k) std::printf(" %d", c.notes[k]);
        std::printf("\n");
    }
    return ok;
}

uint16_t pcsOf(std::initializer_list<int> pcs) {
    uint16_t m = 0;
    for (int p : pcs) m = static_cast<uint16_t>(m | 1u << p);
    return m;
}

Chord chordOf(std::initializer_list<int> notes) {
    Chord c;
    for (int n : notes) {
        c.notes[c.n++] = n;
        c.pcs = static_cast<uint16_t>(c.pcs | 1u << (n % 12));
    }
    c.root = c.notes[0];
    return c;
}

bool inScale(int scale, int key, int note) {
    const int rel = ((note - key) % 12 + 12) % 12;
    for (int d = 0; d < af::scaleSize(scale); ++d)
        if (af::scaleStep(scale, d) == rel) return true;
    return false;
}

void testScales() {
    std::printf("== harmony: scales\n");
    const int want[af::SC_COUNT][13] = {
        {0, 2, 4, 5, 7, 9, 11, -1},                      // Major
        {0, 2, 3, 5, 7, 8, 10, -1},                      // Minor
        {0, 2, 3, 5, 7, 9, 10, -1},                      // Dorian
        {0, 2, 4, 6, 7, 9, 11, -1},                      // Lydian
        {0, 2, 4, 5, 7, 9, 10, -1},                      // Mixolydian
        {0, 1, 3, 5, 7, 8, 10, -1},                      // Phrygian
        {0, 2, 4, 7, 9, -1},                             // Maj Pent
        {0, 3, 5, 7, 10, -1},                            // Min Pent
        {0, 2, 3, 7, 8, -1},                             // Hirajoshi
        {0, 1, 5, 7, 10, -1},                            // In-Sen
        {0, 2, 4, 6, 8, 10, -1},                         // Whole Tone
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1},      // Chromatic
    };
    for (int s = 0; s < af::SC_COUNT; ++s) {
        int n = 0;
        while (want[s][n] >= 0) ++n;
        bool ok = af::scaleSize(s) == n;
        for (int d = 0; d < n; ++d) ok = ok && af::scaleStep(s, d) == want[s][d];
        if (!ok) std::printf("  scale %d\n", s);
        CHECK(ok);
    }
    // Degrees past the scale go on into the next octave, and below 0 into the one below.
    CHECK(af::scaleStep(af::SC_MAJOR, 7) == 12);
    CHECK(af::scaleStep(af::SC_MAJOR, 8) == 14);
    CHECK(af::scaleStep(af::SC_MIN_PENT, 6) == 15);
    CHECK(af::scaleStep(af::SC_MAJOR, -1) == -1);
    CHECK(af::scaleStep(af::SC_MAJOR, -7) == -12);
}

void testMapInput() {
    std::printf("== harmony: input mapping\n");
    HarmonyPatch h = patch(2, af::SC_MAJOR);   // D Major
    h.input = af::IN_AS_PLAYED;
    bool same = true;
    for (int n = 0; n < 128; ++n) same = same && af::mapInput(h, n) == n;
    CHECK(same);

    // Snap: the nearest scale tone, the lower one on a tie.
    h.input = af::IN_SNAP;
    CHECK(af::mapInput(h, 61) == 61);   // C# is in D major
    CHECK(af::mapInput(h, 60) == 59);   // C: B and C# both a semitone away, B is lower
    CHECK(af::mapInput(h, 63) == 62);   // D#: D and E, D is lower
    CHECK(af::mapInput(h, 62) == 62);
    CHECK(af::mapInput(h, 0) == 1);     // C0: the B below would be note -1
    // Every key and scale: the snapped note is a valid note in the scale, no tone of the scale
    // between 0 and 127 is nearer, and of two as near it is the lower. (At the top of the range
    // the nearest tone may be above 127: then the one below, even 3 semitones away.)
    bool snaps = true;
    for (int s = 0; s < af::SC_COUNT; ++s)
        for (int k = 0; k < 12; ++k) {
            HarmonyPatch q = patch(k, s);
            q.input = af::IN_SNAP;
            for (int n = 0; n < 128; ++n) {
                const int m = af::mapInput(q, n);
                bool ok = m >= 0 && m <= 127 && inScale(s, k, m);
                for (int x = n - 12; x <= n + 12 && ok; ++x) {
                    const int dx = std::abs(x - n), dm = std::abs(m - n);
                    if (x >= 0 && x <= 127 && inScale(s, k, x) && (dx < dm || (dx == dm && x < m))) ok = false;
                }
                if (!ok && snaps) std::printf("  scale %d key %d note %d -> %d\n", s, k, n, m);
                snaps = snaps && ok;
            }
        }
    CHECK(snaps);

    // Degrees: the white keys are the scale's degrees, C4 the tonic.
    h.input = af::IN_DEGREES;
    CHECK(af::mapInput(h, 60) == 62);
    CHECK(af::mapInput(h, 62) == 64);
    CHECK(af::mapInput(h, 64) == 66);
    CHECK(af::mapInput(h, 72) == 74);
    CHECK(af::mapInput(h, 61) == 62);   // a black key plays the white key below it
    CHECK(af::mapInput(h, 63) == 64);
    CHECK(af::mapInput(h, 71) == 73);   // B: degree 6, the C# of D major
    HarmonyPatch p = patch(0, af::SC_MIN_PENT);   // C Min Pent: five tones, so A and B go on up
    p.input = af::IN_DEGREES;
    CHECK(af::mapInput(p, 60) == 60);
    CHECK(af::mapInput(p, 62) == 63);
    CHECK(af::mapInput(p, 64) == 65);
    CHECK(af::mapInput(p, 65) == 67);
    CHECK(af::mapInput(p, 67) == 70);
    CHECK(af::mapInput(p, 69) == 72);
    CHECK(af::mapInput(p, 71) == 75);
    // Chromatic: every key plays as played, moved up by the key (C4 still the tonic).
    HarmonyPatch chroma = patch(2, af::SC_CHROMATIC);
    chroma.input = af::IN_DEGREES;
    CHECK(af::mapInput(chroma, 60) == 62);
    CHECK(af::mapInput(chroma, 61) == 63);
    CHECK(af::mapInput(chroma, 71) == 73);
    CHECK(af::mapInput(chroma, 72) == 74);
    chroma.key = 11;
    CHECK(af::mapInput(chroma, 127) == 126);   // 138, an octave down into range
    // Every key and scale: a valid note in the scale.
    bool degrees = true;
    for (int s = 0; s < af::SC_COUNT; ++s)
        for (int k = 0; k < 12; ++k) {
            HarmonyPatch q = patch(k, s);
            q.input = af::IN_DEGREES;
            for (int n = 0; n < 128; ++n) {
                const int m = af::mapInput(q, n);
                const bool ok = m >= 0 && m <= 127 && inScale(s, k, m);
                if (!ok && degrees) std::printf("  scale %d key %d note %d -> %d\n", s, k, n, m);
                degrees = degrees && ok;
            }
        }
    CHECK(degrees);

    // Not a note: dropped, in every mode.
    for (int in = 0; in < af::IN_COUNT; ++in) {
        h.input = in;
        CHECK(af::mapInput(h, -1) == -1 && af::mapInput(h, 128) == -1);
    }
}

void testChords() {
    std::printf("== harmony: diatonic chords\n");
    using namespace af;
    auto close = [](int chord, int root, int key = 0, int scale = SC_MAJOR) {
        return buildChord(patch(key, scale, chord, VO_CLOSE), root);
    };
    CHECK(notesAre(close(CH_TRIAD, 60), {60, 64, 67}));
    CHECK(notesAre(close(CH_SEVENTH, 60), {60, 64, 67, 71}));
    CHECK(notesAre(close(CH_SUS2, 60), {60, 62, 67}));
    CHECK(notesAre(close(CH_SUS4, 60), {60, 65, 67}));
    CHECK(notesAre(close(CH_ADD9, 60), {60, 64, 67, 74}));
    CHECK(notesAre(close(CH_QUARTAL, 60), {60, 65, 71}));
    CHECK(notesAre(close(CH_FIFTHS, 60), {60, 67, 74}));
    CHECK(notesAre(close(CH_CLUSTER, 60), {60, 62, 64}));
    CHECK(notesAre(close(CH_TRIAD, 62), {62, 65, 69}));                  // D minor: diatonic
    CHECK(notesAre(close(CH_TRIAD, 71), {71, 74, 77}));                  // B diminished
    CHECK(notesAre(close(CH_TRIAD, 62, 0, SC_CHROMATIC), {62, 66, 69})); // the major scale on D
    CHECK(notesAre(close(CH_TRIAD, 69, 9, SC_MINOR), {69, 72, 76}));     // A minor in A Minor
    CHECK(notesAre(close(CH_TRIAD, 60, 0, SC_MAJ_PENT), {60, 64, 69}));  // degrees 0 2 4: C E A
    // A root outside the scale: the stack on the degree below (C), moved up to the root.
    CHECK(notesAre(close(CH_TRIAD, 61), {61, 65, 68}));
    const Chord off = close(CH_OFF, 60);
    CHECK(notesAre(off, {60}) && off.root == 60 && off.pcs == pcsOf({0}));

    const Chord c = close(CH_TRIAD, 60);
    CHECK(c.root == 60 && c.pcs == pcsOf({0, 4, 7}));
    const Chord d = close(CH_SEVENTH, 62);
    CHECK(d.root == 62 && d.pcs == pcsOf({2, 5, 9, 0}));

    CHECK(buildChord(patch(0, SC_MAJOR), -1).root == -1 && buildChord(patch(0, SC_MAJOR), -1).n == 0);
    CHECK(buildChord(patch(0, SC_MAJOR), 128).root == -1 && buildChord(patch(0, SC_MAJOR), 128).n == 0);
}

void testVoicings() {
    std::printf("== harmony: voicings\n");
    using namespace af;
    auto voiced = [](int chord, int voicing, int root = 60) {
        return buildChord(patch(0, SC_MAJOR, chord, voicing), root);
    };
    CHECK(notesAre(voiced(CH_TRIAD, VO_OPEN), {60, 67, 76}));
    CHECK(notesAre(voiced(CH_SEVENTH, VO_OPEN), {60, 67, 76, 83}));
    CHECK(notesAre(voiced(CH_TRIAD, VO_DROP2), {52, 60, 67}));
    CHECK(notesAre(voiced(CH_SEVENTH, VO_DROP2), {55, 60, 64, 71}));
    CHECK(notesAre(voiced(CH_TRIAD, VO_SPREAD), {48, 55, 64, 67}));
    CHECK(notesAre(voiced(CH_SEVENTH, VO_SPREAD), {48, 55, 64, 67, 71}));
    CHECK(notesAre(voiced(CH_ADD9, VO_SPREAD), {48, 55, 64, 67, 74}));
    // The Spread chord: the triad's tones, voiced Spread whatever the Voicing.
    CHECK(notesAre(voiced(CH_SPREAD, VO_CLOSE), {48, 55, 64, 67}));
    CHECK(notesAre(voiced(CH_SPREAD, VO_OPEN), {48, 55, 64, 67}));
    // Chord Off is the root alone, whatever the Voicing.
    CHECK(notesAre(voiced(CH_OFF, VO_SPREAD), {60}));
    // The root stays the root the chord was asked for, wherever the voicing puts it.
    CHECK(voiced(CH_TRIAD, VO_SPREAD).root == 60 && voiced(CH_TRIAD, VO_DROP2).root == 60);

    // Every root, chord and voicing in every scale: no more than kChordMax notes, ascending,
    // within 24..108, the close voicing's pitch classes, and nothing dropped (as many notes as
    // the chord has tones, one more for Spread's doubled tone).
    const int tones[CH_COUNT] = {1, 3, 4, 3, 3, 4, 3, 3, 3, 3};
    bool all = true;
    for (int s = 0; s < SC_COUNT; ++s)
        for (int ch = 0; ch < CH_COUNT; ++ch)
            for (int v = 0; v < VO_COUNT; ++v)
                for (int root = 0; root < 128; ++root) {
                    const Chord c = buildChord(patch(5, s, ch, v), root);
                    const Chord cl = buildChord(patch(5, s, ch, VO_CLOSE), root);
                    const bool spread = ch != CH_OFF && (v == VO_SPREAD || ch == CH_SPREAD);
                    bool ok = c.n == tones[ch] + (spread ? 1 : 0) && c.n <= kChordMax && c.pcs == cl.pcs &&
                              c.root >= 0 && c.root % 12 == root % 12;
                    for (int i = 0; i < c.n && i < kChordMax; ++i) {
                        ok = ok && c.notes[i] >= kChordLowest && c.notes[i] <= kChordHighest;
                        ok = ok && (i == 0 || c.notes[i] > c.notes[i - 1]);
                    }
                    if (!ok && all) std::printf("  scale %d chord %d voicing %d root %d\n", s, ch, v, root);
                    all = all && ok;
                }
    CHECK(all);
    // Near the edges the whole chord moves by octaves, keeping its shape.
    CHECK(notesAre(voiced(CH_TRIAD, VO_SPREAD, 24), {24, 31, 40, 43}));
    // 120 127 136 146, down four octaves.
    CHECK(notesAre(voiced(CH_ADD9, VO_OPEN, 120), {72, 79, 88, 98}));
    CHECK(voiced(CH_ADD9, VO_OPEN, 120).root == 72);
}

void testLeading() {
    std::printf("== harmony: voice leading\n");
    using namespace af;
    const HarmonyPatch cmaj = patch(0, SC_MAJOR);
    const Chord c = buildChord(cmaj, 60);
    CHECK(notesAre(c, {60, 64, 67}));

    // Voiced Close. C to F: 60 stays, 64 -> 65, 67 -> 69.
    const Chord f = leadFrom(cmaj, c, buildChord(cmaj, 65));
    CHECK(notesAre(f, {60, 65, 69}));
    CHECK(voiceLeadCost(c, f) == 3);
    CHECK(f.root == 65 && f.pcs == pcsOf({5, 9, 0}));
    // C to G: 60 -> 59, 64 -> 62, 67 stays.
    const Chord g = leadFrom(cmaj, c, buildChord(cmaj, 67));
    CHECK(notesAre(g, {59, 62, 67}));
    CHECK(voiceLeadCost(c, g) == 3);
    // Nothing before: the chord as it is.
    const HarmonyPatch hOpen = patch(0, SC_MAJOR, CH_TRIAD, VO_OPEN);
    const Chord open = buildChord(hOpen, 65);
    CHECK(notesAre(leadFrom(hOpen, Chord{}, open), {65, 72, 81}));
    // A tie goes to the lowest note nearest the chord before's: E minor to D minor costs 6 both
    // as built (62 65 69) and as 65 69 74; 65 is nearer 64.
    const Chord em = buildChord(cmaj, 64);
    const Chord dm = leadFrom(cmaj, em, buildChord(cmaj, 62));
    CHECK(notesAre(dm, {65, 69, 74}));
    CHECK(voiceLeadCost(em, chordOf({62, 65, 69})) == 6 && voiceLeadCost(em, dm) == 6);

    // Leading keeps the voicing: each inversion is voiced by the Voicing's rule. Open: C (60 67 76)
    // to F is the inversion C F A voiced Open, 60 69 77, cost 3.
    const Chord cOpen = buildChord(hOpen, 60);
    CHECK(notesAre(cOpen, {60, 67, 76}));
    const Chord fOpen = leadFrom(hOpen, cOpen, buildChord(hOpen, 65));
    CHECK(notesAre(fOpen, {60, 69, 77}) && voiceLeadCost(cOpen, fOpen) == 3);
    // Drop 2: C (52 60 67) to F is C F A with its F dropped, 53 60 69, cost 3.
    const HarmonyPatch hDrop = patch(0, SC_MAJOR, CH_TRIAD, VO_DROP2);
    const Chord cDrop = buildChord(hDrop, 60);
    const Chord fDrop = leadFrom(hDrop, cDrop, buildChord(hDrop, 65));
    CHECK(notesAre(fDrop, {53, 60, 69}) && voiceLeadCost(cDrop, fDrop) == 3);
    // Spread never inverts: the root stays at the bottom and the four-note shape only moves by
    // octaves. C (48 55 64 67) to F: 53 60 69 72 (cost 20; an octave down costs 28). To B
    // (diminished): an octave down, 47 53 62 65 (cost 7), rather than 59 65 74 77.
    const HarmonyPatch hSpread = patch(0, SC_MAJOR, CH_TRIAD, VO_SPREAD);
    const Chord cSpread = buildChord(hSpread, 60);
    CHECK(notesAre(cSpread, {48, 55, 64, 67}));
    const Chord fSpread = leadFrom(hSpread, cSpread, buildChord(hSpread, 65));
    CHECK(notesAre(fSpread, {53, 60, 69, 72}) && fSpread.root == 65);
    CHECK(notesAre(leadFrom(hSpread, cSpread, buildChord(hSpread, 71)), {47, 53, 62, 65}));
    // The Spread chord likewise, whatever the Voicing.
    const HarmonyPatch hSpreadChord = patch(0, SC_MAJOR, CH_SPREAD, VO_OPEN);
    CHECK(notesAre(leadFrom(hSpreadChord, cSpread, buildChord(hSpreadChord, 65)), {53, 60, 69, 72}));
    // Chord Off: the note as played, never moved an octave towards the chord before.
    const HarmonyPatch hOff = patch(0, SC_MAJOR, CH_OFF);
    CHECK(notesAre(leadFrom(hOff, chordOf({60, 64, 67}), buildChord(hOff, 72)), {72}));
    // A chord the patch doesn't build (C minor in C Major): as it is.
    CHECK(notesAre(leadFrom(cmaj, c, chordOf({60, 63, 67})), {60, 63, 67}));

    // Every scale, chord type and voicing, from a few chords before: the same notes count, pitch
    // classes and root as built; ascending with no note doubled; within 24..108; never costlier
    // than the chord as built. Spread keeps its shape, moved by octaves, root at the bottom.
    bool all = true;
    for (int s = 0; s < SC_COUNT; ++s)
        for (int ch = 0; ch < CH_COUNT; ++ch)
            for (int v = 0; v < VO_COUNT; ++v) {
                const HarmonyPatch h = patch(2, s, ch, v);
                for (int from : {43, 60, 67, 84})
                    for (int root = 26; root <= 106; root += 5) {
                        const Chord prev = buildChord(h, from), built = buildChord(h, root);
                        const Chord led = leadFrom(h, prev, built);
                        bool ok = led.n == built.n && led.pcs == built.pcs && led.root == built.root &&
                                  voiceLeadCost(prev, led) <= voiceLeadCost(prev, built);
                        for (int i = 0; i < led.n && ok; ++i)
                            ok = led.notes[i] >= kChordLowest && led.notes[i] <= kChordHighest &&
                                 (i == 0 || led.notes[i] > led.notes[i - 1]);
                        if (ch != CH_OFF && (v == VO_SPREAD || ch == CH_SPREAD)) {
                            const int shift = led.notes[0] - built.notes[0];
                            ok = ok && (shift == -12 || shift == 0 || shift == 12) && led.notes[0] % 12 == root % 12;
                            for (int i = 0; i < led.n && ok; ++i) ok = led.notes[i] - built.notes[i] == shift;
                        }
                        if (ch == CH_OFF) ok = ok && notesAre(led, {built.notes[0]});
                        if (!ok && all)
                            std::printf("  scale %d chord %d voicing %d from %d root %d\n", s, ch, v, from, root);
                        all = all && ok;
                    }
            }
    CHECK(all);

    // The baseline cost: equal counts sum the moves of the sorted notes; unequal counts sum each
    // note's distance to the nearest note of the other chord, both ways, halved.
    CHECK(voiceLeadCost(chordOf({60, 64, 67}), chordOf({60, 65, 69})) == 3);
    CHECK(voiceLeadCost(chordOf({67, 60, 64}), chordOf({69, 65, 60})) == 3);   // sorted first
    CHECK(voiceLeadCost(chordOf({60, 64, 67}), chordOf({62, 67})) == 3);       // (2+2+0 + 2+0) / 2
    CHECK(voiceLeadCost(chordOf({60, 64, 67}), chordOf({60, 64, 67})) == 0);
    CHECK(voiceLeadCost(Chord{}, chordOf({60, 64, 67})) == 0);

    // Every candidate stays within 24..108, even from a chord far down.
    const Chord low = buildChord(cmaj, 24);
    // F on 29 from 24 28: 21 24 29 would cost 2, but 21 is out of range; 24 29 33 costs 3.
    const Chord led = leadFrom(cmaj, chordOf({24, 28}), buildChord(cmaj, 29));
    CHECK(notesAre(led, {24, 29, 33}) && notesAre(low, {24, 28, 31}));

    // Through the memory: Leading on moves from the chord before, Leading off plays it as built.
    HarmonyPatch h = patch(0, SC_MAJOR);
    h.leading = true;
    Harmony mem;
    mem.set(h);
    mem.noteOn(60);
    CHECK(notesAre(mem.current(), {60, 64, 67}));
    mem.noteOn(65);
    CHECK(notesAre(mem.current(), {60, 65, 69}));
    h.voicing = VO_OPEN;
    Harmony opened;
    opened.set(h);
    opened.noteOn(60);
    CHECK(notesAre(opened.current(), {60, 67, 76}));
    opened.noteOn(65);
    CHECK(notesAre(opened.current(), {60, 69, 77}));
    h.voicing = VO_CLOSE;
    h.leading = false;
    Harmony plain;
    plain.set(h);
    plain.noteOn(60);
    plain.noteOn(65);
    CHECK(notesAre(plain.current(), {65, 69, 72}));
}

void testTuning() {
    std::printf("== harmony: tunings\n");
    using namespace af;
    HarmonyPatch h = patch(0, SC_MAJOR);
    h.tuning = TU_EQUAL;
    CHECK(tunedPitch(h, 69) == 69.0);
    bool equal = true;
    for (int k = 0; k < 12; ++k) {
        h.key = k;
        for (int n = 0; n < 128; ++n) equal = equal && tunedPitch(h, n) == static_cast<double>(n);
    }
    CHECK(equal);

    h.tuning = TU_JUST;
    h.key = 9;   // A
    CHECK(std::fabs(tunedPitch(h, 76) - (69.0 + 12.0 * std::log2(1.5))) < 1e-9);
    CHECK(std::fabs(tunedPitch(h, 76) - 76.01955) < 1e-5);
    CHECK(tunedPitch(h, 69) == 69.0);
    h.key = 0;   // C
    CHECK(std::fabs(tunedPitch(h, 64) - (60.0 + 12.0 * std::log2(1.25))) < 1e-9);
    CHECK(std::fabs(tunedPitch(h, 64) - 63.86314) < 1e-5);
    CHECK(tunedPitch(h, 60) == 60.0 && tunedPitch(h, 72) == 72.0 && tunedPitch(h, 48) == 48.0);
    h.tuning = TU_PYTHAGOREAN;
    CHECK(std::fabs(tunedPitch(h, 64) - (60.0 + 12.0 * std::log2(81.0 / 64.0))) < 1e-9);

    // The whole tables, on two keys, in the tonic's octave and the one below.
    const double just[12] = {1.0, 16.0 / 15, 9.0 / 8, 6.0 / 5, 5.0 / 4, 4.0 / 3,
                             45.0 / 32, 3.0 / 2, 8.0 / 5, 5.0 / 3, 9.0 / 5, 15.0 / 8};
    const double pyth[12] = {1.0, 256.0 / 243, 9.0 / 8, 32.0 / 27, 81.0 / 64, 4.0 / 3,
                             729.0 / 512, 3.0 / 2, 128.0 / 81, 27.0 / 16, 16.0 / 9, 243.0 / 128};
    bool tables = true;
    for (int t : {TU_JUST, TU_PYTHAGOREAN}) {
        h.tuning = t;
        for (int k : {0, 2, 11}) {
            h.key = k;
            for (int i = 0; i < 12; ++i)
                for (int tonic : {48 + k, 60 + k}) {
                    const double want = tonic + 12.0 * std::log2(t == TU_JUST ? just[i] : pyth[i]);
                    const bool ok = std::fabs(tunedPitch(h, tonic + i) - want) < 1e-9;
                    if (!ok && tables) std::printf("  tuning %d key %d note %d\n", t, k, tonic + i);
                    tables = tables && ok;
                }
        }
    }
    CHECK(tables);
}

void testMemory() {
    std::printf("== harmony: the memory\n");
    using namespace af;
    // Chord Off: the keys held, as played; the root is the lowest.
    {
        Harmony m;
        HarmonyPatch h = patch(0, SC_MAJOR, CH_OFF);
        m.set(h);
        m.noteOn(64);
        m.noteOn(60);
        m.noteOn(67);
        CHECK(m.current().pcs == pcsOf({0, 4, 7}) && m.current().root == 60);
        CHECK(notesAre(m.current(), {60, 64, 67}));
    }
    // A chord type: the chord on the latest key.
    {
        Harmony m;
        m.set(patch(0, SC_MAJOR, CH_TRIAD, VO_OPEN));
        m.noteOn(62);
        CHECK(m.current().root == 62 && m.current().pcs == pcsOf({2, 5, 9}));
        m.noteOn(65);
        CHECK(m.current().root == 65 && m.current().pcs == pcsOf({5, 9, 0}));
        // Keys going up change nothing until the last.
        m.noteOff(65);
        CHECK(m.current().root == 65);
    }
    // Memory Forever: the chord stays after every key is up, however long.
    {
        Harmony m;
        HarmonyPatch h = patch(0, SC_MAJOR);
        h.memoryBars = -1;
        m.set(h);
        m.noteOn(60);
        m.noteOff(60);
        const uint32_t v = m.version();
        m.advance(3600.0, 120.0);
        CHECK(m.current().root == 60 && notesAre(m.current(), {60, 64, 67}) && m.version() == v);
    }
    // Memory 1 bar at 120 bpm: 4 beats, 2 s after the last key goes up.
    {
        Harmony m;
        HarmonyPatch h = patch(0, SC_MAJOR);
        h.memoryBars = 1;
        m.set(h);
        m.noteOn(60);
        m.advance(10.0, 120.0);   // the timer doesn't run while a key is down
        m.noteOff(60);
        const uint32_t v = m.version();
        m.advance(1.9, 120.0);
        CHECK(m.current().root == 60 && m.version() == v);
        m.advance(0.2, 120.0);
        CHECK(m.current().root == -1 && m.current().n == 0 && m.version() == v + 1);
        m.advance(10.0, 120.0);
        CHECK(m.version() == v + 1);
        // A key in between starts the timer again; the tempo sets its length.
        m.noteOn(62);
        m.noteOff(62);
        m.advance(1.5, 120.0);
        m.noteOn(62);
        m.noteOff(62);
        m.advance(1.5, 120.0);
        CHECK(m.current().root == 62);
        m.advance(0.9, 60.0);     // 3 beats at 120 bpm, 0.9 more at 60 (at 120 they'd be 1.8)
        CHECK(m.current().root == 62);
        m.advance(0.2, 60.0);
        CHECK(m.current().root == -1);
    }
    // Memory turned down after the release counts from the release: a chord kept 10 s (20 beats)
    // under Forever is older than 4 bars, so it goes at the next advance(); one kept 2 s isn't.
    {
        Harmony m;
        HarmonyPatch h = patch(0, SC_MAJOR);
        m.set(h);
        m.noteOn(60);
        m.noteOff(60);
        m.advance(2.0, 120.0);
        h.memoryBars = 4;
        m.set(h);
        m.advance(0.0, 120.0);
        CHECK(m.current().root == 60);
        h.memoryBars = -1;
        m.set(h);
        m.advance(8.0, 120.0);
        h.memoryBars = 4;
        m.set(h);
        CHECK(m.current().root == 60);   // set() alone changes nothing
        m.advance(0.0, 120.0);
        CHECK(m.current().root == -1);
        // Memory Off after the release: gone at the next advance().
        m.noteOn(62);
        m.noteOff(62);
        h.memoryBars = 0;
        m.set(h);
        m.advance(0.0, 120.0);
        CHECK(m.current().root == -1);
    }
    // Memory Off: gone when the last key goes up, not before.
    {
        Harmony m;
        HarmonyPatch h = patch(0, SC_MAJOR);
        h.memoryBars = 0;
        m.set(h);
        m.noteOn(60);
        m.noteOn(64);
        m.noteOff(60);
        CHECK(m.current().root == 64);
        m.noteOff(64);
        CHECK(m.current().root == -1);
    }
    // version(): +1 on every change, never otherwise.
    {
        Harmony m;
        m.set(patch(0, SC_MAJOR));
        CHECK(m.version() == 0 && m.current().root == -1);
        m.noteOn(60);
        CHECK(m.version() == 1);
        m.noteOn(60);   // a double note-on: nothing changes
        CHECK(m.version() == 1 && m.held() == 1);
        m.noteOn(64);
        CHECK(m.version() == 2);
        m.noteOff(64);
        m.noteOff(64);   // a key not down
        m.noteOff(70);   // a key never pressed
        CHECK(m.version() == 2 && m.held() == 1);
        m.noteOff(60);
        CHECK(m.version() == 2 && m.held() == 0);
        m.noteOn(-1);    // a dropped note
        CHECK(m.version() == 2 && m.held() == 0);
        m.clear();
        CHECK(m.version() == 3 && m.current().root == -1);
        m.clear();
        CHECK(m.version() == 3);
    }
    // lowestHeld(): the keys down, whatever the chord.
    {
        Harmony m;
        m.set(patch(0, SC_MAJOR));
        CHECK(m.lowestHeld() == -1);
        m.noteOn(64);
        m.noteOn(60);
        m.noteOn(67);
        CHECK(m.lowestHeld() == 60 && m.held() == 3);
        m.noteOff(60);
        CHECK(m.lowestHeld() == 64);
        m.noteOff(64);
        m.noteOff(67);
        CHECK(m.lowestHeld() == -1 && m.held() == 0);
        // clear() forgets the keys too.
        m.noteOn(50);
        m.clear();
        CHECK(m.lowestHeld() == -1 && m.held() == 0 && m.current().root == -1);
        m.noteOff(50);
        CHECK(m.current().root == -1);
    }
    // More keys than it remembers: the extra ones are ignored, safely.
    {
        Harmony m;
        HarmonyPatch h = patch(0, SC_MAJOR, CH_OFF);
        m.set(h);
        for (int i = 0; i < 24; ++i) m.noteOn(40 + i);
        CHECK(m.held() == Harmony::kHeldMax && m.lowestHeld() == 40);
        // Chord Off with more keys than voices: the lowest and the latest.
        CHECK(notesAre(m.current(), {40, 51, 52, 53, 54, 55}) && m.current().root == 40);
        for (int i = 0; i < 24; ++i) m.noteOff(40 + i);
        CHECK(m.held() == 0 && m.lowestHeld() == -1);
        CHECK(m.current().root == 40);   // remembered (Forever)
    }
    // Chord Off near the edges: the keys move into 24..108 as a whole, by octaves, so the lowest
    // key stays the root at the bottom.
    {
        auto held = [](std::initializer_list<int> keys) {
            Harmony m;
            m.set(patch(0, SC_MAJOR, CH_OFF));
            for (int k : keys) m.noteOn(k);
            return m.current();
        };
        const Chord a = held({20, 30});            // up an octave together
        CHECK(notesAre(a, {32, 42}) && a.root == 32);
        const Chord b = held({100, 120});          // down an octave together
        CHECK(notesAre(b, {88, 108}) && b.root == 88);
        // Wider than the range: the root goes in first (10 -> 34, the others with it to 124 and
        // 144), then what is still above 108 folds down into the top octave, above the root.
        const Chord w = held({10, 100, 120});
        CHECK(notesAre(w, {34, 100, 108}) && w.root == 34);
        // Two keys folding onto one note (both Es, 136 and 148, to 100): one note.
        const Chord e = held({10, 112, 124});
        CHECK(notesAre(e, {34, 100}) && e.root == 34);
        // Every pair and triple of keys: the root at the bottom, on the lowest key's pitch class,
        // ascending with nothing doubled, within range; and a chord that some whole-octave move
        // fits into the range keeps its shape. (A span of 84 isn't always enough: keys 5 24 89
        // can only move to 29 48 113.)
        bool all = true;
        for (int lo = 0; lo < 128; lo += 5)
            for (int mid = lo + 1; mid < 128; mid += 9)
                for (int hi = mid; hi < 128; hi += 13) {
                    const Chord c = hi == mid ? held({mid, lo}) : held({hi, lo, mid});
                    bool ok = c.n >= 1 && c.root == c.notes[0] && c.root % 12 == lo % 12;
                    for (int i = 0; i < c.n && ok; ++i)
                        ok = c.notes[i] >= kChordLowest && c.notes[i] <= kChordHighest &&
                             (i == 0 || c.notes[i] > c.notes[i - 1]);
                    bool fits = false;
                    for (int s = -120; s <= 120; s += 12)
                        fits = fits || (lo + s >= kChordLowest && hi + s <= kChordHighest);
                    if (ok && fits) {
                        const int shift = c.root - lo;
                        ok = c.n == (hi == mid ? 2 : 3) && c.notes[1] == mid + shift && c.notes[c.n - 1] == hi + shift;
                    }
                    if (!ok && all) std::printf("  keys %d %d %d\n", lo, mid, hi);
                    all = all && ok;
                }
        CHECK(all);
    }
}

} // namespace

void harmonyTests() {
    testScales();
    testMapInput();
    testChords();
    testVoicings();
    testLeading();
    testTuning();
    testMemory();
}

} // namespace aft
