#include "airgen.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace af {
namespace {

constexpr double kMaxPassS = 120.0;       // a synced pass longer than this at the tempo halves
// A synced pass shorter than this doubles: no tempo MPC sends gets there (a quarter note at 1200
// BPM), but a host's absurd one could, and a pass longer than a step keeps a step to one pass end.
constexpr double kMinPassS = 0.05;
constexpr double kJumpSamples = 256.0;     // the clock moving this much more than a step: a jump
constexpr double kNever = std::numeric_limits<double>::infinity();
constexpr int64_t kAlways = std::numeric_limits<int64_t>::min();   // replayed from any pass

// A setting kept in its range; NaN (which no comparison catches) becomes `nan`.
float param(float x, float lo, float hi, float nan) { return x >= lo ? (x <= hi ? x : hi) : (x < lo ? lo : nan); }
int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int pitchClass(int note) { return (note % 12 + 12) % 12; }

// The pass a position (in passes) is in. A position past 2^52 has no fraction left anyway; a host's
// wild song position is kept from overflowing the conversion.
int64_t passOf(double pos) {
    const double f = std::floor(pos);
    return static_cast<int64_t>(f > 4.0e15 ? 4.0e15 : f < -4.0e15 ? -4.0e15 : f);
}

} // namespace

AirGen::AirGen() {
    set(AirGenPatch{}, HarmonyPatch{});
    rebuildCandidates();   // set() builds them only on a change
    reset();
}

void AirGen::seed(uint32_t s) {
    seed_ = s;
    reset();
}

void AirGen::set(const AirGenPatch& p, const HarmonyPatch& h) {
    AirGenPatch q;
    q.density = param(p.density, 0.0f, 60.0f, 0.0f);
    q.pattern = clampi(p.pattern, 0, AP_COUNT - 1);
    q.registerOct = clampi(p.registerOct, 4, 6);
    q.rangeOct = param(p.rangeOct, 0.5f, 3.0f, 0.5f);
    q.gravity = param(p.gravity, 0.0f, 1.0f, 0.0f);
    q.motif = clampi(p.motif, 3, kMotifMax);
    q.mutate = param(p.mutate, 0.0f, 1.0f, 0.0f);
    q.loop = p.loop;
    q.loopS = param(p.loopS, 2.0f, 120.0f, 2.0f);
    q.loopBeats = p.loopBeats > 0.0f ? param(p.loopBeats, 1.0f, 256.0f, 1.0f) : 0.0f;   // NaN: free
    q.rubato = param(p.rubato, 0.0f, 1.0f, 0.0f);
    const int key = pitchClass(h.key), scale = clampi(h.scale, 0, SC_COUNT - 1);

    const bool harmony = key != key_ || scale != scale_;
    const bool allowedMoved = harmony || q.gravity != p_.gravity;   // which notes are allowed
    const int shift = 12 * (q.registerOct - p_.registerOct);
    const bool candidates = allowedMoved || shift != 0 || q.rangeOct != p_.rangeOct;
    if (q.loop && !p_.loop) startLoop_ = true;   // the first pass starts at the next step
    if (!q.loop) {
        loop_ = LS_OFF;
        nRec_ = 0;
        startLoop_ = false;
    }
    p_ = q;
    key_ = key;
    scale_ = scale;
    if (harmony) ++harmony_;
    if (candidates) {
        rebuildCandidates();
        moveMotifs(shift, allowedMoved);
    }
}

void AirGen::setChord(const Chord& c, bool generate) {
    generate_ = generate;
    // A forgotten harmony (root -1) keeps the last chord's tones: a loop replays in it, and nothing
    // moves until a chord comes.
    if (c.root < 0 || c.pcs == pcs_) return;
    pcs_ = c.pcs;
    ++harmony_;
    rebuildCandidates();
    moveMotifs(0, true);
}

