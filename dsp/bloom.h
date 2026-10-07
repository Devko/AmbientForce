#pragma once
// Bloom (docs/CONCEPT.md 5.2): the chord stratum. Six voices, each a pair of lifetime oscillators
// joined by Couple (dsp/lifeosc.h), a breath of noise at the note, one state-variable filter and an
// envelope shaped for slow music.
//
// A voice, per sample:
//   A (Hermite) on Table and B (linear) on Table B, B at B Oct, both at the voice's own LifeScan
//   position, coupled (renderCoupled: Mix at Blend 0 doesn't render B at all)
//   + breath: white noise band-passed at the note (Q 4), scaled to the note's band so that
//     Breath 1 is as loud as the tone at any pitch. The voices band-pass one white noise between
//     them, so the breath of two close notes is partly the same noise: unheard at small Breath.
//   -> the SVF (dsp/svf.h): LP, BP or HP at Tone; Reso is its Q, 0.5 x 32^reso (0.5 at 0, 0.71
//     at 0.1, 16 at 1); BP is the band output, whose peak matches LP's resonant peak
//   x envelope x velocity -> pan (equal power) -> the dry and the send
// A new Table or Table B (another one chosen, or the slot's table published over the sine it
// played until it was built) fades in over 20 ms in every sounding voice, both pairs of tables
// read only for that long, as Ground does; a second change waits for the fade under way. So Bloom
// keeps the TableSet's table pointers from one render to the next (the pair it plays, and the old
// pair while a new one fades in): the tables must outlive it, as plugin/tables.h promises while
// an instance lives (nothing is unpublished or freed). A Bloom still around after
// releaseTables() must be reset() before it renders again.
//
// Unison 2: a second pair, the two detuned by -Detune/2 and +Detune/2 cents, each at 1/sqrt 2 (the
// same loudness as one, their slow drift averaging their sum to it), in the SVF's two lanes. Below
// 1 cent of Detune it plays as unison 1: two halves that close are one sound, and at Detune 0 any
// fixed phase between them is a comb on a rich table (a quarter cycle takes a saw's 2nd harmonic
// down 40 dB). From 1 cent up the second half starts a quarter of a cycle after the first (B by the
// same time), so the sum never depends on the seed. The breath is the same in both lanes, so it
// goes in at 1 / (g0 + g1) (the halves' gains): coherent, as loud as unison 1's. Switching unison
// (or Detune across 1 cent) glides the second half in or out over 3 ms, its gain, place and detune,
// never at once. Width spreads a chord's notes, lowest to highest, over +-0.6 Width and each
// voice's two halves +-0.4 Width around it.
//
// The envelope: Swell rises in amplitude along an S-curve, 0.5 - 0.5 cos(pi t / swell), so it is
// half way (-6 dB) at half the time; sustain 1; Release falls exponentially, -60 dB over Release,
// and the voice is free there. Velocity: a gain of 1 - Vel + Vel x vel (linear in amplitude).
//
// The tail handoff (CONCEPT.md 8), Tail Space with Release longer than kHandoffS and the send open
// (the last render's spaceSend above 0; the engine passes 0 when Space's return is 0, when Space is
// off or bypassed, or when Bloom's own send is 0, and with no Space to carry it a release is Tail
// Voice's): once released, a voice's dry follows its release
// times a cos^2 fade from 1 to 0 over kHandoffS, while its send carries the same energy into Space
// as the whole release would have with Tail Voice. The send is the release times a boost that ramps
// from 1 to B over 0.2 s, and over the handoff's last 0.2 s it fades out too (cos^2), so it never
// steps when the voice goes; B is what makes that send's energy equal the whole release's (worked
// out in set() when Release changes; at most 4). An exponential release spends most of its energy
// early (Release 10: 87% in the first 1.5 s), so B stays small: 1.10 at Release 10, 1.50 at 30 (the
// plan's sqrt(Release / kHandoffS), 2.58 and 4, sent 6.9 and 8.3 dB too much). The voice is free
// at kHandoffS. The engine holds Space's decay at Release or longer while Tail is Space, so the
// reverb carries on the tail the voice let go. Turning the send to 0 during a handoff loses that
// tail.
//
// Voices and owners:
// - A note takes a free voice, else the quietest releasing one, else the oldest. A voice taken
//   from a note fades it out over 3 ms before its own note starts. "Quietest" is the dry's level:
//   a voice handing off can go while its send still carries some tail (accepted: its dry, what
//   the ear follows, is the quieter for it).
// - A note some voice already holds keeps sounding untouched: it only gains the new owner, and
//   sounds until all its owners have let go (two chords sharing a note, either key up alone keeps
//   it). A note still releasing, or handing off, swells again in its voice from where it is (the
//   handoff's dry fade folded into its level, its send beyond the dry fading out over 3 ms; its
//   velocity glides over 3 ms): a key pressed again and again keeps to one voice.
// - Re-voicing a held chord (the next key under Hold, say): play(new, newKey) before
//   release(oldKey), so the notes common to both carry on untouched (C E G to E G B: 4 voices, not
//   6). The other way round they swell again from where their release has them, in their voices.
// - Strum: note k of n (lowest first) starts k strumS / max(1, n - 1) later (strumS 0..2),
//   counted in samples (render ends a control step where a start falls). A start still waiting is
//   cancelled when its owners have all let go. moveTo() strums only the notes that are new.
// - Owners are keys 0..127, or -1 for the Harmony and Free modes' chords; any other value counts
//   as -1.
// The engine owns Listen, Hold and the pedal: it simply doesn't call release().
//
// A voice's state (checkInvariants() checks it; the tests call it after odd sequences):
// - stage ST_FREE: no note, its gains at 0; it may hold a note waiting to start (next >= 0).
// - It has owners exactly while it holds a note: sounding in ST_ATTACK or ST_SUSTAIN, or waiting
//   to start (next >= 0). heldNote() is that note; no two voices hold the same one.
// - A start waiting either swells again the note releasing in the voice (retrig: ST_RELEASE or
//   ST_HANDOFF, the same note), or starts a fresh one once the voice is free (ST_FREE, or ST_STEAL
//   fading the note it had).
// - Envelope, unison presence and the steal's fade stay within 0..1; a handoff ends at kHandoffS.
//
// Level is a gain 0..1: the knob's audio taper is the patch map's (as Ground's level). Level and
// Mute glide (10 ms). At 0 the voices aren't rendered at all, only their envelopes and starts move
// on, so a muted Bloom costs next to nothing and its voices still come free.
//
// Control rate: every kChunk samples (or where a strum's start or a steal's fade ends) the
// envelopes, the LifeScan positions and the filter's targets are worked out; every gain then moves
// in a straight line to its new value across the step and the filter's coefficients glide per
// sample (the cutoff evenly in octaves), so nothing steps. The SVF runs per sample.
//
// The cost, as ARM instructions per 128-sample block with all six voices sounding on a lifetime
// table (the device's flags, counted under qemu): unison 1 about 97k with Mix (126 a voice and
// sample: A's read 74, the breath, SVF, pan and gains 36, the control steps and the bus the rest),
// 121k with FM; unison 2 153k, FM 203k. By PolyForce's calibration (about 1 ns an instruction on
// the device) that is 3.3% / 4.2% of a block at unison 1 and 5.3% / 7.0% at unison 2, against
// Bloom's 4% (CONCEPT.md 11): if the device bench agrees, unison 2 is the first cap to fall. A
// table change reads both pairs of tables for its 20 ms.
//
// Real-time rules: everything lives in fixed arrays. Nothing allocates, locks or throws.
#include "common.h"
#include "harmony.h"
#include "lifeosc.h"
#include "lifetime.h"
#include "svf.h"

