// The engine on its own (dsp/engine.h): Listen routing from the harmony to Ground and Bloom, the
// keys, the pedal and Hold, Stop, the mix and the output (tilt, volume, the limiter, the guard), the
// clock and the cost. Only signal.h's helpers, so `make test-module M=engine` builds it alone.
#include "check.h"
#include "signal.h"
#include "../dsp/engine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace aft {
namespace {

using af::Engine;
using af::Patch;

// Every slot TableSet's fallback, the one-frame sine: pure partials, easy to measure.
const af::TableSet& sines() {
    static af::TableSet s;
    return s;
}

// Every slot Saw, the brightest table: the limiter's and the cost's worst case.
const af::TableSet& saws() {
    static af::TableSet s;
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < af::TB_COUNT; ++i) s.t[i].store(&testTable(af::TB_SAW));
        ready = true;
    }
    return s;
}

// The default patch's own tables, Felt Piano (Bloom) and Cello Tasto (Ground), built as the
// library builds them; every other slot the sine.
const af::TableSet& defaults() {
    static af::TableSet s;
    static bool ready = false;
    if (!ready) {
        for (int id : {af::TB_FELT_PIANO, af::TB_CELLO_TASTO}) s.t[id].store(&testTable(id));
        ready = true;
    }
    return s;
}

// MPC's transport as the plugin hands it over: once per block, moving on while it plays.
struct Clock {
    double bpm = 120.0, beats = 0.0;
    bool playing = false;
};

struct Out {
    Buf L, R;
};

// `seconds` of output in 128-frame blocks, as the plugin renders them.
Out render(Engine& e, double seconds, Clock* t = nullptr) {
    const size_t n = static_cast<size_t>(seconds * af::kRate);
    Out o{Buf(n), Buf(n)};
    for (size_t b = 0; b < n; b += 128) {
        const int m = static_cast<int>(std::min<size_t>(128, n - b));
        if (t) {
            e.setTransport(t->bpm, t->beats, t->playing, true);
            if (t->playing) t->beats += m / static_cast<double>(af::kRate) * t->bpm / 60.0;
        }
        e.render(&o.L[b], &o.R[b], m);
    }
    return o;
}

float peakOf(const Out& o) { return std::max(peak(o.L), peak(o.R)); }
bool finiteOut(const Out& o) { return allFinite(o.L) && allFinite(o.R); }
// The last `seconds` of a render.
Buf tail(const Buf& x, double seconds) {
    const size_t k = std::min(x.size(), static_cast<size_t>(seconds * af::kRate));
    return Buf(x.end() - static_cast<std::ptrdiff_t>(k), x.end());
}

// The default patch at 0 dB with Space off (its return at 0): the strata dry.
Patch dry() {
    Patch p;
    p.volumeDb = 0.0f;
    p.spaceReturn = 0.0f;
    return p;
}
Patch groundOnly() {
    Patch p = dry();
    p.bloom.mute = true;
    return p;
}
Patch bloomOnly() {
    Patch p = dry();
    p.ground.mute = true;
    return p;
}
// Bloom as a plain sine per note: Equal, as played, no chord, the filter open, nothing moving.
Patch bloomSine() {
    Patch p = bloomOnly();
    p.harmony.tuning = af::TU_EQUAL;
    p.harmony.input = af::IN_AS_PLAYED;
    p.harmony.chord = af::CH_OFF;
    p.bloom.cutoffHz = 20000.0f;
    p.bloom.reso = 0.0f;
    p.bloom.breath = 0.0f;
    p.bloom.swellS = 0.005f;
    p.bloom.width = 0.0f;
    return p;
}

bool sounding(int stage) { return stage == af::Bloom::ST_ATTACK || stage == af::Bloom::ST_SUSTAIN; }
// The pitch classes Bloom's voices sound (attack or sustain), as a set.
uint16_t bloomPcs(const Engine& e) {
    uint16_t pcs = 0;
    for (int i = 0; i < af::Bloom::kVoices; ++i) {
        const af::Bloom::VoiceView v = e.bloom().voice(i);
        if (sounding(v.stage)) pcs = static_cast<uint16_t>(pcs | 1u << (v.note % 12));
    }
    return pcs;
}
int stageOf(const Engine& e, int note) {   // the stage of the voice sounding `note`; -1: none
    for (int i = 0; i < af::Bloom::kVoices; ++i) {
        const af::Bloom::VoiceView v = e.bloom().voice(i);
        if (v.stage != af::Bloom::ST_FREE && v.note == note) return v.stage;
    }
    return -1;
}
bool inChord(const af::Chord& c, int note) {
    for (int i = 0; i < c.n; ++i)
        if (c.notes[i] == note) return true;
    return false;
}

// Check 1: nothing sounds before the first note-on, Free strata included.
void testSilence() {
    std::printf("== engine: silence before the first note\n");
    for (int listen : {static_cast<int>(af::LI_FREE), -1}) {
        Engine e(sines());
        Patch p;
        if (listen >= 0) p.ground.listen = p.bloom.listen = listen;
        e.setPatch(p);
        Clock t;
        t.playing = true;
        // What isn't a note-on wakes nothing: a pedal, a note-off, CC 123, a Stop.
        e.sustain(true);
        e.noteOff(60);
        e.allNotesOff();
        const Out o = render(e, 2.0, &t);
        t.playing = false;
        const Out o2 = render(e, 0.1, &t);
        CHECK(peakOf(o) == 0.0f && peakOf(o2) == 0.0f);   // exactly 0, not just quiet
        CHECK(!e.info().awake && e.info().idle && e.activeVoices() == 0);
    }
}

// Check 2: the defaults, Bloom on the notes and Ground on the harmony, memory Forever.
void testDefaults() {
    std::printf("== engine: Listen defaults (Bloom Notes, Ground Harmony)\n");
    Engine e(sines());
    e.setPatch(dry());
    e.noteOn(60, 100);
    render(e, 3.0);
    CHECK(e.bloom().active() == 3 && e.ground().audible() && e.activeVoices() == 4);
    CHECK(e.info().awake && e.info().groundTarget % 12 == 0);
    e.noteOff(60);
    render(e, 7.0);   // Release 6 s to -60 dB, as Tail Voice with Space off
    CHECK(e.bloom().active() == 0);
    const Out o = render(e, 23.0);   // 30 s after the note-off
    const double r = rms(tail(o.L, 1.0));
    std::printf("  Ground 30 s after the note-off: %.1f dBFS RMS\n", db(r));
    CHECK(e.ground().audible() && r > 0.01 && e.activeVoices() == 1 && e.info().awake);

    // With Space on, the tail handoff frees Bloom's voices after 1.5 s while the reverb rings on.
    Engine s(sines());
    Patch p;
    p.volumeDb = 0.0f;
    s.setPatch(p);
    s.noteOn(60, 100);
    render(s, 3.0);
    s.noteOff(60);
    render(s, 1.0);
    CHECK(s.bloom().active() == 3);
    render(s, 1.0);
    CHECK(s.bloom().active() == 0 && s.activeVoices() == 1);
}

// Check 3: Bloom on the harmony holds the chord after the keys are up, and moves with it.
void testBloomHarmony() {
    std::printf("== engine: Bloom Harmony\n");
    Engine e(sines());
    Patch p = bloomOnly();
    p.bloom.listen = af::LI_HARMONY;
    p.bloom.swellS = 0.05f;
    e.setPatch(p);
    e.noteOn(60, 100);
    render(e, 1.0);
    e.noteOff(60);
    const Out o = render(e, 3.0);
    CHECK(e.bloom().active() == 3 && bloomPcs(e) == ((1u << 0) | (1u << 4) | (1u << 7)));   // C E G
    CHECK(rms(tail(o.L, 1.0)) > 0.01);
    e.noteOn(65, 100);   // F A C: C carries on, E and G release, F and A start
    render(e, 0.5);
    CHECK(e.bloom().active() <= 6);
    CHECK(bloomPcs(e) == ((1u << 5) | (1u << 9) | (1u << 0)));
    // The memory forgetting lets Bloom go: Memory Off, the last key up.
    p.harmony.memoryBars = 0;
    e.setPatch(p);
    e.noteOff(65);
    render(e, 0.1);
    CHECK(e.info().harmonyRoot == -1 && bloomPcs(e) == 0);
}

// Check 4: Free strata play the key's tonic from the first note on, whatever the note was.
void testFree() {
    std::printf("== engine: Free\n");
    Engine e(sines());
    Patch p = groundOnly();
    p.ground.listen = af::LI_FREE;
    p.harmony.key = 7;   // G
    p.harmony.tuning = af::TU_EQUAL;
    p.ground.sub = p.ground.fifth = p.ground.octave = p.ground.color = 0.0f;   // the root alone
    p.ground.breath = 0.0f;
    p.ground.beatHz = 0.0f;
    p.ground.gravityS = 0.0f;
    p.ground.fadeS = 0.05f;
    e.setPatch(p);
    e.noteOn(64, 100);   // E
    render(e, 0.2);
    e.noteOff(64);
    const Out o = render(e, 2.0);
    const double hz = zeroCrossHz(o.L, o.L.size() / 2, o.L.size());
    std::printf("  Ground Free in G after an E: target %d, %.3f Hz (G2 98.00)\n", e.info().groundTarget, hz);
    CHECK(e.info().groundTarget % 12 == 7 && e.ground().audible());
    CHECK(std::fabs(hz - 97.9989) < 0.05);
    // A new key moves it.
    p.harmony.key = 9;
    e.setPatch(p);
    CHECK(e.info().groundTarget % 12 == 9);

    // Bloom Free: the tonic triad, re-voiced when the key changes.
    Engine b(sines());
    Patch q = bloomOnly();
    q.bloom.listen = af::LI_FREE;
    q.bloom.swellS = 0.05f;
    q.harmony.key = 2;   // D: D F# A
    b.setPatch(q);
    b.noteOn(71, 100);
    b.noteOff(71);
    render(b, 0.5);
    CHECK(bloomPcs(b) == ((1u << 2) | (1u << 6) | (1u << 9)));
    q.harmony.key = 7;   // G: G B D
    b.setPatch(q);
    render(b, 0.5);
    CHECK(bloomPcs(b) == ((1u << 7) | (1u << 11) | (1u << 2)));
    // Chord Off plays the triad still.
    q.harmony.chord = af::CH_OFF;
    b.setPatch(q);
    render(b, 0.5);
    CHECK(bloomPcs(b) == ((1u << 7) | (1u << 11) | (1u << 2)));
}

// Check 5: Input Snap maps the key before the harmony hears it.
void testSnap() {
    std::printf("== engine: Snap\n");
    Engine e(sines());
    Patch p;
    p.harmony.key = 2;   // D Major: C snaps down to B (the lower one of B and C#)
    p.harmony.scale = af::SC_MAJOR;
    p.harmony.input = af::IN_SNAP;
    e.setPatch(p);
    e.noteOn(60, 100);
    CHECK(e.info().groundTarget >= 0 && e.info().groundTarget % 12 == 11);
    CHECK(e.info().harmonyRoot % 12 == 11);
    // The note-off finds the key's note though the patch changed in between.
    p.harmony.input = af::IN_AS_PLAYED;
    e.setPatch(p);
    e.noteOff(60);
    CHECK(e.harmony().held() == 0);
}

// Check 6: Hold latches; the next note-on lets the latched chord go, the common notes carrying on.
// The pedal holds until it comes up.
void testHold() {
    std::printf("== engine: Hold and the pedal\n");
    Engine e(sines());
    Patch p = bloomOnly();
    p.hold = true;
    p.harmony.voicing = af::VO_CLOSE;
    p.bloom.swellS = 0.05f;
    e.setPatch(p);
    e.noteOn(60, 100);
    render(e, 0.5);
    e.noteOff(60);
    render(e, 1.0);
    bool held = e.bloom().active() == 3;
    for (int i = 0; i < af::Bloom::kVoices; ++i) {
        const af::Bloom::VoiceView v = e.bloom().voice(i);
        held = held && (v.stage == af::Bloom::ST_FREE || v.stage == af::Bloom::ST_SUSTAIN);
    }
    CHECK(held);
    CHECK(e.harmony().held() == 1);   // the latched key still counts for the harmony
    const af::Chord c1 = af::buildChord(p.harmony, 60);
    const af::Chord c2 = af::leadFrom(p.harmony, c1, af::buildChord(p.harmony, 69));   // A C E
    int common = 0;
    for (int k = 0; k < c1.n; ++k) common += inChord(c2, c1.notes[k]) ? 1 : 0;
    CHECK(common == 2);
    e.noteOn(69, 100);
    bool ok = true;
    for (int k = 0; k < c1.n; ++k) {
        const int note = c1.notes[k], st = stageOf(e, note);
        ok = ok && (inChord(c2, note) ? st == af::Bloom::ST_SUSTAIN : st == af::Bloom::ST_RELEASE);
    }
    CHECK(ok);
    render(e, 0.2);
    ok = true;
    for (int k = 0; k < c2.n; ++k) ok = ok && sounding(stageOf(e, c2.notes[k]));
    CHECK(ok && e.harmony().held() == 1);
    // Latched again; Hold off lets it go.
    e.noteOff(69);
    render(e, 0.2);
    CHECK(bloomPcs(e) == c2.pcs);
    p.hold = false;
    e.setPatch(p);
    CHECK(bloomPcs(e) == 0 && e.harmony().held() == 0);

    // The pedal.
    Engine s(sines());
    Patch q = bloomOnly();
    q.bloom.swellS = 0.05f;
    s.setPatch(q);
    s.sustain(true);
    s.noteOn(60, 100);
    s.noteOff(60);
    render(s, 0.5);
    CHECK(bloomPcs(s) == af::buildChord(q.harmony, 60).pcs && s.harmony().held() == 1);
    s.sustain(false);
    CHECK(bloomPcs(s) == 0 && s.harmony().held() == 0);
    // A key still down when the pedal comes up keeps sounding; CC 121 puts the pedal up.
    s.sustain(true);
    s.noteOn(62, 100);
    s.sustain(false);
    render(s, 0.2);
    CHECK(bloomPcs(s) == af::buildChord(q.harmony, 62).pcs);
    s.sustain(true);
    s.noteOff(62);
    s.resetControllers();
    CHECK(bloomPcs(s) == 0);
    // Hold over the pedal: the latch wins, the pedal coming up leaves it.
    q.hold = true;
    s.setPatch(q);
    s.sustain(true);
    s.noteOn(64, 100);
    s.noteOff(64);
    s.sustain(false);
    render(s, 0.2);
    CHECK(bloomPcs(s) == af::buildChord(q.harmony, 64).pcs);
    // CC 123 lets every key go, the latched too, and the engine stays awake.
    s.allNotesOff();
    CHECK(bloomPcs(s) == 0 && s.harmony().held() == 0 && s.info().awake);

    // Hold under Chord Off, where the harmony's chord is the keys held: the latched keys leave it
    // before the next key, so the next chord is that key alone, for Bloom on the harmony too.
    for (int listen : {static_cast<int>(af::LI_NOTES), static_cast<int>(af::LI_HARMONY)}) {
        Engine c(sines());
        Patch r = bloomOnly();
        r.hold = true;
        r.harmony.chord = af::CH_OFF;
        r.harmony.input = af::IN_AS_PLAYED;
        r.bloom.listen = listen;
        r.bloom.swellS = 0.05f;
        c.setPatch(r);
        c.noteOn(60, 100);
        c.noteOn(64, 100);
        c.noteOff(60);
        c.noteOff(64);
        render(c, 0.3);
        CHECK(c.harmony().current().n == 2 && bloomPcs(c) == ((1u << 0) | (1u << 4)));
        c.noteOn(67, 100);
        render(c, 0.3);
        CHECK(c.harmony().current().n == 1 && c.harmony().current().root == 67 && bloomPcs(c) == (1u << 7));
        // A latched key pressed again keeps its voice, untouched.
        c.noteOff(67);
        const int was = stageOf(c, 67);
        c.noteOn(67, 100);
        CHECK(was == af::Bloom::ST_SUSTAIN && stageOf(c, 67) == af::Bloom::ST_SUSTAIN && c.bloom().active() <= 3);
    }
}

