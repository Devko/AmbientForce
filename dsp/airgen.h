#pragma once
// Air's generator (docs/CONCEPT.md 5.3): when Air plays, and which note. Pure logic, no sound: it
// turns the harmony (the key, the scale, the chord) and Air's settings into events, a note and a
// velocity at a sample, which Air, the stratum, strikes on its voices.
//
// Real-time rules: step() runs on the audio thread, so everything lives in fixed arrays; nothing
// allocates, locks or throws. Time that runs for hours (the clock's work, the loop's position and
// its passes) is double or 64-bit.
//
// WHEN. A Poisson process at Density a minute. Each event draws the work to the next one, an
// exponential amount with mean 1, and the clock uses work up at Density / 60 a second: at a steady
// Density each gap is exponential with mean 60 / Density s, so events never fall on a grid. A
// Density change takes effect at once, on the rest of the gap under way, not from the next gap:
// a turn from 1 to 60 a minute isn't held up by a gap of a minute, and the rest of an exponential
// gap is exponential too, so it stays a Poisson process, at the new rate from the change on.
// Density 0 stops the clock where it is. The clock runs whether or not Air generates: an event that
// finds Air not generating (Notes, a forgotten harmony, a loop replaying, no room in step()'s
// output) is silent, and still makes its draws.
//
// WHICH NOTE. The candidates are the scale's tones (Chromatic: all twelve) from the key's tonic in
// Register's octave, tonic(o) = 12 x (o + 1) + key (C4 = 60), up to Range octaves over it, both
// ends in: tonic .. tonic + floor(12 x Range), never above MIDI 108. A chord tone (its pitch class
// in the chord) weighs 1, any other candidate 1 - Gravity. "Allowed" below means a candidate of
// weight above 0: every candidate while Gravity < 1, the chord tones at 1, and every candidate
// again when none in the range is a chord tone. An As Played chord's tones off the scale are not
// candidates: Air stays in the key. A forgotten harmony (a chord with root -1) keeps the last
// chord's tones.
//
// The last two notes Air played from here (generated or replayed by the loop; not the player's,
// which Air strikes itself) are never chosen. When the weights leave nothing else, the rule gives
// way a step at a time, and every choice is one draw:
//   1. by weight, neither of the last two;
//   2. by weight, not the last one;
//   3. evenly (weights aside), neither of the last two;
//   4. evenly, not the last one;
//   5. evenly, any.
// There are three candidates at least (every scale has three tones in the six semitones over its
// tonic, Range 0.5's span), so step 4 always finds one and a note never repeats the one before it.
// Step 5 is there for a single candidate, where a repeat would be unavoidable; no scale gives one.
// A C chord of one tone, Gravity 1, Range 0.5 in C major (60 62 64 65): 60, then a draw among the
// other three, 60 again, ...: the chord tone every other note.
//
// A note that is moved rather than drawn (a motif's after a change, a loop's replay that would
// repeat) goes to the nearest candidate, the lower on a tie, that keeps the rule against the notes
// around it: for a motif's note its neighbours within two, round the motif, the two next to it
// first; for a replay the last two notes and the loop's next, the note before and that one first.
// It gives way a step at a time too, but the rule comes before Gravity here: a motif or a loop plays
// its notes again pass after pass, so a repeat moved into it would come back every pass.
//   1. allowed, repeating none of them;
//   2. any candidate (of weight 0 too), repeating none of them;
//   3. allowed, repeating neither next to it;
//   4. any candidate, repeating neither next to it;
//   5. the nearest allowed.
// With three candidates step 4 always finds one. So a chord that leaves one allowed tone in the
// range (Gravity 1) doesn't make a motif or a loop that tone throughout: the tone goes where the
// rule lets it, the nearest other candidates around it.
//
// THE PATTERNS:
// - Random: each event a draw as above.
// - Rise: the allowed candidate next above the last note, by weight between the next two allowed;
//   past the top it starts again from the bottom (so the next two of the one under the top are the
//   top and the bottom). Before any note, from the bottom. So at Gravity 1 it walks the chord tones.
//   With fewer than two allowed (a chord of one tone in the range) it walks every candidate, by
//   weight, evenly between two of weight 0, so the lone chord tone doesn't repeat. Fall is the
//   mirror.
// - Constellation: a motif of Motif notes (3..8), drawn while its first pass plays as Random's are,
//   keeping out also the notes within two of it in the motif (round its end), then replayed in
//   order, one note per event. At the end of each pass, with probability Mutate, one of its notes
//   (drawn) moves to its neighbour (up or down, drawn): the next allowed candidate, which is the
//   next scale tone while Gravity < 1 and the next chord tone at 1, so Gravity 1 keeps the motif on
//   the chord (while the chord has the tones in the range to keep the rule). It moves only if that
//   keeps "no repeat within two" round the motif; otherwise the other way, then the notes after it
//   in turn; none can: no change this pass. A Register change moves the whole motif by its octaves,
//   keeping its shape; then a note still outside the range (Range made smaller, or the key moved
//   the tonic) folds in by octaves. A key, scale or chord change, or Gravity crossing 1 (where what
//   is allowed changes), then moves each note no longer allowed; any of these, or a change of the
//   range, moves a note that found no octave in the range, or that now repeats one within two of
//   it. Each such note is moved as above, keeping the rule against its neighbours. A Motif change
//   draws a new motif from the next pass on.
// - Echo: the motif is the last Motif notes the player gave (played()), each moved by octaves into
//   the range (a pitch class with no octave in a range under an octave: the nearest allowed note to
//   its octave nearest the range),
//   a note repeating either of the two kept before it dropped, round the motif's end too (the rule
//   applied to the echo: the plan dropped only consecutive repeats), then replayed, mutated and
//   moved as a Constellation's: so a Register or Range change keeps the player's pitch classes
//   (62 64 67 at Register 4 are 74 76 79 at 5). A new echo takes over at the start of the pass
//   after the player gave a note.
//   Until the player's notes leave three after the drops, Echo plays the Constellation's motif: a
//   figure of one or two notes can't be replayed without repeats.
// Where the last two notes aren't the motif's own (a new echo, a snap, a pattern switched midway, a
// loop gone off), or the motif holds a repeat within two (drawn when the weights left too little),
// its next note may repeat one of them: it is passed over for the next one in the pass that doesn't;
// none does: the pass ends there and the next pass is looked in; still none: the next that isn't
// the last note. None at all: a motif drawn one note at a time between another pattern's notes,
// which its draws can't see, can be one note throughout (60 60 60 in the corner above). Then the
// note is moved as above, against the last two, and the motif stays as it is.
//
// VELOCITY: 0.7 x (1 - 0.5 x Rubato x u), u uniform 0..1. The player's notes keep their own.
//
// LOOP (CONCEPT 5.3: the principle of Music for Airports). On, it records one pass of everything
// Air plays, generated or the player's, at its place in the pass (up to kLoopMax events; more are
// played and not recorded). That first pass starts when Loop is turned on (at the next step), or at
// the first event Air plays after reset(). A pass that recorded nothing is never replayed: the loop
// records again from the next event Air plays (so Notes with no notes yet waits for the first).
// From the end of the first pass the recording replays:
// - each event at its place, moved by up to Rubato x 0.1 x its gap, the gap being the shorter of
//   those to the events before and after it, round the pass (one event alone: a pass). Two
//   neighbours each move less than a tenth of the gap between them, so the order never changes,
//   across the pass's end too. Generated events take a new velocity, as above; the player's keep
//   theirs, and come out marked `played`;
// - nothing is generated (the clock runs on, silent); the player's notes are added to the
//   recording (overdub) and replayed from the next pass on; the replays count among the last two
//   notes, so the first note generated after Loop goes off repeats neither;
// - a generated note replays in the range as it is now: moved by Register's octaves since it was
//   recorded, keeping the loop's shape as a motif does, then, still outside the range (Range made
//   smaller, or the key moved the tonic), folded in by octaves, its pitch class kept (no octave of
//   it in the range: the nearest allowed to its nearest octave). Recorded before a change of key,
//   scale or chord, it then goes to the nearest allowed candidate, the lower on a tie. So a loop
//   recorded before a Register or Range change keeps its pitch classes through a chord change
//   rather than piling up at the range's edge. The player's notes stay where they were played;
//   after a change of key, scale or chord each goes to the nearest note whose pitch class is
//   allowed. Every note moves from what was recorded, so the loop comes back as it was when the
//   harmony and the range do;
// - a generated note that would repeat the note before it (where the recording comes round, its
//   last note then its first, or two notes brought together by a change) is moved as above, the
//   note before and the loop's next kept out first, so that the next doesn't then repeat it in turn,
//   and so on round the loop. A loop of one note replays it as it is: a pass apart, it is the loop's
//   pulse, not a repeat;
// - every replayed note is moved by octaves into 24..108 (harmony.h's chord range, Air's voices'):
//   the player's notes can be anywhere.
// A pass is Loop s long (2..120), free. Synced (loopBeats > 0: 1..256 quarter notes) it is
// loopBeats on the stratum's BeatClock, halved while longer than 120 s at the tempo, and passes
// start where the clock crosses a multiple of it (on the bar while MPC plays); the first pass still
// records one whole pass from where it started, round the multiple, and the replay goes on in
// phase with the clock. The recording keeps places in the pass (fractions), so a change of length
// stretches it. A jump of the clock (MPC located, the division or Free / Sync changed) finds the
// replay's place again; a first pass under way runs its full length on from there. Off: the
// recording is forgotten, and generation goes on.
//
// DETERMINISM: one xorshift, seeded by seed(). Every clock event makes six draws, in this order,
// whether it sounds or not and whatever the pattern uses: the work to the next event, the note, the
// velocity, then the mutation's chance, its note and its direction (used only where a motif's pass
// ends). reset() makes one: the first event's work. A replayed event's two numbers (its move and
// its velocity) aren't draws: they are a hash of the seed, its pass and its place in the
// recording. So no setting (Pattern, Gravity, Register, Range, Motif, Mutate, Rubato, Density, the
// chord, generating or not, the player's notes, Loop) shifts the sequence that follows: the k-th
// clock event after reset() draws the same numbers whatever happens between, and Density only
// moves it in time. (Its note can still differ: the last two notes and a motif's place depend on
// what was played.)
//
// USE (Air, Task 8): setTransport() once a block, before the block's first step(). A note the
// player gives Air is struck by Air and passed on as played(note, vel, 0): the plugin splits its
// blocks at every MIDI event, so the note sounds at the next step()'s first sample.
//
// COST: the ARM instructions of a 128-sample block, built with the device's flags and counted by an
// instruction-counting qemu (the difference of 40 000 and 20 000 blocks, over 20 000). step() alone:
// 133 generating at Density 60 without a loop; 327 at Density 60 with Loop on (16 s, replaying); 384
// replaying a full loop (64 events every 2 s, synced, Rubato 1). With set() every block, as Air
// calls it: 319, 512 and 580. These were counted before the review's fixes, which add the range's
// ends to set() and a fold and a compare to each replayed note (count pending). A Range or Gravity
// knob turning no longer works the candidates out every block: Range only where an end of the range
// crosses a semitone, Gravity never. Air's share of the budget is 32k a block
// (docs/plans/2026-10-07-m2-weather.md): the generator is about 2% of it (count pending). On the
// device: pending.
#include "common.h"
#include "harmony.h"

