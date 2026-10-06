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
        c.pcs = static_cast<uint16_t>(c.pcs | 1u << (n % 12));
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

// Check 2: strum 0.6 s over three notes: they start at 0, 0.3 and 0.6 s (+-1 block: the first block
// whose end finds a voice sounding holds its first non-zero sample). A start still waiting when its
// owner lets go never happens.
void testStrum() {
    std::printf("== bloom: strum\n");
    af::HarmonyPatch h;
    h.strumS = 0.6f;
    {
        Bloom b;
        b.seed(2);
        b.set(plain(), h);
        b.play(chordOf({60, 64, 67}), 60, 1.0f);
        CHECK(b.active() == 3);   // all three voices taken at once, two of them waiting
        const int notes[3] = {60, 64, 67};
        long onset[3] = {-1, -1, -1};
        Run r;
        render(b, r, samples(0.8), [&](size_t at) {
            for (int k = 0; k < 3; ++k) {
                const int v = voiceOf(b, notes[k]);
                if (onset[k] < 0 && v >= 0 && b.voice(v).env > 0.0f) onset[k] = static_cast<long>(at);
            }
        });
        std::printf("  onsets %.4f %.4f %.4f s\n", onset[0] / 44100.0, onset[1] / 44100.0, onset[2] / 44100.0);
        for (int k = 0; k < 3; ++k) CHECK(onset[k] >= 0 && std::labs(onset[k] - samples(0.3 * k)) < kBlk);
        CHECK(b.active() == 3);
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
// step bigger than the steady sound's (a half dropped at once stepped 0.148 against 0.022). At
// Detune 0 the halves sit a quarter cycle apart, as loud as unison 1 (+-0.5 dB) whatever the seed,
// and the breath, the same in both halves, is as loud at unison 2 as at 1.
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
    const auto level = [](int unison, uint32_t seed, float breath, double hz) {
        Bloom b;
        b.seed(seed);
        af::BloomPatch p = plain();
        p.unison = unison;
        p.detuneCents = 0.0f;
        p.breath = breath;
        b.set(p, af::HarmonyPatch{});
        b.play(chordOf({60}), 60, 1.0f);
        Run r;
        render(b, r, samples(0.6));
        const size_t from = static_cast<size_t>(samples(0.1));
        return hz > 0.0 ? magnitude(r.L, hz, from) : rms(r.L, from);
    };
    double worst = 0.0;
    for (uint32_t seed : {1u, 2u, 3u, 7u, 42u})
        worst = std::max(worst, std::fabs(db(level(2, seed, 0.0f, 0.0) / level(1, seed, 0.0f, 0.0))));
    // The breath alone, away from the tone (a sine): its band at 1.5 and 0.75 times the note.
    const auto breath = [&](int unison) {
        const double a = level(unison, 3, 1.0f, 1.5 * kC4), c = level(unison, 3, 1.0f, 0.75 * kC4);
        return std::sqrt(a * a + c * c);
    };
    const double breathDb = db(breath(2) / breath(1));
    std::printf("  Detune 0: unison 2 within %.3f dB of unison 1 over five seeds; breath %+.3f dB\n", worst, breathDb);
    CHECK(worst <= 0.5);
    CHECK(std::fabs(breathDb) <= 0.5);
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
    testWidthAndMute();
    testDeterminism();
    testNoAllocation();
}

} // namespace aft