// Check 7: On Stop, from the transport and from a suspend.
void testStop() {
    std::printf("== engine: On Stop\n");
    for (int mode : {af::OS_FADE, af::OS_KEEP, af::OS_CUT}) {
        Engine e(sines());
        Patch p;
        p.volumeDb = 0.0f;
        p.onStop = mode;
        e.setPatch(p);
        Clock t;
        t.playing = true;
        e.noteOn(60, 100);
        const Out before = render(e, 2.0, &t);
        e.noteOff(60);
        t.playing = false;   // the falling edge, at the next block
        const Out first = render(e, 128.0 / af::kRate, &t);
        if (mode == af::OS_CUT) {
            CHECK(peakOf(first) == 0.0f && !e.info().awake);
            continue;
        }
        const Out mid = render(e, 4.0, &t);
        const Out last = render(e, 4.2 - 128.0 / af::kRate, &t);
        const float end = std::max(peak(tail(last.L, 0.2)), peak(tail(last.R, 0.2)));
        if (mode == af::OS_FADE) {
            std::printf("  Fade: peak %.1f dBFS before, %.1f at 4 s, %.1f at 8.2 s; asleep %d\n", db(peakOf(before)),
                        db(std::max(peak(tail(mid.L, 0.1)), peak(tail(mid.R, 0.1)))), db(end), !e.info().awake);
            CHECK(end < 1e-3f && !e.info().awake && !e.info().fading);
            CHECK(peakOf(render(e, 0.5, &t)) == 0.0f);
        } else {
            CHECK(end > 1e-3f && e.info().awake);
        }
    }
    // A fade turned round by a note-on.
    {
        Engine e(sines());
        Patch p;
        e.setPatch(p);
        Clock t;
        t.playing = true;
        e.noteOn(60, 100);
        render(e, 1.0, &t);
        t.playing = false;
        render(e, 2.0, &t);
        CHECK(e.info().fading && e.info().fadeDb < -10.0f);
        e.noteOn(62, 100);
        CHECK(!e.info().fading);
        render(e, 1.0, &t);
        CHECK(e.info().fadeDb == 0.0f && e.info().awake);
        render(e, 8.0, &t);
        CHECK(e.info().awake);
    }
    // Suspend and resume: 100 ms apart is a Stop, a second is a reset.
    {
        Engine e(sines());
        Patch p;
        p.onStop = af::OS_CUT;
        e.setPatch(p);
        e.noteOn(60, 100);
        render(e, 1.0);
        e.suspend();
        CHECK(e.info().suspended);
        e.resume(0.1);
        CHECK(peakOf(render(e, 128.0 / af::kRate)) == 0.0f && !e.info().awake && !e.info().suspended);
        p.onStop = af::OS_FADE;
        e.setPatch(p);
        e.noteOn(60, 100);
        render(e, 1.0);
        e.suspend();
        e.resume(0.1);
        CHECK(e.info().fading);
        p.onStop = af::OS_KEEP;
        e.setPatch(p);
        e.noteOn(60, 100);   // turns the fade round
        render(e, 1.0);
        e.suspend();
        e.resume(0.1);
        CHECK(e.info().awake && !e.info().fading);
        e.suspend();
        e.resume(1.0);
        CHECK(peakOf(render(e, 128.0 / af::kRate)) == 0.0f && !e.info().awake && e.harmony().current().root == -1);
        e.resume(0.0);   // without a suspend: nothing
        CHECK(!e.info().awake);
    }
}

// The worst case for level: every level, send and partial at full, resonance, shimmer and freeze,
// Saw everywhere, the lows tilted up, +6 dB of volume, chords of 6 notes struck hard.
Patch loudest(bool abyss) {
    Patch p;
    p.volumeDb = 6.0f;
    p.tilt = -1.0f;
    p.harmony.chord = af::CH_OFF;
    p.harmony.tuning = af::TU_EQUAL;
    p.harmony.input = af::IN_AS_PLAYED;
    af::GroundPatch& g = p.ground;
    g.level = 1.0f;
    g.table = af::TB_SAW;
    g.sub = g.root = g.fifth = g.octave = g.color = 1.0f;
    g.cutoffHz = 16000.0f;
    g.registerOct = 1;
    g.body = 1.0f;
    g.breath = 1.0f;
    g.breathHz = 0.5f;
    g.beatHz = 3.0f;
    g.width = 0.0f;
    g.fadeS = 0.05f;
    af::BloomPatch& b = p.bloom;
    b.level = 1.0f;
    b.table = b.tableB = af::TB_SAW;
    b.cutoffHz = 1500.0f;
    b.reso = 1.0f;
    b.velSens = 0.0f;
    b.swellS = 0.005f;
    b.releaseS = 30.0f;
    b.breath = 1.0f;
    b.width = 0.0f;
    p.groundSpace = p.bloomSpace = p.spaceReturn = 1.0f;
    af::Reverb::Params& r = p.space.reverb;
    r.mode = abyss ? af::Reverb::ABYSS : af::Reverb::HALL;
    r.size = 1.0f;
    r.decayS = 30.0f;
    r.predelayMs = 0.0f;
    r.dampHz = 20000.0f;
    r.lowCutHz = 20.0f;
    r.freeze = true;
    r.shimmer = 1.0f;
    p.space.rise = 0.0f;
    return p;
}

// Six keys at a time, a new six every `every` seconds (the latest key up as the next go down).
float playChords(Engine& e, double seconds, double every, float* minGain = nullptr, bool* finite = nullptr) {
    static const int kSets[4][6] = {{36, 43, 48, 55, 64, 72}, {38, 45, 50, 57, 65, 74},
                                    {31, 38, 43, 50, 59, 67}, {33, 40, 45, 52, 60, 69}};
    float worst = 0.0f;
    int set = -1;
    for (double at = 0.0; at < seconds; at += every) {
        if (set >= 0)
            for (int k : kSets[set]) e.noteOff(k);
        set = (set + 1) % 4;
        for (int k : kSets[set]) e.noteOn(k, 127);
        const size_t n = static_cast<size_t>(every * af::kRate);
        float L[128], R[128];
        for (size_t b = 0; b < n; b += 128) {
            const int m = static_cast<int>(std::min<size_t>(128, n - b));
            e.render(L, R, m);
            for (int i = 0; i < m; ++i) {
                worst = std::max(worst, std::max(std::fabs(L[i]), std::fabs(R[i])));
                if (finite && (!std::isfinite(L[i]) || !std::isfinite(R[i]))) *finite = false;
            }
            if (minGain) *minGain = std::min(*minGain, e.info().limiterGain);
        }
    }
    return worst;
}

// Check 8: the limiter holds -1 dBFS at every sample, whatever comes in.
void testLimiter() {
    std::printf("== engine: the limiter\n");
    for (bool abyss : {false, true}) {
        Engine e(saws());
        // 15 s into the reverb, then frozen (Freeze mutes the input) under 45 s more of chords.
        Patch p = loudest(abyss);
        p.space.reverb.freeze = false;
        e.setPatch(p);
        float minGain = 1.0f;
        bool finite = true;
        float worst = playChords(e, 15.0, 5.0, &minGain, &finite);
        e.setPatch(loudest(abyss));
        worst = std::max(worst, playChords(e, 45.0, 5.0, &minGain, &finite));
        std::printf("  %s, Decay 30, freeze, shimmer 1: peak %.5f (%.2f dBFS), the limiter down to %.1f dB\n",
                    abyss ? "Abyss" : "Hall", worst, db(worst), db(minGain));
        CHECK(finite && worst <= 0.8913f && worst > 0.8f && e.info().guards == 0);
    }
}

// Check 9: a NaN in the reverb's return zeroes the block, resets the DSP, and is counted once.
void testGuard() {
    std::printf("== engine: the non-finite guard\n");
    Engine e(sines());
    Patch p;
    e.setPatch(p);
    e.noteOn(60, 100);
    CHECK(peakOf(render(e, 1.0)) > 0.0f);
    e.testInjectNaN();
    const uint32_t trips = af::guardTrips();
    const Out hit = render(e, 128.0 / af::kRate);
    CHECK(peakOf(hit) == 0.0f && finiteOut(hit) && e.info().guards == 1);
    CHECK(af::guardTrips() - trips == 1);   // the process-wide count the soak reads
    const Out after = render(e, 2.0);
    CHECK(finiteOut(after) && e.info().guards == 1);
    // The harmony and the key stayed: Ground (Harmony) and Bloom come back by themselves.
    CHECK(e.info().awake && e.ground().audible() && peak(tail(after.L, 0.5)) > 0.0f);
}

// Check 10: a transport far into a song and a sample count past 2^32: the timers still run.
void testClock() {
    std::printf("== engine: the clock past 2^31 beats and 2^32 samples\n");
    Engine e(sines());
    Patch p = dry();
    p.harmony.memoryBars = 1;   // 2 s at 120
    e.setPatch(p);
    Clock t;
    t.playing = true;
    t.beats = 2147483648.0 + 10.0;
    e.testSetSamples((uint64_t{1} << 32) - 64 * 128);
    e.noteOn(60, 100);
    const Out a = render(e, 0.5, &t);
    e.noteOff(60);
    const Out b = render(e, 2.5, &t);
    CHECK(finiteOut(a) && finiteOut(b) && peakOf(b) > 0.0f);
    CHECK(e.info().samples > (uint64_t{1} << 32) && e.info().harmonyRoot == -1 && e.info().groundTarget == -1);
    render(e, 6.0, &t);
    CHECK(!e.ground().audible());   // faded out (Fade 4 s)
    e.noteOn(62, 100);
    render(e, 1.0, &t);
    t.playing = false;
    render(e, 8.2, &t);
    CHECK(!e.info().awake);
}

// The synced cycles run on the engine's one beat count (clockStrata): MPC's position while it plays, on at the tempo
// while it is stopped, whether a stratum sounds or not. And a cycle that starts from silence starts on its place, not
// gliding from where it stood: a chord after a second and more of silence (Bloom rendered nothing) has each voice on
// its staggered place at its first control step, within 1e-6 of the table; the breath unmuted after a second and more
// muted (Ground not rendered) is on the beat at its first step, within 0.1 dB of where the count puts it; and after 3 s
// stopped with only Ground sounding, a chord's voices and the breath are on that one count together. Playing and
// stopped. (Each stratum ran its own clock while stopped, only while rendered: after 3 s stopped a chord was 0.17
// of the table off the drone's grid; and a stale phase glided: the chord from silence 0.41 of the table off at its
// first step, the breath unmuted 3.1 dB.)
void testSyncFromSilence() {
    std::printf("== engine: the synced cycles on one count, from silence on their places\n");
    constexpr double kBpm = 120.0;
    for (bool stop : {false, true}) {
        Engine e(sines());
        Patch p = dry();
        p.onStop = af::OS_KEEP;
        p.ground.listen = af::LI_FREE;   // the drone on the tonic throughout; Bloom on the keys
        p.ground.fadeS = 0.05f;
        p.ground.breath = 1.0f;
        p.ground.breathBeats = 1.0f;     // 1/4: two breaths a second, its top on each beat
        p.bloom.pos = af::LifePos{0.5f, 1.0f, 0.3f, 0.0f, 4.0f};   // the full sway, one a bar, no smear
        p.bloom.swellS = 0.005f;
        p.bloom.releaseS = 0.05f;
        p.bloom.tail = af::TL_VOICE;
        e.setPatch(p);
        double mpc = 37.3;    // MPC's position: it stands still while MPC is stopped
        double count = mpc;   // where the strata's count should be: on at the tempo either way
        bool playing = true;
        float L[128], R[128];
        const auto run = [&](double seconds) {
            for (int b = 0; b < static_cast<int>(seconds * af::kRate / 128.0); ++b) {
                e.setTransport(kBpm, mpc, playing, true);
                e.render(L, R, 128);
                count += 128.0 * kBpm / 60.0 / af::kRate;
                if (playing) mpc = count;
            }
        };
        // One control step (32 samples): the breath's gain off the count's, in dB; how far Bloom's voices read from
        // their staggered places on it (the largest), and how many sound.
        float off = 0.0f;
        int voices = 0;
        const auto step = [&]() {
            e.setTransport(kBpm, mpc, playing, true);
            e.render(L, R, af::kChunk);
            count += af::kChunk * kBpm / 60.0 / af::kRate;
            if (playing) mpc = count;
            off = 0.0f;
            voices = 0;
            for (int i = 0; i < af::Bloom::kVoices; ++i) {
                const af::Bloom::VoiceView v = e.bloom().voice(i);
                if (v.stage != af::Bloom::ST_ATTACK) continue;
                ++voices;
                double ph = count / 4.0 + static_cast<double>(i) / af::Bloom::kVoices;
                ph -= std::floor(ph);
                off = std::max(off, std::fabs(v.pos - (0.5f + 0.25f * af::sinCycle(static_cast<float>(ph)))));
            }
            double ph = count + 0.25;
            ph -= std::floor(ph);
            return 20.0 * std::log10(e.ground().outputGain() / p.ground.level) - 3.0 * std::sin(2.0 * kPi * ph);
        };
        e.noteOn(60, 100);
        run(1.0);
        playing = !stop;
        run(0.5);
        e.noteOff(60);
        p.ground.mute = true;
        e.setPatch(p);
        run(1.3);   // Bloom released and silent, Ground muted: neither rendered
        CHECK(e.bloom().active() == 0 && !e.ground().audible());
        p.ground.mute = false;
        e.setPatch(p);
        e.noteOn(64, 100);
        const double breath = step();
        const float bloom = off;
        const int chord = voices;
        e.noteOff(64);
        run(3.0);   // Bloom silent, Ground sounding
        CHECK(e.bloom().active() == 0 && e.ground().audible());
        e.noteOn(62, 100);
        const double together = step();
        std::printf("  %s: from silence the breath %.3f dB off the count, the chord %.7f of the table off its places; "
                    "3 s on, Bloom %.7f off and the breath %.3f dB\n",
                    stop ? "stopped" : "playing", breath, bloom, off, together);
        CHECK(chord >= 3 && voices >= 3);
        CHECK(std::fabs(breath) < 0.1 && bloom < 1e-6f);
        CHECK(std::fabs(together) < 0.1 && off < 1e-6f);
    }
}