#include <cstdint>

namespace af {

enum AirPattern : int { AP_RANDOM, AP_RISE, AP_FALL, AP_CONSTELLATION, AP_ECHO, AP_COUNT };
inline constexpr const char* kAirPatternNames[] = {"Random", "Rise", "Fall", "Constellation", "Echo"};
static_assert(sizeof kAirPatternNames / sizeof *kAirPatternNames == AP_COUNT, "a name per pattern");

struct AirGenPatch {
    float density = 12.0f;      // events a minute, 0..60 (a Poisson process)
    int pattern = AP_CONSTELLATION;
    int registerOct = 5;        // the range starts at the key's tonic in this octave: 4, 5, 6 (C4 = 60)
    float rangeOct = 2.0f;      // 0.5..3: how far above it the notes go
    float gravity = 0.6f;       // 0..1: chord tones weigh 1, other scale tones 1 - gravity
    int motif = 5;              // 3..8: a Constellation's (and Echo's) notes
    float mutate = 0.3f;        // 0..1: the chance a pass swaps one note for a scale neighbour
    bool loop = false;
    float loopS = 16.0f;        // 2..120: the loop's length, free
    float loopBeats = 0.0f;     // synced: its length in quarter notes (common.h kBarDivBeats); 0: free
    float rubato = 0.2f;        // 0..1: jitter on velocity, and on a loop's timing
};

struct AirEvent {
    int offset;    // samples into the step()
    int note;      // MIDI, 24..108
    float vel;     // 0..1
    bool played;   // a replay of a note the player gave (Notes, Split): Air keeps its pan by pitch and
                   // its velocity through Vel. Generated events (and their replays): false.
};

class AirGen {
public:
    static constexpr int kLoopMax = 64;     // events a loop keeps
    static constexpr int kMotifMax = 8;     // a motif's notes at most (Motif's top)
    static constexpr int kPlayedMax = 16;   // the player's notes kept for the next step() (more aren't recorded)
    static constexpr int kCandMax = 37;     // candidates at most: Chromatic over 3 octaves, both ends in