#include <cstdint>

namespace af {

enum FilterMode : int { FM_LP, FM_BP, FM_HP, FM_COUNT };
enum Tail : int { TL_VOICE, TL_SPACE, TL_COUNT };
// The surface's names for them (harmony.h says why).
inline constexpr const char* kFilterModeNames[] = {"LP", "BP", "HP"};
inline constexpr const char* kTailNames[] = {"Voice", "Space"};
static_assert(sizeof kFilterModeNames / sizeof *kFilterModeNames == FM_COUNT &&
                  sizeof kTailNames / sizeof *kTailNames == TL_COUNT,
              "a name per value");

struct BloomPatch {
    int listen = LI_NOTES;     // the engine's: how Bloom hears the player (Bloom itself ignores it)
    bool mute = false;
    float level = 0.7f * 0.7f; // a gain 0..1 (the knob's audio taper is the patch map's): the knob at 70%
    float cutoffHz = 5000.0f;  // Tone: 20..20000
    float reso = 0.1f;         // 0..1: Q 0.5 x 32^reso
    int filterMode = FM_LP;
    int table = TB_FELT_PIANO;
    LifePos pos{0.6f, 0.25f, 0.07f, 0.1f};
    int tableB = TB_SINE;
    int bOctave = 0;           // -2..+2
    float blend = 0.0f;
    int couple = CP_MIX;
    float coupleAmt = 0.0f;
    int unison = 1;            // 1..2
    float detuneCents = 8.0f;  // 0..50: between the two halves of unison 2
    float swellS = 2.5f;       // attack 0.005..30
    float releaseS = 6.0f;     // 0.01..30: to -60 dB
    float velSens = 0.4f;      // 0..1
    float breath = 0.05f;      // band-passed noise at the note, 0..1 (1: as loud as the tone)
    int tail = TL_SPACE;
    float width = 0.6f;        // unison / chord spread across the stereo field, 0..1
};

class Bloom {
public:
    static constexpr int kVoices = 6;
    static constexpr float kHandoffS = 1.5f;
    static constexpr int kMaxBlock = 128;   // render's n at most