void AirGen::played(int note, float vel, int offset) {
    if (note < 0 || note > 127) return;
    // Echo learns it at once (its motif changes only at a pass's start).
    if (nHeard_ == kMotifMax) {
        for (int i = 1; i < kMotifMax; ++i) heard_[i - 1] = heard_[i];
        --nHeard_;
    }
    heard_[nHeard_++] = note;
    heardNew_ = true;
    // The loop records it at its sample, in the next step.
    if (nHeardNow_ < kPlayedMax) heardNow_[nHeardNow_++] = {note, param(vel, 0.0f, 1.0f, 0.0f), offset > 0 ? offset : 0};
}

void AirGen::reset() {
    rng_ = seed_ * 0x9E3779B1u + 0x7F4A7C15u;
    if (rng_ == 0) rng_ = 0x7F4A7C15u;   // xorshift's one stuck state
    work_ = -std::log(draw());           // the first event's work
    last_ = last2_ = -1;
    conN_ = conLen_ = 0;
    echoLen_ = 0;
    nHeard_ = 0;
    heardNew_ = false;
    echoMotif_ = 0;
    useEcho_ = false;
    mpos_ = 0;
    loop_ = p_.loop ? LS_ARMED : LS_OFF;
    startLoop_ = false;
    nRec_ = 0;
    pos_ = 0.0;
    recStart_ = 0.0;
    repPass_ = 0;
    repIdx_ = 0;
    nHeardNow_ = 0;
}

double AirGen::draw() { return static_cast<double>(xorshift(rng_)) * (1.0 / 4294967296.0); }   // never 0: xorshift isn't

// --- the candidates -------------------------------------------------------------------------------

void AirGen::rebuildCandidates() {
    scalePcs_ = 0;
    for (int d = 0; d < scaleSize(scale_); ++d)
        scalePcs_ = static_cast<uint16_t>(scalePcs_ | 1u << pitchClass(key_ + scaleStep(scale_, d)));
    // Whole semitones (a hair over, so 23/12 octaves is 23 however the float rounds), never above
    // the top of the chord range: nothing up there is music anyway.
    lo_ = 12 * (p_.registerOct + 1) + key_;
    hi_ = lo_ + static_cast<int>(std::floor(12.0f * p_.rangeOct + 1e-3f));
    if (hi_ > kChordHighest) hi_ = kChordHighest;
    nCand_ = 0;
    noneAllowed_ = true;
    for (int n = lo_; n <= hi_ && nCand_ < kCandMax; ++n) {
        if (!(scalePcs_ >> pitchClass(n) & 1u)) continue;
        const float w = pcs_ >> pitchClass(n) & 1u ? 1.0f : 1.0f - p_.gravity;
        cand_[nCand_] = n;
        w_[nCand_++] = w;
        noneAllowed_ = noneAllowed_ && !(w > 0.0f);
    }
}

int AirGen::indexOf(int note) const {
    for (int i = 0; i < nCand_; ++i)
        if (cand_[i] == note) return i;
    return -1;
}

// The allowed candidate nearest `note`, the lower on a tie, that is none of `keep` (a motif note's
// neighbours); if every allowed one is, the nearest.
int AirGen::nearestAllowed(int note, const int* keep, int nKeep) const {
    int best = -1, bestAny = -1;
    for (int i = 0; i < nCand_; ++i) {
        if (!allowed(i)) continue;
        const int c = cand_[i];
        // Ascending, so a strictly nearer one replaces it and a tie keeps the lower.
        if (bestAny < 0 || std::abs(c - note) < std::abs(bestAny - note)) bestAny = c;
        bool kept = false;
        for (int k = 0; k < nKeep; ++k) kept = kept || keep[k] == c;
        if (!kept && (best < 0 || std::abs(c - note) < std::abs(best - note))) best = c;
    }
    return best >= 0 ? best : bestAny;
}

