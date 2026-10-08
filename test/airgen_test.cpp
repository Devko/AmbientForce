// Air's generator on its own (docs/plans/2026-10-07-m2-weather.md, Task 4; CONCEPT §13's `air`
// test): the same seed gives the same events and no setting shifts them; Gravity, a motif kept on a
// chord of fewer tones than it has notes; no repeats within two, the fallbacks when the weights
// leave too little, and a chord that leaves one allowed tone;
// Density's rate and the gaps' shape; the range; Rise and Fall; Constellation and Mutate; Echo; the
// Loop, free and synced, through Register, Range and chord changes, and where it comes round; Notes
// off; odd input; a knob turning (the candidates worked out only when they change). Pure logic, so
// every expectation is worked out here from the rules in dsp/airgen.h.
#include "check.h"
#include "../dsp/airgen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

namespace aft {
namespace {

using af::AirEvent;
using af::AirGen;
using af::AirGenPatch;
using af::Chord;
using af::HarmonyPatch;

constexpr int kStep = 128;
constexpr double kSr = 44100.0;

struct Ev {
    int64_t at;   // samples since the run began
    int note;
    float vel;
    bool played;  // a replay of the player's note
};

// A generator and the events it gave, stepped as Air steps it: 128 samples at a time; when `synced`,
// MPC's transport (playing) set before each step at its first sample. `beats` is the song position
// at `now`: a test may move it (a locate) or change `bpm` between steps.
struct Run {
    AirGen g;
    std::vector<Ev> ev;
    std::vector<double> evBeats;   // the song position at each event
    int64_t now = 0;
    bool synced = false;
    double bpm = 120.0, beats = 0.0;
    int room = 16;   // step()'s max

    void step(int n = kStep) {
        if (synced) g.setTransport(bpm, beats, true);
        AirEvent out[16];
        const int k = g.step(n, out, room);
        for (int i = 0; i < k; ++i) {
            ev.push_back({now + out[i].offset, out[i].note, out[i].vel, out[i].played});
            evBeats.push_back(beats + out[i].offset * bpm / 60.0 / kSr);
        }
        now += n;
        beats += n * bpm / 60.0 / kSr;
    }
    void seconds(double s, int chunk = kStep) {   // `chunk`: samples a step (any n works)
        const int64_t end = now + static_cast<int64_t>(s * kSr);
        while (now < end) step(chunk);
    }
    void events(size_t k) {   // until k events in all (or 4 hours)
        const int64_t limit = now + static_cast<int64_t>(4 * 3600 * kSr);
        while (ev.size() < k && now < limit) step();
    }
    // The player's note at sample t (in the step that holds it; call before stepping past it).
    void playAt(int64_t t, int note, float vel) {
        while (now + kStep <= t) step();
        g.played(note, vel, static_cast<int>(t - now));
    }
    // The player's note on song position b (synced runs); returns its sample.
    int64_t playOnBeat(double b, int note, float vel) {
        while (beats + kStep * bpm / 60.0 / kSr <= b) step();
        const int off = static_cast<int>(std::lround((b - beats) * 60.0 / bpm * kSr));
        g.played(note, vel, off);
        return now + off;
    }
};

Chord chordOf(std::initializer_list<int> notes) {
    Chord c;
    for (int n : notes) {
        c.notes[c.n++] = n;
        c.pcs = static_cast<uint16_t>(c.pcs | 1u << (n % 12));
    }
    c.root = c.notes[0];
    return c;
}
Chord triadC() { return chordOf({60, 64, 67}); }

AirGenPatch patchOf(int pattern, float density = 60.0f) {
    AirGenPatch p;
    p.pattern = pattern;
    p.density = density;
    return p;
}

HarmonyPatch keyOf(int key, int scale = af::SC_MAJOR) {
    HarmonyPatch h;
    h.key = key;
    h.scale = scale;
    return h;
}

void start(Run& r, const AirGenPatch& p, uint32_t seed = 1, const HarmonyPatch& h = HarmonyPatch{},
           const Chord& c = triadC(), bool generate = true) {
    r.g.set(p, h);
    r.g.setChord(c, generate);
    r.g.seed(seed);
}

bool inScale(int scale, int key, int note) {
    const int rel = ((note - key) % 12 + 12) % 12;
    for (int d = 0; d < af::scaleSize(scale); ++d)
        if (af::scaleStep(scale, d) == rel) return true;
    return false;
}

int rangeLo(const AirGenPatch& p, const HarmonyPatch& h) { return 12 * (p.registerOct + 1) + h.key; }
int rangeHi(const AirGenPatch& p, const HarmonyPatch& h) {
    return std::min(rangeLo(p, h) + static_cast<int>(std::floor(12.0 * p.rangeOct + 1e-3)), 108);
}

// The candidates as the header defines them: the scale's tones in the range, ascending.
std::vector<int> candidates(const AirGenPatch& p, const HarmonyPatch& h) {
    std::vector<int> c;
    for (int n = rangeLo(p, h); n <= rangeHi(p, h); ++n)
        if (inScale(h.scale, h.key, n)) c.push_back(n);
    return c;
}

int indexIn(const std::vector<int>& c, int note) {
    for (size_t i = 0; i < c.size(); ++i)
        if (c[i] == note) return static_cast<int>(i);
    return -1;
}

// The nearest of `c` to `note`, the lower on a tie.
int nearestOf(const std::vector<int>& c, int note) {
    int best = c[0];
    for (int x : c)
        if (std::abs(x - note) < std::abs(best - note)) best = x;
    return best;
}

// The nearest of `c` to `note` (the lower on a tie) that is none of `ex`; -1: none.
int nearestBut(const std::vector<int>& c, int note, std::initializer_list<int> ex) {
    int best = -1;
    for (int x : c)
        if (std::find(ex.begin(), ex.end(), x) == ex.end() && (best < 0 || std::abs(x - note) < std::abs(best - note)))
            best = x;
    return best;
}

// A generated note of a loop as the range and the harmony have it now (dsp/airgen.h's Loop), while
// every candidate is allowed (Gravity under 1): recorded as `q` at Register `then`, moved by
// Register's octaves since, folded into the range by octaves (a pitch class with no octave there: to
// the octave nearest the range), then the nearest candidate.
int loopBase(int q, int then, const AirGenPatch& p, const HarmonyPatch& h) {
    const int lo = rangeLo(p, h), hi = rangeHi(p, h);
    int n = q + 12 * (p.registerOct - then);
    while (n < lo) n += 12;
    while (n > hi) n -= 12;
    if (n < lo && lo - n > n + 12 - hi) n += 12;
    return nearestOf(candidates(p, h), n);
}

// As it replays after `prev` and `prev2`, before the loop's next two notes (their loopBase), in a loop
// of more than two: where it would repeat either of the two before it, the nearest that is none of
// the four, else neither next to it, else as it is.
int loopNote(int base, int prev, int prev2, int next, int next2, const std::vector<int>& c) {
    if (base != prev && base != prev2) return base;
    int n = nearestBut(c, base, {prev, next, prev2, next2});
    if (n < 0) n = nearestBut(c, base, {prev, next});
    return n >= 0 ? n : base;
}

// No note equal to either of the two before it (within = 2), or to the one before (within = 1).
bool noRepeats(const std::vector<Ev>& ev, int within, size_t from = 0) {
    for (size_t i = from + 1; i < ev.size(); ++i) {
        if (ev[i].note == ev[i - 1].note || (within >= 2 && i >= from + 2 && ev[i].note == ev[i - 2].note)) {
            std::printf("  a repeat at event %zu:", i);
            for (size_t k = i >= 6 ? i - 6 : 0; k <= i; ++k) std::printf(" %d", ev[k].note);
            std::printf("\n");
            return false;
        }
    }
    return true;
}

const char* kPat[] = {"Random", "Rise", "Fall", "Constellation", "Echo"};

// --- 1. the same seed, the same events; no setting shifts them ----------------------------------

void testSameSeed() {
    std::printf("== airgen: the same seed, the same events\n");
    bool same = true, other = true;
    size_t total = 0;
    for (int pat = 0; pat < af::AP_COUNT; ++pat) {
        Run a, b, c;
        start(a, patchOf(pat, 30.0f), 7);
        start(b, patchOf(pat, 30.0f), 7);
        start(c, patchOf(pat, 30.0f), 8);
        a.seconds(600);
        b.seconds(600);
        c.seconds(600);
        same = same && a.ev.size() == b.ev.size() && a.ev.size() > 200;
        for (size_t i = 0; same && i < a.ev.size(); ++i)
            same = a.ev[i].at == b.ev[i].at && a.ev[i].note == b.ev[i].note && a.ev[i].vel == b.ev[i].vel;
        bool differ = a.ev.size() != c.ev.size();
        for (size_t i = 0; !differ && i < a.ev.size(); ++i) differ = a.ev[i].at != c.ev[i].at || a.ev[i].note != c.ev[i].note;
        other = other && differ;
        total += a.ev.size();
    }
    CHECK(same);
    CHECK(other);
    std::printf("  5 patterns x 10 min at 30 a minute: %zu events, the same twice; seed 8 gives others\n", total);

    // reset() gives the seed's events again.
    {
        Run a;
        start(a, patchOf(af::AP_CONSTELLATION, 30.0f), 7);
        a.seconds(120);
        const std::vector<Ev> first = a.ev;
        a.g.reset();
        a.ev.clear();
        const int64_t t0 = a.now;
        a.seconds(120);
        bool again = a.ev.size() == first.size() && !first.empty();
        for (size_t i = 0; again && i < first.size(); ++i)
            again = a.ev[i].at - t0 == first[i].at && a.ev[i].note == first[i].note && a.ev[i].vel == first[i].vel;
        CHECK(again);
    }

    // No setting shifts the sequence. A run that keeps changing every setting but Density and Rubato
    // (each pattern, Gravity, Register, Range, Motif, Mutate, the key, the chord, the player's notes,
    // and not generating for a minute) plays at the very samples, and at the very velocities, of a
    // plain Random run with the same seed: the same events, minus the minute it didn't generate.
    {
        Run a, b;
        start(a, patchOf(af::AP_RANDOM, 30.0f), 11);
        start(b, patchOf(af::AP_RANDOM, 30.0f), 11);
        a.seconds(600);
        int64_t offFrom = 0, offTo = 0;   // where b was told to stop generating, and to start again
        const Chord chords[3] = {triadC(), chordOf({62, 65, 69}), chordOf({67, 71, 74})};
        for (int k = 0; k < 30; ++k) {
            AirGenPatch p = patchOf(k % af::AP_COUNT, 30.0f);
            p.gravity = (k % 3) * 0.5f;
            p.registerOct = 4 + k % 3;
            p.rangeOct = 0.5f + (k % 6) * 0.5f;
            p.motif = 3 + k % 6;
            p.mutate = (k % 4) * 0.33f;
            b.g.set(p, keyOf(k % 5 == 4 ? 2 : 0, k % 7 == 6 ? af::SC_DORIAN : af::SC_MAJOR));
            if (k == 10) offFrom = b.now;
            if (k == 13) offTo = b.now;
            b.g.setChord(chords[k % 3], k < 10 || k >= 13);
            if (k % 4 == 1) {
                b.g.played(60 + k % 12, 0.8f, 5);
                b.g.played(67 + k % 5, 0.6f, 9);
            }
            b.seconds(20);
        }
        size_t j = 0, missing = 0, extra = 0, skipped = 0;
        bool vel = true;
        for (const Ev& e : a.ev) {
            if (e.at >= offFrom && e.at < offTo) {
                ++skipped;
                continue;
            }
            while (j < b.ev.size() && b.ev[j].at < e.at) {
                ++extra;
                ++j;
            }
            if (j < b.ev.size() && b.ev[j].at == e.at) {
                vel = vel && b.ev[j].vel == e.vel;
                ++j;
            } else {
                ++missing;
            }
        }
        extra += b.ev.size() - j;
        size_t inside = 0;
        for (const Ev& e : b.ev) inside += e.at >= offFrom && e.at < offTo ? 1 : 0;
        CHECK(missing == 0 && extra == 0);
        CHECK(vel);
        CHECK(inside == 0);
        CHECK(noRepeats(b.ev, 1));   // through every change, never the note before
        std::printf("  30 changes of every setting, 10 min: %zu events where a plain run has them (%zu missing, %zu extra, "
                    "same velocities %s); none in the minute it didn't generate\n",
                    a.ev.size() - skipped, missing, extra, vel ? "yes" : "no");
    }

    // Density only moves the events in time: at 60 a minute the k-th event comes at half the time it
    // comes at 30, with the same note and velocity.
    {
        Run a, b;
        start(a, patchOf(af::AP_CONSTELLATION, 30.0f), 13);
        start(b, patchOf(af::AP_CONSTELLATION, 60.0f), 13);
        a.seconds(600);
        b.seconds(300);
        const size_t n = std::min(a.ev.size(), b.ev.size());
        int64_t worst = 0;
        bool same = n > 200 && std::max(a.ev.size(), b.ev.size()) - n <= 1;
        for (size_t i = 0; i < n; ++i) {
            worst = std::max(worst, std::abs(a.ev[i].at - 2 * b.ev[i].at));
            same = same && a.ev[i].note == b.ev[i].note && a.ev[i].vel == b.ev[i].vel;
        }
        CHECK(same);
        CHECK(worst <= 4);
        std::printf("  Density 30 vs 60: %zu events, the same notes and velocities, at twice the time within %lld samples\n",
                    n, static_cast<long long>(worst));
    }
}

// --- 2. Gravity -----------------------------------------------------------------------------------

void testGravity() {
    std::printf("== airgen: Gravity\n");
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_RANDOM);
        p.gravity = 1.0f;
        start(r, p, 3);
        r.events(1000);
        bool chord = r.ev.size() >= 1000;
        for (const Ev& e : r.ev) chord = chord && (e.note % 12 == 0 || e.note % 12 == 4 || e.note % 12 == 7);
        CHECK(chord);
        std::printf("  Gravity 1: %zu Random events, all chord tones: %s\n", r.ev.size(), chord ? "yes" : "no");
    }
    // Every pattern at Gravity 1 (Echo before the player gives a note): only chord tones. Rise and
    // Fall walk the chord tones of the range (72 76 79 84 88 91 96), one or two at a time.
    {
        const int tones[7] = {72, 76, 79, 84, 88, 91, 96};
        auto toneIndex = [&tones](int note) {
            for (int i = 0; i < 7; ++i)
                if (tones[i] == note) return i;
            return -1;
        };
        bool chord = true, walk = true;
        int off = 0;
        for (int pat = 0; pat < af::AP_COUNT; ++pat) {
            Run r;
            AirGenPatch p = patchOf(pat);
            p.gravity = 1.0f;
            start(r, p, 7);
            r.events(600);
            for (size_t i = 0; i < r.ev.size(); ++i) {
                const int a = i > 0 ? toneIndex(r.ev[i - 1].note) : -1, b = toneIndex(r.ev[i].note);
                chord = chord && b >= 0;
                off += b < 0;
                if (i > 0 && (pat == af::AP_RISE || pat == af::AP_FALL)) {
                    const int up = ((pat == af::AP_RISE ? b - a : a - b) + 7) % 7;
                    walk = walk && a >= 0 && b >= 0 && (up == 1 || up == 2);
                }
            }
            chord = chord && r.ev.size() >= 600;
        }
        CHECK(chord);
        CHECK(walk);
        std::printf("  Gravity 1, every pattern, 600 events each: %d off the chord; Rise and Fall a chord tone or two at "
                    "a time: %s\n",
                    off, walk ? "yes" : "no");
    }
    // Gravity 1 keeps a motif on the chord where the chord has fewer tones in the range than the
    // motif has notes: Register 5, Range 1 (72..84) holds C's 72 76 79 84, four against Motif 5. A
    // motif drawn at Gravity 0.6, then Gravity 1: from the crossing on, C's tones only; then F's chord
    // (72 77 81 84): F's only. Never the note before. A moved note keeps out the notes next to it on
    // the chord before it leaves the chord to keep out the ones two away (eight seeds).
    {
        bool onC = true, onF = true, never = true;
        size_t total = 0;
        auto of = [](int note, int a, int b, int c) { return note % 12 == a || note % 12 == b || note % 12 == c; };
        for (uint32_t seed = 1; seed <= 8; ++seed) {
            Run r;
            AirGenPatch p = patchOf(af::AP_CONSTELLATION, 30.0f);
            p.rangeOct = 1.0f;
            p.mutate = 0.0f;
            start(r, p, seed);
            r.seconds(30);
            p.gravity = 1.0f;
            r.g.set(p, HarmonyPatch{});
            const size_t at1 = r.ev.size();
            r.seconds(60);
            for (size_t i = at1; i < r.ev.size(); ++i) onC = onC && of(r.ev[i].note, 0, 4, 7);
            r.g.setChord(chordOf({65, 69, 72}), true);
            const size_t atF = r.ev.size();
            r.seconds(60);
            for (size_t i = atF; i < r.ev.size(); ++i) onF = onF && of(r.ev[i].note, 5, 9, 0);
            never = never && at1 > 0 && atF > at1 + 20 && r.ev.size() > atF + 20 && noRepeats(r.ev, 1, at1 - 1);
            total += r.ev.size() - at1;
        }
        CHECK(onC && onF);
        CHECK(never);
        std::printf("  Range 1 (4 chord tones), Motif 5, Gravity 0.6 -> 1, then C -> F: %zu notes, all on the chord %s, never "
                    "the note before %s\n",
                    total, onC && onF ? "yes" : "NO", never ? "yes" : "NO");
    }
    // Range 23/12: C5..B6, 14 scale tones of which 6 are the triad's, the scale's share (3/7) exactly.
    for (float g : {0.0f, 0.6f}) {
        Run r;
        AirGenPatch p = patchOf(af::AP_RANDOM);
        p.gravity = g;
        p.rangeOct = 23.0f / 12.0f;
        start(r, p, 5);
        r.events(1000);
        int chord = 0;
        for (const Ev& e : r.ev) chord += e.note % 12 == 0 || e.note % 12 == 4 || e.note % 12 == 7;
        const double share = chord / static_cast<double>(r.ev.size()), want = 6.0 / (6.0 + 8.0 * (1.0 - g));
        CHECK(std::fabs(share - want) <= 0.05);
        std::printf("  Gravity %.1f: chord tones %.1f%% of %zu (their weight's share %.1f%%)\n", g, 100 * share, r.ev.size(),
                    100 * want);
    }
}

