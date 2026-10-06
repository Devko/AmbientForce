#include "harmony.h"

#include <cmath>
#include <cstdlib>

namespace af {
namespace {

// The scales of CONCEPT §6: semitones above the tonic, ascending.
struct ScaleDef {
    int size;
    int steps[12];
};
constexpr ScaleDef kScales[SC_COUNT] = {
    {7, {0, 2, 4, 5, 7, 9, 11}},                    // Major
    {7, {0, 2, 3, 5, 7, 8, 10}},                    // Minor (natural)
    {7, {0, 2, 3, 5, 7, 9, 10}},                    // Dorian
    {7, {0, 2, 4, 6, 7, 9, 11}},                    // Lydian
    {7, {0, 2, 4, 5, 7, 9, 10}},                    // Mixolydian
    {7, {0, 1, 3, 5, 7, 8, 10}},                    // Phrygian
    {5, {0, 2, 4, 7, 9}},                           // Major pentatonic
    {5, {0, 3, 5, 7, 10}},                          // Minor pentatonic
    {5, {0, 2, 3, 7, 8}},                           // Hirajoshi
    {5, {0, 1, 5, 7, 10}},                          // In-Sen
    {6, {0, 2, 4, 6, 8, 10}},                       // Whole tone
    {12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},   // Chromatic
};

// Each chord type as scale degrees stacked on the root's degree. Spread has the triad's tones;
// buildChord() always voices it Spread, or it would be the Triad again.
struct Stack {
    int n;
    int degrees[4];
};
constexpr Stack kStacks[CH_COUNT] = {
    {1, {0}},            // Off: the root alone
    {3, {0, 2, 4}},      // Triad
    {4, {0, 2, 4, 6}},   // Seventh
    {3, {0, 1, 4}},      // Sus2
    {3, {0, 3, 4}},      // Sus4
    {4, {0, 2, 4, 8}},   // Add9: the 9th is degree 8, an octave above the 2nd
    {3, {0, 3, 6}},      // Quartal: diatonic fourths, so one of them may be a tritone
    {3, {0, 4, 8}},      // Fifths
    {3, {0, 1, 2}},      // Cluster
    {3, {0, 2, 4}},      // Spread
};

// The tunings as ratios above the tonic, per semitone. Just: 5-limit, the ratios of the
// harmonic series' first few partials, so a held fifth or third doesn't beat. Pythagorean:
// everything from stacked 3:2 fifths, pure fifths and sharp thirds.
struct Ratio {
    int num, den;
};
constexpr Ratio kJust[12] = {{1, 1}, {16, 15}, {9, 8},  {6, 5}, {5, 4},  {4, 3},
                             {45, 32}, {3, 2}, {8, 5}, {5, 3}, {9, 5}, {15, 8}};
constexpr Ratio kPythagorean[12] = {{1, 1},     {256, 243}, {9, 8},    {32, 27}, {81, 64}, {4, 3},
                                    {729, 512}, {3, 2},     {128, 81}, {27, 16}, {16, 9},  {243, 128}};

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int pitchClass(int note) { return (note % 12 + 12) % 12; }   // of negative numbers too

// A patch's fields kept in range: a stray value plays something sensible and never reads past
// a table.
int keyOf(const HarmonyPatch& h) { return pitchClass(h.key); }
int scaleOf(int scale) { return clampi(scale, 0, SC_COUNT - 1); }

bool inScale(int scale, int key, int note) {
    const ScaleDef& s = kScales[scale];
    const int rel = pitchClass(note - key);
    for (int i = 0; i < s.size; ++i)
        if (s.steps[i] == rel) return true;
    return false;
}

// Insertion sort: never more than kChordMax notes.
void sortNotes(int* a, int n) {
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && a[j] < a[j - 1]; --j) {
            const int t = a[j];
            a[j] = a[j - 1];
            a[j - 1] = t;
        }
}

uint16_t pcsOf(const int* notes, int n) {
    uint16_t pcs = 0;
    for (int i = 0; i < n; ++i) pcs = static_cast<uint16_t>(pcs | 1u << pitchClass(notes[i]));
    return pcs;
}

int lowestOf(const Chord& c) {   // of a chord with notes
    const int n = clampi(c.n, 1, kChordMax);
    int lo = c.notes[0];
    for (int i = 1; i < n; ++i)
        if (c.notes[i] < lo) lo = c.notes[i];
    return lo;
}

// A single note into the chord range by octaves.
int foldNote(int note) {
    while (note < kChordLowest) note += 12;
    while (note > kChordHighest) note -= 12;
    return note;
}

// A sorted chord into the chord range by whole octaves, keeping its shape: every voicing spans
// far less than the seven octaves in between. The root moves with it.
void fit(Chord& c) {
    int shift = 0;
    while (c.notes[0] + shift < kChordLowest) shift += 12;
    while (c.notes[c.n - 1] + shift > kChordHighest) shift -= 12;
    for (int i = 0; i < c.n; ++i) c.notes[i] += shift;
    c.root += shift;
}

bool sameChord(const Chord& a, const Chord& b) {
    if (a.n != b.n || a.root != b.root || a.pcs != b.pcs) return false;
    for (int i = 0; i < a.n; ++i)
        if (a.notes[i] != b.notes[i]) return false;
    return true;
}

int chordTypeOf(const HarmonyPatch& h) { return clampi(h.chord, 0, CH_COUNT - 1); }

// The Spread chord type is always voiced Spread, or it would be the Triad again.
int voicingOf(const HarmonyPatch& h) {
    return chordTypeOf(h) == CH_SPREAD ? VO_SPREAD : clampi(h.voicing, 0, VO_COUNT - 1);
}

// The close voicing of h's chord type on `root`: the stack, each tone the next one up from the
// one before. Returns its tones, 1..4.
int closeVoicing(const HarmonyPatch& h, int root, int* out) {
    const Stack& stack = kStacks[chordTypeOf(h)];
    // Chromatic has no chords of its own: it borrows the major scale on the root.
    const bool chromatic = scaleOf(h.scale) == SC_CHROMATIC;
    const int scale = chromatic ? SC_MAJOR : scaleOf(h.scale);
    const int rel = chromatic ? 0 : pitchClass(root - keyOf(h));   // the root above the tonic
    // The root's degree, or the nearest one below when the root isn't in the scale: the stack is
    // built there and moved up to the root, so a played key always gets a chord on itself.
    const ScaleDef& s = kScales[scale];
    int r = 0;
    while (r + 1 < s.size && s.steps[r + 1] <= rel) ++r;
    for (int i = 0; i < stack.n; ++i) out[i] = root + scaleStep(scale, r + stack.degrees[i]) - scaleStep(scale, r);
    return stack.n;
}

// A voicing's rule applied to a close voicing (ascending), or to any inversion of one. Writes
// the notes ascending into `out` (room for kChordMax); returns how many.
int applyVoicing(int voicing, const int* close, int n, int* out) {
    int k = 0;
    auto add = [&k, out](int note) {
        if (k < kChordMax) out[k++] = note;
    };
    if (n < 3 || voicing == VO_CLOSE) {   // the root alone (Chord Off) has nothing to voice
        for (int i = 0; i < n; ++i) add(close[i]);
    } else if (voicing == VO_OPEN) {
        // Every second tone up an octave: a triad's 3rd above its 5th, a 10th from the root.
        for (int i = 0; i < n; ++i) add(close[i] + (i % 2 ? 12 : 0));
    } else if (voicing == VO_DROP2) {
        // The second-highest tone an octave down, under the lowest.
        for (int i = 0; i < n; ++i) add(close[i] - (i == n - 2 ? 12 : 0));
    } else {
        // Spread: the root and the stack's third tone (a triad's 5th) an octave down, the rest
        // of the close voicing above them, so the 5th sounds in both octaves.
        add(close[0] - 12);
        add(close[2] - 12);
        for (int i = 1; i < n; ++i) add(close[i]);
    }
    sortNotes(out, k);
    return k;
}

} // namespace