    AirGen();
    // The seed of the random numbers, then reset(): the same seed and the same calls give the same
    // events.
    void seed(uint32_t s);
    // Every field clamped to its range (NaN: the low end; loopBeats NaN or <= 0: free). Cheap per
    // block, a knob turning too: the candidates are worked out again only when the key or the scale
    // changes or an end of the range crosses a semitone (Register, Range); Gravity's weights are read
    // as notes are drawn, and it moves the motifs only where it crosses 1.
    void set(const AirGenPatch& p, const HarmonyPatch& h);
    // What it generates from: the chord (its pitch classes and root) and whether to generate at all
    // (Harmony with a chord, or Free: yes; Notes, or a forgotten harmony: no). A loop replays either way.
    void setChord(const Chord& c, bool generate);
    // A note the player gave Air (Notes, or a key above Split): Echo's motif learns it, a loop records it.
    // Air strikes it itself, so step() doesn't return it (a loop's replay of it, later, it does).
    // `offset`: where in the next step() it sounded (0..n-1; later ones count as its last sample).
    void played(int note, float vel, int offset);
    // MPC's tempo and, while it plays (locked), its position at the next step's first sample: a
    // synced loop's clock. Not locked, the clock runs on at the tempo. (The engine hands its strata
    // one running beat count, locked always: dsp/engine.h.)
    void setTransport(double bpm, double beats, bool locked) { clock_.set(bpm, locked ? beats : clock_.beats); }
    // Back to the start: the seed's numbers again (the first event's work drawn), no notes played,
    // no motif, Echo's notes and the loop's recording forgotten, the loop (if on) waiting for the
    // first event. The patch, the chord and the clock stay.
    void reset();
    // Moves on by n samples (n <= 128; any n works; n <= 0: nothing); writes the events due in them,
    // in order, at most `max`; returns how many. The same seed and the same calls give the same
    // events. A replayed event that finds no room plays at the start of the next step; a generated
    // one is silent.
    int step(int n, AirEvent* out, int max);