// --- 3. no repeats within two, and the fallbacks --------------------------------------------------

void testNoRepeats() {
    std::printf("== airgen: no repeats within two\n");
    for (int pat = 0; pat < af::AP_COUNT; ++pat) {
        Run r;
        start(r, patchOf(pat), 17);
        r.events(1000);
        const bool ok = r.ev.size() >= 1000 && noRepeats(r.ev, 2);
        CHECK(ok);
        std::printf("  %-13s 1000 events: %s\n", kPat[pat], ok ? "none" : "REPEATS");
    }
    // Echo with the player's notes, repeats among them too, given every 40 events.
    {
        Run r;
        start(r, patchOf(af::AP_ECHO), 19);
        const int figures[4][5] = {{62, 64, 62, 67, -1}, {67, 67, 69, 72, 74}, {72, 74, -1, -1, -1}, {60, 64, 67, 64, 60}};
        for (int k = 0; k < 25; ++k) {
            r.events(40 * (k + 1));
            for (int i = 0; i < 5 && figures[k % 4][i] >= 0; ++i) r.g.played(figures[k % 4][i], 0.7f, i * 20);
        }
        const bool ok = noRepeats(r.ev, 2);
        CHECK(ok);
        std::printf("  Echo with the player's figures (repeats in them): %zu events, %s\n", r.ev.size(), ok ? "none" : "REPEATS");
    }

    // The fallbacks. A chord of one tone (C) with Gravity 1, Register 4 and Range 0.5: the candidates
    // 60 62 64 65, only 60 weighing anything. Random: 60, then (60 being the last) an even draw among
    // the others that isn't the one before 60, then 60 again: the chord tone every other note.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_RANDOM);
        p.gravity = 1.0f;
        p.registerOct = 4;
        p.rangeOct = 0.5f;
        start(r, p, 23, HarmonyPatch{}, chordOf({60}));
        r.events(1000);
        const size_t i0 = !r.ev.empty() && r.ev[0].note == 60 ? 0 : 1;
        bool alt = r.ev.size() >= 1000;
        int others[3] = {};
        for (size_t i = i0; i < r.ev.size(); ++i) {
            const int n = r.ev[i].note;
            if ((i - i0) % 2 == 0) alt = alt && n == 60;
            else {
                alt = alt && (n == 62 || n == 64 || n == 65) && (i < i0 + 3 || n != r.ev[i - 2].note);
                others[n == 62 ? 0 : n == 64 ? 1 : 2]++;
            }
        }
        CHECK(alt);
        CHECK(others[0] > 100 && others[1] > 100 && others[2] > 100);
        std::printf("  one chord tone, Gravity 1, Range 0.5: 60 every other note, the others %d %d %d: %s\n", others[0],
                    others[1], others[2], alt ? "yes" : "no");
    }
    // Every pattern in that corner (Motif 8, more than the four candidates can keep apart), and with
    // three candidates (C minor pentatonic, Range 0.5: 60 63 65): never the note before. Random, Rise
    // and Fall keep both rules with three.
    for (int corner = 0; corner < 2; ++corner) {
        bool one = true, two = true;
        for (int pat = 0; pat < af::AP_COUNT; ++pat) {
            Run r;
            AirGenPatch p = patchOf(pat);
            p.registerOct = 4;
            p.rangeOct = 0.5f;
            p.motif = 8;
            p.mutate = 0.5f;
            if (corner == 0) {
                p.gravity = 1.0f;
                start(r, p, 29, HarmonyPatch{}, chordOf({60}));
            } else {
                start(r, p, 31, keyOf(0, af::SC_MIN_PENT));
            }
            for (int k = 0; k < 10; ++k) {
                r.events(100 * (k + 1));
                if (pat == af::AP_ECHO) {
                    r.g.played(60, 0.5f, 0);
                    r.g.played(corner == 0 ? 64 : 63, 0.5f, 1);
                    r.g.played(65, 0.5f, 2);
                    r.g.played(48, 0.5f, 3);
                }
            }
            bool notes = r.ev.size() >= 1000;
            for (const Ev& e : r.ev) notes = notes && e.note >= 60 && e.note <= 66;
            one = one && notes && noRepeats(r.ev, 1);
            if (corner == 1 && pat <= af::AP_FALL) two = two && noRepeats(r.ev, 2);
        }
        CHECK(one);
        if (corner == 1) CHECK(two);
        std::printf("  %s, every pattern, Motif 8: never the note before %s%s\n",
                    corner == 0 ? "one chord tone (4 candidates)" : "3 candidates", one ? "yes" : "NO",
                    corner == 1 ? (two ? "; Random, Rise, Fall within two: yes" : "; within two: NO") : "");
    }

    // A chord that leaves one allowed tone in the range doesn't make the motif that tone throughout.
    // Gravity 1, Register 4, Range 1 (C major 60..72, eight candidates), a motif on C's tones; then D
    // alone, so 62 is the only chord tone. A moved note takes 62 wherever that repeats neither note
    // next to it, two of the motif's five places, and the nearest other candidates elsewhere; one of
    // the two 62s repeats the note two before it and is passed over as the motif plays. So 62 comes
    // every fourth note, and no note repeats either of the two before it. Constellation, and Echo of
    // the player's five.
    for (int pat : {af::AP_CONSTELLATION, af::AP_ECHO}) {
        Run r;
        AirGenPatch p = patchOf(pat);
        p.gravity = 1.0f;
        p.registerOct = 4;
        p.rangeOct = 1.0f;
        p.mutate = 0.5f;
        start(r, p, 83);
        r.events(20);
        if (pat == af::AP_ECHO)
            for (int n : {60, 64, 67, 72, 65}) r.g.played(n, 0.7f, 0);
        r.events(60);
        r.g.setChord(chordOf({62}), true);
        const size_t from = r.ev.size();
        r.events(from + 300);
        int tone = 0;
        bool fourth = true;   // 62 in every four notes in a row, once the motif has changed
        for (size_t i = from; i < r.ev.size(); ++i) {
            tone += r.ev[i].note == 62;
            if (i >= from + 5 && i + 4 <= r.ev.size())
                fourth = fourth && (r.ev[i].note == 62 || r.ev[i + 1].note == 62 || r.ev[i + 2].note == 62 ||
                                    r.ev[i + 3].note == 62);
        }
        const bool ok = r.ev.size() >= from + 300 && noRepeats(r.ev, 2, from - 2);
        CHECK(ok);
        CHECK(fourth);
        std::printf("  %s, then a chord of one allowed tone (62): 300 events, no repeat within two %s, 62 %d times, in "
                    "every four notes %s\n",
                    kPat[pat], ok ? "yes" : "NO", tone, fourth ? "yes" : "NO");
    }

    // A motif drawn one note at a time between Random's notes can't see them: in the one-chord-tone
    // corner (Gravity 1, C alone, Range 0.5: 60 62 64 65, Motif 3) each of its draws keeps out only
    // the note before, Random's, and takes the chord tone, so the motif is 60 60 60. Replayed, every
    // note of it is the last one: the nearest that keeps the rule plays instead, so never the note
    // before (60 every other note, as Random's draws give it).
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_CONSTELLATION);
        p.gravity = 1.0f;
        p.registerOct = 4;
        p.rangeOct = 0.5f;
        p.motif = 3;
        p.mutate = 0.0f;
        start(r, p, 89, HarmonyPatch{}, chordOf({60}));
        for (int k = 1; k <= 5; ++k) {   // Constellation, Random, Constellation, Random, Constellation
            p.pattern = k % 2 ? af::AP_CONSTELLATION : af::AP_RANDOM;
            r.g.set(p, HarmonyPatch{});
            r.events(static_cast<size_t>(k));
        }
        const bool drawn = r.ev.size() == 5 && r.ev[0].note == 60 && r.ev[2].note == 60 && r.ev[4].note == 60;
        p.pattern = af::AP_CONSTELLATION;
        r.g.set(p, HarmonyPatch{});
        r.events(305);
        const bool ok = r.ev.size() >= 305 && noRepeats(r.ev, 1);
        CHECK(drawn);
        CHECK(ok);
        std::printf("  a motif drawn between Random's notes, 60 60 60: 300 events on, never the note before %s\n",
                    ok ? "yes" : "NO");
    }
}