int scaleSize(int scale) { return kScales[scaleOf(scale)].size; }

int scaleStep(int scale, int degree) {
    const ScaleDef& s = kScales[scaleOf(scale)];
    degree = clampi(degree, -1000, 1000);   // far past any note, and no overflow below
    const int oct = degree >= 0 ? degree / s.size : -((-degree + s.size - 1) / s.size);   // floor
    return s.steps[degree - oct * s.size] + 12 * oct;
}

int mapInput(const HarmonyPatch& h, int note) {
    if (note < 0 || note > 127) return -1;
    const int key = keyOf(h), scale = scaleOf(h.scale);
    switch (h.input) {
    case IN_SNAP:
        // Outward from the note, below first: the nearest tone, the lower one on a tie. Every
        // scale has a tone within two semitones of any note (three at the ends of the range,
        // where one side is cut off), so this ends long before 12.
        for (int d = 0; d < 12; ++d) {
            if (note - d >= 0 && inScale(scale, key, note - d)) return note - d;
            if (note + d <= 127 && inScale(scale, key, note + d)) return note + d;
        }
        return note;
    case IN_DEGREES: {
        // The white key at or below the note, as a degree counted from C: a black key plays the
        // white key below it, and the octave's C plays the tonic. Chromatic has all twelve keys
        // in it, so they play as played, moved up by the key.
        static constexpr int kWhite[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
        int m = scale == SC_CHROMATIC ? note + key : note / 12 * 12 + key + scaleStep(scale, kWhite[note % 12]);
        while (m > 127) m -= 12;   // a high key's top octave: the same tone, an octave down
        return m;
    }
    default:   // As Played
        return note;
    }
}

Chord buildChord(const HarmonyPatch& h, int root) {
    Chord c;
    if (root < 0 || root > 127) return c;
    int close[4];
    const int n = closeVoicing(h, root, close);
    c.n = applyVoicing(voicingOf(h), close, n, c.notes);
    c.root = root;
    c.pcs = pcsOf(c.notes, c.n);
    fit(c);
    return c;
}

// The cost of moving from `prev` to `candidate`: how far the voices travel, in semitones. Both
// chords are sorted first. The same number of notes: each note of one moves to the note of the
// other in the same place, and the moves add up. Different numbers: every note of either chord
// goes to the nearest note of the other, and the sum (which counts each move from both ends) is
// halved, rounding down.
//
// TODO(Roland): this is the baseline, and the place where taste shows: it decides how Bloom moves
// between chords. Trade-offs worth weighing:
// - Penalise parallel fifths (and octaves)? Two voices a fifth apart moving the same way sound
//   hollow in classical writing; in ambient music that hollowness may be the point (organum).
// - Weight the upper voices more than the bass? The ear follows the top voice as a line, and the
//   bass may leap freely: Ground already holds the root underneath.
// - Prefer common tones staying exactly still? A plain sum lets a chord nudge every voice a
//   little rather than hold one and move another further. A bonus for each note held keeps more
//   of Bloom's voices sustaining, since moveTo() leaves common notes untouched.
// - Unequal counts: the halved nearest-note sum lets several voices share one target.
// Whatever replaces it must still lead, voiced Close, C (60 64 67) to F as 60 65 69 and to G as
// 59 62 67; and voiced Open, C (60 67 76) to F as 60 69 77.
int voiceLeadCost(const Chord& prev, const Chord& candidate) {
    const int na = clampi(prev.n, 0, kChordMax), nb = clampi(candidate.n, 0, kChordMax);
    if (na == 0 || nb == 0) return 0;   // nothing to move from, or to
    int a[kChordMax], b[kChordMax];
    for (int i = 0; i < na; ++i) a[i] = prev.notes[i];
    for (int i = 0; i < nb; ++i) b[i] = candidate.notes[i];
    sortNotes(a, na);
    sortNotes(b, nb);
    int cost = 0;
    if (na == nb) {
        for (int i = 0; i < na; ++i) cost += std::abs(a[i] - b[i]);
        return cost;
    }
    auto nearest = [](int note, const int* to, int n) {
        int d = std::abs(note - to[0]);
        for (int i = 1; i < n; ++i)
            if (std::abs(note - to[i]) < d) d = std::abs(note - to[i]);
        return d;
    };
    for (int i = 0; i < na; ++i) cost += nearest(a[i], b, nb);
    for (int i = 0; i < nb; ++i) cost += nearest(b[i], a, na);
    return cost / 2;
}

// The candidates keep the voicing's character. `c` as built comes first (so it wins a full tie);
// then each inversion of the chord's close voicing, voiced by h's rule (Open: every second tone
// of the inversion up an octave; Drop 2: its second-highest down an octave; Close: as it is),
// each in place, an octave down and an octave up. Spread never inverts: its root stays at the
// bottom, so its candidates are the whole shape moved by octaves. Only candidates within the
// chord range and without a doubled note count (an inversion can land on a tone the stack
// already has an octave up, as the whole-tone Seventh's root). The least cost wins; a tie goes to
// the lowest note nearest prev's lowest, so the chord doesn't wander up or down the keyboard;
// then to the earlier candidate.
Chord leadFrom(const HarmonyPatch& h, const Chord& prev, const Chord& c) {
    const int cn = clampi(c.n, 0, kChordMax);
    if (prev.n <= 0 || cn == 0 || c.root < 0) return c;
    if (chordTypeOf(h) == CH_OFF) return c;   // a single note plays as played, never an octave off
    int inv[4];
    const int n = closeVoicing(h, c.root, inv);
    const uint16_t pcs = pcsOf(c.notes, cn);
    if (pcsOf(inv, n) != pcs) return c;   // not a chord h builds: nothing to invert it by
    const int voicing = voicingOf(h);
    const int prevLow = lowestOf(prev);

    Chord best = c;
    int bestCost = voiceLeadCost(prev, c), bestLow = std::abs(lowestOf(c) - prevLow);
    auto consider = [&](const int* notes, int k) {
        static constexpr int kShifts[3] = {0, -12, 12};
        for (int shift : kShifts) {
            Chord cand;
            cand.n = k;
            bool ok = true;
            for (int i = 0; i < k; ++i) {
                cand.notes[i] = notes[i] + shift;
                ok = ok && cand.notes[i] >= kChordLowest && cand.notes[i] <= kChordHighest &&
                     (i == 0 || cand.notes[i] > cand.notes[i - 1]);
            }
            if (!ok) continue;
            cand.root = c.root;
            cand.pcs = pcs;
            const int cost = voiceLeadCost(prev, cand), low = std::abs(cand.notes[0] - prevLow);
            if (cost < bestCost || (cost == bestCost && low < bestLow)) {
                best = cand;
                bestCost = cost;
                bestLow = low;
            }
        }
    };
    if (voicing == VO_SPREAD) {
        consider(c.notes, cn);
        return best;
    }
    for (int r = 0; r < n; ++r) {
        int voiced[kChordMax];
        consider(voiced, applyVoicing(voicing, inv, n, voiced));
        // The next inversion: the lowest note up an octave. A close voicing wider than an octave
        // (Add9's) doesn't put it on top, so sort again.
        inv[0] += 12;
        sortNotes(inv, n);
    }
    return best;
}

double tunedPitch(const HarmonyPatch& h, int note) {
    const int tuning = clampi(h.tuning, 0, TU_COUNT - 1);
    if (tuning == TU_EQUAL) return note;
    const int pc = pitchClass(note - keyOf(h));   // semitones above the tonic below the note
    const Ratio& r = (tuning == TU_JUST ? kJust : kPythagorean)[pc];
    return static_cast<double>(note - pc) + 12.0 * std::log2(static_cast<double>(r.num) / r.den);
}

// --- the memory ---------------------------------------------------------------------------

void Harmony::set(const HarmonyPatch& h) { patch_ = h; }

void Harmony::noteOn(int mappedNote) {
    if (mappedNote < 0 || mappedNote > 127) return;   // a dropped note
    for (int i = 0; i < nHeld_; ++i)
        if (held_[i] == mappedNote) return;   // already down: a double note-on changes nothing
    if (nHeld_ == kHeldMax) return;
    held_[nHeld_++] = mappedNote;
    releasedBeats_ = 0.0;
    if (clampi(patch_.chord, 0, CH_COUNT - 1) == CH_OFF) {
        change(heldChord());
        return;
    }
    const Chord c = buildChord(patch_, mappedNote);
    change(patch_.leading && current_.root >= 0 ? leadFrom(patch_, current_, c) : c);
}

void Harmony::noteOff(int mappedNote) {
    int w = 0;
    for (int i = 0; i < nHeld_; ++i)
        if (held_[i] != mappedNote) held_[w++] = held_[i];
    if (w == nHeld_) return;   // not a key that is down
    nHeld_ = w;
    // Fingers leave a chord one by one: only the last key up counts, and the chord stays as it
    // was while it lasted.
    if (nHeld_ > 0) return;
    releasedBeats_ = 0.0;
    if (patch_.memoryBars == 0) change(Chord{});
}

void Harmony::advance(double seconds, double bpm) {
    if (nHeld_ > 0 || current_.root < 0) return;   // keys down, or nothing kept
    // Counted in beats, so a tempo change on the way counts at the tempo it came in. It runs under
    // Forever too: Memory turned down later forgets a chord older than its bars at once.
    if (!std::isfinite(bpm) || bpm <= 0.0) bpm = 120.0;
    if (std::isfinite(seconds) && seconds > 0.0) releasedBeats_ += seconds * bpm / 60.0;
    const int bars = patch_.memoryBars;
    if (bars < 0) return;   // Forever
    if (bars == 0 || releasedBeats_ >= 4.0 * (bars < 64 ? bars : 64)) change(Chord{});   // 4/4
}

void Harmony::clear() {
    nHeld_ = 0;
    releasedBeats_ = 0.0;
    change(Chord{});
}

int Harmony::lowestHeld() const {
    int lo = -1;
    for (int i = 0; i < nHeld_; ++i)
        if (lo < 0 || held_[i] < lo) lo = held_[i];
    return lo;
}

// Chord Off: the keys held, as played, the lowest as the root. More keys than a chord has notes:
// the lowest and then the latest, so a key just pressed is always heard. The notes go into the
// chord range by octaves, as buildChord's do (and a note landing on one already there is one).
Chord Harmony::heldChord() const {
    Chord c;
    const int lo = lowestHeld();
    if (lo < 0) return c;
    auto add = [&c](int note) {
        note = foldNote(note);
        for (int i = 0; i < c.n; ++i)
            if (c.notes[i] == note) return;
        c.notes[c.n++] = note;
    };
    add(lo);
    for (int i = nHeld_ - 1; i >= 0 && c.n < kChordMax; --i)
        if (held_[i] != lo) add(held_[i]);
    sortNotes(c.notes, c.n);
    c.root = foldNote(lo);
    c.pcs = pcsOf(c.notes, c.n);
    return c;
}

void Harmony::change(const Chord& c) {
    if (sameChord(c, current_)) return;
    current_ = c;
    ++version_;
}

} // namespace af