    enum Stage : int { ST_FREE, ST_ATTACK, ST_SUSTAIN, ST_RELEASE, ST_HANDOFF, ST_STEAL };
    // What the tests see of a voice.
    struct VoiceView {
        int stage = ST_FREE;
        int note = -1;      // the note sounding (stage != ST_FREE; in ST_STEAL the one fading out)
        int next = -1;      // a note waiting to start in it (a strum, a steal's fade); -1: none
        float env = 0.0f;   // the envelope at the last control step, 0..1 (before velocity and fades)
        float pos = 0.0f;   // where in the table's life it read at the last control step (Age, Sway, Smear)
    };

    Bloom();
    void seed(uint32_t s);
    void set(const BloomPatch& p, const HarmonyPatch& h);
    // A chord (or one note) starting now. `owner`: the key that started it (release(owner) releases
    // its notes); -1 for the Harmony and Free modes' chords. vel 0..1. Strum from h.strumS.
    void play(const Chord& c, int owner, float vel);
    void release(int owner);              // the notes `owner` holds (those no other owner holds too)
    void releaseAll();
    // Harmony/Free modes: move to `c`. Notes common to both keep sounding untouched; the others
    // release, the new ones start (with the strum).
    void moveTo(const Chord& c, float vel);
    void reset();                         // every voice free and silent at once; the seed's numbers again
    // MPC's tempo and, while it plays (locked), its position at the next render's first sample: a
    // synced sway's clock (LifePos swayBeats). Synced, the voices are staggered on it, voice i a
    // sixth of a cycle times i after the bar's phase, so a chord still shimmers while it stays on
    // the bar, and each voice keeps its place from note to note. Not locked, the clock runs on at
    // the tempo. Each voice keeps its own free phase meanwhile, and goes back to it, gliding, when
    // the sway is Free again (LifeScan).
    void setTransport(double bpm, double beats, bool locked) { clock_.set(bpm, beats, locked); }
    // Adds the dry into outL/outR and the send, at spaceSend 0..1 (gliding across the call from the
    // last call's), into sendL/sendR. n <= kMaxBlock (more is rendered kMaxBlock at a time).
    void render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR,
                float spaceSend, int n);
    // Voices in use: sounding (attack, sustain, release, a steal's fade) or waiting to start.
    int active() const;
    // The largest factor the tail handoff puts on a voice's send now, over its release curve: the
    // energy-matched boost as it ramps in (1 to B over 0.2 s) and fades out (the last 0.2 s);
    // 1 when no voice is handing off.
    float handoffBoost() const;
    VoiceView voice(int i) const;
    bool checkInvariants() const;         // every voice's state as the head of this file has it

private:
    static constexpr int kOwnerWords = 5;   // keys 0..127, and bit 128 for owner -1