// --- 4. Density ---------------------------------------------------------------------------------

void testDensity() {
    std::printf("== airgen: Density\n");
    // A Poisson count over 10 min at 6 a minute is 60 +- 7.7 (one sigma), so +-10% is under one sigma
    // there: a seed passing says little, and about a third of them fail (seed 1 gives 68, +13%). So
    // each rate is checked twice: one seed that passes (3) within +-10%, as the plan asks; and the mean
    // over enough other seeds for about 10 000 events, within +-3%, three sigma, so it is the rate that
    // is tested, not one lucky sequence.
    // The seeds' runs step 4096 samples at a time, 32 times fewer calls than Air's 128 (they were most
    // of the suite's time). That counts the same events: seed 3 stepped both ways gives each event at
    // the same sample, or one beside it (an event in a step's last half sample is rounded into that
    // step), with the same note and velocity. A clock that lost or gained work where a step ends
    // would drift apart here over the 10 minutes, at 6 a minute most (3400 step ends an event).
    std::vector<double> gaps;
    bool chunks = true;
    for (float d : {6.0f, 12.0f, 60.0f}) {
        Run one, big;
        start(one, patchOf(af::AP_RANDOM, d), 3);
        start(big, patchOf(af::AP_RANDOM, d), 3);
        one.seconds(600);
        big.seconds(600, 4096);
        const double want = 10.0 * d, got = static_cast<double>(one.ev.size());
        CHECK(std::fabs(got / want - 1.0) <= 0.10);
        bool same = big.ev.size() == one.ev.size() && !one.ev.empty();
        for (size_t i = 0; same && i < one.ev.size(); ++i)
            same = std::llabs(big.ev[i].at - one.ev[i].at) <= 1 && big.ev[i].note == one.ev[i].note &&
                   big.ev[i].vel == one.ev[i].vel;
        chunks = chunks && same;
        const int seeds = static_cast<int>(std::ceil(10000.0 / want));
        double total = 0.0;
        for (int s = 100; s < 100 + seeds; ++s) {
            Run r;
            start(r, patchOf(af::AP_RANDOM, d), static_cast<uint32_t>(s));
            r.seconds(600, 4096);
            total += static_cast<double>(r.ev.size());
            if (d == 60.0f)
                for (size_t i = 1; i < r.ev.size(); ++i) gaps.push_back(static_cast<double>(r.ev[i].at - r.ev[i - 1].at) / kSr);
        }
        const double mean = total / seeds;
        CHECK(std::fabs(mean / want - 1.0) <= 0.03);
        std::printf("  %2.0f a minute: seed 3 %3.0f in 10 min (%+.1f%%; in steps of 4096 the same events: %s); %d seeds' "
                    "mean %.1f (%+.2f%%)\n",
                    d, got, 100.0 * (got / want - 1.0), same ? "yes" : "NO", seeds, mean, 100.0 * (mean / want - 1.0));
    }
    CHECK(chunks);
    // The gaps are exponential: 1 - 1/e of them (63.2%) shorter than the mean.
    size_t shorter = 0;
    for (double g : gaps) shorter += g < 1.0 ? 1 : 0;
    const double frac = static_cast<double>(shorter) / static_cast<double>(gaps.size());
    CHECK(std::fabs(frac - (1.0 - std::exp(-1.0))) <= 0.02);
    std::printf("  60 a minute: %.1f%% of %zu gaps shorter than 1 s (exponential: 63.2%%)\n", 100 * frac, gaps.size());

    // Density 0: nothing. A change takes effect at once: from 1 a minute to 60, events within
    // seconds; 0 stops the clock and 30 starts it again.
    {
        Run r;
        start(r, patchOf(af::AP_RANDOM, 0.0f), 1);
        r.seconds(600);
        CHECK(r.ev.empty());
        Run s;
        start(s, patchOf(af::AP_RANDOM, 1.0f), 37);
        s.seconds(5);
        const size_t before = s.ev.size();
        s.g.set(patchOf(af::AP_RANDOM, 60.0f), HarmonyPatch{});
        const int64_t at = s.now;
        s.seconds(10);
        CHECK(s.ev.size() > before);
        const double first = s.ev.size() > before ? (s.ev[before].at - at) / kSr : 99.0;
        s.g.set(patchOf(af::AP_RANDOM, 0.0f), HarmonyPatch{});
        const size_t stopped = s.ev.size();
        s.seconds(120);
        CHECK(s.ev.size() == stopped);
        s.g.set(patchOf(af::AP_RANDOM, 30.0f), HarmonyPatch{});
        s.seconds(60);
        CHECK(s.ev.size() > stopped + 10);
        std::printf("  Density 0: none in 10 min; 1 -> 60 a minute: the next event %.2f s on; 0 stops it, 30 starts it again\n",
                    first);
    }
}

// --- 5. the range ---------------------------------------------------------------------------------