    // For the tests: how many times the candidates have been worked out (a knob turning shouldn't
    // do it every block).
    uint64_t rebuilds() const { return rebuilds_; }

private:
    enum LoopState : int { LS_OFF, LS_ARMED, LS_RECORDING, LS_PLAYING };
    struct Recorded {
        double place;      // in the pass, 0..1
        int note;          // as it sounded when recorded
        float vel;         // the player's (generated ones draw theirs again)
        bool played;       // the player's note
        uint32_t harmony;  // harmony_ when it was recorded
        int reg;           // Register when it was recorded (a generated note follows it by octaves)
        int64_t from;      // the first pass that replays it (an overdub isn't replayed in its own pass)
    };
    struct Played {
        int note;
        float vel;
        int offset;
    };

    double draw();                                     // uniform 0..1 (never 0 or 1)
    void rebuildCandidates();
    // Gravity is read here as notes are drawn, not stored in the candidates, so turning it costs
    // nothing until it crosses 1 (where what is allowed changes).
    bool chordTone(int i) const { return chord_ >> i & 1u; }
    float weight(int i) const { return chordTone(i) ? 1.0f : 1.0f - p_.gravity; }
    bool noneAllowed() const { return !(p_.gravity < 1.0f) && chord_ == 0; }   // no weight above 0: all allowed
    bool allowed(int i) const { return p_.gravity < 1.0f || chord_ == 0 || chordTone(i); }
    int indexOf(int note) const;                       // the candidate's index, or -1
    int fold(int note) const;                          // into the range by octaves (the header's Echo)
    // The candidate nearest `note` that keeps the rule against `keep` as far as it can (the header's
    // list for moved notes); keep[0..nNear) are the notes next to it.
    int nearestAllowed(int note, const int* keep, int nNear, int nKeep) const;
    int pick(const int* idx, int n, const int* ex, int nEx, double u) const;
    int choose(double uPick, double uMut, double uWhich, double uDir);   // the generated note
    void startPass();
    void buildEcho();
    void mutate(int* m, int len, double uMut, double uWhich, double uDir);
    void moveMotifs(int shift, bool snap);
    void remember(int note);                           // the last two notes played
    // The loop.
    double passBeats() const;
    void beginRecording(double pos);
    int record(double pos, int note, float vel, bool played, int64_t from);
    void seekReplay(int64_t pass, double place);
    double replayNumber(int which) const;              // the next replayed event's numbers (0: move, 1: velocity)
    double replayAt() const;                           // where the next replayed event is due, in passes
    int replayBase(const Recorded& r) const;           // as the range and the harmony have it now
    int replayNote(int i) const;                       // rec_[i] as it replays
    void nextReplay();

