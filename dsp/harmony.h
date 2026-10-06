#pragma once
// The harmony brain (CONCEPT §6): everything that has a pitch asks it first. Pure logic, no sound:
// - the scales, and how a played key maps into the key and scale (Input);
// - the chord on a root, diatonic to the scale, and its voicing;
// - voice leading from the chord before;
// - the tunings, as fractional MIDI pitches;
// - the harmony memory: the chord that Ground and Bloom follow in their Harmony modes, which
//   outlives the keys by Memory bars.
//
// Real-time rules: Harmony's methods run on the audio thread, so it keeps everything in fixed
// arrays, allocates nothing and throws nothing. The free functions are just as plain.
#include <cstdint>

namespace af {

enum Scale : int { SC_MAJOR, SC_MINOR, SC_DORIAN, SC_LYDIAN, SC_MIXOLYDIAN, SC_PHRYGIAN, SC_MAJ_PENT,
                   SC_MIN_PENT, SC_HIRAJOSHI, SC_IN_SEN, SC_WHOLE_TONE, SC_CHROMATIC, SC_COUNT };
enum Tuning : int { TU_EQUAL, TU_JUST, TU_PYTHAGOREAN, TU_COUNT };
enum Input : int { IN_AS_PLAYED, IN_SNAP, IN_DEGREES, IN_COUNT };
enum ChordType : int { CH_OFF, CH_TRIAD, CH_SEVENTH, CH_SUS2, CH_SUS4, CH_ADD9, CH_QUARTAL, CH_FIFTHS,
                       CH_CLUSTER, CH_SPREAD, CH_COUNT };
enum Voicing : int { VO_CLOSE, VO_OPEN, VO_DROP2, VO_SPREAD, VO_COUNT };

struct HarmonyPatch {
    int key = 0;              // 0 = C .. 11 = B: the tonic's pitch class
    int scale = SC_MAJOR;
    int tuning = TU_JUST;
    int input = IN_SNAP;
    int chord = CH_TRIAD;
    int voicing = VO_OPEN;
    bool leading = true;
    float strumS = 0.0f;      // 0..2: the chord's notes enter one after another, low to high
    int memoryBars = -1;      // 0: off; 1..64: bars after the last release; -1: forever
};

constexpr int kChordMax = 6;  // notes in a voicing (Bloom has 6 voices)
// Every chord buildChord() makes stays in this range, moved there by octaves: the oscillators
// are tuned for it, and nothing below or above it is music anyway.
constexpr int kChordLowest = 24, kChordHighest = 108;

struct Chord {
    int n = 0;
    int notes[kChordMax] = {};  // MIDI notes, ascending
    int root = -1;              // MIDI note of the chord's root (-1: no chord)
    uint16_t pcs = 0;           // pitch-class set, bit i = pitch class i
};

// The scale tables: semitones above the tonic, ascending. Degrees past the last one go on into
// the octaves above (and negative ones below), so scaleStep(SC_MAJOR, 7) is 12.
int scaleSize(int scale);                       // 5..12
int scaleStep(int scale, int degree);

// Input mapping, for a note 0..127. -1 = drop the note: reserved, these modes never drop one
// (-1 only for a note outside 0..127).
// - As Played: the note.
// - Snap: the nearest tone of the scale, the lower one on a tie.
// - Degrees: the white keys C D E F G A B are degrees 0..6 (a black key plays the white key
//   below it), so C4 (60) plays the tonic at 60 + key. Scales with fewer than 7 tones run on
//   into the next octave. Chromatic has every key in it, so each key plays as played, moved up
//   by the key (C4 still the tonic).
int mapInput(const HarmonyPatch& h, int note);

// The chord on `root` (already mapped): its scale degrees stacked by the chord type, diatonic to
// h.scale (Chromatic: the major scale on the root), voiced by h.voicing near the root. A root
// outside the scale (As Played) borrows the stack of the nearest degree below it, moved up to
// the root: a parallel chord, so a played key always gets a chord on itself (C# in C Major: the
// C major triad moved up, C# F G#). The whole chord moves by octaves into
// kChordLowest..kChordHighest, the root with it. A root outside 0..127: no chord.
Chord buildChord(const HarmonyPatch& h, int root);

// Voice leading: among the inversions and octave placements of `c` (= buildChord(h, root)), the
// one with the least cost from `prev`, keeping h's voicing. prev.n == 0: c as is.
// - The candidates: c as built; then each inversion of the close voicing, voiced by h.voicing's
//   rule, an octave down, in place and an octave up. Spread (the voicing, or the Spread chord)
//   never inverts: its root stays at the bottom and only the whole shape moves by octaves.
// - Only candidates within kChordLowest..kChordHighest, with no note doubled, count.
// - A tie goes to the lowest note nearest prev's lowest, then to the earlier candidate (so c as
//   built wins a full tie).
// - Chord Off (a single note as played), or a c that h doesn't build: c as is.
// The result keeps c's root and pitch classes.
Chord leadFrom(const HarmonyPatch& h, const Chord& prev, const Chord& c);
int voiceLeadCost(const Chord& prev, const Chord& candidate);   // lower = smoother

// Pitch of a MIDI note under the tuning, in fractional semitones (69.0 = A4 = 440 Hz under Equal).
// Just and Pythagorean are relative to the key's tonic: the tonic stays where Equal has it.
double tunedPitch(const HarmonyPatch& h, int note);

// The harmony memory: what Ground and Bloom (Harmony mode) follow.
//
// A key going down sets the chord: the chord on that key (the latest key wins), voice-led from
// the chord before when Leading is on. With Chord Off the chord is the keys held, as played.
// Keys going up change nothing until the last one: then the chord stays for Memory bars (4/4,
// at the tempo advance() is given), forever, or (Memory Off) not at all.
class Harmony {
public:
    static constexpr int kHeldMax = 16;   // keys remembered; more are ignored until some go up

    // The chord settings take effect from the next key (the chord kept isn't re-voiced); Memory
    // at the next advance(), counting from the last release.
    void set(const HarmonyPatch& h);
    void noteOn(int mappedNote);                // a key down (after mapping)
    void noteOff(int mappedNote);
    void advance(double seconds, double bpm);   // runs the memory's timer
    void clear();                               // forget the chord and the keys (reset, Stop with Cut)
    const Chord& current() const { return current_; }   // root -1: nothing (never played, or forgotten)
    int lowestHeld() const;                     // -1: no key down
    int held() const { return nHeld_; }
    uint32_t version() const { return version_; }       // +1 whenever current() changes

private:
    Chord heldChord() const;                    // Chord Off: the keys held
    void change(const Chord& c);                // current_ = c, counting a change

    HarmonyPatch patch_;
    int      held_[kHeldMax] = {};              // keys down, oldest first
    int      nHeld_ = 0;
    Chord    current_;
    double   releasedBeats_ = 0.0;              // since the last key went up
    uint32_t version_ = 0;
};

} // namespace af