void testRange() {
    std::printf("== airgen: the range\n");
    bool inside = true;
    int combos = 0, lowest = 127, highest = 0;
    for (int pat = 0; pat < af::AP_COUNT; ++pat)
        for (int reg = 4; reg <= 6; ++reg)
            for (int key : {0, 5, 11})
                for (float range : {0.5f, 1.25f, 3.0f}) {
                    Run r;
                    AirGenPatch p = patchOf(pat);
                    p.registerOct = reg;
                    p.rangeOct = range;
                    const HarmonyPatch h = keyOf(key, key == 5 ? af::SC_CHROMATIC : af::SC_DORIAN);
                    start(r, p, static_cast<uint32_t>(41 + combos), h, chordOf({60 + key, 63 + key, 67 + key}));
                    r.events(50);
                    if (pat == af::AP_ECHO) {   // the player's notes from far outside the range
                        for (int n : {30, 100, 61, 127, 0}) r.g.played(n, 0.5f, 0);
                    }
                    r.events(100);
                    const int lo = rangeLo(p, h), hi = rangeHi(p, h);
                    for (const Ev& e : r.ev) {
                        const bool ok = e.note >= lo && e.note <= hi && e.note <= 108 &&
                                        (pat == af::AP_ECHO || inScale(h.scale, key, e.note));
                        if (!ok && inside)
                            std::printf("  %s, Register %d, key %d, Range %.2f: note %d outside %d..%d\n", kPat[pat], reg, key,
                                        range, e.note, lo, hi);
                        inside = inside && ok;
                        lowest = std::min(lowest, e.note);
                        highest = std::max(highest, e.note);
                    }
                    inside = inside && r.ev.size() >= 100;
                    ++combos;
                }
    CHECK(inside);
    std::printf("  %d combinations of pattern, Register, key and Range: every note within its range (%d..%d in all)\n",
                combos, lowest, highest);

    // Three candidates at least, in every scale, key and Register at Range 0.5 (the header's ground
    // for "never the note before"), and so in each scale at its least, one chord tone at Gravity 1,
    // Random never plays the note before.
    size_t fewest = 99;
    bool never = true;
    for (int scale = 0; scale < af::SC_COUNT; ++scale) {
        for (int key = 0; key < 12; ++key)
            for (int reg = 4; reg <= 6; ++reg) {
                AirGenPatch p;
                p.registerOct = reg;
                p.rangeOct = 0.5f;
                fewest = std::min(fewest, candidates(p, keyOf(key, scale)).size());
            }
        Run r;
        AirGenPatch p = patchOf(af::AP_RANDOM);
        p.gravity = 1.0f;
        p.registerOct = 4;
        p.rangeOct = 0.5f;
        start(r, p, static_cast<uint32_t>(200 + scale), keyOf(0, scale), chordOf({60}));
        r.events(100);
        never = never && r.ev.size() >= 100 && noRepeats(r.ev, 1);
    }
    CHECK(fewest >= 3);
    CHECK(never);
    std::printf("  Range 0.5, every scale, key and Register: %zu candidates at fewest; one chord tone at Gravity 1, never "
                "the note before: %s\n",
                fewest, never ? "yes" : "no");
}

// --- 6. Rise and Fall -----------------------------------------------------------------------------

void testRiseFall() {
    std::printf("== airgen: Rise and Fall\n");
    struct Case {
        int key, scale, reg;
        float range;
    };
    const Case cases[] = {{0, af::SC_MAJOR, 5, 2.0f}, {2, af::SC_DORIAN, 4, 0.5f}, {11, af::SC_CHROMATIC, 6, 3.0f},
                          {0, af::SC_MIN_PENT, 5, 0.5f}};
    for (int pat : {af::AP_RISE, af::AP_FALL}) {
        bool ok = true;
        int wraps = 0, skips = 0;
        for (const Case& c : cases) {
            Run r;
            AirGenPatch p = patchOf(pat);
            p.registerOct = c.reg;
            p.rangeOct = c.range;
            const HarmonyPatch h = keyOf(c.key, c.scale);
            start(r, p, 43, h, chordOf({60 + c.key, 64 + c.key, 67 + c.key}));
            r.events(1000);
            const std::vector<int> cand = candidates(p, h);
            const int n = static_cast<int>(cand.size());
            // The first from the bottom (Rise) or the top (Fall), by weight between the two there.
            const int first = r.ev.empty() ? -1 : indexIn(cand, r.ev[0].note);
            ok = ok && r.ev.size() >= 1000 && (pat == af::AP_RISE ? first <= 1 : first >= n - 2);
            for (size_t i = 1; i < r.ev.size(); ++i) {
                const int a = indexIn(cand, r.ev[i - 1].note), b = indexIn(cand, r.ev[i].note);
                // The next above (below) by one or two candidates, round from the top (bottom).
                const int stepUp = ((pat == af::AP_RISE ? b - a : a - b) + n) % n;
                ok = ok && a >= 0 && b >= 0 && (stepUp == 1 || stepUp == 2);
                const bool wrap = pat == af::AP_RISE ? b < a : b > a;
                wraps += wrap;
                skips += stepUp == 2;
            }
        }
        CHECK(ok);
        CHECK(wraps > 0);
        std::printf("  %s, 4 ranges x 1000: each note the next %s by one candidate or two (%d skipped one), %d wraps: %s\n",
                    pat == af::AP_RISE ? "Rise" : "Fall", pat == af::AP_RISE ? "above" : "below", skips, wraps,
                    ok ? "yes" : "no");
    }
}

// --- 7. Constellation -----------------------------------------------------------------------------

void testConstellation() {
    std::printf("== airgen: Constellation\n");
    // Mutate 0: the motif's notes repeat in order.
    bool repeat = true;
    for (int m = 3; m <= 8; ++m) {
        Run r;
        AirGenPatch p = patchOf(af::AP_CONSTELLATION);
        p.mutate = 0.0f;
        p.motif = m;
        start(r, p, static_cast<uint32_t>(50 + m));
        r.events(500);
        for (size_t i = static_cast<size_t>(m); i < r.ev.size(); ++i) repeat = repeat && r.ev[i].note == r.ev[i - m].note;
        repeat = repeat && r.ev.size() >= 500;
    }
    CHECK(repeat);
    std::printf("  Mutate 0, Motif 3..8: the same notes in the same order, pass after pass: %s\n", repeat ? "yes" : "no");

    // Mutate 1: each pass differs from the one before in exactly one note, by one scale step.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_CONSTELLATION);
        p.mutate = 1.0f;
        start(r, p, 61);
        r.events(1000);
        const std::vector<int> cand = candidates(p, HarmonyPatch{});
        bool one = r.ev.size() >= 1000;
        for (size_t q = 1; one && q < 200; ++q) {
            int diff = 0, step = 0;
            for (size_t i = 0; i < 5; ++i) {
                const int a = r.ev[(q - 1) * 5 + i].note, b = r.ev[q * 5 + i].note;
                if (a != b) {
                    ++diff;
                    step = std::abs(indexIn(cand, a) - indexIn(cand, b));
                }
            }
            one = one && diff == 1 && step == 1;
        }
        CHECK(one);
        std::printf("  Mutate 1: each of 199 passes one note off the pass before, by one scale step: %s\n", one ? "yes" : "no");
    }
    // Mutate 0.3: about 30% of the passes change.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_CONSTELLATION);
        p.mutate = 0.3f;
        start(r, p, 67);
        r.events(1505);
        int changed = 0;
        for (size_t q = 1; r.ev.size() >= 1505 && q < 301; ++q) {
            bool d = false;
            for (size_t i = 0; i < 5; ++i) d = d || r.ev[(q - 1) * 5 + i].note != r.ev[q * 5 + i].note;
            changed += d;
        }
        CHECK(std::abs(changed - 90) <= 30);
        std::printf("  Mutate 0.3: %d of 300 passes changed\n", changed);
    }
    // A Register change moves the whole motif by its octaves, keeping its shape: Register 4 (60..72)
    // to 6 (84..96), each note 24 up, in the same place in the pass.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_CONSTELLATION);
        p.mutate = 0.0f;
        p.registerOct = 4;
        p.rangeOct = 1.0f;
        start(r, p, 73);
        r.events(52);
        p.registerOct = 6;
        r.g.set(p, HarmonyPatch{});
        const size_t from = r.ev.size();
        r.events(from + 15);
        bool shape = from >= 52;
        for (size_t i = from; shape && i < r.ev.size(); ++i)
            shape = r.ev[i].note == r.ev[i - 5].note + (i - 5 < from ? 24 : 0);
        CHECK(shape);
        std::printf("  Register 4 -> 6: the motif 24 up, its shape and place kept: %s\n", shape ? "yes" : "no");
    }
    // A chord change at Gravity 1 moves the motif onto the new chord's tones, still without repeats;
    // a key change at Gravity 0.6 into the new key.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_CONSTELLATION);
        p.gravity = 1.0f;
        p.mutate = 0.5f;
        start(r, p, 71);
        r.events(100);
        r.g.setChord(chordOf({65, 69, 72}), true);   // F
        const size_t from = r.ev.size();
        r.events(300);
        bool onF = true;
        for (size_t i = from; i < r.ev.size(); ++i) onF = onF && (r.ev[i].note % 12 == 5 || r.ev[i].note % 12 == 9 || r.ev[i].note % 12 == 0);
        CHECK(onF);
        CHECK(noRepeats(r.ev, 2, from > 2 ? from - 2 : 0));
        p.gravity = 0.6f;
        r.g.set(p, keyOf(1));   // D-flat major
        const size_t from2 = r.ev.size();
        r.events(500);
        bool inDb = true;
        for (size_t i = from2; i < r.ev.size(); ++i)
            inDb = inDb && inScale(af::SC_MAJOR, 1, r.ev[i].note) && r.ev[i].note >= 73 && r.ev[i].note <= 97;
        CHECK(inDb);
        CHECK(noRepeats(r.ev, 2, from2 > 2 ? from2 - 2 : 0));
        std::printf("  Gravity 1, C -> F: the motif on F's tones %s; C -> D-flat major: in the key and its range %s\n",
                    onF ? "yes" : "no", inDb ? "yes" : "no");
    }
}

// --- 8. Echo --------------------------------------------------------------------------------------

// The last 3k events repeat with period k, and the last k are `want` in some order.
bool echoes(const std::vector<Ev>& ev, std::initializer_list<int> want) {
    const size_t k = want.size();
    if (ev.size() < 3 * k) return false;
    for (size_t i = ev.size() - 2 * k; i < ev.size(); ++i)
        if (ev[i].note != ev[i - k].note) return false;
    std::vector<int> got, w(want);
    for (size_t i = ev.size() - k; i < ev.size(); ++i) got.push_back(ev[i].note);
    std::sort(got.begin(), got.end());
    std::sort(w.begin(), w.end());
    if (got != w) {
        std::printf("  got");
        for (int g : got) std::printf(" %d", g);
        std::printf("\n");
    }
    return got == w;
}

