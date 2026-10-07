// The engine on its own (dsp/engine.h): Listen routing from the harmony to Ground and Bloom, the
// keys, the pedal and Hold, Stop, the mix and the output (tilt, volume, the limiter, the guard), the
// clock and the cost. Only signal.h's helpers, so `make test-module M=engine` builds it alone.
#include "check.h"
#include "signal.h"
#include "../dsp/engine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
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
    const Out hit = render(e, 128.0 / af::kRate);
    CHECK(peakOf(hit) == 0.0f && finiteOut(hit) && e.info().guards == 1);
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

    // The decay hold: Tail Space holds the reverb at Release or longer, except in Abyss.
    Patch d;
    d.space.reverb.decayS = 4.0f;
    d.bloom.releaseS = 12.0f;
    e.setPatch(d);
    CHECK(e.info().spaceDecayS == 12.0f);
    d.bloom.tail = af::TL_VOICE;
    e.setPatch(d);
    CHECK(e.info().spaceDecayS == 4.0f);
    d.bloom.tail = af::TL_SPACE;
    d.space.reverb.mode = af::Reverb::ABYSS;
    e.setPatch(d);
    CHECK(e.info().spaceDecayS == 4.0f);
}

// Rise (space.cpp's kFull) against a real pad's send: the Init levels (the knobs' defaults,
// squared), a triad. The wet is the output with the return on less the output with it off (the
// same seed and notes; nothing else differs while the chord is held).
void testRise() {
    std::printf("== engine: Rise against the Init pad's send\n");
    double wet[2] = {};
    for (int k = 0; k < 2; ++k) {
        Patch p;
        p.volumeDb = 0.0f;
        p.ground.level = p.bloom.level = 0.49f;
        p.groundSpace = 0.16f;
        p.bloomSpace = 0.25f;
        p.spaceReturn = 0.64f;
        p.space.reverb.decayS = 8.0f;
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

// Check 11 (not a gate): the worst case's cost on this machine.
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

} // namespace

void engineTests() {
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
    testRise();
    testLimiter();
    testLimiterRelease();
    testManyKeys();
    testCost();
}

} // namespace aft