// The mix: pitch, velocity, volume, pan, tilt.
void testMix() {
    std::printf("== engine: pitch, velocity, volume, pan, tilt\n");
    // Bloom alone, a sine: the note's pitch, a voice's level from the velocity.
    Engine a(sines()), v(sines());
    a.setPatch(bloomSine());
    v.setPatch(bloomSine());
    a.noteOn(69, 127);
    v.noteOn(69, 64);
    render(a, 0.2);
    render(v, 0.2);
    const Out oa = render(a, 1.0), ov = render(v, 1.0);
    const double hz = zeroCrossHz(oa.L, 0, oa.L.size());
    const double ratio = rms(ov.L) / rms(oa.L), want = 0.6 + 0.4 * 64.0 / 127.0;   // Vel 0.4
    std::printf("  A4 %.3f Hz; velocity 64 / 127: %.4f (Vel 0.4: %.4f)\n", hz, ratio, want);
    CHECK(std::fabs(hz - 440.0) < 0.05 && std::fabs(ratio - want) < 0.003);
    // Ground alone: Register 2's A, on the root.
    Engine g(sines());
    Patch pg = groundOnly();
    pg.harmony.tuning = af::TU_EQUAL;
    pg.ground.sub = pg.ground.fifth = pg.ground.octave = 0.0f;
    pg.ground.beatHz = pg.ground.breath = 0.0f;
    pg.ground.fadeS = 0.05f;
    g.setPatch(pg);
    g.noteOn(69, 100);
    render(g, 0.5);
    const Out og = render(g, 1.0);
    CHECK(std::fabs(zeroCrossHz(og.L, 0, og.L.size()) - 110.0) < 0.05);

    // The volume: -6 dB halves exactly; -inf is exact silence; NaN is off.
    Patch q = bloomSine();
    q.volumeDb = -6.0206f;
    a.setPatch(q);
    render(a, 0.1);
    CHECK(std::fabs(rms(render(a, 1.0).L) / rms(oa.L) - 0.5) < 0.002);
    q.volumeDb = -60.0f;   // the glide (10 ms) lands on 0 within 0.2 s
    a.setPatch(q);
    render(a, 0.2);
    CHECK(peakOf(render(a, 0.5)) == 0.0f);
    q.volumeDb = 0.0f;
    a.setPatch(q);
    render(a, 0.2);
    q.volumeDb = std::nanf("");
    a.setPatch(q);
    render(a, 0.2);
    CHECK(peakOf(render(a, 0.5)) == 0.0f);

    // Pan: hard left leaves the right channel empty, exactly; the middle is unity.
    Engine l(sines());
    Patch pl = bloomSine();
    pl.bloomPan = -1.0f;
    l.setPatch(pl);
    l.noteOn(69, 127);
    render(l, 0.2);
    const Out ol = render(l, 1.0);
    std::printf("  Bloom hard left: L %.4f (the middle %.4f, 3 dB up), R %.4f\n", rms(ol.L), rms(oa.L), rms(ol.R));
    CHECK(peak(ol.R) == 0.0f && std::fabs(rms(ol.L) / rms(oa.L) - std::sqrt(2.0)) < 0.01);
    Engine r(sines());
    Patch pr = groundOnly();
    pr.groundPan = 1.0f;
    r.setPatch(pr);
    r.noteOn(60, 100);
    render(r, 0.5);
    const Out orr = render(r, 0.5);
    CHECK(peak(orr.L) == 0.0f && peak(orr.R) > 0.0f);

    // Tilt 1: the highs up and the lows down, 0 dB at the pivot (the analog shelf: +5.3 dB at
    // 3520 Hz, -5.7 dB at 110 Hz).
    for (int note : {105, 45}) {
        double level[2];
        for (int k = 0; k < 2; ++k) {
            Engine t(sines());
            Patch pt = bloomSine();
            pt.tilt = k ? 1.0f : 0.0f;
            t.setPatch(pt);
            t.noteOn(note, 127);
            render(t, 0.3);
            level[k] = rms(render(t, 1.0).L);
        }
        const double d = db(level[1] / level[0]);
        std::printf("  tilt 1 at %.0f Hz: %+.2f dB\n", 440.0 * std::pow(2.0, (note - 69) / 12.0), d);
        CHECK(note == 105 ? (d > 4.8 && d < 5.8) : (d < -5.2 && d > -6.0));
    }
}

// CC 123 doesn't sleep, reset() does; the idle gate; a muted Ground rendered until it is down.
void testIdle() {
    std::printf("== engine: reset, CC 123, idle, mute\n");
    Engine e(sines());
    Patch p = dry();
    p.ground.listen = af::LI_NOTES;
    p.ground.fadeS = 0.5f;
    p.bloom.releaseS = 0.5f;
    e.setPatch(p);
    e.noteOn(60, 100);
    render(e, 1.0);
    CHECK(e.activeVoices() == 4 && !e.info().idle);
    e.noteOff(60);   // Ground on the notes fades, Bloom releases
    render(e, 1.0);
    CHECK(e.activeVoices() == 0 && e.info().awake && e.info().idle);
    CHECK(peakOf(render(e, 0.5)) == 0.0f);
    // CC 123: keys let go, awake still; Ground on the harmony goes on.
    p.ground.listen = af::LI_HARMONY;
    e.setPatch(p);
    e.noteOn(62, 100);
    render(e, 1.0);
    e.allNotesOff();
    render(e, 1.0);
    CHECK(e.info().awake && e.ground().audible() && e.bloom().active() == 0);
    // reset(): silence at once, the harmony forgotten, asleep.
    e.reset();
    CHECK(peakOf(render(e, 128.0 / af::kRate)) == 0.0f && !e.info().awake && e.info().harmonyRoot == -1 &&
          e.activeVoices() == 0);
    // Muting Ground: rendered until its gain is down, then skipped; unmuted it comes back.
    e.noteOn(60, 100);
    render(e, 1.0);
    p.ground.mute = true;
    p.bloom.mute = true;
    e.setPatch(p);
    CHECK(e.ground().audible());
    render(e, 0.01);
    CHECK(!e.ground().audible() && e.ground().sounding());
    e.noteOff(60);
    render(e, 1.0);
    CHECK(e.info().idle && peakOf(render(e, 0.1)) == 0.0f);
    p.ground.mute = false;
    e.setPatch(p);
    CHECK(e.ground().audible() && peakOf(render(e, 0.5)) > 0.0f);
    // Out-of-range notes and controllers are refused.
    e.noteOn(-1, 100);
    e.noteOn(128, 100);
    e.noteOff(500);
    e.noteOn(60, 0);   // velocity 0: a note-off
    render(e, 0.1);
    CHECK(e.harmony().held() == 0);
}

// Listen changed while sounding; the decay hold.
void testListenChanges() {
    std::printf("== engine: Listen changed mid-sound, the decay hold\n");
    Engine e(sines());
    Patch p = bloomOnly();
    p.bloom.swellS = 0.05f;
    e.setPatch(p);
    e.noteOn(60, 100);
    render(e, 0.5);
    const uint16_t c = af::buildChord(p.harmony, 60).pcs;
    CHECK(bloomPcs(e) == c);
    p.bloom.listen = af::LI_HARMONY;   // the same chord from the harmony: nothing moves
    e.setPatch(p);
    render(e, 0.2);
    CHECK(bloomPcs(e) == c && e.bloom().active() == 3);
    e.noteOff(60);   // the harmony keeps it
    render(e, 0.2);
    CHECK(bloomPcs(e) == c);
    p.bloom.listen = af::LI_NOTES;   // back on the notes: no key down, the harmony's chord goes
    e.setPatch(p);
    CHECK(bloomPcs(e) == 0);
    p.bloom.listen = af::LI_FREE;    // the tonic's chord (C)
    e.setPatch(p);
    render(e, 0.2);
    CHECK(bloomPcs(e) == af::buildChord(p.harmony, 48).pcs);
    p.ground.mute = false;
    p.ground.listen = af::LI_NOTES;  // no key down: Ground fades out
    e.setPatch(p);
    CHECK(e.info().groundTarget == -1);
    p.ground.listen = af::LI_FREE;
    e.setPatch(p);
    CHECK(e.info().groundTarget % 12 == 0);

    // The decay hold: Tail Space holds the reverb at Release or longer; Abyss, which rings four
    // times its Decay, at a quarter of it.
    Patch d;
    d.space.reverb.decayS = 2.0f;
    d.bloom.releaseS = 12.0f;
    e.setPatch(d);
    CHECK(e.info().spaceDecayS == 12.0f);
    d.bloom.tail = af::TL_VOICE;
    e.setPatch(d);
    CHECK(e.info().spaceDecayS == 2.0f);
    d.bloom.tail = af::TL_SPACE;
    d.space.reverb.mode = af::Reverb::ABYSS;
    e.setPatch(d);
    CHECK(e.info().spaceDecayS == 3.0f);

    // Back on the notes with keys held (one by the pedal, one by a finger): they play their chords
    // again, so Bloom sounds what Ground (on the notes) hears; the keys own them.
    Engine k(sines());
    Patch q = bloomOnly();
    q.bloom.listen = af::LI_HARMONY;
    q.bloom.swellS = 0.05f;
    k.setPatch(q);
    k.sustain(true);
    k.noteOn(60, 100);
    k.noteOff(60);
    k.noteOn(65, 100);
    render(k, 0.3);
    CHECK(bloomPcs(k) == af::buildChord(q.harmony, 65).pcs);   // the harmony's: the latest key
    q.bloom.listen = af::LI_NOTES;
    k.setPatch(q);
    render(k, 0.3);
    CHECK(bloomPcs(k) == (af::buildChord(q.harmony, 60).pcs | af::buildChord(q.harmony, 65).pcs));
    k.sustain(false);
    k.noteOff(65);
    CHECK(bloomPcs(k) == 0);
}

// Rise (space.cpp's kFull) against a real pad's send: the Init levels (the knobs' defaults,
// squared), a triad. The wet is the output with the return on less the output with it off (the
// same seed and notes; nothing else differs while the chord is held).
void testRise() {
    std::printf("== engine: Rise against the Init pad's send\n");
    double wet[2] = {};
    for (int k = 0; k < 2; ++k) {
        Patch p;   // Init: Patch{}'s levels and sends are the default knobs squared, its Space an 8 s hall
        p.volumeDb = 0.0f;
        p.space.rise = k ? 1.0f : 0.0f;
        Patch off = p;
        off.spaceReturn = 0.0f;
        Engine a(defaults()), b(defaults());
        a.setPatch(p);
        b.setPatch(off);
        a.noteOn(60, 100);
        b.noteOn(60, 100);
        render(a, 4.0);
        render(b, 4.0);
        const Out oa = render(a, 2.0), ob = render(b, 2.0);
        Buf d(oa.L.size());
        for (size_t i = 0; i < d.size(); ++i) d[i] = oa.L[i] - ob.L[i];
        wet[k] = rms(d);
    }
    std::printf("  the wet under a held triad: Rise 0 %.1f dBFS, Rise 1 %.1f dBFS\n", db(wet[0]), db(wet[1]));
    CHECK(db(wet[1] / wet[0]) < -20.0);
}

// Freeze: a frozen reverb takes no input, so a tail handed to it would be lost. Bloom is given a
// send of 0 and releases as Tail Voice: its release is heard as it is with Space off.
void testFreezeTail() {
    std::printf("== engine: Freeze and Bloom's tail\n");
    double level[2] = {};
    for (int k = 0; k < 2; ++k) {
        Engine e(sines());
        Patch p = bloomSine();
        p.bloom.releaseS = 6.0f;
        p.bloom.tail = af::TL_SPACE;
        if (k == 0) {   // Space on and frozen
            p.spaceReturn = 0.64f;
            p.space.reverb.freeze = true;
        }
        e.setPatch(p);
        e.noteOn(69, 127);
        render(e, 0.5);
        e.noteOff(69);
        if (k == 0) CHECK(stageOf(e, 69) == af::Bloom::ST_RELEASE);   // not a handoff
        const Out o = render(e, 3.0);
        level[k] = rms(o.L, 2 * 44100, 3 * 44100);   // 2..3 s into the release
    }
    std::printf("  2..3 s into a 6 s release: frozen Space %.1f dBFS, Space off %.1f dBFS\n", db(level[0]), db(level[1]));
    CHECK(level[1] > 1e-3 && std::fabs(db(level[0] / level[1])) < 0.1);
}