void testEcho() {
    std::printf("== airgen: Echo\n");
    // Before the player gives a note it is the Constellation, event for event.
    {
        Run a, b;
        start(a, patchOf(af::AP_CONSTELLATION, 30.0f), 9);
        start(b, patchOf(af::AP_ECHO, 30.0f), 9);
        a.seconds(300);
        b.seconds(300);
        bool same = a.ev.size() == b.ev.size() && a.ev.size() > 50;
        for (size_t i = 0; same && i < a.ev.size(); ++i) same = a.ev[i].at == b.ev[i].at && a.ev[i].note == b.ev[i].note;
        CHECK(same);
    }
    // 62 64 67 given: from the next pass on the motif is theirs, moved by octaves into the range.
    struct Case {
        int reg;
        std::initializer_list<int> played, want;
        const char* what;
    };
    const Case cases[] = {
        {4, {62, 64, 67}, {62, 64, 67}, "62 64 67, Register 4 (60..84)"},
        {5, {62, 64, 67}, {74, 76, 79}, "62 64 67, Register 5 (72..96)"},
        {6, {62, 64, 67}, {86, 88, 91}, "62 64 67, Register 6 (84..108)"},
        {4, {60, 62, 64, 65, 67, 69, 71, 72}, {65, 67, 69, 71, 72}, "eight notes, Motif 5: the last five"},
        {4, {62, 64, 62, 67}, {62, 64, 67}, "62 64 62 67: the repeat within two dropped"},
        {4, {62, 62, 64, 67}, {62, 64, 67}, "62 62 64 67: the repeat dropped"},
        {4, {30, 100, 61}, {66, 76, 61}, "30 100 61: into the range by octaves"},
    };
    for (const Case& c : cases) {
        Run r;
        AirGenPatch p = patchOf(af::AP_ECHO);
        p.mutate = 0.0f;
        p.registerOct = c.reg;
        start(r, p, 2);
        r.events(7);
        int off = 0;
        for (int n : c.played) r.g.played(n, 0.8f, off++);
        r.events(7 + 6 + 3 * 8);
        const bool ok = echoes(r.ev, c.want);
        CHECK(ok);
        std::printf("  %s: %s\n", c.what, ok ? "yes" : "no");
    }
    // A Register change moves the echo by octaves and keeps the player's notes: 62 64 67 at Register
    // 4, then Register 5: 74 76 79, from the next note on.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_ECHO);
        p.mutate = 0.0f;
        p.registerOct = 4;
        start(r, p, 2);
        r.events(7);
        int off = 0;
        for (int n : {62, 64, 67}) r.g.played(n, 0.8f, off++);
        r.events(7 + 6 + 3 * 8);
        const bool before = echoes(r.ev, {62, 64, 67});
        p.registerOct = 5;
        r.g.set(p, HarmonyPatch{});
        const size_t from = r.ev.size();
        r.events(from + 3 * 4);
        bool after = echoes(r.ev, {74, 76, 79});
        for (size_t i = from; i < r.ev.size(); ++i) after = after && (r.ev[i].note == 74 || r.ev[i].note == 76 || r.ev[i].note == 79);
        CHECK(before && after);
        std::printf("  62 64 67 at Register 4, then Register 5: 74 76 79 from the next note: %s\n", after ? "yes" : "no");
    }
    // Two notes can't make a motif without repeats: still the Constellation, event for event.
    {
        Run a, b;
        start(a, patchOf(af::AP_CONSTELLATION, 30.0f), 21);
        start(b, patchOf(af::AP_ECHO, 30.0f), 21);
        a.seconds(30);
        b.seconds(30);
        b.g.played(62, 0.8f, 0);
        b.g.played(64, 0.8f, 0);
        a.seconds(200);
        b.seconds(200);
        bool same = a.ev.size() == b.ev.size();
        for (size_t i = 0; same && i < a.ev.size(); ++i) same = a.ev[i].note == b.ev[i].note;
        CHECK(same);
        std::printf("  two notes: still the Constellation %s\n", same ? "yes" : "no");
    }
}

// --- 9. Loop --------------------------------------------------------------------------------------

// The gaps of each recorded event to its neighbours, the shorter, cyclically over a pass of `pass`.
std::vector<double> gapsOf(const std::vector<Ev>& rec, int64_t pass) {
    std::vector<double> g(rec.size(), static_cast<double>(pass));
    const size_t n = rec.size();
    if (n < 2) return g;
    for (size_t i = 0; i < n; ++i) {
        const double prev = i > 0 ? rec[i].at - rec[i - 1].at : rec[0].at + pass - rec[n - 1].at;
        const double next = i + 1 < n ? rec[i + 1].at - rec[i].at : rec[0].at + pass - rec[n - 1].at;
        g[i] = std::min(prev, next);
    }
    return g;
}

// The events from `first` on, a first pass of `pass` samples from `on`, then replays: each replay pass
// the same notes in the same order, each at its recorded time + q passes within rubato x 0.1 x its gap
// (and a sample or two). A generated note that would repeat either of the two before it (where the
// loop comes round) is moved: any other note but the one before will do here. Returns how many replay
// passes matched; sets `rec` to the first pass.
int replays(const std::vector<Ev>& ev, size_t first, int64_t on, int64_t pass, double rubato, std::vector<Ev>& rec,
            double* worst = nullptr, bool* moved = nullptr) {
    rec.clear();
    size_t i = first;
    while (i < ev.size() && ev[i].at < on + pass) rec.push_back(ev[i++]);
    if (rec.empty()) return 0;
    const std::vector<double> gap = gapsOf(rec, pass);
    int good = 0;
    for (int q = 1;; ++q) {
        if (i + rec.size() > ev.size()) return good;
        for (size_t k = 0; k < rec.size(); ++k, ++i) {
            const double dev = std::fabs(static_cast<double>(ev[i].at - (rec[k].at + q * pass)));
            const int prev = ev[i - 1].note, prev2 = i >= 2 ? ev[i - 2].note : -1;
            const bool seam = !rec[k].played && ((rec.size() > 1 && rec[k].note == prev) || (rec.size() > 2 && rec[k].note == prev2));
            if ((seam ? ev[i].note == prev : ev[i].note != rec[k].note) || dev > rubato * 0.1 * gap[k] + 2.0) {
                std::printf("  pass %d, event %zu: note %d at %lld, recorded %d at %lld (+%lld; %.0f samples off, gap %.0f)\n", q,
                            k, ev[i].note, static_cast<long long>(ev[i].at), rec[k].note, static_cast<long long>(rec[k].at),
                            static_cast<long long>(q * pass), dev, gap[k]);
                return good;
            }
            if (worst) *worst = std::max(*worst, (dev - 1.0) / gap[k]);
            if (moved && dev > 2.0) *moved = true;
        }
        ++good;
    }
}