// One draw `u` picks among the candidates idx[0..n): by weight, or evenly, keeping out the notes in
// `ex` (ex[0]: the note before), giving way one step at a time (the header's list).
int AirGen::pick(const int* idx, int n, const int* ex, int nEx, double u) const {
    for (int level = 0; level < 5; ++level) {
        const bool weighted = level < 2;
        const int out = level == 0 || level == 2 ? nEx : level == 4 ? 0 : 1;   // how many of ex kept out
        auto weight = [&](int k) {
            for (int e = 0; e < out; ++e)
                if (ex[e] == cand_[idx[k]]) return 0.0;
            return weighted && !noneAllowed_ ? static_cast<double>(w_[idx[k]]) : 1.0;
        };
        double total = 0.0;
        for (int k = 0; k < n; ++k) total += weight(k);
        if (!(total > 0.0)) continue;
        double target = u * total;
        int lastOk = -1;
        for (int k = 0; k < n; ++k) {
            const double w = weight(k);
            if (!(w > 0.0)) continue;
            lastOk = cand_[idx[k]];
            if (target < w) return lastOk;
            target -= w;
        }
        return lastOk;   // rounding at the top end
    }
    return cand_[idx[0]];   // never: level 5 keeps nothing out, and the tonic is always a candidate
}

void AirGen::remember(int note) {
    last2_ = last_;
    last_ = note;
}

// --- the patterns ---------------------------------------------------------------------------------

int AirGen::choose(double uPick, double uMut, double uWhich, double uDir) {
    const int ex[2] = {last_, last2_};
    if (p_.pattern == AP_RANDOM) {
        int all[kCandMax];
        for (int i = 0; i < nCand_; ++i) all[i] = i;
        return pick(all, nCand_, ex, 2, uPick);
    }
    if (p_.pattern == AP_RISE || p_.pattern == AP_FALL) {
        // The walk is over the allowed candidates (Gravity 1: the chord tones); over all of them
        // when fewer than two are allowed, or a lone chord tone would repeat itself.
        int walk[kCandMax], n = 0;
        for (int i = 0; i < nCand_; ++i)
            if (allowed(i)) walk[n++] = i;
        if (n < 2)
            for (n = 0; n < nCand_; ++n) walk[n] = n;
        // The next above (below) the last note, round from the top (bottom); before any note, from
        // the bottom (top).
        int j;
        if (p_.pattern == AP_RISE) {
            j = 0;
            while (j < n && cand_[walk[j]] <= last_) ++j;
            if (j == n) j = 0;
        } else {
            j = n - 1;
            while (j >= 0 && last_ >= 0 && cand_[walk[j]] >= last_) --j;
            if (j < 0) j = n - 1;
        }
        const int next = p_.pattern == AP_RISE ? (j + 1) % n : (j - 1 + n) % n;
        const int two[2] = {walk[j], walk[next]};
        return pick(two, n > 1 ? 2 : 1, ex, 2, uPick);
    }
    // Constellation and Echo: a motif, one note per event, mutated where a pass ends.
    bool mutated = false;
    for (int attempt = 0;; ++attempt) {
        if (mpos_ == 0) startPass();
        int* m = useEcho_ ? echo_ : con_;
        const int len = useEcho_ ? echoLen_ : conLen_;
        int note;
        if (!useEcho_ && conN_ < conLen_) {
            // The Constellation's first pass, drawn as it plays: as Random's, keeping out the notes
            // within two of it in the motif too (round its end, once those are drawn).
            const int i = conN_;
            const int exm[6] = {last_, last2_, i >= 1 ? m[i - 1] : -1, i >= 2 ? m[i - 2] : -1,
                                i + 1 >= len ? m[i + 1 - len] : -1, i + 2 >= len ? m[i + 2 - len] : -1};
            int all[kCandMax];
            for (int k = 0; k < nCand_; ++k) all[k] = k;
            note = pick(all, nCand_, exm, 6, uPick);
            m[conN_++] = note;
            mpos_ = conN_;
        } else {
            // The next note of the pass, passing over one that would repeat one of the last two (only
            // where the motif changed: a new echo, a snap, a pattern switch). None left in this pass
            // fits: the pass ends here, and the next one is looked in.
            int j = -1;
            for (int k = mpos_; k < len && j < 0; ++k)
                if (m[k] != last_ && m[k] != last2_) j = k;
            if (j < 0 && mpos_ > 0 && attempt == 0) {
                mpos_ = 0;
                mutate(m, len, uMut, uWhich, uDir);
                mutated = true;
                continue;
            }
            for (int k = mpos_; k < len && j < 0; ++k)
                if (m[k] != last_) j = k;
            if (j < 0) j = mpos_;
            note = m[j];
            mpos_ = j + 1;
        }
        if (mpos_ >= len) {
            mpos_ = 0;
            if (!mutated) mutate(m, len, uMut, uWhich, uDir);
        }
        return note;
    }
}