// Hold: a key pressed while a finger is still down joins the chord; the first key after every
// finger has left starts a new one, and the latched keys go.
void testHoldJoins() {
    std::printf("== engine: Hold, a chord built key by key\n");
    Engine e(sines());
    Patch p = bloomOnly();
    p.hold = true;
    p.harmony.chord = af::CH_OFF;
    p.harmony.input = af::IN_AS_PLAYED;
    p.bloom.swellS = 0.05f;
    e.setPatch(p);
    e.noteOn(60, 100);   // C
    e.noteOn(64, 100);   // E
    e.noteOff(60);
    e.noteOn(67, 100);   // G, E still down: it joins
    render(e, 0.3);
    const uint16_t ceg = (1u << 0) | (1u << 4) | (1u << 7);
    CHECK(bloomPcs(e) == ceg && e.harmony().current().n == 3);
    e.noteOff(64);
    e.noteOff(67);
    render(e, 0.3);
    CHECK(bloomPcs(e) == ceg && e.harmony().held() == 3);   // latched
    e.noteOn(65, 100);   // F, every finger up: a new chord
    render(e, 0.3);
    CHECK(bloomPcs(e) == (1u << 5) && e.harmony().current().n == 1 && e.harmony().current().root == 65);
    // With chords, the new chord's notes in common with the latched one carry on untouched.
    Engine c(sines());
    Patch q = bloomOnly();
    q.hold = true;
    q.harmony.voicing = af::VO_CLOSE;
    q.bloom.swellS = 0.05f;
    c.setPatch(q);
    c.noteOn(60, 100);   // C E G
    c.noteOff(60);
    render(c, 0.5);
    const af::Chord was = af::buildChord(q.harmony, 60), next = af::leadFrom(q.harmony, was, af::buildChord(q.harmony, 65));
    c.noteOn(65, 100);   // F A C: C carries on
    bool ok = true;
    for (int k = 0; k < was.n; ++k) {
        const int st = stageOf(c, was.notes[k]);
        ok = ok && (inChord(next, was.notes[k]) ? st == af::Bloom::ST_SUSTAIN : st == af::Bloom::ST_RELEASE);
    }
    CHECK(ok && inChord(next, 60));

    // Hold turned off with the pedal down: the latched key goes to the pedal.
    Engine d(sines());
    Patch r = bloomOnly();
    r.hold = true;
    r.bloom.swellS = 0.05f;
    d.setPatch(r);
    d.sustain(true);
    d.noteOn(60, 100);
    d.noteOff(60);
    r.hold = false;
    d.setPatch(r);
    render(d, 0.2);
    CHECK(bloomPcs(d) == af::buildChord(r.harmony, 60).pcs && d.harmony().held() == 1);
    d.sustain(false);
    CHECK(bloomPcs(d) == 0 && d.harmony().held() == 0);

    // A full harmony lets the oldest latched key go: a finger on 70, fifteen keys joining and latched.
    Engine f(sines());
    Patch s2 = dry();
    s2.hold = true;
    s2.harmony.input = af::IN_AS_PLAYED;
    f.setPatch(s2);
    f.noteOn(70, 100);
    for (int k = 41; k < 56; ++k) {
        f.noteOn(k, 100);
        f.noteOff(k);
    }
    CHECK(f.harmony().held() == af::Harmony::kHeldMax && f.harmony().lowestHeld() == 41);
    f.noteOn(56, 100);
    CHECK(f.harmony().held() == af::Harmony::kHeldMax && f.harmony().lowestHeld() == 42 &&
          f.info().harmonyRoot == af::buildChord(s2.harmony, 56).root);
}

// The pans, the return and the volume glide 10 ms, a step a sample from the control step that finds
// them changed, whatever cuts the pieces: hard left to hard right with the next piece one sample
// long steps no more than the signal does itself.
void testGlides() {
    std::printf("== engine: the pans and the return glide\n");
    Engine e(sines());
    Patch p = bloomSine();
    p.bloomPan = -1.0f;
    e.setPatch(p);
    e.noteOn(69, 127);
    const Out steady = render(e, 0.5);
    const float own = maxStep(steady.L, steady.L.size() / 2);
    p.bloomPan = 1.0f;
    e.setPatch(p);
    Out o{Buf(4096), Buf(4096)};
    e.render(&o.L[0], &o.R[0], 1);   // an event one sample in
    for (size_t b = 1; b < o.L.size(); b += 128) {
        const int m = static_cast<int>(std::min<size_t>(128, o.L.size() - b));
        e.render(&o.L[b], &o.R[b], m);
    }
    const float worst = std::max(maxStep(o.L), maxStep(o.R));
    std::printf("  hard left to hard right: largest step %.4f, the sine's own %.4f\n", worst, own);
    CHECK(worst < 2.0f * own);
    // The glide starts at the next control step (within 32 samples) and takes 441: under way 5 ms
    // in, done by 11 ms.
    CHECK(peak(o.L, 200, 221) > 0.0f && peak(o.L, 485, o.L.size()) == 0.0f && peak(o.R, 485, o.R.size()) > 0.05f);

    // The return: a ringing tail (Ground and Bloom muted, the reverb on) taken away over 10 ms.
    Engine r(sines());
    Patch q;
    q.volumeDb = 0.0f;
    q.ground.mute = true;
    r.setPatch(q);
    r.noteOn(60, 127);
    render(r, 2.0);
    q.bloom.mute = true;
    r.setPatch(q);
    render(r, 0.3);
    q.spaceReturn = 0.0f;
    r.setPatch(q);
    const Out t = render(r, 0.05);
    CHECK(std::max(peak(t.L, 200, 221), peak(t.R, 200, 221)) > 0.0f &&
          std::max(peak(t.L, 485, t.L.size()), peak(t.R, 485, t.R.size())) == 0.0f);

    // A change too small for its step a sample to move the gain (under half an ulp): it lands all
    // the same. 0.0001 dB is about 100 ulps over 1, a step of a quarter ulp.
    Engine v(sines());
    Patch pv = bloomSine();
    v.setPatch(pv);
    v.noteOn(69, 127);
    render(v, 0.2);
    pv.volumeDb = 0.0001f;
    v.setPatch(pv);
    render(v, 0.05);
    CHECK(!v.info().gliding);
}

// On Stop changed while a fade is under way: Keep turns it round, Cut resets; the transport
// starting turns it round too.
void testStopChanges() {
    std::printf("== engine: On Stop changed mid-fade, the transport starting again\n");
    Engine e(sines());
    Patch p;
    e.setPatch(p);
    Clock t;
    t.playing = true;
    e.noteOn(60, 100);
    render(e, 1.0, &t);
    t.playing = false;
    render(e, 1.0, &t);
    CHECK(e.info().fading);
    p.onStop = af::OS_KEEP;
    e.setPatch(p);
    CHECK(!e.info().fading);
    render(e, 1.0, &t);
    CHECK(e.info().fadeDb == 0.0f && e.info().awake);
    p.onStop = af::OS_FADE;
    e.setPatch(p);
    t.playing = true;
    render(e, 0.5, &t);
    t.playing = false;
    render(e, 1.0, &t);
    CHECK(e.info().fading);
    t.playing = true;   // playing again
    render(e, 128.0 / af::kRate, &t);
    CHECK(!e.info().fading);
    render(e, 1.0, &t);
    CHECK(e.info().fadeDb == 0.0f && e.info().awake);
    t.playing = false;
    render(e, 1.0, &t);
    CHECK(e.info().fading);
    p.onStop = af::OS_CUT;
    e.setPatch(p);
    CHECK(!e.info().awake && peakOf(render(e, 128.0 / af::kRate, &t)) == 0.0f);
}

// Check 11: the worst case renders faster than real time under ASan, with room to spare.
void testCost() {
    std::printf("== engine: cost of the worst case\n");
    Engine e(saws());
    Patch p = loudest(true);
    p.bloom.unison = 2;
    p.bloom.couple = af::CP_FM;
    p.bloom.coupleAmt = 0.5f;
    p.bloom.blend = 0.5f;
    p.space.reverb.freeze = false;
    e.setPatch(p);
    const auto t0 = std::chrono::steady_clock::now();
    playChords(e, 10.0, 2.5);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  10 s of 6-note chords, unison 2, FM, every partial, Abyss with shimmer: %.2f s\n", s);
#if defined(__SANITIZE_ADDRESS__)
    CHECK(s < 10.0);   // under ASan, in real time with room to spare (qemu's time says nothing)
#endif
}

// The limiter lets go: a second after the last peak over the knee its gain is exactly 1 again and
// it only looks (its release, in float, used to stall at 0.9998 and keep the gain computer running).
void testLimiterRelease() {
    std::printf("== engine: the limiter lets go\n");
    Engine e(sines());
    Patch p = bloomSine();
    p.volumeDb = 12.0f;
    e.setPatch(p);
    for (int k : {57, 60, 64, 67, 71, 74}) e.noteOn(k, 127);
    render(e, 0.5);
    const float down = e.info().limiterGain;
    p.volumeDb = -20.0f;   // every peak far under the knee from here
    e.setPatch(p);
    render(e, 1.5);
    std::printf("  limited to %.1f dB; 1.5 s after the volume came down: gain %.9f, limiting %d\n", db(down),
                e.info().limiterGain, e.info().limiting);
    CHECK(down < 0.9f && e.info().limiterGain == 1.0f && !e.info().limiting);
}

// The harmony keeps 16 keys. With the pedal down a long phrase passes that: the oldest key the
// pedal holds goes, and the harmony follows the latest key. Fingers on 16 keys keep them all.
void testManyKeys() {
    std::printf("== engine: more keys than the harmony keeps\n");
    Engine e(sines());
    Patch p = dry();
    p.harmony.input = af::IN_AS_PLAYED;
    e.setPatch(p);
    e.sustain(true);
    for (int k = 40; k < 60; ++k) {
        e.noteOn(k, 100);
        e.noteOff(k);
    }
    CHECK(e.harmony().held() == af::Harmony::kHeldMax);
    CHECK(e.info().harmonyRoot == af::buildChord(p.harmony, 59).root && e.info().groundTarget % 12 == 59 % 12);
    e.sustain(false);
    CHECK(e.harmony().held() == 0);
    // Sixteen fingers: the seventeenth key isn't heard by the harmony, the sixteen stay.
    for (int k = 40; k < 57; ++k) e.noteOn(k, 100);
    CHECK(e.harmony().held() == af::Harmony::kHeldMax && e.info().harmonyRoot == af::buildChord(p.harmony, 55).root);
}

// A fingerprint of a render (FNV-1a over the samples' bits, L then R, -0 as 0), as the Reverb's.
uint64_t fingerprint(const Out& o) {
    uint64_t h = 1469598103934665603ull;
    for (const Buf* b : {&o.L, &o.R})
        for (float v : *b) {
            uint32_t bits;
            const float x = v == 0.0f ? 0.0f : v;
            std::memcpy(&bits, &x, sizeof bits);
            for (int k = 0; k < 4; ++k) {
                h ^= (bits >> (8 * k)) & 0xffu;
                h *= 1099511628211ull;
            }
        }
    return h;
}

// 10 s of Init (Patch{}) on its own tables, the transport playing at 120: a chord, a key joining it,
// the pedal holding one through its note-off, the chord let go and the tail; blocks of 128, 1, 33,
// 77 and 100 in turn, every event at its own sample (cutting a piece). Seeded.
Out initPhrase() {
    Engine e(defaults());
    e.seed(0x1234567u);
    e.setPatch(Patch{});
    struct Ev {
        double s;
        int what;   // a note (> 0 on, < 0 off), 1000 pedal down, 1001 up
    };
    const Ev ev[] = {{0.0, 60}, {0.5, 64}, {2.0, 1000}, {2.5, -64}, {3.0, -60}, {3.2, 67}, {5.0, 1001}, {6.0, -67}};
    const size_t n = static_cast<size_t>(10.0 * af::kRate);
    Out o{Buf(n), Buf(n)};
    const int sizes[] = {128, 1, 33, 77, 100};
    Clock t;
    t.playing = true;
    size_t pos = 0, next = 0;
    int k = 0;
    while (pos < n) {
        for (; next < sizeof ev / sizeof *ev && static_cast<size_t>(ev[next].s * af::kRate) <= pos; ++next) {
            const int w = ev[next].what;
            if (w == 1000 || w == 1001) e.sustain(w == 1000);
            else if (w > 0) e.noteOn(w, 100);
            else e.noteOff(-w);
        }
        size_t m = static_cast<size_t>(sizes[k++ % 5]);
        if (next < sizeof ev / sizeof *ev) m = std::min(m, static_cast<size_t>(ev[next].s * af::kRate) - pos);
        m = std::min(m, n - pos);
        e.setTransport(t.bpm, t.beats, t.playing, true);
        e.render(&o.L[pos], &o.R[pos], static_cast<int>(m));
        t.beats += static_cast<double>(m) / af::kRate * t.bpm / 60.0;
        pos += m;
    }
    return o;
}

// M2's check 1: off is off. Air, Weather and every Echo send at 0 (Patch{}, Init), the engine plays
// what 0.0.2's did, bit for bit: the fingerprints of initPhrase() taken from 0.0.2's engine (3e1fb35)
// before Air, Weather, Echo and Memory were wired in (the x86 test build and the device's differ: GCC
// fuses multiply-adds for NEON). The plain builds only: against profile-guided objects (make
// test-arm-pgo) the profile moves the fusing, so no fingerprint stays.
void testOffIsOff() {
    std::printf("== engine: off is off (Init plays 0.0.2's samples)\n");
    const Out o = initPhrase();
    const uint64_t got = fingerprint(o);
    std::printf("  10 s of Init: fingerprint %016llx, peak %.3f\n", static_cast<unsigned long long>(got), peakOf(o));
    CHECK(finiteOut(o) && peakOf(o) > 0.1f);
#if AF_PGO_OBJECTS
    std::printf("  (the fingerprint is checked in the plain builds, not against profile-guided objects)\n");
#elif AF_NEON
    CHECK(got == 0x58b942e9ad9667c6ull);
#else
    CHECK(got == 0xc1eb20c9a6e3a242ull);
#endif
}

// --- M2: Air, Weather, Echo and Memory in the engine ------------------------------------------------

