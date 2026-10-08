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
// The last two notes the generator played (its own: not the player's, not a loop's) are never
// chosen. When the weights leave nothing else, the rule gives way a step at a time, and every
// choice is one draw:
//   1. by weight, neither of the last two;
//   2. by weight, not the last one;
//   3. evenly (weights aside), neither of the last two;
//   4. evenly, not the last one;
//   5. evenly, any (one candidate only).
// So with two candidates or more a note never repeats the one before it. A C chord of one tone,
// Gravity 1, Range 0.5 in C major (60 62 64 65): 60, then a draw among the other three, 60 again,
// ...: the chord tone every other note.
//
// THE PATTERNS:
// - Random: each event a draw as above.
// - Rise: the candidate next above the last note, by weight between the next two; past the top it
//   starts again from the bottom (so the next two of the one under the top are the top and the
//   bottom). Before any note, from the bottom. Two of weight 0 are drawn evenly. Fall is the mirror.
// - Constellation: a motif of Motif notes (3..8), drawn while its first pass plays as Random's are,
//   keeping out also the notes within two of it in the motif (round its end), then replayed in
//   order, one note per event. At the end of each pass, with probability Mutate, one of its notes
//   (drawn) moves to its neighbour (up or down, drawn): the next allowed candidate, which is the
//   next scale tone while Gravity < 1 and the next chord tone at 1, so Gravity 1 keeps the motif on
//   the chord. It moves only if that keeps "no repeat within two" round the motif; otherwise the
//   other way, then the notes after it in turn; none can: no change this pass. A change of the
//   candidates (key, scale, chord, Register, Range, Gravity) moves each motif note no longer
//   allowed to the nearest allowed one that keeps the rule (the lower on a tie; none keeps it: the
//   nearest). A Motif change draws a new motif from the next pass on.
// - Echo: the motif is the last Motif notes the player gave (played()), each moved by octaves into
//   the range (a pitch class with no octave in a range under an octave: the nearest allowed note),
//   a note repeating either of the two kept before it dropped, round the motif's end too (the rule
//   applied to the echo: the plan dropped only consecutive repeats), then replayed and mutated as a
//   Constellation's. A new echo takes over at the start of the pass after the player gave a note.
//   Until the player's notes leave three after the drops, Echo plays the Constellation's motif: a
//   figure of one or two notes can't be replayed without repeats.
// Where a motif changes (a new echo, a snap, a pattern switched midway), its next note may repeat
// one of the last two: it is passed over for the next one in the pass that doesn't; none does: the
// pass ends there and the next pass is looked in; still none: the next that isn't the last note.
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
//   across the pass's end too. Generated events draw their velocity again, as above; the player's
//   keep theirs;
// - nothing is generated (the clock runs on, silent); the player's notes are added to the
//   recording (overdub) and replayed from the next pass on;
// - a note recorded before a change of key, scale or chord replays moved to the nearest allowed
//   note, the lower on a tie: a generated one to an allowed candidate (in the range), a played one
//   to the nearest note whose pitch class is allowed (where it is). It moves from what was
//   recorded, so the loop comes back as it was when the harmony does.
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
// ends). reset() makes one: the first event's work. So no setting (Pattern, Gravity, Register,
// Range, Motif, Mutate, Rubato, Density, the chord, generating or not, the player's notes) shifts
// the sequence that follows: the k-th event after reset() draws the same numbers whatever happens
// between, and Density only moves it in time. A replaying loop is the exception: it draws two
// numbers for each recorded event it comes to (its move and its velocity), when it comes to it: at
// the start of the replay, after a jump, and as the one before plays or is passed over.
//
// COST: the ARM instructions of one step() of 128 samples, built with the device's flags and
// counted by an instruction-counting qemu (the difference of 40 000 and 20 000 blocks, over 20 000):
// 136 generating at Density 60 without a loop; 230 at Density 60 with Loop on (16 s, replaying);
// 277 replaying a full loop (64 events every 2 s, synced, Rubato 1). Air's share of the budget is
// 32k a block (docs/plans/2026-10-07-m2-weather.md): the generator is under 1% of it. On the
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
    int offset;   // samples into the step()
    int note;     // MIDI
    float vel;    // 0..1
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
    // block: the candidates are worked out again only when the key, scale, Register, Range or
    // Gravity change.
    void set(const AirGenPatch& p, const HarmonyPatch& h);
    // What it generates from: the chord (its pitch classes and root) and whether to generate at all
    // (Harmony with a chord, or Free: yes; Notes, or a forgotten harmony: no). A loop replays either way.
    void setChord(const Chord& c, bool generate);
    // A note the player gave Air (Notes, or a key above Split): Echo's motif learns it, a loop records it.
    // Air strikes it itself, so step() doesn't return it (a loop's replay of it, later, it does).
    // `offset`: where in the next step() it sounded (0..n-1; later ones count as its last sample).
    void played(int note, float vel, int offset);
    // MPC's tempo and, while it plays (locked), its position at the next step's first sample: a
    // synced loop's clock. Not locked, the clock runs on at the tempo.
    void setTransport(double bpm, double beats, bool locked) { clock_.set(bpm, beats, locked); }
    // Back to the start: the seed's numbers again (the first event's work drawn), no notes played,
    // no motif, Echo's notes and the loop's recording forgotten, the loop (if on) waiting for the
    // first event. The patch, the chord and the clock stay.
    void reset();
    // Moves on by n samples (n <= 128; any n works; n <= 0: nothing); writes the events due in them,
    // in order, at most `max`; returns how many. The same seed and the same calls give the same
    // events. A replayed event that finds no room plays at the start of the next step; a generated
    // one is silent.
    int step(int n, AirEvent* out, int max);