// A pass begins: which motif it plays, and whether one is built afresh.
void AirGen::startPass() {
    if (p_.pattern == AP_ECHO) {
        if (heardNew_ || echoMotif_ != p_.motif) {
            buildEcho();
            heardNew_ = false;
            echoMotif_ = p_.motif;
        }
        useEcho_ = echoLen_ >= 3;
    } else {
        useEcho_ = false;
    }
    if (!useEcho_ && conLen_ != p_.motif) {   // never built, or Motif changed: drawn over this pass
        conN_ = 0;
        conLen_ = p_.motif;
    }
}

// Echo's motif: the player's last Motif notes, into the range by octaves, a note repeating either of
// the two kept before it dropped, then the last ones dropped while the motif's end repeats its start.
void AirGen::buildEcho() {
    const int take = nHeard_ < p_.motif ? nHeard_ : p_.motif;
    int k = 0;
    for (int i = nHeard_ - take; i < nHeard_; ++i) {
        int n = heard_[i];
        while (n < lo_) n += 12;
        while (n > hi_) n -= 12;
        if (n < lo_) {
            // A range under an octave without this pitch class: the nearest allowed note to the
            // octave nearest the range.
            n = nearestAllowed(lo_ - n <= n + 12 - hi_ ? n : n + 12, nullptr, 0);
        }
        if ((k >= 1 && n == echo_[k - 1]) || (k >= 2 && n == echo_[k - 2])) continue;
        echo_[k++] = n;
    }
    auto wrapRepeats = [this](int len) {
        return echo_[0] == echo_[len - 1] || (len >= 3 && (echo_[0] == echo_[len - 2] || echo_[1] == echo_[len - 1]));
    };
    while (k >= 2 && wrapRepeats(k)) --k;
    echoLen_ = k >= 3 ? k : 0;
}

// With probability Mutate, a note (drawn) moves to its next allowed neighbour, up or down (drawn),
// keeping "no repeat within two" round the motif; else the other way, then the notes after it.
void AirGen::mutate(int* m, int len, double uMut, double uWhich, double uDir) {
    if (!(uMut < static_cast<double>(p_.mutate)) || len < 3) return;
    const int first = clampi(static_cast<int>(uWhich * len), 0, len - 1);
    const int dir = uDir < 0.5 ? 1 : -1;
    for (int k = 0; k < len; ++k) {
        const int i = (first + k) % len;
        for (int s : {dir, -dir}) {
            int nb = -1;
            if (s > 0) {
                for (int j = 0; j < nCand_ && nb < 0; ++j)
                    if (cand_[j] > m[i] && allowed(j)) nb = cand_[j];
            } else {
                for (int j = nCand_ - 1; j >= 0 && nb < 0; --j)
                    if (cand_[j] < m[i] && allowed(j)) nb = cand_[j];
            }
            if (nb < 0) continue;
            bool keeps = true;
            for (int d = 1; d <= 2; ++d) keeps = keeps && nb != m[(i + d) % len] && nb != m[(i - d + len) % len];
            if (keeps) {
                m[i] = nb;
                return;
            }
        }
    }
}