// Weather's source for the tests: two seconds of white noise, made a source as the loader makes one.
const af::GrainSource* noiseSource() {
    static std::unique_ptr<af::SourceBuffer> s;
    if (!s) {
        const Buf L = whiteNoise(2 * 44100, 0.5f, 7), R = whiteNoise(2 * 44100, 0.5f, 8);
        s = af::buildSource(L.data(), R.data(), static_cast<int>(L.size()));
    }
    return &s->src;
}

// Nothing but what a test turns on: Ground and Bloom muted (Bloom's muted voices free at once when let
// go), Space's return at 0, the volume at 0 dB.
Patch quiet() {
    Patch p = dry();
    p.ground.mute = p.bloom.mute = true;
    p.bloom.releaseS = 0.01f;
    return p;
}
Patch airOnly() {
    Patch p = quiet();
    p.air.level = 0.5f;
    p.air.voice.decayS = 0.5f;
    return p;
}
Patch weatherOnly() {
    Patch p = quiet();
    p.weather.level = 0.5f;
    return p;
}

// What a hook saw: Air's strikes, Weather's grains.
struct Struck {
    std::vector<int> notes;
    std::vector<float> pans, vels;
    std::vector<bool> played;
};
void onStrike(void* ctx, int note, float vel, float pan, bool played) {
    Struck& s = *static_cast<Struck*>(ctx);
    s.notes.push_back(note);
    s.vels.push_back(vel);
    s.pans.push_back(pan);
    s.played.push_back(played);
}
void onSpawn(void* ctx, float semis) { static_cast<std::vector<float>*>(ctx)->push_back(semis); }
// The pitch classes a set of transpositions land on (every source's root is C).
uint16_t pcsOf(const std::vector<float>& semis) {
    uint16_t pcs = 0;
    for (float t : semis) pcs = static_cast<uint16_t>(pcs | 1u << (((static_cast<int>(std::lround(t)) % 12) + 12) % 12));
    return pcs;
}
uint16_t pcsOf(const std::vector<int>& notes) {
    uint16_t pcs = 0;
    for (int n : notes) pcs = static_cast<uint16_t>(pcs | 1u << (n % 12));
    return pcs;
}

// Check 2: Air by its Listen. Harmony: after a chord and its release (memory Forever) Air goes on
// generating from it; Memory Off, it stops at the release. Notes: the keys' notes only, as played.
// Free: from the first note, on the tonic chord (Gravity 1: its tones only); nothing before it.
void testAirListen() {
    std::printf("== engine: Air by Listen\n");
    // Four seeds, each a minute after the release cut in thirds: strikes in every third, and as many
    // in all as a Poisson process at 30 a minute gives but once in a million (10..60).
    for (uint32_t seed : {1u, 2u, 3u, 4u}) {
        Engine e(sines());
        e.seed(seed);
        Patch p = airOnly();
        p.air.gen.density = 30.0f;
        e.setPatch(p);
        e.noteOn(60, 100);
        render(e, 1.0);
        e.noteOff(60);
        uint64_t at = e.info().airStrikes, all = 0;
        bool every = true;
        Out o;
        for (int third = 0; third < 3; ++third) {
            o = render(e, 20.0);
            const uint64_t k = e.info().airStrikes - at;
            at += k;
            all += k;
            every = every && k > 0;
        }
        std::printf("  Harmony, memory Forever, seed %u: %llu strikes in the minute after the release (density 30)\n",
                    seed, static_cast<unsigned long long>(all));
        CHECK(every && all >= 10 && all <= 60 && rms(tail(o.L, 10.0)) > 1e-4);
    }
    {
        Engine e(sines());
        Patch p = airOnly();
        p.air.gen.density = 60.0f;
        p.harmony.memoryBars = 0;   // Off: the chord goes with the last key
        e.setPatch(p);
        e.noteOn(60, 100);
        render(e, 5.0);
        const uint64_t before = e.info().airStrikes;
        e.noteOff(60);
        render(e, 30.0);
        std::printf("  Harmony, memory Off: %llu strikes while held, %llu after\n", static_cast<unsigned long long>(before),
                    static_cast<unsigned long long>(e.info().airStrikes - before));
        CHECK(before >= 2 && e.info().airStrikes == before);
    }
    {
        Engine e(sines());
        Patch p = airOnly();
        p.air.listen = af::LI_NOTES;
        p.air.gen.density = 30.0f;
        p.air.velSens = 1.0f;
        e.setPatch(p);
        Struck s;
        e.testAirHook(onStrike, &s);
        e.noteOn(62, 127);
        e.noteOn(67, 64);
        render(e, 10.0);
        e.noteOff(62);
        e.noteOff(67);
        render(e, 5.0);
        CHECK(s.notes == (std::vector<int>{62, 67}) && s.played[0] && s.played[1]);
        CHECK(s.vels.size() == 2 && s.vels[0] == 1.0f && std::fabs(s.vels[1] - 64.0f / 127.0f) < 1e-6f);   // Vel 1
    }
    {
        Engine e(sines());
        Patch p = airOnly();
        p.air.listen = af::LI_FREE;
        p.air.gen.density = 60.0f;
        p.air.gen.gravity = 1.0f;
        p.harmony.key = 2;   // D: the tonic chord D F# A
        e.setPatch(p);
        Struck s;
        e.testAirHook(onStrike, &s);
        render(e, 3.0);
        CHECK(s.notes.empty());   // asleep until the first note
        e.noteOn(64, 100);
        e.noteOff(64);
        render(e, 30.0);
        std::printf("  Free in D, Gravity 1: %zu strikes, pitch classes %03x (D F# A: 244)\n", s.notes.size(), pcsOf(s.notes));
        CHECK(s.notes.size() >= 15 && pcsOf(s.notes) == ((1u << 2) | (1u << 6) | (1u << 9)));
        bool panned = true;   // generated notes: pans within +-Width
        for (size_t i = 0; i < s.pans.size(); ++i) panned = panned && !s.played[i] && std::fabs(s.pans[i]) <= p.air.voice.width;
        CHECK(panned);
    }
    // Air off (level 0) hears nothing: the keys strike nothing (no voice waits to ring when it is
    // turned up), and its generator is given no chord; turned up, it takes the chord there is then.
    {
        Engine e(sines());
        Patch p = airOnly();
        p.air.level = 0.0f;
        p.air.listen = af::LI_NOTES;
        p.air.gen.density = 60.0f;
        p.air.gen.gravity = 1.0f;
        e.setPatch(p);
        Struck s;
        e.testAirHook(onStrike, &s);
        e.noteOn(60, 100);
        render(e, 0.5);
        CHECK(s.notes.empty() && e.info().airActive == 0);
        p.air.listen = af::LI_HARMONY;
        e.setPatch(p);
        e.noteOn(65, 100);   // the harmony moves to F A C while Air is off
        render(e, 2.0);
        CHECK(s.notes.empty());
        p.air.level = 0.5f;
        e.setPatch(p);
        render(e, 20.0);
        std::printf("  Air turned up on the harmony: %zu strikes, pitch classes %03x (F A C: 221)\n", s.notes.size(),
                    pcsOf(s.notes));
        CHECK(s.notes.size() >= 10 && pcsOf(s.notes) == ((1u << 5) | (1u << 9) | (1u << 0)));
    }
}

// Check 3: Split. A key at or above it plays Air alone (its pan by its pitch), whatever Air's Listen:
// not Bloom, not the harmony, not Weather's notes; it wakes the engine. Under it, as before: Bloom,
// and not Air, though Air is on the notes.
void testSplit() {
    std::printf("== engine: Split\n");
    Engine e(sines());
    Patch p = dry();
    p.split = 72;
    p.air.listen = af::LI_NOTES;
    p.air.level = 0.5f;
    p.weather.listen = af::LI_NOTES;
    p.weather.level = 0.5f;
    e.setPatch(p);
    e.setWeatherSource(noiseSource());
    Struck s;
    e.testAirHook(onStrike, &s);
    const uint32_t v0 = e.info().harmonyVersion;
    e.noteOn(76, 100);
    render(e, 0.5);
    CHECK(s.notes == std::vector<int>{76} && s.played[0] && e.info().awake);
    CHECK(s.pans.size() == 1 && std::fabs(s.pans[0] - p.air.voice.width * 10.0f / 42.0f) < 1e-6f);   // E5: right of the middle
    CHECK(e.info().bloomActive == 0 && e.info().harmonyVersion == v0 && e.harmony().held() == 0 && !e.info().weatherGate);
    e.noteOff(76);
    e.noteOn(60, 100);
    render(e, 0.5);
    CHECK(e.info().bloomActive == 3 && e.info().harmonyVersion != v0 && s.notes.size() == 1 && e.info().weatherGate);
    // A key above Split at the bottom of the field, and one folded into Air's range.
    p.split = 0;
    e.setPatch(p);
    e.noteOn(20, 100);   // G#0, snapped (C major, the lower on a tie) to G0 (19), moved up an octave to G1
    render(e, 0.1);
    CHECK(s.notes.size() == 2 && s.pans.size() == 2 && s.notes[1] == 31 && s.pans[1] < 0.0f);
}

// Air's and Weather's level, mute, pan and Space send, as Ground's and Bloom's: the level scales the
// dry (exactly: two levels play the same grains and strikes), a mute glides to silence within 10 ms,
// hard right leaves the left channel empty; the send carries a tail into Space, and with Space's
// return at 0 none goes in (Space isn't run: the engine idles as soon as the stratum stops).
void testStrataMix() {
    std::printf("== engine: Air's and Weather's level, mute, pan and Space send\n");
    for (bool weather : {false, true}) {
        const char* name = weather ? "Weather" : "Air";
        // The stratum alone, on the notes, 20 dB down (the limiter idle).
        const auto patch = [weather](float level) {
            Patch p = quiet();
            p.volumeDb = -20.0f;
            if (weather) {
                p.weather.listen = af::LI_NOTES;
                p.weather.level = level;
            } else {
                p.air.listen = af::LI_NOTES;
                p.air.level = level;
                p.air.voice.decayS = 0.5f;
            }
            return p;
        };
        const auto start = [](Engine& e, const Patch& p) {
            e.setPatch(p);
            e.setWeatherSource(noiseSource());
            e.noteOn(69, 127);
        };
        const double from = weather ? 2.5 : 0.1, to = weather ? 3.5 : 0.4;   // past Weather's gate fade
        Engine full(sines()), part(sines());
        start(full, patch(1.0f));
        start(part, patch(0.25f));
        const Out of = render(full, to), op = render(part, to);
        const size_t a = static_cast<size_t>(from * af::kRate), b = static_cast<size_t>(to * af::kRate);
        const double ratio = db(rms(op.L, a, b) / rms(of.L, a, b));
        std::printf("  %s at level 0.25 against 1: %.3f dB\n", name, ratio);
        CHECK(rms(of.L, a, b) > 1e-4 && std::fabs(ratio + 12.0412) < 0.01);
        // Muted: gone within 10 ms (and a step), not rendered after.
        Patch m = patch(1.0f);
        if (weather) m.weather.mute = true;
        else m.air.mute = true;
        full.setPatch(m);
        render(full, 0.011);
        CHECK(!(weather ? full.info().weatherAudible : full.air().audible()) && peakOf(render(full, 0.2)) == 0.0f);
        // Hard right (past the pan's 10 ms glide from the middle, where the asleep engine left it).
        Engine pan(sines());
        Patch pr = patch(1.0f);
        (weather ? pr.weatherPan : pr.airPan) = 1.0f;
        start(pan, pr);
        const Out o = render(pan, to);
        CHECK(peak(o.L, 1000) == 0.0f && peak(o.R, a, b) > 1e-4);
        // The send, Space's return up: a tail after the stratum has stopped (Air rung out, Weather's
        // gate closed); the send at 0, nothing. Space's return at 0: idle once the stratum stops.
        double tail[2] = {};
        for (int k = 0; k < 2; ++k) {
            Engine s(sines());
            Patch ps = patch(1.0f);
            ps.spaceReturn = 1.0f;
            if (k == 0) (weather ? ps.weatherSpace : ps.airSpace) = 0.0f;
            start(s, ps);
            render(s, 3.0);
            s.noteOff(69);
            render(s, 3.0);
            CHECK(!(weather ? s.info().weatherAudible : s.air().audible()));
            tail[k] = rms(render(s, 1.0).L);
        }
        std::printf("  %s's send: the tail 3 s after it stopped %.1f dBFS (send 0: %.1f)\n", name, db(tail[1]), db(tail[0]));
        CHECK(tail[0] == 0.0 && tail[1] > 1e-6);
        Engine z(sines());
        Patch pz = patch(1.0f);   // Space's return at 0, the send at its default
        start(z, pz);
        render(z, 1.0);
        z.noteOff(69);
        render(z, 3.0);
        CHECK(z.info().idle);
        // The Echo send: Echo's repeats (Init's Echo, a dotted quarter, feedback 0.45) ring on after
        // the stratum has stopped; with the send at 0, nothing does.
        double echoed[2] = {};
        for (int k = 0; k < 2; ++k) {
            Engine s(sines());
            Patch ps = patch(1.0f);
            ps.echoReturn = 1.0f;
            (weather ? ps.weatherEcho : ps.airEcho) = k ? 1.0f : 0.0f;
            start(s, ps);
            render(s, weather ? 2.5 : 0.3);   // Weather past its gate's fade in
            s.noteOff(69);
            render(s, weather ? 2.2 : 0.7);
            CHECK(!(weather ? s.info().weatherAudible : s.air().audible()));
            echoed[k] = rms(render(s, 1.0).L);
        }
        std::printf("  %s's Echo send: %.1f dBFS RMS of repeats after it stopped (send 0: %.1f)\n", name, db(echoed[1]),
                    db(echoed[0]));
        CHECK(echoed[0] == 0.0 && echoed[1] > 1e-5);
    }
}