private:
    enum LoopState : int { LS_OFF, LS_ARMED, LS_RECORDING, LS_PLAYING };
    struct Recorded {
        double place;      // in the pass, 0..1
        int note;          // as it sounded when recorded
        float vel;         // the player's (generated ones draw theirs again)
        bool played;       // the player's note
        uint32_t harmony;  // harmony_ when it was recorded
        int64_t from;      // the first pass that replays it (an overdub isn't replayed in its own pass)
    };
    struct Played {
        int note;
        float vel;
        int offset;
    };

    double draw();                                     // uniform 0..1 (never 0 or 1)
    void rebuildCandidates();
    bool allowed(int i) const { return w_[i] > 0.0f || noneAllowed_; }
    int indexOf(int note) const;                       // the candidate's index, or -1
    int nearestAllowed(int note, const int* keep, int nKeep) const;   // a candidate; keeps the rule against `keep` if it can
    int pick(const int* idx, int n, const int* ex, int nEx, double u) const;
    int choose(double uPick, double uMut, double uWhich, double uDir);   // the generated note
    void startPass();
    void buildEcho();
    void mutate(int* m, int len, double uMut, double uWhich, double uDir);
    void snapMotifs();
    void remember(int note);                           // the last two notes played
    // The loop.
    double passBeats() const;
    void beginRecording(double pos);
    int record(double pos, int note, float vel, bool played, int64_t from);
    void seekReplay(int64_t pass, double place);
    double replayAt() const;                           // where the next replayed event is due, in passes
    int replayNote(const Recorded& r) const;
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
    float w_[kCandMax] = {};
    int nCand_ = 0;
    bool noneAllowed_ = true;     // no candidate weighs above 0: all are allowed
    int last_ = -1, last2_ = -1;  // the generator's last two notes (-1: none)

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
    double repMove_ = 0.0, repVel_ = 0.0;   // its two numbers
    Played heardNow_[kPlayedMax] = {};      // the player's notes for the next step()
    int nHeardNow_ = 0;
};

} // namespace af