    struct Voice {
        int stage = ST_FREE;
        int note = -1;
        // A start waiting: the note, the samples until it may start, and how: retrig, the note
        // releasing here swells again; else a fresh start once the voice is free.
        int next = -1;
        int wait = 0;
        bool retrig = false;
        float nextVel = 0.0f, nextSpread = 0.0f;
        uint32_t owners[kOwnerWords] = {};  // who holds the note (the waiting one, if any)
        uint64_t age = 0;                   // when it was last played: the oldest is taken first

        float x = 0.0f;          // the attack's progress, 0..1
        float env = 0.0f;        // the envelope at the last control step's end
        float vel = 0.0f;        // the velocity asked; velNow glides there (a swell again)
        float velNow = 0.0f, velStep = 0.0f;
        float th = 0.0f;         // seconds into the handoff
        float boost = 1.0f;      // the handoff's full boost
        float sendMul = 1.0f;    // the factor the handoff puts on the send now
        // A handoff swelling again: what its send had beyond the dry (a level, like env), fading
        // out over restLeft more samples.
        float rest = 0.0f;
        int restLeft = 0;
        int stealLeft = 0;       // samples of a steal's fade to go
        float loud = 0.0f;       // the dry's gain now, for the quietest-release choice
        float stealLoud = 0.0f;  // ... and the level, when a steal's fade began
        float stealLv = 0.0f;
        float spread = 0.0f;     // -1..1: its place in the chord
        bool live = false;       // rendered in this control step

        double pitch = 0.0;      // tuned, fractional MIDI
        float incA[2] = {}, incB[2] = {};
        float uni = 0.0f;        // the second half's presence, 0..1: gliding to Unison's 0 or 1 over 3 ms
        f2 pan[2] = {};          // each half's (L, R) gains, its share of the voice (1/sqrt 2 at unison 2) in them
        float bk = 1.0f;         // the breath's gain in each lane: 1 / (g0 + g1)
        // Each half's dry gains and its send beyond the dry (handoffs): at the step's start (the
        // last step's end) and its end. A step moves them in a straight line from one to the other.
        f2 P0[2] = {}, P[2] = {}, S0[2] = {}, S[2] = {};
        f2 stealP[2] = {}, stealS[2] = {};   // ... when a steal's fade began

        float bpA1 = 0.0f, bpA2 = 0.0f, bpA3 = 0.0f, bpNorm = 0.0f;   // the breath's band-pass
        float bpIc1 = 0.0f, bpIc2 = 0.0f;
        SvfState svf;
        TableOsc oa[2];
        TableOscLinear ob[2];
        // While a new table fades in: the oscillators as they were at the switch, on the old tables.
        TableOsc oldA[2];
        TableOscLinear oldB[2];
        bool fading = false;
        LifeScan scan;
        float pos = 0.0f;
    };

    // The filter's settings: g, k and the output mix.
    struct Filter {
        float g = 0.0f, k = 0.0f, m[3] = {};
        bool operator==(const Filter& o) const {
            return g == o.g && k == o.k && m[0] == o.m[0] && m[1] == o.m[1] && m[2] == o.m[2];
        }
    };