// Check 4: Weather's gate by its Listen, and To Key's chord. Notes: sounding while a key is held (a
// finger or the pedal), silent kGateS + 0.1 s after the last goes. Harmony: with Memory 1 bar, silent
// the bar plus kGateS after the release. Free: from the first note, nothing before.
void testWeatherGates() {
    std::printf("== engine: Weather's gate by Listen\n");
    const double gate = af::Weather::kGateS;
    {
        Engine e(sines());
        Patch p = weatherOnly();
        p.weather.listen = af::LI_NOTES;
        e.setPatch(p);
        e.setWeatherSource(noiseSource());
        e.noteOn(60, 100);
        const Out on = render(e, 3.0);
        CHECK(e.info().weatherGate && e.info().weatherAudible && rms(tail(on.L, 0.5)) > 1e-3);
        e.noteOff(60);
        const Out half = render(e, 1.0);
        CHECK(e.info().weatherAudible && rms(tail(half.L, 0.2)) > 1e-5);   // fading
        const Out off = render(e, gate - 1.0 + 0.1);
        CHECK(!e.info().weatherAudible && peak(tail(off.L, 0.05)) == 0.0f && peak(tail(off.R, 0.05)) == 0.0f);
        // Held by the pedal, it sounds on; the pedal up lets it go.
        e.sustain(true);
        e.noteOn(62, 100);
        e.noteOff(62);
        const Out ped = render(e, 3.0);
        CHECK(e.info().weatherAudible && rms(tail(ped.L, 0.5)) > 1e-3);
        e.sustain(false);
        render(e, gate + 0.1);
        CHECK(!e.info().weatherAudible);
    }
    {
        Engine e(sines());
        Patch p = weatherOnly();
        p.weather.listen = af::LI_HARMONY;
        p.harmony.memoryBars = 1;   // 2 s at 120
        e.setPatch(p);
        e.setWeatherSource(noiseSource());
        e.noteOn(60, 100);
        render(e, 1.0);
        e.noteOff(60);
        render(e, 1.8);
        CHECK(e.info().weatherGate && e.info().weatherAudible);   // the bar not over yet
        render(e, 0.2 + 1.0);
        CHECK(!e.info().weatherGate && e.info().weatherAudible);   // fading out
        render(e, gate - 1.0 + 0.1);
        CHECK(!e.info().weatherAudible);
    }
    {
        Engine e(sines());
        Patch p = weatherOnly();   // Weather's Listen is Free by default
        e.setPatch(p);
        e.setWeatherSource(noiseSource());
        CHECK(peakOf(render(e, 1.0)) == 0.0f && !e.info().weatherGate);
        e.noteOn(60, 100);
        e.noteOff(60);
        render(e, 3.0);
        const Out o = render(e, 10.0);
        CHECK(e.info().weatherGate && rms(tail(o.L, 1.0)) > 1e-3 && rms(o.L, 0, 44100) > 1e-3);
    }
    // To Key Chord on what the Listen gives: the held notes' pitch classes (Notes), the harmony's
    // chord (Harmony), the tonic chord (Free).
    struct Case {
        int listen, key;
        std::vector<int> keys;
        uint16_t want;
    };
    const Case cases[] = {{af::LI_NOTES, 0, {62, 69}, (1u << 2) | (1u << 9)},
                          {af::LI_HARMONY, 0, {60}, (1u << 0) | (1u << 4) | (1u << 7)},
                          {af::LI_FREE, 2, {64}, (1u << 2) | (1u << 6) | (1u << 9)}};
    for (const Case& c : cases) {
        Engine e(sines());
        Patch p = weatherOnly();
        p.weather.listen = c.listen;
        p.weather.toKey = af::TK_CHORD;
        p.harmony.key = c.key;
        p.harmony.input = af::IN_AS_PLAYED;
        e.setPatch(p);
        e.setWeatherSource(noiseSource());
        std::vector<float> semis;
        e.testWeatherHook(onSpawn, &semis);
        for (int k : c.keys) e.noteOn(k, 100);
        render(e, 2.0);
        std::printf("  To Key Chord, %s: %zu grains on %03x (want %03x)\n", af::kListenNames[c.listen], semis.size(),
                    pcsOf(semis), c.want);
        CHECK(semis.size() > 10 && pcsOf(semis) == c.want);
    }
}

// Check 5: Echo between the strata and Space. Ground alone into Echo (Space off): its repeat comes
// at the Echo's time, at the dry's level (no feedback's share in the first, cuts open, no wow or
// duck); the engine stays awake while the repeats ring and idles once Echo is silent. With every send
// at 0 Echo never runs (with its return up, or only Space to carry it while Space's return is 0).
void testEcho() {
    std::printf("== engine: Echo\n");
    Patch q = dry();
    q.bloom.mute = true;
    q.ground.listen = af::LI_NOTES;
    q.ground.fadeS = 0.05f;
    q.ground.breath = q.ground.beatHz = 0.0f;
    q.ground.sub = 0.0f;
    q.ground.registerOct = 3;
    af::Delay::Params& d = q.echo.delay;
    d.sync = false;
    d.timeMs = 300.0f;
    d.feedback = 0.3f;
    d.diffuse = d.wow = d.duck = 0.0f;
    d.lowCutHz = 20.0f;
    d.highCutHz = 20000.0f;
    q.echoReturn = 1.0f;
    q.groundEcho = 1.0f;
    Patch off = q;
    off.groundEcho = 0.0f;
    Engine e(sines()), r(sines());
    e.setPatch(q);
    r.setPatch(off);
    for (Engine* x : {&e, &r}) x->noteOn(60, 100);
    const Out a = render(e, 0.12), ra = render(r, 0.12);
    for (Engine* x : {&e, &r}) x->noteOff(60);
    const Out b = render(e, 0.88), rb = render(r, 0.88);
    const auto at = [](double s) { return static_cast<size_t>(s * af::kRate); };
    const double dry = rms(a.L, at(0.06), at(0.12)), repeat = rms(b.L, at(0.30 + 0.06 - 0.12), at(0.30 + 0.12 - 0.12));
    const double gap = rms(b.L, at(0.25 - 0.12), at(0.29 - 0.12));
    std::printf("  Ground into Echo at 300 ms: dry %.1f dBFS, its repeat %+.2f dB, between them %.1f dB\n", db(dry),
                db(repeat / dry), db(gap / dry));
    CHECK(std::fabs(db(repeat / dry)) < 1.0 && db(gap / dry) < -40.0);
    CHECK(rb.L.size() == b.L.size() && peak(tail(rb.L, 0.6)) == 0.0f);   // without the send: silence after the dry
    CHECK(r.info().echoRuns == 0 && r.info().echoSilent && e.info().echoRuns > 0);
    CHECK(!e.info().idle && !e.info().echoSilent);   // the repeats ring on
    render(e, 6.0);
    render(r, 6.0);
    CHECK(e.info().idle && e.info().echoSilent && r.info().idle);
    // Every send at 0: Echo isn't run, whatever its return.
    Engine z(sines());
    Patch zq = q;
    zq.groundEcho = 0.0f;
    zq.echoReturn = 1.0f;
    z.setPatch(zq);
    z.noteOn(60, 100);
    render(z, 2.0);
    CHECK(z.info().echoRuns == 0 && z.info().echoSilent);
    // A send heard only through Space, with Space's return at 0: nobody hears Echo, so it isn't run.
    zq.groundEcho = 1.0f;
    zq.echoReturn = 0.0f;
    zq.echoSpace = 1.0f;
    zq.spaceReturn = 0.0f;
    z.setPatch(zq);
    render(z, 1.0);
    CHECK(z.info().echoRuns == 0);
    zq.spaceReturn = 0.5f;   // heard through Space: it runs
    z.setPatch(zq);
    render(z, 1.0);
    CHECK(z.info().echoRuns > 0);
}

// Question (b) of the reviews: the engine tells Echo how long it skipped it, so its duck's envelope
// falls as far as a Delay run on through the pause would have let it. A loud burst at 20 ms with
// duck 1 and no feedback (Echo silent soon after, its envelope still near the burst's), 5 s of
// silence, then a quiet note: its first repeats come as loud as a fresh engine's (whose envelope
// never heard the burst), within 0.5 dB, where the envelope kept as it was put them 15 dB under.
// Echo's return alone: each render less the same render with the send at 0.
void testEchoRest() {
    std::printf("== engine: Echo told how long it was skipped\n");
    Patch q = dry();
    q.bloom.mute = true;
    q.ground.listen = af::LI_NOTES;
    q.ground.fadeS = 0.05f;
    q.ground.breath = q.ground.beatHz = 0.0f;
    q.ground.sub = 0.0f;
    q.ground.registerOct = 3;
    af::Delay::Params& d = q.echo.delay;
    d.sync = false;
    d.timeMs = 20.0f;
    d.feedback = 0.0f;
    d.duck = 1.0f;
    d.diffuse = d.wow = 0.0f;
    d.lowCutHz = 20.0f;
    d.highCutHz = 20000.0f;
    q.echoReturn = 1.0f;
    q.groundEcho = 1.0f;
    q.ground.level = 1.0f;
    Patch soft = q;
    soft.ground.level = 0.001f;   // -60 dB
    // The echo's return after a quiet note: with the loud burst and the pause before it, or alone.
    const auto run = [&](bool burst, bool send) {
        Engine e(sines());
        Patch p = burst ? q : soft, s = soft;
        if (!send) p.groundEcho = s.groundEcho = 0.0f;
        e.setPatch(p);
        if (burst) {
            e.noteOn(60, 100);
            render(e, 0.5);
            e.noteOff(60);
            render(e, 5.0);
            e.setPatch(s);
        }
        e.noteOn(62, 100);
        return render(e, 0.3);
    };
    const Out pa = run(true, true), pd = run(true, false), fa = run(false, true), fd = run(false, false);
    Buf ep(pa.L.size()), ef(fa.L.size());
    for (size_t i = 0; i < ep.size(); ++i) {
        ep[i] = pa.L[i] - pd.L[i];
        ef[i] = fa.L[i] - fd.L[i];
    }
    const size_t from = static_cast<size_t>(0.05 * af::kRate), to = static_cast<size_t>(0.15 * af::kRate);
    const double after = rms(ep, from, to), fresh = rms(ef, from, to);
    std::printf("  a quiet note's repeats 5 s after a loud burst: %.2f dB against a fresh engine's\n", db(after / fresh));
    CHECK(fresh > 0.0 && std::fabs(db(after / fresh)) < 0.5);
}

// Check 6: Remember into Weather. Play 4 s, remember(): Memory holds it (at the next control step),
// and Weather on Memory sounds from it, though the plugin gives it no source; reset() and the guard
// keep it, and Weather plays it again after both.
void testRemember() {
    std::printf("== engine: Remember into Weather\n");
    Engine e(defaults());
    Patch p = dry();
    p.weather.memory = true;
    e.setPatch(p);
    e.noteOn(60, 100);
    render(e, 4.0);
    CHECK(!e.info().remembered && e.info().memoryFill > 0.2f);
    e.remember();
    CHECK(!e.info().remembered);   // not yet: at the next control step
    render(e, 0.01);
    CHECK(e.info().remembered && e.info().memoryGeneration == 1 && e.info().memoryFill < 0.01f);
    // Weather alone, on Memory (Free: on since the first note).
    p.weather.level = 1.0f;
    p.ground.mute = p.bloom.mute = true;
    e.setPatch(p);
    const Out o = render(e, 3.0);
    std::printf("  Weather from the remembered 4 s: %.1f dBFS RMS\n", db(rms(tail(o.L, 1.0))));
    CHECK(e.info().weatherAudible && rms(tail(o.L, 1.0)) > 1e-3);
    e.reset();
    CHECK(e.info().remembered && e.info().memoryGeneration == 1 && e.info().memoryFill == 0.0f);
    e.noteOn(60, 100);
    CHECK(rms(tail(render(e, 3.0).L, 1.0)) > 1e-3);
    e.testInjectNaN();
    render(e, 128.0 / af::kRate);
    CHECK(e.info().guards == 1 && e.info().remembered && e.info().memoryGeneration == 1);
    const Out g = render(e, 3.0);
    CHECK(finiteOut(g) && rms(tail(g.L, 1.0)) > 1e-3);
    // Without Memory's (Weather on the plugin's source, none given), nothing.
    p.weather.memory = false;
    e.setPatch(p);
    render(e, 0.5);
    CHECK(peakOf(render(e, 0.5)) == 0.0f);
}

// A Remember while Weather plays Memory (memory.h: Weather renders between remember() and Memory's
// next write(), so its fading grains copy what they still read from the ring Memory records over
// next). Bloom's 440 Hz remembered; Weather (+12, hard left) plays it at 880 while Bloom (hard right)
// plays 550; Remember again: the new ring's left is Weather's own 880, so Weather moves to 1760 at
// the Remember's control step, the old grains fading over 20 ms without a step (no larger than the
// cloud's own), and 880 is gone from it.
void testRememberAgain() {
    std::printf("== engine: Remember while Weather plays Memory\n");
    Engine e(sines());
    Patch p = bloomSine();   // Bloom a sine a note, Space off
    p.weather.memory = true;
    p.weather.pitch = 12.0f;
    p.weather.spray = 0.0f;
    p.weather.width = 0.0f;
    p.weatherPan = -1.0f;
    e.setPatch(p);
    e.noteOn(69, 100);   // 440 on both sides
    render(e, 3.0);
    e.remember();
    e.noteOff(69);
    render(e, 0.01);
    CHECK(e.info().memoryGeneration == 1);
    p.weather.level = 1.0f;
    p.bloomPan = 1.0f;
    e.setPatch(p);
    e.noteOn(73, 100);   // C#5 554 Hz, on the right
    const Out before = render(e, 4.0);
    const Buf old = tail(before.L, 1.0);
    e.remember();
    const Out after = render(e, 2.0);
    CHECK(e.info().memoryGeneration == 2);
    const Buf near(after.L.begin(), after.L.begin() + 4410), late = tail(after.L, 1.0);
    const double was880 = magnitude(old, 880.0), was1760 = magnitude(old, 1760.0);
    const double now880 = magnitude(late, 880.0), now1760 = magnitude(late, 1760.0);
    const float own = std::max(maxStep(old), maxStep(late));
    std::printf("  Weather before: 880 Hz %.1f dB, 1760 %.1f dB; after: 880 %.1f dB, 1760 %.1f dB; the step %.4f (its own %.4f)\n",
                db(was880), db(was1760), db(now880), db(now1760), maxStep(near), own);
    CHECK(was880 > 0.01 && db(was1760 / was880) < -30.0);
    CHECK(now1760 > 0.01 && db(now880 / now1760) < -30.0);
    CHECK(maxStep(near) <= 1.5f * own);
}