// The candidates changed. Both motifs (the notes drawn so far) move by `shift` semitones (Register's
// octaves), whole, keeping their shape; a note still outside the range folds in by octaves. Then a
// note that found no octave in the range, that repeats one within two of it, or (`snap`: what is
// allowed changed) that isn't allowed, moves to the nearest allowed note that keeps the rule
// against those within two of it (none does: the nearest).
void AirGen::moveMotifs(int shift, bool snap) {
    for (int which = 0; which < 2; ++which) {
        int* m = which ? echo_ : con_;
        const int n = which ? echoLen_ : conN_, len = which ? echoLen_ : conLen_;
        bool lost[kMotifMax] = {};
        for (int i = 0; i < n; ++i) {
            int note = m[i] + shift;
            while (note < lo_) note += 12;
            while (note > hi_) note -= 12;
            lost[i] = note < lo_;   // a range under an octave without this pitch class
            m[i] = note;
        }
        for (int i = 0; i < n; ++i) {
            int keep[4], nk = 0;
            for (int d = 1; d <= 2; ++d) {
                const int a = (i + d) % len, b = (i - d + len) % len;
                if (a < n && a != i) keep[nk++] = m[a];
                if (b < n && b != i) keep[nk++] = m[b];
            }
            bool repeats = false;
            for (int k = 0; k < nk; ++k) repeats = repeats || keep[k] == m[i];
            const int at = indexOf(m[i]);
            if (lost[i] || repeats || (snap && !(at >= 0 && allowed(at)))) m[i] = nearestAllowed(m[i], keep, nk);
        }
    }
}

// --- the loop ---------------------------------------------------------------------------------------

double AirGen::passBeats() const {
    double b = p_.loopBeats;
    while (b * 60.0 / clock_.bpm > kMaxPassS) b *= 0.5;
    while (b * 60.0 / clock_.bpm < kMinPassS) b *= 2.0;
    return b;
}

void AirGen::beginRecording(double pos) {
    loop_ = LS_RECORDING;
    recStart_ = pos;
    nRec_ = 0;
}

// Into the recording at its place in the pass, after any at the same place. `from`: the first pass
// that replays it. Returns where it went (-1: the recording is full).
int AirGen::record(double pos, int note, float vel, bool played, int64_t from) {
    if (nRec_ == kLoopMax) return -1;
    const double place = pos - std::floor(pos);
    int at = nRec_;
    while (at > 0 && rec_[at - 1].place > place) {
        rec_[at] = rec_[at - 1];
        --at;
    }
    rec_[at] = {place, note, vel, played, harmony_, from};
    ++nRec_;
    return at;
}

// The replay goes on from `place` in pass `pass`: its next event is the first at or after that
// place. Overdubs wait no longer for a pass of their own (the passes are numbered afresh). The end of
// the first pass seeks the very place it started at, worked out as the first event's was, so a
// rounding can't put that event a pass later.
void AirGen::seekReplay(int64_t pass, double place) {
    repPass_ = pass;
    repIdx_ = 0;
    while (repIdx_ < nRec_ && rec_[repIdx_].place < place) ++repIdx_;
    if (repIdx_ == nRec_) {
        repIdx_ = 0;
        ++repPass_;
    }
    for (int i = 0; i < nRec_; ++i) rec_[i].from = kAlways;
}

// A replayed event's random numbers (0: its move, 1: its velocity), uniform 0..1: a hash of the
// seed, the pass and the event's index (SplitMix64's finalizer), so the replay never draws from the
// clock's xorshift and a loop never shifts the generator's sequence.
double AirGen::replayNumber(int which) const {
    uint64_t x = static_cast<uint64_t>(seed_) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(repPass_) * 0xBF58476D1CE4E5B9ull ^
                 static_cast<uint64_t>(2 * repIdx_ + which + 1) * 0x94D049BB133111EBull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return (static_cast<double>(x >> 11) + 0.5) * (1.0 / 9007199254740992.0);   // 53 bits, never 0 or 1
}