void testLoop() {
    std::printf("== airgen: Loop\n");
    const int64_t pass = static_cast<int64_t>(10 * kSr);
    // Free, 10 s at 30 a minute, turned on while playing: the first pass recorded, then replayed.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_CONSTELLATION, 30.0f);
        p.loopS = 10.0f;
        p.rubato = 0.2f;
        start(r, p, 3);
        r.seconds(5.3);
        p.loop = true;
        r.g.set(p, HarmonyPatch{});
        const int64_t on = r.now;
        const size_t first = r.ev.size();
        r.seconds(65);
        std::vector<Ev> rec;
        double worst = 0.0;
        const int good = replays(r.ev, first, on, pass, 0.2, rec, &worst);
        const size_t after = r.ev.size() - first;
        bool vel = true, newVel = false;
        for (size_t i = first + rec.size(); !rec.empty() && i < r.ev.size(); ++i) {
            vel = vel && r.ev[i].vel >= 0.7f * 0.9f - 1e-6f && r.ev[i].vel <= 0.7f;
            newVel = newVel || r.ev[i].vel != rec[(i - first) % rec.size()].vel;
        }
        CHECK(rec.size() >= 2);
        CHECK(good >= 5);
        CHECK(after >= 6 * rec.size() && after <= 7 * rec.size());   // nothing generated besides
        CHECK(vel && newVel);
        std::printf("  free, 10 s at 30 a minute: %zu events recorded, %d passes replayed them (moved at most %.3f of "
                    "their gap; Rubato 0.2 allows 0.02), new velocities: %s\n",
                    rec.size(), good, worst, newVel ? "yes" : "no");

        bool flags = true;   // replays of generated notes aren't the player's
        for (const Ev& e : r.ev) flags = flags && !e.played;
        CHECK(flags);

        // Off: the recording is forgotten and generation goes on.
        p.loop = false;
        r.g.set(p, HarmonyPatch{});
        const size_t was = r.ev.size();
        r.seconds(60);
        CHECK(r.ev.size() > was + 15);
        std::printf("  off: %zu events generated in the next minute\n", r.ev.size() - was);
    }
    // A loop doesn't shift the sequence: its replays draw nothing from the clock's numbers. A run with
    // Loop on from 60 s to 120 s plays, from 130 s on, at the very samples and velocities of a run
    // without it (the notes may differ: the last two notes and the motif's place aren't the same).
    {
        Run a, b;
        AirGenPatch p = patchOf(af::AP_CONSTELLATION, 30.0f);
        p.loopS = 8.0f;
        start(a, p, 9);
        start(b, p, 9);
        a.seconds(300);
        b.seconds(60);
        p.loop = true;
        b.g.set(p, HarmonyPatch{});
        b.seconds(60);
        p.loop = false;
        b.g.set(p, HarmonyPatch{});
        b.seconds(180);
        const int64_t from = static_cast<int64_t>(130 * kSr);
        std::vector<Ev> ea, eb;
        for (const Ev& e : a.ev)
            if (e.at >= from) ea.push_back(e);
        for (const Ev& e : b.ev)
            if (e.at >= from) eb.push_back(e);
        bool same = ea.size() == eb.size() && ea.size() > 50;
        for (size_t i = 0; same && i < ea.size(); ++i) same = ea[i].at == eb[i].at && ea[i].vel == eb[i].vel;
        CHECK(same);
        std::printf("  Loop on from 60 s to 120 s: from 130 s the same %zu events as without it (samples, velocities): %s\n",
                    ea.size(), same ? "yes" : "no");
    }
    // Turning the loop off, the next generated note repeats neither of the last two replayed: the
    // replays count among the last two notes. Random over 4 candidates (Range 0.5), a 4 s loop on
    // for 10 s and off for 3 s, 40 times.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_RANDOM, 60.0f);
        p.rangeOct = 0.5f;
        p.loopS = 4.0f;
        start(r, p, 71);
        int turns = 0, repeats = 0;
        for (int k = 0; k < 40; ++k) {
            p.loop = true;
            r.g.set(p, HarmonyPatch{});
            r.seconds(10);
            const size_t last = r.ev.size();
            p.loop = false;
            r.g.set(p, HarmonyPatch{});
            r.seconds(3);
            if (last >= 2 && r.ev.size() > last) {
                ++turns;
                repeats += r.ev[last].note == r.ev[last - 1].note || r.ev[last].note == r.ev[last - 2].note;
            }
        }
        CHECK(turns >= 35 && repeats == 0);
        std::printf("  Loop turned off %d times: the first note after it repeated one of the last two %d times\n", turns,
                    repeats);
    }
    // Rubato 1: the times move, by less than a tenth of their gap, never out of order.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_RANDOM, 30.0f);
        p.loopS = 10.0f;
        p.rubato = 1.0f;
        p.loop = true;
        start(r, p, 47);
        r.seconds(5 + 70);
        std::vector<Ev> rec;
        double worst = 0.0;
        bool moved = false;
        const int64_t on = r.ev.empty() ? 0 : r.ev[0].at;   // the first pass starts at the first event
        const int good = replays(r.ev, 0, on, pass, 1.0, rec, &worst, &moved);
        CHECK(good >= 5 && moved);
        std::printf("  Rubato 1: %d passes, the notes in order, moved up to %.3f of their gap (0.1 allowed)\n", good, worst);
    }
    // After reset() the first pass starts at the first event: Rubato 0, it comes back exactly 10 s on.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_RANDOM, 30.0f);
        p.loopS = 10.0f;
        p.rubato = 0.0f;
        p.loop = true;
        start(r, p, 53);
        r.seconds(40);
        std::vector<Ev> rec;
        const int good = r.ev.empty() ? 0 : replays(r.ev, 0, r.ev[0].at, pass, 0.0, rec);
        bool exact = good >= 2 && r.ev.size() >= 3 * rec.size();
        for (size_t k = 0; exact && k < rec.size(); ++k) exact = std::llabs(r.ev[rec.size() + k].at - rec[k].at - pass) <= 1;
        CHECK(exact);
        std::printf("  from reset(): the first event at %.2f s comes back at %.2f s\n", exact ? r.ev[0].at / kSr : -1.0,
                    exact ? r.ev[rec.size()].at / kSr : -1.0);
    }
    // Synced, 120 BPM, 4 beats (2 s), MPC playing from beat 1.5: turned on mid-bar, the first pass
    // records 2 s from there, and every replay is 2 s (88200 samples) on, so each event keeps its
    // place in the bar.
    {
        Run r;
        r.synced = true;
        r.beats = 1.5;
        AirGenPatch p = patchOf(af::AP_RANDOM, 60.0f);
        p.loopBeats = 4.0f;
        p.rubato = 0.0f;
        start(r, p, 5);
        r.seconds(3.1);
        p.loop = true;
        r.g.set(p, HarmonyPatch{});
        const int64_t on = r.now;
        const size_t first = r.ev.size();
        r.seconds(12);
        std::vector<Ev> rec;
        const int good = replays(r.ev, first, on, 88200, 0.0, rec);
        CHECK(rec.size() >= 1 && good >= 4);
        std::printf("  synced, 4 beats at 120 BPM, on at beat %.2f: %zu recorded, replayed %d times 2 s on, in their "
                    "places in the bar\n",
                    1.5 + on * 2.0 / kSr, rec.size(), good);
    }
    // The player's notes on beats 12 and 13 (Notes mode, the loop turned on at beat 9.3): back on 16
    // and 17, 20 and 21, ...: on the bar. MPC locates from beat 30 to 50.5: the next ones on 52 and
    // 53. Then 90 BPM (from beat 58): on 60 and 61, at the new tempo.
    {
        Run r;
        r.synced = true;
        r.beats = 0.0;
        AirGenPatch p = patchOf(af::AP_RANDOM, 30.0f);
        p.loopBeats = 4.0f;
        p.rubato = 0.0f;
        start(r, p, 7, HarmonyPatch{}, triadC(), false);
        while (r.beats + kStep * 2.0 / kSr <= 9.3) r.step();
        p.loop = true;
        r.g.set(p, HarmonyPatch{});
        r.playOnBeat(12.0, 72, 0.8f);
        r.playOnBeat(13.0, 76, 0.6f);
        while (r.beats < 30.0) r.step();
        r.beats = 50.5;   // located
        const size_t located = r.ev.size();
        while (r.beats < 58.0) r.step();
        r.bpm = 90.0;
        const size_t slower = r.ev.size();
        while (r.beats < 71.5) r.step();
        // 16 17 20 21 24 25 28 29, 52 53 56 57, 60 61 64 65 68 69: each on its beat of the bar.
        bool onBar = r.ev.size() == 2 * 4 + 2 * 2 + 2 * 3;
        for (size_t i = 0; onBar && i < r.ev.size(); ++i) {
            const double b = r.evBeats[i], within = b - 4.0 * std::floor(b / 4.0 + 0.25);
            onBar = std::fabs(within - (r.ev[i].note == 72 ? 0.0 : 1.0)) <= 1.5 * r.bpm / 60.0 / kSr &&
                    r.ev[i].vel == (r.ev[i].note == 72 ? 0.8f : 0.6f);
        }
        const bool afterLocate = r.ev.size() > located && std::fabs(r.evBeats[located] - 52.0) < 1e-3;
        const bool atNinety = r.ev.size() > slower && std::fabs(r.evBeats[slower] - 60.0) < 1e-3;
        CHECK(onBar);
        CHECK(afterLocate && atNinety);
        std::printf("  the player's notes on beats 12 and 13: %zu replays, all on their beat of the bar %s; after a "
                    "locate to 50.5 the next on beat %.3f; at 90 BPM on beat %.3f\n",
                    r.ev.size(), onBar ? "yes" : "no", r.ev.size() > located ? r.evBeats[located] : -1.0,
                    r.ev.size() > slower ? r.evBeats[slower] : -1.0);
    }
    // Synced, 128 beats at 60 BPM is 128 s: halved to 64 s. Notes mode: only the player's note, its
    // own velocity.
    {
        Run r;
        r.synced = true;
        r.bpm = 60.0;
        AirGenPatch p = patchOf(af::AP_RANDOM, 30.0f);
        p.loopBeats = 128.0f;
        p.rubato = 0.0f;
        p.loop = true;
        start(r, p, 59, HarmonyPatch{}, triadC(), false);
        const int64_t t = static_cast<int64_t>(5 * kSr) + 17;
        r.playAt(t, 70, 0.9f);
        r.seconds(140);
        bool ok = r.ev.size() == 2;
        for (size_t k = 0; ok && k < 2; ++k)
            ok = r.ev[k].note == 70 && r.ev[k].vel == 0.9f && std::llabs(r.ev[k].at - t - static_cast<int64_t>(64 * kSr) * (k + 1)) <= 1;
        CHECK(ok);
        std::printf("  128 beats at 60 BPM: halved to 64 s, the player's note back at %.3f s and %.3f s: %s\n",
                    r.ev.empty() ? 0.0 : r.ev[0].at / kSr, r.ev.size() < 2 ? 0.0 : r.ev[1].at / kSr, ok ? "yes" : "no");
    }
    // Notes mode, free: a first pass with nothing in it isn't replayed; recording starts again at the
    // next note the player gives. A chord of six on one sample, then replayed with room for two events
    // a step: the rest wait for the next steps, in order.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_RANDOM, 30.0f);
        p.loopS = 10.0f;
        p.rubato = 0.5f;
        start(r, p, 61, HarmonyPatch{}, triadC(), false);
        r.seconds(1);
        p.loop = true;
        r.g.set(p, HarmonyPatch{});
        r.seconds(12);   // the first pass ends empty
        const int64_t t0 = r.now + 40;
        r.playAt(t0, 64, 0.9f);
        r.playAt(t0 + static_cast<int64_t>(2 * kSr), 67, 0.5f);
        r.seconds(25);
        bool ok = r.ev.size() == 4;
        const int notes[4] = {64, 67, 64, 67};
        const double secs[4] = {10, 12, 20, 22};
        for (size_t k = 0; ok && k < 4; ++k)
            ok = r.ev[k].note == notes[k] && r.ev[k].vel == (notes[k] == 64 ? 0.9f : 0.5f) &&
                 std::fabs((r.ev[k].at - t0) / kSr - secs[k]) <= 0.5 * 0.1 * 2.0 + 1e-4;
        CHECK(ok);
        std::printf("  Notes, an empty first pass: recorded again from the next note, both back at +10 s and +20 s: %s\n",
                    ok ? "yes" : "no");

        // Overdubs: a note given halfway through the second pass comes back from the third on, not in
        // its own pass; one given on the very sample a recorded note replays comes back with it.
        Run o;
        p.rubato = 0.0f;
        p.loop = true;
        start(o, p, 63, HarmonyPatch{}, triadC(), false);
        const int64_t a = static_cast<int64_t>(3 * kSr) + 5;
        o.playAt(a, 64, 0.9f);
        o.playAt(a + pass + pass / 2, 71, 0.6f);
        o.playAt(a + 2 * pass, 76, 0.4f);
        while (o.now < a + 4 * pass + 3 * pass / 4) o.step();
        const struct {
            int64_t at;
            int note;
        } want[] = {{a + pass, 64},         {a + 2 * pass, 64},     {a + 2 * pass + pass / 2, 71}, {a + 3 * pass, 64},
                    {a + 3 * pass, 76},     {a + 3 * pass + pass / 2, 71}, {a + 4 * pass, 64}, {a + 4 * pass, 76},
                    {a + 4 * pass + pass / 2, 71}};
        bool dubs = o.ev.size() == sizeof want / sizeof want[0];
        for (size_t k = 0; dubs && k < o.ev.size(); ++k) dubs = o.ev[k].note == want[k].note && std::llabs(o.ev[k].at - want[k].at) <= 1;
        if (!dubs)
            for (const Ev& e : o.ev) std::printf("  %d at %+.4f passes\n", e.note, static_cast<double>(e.at - a) / pass);
        CHECK(dubs);
        bool flags = !o.ev.empty();   // all the player's: played, at their own velocities
        for (const Ev& e : o.ev) flags = flags && e.played && e.vel == (e.note == 64 ? 0.9f : e.note == 71 ? 0.6f : 0.4f);
        CHECK(flags);
        std::printf("  overdubs: from the pass after their own, one on a replayed note's sample with it: %s\n",
                    dubs ? "yes" : "no");

        // The recording keeps 64 events: 70 given in the first pass, the first 64 come back, each pass;
        // an overdub into the full loop isn't kept. The player's notes outside 24..108 come back moved
        // by octaves into it (117 as 105, 10 as 34).
        {
            Run f;
            start(f, p, 67, HarmonyPatch{}, triadC(), false);
            const int64_t f0 = static_cast<int64_t>(kSr);
            for (int k = 0; k < 70; ++k) {
                const int note = k == 0 ? 117 : k == 1 ? 10 : 40 + k;
                f.playAt(f0 + k * 4410, note, 0.5f);
            }
            f.playAt(f0 + pass + pass / 2, 50, 0.5f);   // the loop is full
            while (f.now < f0 + 4 * pass - 1000) f.step();   // three replays, the last 6.3 s long
            bool cap = f.ev.size() == 3 * 64;
            for (size_t k = 0; cap && k < f.ev.size(); ++k) {
                const int i = static_cast<int>(k % 64);
                const int want = i == 0 ? 105 : i == 1 ? 34 : 40 + i;
                cap = f.ev[k].note == want && f.ev[k].played && std::llabs(f.ev[k].at - (f0 + i * 4410 + pass * static_cast<int64_t>(k / 64 + 1))) <= 1;
            }
            CHECK(cap);
            std::printf("  70 notes in the first pass: the first 64 back each pass, 117 as 105 and 10 as 34, an overdub "
                        "into the full loop left out: %s\n",
                        cap ? "yes" : "no");
        }
        // Off forgets the recording: on again, it records afresh (here the player's next note only).
        {
            Run g;
            start(g, p, 69, HarmonyPatch{}, triadC(), false);
            const int64_t g0 = static_cast<int64_t>(kSr);
            g.playAt(g0, 64, 0.5f);
            while (g.now < g0 + pass + pass / 2) g.step();   // 64 back once
            p.loop = false;
            g.g.set(p, HarmonyPatch{});
            g.seconds(1);
            p.loop = true;
            g.g.set(p, HarmonyPatch{});
            const int64_t g1 = g.now + 1000;
            g.playAt(g1, 67, 0.5f);
            while (g.now < g1 + 2 * pass + pass / 2) g.step();
            const bool afresh = g.ev.size() == 3 && g.ev[0].note == 64 && g.ev[1].note == 67 && g.ev[2].note == 67 &&
                                std::llabs(g.ev[1].at - g1 - pass) <= 1 && std::llabs(g.ev[2].at - g1 - 2 * pass) <= 1;
            CHECK(afresh);
            std::printf("  Loop off and on again: the old recording gone, the new note alone comes back: %s\n",
                        afresh ? "yes" : "no");
        }

        // The end of a pass: a note 50 samples before it, Rubato 1. It moves by at most a tenth of its
        // 50-sample gap to the next pass's first note, so it comes back just before that one, every
        // pass, and the first note just after it.
        Run w;
        p.rubato = 1.0f;
        p.loop = true;
        start(w, p, 65, HarmonyPatch{}, triadC(), false);
        const int64_t b0 = static_cast<int64_t>(2 * kSr) + 7;
        w.playAt(b0, 62, 0.7f);
        w.playAt(b0 + pass - 50, 69, 0.7f);
        while (w.now < b0 + 5 * pass + pass / 2) w.step();
        // 62 at 1, 2, 3, 4, 5 passes on; 69 50 samples before 2, 3, 4, 5.
        bool wraps = w.ev.size() == 9;
        for (size_t k = 0; wraps && k < w.ev.size(); ++k) {
            const int64_t q = static_cast<int64_t>(k / 2) + 1;
            const int64_t want = k % 2 == 0 ? b0 + q * pass : b0 + (q + 1) * pass - 50;
            // A tenth of the 50-sample gap, and a sample of rounding.
            wraps = w.ev[k].note == (k % 2 == 0 ? 62 : 69) && std::llabs(w.ev[k].at - want) <= 6 &&
                    (k == 0 || w.ev[k].at >= w.ev[k - 1].at);
        }
        if (!wraps)
            for (const Ev& e : w.ev) std::printf("  %d at %+.6f passes\n", e.note, static_cast<double>(e.at - b0) / pass);
        CHECK(wraps);
        std::printf("  a note 50 samples before the pass's end, Rubato 1: back each pass within 5 samples, before the "
                    "next pass's first: %s\n",
                    wraps ? "yes" : "no");

        Run c;
        p.rubato = 0.0f;
        p.loop = true;
        start(c, p, 61, HarmonyPatch{}, triadC(), false);
        c.room = 2;
        c.seconds(0.5);
        for (int k = 0; k < 6; ++k) c.g.played(60 + 2 * k, 0.5f, 33);
        const int64_t chord = c.now + 33;
        c.seconds(11);
        bool waits = c.ev.size() == 6;
        for (size_t k = 0; waits && k < 6; ++k)
            waits = c.ev[k].note == 60 + 2 * static_cast<int>(k) &&
                    c.ev[k].at == (k < 2 ? chord + pass : (chord + pass) / kStep * kStep + kStep * static_cast<int64_t>(k / 2));
        CHECK(waits);
        std::printf("  six on one sample, room for two a step: replayed over three steps, in order: %s\n", waits ? "yes" : "no");
    }
    // A key change moves the replayed notes to the nearest candidate (the lower on a tie), from what
    // was recorded: back in the old key, the old notes.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_RANDOM, 30.0f);
        p.loopS = 10.0f;
        p.rubato = 0.0f;
        p.gravity = 0.0f;
        p.loop = true;
        start(r, p, 67);
        r.seconds(25);
        std::vector<Ev> rec;
        if (!r.ev.empty()) replays(r.ev, 0, r.ev[0].at, pass, 0.0, rec);
        CHECK(rec.size() >= 2);
        if (rec.empty()) return;
        // The recorded event a replayed one stands for: the one at its place in the pass.
        auto recorded = [&](const Ev& e) -> const Ev* {
            const int64_t place = (e.at - rec[0].at) % pass;
            for (const Ev& q : rec)
                if (std::llabs(q.at - rec[0].at - place) <= 2) return &q;
            return nullptr;
        };
        const HarmonyPatch minor = keyOf(0, af::SC_MINOR);
        r.g.set(p, minor);
        const std::vector<int> cand = candidates(p, minor);
        size_t from = r.ev.size();
        r.seconds(10);
        bool moved = r.ev.size() - from == rec.size(), changed = false;
        for (size_t k = from; moved && k < r.ev.size(); ++k) {
            const Ev* q = recorded(r.ev[k]);
            moved = q != nullptr;
            if (!q) break;
            const int want = indexIn(cand, q->note) >= 0 ? q->note : nearestOf(cand, q->note);
            moved = r.ev[k].note == want;
            changed = changed || r.ev[k].note != q->note;
        }
        r.g.set(p, HarmonyPatch{});
        from = r.ev.size();
        r.seconds(10);
        bool back = r.ev.size() - from == rec.size();
        for (size_t k = from; back && k < r.ev.size(); ++k) {
            const Ev* q = recorded(r.ev[k]);
            back = q != nullptr && r.ev[k].note == q->note;
        }
        CHECK(moved && changed);
        CHECK(back);
        std::printf("  C major -> C minor: the loop's notes to the nearest of C minor %s; back to major, the old notes %s\n",
                    moved && changed ? "yes" : "no", back ? "yes" : "no");
    }
}