// The order a Remember needs (memory.h): applied at a control step's start, before the strata, so
// Weather sees the new source and its fading grains copy what they still read from the old ring
// before Memory's next write() records over it (Memory writes last). Weather sounding on Memory,
// its grains (short, sixteen, no spray or drift) reading across the remembered ring's end into its
// first frames (the seam faded to nothing there: the frames a new recording takes first, at full
// level), at the middle of their windows; two Remembers 2 s apart: around each, no sample-to-sample
// step larger than the cloud's own.
// (Memory stages 128 frames before it records any, so it also holds with the Remember applied
// after Weather, or after the write: the order is the contract, the staging a margin. Without the
// staging, the order reversed clicks here.)
void testRememberOrder() {
    std::printf("== engine: two Remembers under a cloud on Memory\n");
    Engine e(sines());
    Patch p = bloomSine();
    p.weatherPan = -1.0f;
    p.weather.memory = true;
    p.weather.position = 0.996f;   // about 10 ms before the ring's end
    p.weather.spray = p.weather.drift = 0.0f;
    p.weather.width = 0.0f;
    p.weather.sizeS = 0.03f;
    p.weather.grains = 16;
    e.setPatch(p);
    e.noteOn(69, 100);   // 440 on both sides, remembered
    render(e, 3.0);
    e.remember();
    render(e, 0.01);
    p.weather.level = 1.0f;   // then Weather on the left (recording itself there), Bloom on the right
    p.bloomPan = 1.0f;
    e.setPatch(p);
    const Out before = render(e, 3.0);
    float own = maxStep(tail(before.L, 1.0)), worst = 0.0f;   // the cloud's own: before, and settled after each
    for (int k = 0; k < 2; ++k) {
        e.remember();
        const Out o = render(e, 2.1);
        worst = std::max(worst, maxStep(o.L, 0, static_cast<size_t>(0.1 * af::kRate)));
        own = std::max(own, maxStep(tail(o.L, 1.0)));
    }
    std::printf("  around the Remembers the largest step %.4f, the cloud's own %.4f (generation %u)\n", worst, own,
                e.info().memoryGeneration);
    CHECK(e.info().memoryGeneration == 3 && own > 0.01f && worst <= 1.25f * own);
}

// The loader's contract (plugin/loader.h, Task 7): Weather points into the plugin's source from block
// to block while it is audible, reads the old one once more in the first block given a new one, and
// holds none once on Memory or silent. holdsSource() says so (the loader's `holds`), and a source
// freed as the loader would free it (after the block that was given its replacement, or once nothing
// holds it) is never read again (ASan sees any read of the freed samples).
void testSourceHandoff() {
    std::printf("== engine: the plugin's source handed over, and let go\n");
    const auto source = [](uint32_t seed) {
        const Buf L = whiteNoise(44100, 0.5f, seed), R = whiteNoise(44100, 0.5f, seed + 1);
        return af::buildSource(L.data(), R.data(), static_cast<int>(L.size()));
    };
    std::unique_ptr<af::SourceBuffer> a = source(11), b = source(21), c = source(31);
    Engine e(sines());
    Patch p = weatherOnly();   // Free, level 0.5
    e.setPatch(p);
    float L[128], R[128];
    bool finite = true;
    float loud = 0.0f;
    const auto blocks = [&](const af::GrainSource* s, double seconds) {
        loud = 0.0f;
        for (int k = 0; k < static_cast<int>(seconds * af::kRate / 128.0); ++k) {
            e.setWeatherSource(s);
            e.render(L, R, 128);
            for (int i = 0; i < 128; ++i) {
                finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
                loud = std::max(loud, std::fabs(L[i]));
            }
        }
    };
    e.noteOn(60, 100);
    blocks(&a->src, 3.0);
    CHECK(e.holdsSource() && loud > 0.01f);
    blocks(&b->src, 128.0 / af::kRate);   // the first block given B: Weather copies from A in it
    a.reset();                            // ...and the loader frees A once that block has ended
    blocks(&b->src, 1.0);
    CHECK(e.holdsSource() && loud > 0.01f);
    // On Memory: Weather moves off B in the next block, and holds nothing of it after.
    e.remember();
    p.weather.memory = true;
    e.setPatch(p);
    blocks(&b->src, 128.0 / af::kRate);
    CHECK(e.info().remembered && !e.holdsSource());
    b.reset();
    blocks(nullptr, 1.0);
    CHECK(loud > 0.01f && e.info().weatherAudible);
    // Back on the plugin's source, then turned down: silent, it holds nothing.
    p.weather.memory = false;
    e.setPatch(p);
    blocks(&c->src, 1.0);
    CHECK(e.holdsSource());
    p.weather.level = 0.0f;
    e.setPatch(p);
    blocks(&c->src, 0.05);
    CHECK(!e.info().weatherAudible && !e.holdsSource());
    c.reset();
    blocks(nullptr, 0.5);
    CHECK(finite && loud == 0.0f);
}

// Check 7: Duck. Weather Free (hard right), Bloom (hard left) playing a chord now and then: with duck 1
// Weather's level while Bloom sounds is at least 10 dB under its level in a gap.
void testDuck() {
    std::printf("== engine: Weather ducks under Bloom\n");
    Engine e(sines());
    Patch p = dry();
    p.ground.mute = true;
    p.bloomPan = -1.0f;
    p.bloom.swellS = 0.05f;
    p.bloom.releaseS = 0.3f;
    p.weather.level = 0.5f;
    p.weatherPan = 1.0f;
    p.weather.duck = 1.0f;
    e.setPatch(p);
    e.setWeatherSource(noiseSource());
    e.noteOn(60, 100);   // wakes Weather (Free); Bloom only briefly
    e.noteOff(60);
    const Out gap = render(e, 5.0);
    e.noteOn(64, 100);
    const Out under = render(e, 2.0);
    const double inGap = rms(gap.R, 3 * 44100, 5 * 44100), whileBloom = rms(under.R, 22050, 88200);
    std::printf("  Weather under Bloom %.1f dB against a gap\n", db(whileBloom / inGap));
    CHECK(peak(under.L) > 0.1f && inGap > 1e-3 && db(whileBloom / inGap) < -10.0);
}

// Question (c) of the reviews: within one patch, Weather's gate before its level. Weather on Free,
// heard, then turned down to 0 (silent, its gate still open at 0 dB). A patch that shuts the gate
// (Listen moved to the notes with no key held) and raises the level at once leaves Weather silent;
// set() first would have played two seconds of the gate's fade out of a cloud nothing asked for.
void testGateBeforeLevel() {
    std::printf("== engine: Weather's gate before its level\n");
    Engine e(sines());
    Patch p = weatherOnly();
    e.setPatch(p);
    e.setWeatherSource(noiseSource());
    e.noteOn(60, 100);
    e.noteOff(60);
    CHECK(rms(tail(render(e, 3.0).L, 0.5)) > 1e-3);
    p.weather.level = 0.0f;
    e.setPatch(p);
    render(e, 0.5);
    CHECK(e.info().weatherGate && !e.info().weatherAudible);
    p.weather.listen = af::LI_NOTES;
    p.weather.level = 1.0f;
    e.setPatch(p);
    CHECK(!e.info().weatherGate && !e.info().weatherAudible);
    const Out o = render(e, 3.0);
    std::printf("  the gate shut and the level raised in one patch: peak %.1f dBFS after\n", db(peakOf(o)));
    CHECK(peakOf(o) == 0.0f);
}

// Everything on: Air generating and played above Split, Weather's 16 grains on To Key Chord with
// Duck, Echo at feedback 0.9 with wow, Diffuse and duck in Ping-Pong from every stratum, into Space;
// Memory on the strata.
Patch everything() {
    Patch p;
    p.volumeDb = 0.0f;
    p.split = 84;
    p.air.level = 0.6f;
    p.air.listen = af::LI_FREE;
    p.air.voice.sound = af::AS_BELL;
    p.air.voice.decayS = 8.0f;
    p.air.gen.density = 60.0f;
    p.air.gen.loop = true;
    p.air.gen.loopS = 3.0f;
    p.airPan = 0.3f;
    p.airEcho = 0.5f;
    p.weather.level = 0.6f;
    p.weather.listen = af::LI_HARMONY;
    p.weather.grains = 16;
    p.weather.toKey = af::TK_CHORD;
    p.weather.pitch = 12.0f;
    p.weather.duck = 0.5f;
    p.weather.tilt = 0.3f;
    p.weather.hpHz = 200.0f;
    p.weatherPan = -0.4f;
    p.weatherEcho = 0.5f;
    p.groundEcho = p.bloomEcho = 0.5f;
    af::Delay::Params& d = p.echo.delay;
    d.mode = af::Delay::PING_PONG;
    d.feedback = 0.9f;
    d.wow = d.diffuse = d.duck = 1.0f;
    p.echoReturn = 0.5f;
    p.echoSpace = 0.3f;
    p.memoryTap = af::MT_STRATA;
    return p;
}

// The same events play the same samples whatever MPC's block sizes (multiples of the control step:
// the events cut the pieces at their samples, the blocks never), everything on, and forty events at
// odd samples: notes (over and under Split), the pedal, four Remembers, Weather onto Memory and off
// it, its source dropped and given back, Air's and Weather's mutes, pans and Echo sends gliding,
// their Listen, Split, Memory's tap, the levels. Blocks off the grid (1, 33, 77, 100) play it finite,
// under the ceiling, every Remember taken.
void testBlocks() {
    std::printf("== engine: everything on, forty events, MPC's block sizes\n");
    struct Ev {
        size_t at;
        void (*act)(Engine&, Patch&);
    };
    const Ev ev[] = {
        {0, [](Engine& e, Patch&) { e.noteOn(60, 100); }},
        {1001, [](Engine& e, Patch&) { e.noteOn(64, 90); }},
        {4411, [](Engine& e, Patch&) { e.noteOn(90, 110); }},   // over Split: Air's
        {9999, [](Engine& e, Patch&) { e.sustain(true); }},
        {20001, [](Engine& e, Patch&) { e.noteOff(64); }},      // held by the pedal
        {26463, [](Engine& e, Patch&) { e.remember(); }},
        {30001, [](Engine& e, Patch& p) { p.airPan = -0.7f; e.setPatch(p); }},
        {33333, [](Engine& e, Patch& p) { p.weatherEcho = 0.9f; e.setPatch(p); }},
        {40007, [](Engine& e, Patch&) { e.sustain(false); }},
        {44101, [](Engine& e, Patch&) { e.noteOn(67, 100); }},
        {50003, [](Engine& e, Patch& p) { p.weather.memory = true; e.setPatch(p); }},
        {55555, [](Engine& e, Patch& p) { p.air.mute = true; e.setPatch(p); }},
        {61111, [](Engine& e, Patch& p) { p.groundPan = 0.5f; p.bloomEcho = 0.0f; e.setPatch(p); }},
        {66667, [](Engine& e, Patch& p) { p.weather.mute = true; e.setPatch(p); }},
        {70001, [](Engine& e, Patch&) { e.noteOn(88, 100); }},  // to a muted Air: nothing
        {72229, [](Engine& e, Patch& p) { p.air.mute = false; e.setPatch(p); }},
        {77777, [](Engine& e, Patch& p) { p.weather.mute = false; e.setPatch(p); }},
        {80003, [](Engine& e, Patch&) { e.setWeatherSource(nullptr); }},
        {85009, [](Engine& e, Patch& p) { p.weather.memory = false; e.setPatch(p); }},   // no source: fades
        {90001, [](Engine& e, Patch&) { e.noteOff(60); }},
        {95003, [](Engine& e, Patch&) { e.setWeatherSource(noiseSource()); }},
        {99991, [](Engine& e, Patch& p) { p.air.listen = af::LI_NOTES; e.setPatch(p); }},
        {105001, [](Engine& e, Patch& p) { p.split = -1; e.setPatch(p); }},
        {108003, [](Engine& e, Patch&) { e.noteOn(72, 80); }},   // Air on the notes: struck
        {116967, [](Engine& e, Patch&) { e.remember(); }},
        {120011, [](Engine& e, Patch& p) { p.weather.listen = af::LI_NOTES; e.setPatch(p); }},
        {125003, [](Engine& e, Patch& p) { p.memoryTap = af::MT_OUTPUT; e.setPatch(p); }},
        {130001, [](Engine& e, Patch& p) { p.weatherPan = 0.8f; p.airEcho = 0.0f; e.setPatch(p); }},
        {140009, [](Engine& e, Patch&) { e.noteOff(67); }},
        {150001, [](Engine& e, Patch& p) { p.air.level = 0.2f; p.weather.level = 0.3f; e.setPatch(p); }},
        {160007, [](Engine& e, Patch&) { e.noteOn(62, 100); }},
        {170003, [](Engine& e, Patch& p) { p.weather.memory = true; e.setPatch(p); }},
        {180001, [](Engine& e, Patch& p) { p.groundEcho = 0.9f; e.setPatch(p); }},
        {205211, [](Engine& e, Patch&) { e.remember(); }},
        {210011, [](Engine& e, Patch& p) { p.split = 70; e.setPatch(p); }},
        {215009, [](Engine& e, Patch&) { e.noteOn(79, 100); }},   // over Split again
        {230003, [](Engine& e, Patch& p) { p.memoryTap = af::MT_STRATA; e.setPatch(p); }},
        {260001, [](Engine& e, Patch&) { e.noteOff(62); }},
        {293443, [](Engine& e, Patch&) { e.remember(); }},
        {300007, [](Engine& e, Patch&) { e.noteOff(72); }},
    };
    const auto play = [&](const std::vector<int>& sizes, bool& remembered) {
        Engine e(sines());
        e.seed(77);
        Patch patch = everything();
        e.setPatch(patch);
        e.setWeatherSource(noiseSource());
        const size_t n = static_cast<size_t>(7.0 * af::kRate);
        Out o{Buf(n), Buf(n)};
        Clock t;
        t.playing = true;
        // MPC's blocks on their own grid (the sizes in turn), each cut where an event falls, as the
        // plugin cuts them: an event never moves the blocks after it.
        size_t pos = 0, next = 0, k = 0, blockEnd = 0;
        while (pos < n) {
            for (; next < sizeof ev / sizeof *ev && ev[next].at <= pos; ++next) ev[next].act(e, patch);
            if (pos == blockEnd) {   // a block: the transport once, at its first sample
                blockEnd += static_cast<size_t>(sizes[k++ % sizes.size()]);
                e.setTransport(t.bpm, static_cast<double>(pos) / af::kRate * t.bpm / 60.0, t.playing, true);
            }
            size_t m = blockEnd - pos;
            if (next < sizeof ev / sizeof *ev) m = std::min(m, ev[next].at - pos);
            m = std::min(m, n - pos);
            e.render(&o.L[pos], &o.R[pos], static_cast<int>(m));
            pos += m;
        }
        remembered = e.info().memoryGeneration == 4 && e.info().airStrikes > 3 && e.info().echoRuns > 0;
        return o;
    };
    bool r1 = false, r2 = false, r3 = false, r4 = false;
    const Out a = play({128}, r1), b = play({64}, r2), c = play({256, 32, 96, 128}, r3), d = play({1, 33, 77, 100}, r4);
    std::printf("  blocks of 128: peak %.3f, %.1f dBFS RMS; 64 and 32..256 the same: %d %d; 1, 33, 77, 100: %.1f dBFS RMS\n",
                peakOf(a), db(rms(a.L)), fingerprint(a) == fingerprint(b), fingerprint(a) == fingerprint(c), db(rms(d.L)));
    CHECK(r1 && r2 && r3 && r4 && finiteOut(a) && finiteOut(d));
    CHECK(fingerprint(a) == fingerprint(b) && fingerprint(a) == fingerprint(c));
    CHECK(peakOf(a) <= Engine::kCeiling && peakOf(d) <= Engine::kCeiling && rms(a.L) > 1e-2);
    CHECK(std::fabs(db(rms(d.L) / rms(a.L))) < 1.0);
}