// Where the next replayed event is due, in passes: its place, moved by up to Rubato x 0.1 x the
// shorter of its gaps (cyclically).
double AirGen::replayAt() const {
    const Recorded& e = rec_[repIdx_];
    double gap = 1.0;
    if (nRec_ > 1) {
        const double prev = repIdx_ > 0 ? e.place - rec_[repIdx_ - 1].place : e.place + 1.0 - rec_[nRec_ - 1].place;
        const double next = repIdx_ + 1 < nRec_ ? rec_[repIdx_ + 1].place - e.place : rec_[0].place + 1.0 - e.place;
        gap = prev < next ? prev : next;
    }
    return static_cast<double>(repPass_) + e.place + static_cast<double>(p_.rubato) * 0.1 * gap * (2.0 * replayNumber(0) - 1.0);
}

void AirGen::nextReplay() {
    if (++repIdx_ >= nRec_) {
        repIdx_ = 0;
        ++repPass_;
    }
}

// A recorded note as it replays: as recorded, or (recorded in another harmony) the nearest allowed,
// the lower on a tie: a generated one an allowed candidate (in the range), a played one the nearest
// note whose pitch class is allowed, wherever it is.
// Then into kChordLowest..kChordHighest (Air's voices' range) by octaves: the player's notes can be
// anywhere.
int AirGen::replayNote(const Recorded& e) const {
    int note = e.note;
    if (e.harmony != harmony_) {
        if (!e.played) {
            note = nearestAllowed(e.note, nullptr, 0);
        } else {
            bool found = false;
            for (int d = 0; d < 12 && !found; ++d)
                for (int n : {e.note - d, e.note + d}) {
                    const int pc = pitchClass(n);
                    if (!found && n >= 0 && n <= 127 && (scalePcs_ >> pc & 1u) &&
                        (p_.gravity < 1.0f || noneAllowed_ || (pcs_ >> pc & 1u))) {
                        note = n;
                        found = true;
                    }
                }
        }
    }
    while (note < kChordLowest) note += 12;
    while (note > kChordHighest) note -= 12;
    return note;
}

// --- the step ---------------------------------------------------------------------------------------

