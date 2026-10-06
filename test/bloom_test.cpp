// Bloom (dsp/bloom.h) on its own: one chord and its release, the strum, stealing, voice-led moves,
// the tail handoff, Swell, velocity, stability; owners, retriggers, the filter, breath, width and
// mute; determinism, and nothing allocating. Needs no plugin: make test-module M=bloom runs it alone.
#include "check.h"
#include "signal.h"
#include "../dsp/bloom.h"
#include "../dsp/lifetime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
// The sanitizer runtime's (sanitizer/allocator_interface.h, which GCC doesn't install): hooks its
// allocator calls on every allocation and free.
extern "C" int __sanitizer_install_malloc_and_free_hooks(void (*malloc_hook)(const volatile void*, size_t),
                                                         void (*free_hook)(const volatile void*));
#define BLOOM_COUNTS_ALLOCS 1
#endif

namespace aft {
namespace {

using af::Bloom;
constexpr int kBlk = 128;   // MPC's block

int samples(double s) { return static_cast<int>(std::lround(s * af::kRate)); }

// The tables the checks play: Felt Piano and the saw, built once per run. Every other slot reads
// TableSet's fallback, the one-frame sine (the same wave as TB_SINE).
const af::TableSet& tables() {
    static af::Wavetable t[af::TB_COUNT];
    static af::TableSet set;
    static bool built = false;
    if (!built) {
        for (int id : {af::TB_FELT_PIANO, af::TB_SAW}) {
            CHECK(af::buildTable(id, t[id]));
            set.t[id].store(&t[id]);
        }
        af::sineTable();
        built = true;
    }
    return set;
}

af::Chord chordOf(std::initializer_list<int> notes) {
    af::Chord c;
    for (int n : notes) {
        c.notes[c.n++] = n;
        if (n >= 0) c.pcs = static_cast<uint16_t>(c.pcs | 1u << (n % 12));   // -1: a note play() skips
    }
    c.root = c.n ? c.notes[0] : -1;
    return c;
}

// A patch that is easy to measure: a sine, no breath, the filter open and flat, a 5 ms swell,
// velocity off, full level, Tail Voice, all in the middle.
af::BloomPatch plain() {
    af::BloomPatch p;
    p.table = af::TB_SINE;
    p.breath = 0.0f;
    p.cutoffHz = 20000.0f;
    p.reso = 0.0f;
    p.swellS = 0.005f;
    p.velSens = 0.0f;
    p.level = 1.0f;
    p.tail = af::TL_VOICE;
    p.width = 0.0f;
    return p;
}

// What a run rendered, all samples: the dry and the send.
struct Run {
    Buf L, R, SL, SR;
};

// Renders `n` more samples into r in 128-frame blocks; after each block calls each(at), `at` the
// block's first sample in r.
template <class F>
void render(Bloom& b, Run& r, int n, F each, float send = 0.5f) {
    for (int done = 0; done < n; done += kBlk) {
        const int m = std::min(kBlk, n - done);
        const size_t at = r.L.size();
        for (Buf* x : {&r.L, &r.R, &r.SL, &r.SR}) x->resize(at + static_cast<size_t>(m), 0.0f);
        b.render(tables(), &r.L[at], &r.R[at], &r.SL[at], &r.SR[at], send, m);
        each(at);
    }
}
void render(Bloom& b, Run& r, int n) {
    render(b, r, n, [](size_t) {});
}

// The voice sounding `note` (not one fading it out for a steal), or -1.
int voiceOf(const Bloom& b, int note) {
    for (int i = 0; i < Bloom::kVoices; ++i) {
        const Bloom::VoiceView v = b.voice(i);
        if (v.note == note && v.stage != Bloom::ST_FREE && v.stage != Bloom::ST_STEAL) return i;
    }
    return -1;
}
// Some voice sounds `note` (a steal's fade included) or waits to start it.
bool anyHas(const Bloom& b, int note) {
    for (int i = 0; i < Bloom::kVoices; ++i) {
        const Bloom::VoiceView v = b.voice(i);
        if ((v.note == note && v.stage != Bloom::ST_FREE) || v.next == note) return true;
    }
    return false;
}

constexpr double kC4 = 261.6255653;   // note 60: the key's tonic, the same in every tuning

// Check 1: a chord takes three voices; released (1 s, Tail Voice), they are free within 1.1 s.
void testChord() {
    std::printf("== bloom: one chord and its release\n");
    Bloom b;
    b.seed(1);
    af::BloomPatch p = plain();
    p.releaseS = 1.0f;
    b.set(p, af::HarmonyPatch{});
    CHECK(b.active() == 0);
    b.play(chordOf({60, 64, 67}), 60, 0.8f);
    CHECK(b.active() == 3);
    Run r;
    render(b, r, samples(0.5));
    CHECK(b.active() == 3);
    const double sustain = rms(r.L, static_cast<size_t>(samples(0.3)));
    CHECK(sustain > 0.05);
    b.release(64);   // a key that started nothing: its note is 60's
    render(b, r, kBlk);
    CHECK(b.active() == 3);
    const size_t rel = r.L.size();
    b.release(60);
    long freeAt = -1;
    render(b, r, samples(1.3), [&](size_t at) {
        if (freeAt < 0 && b.active() == 0) freeAt = static_cast<long>(at + kBlk - rel);
    });
    const double half = db(rms(r.L, rel + samples(0.48), rel + samples(0.52)) / sustain);
    std::printf("  release 1 s: free after %.3f s; at 0.5 s %.1f dB (-30 by the curve)\n", freeAt / 44100.0, half);
    CHECK(freeAt > samples(0.9) && freeAt <= samples(1.1));
    CHECK(std::fabs(half + 30.0) < 1.5);
    const size_t gone = rel + static_cast<size_t>(std::max(0L, freeAt));
    CHECK(freeAt > 0 && peak(r.L, gone) == 0.0f && peak(r.SL, gone) == 0.0f);
    CHECK(maxStep(r.L) < 0.05f);
}

// Check 2: strum 0.6 s over three notes: they start at 0, 0.3 and 0.6 s, to the sample. Each note's
// own sound is the chord's less the chord without it (play() skips a note out of range but keeps
// the others' places, and on a sine with no breath a voice's sound is the same in any voice), so
// its first non-zero sample is the first where the two differ: a sine from phase 0 is 0 on its
// first sample, so that is one after the note's start. A start still waiting when its owner lets go
// never happens.
void testStrum() {
    std::printf("== bloom: strum\n");
    af::HarmonyPatch h;
    h.strumS = 0.6f;
    {
        const auto take = [&](std::initializer_list<int> notes) {
            Bloom b;
            b.seed(2);
            b.set(plain(), h);
            b.play(chordOf(notes), 60, 1.0f);
            Run r;
            render(b, r, samples(0.8));
            CHECK(b.checkInvariants());
            return r.L;
        };
        const Buf all = take({60, 64, 67});
        const Buf without[3] = {take({-1, 64, 67}), take({60, -1, 67}), take({60, 64, -1})};
        long first[3];
        for (int k = 0; k < 3; ++k) {
            first[k] = -1;
            for (size_t i = 0; i < all.size() && first[k] < 0; ++i)
                if (all[i] != without[k][i]) first[k] = static_cast<long>(i);
            CHECK(first[k] == samples(0.3 * k) + 1);
        }
        std::printf("  first non-zero samples %ld %ld %ld (starts 0, %d, %d)\n", first[0], first[1], first[2],
                    samples(0.3), samples(0.6));
        Bloom b;
        b.set(plain(), h);
        b.play(chordOf({60, 64, 67}), 60, 1.0f);
        CHECK(b.active() == 3);   // all three voices taken at once, two of them waiting
    }
    {
        Bloom b;
        b.seed(2);
        b.set(plain(), h);
        b.play(chordOf({60, 64, 67}), 60, 1.0f);
        Run r;
        render(b, r, samples(0.2));
        b.release(60);
        CHECK(!anyHas(b, 64) && !anyHas(b, 67));
        bool started = false;
        render(b, r, samples(0.6), [&](size_t) { started = started || anyHas(b, 64) || anyHas(b, 67); });
        CHECK(!started);
        CHECK(b.active() == 1);   // 60, releasing
        CHECK(b.checkInvariants());
    }
}

// Check 3: a seventh note takes the oldest voice, which fades out over 3 ms first: no click.
// Then a released voice goes before the oldest held one, the quietest of them first.
void testSteal() {
    std::printf("== bloom: steal\n");
    Bloom b;
    b.seed(3);
    b.set(plain(), af::HarmonyPatch{});
    for (int k = 0; k < 6; ++k) b.play(chordOf({60 + 2 * k}), 60 + 2 * k, 1.0f);
    Run r;
    render(b, r, samples(0.5));
    CHECK(b.active() == 6);
    const size_t at = r.L.size();
    b.play(chordOf({72}), 72, 1.0f);
    CHECK(b.active() == 6);
    render(b, r, kBlk);   // under 3 ms: 60 still fading out, 72 waiting in its voice
    const int stolen = [&] {
        for (int i = 0; i < Bloom::kVoices; ++i)
            if (b.voice(i).stage == Bloom::ST_STEAL) return i;
        return -1;
    }();
    CHECK(stolen >= 0 && b.voice(stolen).note == 60 && b.voice(stolen).next == 72);
    render(b, r, samples(0.1));
    CHECK(b.active() == 6);
    CHECK(!anyHas(b, 60));
    CHECK(voiceOf(b, 72) == stolen);
    for (int k = 1; k < 6; ++k) CHECK(voiceOf(b, 60 + 2 * k) >= 0);
    const float steady = maxStep(r.L, at - static_cast<size_t>(samples(0.3)), at);
    const float around = maxStep(r.L, at, at + static_cast<size_t>(samples(0.02)));
    std::printf("  largest step %.4f (steady %.4f, around the steal %.4f)\n", maxStep(r.L), steady, around);
    CHECK(maxStep(r.L) <= 0.25f && maxStep(r.R) <= 0.25f);
    CHECK(around <= 1.5f * steady);

    // Release 62 now and 66 a second later: 62 is the quieter. The next note takes 62's voice,
    // then 66's, then (none releasing) the oldest held one, 64.
    b.release(62);
    render(b, r, samples(1.0));
    b.release(66);
    render(b, r, samples(0.1));
    const int v62 = voiceOf(b, 62), v66 = voiceOf(b, 66), v64 = voiceOf(b, 64);
    b.play(chordOf({74}), 74, 1.0f);
    render(b, r, samples(0.01));
    CHECK(v62 >= 0 && voiceOf(b, 74) == v62);
    b.play(chordOf({76}), 76, 1.0f);
    render(b, r, samples(0.01));
    CHECK(v66 >= 0 && voiceOf(b, 76) == v66);
    b.play(chordOf({78}), 78, 1.0f);
    render(b, r, samples(0.01));
    CHECK(v64 >= 0 && voiceOf(b, 78) == v64);
    CHECK(b.active() == 6);
    CHECK(maxStep(r.L) <= 0.25f);
    CHECK(b.checkInvariants());
}

// Check 4: moveTo from C major to A minor: 60 and 64 sound on in their voices, untouched (no new
// attack: their envelopes stay at sustain), 67 releases, 69 starts in a voice of its own.
void testMoveTo() {
    std::printf("== bloom: moveTo\n");
    Bloom b;
    b.seed(4);
    b.set(plain(), af::HarmonyPatch{});
    b.moveTo(chordOf({60, 64, 67}), 0.8f);
    Run r;
    render(b, r, samples(0.3));
    const int i60 = voiceOf(b, 60), i64 = voiceOf(b, 64), i67 = voiceOf(b, 67);
    CHECK(i60 >= 0 && i64 >= 0 && i67 >= 0);
    b.moveTo(chordOf({60, 64, 69}), 0.8f);
    float least = 1.0f;
    bool kept = true;
    render(b, r, samples(0.3), [&](size_t) {
        for (int i : {i60, i64}) {
            least = std::min(least, b.voice(i).env);
            kept = kept && b.voice(i).stage == Bloom::ST_SUSTAIN;
        }
    });
    CHECK(kept && least == 1.0f);
    CHECK(b.voice(i60).note == 60 && b.voice(i64).note == 64);
    CHECK(b.voice(i67).note == 67 && b.voice(i67).stage == Bloom::ST_RELEASE);
    const int i69 = voiceOf(b, 69);
    CHECK(i69 >= 0 && i69 != i60 && i69 != i64 && i69 != i67);
    CHECK(i69 >= 0 && b.voice(i69).stage == Bloom::ST_SUSTAIN);
    CHECK(b.active() == 4);
    // 67 comes back while it is still releasing: it swells again in its own voice.
    b.moveTo(chordOf({60, 64, 67}), 0.8f);
    render(b, r, samples(0.1));
    CHECK(voiceOf(b, 67) == i67 && b.voice(i67).stage == Bloom::ST_SUSTAIN);
    CHECK(i69 >= 0 && b.voice(i69).stage == Bloom::ST_RELEASE);
    b.releaseAll();
    render(b, r, kBlk);
    for (int i : {i60, i64, i67}) CHECK(b.voice(i).stage == Bloom::ST_RELEASE);
    CHECK(b.checkInvariants());
    CHECK(maxStep(r.L) < 0.05f);
}

// What one note's release did, from the moment it was released.
struct Released {
    long freeAt = -1;         // samples until active() was 0
    float boost = 0.0f;       // handoffBoost() 0.5 s in
    float boostMax = 0.0f;
    int least9 = 99;          // the fewest voices sounding over the first 9 s
    double sendEnergy = 0.0;  // the send's, L and R, from the release on
    double sendRms = 0.0;     // 0.2..1.5 s in
    double dryLate = 0.0;     // the dry's RMS 1.4..1.5 s in
    float stepBefore = 0.0f;  // the send's largest step: over the 0.3 s before the release,
    float stepAfter = 0.0f;   // after it,
    float stepLast = 0.0f;    // and from 1.45 s in on
    bool silentAfter16 = false;
};
Released releaseOne(float releaseS, int tail, double seconds) {
    Bloom b;
    b.seed(5);
    af::BloomPatch p = plain();
    p.releaseS = releaseS;
    p.tail = tail;
    b.set(p, af::HarmonyPatch{});
    b.play(chordOf({60}), 60, 1.0f);
    Run r;
    render(b, r, samples(0.5));
    const size_t rel = r.L.size();
    b.release(60);
    Released x;
    render(b, r, samples(seconds), [&](size_t at) {
        const size_t t = at + kBlk - rel;
        if (x.freeAt < 0 && b.active() == 0) x.freeAt = static_cast<long>(t);
        if (t <= static_cast<size_t>(samples(0.5))) x.boost = b.handoffBoost();
        x.boostMax = std::max(x.boostMax, b.handoffBoost());
        if (t <= static_cast<size_t>(samples(9.0))) x.least9 = std::min(x.least9, b.active());
    });
    for (size_t i = rel; i < r.SL.size(); ++i)
        x.sendEnergy += static_cast<double>(r.SL[i]) * r.SL[i] + static_cast<double>(r.SR[i]) * r.SR[i];
    x.sendRms = rms(r.SL, rel + samples(0.2), rel + samples(1.5));
    x.dryLate = rms(r.L, rel + samples(1.4), rel + samples(1.5));
    x.stepBefore = maxStep(r.SL, rel - static_cast<size_t>(samples(0.3)), rel);
    x.stepAfter = maxStep(r.SL, rel, r.SL.size());
    x.stepLast = maxStep(r.SL, rel + static_cast<size_t>(samples(1.45)), r.SL.size());
    const size_t gone = rel + static_cast<size_t>(samples(1.6));
    x.silentAfter16 = gone < r.L.size() && peak(r.L, gone) == 0.0f && peak(r.SL, gone) == 0.0f;
    return x;
}

// Check 5: the tail handoff. With Tail Space the voice is free 1.5 s after its release and its
// send carries the same energy into Space as Tail Voice's whole release does (+-1 dB: CONCEPT.md
// 8), at Release 2, 10 and 30 s, never stepping. At Release 10 its send from 0.2 to 1.5 s is still
// the larger, and its dry is all but gone at 1.5 s; Tail Voice keeps the voice for the whole 10 s.
void testHandoff() {
    std::printf("== bloom: tail handoff\n");
    for (float releaseS : {2.0f, 10.0f, 30.0f}) {
        const Released space = releaseOne(releaseS, af::TL_SPACE, 2.0);
        const Released voice = releaseOne(releaseS, af::TL_VOICE, releaseS + 0.2);
        const double energy = 10.0 * std::log10(space.sendEnergy / voice.sendEnergy);
        std::printf("  Release %4.1f: Space free after %.3f s, boost %.3f, send energy %+.2f dB of Voice's;"
                    " Voice free after %.3f s\n",
                    releaseS, space.freeAt / 44100.0, space.boost, energy, voice.freeAt / 44100.0);
        CHECK(space.freeAt > samples(1.4) && space.freeAt <= samples(1.6));
        CHECK(space.silentAfter16);
        CHECK(std::fabs(energy) <= 1.0);
        CHECK(space.boost >= 1.0f && space.boost <= 4.0f && space.boostMax <= space.boost + 1e-6f);
        CHECK(voice.boostMax == 1.0f);
        CHECK(voice.freeAt > samples(0.9 * releaseS) && voice.freeAt <= samples(releaseS + 0.1));
        // The send never steps: after the release it grows by the boost at most, and over the
        // last 50 ms before the voice goes it has faded to a fraction of the sustain's (without
        // the fade-out it would stop from 0.4 of it at Release 10).
        std::printf("    send's largest step %.4f after the release, %.4f before, %.4f in the last 50 ms\n",
                    space.stepAfter, space.stepBefore, space.stepLast);
        CHECK(space.stepAfter <= 1.05f * space.boost * space.stepBefore);
        CHECK(space.stepLast <= 0.2f * space.stepBefore);
        if (releaseS == 10.0f) {
            std::printf("    send RMS 0.2..1.5 s: Space %.4f, Voice %.4f; dry 1.4..1.5 s: Space %.5f, Voice %.4f;"
                        " Voice: at least %d voice for 9 s\n",
                        space.sendRms, voice.sendRms, space.dryLate, voice.dryLate, voice.least9);
            CHECK(space.sendRms > voice.sendRms);
            CHECK(space.dryLate < 0.05 * voice.dryLate);
            CHECK(voice.least9 >= 1);
        }
    }
    // With the send closed there is no Space to hand the tail to: Tail Space releases as Voice.
    Bloom b;
    b.seed(5);
    af::BloomPatch p = plain();
    p.releaseS = 10.0f;
    p.tail = af::TL_SPACE;
    b.set(p, af::HarmonyPatch{});
    b.play(chordOf({60}), 60, 1.0f);
    Run r;
    render(b, r, samples(0.5), [](size_t) {}, 0.0f);
    b.release(60);
    const size_t rel = r.L.size();
    float boost = 0.0f;
    render(b, r, samples(2.0), [&](size_t) { boost = std::max(boost, b.handoffBoost()); }, 0.0f);
    const int v = voiceOf(b, 60);
    const double late = db(rms(r.L, rel + samples(1.9), rel + samples(2.0)) / rms(r.L, rel - samples(0.1), rel));
    std::printf("  send 0: still releasing 2 s on (%.1f dB; Tail Voice's curve -11.7), boost %.3f\n", late, boost);
    CHECK(v >= 0 && b.voice(v).stage == Bloom::ST_RELEASE);
    CHECK(boost == 1.0f && peak(r.SL) == 0.0f);
    CHECK(std::fabs(late + 11.7) < 1.0);   // -60 dB over 10 s, at 1.95 s
}

// Check 6: Swell 2 s: at 1 s the voice is half way, -6 dB under its sustain (+-1.5 dB); at 0.5 s
// the S-curve has 0.5 - 0.5 cos(pi / 4) = 0.146, -16.7 dB.
void testSwell() {
    std::printf("== bloom: swell\n");
    Bloom b;
    b.seed(6);
    af::BloomPatch p = plain();
    p.swellS = 2.0f;
    b.set(p, af::HarmonyPatch{});
    b.play(chordOf({60}), 60, 1.0f);
    Run r;
    render(b, r, samples(3.0));
    const double sus = magnitude(r.L, kC4, static_cast<size_t>(samples(2.5)), static_cast<size_t>(samples(3.0)));
    const auto around = [&](double t) {   // 50 ms around t, against the sustain
        const size_t from = static_cast<size_t>(samples(t - 0.025)), to = static_cast<size_t>(samples(t + 0.025));
        return db(magnitude(r.L, kC4, from, to) / sus);
    };
    const double at1 = around(1.0), at05 = around(0.5);
    std::printf("  at 1 s %.2f dB, at 0.5 s %.2f dB\n", at1, at05);
    CHECK(std::fabs(at1 + 6.02) <= 1.5);
    CHECK(std::fabs(at05 + 16.7) <= 1.5);
}

// Check 7: velocity. Vel 0: velocities 1 and 127 sound the same. Vel 1: the level follows the
// velocity, linear in amplitude (+-1 dB).
void testVelocity() {
    std::printf("== bloom: velocity\n");
    const auto level = [](float sens, float vel) {
        Bloom b;
        b.seed(7);
        af::BloomPatch p = plain();
        p.velSens = sens;
        b.set(p, af::HarmonyPatch{});
        b.play(chordOf({60}), 60, vel);
        Run r;
        render(b, r, samples(0.3));
        return rms(r.L, static_cast<size_t>(samples(0.2)));
    };
    const double flat = db(level(0.0f, 1.0f / 127.0f) / level(0.0f, 1.0f));
    const double half = db(level(1.0f, 0.5f) / level(1.0f, 1.0f));
    const double low = db(level(1.0f, 1.0f / 127.0f) / level(1.0f, 1.0f));
    std::printf("  Vel 0: 1 vs 127 %.3f dB; Vel 1: 64 vs 127 %.2f dB (-6.02), 1 vs 127 %.2f dB (-42.08)\n", flat, half, low);
    CHECK(std::fabs(flat) < 0.01);
    CHECK(std::fabs(half + 6.02) <= 1.0);
    CHECK(std::fabs(low + 42.08) <= 1.0);
}

// Check 8: the worst knobs for 30 s (LP; BP and HP 5 s): Reso 1 at Tone 40 and 20000, FM at full,
// unison 2 detuned 50 cents, notes 24 and 108 on Felt Piano. Finite, peak 4 at most. Then Breath 1
// on top, just finite.
void testStability() {
    std::printf("== bloom: stability\n");
    float worst = 0.0f;
    bool finite = true;
    const auto play = [&](float cutoff, int mode, float breath, double seconds) {
        Bloom b;
        b.seed(8);
        af::BloomPatch p;
        p.reso = 1.0f;
        p.cutoffHz = cutoff;
        p.filterMode = mode;
        p.couple = af::CP_FM;
        p.coupleAmt = 1.0f;
        p.unison = 2;
        p.detuneCents = 50.0f;
        p.breath = breath;
        p.level = 1.0f;
        p.velSens = 0.0f;
        b.set(p, af::HarmonyPatch{});
        b.play(chordOf({24, 108}), 24, 1.0f);
        float L[kBlk], R[kBlk], SL[kBlk], SR[kBlk];
        float top = 0.0f;
        for (int done = 0; done < samples(seconds); done += kBlk) {
            std::fill(L, L + kBlk, 0.0f);
            std::fill(R, R + kBlk, 0.0f);
            std::fill(SL, SL + kBlk, 0.0f);
            std::fill(SR, SR + kBlk, 0.0f);
            b.render(tables(), L, R, SL, SR, 1.0f, kBlk);
            for (int i = 0; i < kBlk; ++i)
                for (float x : {L[i], R[i], SL[i], SR[i]}) {
                    if (!std::isfinite(x)) finite = false;
                    else top = std::max(top, std::fabs(x));
                }
        }
        return top;
    };
    for (float cutoff : {40.0f, 20000.0f})
        for (int mode : {af::FM_LP, af::FM_BP, af::FM_HP}) {
            const float top = play(cutoff, mode, 0.05f, mode == af::FM_LP ? 30.0 : 5.0);
            std::printf("  Tone %5.0f %s: peak %.3f\n", cutoff, mode == af::FM_LP ? "LP" : mode == af::FM_BP ? "BP" : "HP", top);
            worst = std::max(worst, top);
        }
    CHECK(finite);
    CHECK(worst <= 4.0f);
    for (float cutoff : {40.0f, 20000.0f})
        std::printf("  Breath 1, Tone %5.0f LP: peak %.3f\n", cutoff, play(cutoff, af::FM_LP, 1.0f, 5.0));
    CHECK(finite);
}

// Owners: two chords sharing notes. The shared notes sound once, until both keys are up.
void testOwners() {
    std::printf("== bloom: owners\n");
    Bloom b;
    b.seed(9);
    b.set(plain(), af::HarmonyPatch{});
    b.play(chordOf({60, 64, 67}), 60, 1.0f);
    Run r;
    render(b, r, samples(0.1));
    b.play(chordOf({64, 67, 71}), 64, 1.0f);
    render(b, r, samples(0.1));
    CHECK(b.active() == 4);
    b.release(64);   // 71 goes; 64 and 67 stay, 60 still holds them
    render(b, r, samples(0.1));
    CHECK(b.voice(voiceOf(b, 71)).stage == Bloom::ST_RELEASE);
    for (int n : {60, 64, 67}) CHECK(voiceOf(b, n) >= 0 && b.voice(voiceOf(b, n)).stage == Bloom::ST_SUSTAIN);
    b.release(60);
    render(b, r, kBlk);
    for (int n : {60, 64, 67}) CHECK(voiceOf(b, n) >= 0 && b.voice(voiceOf(b, n)).stage == Bloom::ST_RELEASE);
    // A note played again while it releases swells again in the same voice, from where it was
    // (a slower swell, so the block after it is still in the attack).
    const int v = voiceOf(b, 64);
    render(b, r, samples(0.5));
    af::BloomPatch slow = plain();
    slow.swellS = 0.5f;
    b.set(slow, af::HarmonyPatch{});
    const float was = b.voice(v).env;
    b.play(chordOf({64}), 64, 1.0f);
    render(b, r, kBlk);
    CHECK(voiceOf(b, 64) == v && b.voice(v).stage == Bloom::ST_ATTACK && b.voice(v).env >= was);
    CHECK(b.active() == 4);
    CHECK(maxStep(r.L) < 0.05f);
    CHECK(b.checkInvariants());
    // Owners out of range count as -1: release(-1) lets go of what owner 1000 started.
    b.releaseAll();
    b.play(chordOf({72}), 1000, 1.0f);
    render(b, r, samples(0.6));   // swelled in: a release from under -60 dB would end at once
    b.release(-1);
    render(b, r, kBlk);
    CHECK(voiceOf(b, 72) >= 0 && b.voice(voiceOf(b, 72)).stage == Bloom::ST_RELEASE);
    CHECK(b.checkInvariants());
}

// The analog prototypes' gains at r = f / cutoff, for Q: what the SVF should give well under
// Nyquist (its prewarp is exact only at the cutoff).
double lpDb(double r, double q) { return db(1.0 / std::hypot(1.0 - r * r, r / q)); }
double hpDb(double r, double q) { return db(r * r / std::hypot(1.0 - r * r, r / q)); }
double bpDb(double r, double q) { return db(r / std::hypot(1.0 - r * r, r / q)); }   // peak Q at r = 1

// The filter and the breath, on a saw at note 48 against Tone 20000: LP, HP and BP take its
// harmonics where the analog filters would (+-1 dB); BP's peak is Q (LP's resonant peak), its
// skirts fall 6 dB an octave. Breath fills the space between the harmonics.
void testTone() {
    std::printf("== bloom: tone and breath\n");
    const double f = 130.8127827;   // note 48
    const auto take = [&](float cutoff, int mode, float reso, float breath) {
        Bloom b;
        b.seed(10);
        af::BloomPatch p = plain();
        p.table = af::TB_SAW;
        p.cutoffHz = cutoff;
        p.filterMode = mode;
        p.reso = reso;
        p.breath = breath;
        b.set(p, af::HarmonyPatch{});
        b.play(chordOf({48}), 48, 1.0f);
        Run r;
        render(b, r, samples(0.6));
        return r.L;
    };
    const size_t from = static_cast<size_t>(samples(0.1));
    const double q01 = 0.5 * std::pow(32.0, 0.1), q05 = 0.5 * std::pow(32.0, 0.5);   // Reso 0.1, 0.5
    const Buf open = take(20000.0f, af::FM_LP, 0.0f, 0.0f), lp = take(200.0f, af::FM_LP, 0.1f, 0.0f);
    const Buf hp = take(2000.0f, af::FM_HP, 0.1f, 0.0f), bp = take(4.0f * static_cast<float>(f), af::FM_BP, 0.5f, 0.0f);
    const auto rel = [&](const Buf& x, int h) { return db(magnitude(x, h * f, from) / magnitude(open, h * f, from)); };
    std::printf("  LP 200: h1 %.2f (%.2f) h10 %.2f (%.2f); HP 2k: h1 %.2f (%.2f) h20 %.2f (%.2f) dB\n", rel(lp, 1),
                lpDb(f / 200.0, q01), rel(lp, 10), lpDb(10 * f / 200.0, q01), rel(hp, 1), hpDb(f / 2000.0, q01), rel(hp, 20),
                hpDb(20 * f / 2000.0, q01));
    std::printf("  BP at h4, Reso 0.5: h4 %.2f (%.2f) h1 %.2f (%.2f) dB\n", rel(bp, 4), bpDb(1.0, q05), rel(bp, 1), bpDb(0.25, q05));
    CHECK(std::fabs(rel(lp, 1) - lpDb(f / 200.0, q01)) < 1.0);
    CHECK(std::fabs(rel(lp, 10) - lpDb(10 * f / 200.0, q01)) < 1.0);
    CHECK(std::fabs(rel(hp, 1) - hpDb(f / 2000.0, q01)) < 1.0);
    CHECK(std::fabs(rel(hp, 20) - hpDb(20 * f / 2000.0, q01)) < 1.0);
    CHECK(std::fabs(rel(bp, 4) - bpDb(1.0, q05)) < 1.0);
    CHECK(std::fabs(rel(bp, 1) - bpDb(0.25, q05)) < 1.0);
    const Buf breathy = take(20000.0f, af::FM_LP, 0.0f, 1.0f);
    const double gap = db(magnitude(breathy, 4.5 * f, from) / std::max(1e-9, magnitude(open, 4.5 * f, from)));
    std::printf("  Breath 1: between h4 and h5 %.1f dB over none; RMS %.3f vs %.3f\n", gap, rms(breathy, from), rms(open, from));
    CHECK(gap > 20.0);
    CHECK(rms(breathy, from) > rms(open, from));
    CHECK(allFinite(breathy));
}

// Unison. Switched while a chord sounds, 2 to 1 and back, the second half glides out and in: no
// step bigger than the steady sound's (a half dropped at once stepped 0.148 against 0.022). On rich
// tables (Saw, Felt Piano): below 1 cent of Detune unison 2 is unison 1 exactly (two halves a fixed
// phase apart would be a comb); at the default 8 cents it is as loud as unison 1 over 2 s (+-1 dB),
// and the breath, the same in both halves, is as loud too (+-0.5 dB).
void testUnison() {
    std::printf("== bloom: unison\n");
    {
        Bloom b;
        b.seed(13);
        af::BloomPatch p = plain();
        p.unison = 2;
        p.width = 0.6f;
        b.set(p, af::HarmonyPatch{});
        b.play(chordOf({60, 64, 67}), 60, 1.0f);
        Run r;
        render(b, r, samples(0.5));
        const size_t one = r.L.size();
        p.unison = 1;
        b.set(p, af::HarmonyPatch{});
        render(b, r, samples(0.3));
        const size_t two = r.L.size();
        p.unison = 2;
        b.set(p, af::HarmonyPatch{});
        render(b, r, samples(0.3));
        const auto step = [&](size_t from, size_t to) {
            return std::max(maxStep(r.L, from, to), maxStep(r.R, from, to));
        };
        const size_t ms20 = static_cast<size_t>(samples(0.02));
        const float steady = std::max(step(one - 10 * ms20, one), step(one + 5 * ms20, two));
        const float out = step(one, one + ms20), in = step(two, two + ms20);
        std::printf("  2 -> 1: largest step %.4f, 1 -> 2: %.4f (steady %.4f)\n", out, in, steady);
        CHECK(out <= 1.5f * steady && in <= 1.5f * steady);
        CHECK(b.active() == 3);
    }
    // A chord on `table`, 2.5 s: L, R and the send.
    const auto take = [](int table, int unison, float detune, float breath, float width = 0.6f) {
        Bloom b;
        b.seed(19);
        af::BloomPatch p = plain();
        p.table = table;
        p.unison = unison;
        p.detuneCents = detune;
        p.breath = breath;
        p.width = width;
        b.set(p, af::HarmonyPatch{});
        b.play(chordOf({48, 55, 64}), 48, 1.0f);
        Run r;
        render(b, r, samples(2.5));
        return r;
    };
    const auto power = [](const Run& r) {   // L and R over the last 2 s
        const size_t from = static_cast<size_t>(samples(0.5));
        const double l = rms(r.L, from), rr = rms(r.R, from);
        return std::sqrt(0.5 * (l * l + rr * rr));
    };
    for (int table : {af::TB_SAW, af::TB_FELT_PIANO}) {
        const Run one = take(table, 1, 8.0f, 0.0f), zero = take(table, 2, 0.0f, 0.0f), low = take(table, 2, 0.9f, 0.0f);
        const bool same = zero.L == one.L && zero.R == one.R && zero.SL == one.SL && low.L == one.L;
        const double at8 = db(power(take(table, 2, 8.0f, 0.0f)) / power(one));
        std::printf("  %s: Detune 0 and 0.9 %s unison 1; 8 cents %+.2f dB of it over 2 s\n",
                    table == af::TB_SAW ? "Saw" : "Felt Piano", same ? "are exactly" : "differ from", at8);
        CHECK(same);
        CHECK(std::fabs(at8) <= 1.0);
    }
    // The breath alone, away from the tones (sines), at width 0: its bands at 1.25 and 0.75 times
    // the root.
    const auto breath = [&](int unison) {
        const Run r = take(af::TB_SINE, unison, 8.0f, 1.0f, 0.0f);
        const size_t from = static_cast<size_t>(samples(0.5));
        const double f = 130.8127827, a = magnitude(r.L, 1.25 * f, from), c = magnitude(r.L, 0.75 * f, from);
        return std::sqrt(a * a + c * c);
    };
    const double breathDb = db(breath(2) / breath(1));
    std::printf("  breath at unison 2 %+.3f dB of unison 1's\n", breathDb);
    CHECK(std::fabs(breathDb) <= 0.5);
}

// L and R together: their RMS over [from, to).
double stereoRms(const Run& r, size_t from, size_t to) {
    const double l = rms(r.L, from, to), rr = rms(r.R, from, to);
    return std::sqrt(0.5 * (l * l + rr * rr));
}

// A note pressed again while it hands its tail to Space swells again in its voice from where it
// is. The default patch (Felt Piano, Tail Space, Release 6, Swell 2.5): pressed again 150 ms after
// its release the dry dips no more than 1 dB (a fresh voice from zero under the fading one dipped
// 11.8 dB), and the note keeps its one voice. A key pressed 12 times, 150 ms apart, keeps to one
// voice; Harmony's C -> Am -> C within 1.5 s takes 67 up again in its voice from where it was.
void testPressedAgain() {
    std::printf("== bloom: pressed again\n");
    {
        Bloom b;
        b.seed(14);
        b.set(af::BloomPatch{}, af::HarmonyPatch{});
        b.play(chordOf({60}), 60, 1.0f);
        Run r;
        render(b, r, samples(3.0));
        b.release(60);
        render(b, r, samples(0.15));
        const int v = voiceOf(b, 60);
        CHECK(v >= 0 && b.voice(v).stage == Bloom::ST_HANDOFF);
        const size_t at = r.L.size(), w = static_cast<size_t>(samples(0.02));
        b.play(chordOf({60}), 60, 1.0f);
        render(b, r, samples(0.6));
        CHECK(b.active() == 1 && voiceOf(b, 60) == v);
        CHECK(b.checkInvariants());
        const double before = stereoRms(r, at - w, at);
        double least = 1e9;
        for (size_t t = at; t + w <= at + static_cast<size_t>(samples(0.5)); t += w / 2)
            least = std::min(least, stereoRms(r, t, t + w));
        // No click either, in the dry or the send (whose handoff boost goes back to 1 over 3 ms).
        const size_t ms300 = static_cast<size_t>(samples(0.3));
        const float dry = maxStep(r.L, at - ms300, at), send = maxStep(r.SL, at - ms300, at);
        const float dryAt = maxStep(r.L, at - w, at + w), sendAt = maxStep(r.SL, at - w, at + w);
        std::printf("  pressed again 150 ms into the handoff: dips %.2f dB, %d voice; steps %.4f / %.4f (dry / send, "
                    "%.4f / %.4f before)\n",
                    db(least / before), b.active(), dryAt, sendAt, dry, send);
        CHECK(db(least / before) > -1.0);
        CHECK(dryAt <= 1.5f * dry && sendAt <= 1.5f * send);
    }
    {
        // Late in the handoff (1.2 s: the dry down to 0.1, the send still at its boost) the send
        // falls back to the dry over 3 ms, slower than a low sine moves: its largest step stays
        // under the steady send's (0.8 of it; over one 32-sample control step it went over 1.2).
        Bloom b;
        b.seed(14);
        af::BloomPatch p = plain();   // a sine: steady steps small against a step in the send
        p.tail = af::TL_SPACE;
        p.swellS = 2.5f;              // so that, pressed again, it stays about as loud for a while
        b.set(p, af::HarmonyPatch{});
        b.play(chordOf({36}), 36, 1.0f);
        Run r;
        render(b, r, samples(3.0));
        b.release(36);
        render(b, r, samples(1.2));
        const size_t at = r.L.size(), w = static_cast<size_t>(samples(0.01));
        b.play(chordOf({36}), 36, 1.0f);
        render(b, r, samples(0.1));
        const float send = maxStep(r.SL, at - 10 * w, at), sendAt = maxStep(r.SL, at, at + w);
        std::printf("  pressed again 1.2 s into the handoff: send's step %.5f (%.5f before)\n", sendAt, send);
        CHECK(sendAt <= 1.2f * send);
        CHECK(b.active() == 1 && b.checkInvariants());
    }
    {
        Bloom b;
        b.seed(15);
        b.set(af::BloomPatch{}, af::HarmonyPatch{});
        Run r;
        int most = 0, first = -1;
        bool same = true;
        for (int k = 0; k < 12; ++k) {
            b.play(chordOf({60}), 60, 1.0f);
            render(b, r, samples(0.075), [&](size_t) { most = std::max(most, b.active()); });
            const int v = voiceOf(b, 60);
            if (first < 0) first = v;
            same = same && v == first;
            b.release(60);
            render(b, r, samples(0.075), [&](size_t) { most = std::max(most, b.active()); });
            CHECK(b.checkInvariants());
        }
        std::printf("  12 presses 150 ms apart: %d voice at most, %s\n", most,
                    same ? "always the same" : "not the same");
        CHECK(most == 1 && same);
    }
    {
        Bloom b;
        b.seed(16);
        b.set(af::BloomPatch{}, af::HarmonyPatch{});
        b.moveTo(chordOf({60, 64, 67}), 0.8f);
        Run r;
        render(b, r, samples(3.0));
        const int i67 = voiceOf(b, 67);
        b.moveTo(chordOf({60, 64, 69}), 0.8f);
        render(b, r, samples(0.5));
        CHECK(i67 >= 0 && b.voice(i67).stage == Bloom::ST_HANDOFF);
        const float was = i67 >= 0 ? b.voice(i67).env : 0.0f;
        b.moveTo(chordOf({60, 64, 67}), 0.8f);
        render(b, r, kBlk);
        const float now = i67 >= 0 ? b.voice(i67).env : 0.0f;
        std::printf("  C -> Am -> C: 67 back in its voice at %.3f (its release had %.3f, its dry %.3f)\n", now, was,
                    was * 0.75f);
        CHECK(voiceOf(b, 67) == i67 && b.voice(i67).stage == Bloom::ST_ATTACK);
        CHECK(now >= 0.74f * was);   // the handoff's dry fade at 0.5 s, cos^2(pi / 6) = 0.75, folded in
        CHECK(b.active() == 4);      // and 69 handing off
        CHECK(b.checkInvariants());
    }
}

// Re-voicing a held chord, play(new) before release(old): the notes the chords share carry on
// untouched in their voices, 4 voices for C E G -> E G B. The other way round they swell again in
// their voices, 4 as well.
void testRevoice() {
    std::printf("== bloom: re-voicing\n");
    for (int playFirst = 1; playFirst >= 0; --playFirst) {
        Bloom b;
        b.seed(17);
        b.set(af::BloomPatch{}, af::HarmonyPatch{});
        b.play(chordOf({60, 64, 67}), 60, 1.0f);
        Run r;
        render(b, r, samples(3.0));
        const int e = voiceOf(b, 64), g = voiceOf(b, 67);
        if (playFirst) {
            b.play(chordOf({64, 67, 71}), 64, 1.0f);
            b.release(60);
        } else {
            b.release(60);
            b.play(chordOf({64, 67, 71}), 64, 1.0f);
        }
        render(b, r, kBlk);
        std::printf("  %s: %d voices\n", playFirst ? "play, then release" : "release, then play", b.active());
        CHECK(b.active() == 4);
        CHECK(voiceOf(b, 64) == e && voiceOf(b, 67) == g);
        if (playFirst) CHECK(b.voice(e).stage == Bloom::ST_SUSTAIN && b.voice(g).stage == Bloom::ST_SUSTAIN);
        CHECK(b.checkInvariants());
    }
}

// A new Table (or Table B) while a chord sounds fades in over 20 ms in every voice: no step bigger
// than the steady sound's on either side. Low notes, whose waves move little from one sample to the
// next: switched at once, the sound steps as far as the two waves differ at that phase.
void testTableChange() {
    std::printf("== bloom: table change\n");
    for (int which = 0; which < 2; ++which) {
        Bloom b;
        b.seed(20);
        af::BloomPatch p = plain();
        p.table = af::TB_SINE;
        p.tableB = af::TB_SINE;
        p.blend = which == 1 ? 0.6f : 0.0f;
        b.set(p, af::HarmonyPatch{});
        b.play(chordOf({36, 40, 43}), 36, 1.0f);
        Run r;
        render(b, r, samples(0.5));
        const size_t at = r.L.size();
        (which == 0 ? p.table : p.tableB) = af::TB_FELT_PIANO;
        b.set(p, af::HarmonyPatch{});
        render(b, r, samples(0.3));
        const size_t ms30 = static_cast<size_t>(samples(0.03));
        const float steady = std::max(maxStep(r.L, at - 10 * ms30, at), maxStep(r.L, at + 2 * ms30, r.L.size()));
        const float around = maxStep(r.L, at, at + ms30);
        std::printf("  %s: largest step %.4f around the change (steady %.4f)\n", which == 0 ? "Table" : "Table B",
                    around, steady);
        CHECK(around <= 1.5f * steady);
        CHECK(b.checkInvariants());
    }
}

// Odd input: NaN, infinities and values out of range everywhere. Nothing goes non-finite, the
// state holds, and a sane patch plays as ever afterwards.
void testOddInput() {
    std::printf("== bloom: odd input\n");
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    Bloom b;
    b.seed(18);
    af::BloomPatch p;
    p.level = 2.0f;
    p.cutoffHz = nan;
    p.reso = inf;
    p.filterMode = 7;
    p.table = -3;
    p.pos = {nan, inf, -inf, nan};
    p.tableB = 99;
    p.bOctave = -9;
    p.blend = nan;
    p.couple = 42;
    p.coupleAmt = inf;
    p.unison = 9;
    p.detuneCents = inf;
    p.swellS = nan;
    p.releaseS = -inf;
    p.velSens = nan;
    p.breath = inf;
    p.tail = 5;
    p.width = -nan;
    af::HarmonyPatch h;
    h.key = 99;
    h.tuning = -4;
    h.strumS = nan;
    b.set(p, h);
    af::Chord c;
    c.n = 99;
    const int notes[af::kChordMax] = {-5, 200, 60, 64, 128, 67};
    std::copy(notes, notes + af::kChordMax, c.notes);
    b.play(c, 1000, nan);
    CHECK(b.checkInvariants());
    std::vector<float> L(300, 0.0f), R(300, 0.0f), SL(300, 0.0f), SR(300, 0.0f);
    bool finite = true;
    const auto run = [&](float send, int n) {
        b.render(tables(), L.data(), R.data(), SL.data(), SR.data(), send, n);
        finite = finite && allFinite(L) && allFinite(R) && allFinite(SL) && allFinite(SR);
    };
    run(nan, 300);
    run(inf, 0);
    run(-inf, -4);
    b.release(-7);
    b.moveTo(c, inf);
    run(0.5f, 300);
    b.release(500);
    b.releaseAll();
    run(-1.0f, 300);
    CHECK(b.checkInvariants());
    CHECK(finite);
    CHECK(b.active() <= Bloom::kVoices);
    b.set(plain(), af::HarmonyPatch{});
    b.play(chordOf({60}), 60, 1.0f);
    Run r;
    render(b, r, samples(0.3));
    CHECK(allFinite(r.L) && rms(r.L, static_cast<size_t>(samples(0.2))) > 0.05);
    CHECK(b.checkInvariants());
}

// Width: 0 puts everything in the middle (L = R); unison 2 at width 1 spreads its halves apart.
// Mute and Level 0 are silence, and voices still come free under them.
void testWidthAndMute() {
    std::printf("== bloom: width, mute\n");
    const auto take = [](int unison, float width, bool mute, float level, Bloom* keep = nullptr) {
        Bloom local;
        Bloom& b = keep ? *keep : local;
        b.seed(11);
        af::BloomPatch p = plain();
        p.unison = unison;
        p.width = width;
        p.mute = mute;
        p.level = level;
        p.releaseS = 0.2f;
        b.set(p, af::HarmonyPatch{});
        b.play(chordOf({60, 64, 67}), 60, 1.0f);
        Run r;
        render(b, r, samples(0.3));
        b.release(60);
        render(b, r, samples(0.3));
        return r;
    };
    const Run mid = take(2, 0.0f, false, 1.0f);
    CHECK(mid.L == mid.R && rms(mid.L) > 0.05);
    const Run wide = take(2, 1.0f, false, 1.0f);
    const double lr = rms(wide.L) / rms(wide.R), diff = [&] {
        Buf d(wide.L.size());
        for (size_t i = 0; i < d.size(); ++i) d[i] = wide.L[i] - wide.R[i];
        return rms(d) / rms(wide.L);
    }();
    std::printf("  unison 2, width 1: L/R RMS %.3f, L-R %.2f of L\n", lr, diff);
    CHECK(std::fabs(db(lr)) < 3.0 && diff > 0.3);
    for (int muted = 0; muted < 2; ++muted) {
        Bloom b;
        const Run r = take(1, 0.6f, muted == 1, muted ? 1.0f : 0.0f, &b);
        CHECK(peak(r.L) == 0.0f && peak(r.R) == 0.0f && peak(r.SL) == 0.0f && peak(r.SR) == 0.0f);
        CHECK(b.active() == 0);
    }
}

// The same seed and calls give the same output to the bit (reset() too); another seed differs.
void testDeterminism() {
    std::printf("== bloom: determinism\n");
    const auto take = [](uint32_t seed, Bloom* reuse = nullptr) {
        Bloom local;
        Bloom& b = reuse ? *reuse : local;
        if (!reuse) b.seed(seed);
        af::BloomPatch p;
        p.unison = 2;
        p.breath = 0.3f;
        p.pos.smear = 1.0f;
        p.swellS = 0.2f;
        af::HarmonyPatch h;
        h.strumS = 0.3f;
        b.set(p, h);
        b.play(chordOf({48, 55, 64, 71}), 48, 0.7f);
        Run r;
        render(b, r, samples(1.0));
        b.moveTo(chordOf({50, 57, 65}), 0.9f);
        render(b, r, samples(1.0));
        b.releaseAll();
        render(b, r, samples(0.5));
        Buf all = r.L;
        all.insert(all.end(), r.SR.begin(), r.SR.end());
        return all;
    };
    const Buf a = take(7), c = take(8);
    Bloom b;
    b.seed(7);
    const Buf first = take(0, &b);
    b.reset();
    const Buf again = take(0, &b);
    CHECK(a == first);
    CHECK(a == again);
    CHECK(a != c);
    CHECK(allFinite(a) && rms(a) > 0.01);
}

#if BLOOM_COUNTS_ALLOCS
// ASan's allocator calls these for every allocation; only the test's own thread counts.
thread_local bool t_counting = false;
int g_allocs = 0;
void onMalloc(const volatile void*, size_t) {
    if (t_counting) ++g_allocs;
}
void onFree(const volatile void*) {}
#endif

// Nothing in play, moveTo, release, set or render allocates.
void testNoAllocation() {
    std::printf("== bloom: no allocation\n");
#if BLOOM_COUNTS_ALLOCS
    static const bool hooked = __sanitizer_install_malloc_and_free_hooks(onMalloc, onFree) != 0;
    CHECK(hooked);
    tables();
    Bloom b;
    b.seed(12);
    float L[kBlk] = {}, R[kBlk] = {}, SL[kBlk] = {}, SR[kBlk] = {};
    af::BloomPatch p;
    p.unison = 2;
    p.couple = af::CP_RING;
    p.coupleAmt = 0.5f;
    p.blend = 0.3f;
    af::HarmonyPatch h;
    h.strumS = 0.1f;
    t_counting = true;
    g_allocs = 0;
    b.set(p, h);
    b.play(chordOf({48, 52, 55, 59, 62, 66}), 48, 0.8f);
    for (int i = 0; i < 100; ++i) b.render(tables(), L, R, SL, SR, 0.5f, kBlk);
    b.moveTo(chordOf({50, 53, 57}), 0.5f);
    p.cutoffHz = 300.0f;
    p.filterMode = af::FM_HP;
    h.tuning = af::TU_EQUAL;
    b.set(p, h);
    for (int i = 0; i < 100; ++i) b.render(tables(), L, R, SL, SR, 0.5f, 77);
    b.release(48);
    b.releaseAll();
    b.play(chordOf({60}), 60, 1.0f);
    for (int i = 0; i < 400; ++i) b.render(tables(), L, R, SL, SR, 0.5f, kBlk);
    b.reset();
    t_counting = false;
    std::printf("  %d allocations\n", g_allocs);
    CHECK(g_allocs == 0);
#else
    std::printf("  (counted under ASan: make test)\n");
#endif
}

} // namespace

void bloomTests() {
    testChord();
    testStrum();
    testSteal();
    testMoveTo();
    testHandoff();
    testSwell();
    testVelocity();
    testStability();
    testOwners();
    testTone();
    testUnison();
    testPressedAgain();
    testRevoice();
    testTableChange();
    testOddInput();
    testWidthAndMute();
    testDeterminism();
    testNoAllocation();
}

} // namespace aft