// Check 8: the limiter and the guard with everything on, at its loudest: Air at 60 a minute, Weather's
// 16 grains, Echo at feedback 1 with Diffuse from every stratum, on top of M1's loudest. The peak never
// passes -1 dBFS over 60 s; a NaN in Echo's return is one guard trip, then finite again.
void testEverythingLoud() {
    std::printf("== engine: the limiter and the guard with everything on\n");
    Engine e(saws());
    Patch p = loudest(false);
    p.space.reverb.freeze = false;
    const Patch all = everything();
    p.air = all.air;
    p.air.level = 1.0f;
    p.air.listen = af::LI_HARMONY;
    p.weather = all.weather;
    p.weather.level = 1.0f;
    p.echo = all.echo;
    p.echo.delay.feedback = 1.0f;
    p.groundEcho = p.bloomEcho = p.airEcho = p.weatherEcho = 1.0f;
    p.echoReturn = p.echoSpace = 1.0f;
    p.airSpace = p.weatherSpace = 1.0f;
    e.setPatch(p);
    e.setWeatherSource(noiseSource());
    float minGain = 1.0f;
    bool finite = true;
    const float worst = playChords(e, 60.0, 5.0, &minGain, &finite);
    std::printf("  60 s: peak %.5f (%.2f dBFS), the limiter down to %.1f dB, Air %llu strikes\n", worst, db(worst), db(minGain),
                static_cast<unsigned long long>(e.info().airStrikes));
    CHECK(finite && worst <= 0.8913f && e.info().guards == 0 && e.info().airStrikes > 40 && e.info().weatherAudible);
    const uint32_t trips = af::guardTrips();
    e.testInjectEchoNaN();
    const Out hit = render(e, 128.0 / af::kRate);
    CHECK(peakOf(hit) == 0.0f && finiteOut(hit) && e.info().guards == 1 && af::guardTrips() - trips == 1);
    const Out after = render(e, 2.0);
    CHECK(finiteOut(after) && e.info().guards == 1 && peakOf(after) > 0.0f);
}

// The author's decision: Memory keeps recording across Stop and a suspend. Played 4 s, then Stop
// (Fade, after its fade has put the engine to sleep; Cut; Keep) or a long suspend: a Remember is
// taken (the generation moves) and the remembered source holds what was played. A guard trip and
// CC 120 (reset()) start the ring afresh: a Remember then is refused (under 0.5 s recorded).
void testMemoryAcrossStop() {
    std::printf("== engine: Memory across Stop and a suspend\n");
    enum Way { FADE, CUT, KEEP, SUSPEND, GUARD, CC120 };
    const char* names[] = {"Fade", "Cut", "Keep", "a long suspend", "a guard trip", "CC 120"};
    for (int way = FADE; way <= CC120; ++way) {
        Engine e(defaults());
        Patch p = dry();
        p.onStop = way == CUT ? af::OS_CUT : way == KEEP ? af::OS_KEEP : af::OS_FADE;
        e.setPatch(p);
        Clock t;
        t.playing = true;
        e.noteOn(60, 100);
        render(e, 4.0, &t);
        switch (way) {
            case FADE:
            case CUT:
            case KEEP:
                t.playing = false;
                render(e, way == FADE ? Engine::kFadeS + 0.5 : 0.1, &t);
                break;
            case SUSPEND:
                e.suspend();
                e.resume(2.0);
                break;
            case GUARD:
                e.testInjectNaN();
                render(e, 128.0 / af::kRate, &t);
                break;
            default: e.reset(); break;
        }
        const bool asleep = !e.info().awake;
        e.remember();
        render(e, 0.01, &t);
        const bool taken = e.info().memoryGeneration == 1;
        double level = 0.0;
        if (const af::GrainSource* s = e.memory().remembered()) {
            double sum = 0.0;
            for (int f = 0; f < s->frames; ++f) sum += static_cast<double>(s->level[0][2 * f]) * s->level[0][2 * f];
            level = std::sqrt(sum / std::max(1, s->frames)) * s->gain;
        }
        std::printf("  %-14s %s; Remember %s, %.1f dBFS RMS remembered\n", names[way], asleep ? "asleep" : "awake",
                    taken ? "taken" : "refused", db(level));
        if (way == GUARD || way == CC120) CHECK(!taken && !e.info().remembered);
        else CHECK(taken && level > 1e-3 && asleep == (way != KEEP));
    }
}

// A sleep that keeps the ring (Stop's Cut, the end of its Fade, a long suspend) cuts the sound
// mid-way, at the Fade's end at full level (the tap is before the output's fade): Memory seals the
// ring there (memory.h: Sleeps). Bloom's 440 Hz sine played 1 s, the sleep, the sine struck again
// and played 1 s, then remembered: at every level the join's largest step is at most the sine's own
// (its steady parts, both sides).
void testMemoryJoinAfterSleep() {
    std::printf("== engine: Memory's join after a sleep\n");
    enum Way { CUT, FADE, SUSPEND };
    const char* names[] = {"Cut", "the Fade's end", "a long suspend"};
    for (int way = CUT; way <= SUSPEND; ++way) {
        Engine e(defaults());
        Patch p = bloomSine();
        p.onStop = way == CUT ? af::OS_CUT : af::OS_FADE;
        e.setPatch(p);
        Clock t;
        t.playing = true;
        e.noteOn(69, 100);
        render(e, 1.0, &t);
        if (way == SUSPEND) {
            e.suspend();
            e.resume(2.0);
        } else {
            t.playing = false;
            render(e, way == FADE ? Engine::kFadeS + 0.5 : 0.1, &t);
        }
        const bool asleep = !e.info().awake;
        const int join = static_cast<int>(std::lround(e.info().memoryFill * af::Memory::kFrames));   // frames before it
        t.playing = true;
        e.noteOn(69, 100);
        render(e, 1.0, &t);
        e.remember();
        render(e, 0.01, &t);
        const af::GrainSource* s = e.memory().remembered();
        CHECK(asleep && s && e.info().memoryGeneration == 1);
        if (!s) continue;
        std::printf("  %-14s (the join at frame %d):", names[way], join);
        bool ok = true;
        for (int k = 0; k < af::GrainSource::kLevels; ++k) {
            Buf x(static_cast<size_t>(s->frames >> k));   // not full: in order from frame 0
            for (size_t i = 0; i < x.size(); ++i) x[i] = s->level[k][2 * i] / 32768.0f;
            const size_t j = static_cast<size_t>(join >> k), reach = static_cast<size_t>(300 >> k);
            const size_t far = static_cast<size_t>(8000 >> k), near = static_cast<size_t>(2000 >> k);
            const float at = maxStep(x, j - reach, j + reach);
            const float own = std::max(maxStep(x, j - far, j - near), maxStep(x, j + near, j + far));
            std::printf("  %d: %.4f of %.4f", k, at, own);
            ok = ok && at <= own;
        }
        std::printf("\n");
        CHECK(ok);
    }
}

// The author's decision: Air lets its voices go once its level has glided to 0 (or is set to 0 where
// it already is). Two Felt notes at Decay 20, Air muted a second in: within 10 ms (and a step) none
// rings, in Info or the meter's count; unmuted a minute later, nothing comes back. And a note struck
// as the level was raised, before any render, goes when the level is set back to 0 there.
void testAirLetsGo() {
    std::printf("== engine: Air lets its voices go at level 0\n");
    Engine e(sines());
    Patch p = airOnly();
    p.air.listen = af::LI_NOTES;
    p.air.voice.sound = af::AS_FELT;
    p.air.voice.decayS = 20.0f;
    e.setPatch(p);
    e.noteOn(72, 100);
    e.noteOn(76, 100);
    e.noteOff(72);
    e.noteOff(76);
    const Out on = render(e, 1.0);
    CHECK(e.info().airActive == 2 && e.activeVoices() == 2 && rms(tail(on.L, 0.2)) > 1e-3);
    p.air.mute = true;
    e.setPatch(p);
    render(e, 0.011);
    CHECK(e.info().airActive == 0 && e.activeVoices() == 0);
    render(e, 60.0);
    p.air.mute = false;
    e.setPatch(p);
    const Out back = render(e, 1.0);
    std::printf("  unmuted after a minute: %.1f dBFS peak\n", db(peakOf(back)));
    CHECK(peakOf(back) == 0.0f && e.info().airActive == 0);
    // Set to 0 where the level already is.
    p.air.level = 0.0f;
    e.setPatch(p);
    render(e, 0.1);
    p.air.level = 0.5f;
    e.setPatch(p);
    e.noteOn(74, 100);   // struck: the level is on its way up
    CHECK(e.info().airActive == 1);
    p.air.level = 0.0f;
    e.setPatch(p);       // back to 0 before any render: nothing to glide down
    CHECK(e.info().airActive == 0);
}

// The Remember's order, caught where it acts: a Remember applies at a control step's start, so the
// first grain Weather (on Memory) starts after the generation moves starts before Memory has written
// anything into its new ring (fill() 0). Applied after Weather's render, Memory would write the piece
// first. Three Remembers 2.1 s apart under a cloud of 8 grains of 30 ms on Memory (8 of the 16
// voices free, so a grain starts at the source change itself: every voice busy fading would hold
// the new grains back a step or two).
void testRememberBeforeWeather() {
    std::printf("== engine: a Remember before Weather renders\n");
    Engine e(sines());
    Patch p = bloomSine();
    p.weatherPan = -1.0f;
    p.weather.memory = true;
    p.weather.sizeS = 0.03f;
    p.weather.grains = 8;
    p.weather.width = 0.0f;
    e.setPatch(p);
    e.noteOn(69, 100);
    render(e, 3.0);
    e.remember();
    render(e, 0.01);
    struct Watch {
        const Engine* e;
        uint32_t gen;
        int checked, bad;
    } w{&e, e.info().memoryGeneration, 0, 0};
    e.testWeatherHook(
        [](void* ctx, float) {
            Watch& x = *static_cast<Watch*>(ctx);
            const Engine::Info i = x.e->info();
            if (i.memoryGeneration == x.gen) return;
            x.gen = i.memoryGeneration;
            ++x.checked;
            if (i.memoryFill != 0.0f) ++x.bad;
        },
        &w);
    p.weather.level = 1.0f;
    p.bloomPan = 1.0f;
    e.setPatch(p);
    render(e, 2.1);
    for (int k = 0; k < 3; ++k) {
        e.remember();
        render(e, 2.1);
    }
    std::printf("  %d Remembers seen at a grain's start, %d with the new ring already written\n", w.checked, w.bad);
    CHECK(w.checked == 3 && w.bad == 0 && e.info().memoryGeneration == 4);
}

// reset() and the guard reset Air, Weather and Echo: silent at once, nothing left ringing.
void testResets() {
    std::printf("== engine: reset() and the guard silence Air, Weather and Echo\n");
    for (bool guard : {false, true}) {
        Engine e(sines());
        Patch p = everything();
        p.space.rise = 0.0f;
        e.setPatch(p);
        e.setWeatherSource(noiseSource());
        e.noteOn(60, 100);
        e.noteOn(90, 100);
        render(e, 3.0);
        CHECK(e.info().airActive > 0 && e.info().weatherAudible && !e.info().echoSilent);
        if (guard) e.testInjectNaN();
        else e.reset();
        render(e, 128.0 / af::kRate);
        CHECK(e.info().airActive == 0 && !e.info().weatherAudible && e.info().echoSilent);
        CHECK(guard ? e.info().awake && e.info().guards == 1 : !e.info().awake);
    }
}

} // namespace

void engineTests() {
    testOffIsOff();
    testSilence();
    testDefaults();
    testBloomHarmony();
    testFree();
    testSnap();
    testHold();
    testStop();
    testMix();
    testIdle();
    testListenChanges();
    testGuard();
    testClock();
    testSyncFromSilence();
    testRise();
    testLimiter();
    testLimiterRelease();
    testManyKeys();
    testFreezeTail();
    testHoldJoins();
    testGlides();
    testStopChanges();
    testAirListen();
    testSplit();
    testStrataMix();
    testWeatherGates();
    testEcho();
    testEchoRest();
    testRemember();
    testRememberAgain();
    testRememberOrder();
    testSourceHandoff();
    testDuck();
    testGateBeforeLevel();
    testBlocks();
    testEverythingLoud();
    testResets();
    testMemoryAcrossStop();
    testMemoryJoinAfterSleep();
    testAirLetsGo();
    testRememberBeforeWeather();
    testCost();
}

} // namespace aft