int AirGen::step(int n, AirEvent* out, int max) {
    if (n <= 0) return 0;
    if (max < 0) max = 0;

    // Where the loop is, in passes, at this step's first sample, and how far a sample moves it.
    double P, dP;
    if (p_.loopBeats > 0.0f) {
        const double beats = passBeats();
        dP = clock_.bpm / (60.0 * static_cast<double>(kRate)) / beats;
        P = clock_.beats / beats;
        if (!(std::fabs(P) < 4.0e15)) P = 0.0;   // a wild song position: anywhere will do
    } else {
        dP = 1.0 / (static_cast<double>(p_.loopS) * static_cast<double>(kRate));
        P = pos_;
    }
    if (startLoop_) {
        startLoop_ = false;
        beginRecording(P);
    } else if (std::fabs(P - pos_) > kJumpSamples * dP) {
        // The clock jumped (MPC located, the division or Free / Sync changed): a recording keeps
        // what it has and runs its pass's length on from here; a replay finds its place again.
        if (loop_ == LS_RECORDING) recStart_ += P - pos_;
        else if (loop_ == LS_PLAYING) seekReplay(passOf(P), P - std::floor(P));
    }

    // The player's notes in the order of their samples (insertion: a few at most).
    for (int i = 0; i < nHeardNow_; ++i) {
        if (heardNow_[i].offset > n - 1) heardNow_[i].offset = n - 1;
        for (int j = i; j > 0 && heardNow_[j].offset < heardNow_[j - 1].offset; --j) {
            const Played t = heardNow_[j];
            heardNow_[j] = heardNow_[j - 1];
            heardNow_[j - 1] = t;
        }
    }

    // Through the step in time order: the first pass ending, the player's notes, the replay, the
    // clock's events (that order on a tie).
    const double rate = static_cast<double>(p_.density) / (60.0 * static_cast<double>(kRate));   // work a sample
    int count = 0, heard = 0;
    double t = 0.0;
    bool replayWaits = false;
    for (int guard = 0; guard < 4 * kLoopMax + kPlayedMax + 64; ++guard) {
        const double tEnd = loop_ == LS_RECORDING ? (recStart_ + 1.0 - P) / dP : kNever;
        const double tHeard = heard < nHeardNow_ ? static_cast<double>(heardNow_[heard].offset) : kNever;
        const double tRep = loop_ == LS_PLAYING && !replayWaits ? (replayAt() - P) / dP : kNever;
        const double tGen = rate > 0.0 ? t + work_ / rate : kNever;
        double te = tEnd < tHeard ? tEnd : tHeard;
        te = tRep < te ? tRep : te;
        te = tGen < te ? tGen : te;
        if (!(te < n)) break;
        if (te < t) te = t;   // late: a replayed event that found no room, or the clock's rounding
        work_ -= rate * (te - t);
        if (work_ < 0.0) work_ = 0.0;
        t = te;
        const double pos = P + t * dP;
        const int off = clampi(static_cast<int>(t + 0.5), 0, n - 1);

        if (tEnd <= t) {   // the first pass is over: replay it, or (nothing in it) wait for the next event
            if (nRec_ == 0) {
                loop_ = LS_ARMED;
            } else {
                loop_ = LS_PLAYING;
                seekReplay(passOf(recStart_) + 1, recStart_ - std::floor(recStart_));
            }
            continue;
        }
        if (tHeard <= t) {   // the player's note: recorded, or overdubbed
            const Played& h = heardNow_[heard++];
            if (loop_ == LS_ARMED) beginRecording(pos);
            if (loop_ == LS_RECORDING) {
                record(pos, h.note, h.vel, true, kAlways);
            } else if (loop_ == LS_PLAYING) {
                const int64_t now = passOf(pos);
                const int at = record(pos, h.note, h.vel, true, now + 1);
                // The replay keeps its next event (an index under the count still: it moves up by
                // one as the count does). An overdub just at it goes behind it if the replay is in
                // the overdub's own pass, and is passed over there anyway.
                if (at >= 0 && (at < repIdx_ || (at == repIdx_ && repPass_ <= now))) ++repIdx_;
            }
            continue;
        }
        if (tRep <= t) {
            const Recorded& e = rec_[repIdx_];
            if (repPass_ < e.from) {   // an overdub in its own pass: passed over
                nextReplay();
            } else if (count < max) {
                const float vel = e.played ? e.vel : 0.7f * (1.0f - 0.5f * p_.rubato * static_cast<float>(replayNumber(1)));
                const int note = replayNote(e);
                out[count++] = {off, note, vel, e.played};
                remember(note);   // among the last two notes: the generator, once the loop is off, won't repeat it
                nextReplay();
            } else {
                replayWaits = true;   // no room: it plays at the next step's start
            }
            continue;
        }
        // A clock event: its six draws, whatever comes of it.
        const double uGap = draw(), uPick = draw(), uVel = draw(), uMut = draw(), uWhich = draw(), uDir = draw();
        work_ = -std::log(uGap);
        if (!generate_ || loop_ == LS_PLAYING || count >= max) continue;
        const int note = choose(uPick, uMut, uWhich, uDir);
        remember(note);
        const float vel = 0.7f * (1.0f - 0.5f * p_.rubato * static_cast<float>(uVel));
        out[count++] = {off, note, vel, false};
        if (loop_ == LS_ARMED) beginRecording(pos);
        if (loop_ == LS_RECORDING) record(pos, note, vel, false, kAlways);
    }
    work_ -= rate * (n - t);
    if (work_ < 0.0) work_ = 0.0;
    pos_ = P + n * dP;
    clock_.advance(n);
    nHeardNow_ = 0;
    return count;
}

} // namespace af