// The recorded event a replayed one at `at` stands for: the one at its place in the pass (Rubato 0);
// rec.size(): none.
size_t recordedAt(const std::vector<Ev>& rec, int64_t pass, int64_t at) {
    const int64_t place = ((at - rec[0].at) % pass + pass) % pass;
    for (size_t k = 0; k < rec.size(); ++k)
        if (std::llabs(rec[k].at - rec[0].at - place) <= 2) return k;
    return rec.size();
}

void testLoopMoves() {
    std::printf("== airgen: a loop through changes, and its seam\n");
    // A loop recorded before a Register or Range change keeps its pitch classes, through the next
    // chord change too, rather than piling up at the range's edge. Random at Gravity 0.6 (every tone
    // of C major allowed, whatever the chord), recorded at Register 4 (60..72). At Register 6 each note
    // plays 24 up, the loop's shape kept, and on F's chord still. At Range 0.5 (84..90) each folds in
    // by octaves (G, A and B have no octave there: the nearest candidate to their nearest octave), and
    // on G's chord the same. A note that then would repeat the one before goes to the nearest that
    // doesn't (the seam's rule, below). Every replayed note is checked against these rules.
    {
        const int64_t pass = static_cast<int64_t>(10 * kSr);
        Run r;
        AirGenPatch p = patchOf(af::AP_RANDOM, 30.0f);
        p.loopS = 10.0f;
        p.rubato = 0.0f;
        p.registerOct = 4;
        p.rangeOct = 1.0f;
        p.loop = true;
        start(r, p, 97);
        r.seconds(25);
        std::vector<Ev> rec;
        if (!r.ev.empty()) replays(r.ev, 0, r.ev[0].at, pass, 0.0, rec);
        CHECK(rec.size() >= 3);
        if (rec.size() < 3) return;
        const HarmonyPatch h;
        bool all = true;
        auto follows = [&](const char* what) {
            const size_t from = r.ev.size();
            r.seconds(10);
            bool ok = r.ev.size() + 1 >= from + rec.size();   // a pass's worth (one may fall just after)
            std::vector<int> notes;
            for (size_t k = from; ok && k < r.ev.size(); ++k) {
                const size_t i = recordedAt(rec, pass, r.ev[k].at);
                ok = i < rec.size();
                if (!ok) break;
                const int next = loopBase(rec[(i + 1) % rec.size()].note, 4, p, h);
                const int next2 = loopBase(rec[(i + 2) % rec.size()].note, 4, p, h);
                const int base = loopBase(rec[i].note, 4, p, h);
                const int want = loopNote(base, r.ev[k - 1].note, r.ev[k - 2].note, next, next2, candidates(p, h));
                ok = r.ev[k].note == want;
                if (!ok) std::printf("  %s: recorded %d, played %d, the rules say %d\n", what, rec[i].note, r.ev[k].note, want);
                notes.push_back(r.ev[k].note);
            }
            all = all && ok;
            std::sort(notes.begin(), notes.end());
            std::printf("  %s:", what);
            for (int n : notes) std::printf(" %d", n);
            std::printf(" (%zu different) %s\n", static_cast<size_t>(std::unique(notes.begin(), notes.end()) - notes.begin()),
                        ok ? "yes" : "NO");
        };
        std::printf("  recorded at Register 4:");
        for (const Ev& e : rec) std::printf(" %d", e.note);
        std::printf("\n");
        p.registerOct = 6;
        r.g.set(p, h);
        follows("Register 6");
        r.g.setChord(chordOf({65, 69, 72}), true);
        follows("Register 6, on F");
        p.rangeOct = 0.5f;
        r.g.set(p, h);
        follows("Range 0.5");
        r.g.setChord(chordOf({67, 71, 74}), true);
        follows("Range 0.5, on G");
        CHECK(all);
    }

    // The seam. Random over three candidates (C minor pentatonic, Register 4, Range 0.5: 60 63 65)
    // can only go round them, a b c a b ...; four notes recorded (then not generating) are a b c a, a
    // loop whose last note is its first. Coming round, the first would repeat the note before it: it
    // goes to the candidate that is neither that one nor the one after it, c (not b, which would make
    // the next one move in turn, and the next, all the way round), so the loop plays c b c a, pass
    // after pass, never the note before (c b c: three candidates can't keep a loop of four apart
    // within two). Loops of one note and of two (a b a b, each note two from itself) replay as they
    // are.
    for (size_t n : {4, 1, 2}) {
        Run r;
        AirGenPatch p = patchOf(af::AP_RANDOM, 60.0f);
        p.registerOct = 4;
        p.rangeOct = 0.5f;
        p.loopS = 20.0f;
        p.rubato = 0.0f;
        p.loop = true;
        start(r, p, 103, keyOf(0, af::SC_MIN_PENT));
        r.events(n);
        r.g.setChord(triadC(), false);
        r.events(5 * n);   // four passes replayed
        bool ok = r.ev.size() == 5 * n;
        if (ok && n == 4) {
            const int a = r.ev[0].note, b = r.ev[1].note, c = r.ev[2].note, x = 60 + 63 + 65 - a - b;
            ok = r.ev[3].note == a && a != b && b != c && c != a;   // a b c a
            for (size_t k = 4; ok && k < r.ev.size(); ++k) ok = r.ev[k].note == (k % 4 == 0 ? x : r.ev[k % 4].note);
            ok = ok && noRepeats(r.ev, 1);
        } else if (ok) {
            for (size_t k = 0; k < r.ev.size(); ++k) ok = ok && r.ev[k].note == r.ev[k % n].note;
            ok = ok && (n == 1 || r.ev[0].note != r.ev[1].note);
        }
        if (!ok)
            for (const Ev& e : r.ev) std::printf("  %d at %.2f s\n", e.note, e.at / kSr);
        CHECK(ok);
        if (n == 4)
            std::printf("  a loop of %d %d %d %d: replayed as %d %d %d %d, four passes, never the note before: %s\n",
                        r.ev[0].note, r.ev[1].note, r.ev[2].note, r.ev[3].note, r.ev.size() > 4 ? r.ev[4].note : -1,
                        r.ev.size() > 5 ? r.ev[5].note : -1, r.ev.size() > 6 ? r.ev[6].note : -1,
                        r.ev.size() > 7 ? r.ev[7].note : -1, ok ? "yes" : "no");
        else
            std::printf("  a loop of %s: the same each pass: %s\n", n == 1 ? "one note" : "two notes", ok ? "yes" : "no");
    }

    // The seam within two: a b c a b, the loop coming round, would replay a b | a b. A Constellation of
    // three (Mutate 0) over C major's eight candidates in Register 4, Range 1, five notes recorded
    // (then not generating): coming round, a and then b would each repeat the note two before; each
    // goes to the nearest candidate that is none of the two before it and the two after, so the loop
    // plays x y c a b, the same x y every pass, and no note repeats either of the two before it.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_CONSTELLATION, 60.0f);
        p.registerOct = 4;
        p.rangeOct = 1.0f;
        p.motif = 3;
        p.mutate = 0.0f;
        p.loopS = 20.0f;
        p.rubato = 0.0f;
        p.loop = true;
        start(r, p, 109);
        r.events(5);
        r.g.setChord(triadC(), false);
        r.events(25);   // four passes replayed
        bool ok = r.ev.size() == 25;
        if (ok) {
            const int a = r.ev[0].note, b = r.ev[1].note, c = r.ev[2].note;
            ok = r.ev[3].note == a && r.ev[4].note == b && a != b && b != c && c != a;   // a b c a b
            const int x = r.ev[5].note, y = r.ev[6].note;
            ok = ok && x != a && y != b;
            for (size_t k = 5; ok && k < r.ev.size(); ++k) {
                const int want[5] = {x, y, c, a, b};
                ok = r.ev[k].note == want[k % 5];
            }
            ok = ok && noRepeats(r.ev, 2);
        }
        if (!ok)
            for (const Ev& e : r.ev) std::printf("  %d at %.2f s\n", e.note, e.at / kSr);
        CHECK(ok);
        std::printf("  a loop of %d %d %d %d %d: replayed as %d %d %d %d %d, four passes, no repeat within two: %s\n",
                    r.ev[0].note, r.ev[1].note, r.ev[2].note, r.ev[3].note, r.ev[4].note, r.ev.size() > 5 ? r.ev[5].note : -1,
                    r.ev.size() > 6 ? r.ev[6].note : -1, r.ev.size() > 7 ? r.ev[7].note : -1,
                    r.ev.size() > 8 ? r.ev[8].note : -1, r.ev.size() > 9 ? r.ev[9].note : -1, ok ? "yes" : "no");
    }
}