    static constexpr int kTableFade = 882;  // 20 ms: a new table fades in over this many samples

    static int ownerBit(int owner) { return owner >= 0 && owner <= 127 ? owner : 128; }   // else -1's
    static int heldNote(const Voice& v) { return v.next >= 0 ? v.next : v.note; }   // when held(v)
    static bool held(const Voice& v);
    static bool owns(const Voice& v, int bit) { return (v.owners[bit >> 5] >> (bit & 31)) & 1u; }
    static void addOwner(Voice& v, int bit) { v.owners[bit >> 5] |= 1u << (bit & 31); }
    static void clearOwners(Voice& v);

    void reseed();
    Voice* holding(int note);             // the voice that holds `note` (sounding or waiting)
    Voice* releasing(int note);           // a voice releasing `note` that may swell again
    Voice& allocate(uint32_t taken);      // free, quietest releasing, oldest; never one in `taken`
    // `note` for `owner` in a chord's place `spread`, `wait` samples from now: kept if held,
    // swelling again if releasing, else in a voice of its own. Marks the voice in `taken`.
    void add(int note, int owner, float vel, float spread, int wait, uint32_t& taken);
    void letGo(Voice& v);                 // its owners are gone: cancel its start, or release it
    void steal(Voice& v);
    void begin(Voice& v);                 // the waiting note starts now
    void tune(Voice& v);                  // increments (the halves' detune by v.uni) and the breath's band-pass
    void placePan(Voice& v);              // the halves' gains and places, by Width and v.uni
    int strumWait(int k, int n) const;
    void control(int m);                  // a control step of m samples: starts, envelopes, gains
    void filterFor(int m);                // the filter's coefficients across the next m samples
    void renderBlock(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR,
                     float spaceSend, int n);
    // fade: the new tables' share across the step while they fade in, else nullptr.
    void renderVoice(Voice& v, int o, int m, const float* fade);
    // Mode: the filter's FM_LP, FM_BP or FM_HP, each its own output; FM_COUNT while it glides
    // (the general mix, per sample).
    template <bool Unison, bool Breath, int Mode, bool Send>
    void voiceLoop(Voice& v, const float* a0, const float* a1, int o, int m);

    BloomPatch p_;
    HarmonyPatch h_;
    uint32_t seed_ = 1;
    uint32_t rng_ = 1;                    // the breath's white noise
    uint64_t played_ = 0;                 // notes played so far: each voice's age
    BeatClock clock_;                     // a synced sway's beats (setTransport)
    float attackRate_ = 0.0f;             // the attack's progress per sample
    float releaseLog2_ = 0.0f;            // log2 of the release's factor per sample
    float boost_ = 0.0f;                  // the handoff's B for this Release (0: not worked out yet)
    float lv_ = 0.0f, lvT_ = 0.0f;        // the level at the last step's end, and where it goes
    float send_ = 0.0f;                   // the last render's spaceSend
    bool panDirty_ = true;
    Filter fT_, fNow_;                    // the filter's target, and where it is
    bool glide_ = false;                  // the coefficients glide in this step (the arrays), else a1_..a3_
    float a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    float ga1_[kChunk] = {}, ga2_[kChunk] = {}, ga3_[kChunk] = {}, gm_[3][kChunk] = {};
    bool sendOn_ = false;                 // a voice may send beyond its dry in this render (sendX_ in use)
    // The tables the voices read (TableSet's, as of the last render), and while a new pair fades
    // in the old pair and the samples of the fade to go.
    const Wavetable* tabA_ = nullptr;
    const Wavetable* tabB_ = nullptr;
    const Wavetable* oldA_ = nullptr;
    const Wavetable* oldB_ = nullptr;
    int tableFade_ = 0;
    Voice v_[kVoices];
    f2 bus_[kMaxBlock] = {};              // the voices' dry, L and R side by side
    f2 sendX_[kMaxBlock] = {};            // the send beyond the dry (handoffs)
    float noise_[kMaxBlock] = {};
};

} // namespace af