    uint32_t seed_ = 1, rng_ = 1;
    AirGenPatch p_;
    int key_ = 0, scale_ = SC_MAJOR;
    uint16_t pcs_ = 0;            // the chord's pitch classes
    bool generate_ = false;
    uint32_t harmony_ = 0;        // +1 at every change of key, scale or chord

    uint16_t scalePcs_ = 0;       // the scale's pitch classes (in the key)
    int lo_ = 72, hi_ = 96;       // the range, both ends in
    int cand_[kCandMax] = {};     // the candidates, ascending
    uint64_t chord_ = 0;          // bit i: candidate i is a chord tone
    int nCand_ = 0;
    uint64_t rebuilds_ = 0;
    int last_ = -1, last2_ = -1;  // the last two notes generated or replayed (-1: none)

    int con_[kMotifMax] = {};     // the Constellation's motif: conN_ of conLen_ notes drawn so far
    int conN_ = 0, conLen_ = 0;
    int echo_[kMotifMax] = {};    // Echo's motif (echoLen_ 0: none, the Constellation's plays)
    int echoLen_ = 0;
    int heard_[kMotifMax] = {};   // the player's last notes, oldest first
    int nHeard_ = 0;
    bool heardNew_ = false;       // notes since Echo's motif was built
    int echoMotif_ = 0;           // the Motif setting Echo's motif was built for
    bool useEcho_ = false;        // this pass plays Echo's motif
    int mpos_ = 0;                // the next note of the pass

    double work_ = 1.0;           // the clock's work left to the next event

    BeatClock clock_;
    int loop_ = LS_OFF;
    bool startLoop_ = false;      // turned on: the first pass starts at the next step
    Recorded rec_[kLoopMax] = {};
    int nRec_ = 0;
    double pos_ = 0.0;            // the loop's position, in passes, at the next step's start
    double recStart_ = 0.0;       // where the first pass started (it ends a pass later)
    int64_t repPass_ = 0;         // the next replayed event: pass and index
    int repIdx_ = 0;
    Played heardNow_[kPlayedMax] = {};      // the player's notes for the next step()
    int nHeardNow_ = 0;
};

} // namespace af