// --- 10. Notes off; odd input ---------------------------------------------------------------------

void testNotesOff() {
    std::printf("== airgen: not generating\n");
    Run r;
    start(r, patchOf(af::AP_RANDOM), 3, HarmonyPatch{}, triadC(), false);
    r.seconds(300);
    r.g.played(64, 0.8f, 0);   // the player's notes aren't step()'s
    r.g.played(67, 0.8f, 3);
    r.seconds(300);
    CHECK(r.ev.empty());
    std::printf("  setChord(c, false), no loop, 10 min at 60 a minute: %zu events\n", r.ev.size());
}

void testOdd() {
    std::printf("== airgen: odd input\n");
    const float nan = std::nanf(""), inf = INFINITY;
    bool ok = true;
    for (int k = 0; k < 6; ++k) {
        Run r;
        AirGenPatch p;
        p.density = k == 0 ? nan : k == 1 ? inf : k == 2 ? -5.0f : 1e9f;
        p.pattern = k == 3 ? 99 : -3 + k;
        p.registerOct = k == 4 ? -10 : 40;
        p.rangeOct = k == 5 ? nan : k * 10.0f - 20.0f;
        p.gravity = k % 2 ? nan : 7.0f;
        p.motif = k % 2 ? -4 : 100;
        p.mutate = k % 2 ? inf : nan;
        p.loop = true;
        p.loopS = k % 3 ? nan : 1e9f;
        p.loopBeats = k % 2 ? -5.0f : (k == 4 ? nan : 1e9f);
        p.rubato = k % 2 ? inf : -1.0f;
        HarmonyPatch h;
        h.key = k * 37 - 50;
        h.scale = k * 9 - 20;
        r.synced = k % 2 == 0;
        r.bpm = k == 2 ? nan : k == 4 ? 1e9 : 0.0;
        start(r, p, 0u, h, chordOf({60, 64, 67}), true);
        for (int i = 0; i < 20; ++i) {
            r.g.played(i * 31 - 100, i % 2 ? nan : 5.0f, i * 1000 - 9000);
            r.seconds(6);
            AirEvent out[4];
            ok = ok && r.g.step(0, out, 4) == 0 && r.g.step(64, out, 0) == 0 && r.g.step(-5, out, 4) == 0;
        }
        for (const Ev& e : r.ev) {   // (the player's notes replay moved by octaves into 24..108)
            const bool sane = e.note >= 24 && e.note <= 108 && e.vel >= 0.0f && e.vel <= 1.0f && std::isfinite(e.vel);
            if (!sane && ok) std::printf("  case %d: note %d, vel %g at %lld\n", k, e.note, e.vel, static_cast<long long>(e.at));
            ok = ok && sane;
        }
    }
    CHECK(ok);
    std::printf("  NaN, infinite and out-of-range settings, odd notes and offsets, n 0, max 0: events sane: %s\n",
                ok ? "yes" : "no");
}

// --- 11. a knob turning ---------------------------------------------------------------------------

void testSweeps() {
    std::printf("== airgen: a knob turning\n");
    // Air calls set() every block. Range turned from 0.5 to 3 over 10 000 blocks (29 s): the
    // candidates are worked out again only where the range's top crosses a semitone, 78 to 108, 30
    // times, not every block; the notes stay in the range as it grows.
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_CONSTELLATION);
        p.rangeOct = 0.5f;
        start(r, p, 101);
        const uint64_t before = r.g.rebuilds();
        bool inside = true;
        for (int b = 0; b <= 10000; ++b) {
            p.rangeOct = 0.5f + 2.5f * static_cast<float>(b) / 10000.0f;
            r.g.set(p, HarmonyPatch{});
            const size_t k = r.ev.size();
            r.step();
            for (size_t i = k; i < r.ev.size(); ++i)
                inside = inside && r.ev[i].note >= 72 && r.ev[i].note <= rangeHi(p, HarmonyPatch{});
        }
        const uint64_t n = r.g.rebuilds() - before;
        CHECK(n == 30);
        CHECK(inside && r.ev.size() >= 15);
        std::printf("  Range 0.5 -> 3 over 10 000 blocks: the candidates worked out %llu times; %zu notes, all in the "
                    "range: %s\n",
                    static_cast<unsigned long long>(n), r.ev.size(), inside ? "yes" : "no");
    }
    // Gravity turned from 0 to 1 over 5000 blocks, held, then (Random) back to 0: never. Its weights
    // are read as notes are drawn, and what is allowed changes only where it crosses 1: there the
    // motif's notes off the chord move onto it, so every note after is a chord tone; back at 0 the
    // other tones come again, at their weight's share (7 chord tones of 15 candidates).
    {
        Run r;
        AirGenPatch p = patchOf(af::AP_CONSTELLATION);
        p.gravity = 0.0f;
        p.mutate = 0.0f;
        start(r, p, 107);
        r.seconds(10);
        const uint64_t before = r.g.rebuilds();
        auto isChord = [](int note) { return note % 12 == 0 || note % 12 == 4 || note % 12 == 7; };
        bool offBefore = false;
        for (const Ev& e : r.ev) offBefore = offBefore || !isChord(e.note);
        for (int b = 0; b <= 5000; ++b) {
            p.gravity = static_cast<float>(b) / 5000.0f;
            r.g.set(p, HarmonyPatch{});
            r.step();
        }
        const size_t at1 = r.ev.size();
        r.seconds(30);
        bool onChord = r.ev.size() >= at1 + 15;
        for (size_t i = at1; i < r.ev.size(); ++i) onChord = onChord && isChord(r.ev[i].note);
        p.pattern = af::AP_RANDOM;
        for (int b = 5000; b >= 0; --b) {
            p.gravity = static_cast<float>(b) / 5000.0f;
            r.g.set(p, HarmonyPatch{});
            r.step();
        }
        const size_t at0 = r.ev.size();
        r.seconds(300);
        int chord = 0;
        for (size_t i = at0; i < r.ev.size(); ++i) chord += isChord(r.ev[i].note);
        const double share = chord / static_cast<double>(r.ev.size() - at0);
        const uint64_t n = r.g.rebuilds() - before;
        CHECK(n == 0);
        CHECK(offBefore && onChord);
        CHECK(std::fabs(share - 7.0 / 15.0) <= 0.1);
        std::printf("  Gravity 0 -> 1 -> 0 over 10 000 blocks: the candidates worked out %llu times; at 1 only chord tones "
                    "(the motif moved onto them) %s; back at 0, chord tones %.0f%% (7 of 15)\n",
                    static_cast<unsigned long long>(n), onChord ? "yes" : "no", 100 * share);
    }
}

} // namespace

void airgenTests() {
    testSameSeed();
    testGravity();
    testNoRepeats();
    testDensity();
    testRange();
    testRiseFall();
    testConstellation();
    testEcho();
    testLoop();
    testLoopMoves();
    testNotesOff();
    testOdd();
    testSweeps();
}

} // namespace aft
