# M1 "First light" implementation plan

> **For Claude:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (this session)
> or superpowers:executing-plans to implement this plan task by task.

**Goal:** a playable AmbientForce on the Force. One pad plays a Bloom chord over a Ground drone
that follows the harmony, in just intonation, into a Space reverb with Haze and Abyss voicings.
It has the M1 pages, 16 factory presets, and a CPU gate of ≤15% p99.

**Architecture:** SubForce's plugin side (VST2 glue, surface, presets, tests, tools, CI), copied and
renamed, the way EffectForce was made from SubForce. The page machinery with sub-page groups
comes from EffectForce's `surface.py` and `skin_polish.py`. EffectForce's Reverb, ported. The
wavetable memory layout comes from PolyForce. New code: the harmony brain, lifetime tables and
their oscillator, Ground, Bloom, and the engine that routes one MIDI stream through Listen modes.

**Tech stack:** C++17, GNU make in WSL Ubuntu (`g++` 13 for x86 tests and ASan;
`arm-linux-gnueabihf-g++` and `qemu-arm` for ARM), Python 3 for `surface.py` (Pillow for the skin,
from `$(HOME)/.venvs/rackforce`), hand-written VST2.

**Source documents:** [`docs/CONCEPT.md`](../CONCEPT.md) (what and why). The sibling repos are
the references for every convention:
- `D:\DEV\SubForce`: the instrument template.
- `D:\DEV\EffectForce`: the reverb, the sub-page groups, the newest conventions.
- `D:\DEV\PolyForce`: the wavetables and the loader.
- `D:\DEV\mockba\RackForcePlugin\docs\MPC_PLUGIN_SPEC.md`: the device-verified host facts.

---

## Ground rules for every task

- **Running commands:** from Windows, every build and test runs in WSL:
  `wsl -e bash -lc 'cd /mnt/d/DEV/AmbientForce && make test'`. The toolchain is already
  installed there. `local.mk` (git-ignored) holds `FORCE`, `SSH_KEY` and `PY`, copied from
  SubForce's.
- **Real-time rules** (from the family's ARCHITECTURE docs):
  - Nothing on the audio thread allocates, locks or throws.
  - Host callbacks only from `processReplacing`.
  - A `try`/`catch` at every entry point.
  - Getters read caches only.
  - Every `dsp/` class allocates in its constructor or `prepare()`, never in `render()`/`process()`.
- **Time:** any sample counter that can run for hours is 64-bit (the PolyForce hang lesson).
  Delay and line lengths in samples are `double`, or integers plus a float fraction (the EffectForce
  float-stall lesson).
- **Namespaces and names:**

  | | |
  |---|---|
  | Namespace | `af` (tests: `aft`) |
  | Environment variables | `AF_FIXED_SEED`, `AF_PRESET_ROOTS`, `AF_DATA_DIR` |
  | Trace file | `/tmp/ambientforce.trace` |
  | Plugin and bench | `ambientforce.so`, `afbench` |
  | Plugin identity | uid `AmFc`, vendor `Devko`, bundle folder `Devko - VST - AmbientForce` |
  | Presets | `.afp`, magic `ambientforce 1` |
  | Accent colour | mist blue `8ab4f8` |

- **Code copied from a sibling** gets a first-line provenance comment. For example:
  `// From EffectForce dsp/reverb.h (4160e87), namespace ef -> af; Haze and Abyss added.`
- **Comments and docs** follow the family's voice: plain sentences that explain why. **No hardware
  product names** in code or user docs (CONCEPT.md §17, decision 7).
- **TDD:** write the checks first, run them and watch them fail, implement, then watch them pass.
  Checks use `CHECK()` from `test/check.h`. Each suite is a `void xxxTests()` called from
  `plugin_test.cpp`'s `main` (the SubForce pattern).
- **Commits:** one per task at least, with a message in the family's style: a subject line, then
  a body explaining what and why, ending with:

  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```

  Commit with Git Bash: `git commit -F - <<'EOF' … EOF`.
- **Done means:** `make test` passes under ASan/UBSan with no warnings in our code, and
  `make test-arm` passes under qemu.

---

## Task 1: Scaffold from SubForce

**Goal:** an AmbientForce that builds, loads in the fake host, passes its tests and has a skin. The
engine is a stub that plays a sine per note, so there is something to hear and test.

**Files:**
- **Copy from SubForce:**
  - `Makefile`
  - `.github/workflows/build.yml`
  - `plugin/*` (all)
  - `test/check.h`, `test/host.h`, `test/plugin_test.cpp`, `test/preset_test.cpp`
  - `tools/bench.cpp`, `tools/pgo_train.cpp`, `tools/demos.cpp`
  - `surface/surface.py`, `surface/skin_polish.py`, `surface/fonts/*`
  - `presets/Factory/01_Templates/01_Init.sfp` → `01_Init.afp`
  - `third_party/mpc-vst-plugins/`
- **Take from EffectForce:**
  - `surface/skin_polish.py`, plus the page-group machinery in `surface.py`: `Layout.group()`,
    `Layout.page()`, and `skin_style()` with `tab_groups`.
  - `third_party/mpc-vst-plugins/` instead of SubForce's copy, if its marked local patches are a
    superset. Diff the two READMEs' patch lists, and keep the one with all of them.
- **Create:** `dsp/engine.h`, `dsp/engine.cpp` (the stub), `dsp/stages.h` (from SubForce,
  renamed), `local.mk` (git-ignored), and `README.md` (a short stub; Task 12 writes the real one).
- **Delete** everything SubForce-specific: `dsp/synth.*`, `osc.h`, `ladder.h`, `halfband.h`,
  `env.h`, `mod.h`, the `keys_test`, `mod_test` and `engine_test` suites, and
  `tools/halfband_design.py`.

**Steps:**
1. **Copy and rename.** Change `SubForce` → `AmbientForce`, `subforce` → `ambientforce`,
   `sf::` / `namespace sf` → `af`, `sft` → `aft`, `SF_` → `AF_`, `.sfp` → `.afp`, `SbFc` → `AmFc`,
   `sfbench` → `afbench`, `SubForceStageTimes` → `AmbientForceStageTimes`. Use the accent palette
   from EffectForce's `PALETTE`, re-coloured to `8ab4f8`, with `accent_hi` `c6dcff`,
   `seg_active_tx` `0d1b33` and `tile_on` `1f2d45`.
2. **Write the stub engine** in `dsp/engine.h`. It keeps `sf::Synth`'s public API, so
   `plugin/plugin.cpp` changes only in its type names:

   ```cpp
   namespace af {
   struct Patch { float volumeDb = -6.0f; };
   class Engine {
   public:
       explicit Engine(float sampleRate = 44100.0f);
       void setPatch(const Patch& p);
       void noteOn(int note, int velocity);   // velocity 0 = note off
       void noteOff(int note);
       void pitchBend(float) {}
       void sustain(bool down);
       void allNotesOff();
       void reset();
       void controller(int, int) {}
       void aftertouch(float) {}
       void polyAftertouch(int, float) {}
       void resetControllers() {}
       void seed(uint32_t s);
       void setTransport(double bpm, double beats, bool playing, bool beatsValid);
       void render(float* outL, float* outR, int n);   // overwrites n samples
       int  activeVoices() const;
   };
   }
   ```

   Stub behaviour: up to 6 sine voices with a 10 ms attack and 300 ms release.
3. **Cut `surface.py`** down to: `status` (index 0), `volume`, the preset stepper and buttons,
   `rand_amt`, and the BROWSE tiles. Its pages are one group BROWSE with one page PRESETS (the
   SubForce BROWSE layout, made a `page()` of a `group()`). `plugin/patch_map.cpp` maps `volume`
   only. The `static_assert`s that mirrored SubForce's lists go.
4. **Cut the tests.** `plugin_test.cpp` and `preset_test.cpp` lose their SubForce-specific
   checks, but keep every generic one: chunks round-trip, every getter is safe for every index,
   the browser, preset save and load, button taps, stepper detents, non-finite host values
   ignored. Add `test/engine_test.cpp` with `void engineTests()`:
   - **Silence:** with no notes held, output is exactly 0.
   - **Note:** note 69 at velocity 100 → a 440 Hz peak (Hann correlation as in SubForce's
     `toneAmp`, at least 20 dB above 430 Hz and 450 Hz).
   - **Note-off:** output < 1e-6 within 0.5 s of note-off.
   - **Six voices:** 6 voices sound at once; a 7th note steals.
5. **Adjust the Makefile:** `SRC` and target names, `PLUGIN_VERSION ?= 0.0.1`,
   `--repo Devko/AmbientForce`, the `--about` text ("AmbientForce ambient instrument (preview):
   …"), and the `make loudness`/`demos` paths. Point CI at the new names.
6. **Run, and fix until all pass:**
   - `make test`: ASan suite passes.
   - `make test-arm`: passes under qemu.
   - `make arm-plugin`: profile-guided build, "exported symbols: 1".
   - `make skin preview`: writes `surface/build/page_*.png`.
   - `make bench`: the x86 bench runs.
7. **Look at** `surface/build/page_1.png` (Read the PNG) and confirm the BROWSE page renders in
   mist blue.
8. **Commit:** "Scaffold: SubForce's plugin side, renamed; a stub engine".

---

## Task 2: Space, the reverb ported, with Haze and Abyss

**Files:**
- **Copy from EffectForce, `ef` → `af`, with provenance lines:** `dsp/common.h`, `dsp/simd.h`
  (EffectForce's version replaces SubForce's), `dsp/fastmath.h`, `dsp/pitch.h`, `dsp/svf.h`,
  `dsp/reverb.h`, `dsp/reverb.cpp`.
- **Copy:** `test/signal.h`, and the reverb checks in `test/reverb_test.cpp` (EffectForce's
  suite, trimmed to the checks that test the Reverb class itself, not the rack).
- **Create:** `dsp/space.h` and `dsp/space.cpp`.

**Reverb changes:**
- **Two new modes** at the end of `enum Mode`: `HAZE` and `ABYSS` (`kModes` = 6). The existing
  modes stay bit for bit the same, and their ported tests must pass unchanged.
- **Haze:** veiled and distant. Space's line lengths (75..250 ms), the strongest input diffusion
  of all modes, modulation depth 1.5× Space's, and the damping frequency multiplied by 0.5 (darker
  than asked).
- **Abyss:** near-infinite. Line lengths 1.3× Space's, Hall's diffusion, Space's modulation, and
  the decay time multiplied by 4. Because of that factor, `decayS` 30 gives RT60 = 120 s. Clamp the
  loop gain below 1 - 1e-6 so it never grows.

**`dsp/space.h`:** the send/return wrapper the engine uses.

```cpp
namespace af {
class Space {
public:
    struct Params {
        Reverb::Params reverb;   // mix is forced to 1 (wet only): Space is a return, the dry is elsewhere
        float rise = 0.2f;       // 0..1: the wet ducks under the send's level and blooms after it
    };
    Space();
    void reset();
    void set(const Params& p, const Transport& t);
    // sendL/R: the strata's summed sends; out: the wet return (overwritten). n <= 128.
    void process(const float* sendL, const float* sendR, float* outL, float* outR, int n);
    bool silent() const;   // the tail is under -120 dBFS and nothing is coming in
};
}
```

**Rise:**
- An envelope follower on the send, mono `max(|L|,|R|)`, 5 ms attack and 400 ms release.
- The wet gain is `1 - rise * min(1, env / 0.25)`, smoothed over 20 ms.
- At rise 0 it is bit for bit the plain reverb.

**Checks** (in `test/reverb_test.cpp`, `void reverbTests()`):
1. Every ported EffectForce reverb check passes.
2. **Haze RT60:** within ±10% of `decayS` at 1 kHz for decay 4 s. Its spectral centroid of the
   tail at 2 s is lower than Space mode's under the same settings.
3. **Abyss:** `decayS` 5 measures RT60 between 18 and 22 s.
4. **No mode grows:** for every mode, with decay at its maximum and freeze off, a 1 s burst
   produces an output peak in the last second of 60 s no higher than the peak at second 2.
5. **Rise:** with rise 1, the wet RMS during a 2 s steady send is at least 12 dB below the wet RMS
   in the 2 s after the send stops. With rise 0, the output is identical to `Reverb::process` with
   mix 1.
6. **`silent()`:** true 0.5 s after a 0.3 s decay tail ends; false while frozen with a tail.

**Commit:** "Space: EffectForce's reverb with Haze and Abyss, as a send/return with Rise".

---

## Task 3: The harmony brain

This is pure logic, so it is ideal for TDD.

**Files:** create `dsp/harmony.h`, `dsp/harmony.cpp`, `test/harmony_test.cpp`.

**Interface:**

```cpp
namespace af {
enum Scale : int { SC_MAJOR, SC_MINOR, SC_DORIAN, SC_LYDIAN, SC_MIXOLYDIAN, SC_PHRYGIAN, SC_MAJ_PENT,
                   SC_MIN_PENT, SC_HIRAJOSHI, SC_IN_SEN, SC_WHOLE_TONE, SC_CHROMATIC, SC_COUNT };
enum Tuning : int { TU_EQUAL, TU_JUST, TU_PYTHAGOREAN, TU_COUNT };
enum Input : int { IN_AS_PLAYED, IN_SNAP, IN_DEGREES, IN_COUNT };
enum ChordType : int { CH_OFF, CH_TRIAD, CH_SEVENTH, CH_SUS2, CH_SUS4, CH_ADD9, CH_QUARTAL, CH_FIFTHS,
                       CH_CLUSTER, CH_SPREAD, CH_COUNT };
enum Voicing : int { VO_CLOSE, VO_OPEN, VO_DROP2, VO_SPREAD, VO_COUNT };

struct HarmonyPatch {
    int key = 0;              // 0 = C .. 11 = B: the tonic's pitch class
    int scale = SC_MAJOR;
    int tuning = TU_JUST;
    int input = IN_SNAP;
    int chord = CH_TRIAD;
    int voicing = VO_OPEN;
    bool leading = true;
    float strumS = 0.0f;      // 0..2: the chord's notes enter one after another, low to high
    int memoryBars = -1;      // 0: off; 1..64: bars after the last release; -1: forever
};

constexpr int kChordMax = 6;  // notes in a voicing (Bloom has 6 voices)

struct Chord {
    int n = 0;
    int notes[kChordMax] = {};  // MIDI notes, ascending
    int root = -1;              // MIDI note of the chord's root (-1: no chord)
    uint16_t pcs = 0;           // pitch-class set, bit i = pitch class i
};

// Scale tables (semitones above the tonic, ascending); scaleSize() 5..12.
int scaleSize(int scale);
int scaleStep(int scale, int degree);           // degree 0..size-1

// Input mapping. -1 = drop the note (never happens for these modes; reserved).
int mapInput(const HarmonyPatch& h, int note);

// The chord on `root` (already mapped): its scale degrees stacked by the chord type, diatonic to
// h.scale (Chromatic: the major scale on the root), voiced by h.voicing near the root.
Chord buildChord(const HarmonyPatch& h, int root);

// Voice leading: among the inversions and octave placements of `c` (same pitch classes, root
// kept the lowest only for VO_SPREAD), the one with the least cost from `prev`. prev.n == 0: c as is.
Chord leadFrom(const HarmonyPatch& h, const Chord& prev, const Chord& c);   // keeps h.voicing's shape
int voiceLeadCost(const Chord& prev, const Chord& candidate);   // lower = smoother

// Pitch of a MIDI note under the tuning, in (fractional) semitones, so 69.0 = A4 = 440 Hz under
// Equal. Just and Pythagorean are relative to the key's tonic: the tonic stays where Equal has it.
double tunedPitch(const HarmonyPatch& h, int note);

// The harmony memory: what Ground and Bloom (Harmony mode) follow.
class Harmony {
public:
    void set(const HarmonyPatch& h);
    void noteOn(int key, int mapped);     // a key down and the note it maps to (two keys may share one)
    void noteOff(int key);
    void advance(double seconds, double bpm);   // runs the memory's timer
    void clear();                         // forget (reset, Stop with Cut)
    const Chord& current() const;         // root -1: nothing (never played, or forgotten)
    int lowestHeld() const;               // -1: no key down
    int held() const;
    uint32_t version() const;             // +1 whenever current() changes
};
}
```

**Behaviour, stated as tests:**
1. **Scales:** the step tables are exactly the ones in CONCEPT §6 (Major 0 2 4 5 7 9 11 … In-Sen
   0 1 5 7 10, Whole Tone 0 2 4 6 8 10, Chromatic 0..11).
2. **`mapInput`:**
   - **As Played** returns the note unchanged.
   - **Snap** goes to the nearest scale tone at or below the note, so a tie goes down. Key D Major,
     note 61 (C#) → 61 (C# is in D major). Note 60 (C) → 59 (B). Note 63 (D#) → 62 (D).
   - **Degrees:**
     - The white keys C D E F G A B of each octave are degrees 0..6. Degree d maps to the
       `scale[d % size]` tone of octave `o + d / size`.
     - Note 60 (C4) maps to the tonic in that octave, at `60 + key`.
     - A black key plays the white key below it.
     - Key D Major: 60 → 62, 62 → 64, 64 → 66, 72 → 74.
     - Key C Min Pent: 60 → 60, 62 → 63, 64 → 65, 65 → 67, 67 → 70, 69 → 72, 71 → 75.
3. **`buildChord`** in key C Major, with voicing Close and root 60:
   - Triad → 60 64 67. Seventh → 60 64 67 71. Sus2 → 60 62 67. Sus4 → 60 65 67.
   - Add9 → 60 64 67 74. Quartal → 60 65 71. Fifths → 60 67 74. Cluster → 60 62 64.
   - Root 62 (D) Triad → 62 65 69 (D minor: diatonic).
   - Chromatic scale, root 62 Triad → 62 66 69 (major on the root).
   - `CH_OFF` → just the root.
4. **Voicings** for the C Major triad on 60:
   - Open → root, 5th, then the 3rd up an octave → 60 67 76. A chord with more tones puts every
     second tone of the close voicing up an octave (Seventh: 60 67 76 83).
   - Drop 2 → the second-highest note of the close voicing dropped an octave → 55 60 64.
   - Spread → root one octave down, then 5th, 3rd+12, 5th+12 → 48 55 64 67. A chord with more
     tones puts its remaining tones in the upper octave, ascending, up to `kChordMax`.
   - A voicing never exceeds `kChordMax` notes, and its notes stay within MIDI 24..108 (octave
     shifts, never dropped notes).
5. **Voice leading:**
   - C major (60 64 67) → F major with leading on: the result is 60 65 69. 60 is common, 64→65,
     67→69, cost 3.
   - G major from C (60 64 67) → 59 62 67 (cost 3).
   - With leading off, `buildChord` is used as is.
   - The cost function is `voiceLeadCost`. Baseline: sort both chords. For equal counts, sum
     `|a_i - b_i|`. For unequal counts, sum over each note of the distance to the nearest note of
     the other chord, both ways, halved.
   - Candidate set: every rotation (inversion) of the close voicing, each shifted by -12, 0 or +12
     octaves, and also the requested voicing as built. Ties go to the candidate whose lowest note
     is nearest `prev`'s lowest.
6. **`tunedPitch`:**
   - Equal: `tunedPitch(69) == 69.0`.
   - Just, key A: `tunedPitch(76)` (E, a fifth above A) = 69 + 12·log2(1.5) = 76.01955, to 1e-9.
   - Just, key C: `tunedPitch(64)` (E) = 60 + 12·log2(5/4) = 63.86314. The tonic stays: 60.0.
     The octave stays: 72.0.
   - Pythagorean, key C: E = 60 + 12·log2(81/64).
   - The 5-limit Just table on the key: 1, 16/15, 9/8, 6/5, 5/4, 4/3, 45/32, 3/2, 8/5, 5/3,
     9/5, 15/8.
7. **The `Harmony` memory:**
   - Chord Off: `noteOn` 60, 64, 67 → `current()` has pcs {0,4,7} and root 60 (the lowest held).
   - Chord Triad: `noteOn(62)` → the D-minor chord, root 62. A second `noteOn(65)` while 62 is held
     → the chord on the **latest** key: 65.
   - **Release with memory Forever:** `current()` is unchanged after every key is up, for any
     `advance()` time.
   - **Memory 1 bar at 120 bpm:** after the last release, `advance(1.9)` → still there.
     `advance(0.2)` more → root -1.
   - **Memory Off:** root -1 right at the last release.
   - `version()` increments exactly on changes.
   - `lowestHeld()` tracks keys down.
   - Re-pressing a held key (a double note-on) is idempotent.

**Learning-mode contribution:** `voiceLeadCost` is the place where taste shows: it decides how
Bloom moves between chords. The task implements the baseline above with a `// TODO(Roland)`
comment that names the trade-offs:
- Penalise parallel fifths?
- Weight upper voices more than the bass?
- Prefer common tones staying exactly still?

The user may replace it later, and test 5's two cases must still pass.

**Commit:** "Harmony: scales, input mapping, diatonic chords, voicings, voice leading, tunings, memory".

---

## Task 4: Tables, the lifetime models and the digital waves

**Files:**
- **Create:**
  - `dsp/wavetable.h`, `dsp/wavetable.cpp`: PolyForce's layout. Port `Wavetable`, the `kMip*`
    constants, `mipFor`, `newTableId`, and PolyForce's band-limited frame builder (the code in
    PolyForce `dsp/wavetable.cpp` that turns a harmonic spectrum into the 11 mip levels with
    16-bit samples and a per-frame scale). Not the WAV loader or the builtins.
  - `dsp/lifetime.h`, `dsp/lifetime.cpp`.
  - `plugin/tables.h`, `plugin/tables.cpp`.
  - `test/tables_test.cpp`.

**`dsp/lifetime.h`:**

```cpp
namespace af {
constexpr int kLifeFrames = 256;
// The table library, in browser order: lifetime tables first, then the digital waves (one frame each).
enum TableId : int { TB_FELT_PIANO, TB_CELESTA, TB_GLASS_HARMONICA, TB_CELLO_TASTO, TB_CHOIR_AH_OO,
                     TB_REED_ORGAN, TB_SINE_BLOOM, TB_TAPE_STRINGS,
                     TB_SINE, TB_TRIANGLE, TB_SAW, TB_SQUARE, TB_COUNT };
const char* tableName(int id);     // "Felt Piano", ..., "Square"; "" out of range
bool isLifetime(int id);           // the first 8
bool buildTable(int id, Wavetable& out);   // load time, not real time: allocates, ~1e8 flops
const Wavetable& sineTable();      // one frame, built on first use (thread-safe static): the fallback

// What a slot reads: published by the plugin's builder thread, read by the audio thread.
struct TableSet {
    std::atomic<const Wavetable*> t[TB_COUNT] = {};
    const Wavetable& get(int id) const;   // the table if built, else sineTable()
};

// Frame f of a lifetime table shows the note at time T * (f / 255)^kLifeCurve (T: the model's
// length), so frames 0..63 cover the first tenth of the note.
constexpr double kLifeCurve = 1.661;   // (64/255)^1.661 = 0.1
}
```

**Life models**, in `lifetime.cpp`. Each model is a small struct of numbers and functions. For
frame f, at time t:
1. Each harmonic h = 1..1024 gets an amplitude
   `A_h(t) = S_h * exp(-t / tau_h) * (1 + beat_h(t)) * formant(h, t)`, where:
   - `S_h` is the initial spectrum: a tilt in dB/oct plus the odd/even balance.
   - `tau_h = tau1 / (1 + damp * (h - 1))`: upper harmonics die faster.
   - `beat_h` is a slow sinusoidal wobble, so partials pulse the way piano strings do.
   - `formant` is an optional two-formant (F1/F2) gain whose formants move over the life (the
     choir's ah → oo).
2. Phases: a fixed pseudo-random phase per harmonic (seeded by the table id), so frames don't
   peak, and phases are identical across frames, so frames crossfade without comb filtering.
3. **Every frame is normalised to the same RMS.** A lifetime table carries *timbre*, not level:
   Age 1 must still be audible. Record each frame's original level in `Wavetable::scale` only as
   the storage scale; the oscillator must not use it as a level.

| Table | Character |
|---|---|
| Felt Piano | Tilt -9 dB/oct, damp 0.08, the 2nd harmonic strong, beating 0.3–0.9 Hz on h ≥ 3 |
| Celesta | Odd-heavy, tilt -6, damp 0.25 (bright attack, sine-like tail) |
| Glass Harmonica | Harmonics 1, 2, 3 and 5 only, slow beating, almost no damping |
| Cello Tasto | Saw-like with tilt -12, a body formant at 300 Hz and 1 kHz, slow bow-pressure wobble |
| Choir Ah→Oo | Formants move from ah (F1 800, F2 1150) to oo (F1 350, F2 600) over the life |
| Reed Organ | Odd harmonics, steady, a little beating |
| Sine Bloom | Starts a pure sine; harmonics *grow* over the life to a soft saw (a reverse decay) |
| Tape Strings | Saw, tilt -6, gentle beating on all harmonics |

**Digital waves:** one-frame tables (Sine, Triangle, Saw, Square) from their Fourier series.

**`plugin/tables.*`:**
- `TableSet& sharedTables()`: a process-wide `TableSet`.
- `void ensureTablesBuilding()`: the first call starts one detached builder `std::thread` that
  builds the tables in id order and publishes each with `store(ptr, std::memory_order_release)`.
  Later calls return at once.
- Tables live for the process, so they are never freed and no graveyard is needed.
- `VSTPluginMain` calls `ensureTablesBuilding()`.
- If the thread can't be created, log it with `trace()` and leave the sine fallback in place.

**Checks** (`void tablesTests()`):
1. **Band limits:** for every table, every frame and every mip level k, the energy above harmonic
   `1024 >> k` (via a DFT of the level) is under -80 dB of the level's energy.
2. **Equal RMS:** frames 0, 128 and 255 of every lifetime table have RMS within ±0.5 dB of each
   other.
3. **The life curve:** Felt Piano's spectral centroid (harmonic-weighted) falls monotonically
   from frame 0 to 255. Sine Bloom's rises.
4. **Deterministic:** building any table twice gives identical data.
5. **`TableSet::get`** returns `sineTable()` for an unbuilt slot. After the builder publishes, it
   returns the built table, and the builder finishes all 12 within 30 s on x86 under ASan.
6. **Size:** `bytes()` of a lifetime table ≤ 4.8 MB.

**Commit:** "Tables: PolyForce's wavetable layout, 8 lifetime models, 4 digital waves, a builder thread".

---

## Task 5: The lifetime oscillator

**Files:** create `dsp/lifeosc.h` (header-only, for inlining) and `test/lifeosc_test.cpp`.

**Interface:**

```cpp
namespace af {
enum Couple : int { CP_MIX, CP_FM, CP_AM, CP_RING, CP_COUNT };

// Where in the table's life to read, and how that point moves. Control rate: call once per
// control step (kControl = 32 samples) with the elapsed time.
struct LifePos {
    float age = 0.5f;          // 0..1
    float sway = 0.0f;         // 0..1: depth of the slow back-and-forth (±0.25 of the table at 1)
    float swayHz = 0.05f;      // 0.002..2
    float smear = 0.0f;        // 0..1: fast random micro-motion of the position (±0.03 at 1)
};
class LifeScan {   // the moving read position; one per voice (each voice its own sway phase)
public:
    void seed(uint32_t s);     // the sway's start phase and the smear's random numbers
    void reset(float phase);
    float step(const LifePos& p, float seconds);   // the position 0..1 for the next control step
};

// One oscillator over one table: phase, frame crossfade, mip choice. Frames are read as
// int16 * scale and normalised (lifetime frames are equal-RMS already, so no level per frame).
class TableOsc {
public:
    void reset(float phase = 0.0f);
    // inc: cycles per sample; pos: 0..1 over the frames; renders n samples into out (overwrites);
    // fmIn: per-sample phase offset in cycles, or nullptr.
    void render(const Wavetable& t, float inc, float pos, float* out, int n, const float* fmIn = nullptr);
};
}
```

**Behaviour:**
- **Frame crossfade:** `pos * (frames - 1)` → frames i and i + 1 with fraction x. Each sample
  linearly interpolates within both frames at the mip level `mipFor(inc)`, then crossfades. When
  `pos` changes between calls, it glides linearly across the n samples, so a moving Age never
  steps.
- **Mip changes:** when the mip level changes within a render, the next render uses the new level.
  The change happens at a block edge, and the crossfade masks it.
- **Couple** is applied by the voice, not the oscillator. Each control step the voice renders B
  into a scratch buffer.
  - **Mix:** `(1 - blend) A + blend B`.
  - **FM:** `fmIn = amt * 0.5 * B` (cycles), applied to A. The output is then mixed with B by blend.
  - **AM:** `A * (1 - amt + amt * (0.5 + 0.5 B))`, then the blend with B.
  - **Ring:** `(1 - amt) A + amt (A * B)`, then the blend.

**Checks:**
1. **Pitch:** the sine table at `inc = 440/44100` gives a 440 Hz peak, with harmonics under -90 dB.
2. **No aliasing:** the Saw table at notes 24..108 in steps of 12 has no component above Nyquist
   folded back below 15 kHz louder than -70 dB relative to the fundamental (the PolyForce
   `kAliasLimit` rule).
3. **Position glide:** a pos jump from 0 to 1 within one 128-sample render has no sample-to-sample
   step bigger than 1.5× the largest step of the steady state on either side.
4. **Sway:** with sway 1 and swayHz 0.1, `LifeScan::step` stays within age ± 0.25 (clamped to
   0..1). Over 10 s it visits both ends of that range within 2%.
5. **Smear:** the position stays within ±0.03.
6. **FM:** with the sine and B the sine at the same frequency and amt 0.5, energy appears at 2f
   and 3f.
7. **Ring:** with A at f1 and B at f2 and amt 1, the peaks are at f1 ± f2, and f1 is suppressed
   by more than 40 dB.

**Commit:** "Lifetime oscillator: frame crossfade, mip choice, sway and smear, Couple modes".

---

## Task 6: Ground, the drone

**Files:** create `dsp/ground.h`, `dsp/ground.cpp`, `test/ground_test.cpp`.

**Interface:**

```cpp
namespace af {
// enum Listen (LI_NOTES, LI_HARMONY, LI_FREE, LI_COUNT) lives in dsp/harmony.h, shared by every stratum.
enum ColorInterval : int { CI_MIN3, CI_MAJ3, CI_FOURTH, CI_MIN7, CI_NINTH, CI_ELEVENTH, CI_COUNT };

struct GroundPatch {
    int listen = LI_HARMONY;
    bool mute = false;
    float level = 0.7f;             // 0..1 (audio taper in the mapping: the engine gets a gain)
    float cutoffHz = 2500.0f;       // Tone: low-pass, 40..16000
    int table = TB_CELLO_TASTO;
    LifePos pos{0.5f, 0.3f, 0.05f, 0.0f};
    float beatHz = 0.3f;            // 0..3: how fast the partials beat against each other
    float gravityS = 6.0f;          // 0..30: the glide to a new root (95% there after gravityS)
    float fadeS = 4.0f;             // 0.05..30: fade in when it starts, fade out when it stops
    float sub = 0.3f, root = 1.0f, fifth = 0.5f, octave = 0.25f, color = 0.0f;   // partial levels 0..1
    int colorInterval = CI_NINTH;
    int registerOct = 2;            // the root's octave: 1 (C1..B1), 2 (C2..), 3 (C3..)
    float body = 0.0f;              // 0: off; 0..1/3 fades in an "a" vowel; then a -> o -> u
    float breath = 0.3f;            // 0..1: a slow swell of level and brightness
    float breathHz = 0.07f;
    float width = 0.5f;             // 0..1: the partials spread across the stereo field
};

class Ground {
public:
    Ground();
    void seed(uint32_t s);
    void set(const GroundPatch& p, const HarmonyPatch& h);
    void setTarget(int rootNote);   // a MIDI note (its pitch class and octave are what count); -1: stop
    void reset();
    // Adds into outL/outR, and adds the send into sendL/sendR at `spaceSend`. n <= 128.
    void render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR,
                float spaceSend, int n);
    bool sounding() const;
};
}
```

The engine decides the target by Listen; Ground only follows `setTarget`.

**Behaviour:**
- **Root pitch:** the target's pitch class in `registerOct`, tuned with `tunedPitch()`.
- **Partials:** Sub = root - 12; Root; Fifth = root · 3/2 under Just and Pythagorean, +7 semitones
  under Equal; Octave = ×2; Color by interval. Just ratios: 6/5, 5/4, 4/3, 9/5, 9/4, 8/3. Equal
  uses semitones. Each partial is a `TableOsc` on the Ground table at one shared `LifeScan`
  position.
- **Beat:** partial i's frequency gets `beatHz * {0, -0.5, +0.5, -0.25, +0.25}[i]` added **in Hz**
  (Sub, Root, Fifth, Octave, Color). The beating between the Root and Fifth harmonics' meeting
  points therefore runs at a rate set in Hz, the same in every register.
- **Gravity:** the pitch approaches the new root exponentially with time constant
  `gravityS / 3`, so it is 95% there at gravityS. Gravity 0 jumps.
- **Fade:** linear in dB from -60 to 0 over fadeS on start, and the reverse on stop. When it
  reaches -60, `sounding()` becomes false and the voice stops rendering.
- **Body:** two band-pass SVFs at the vowel formants (a: 800/1150 Hz, o: 450/800, u: 350/600),
  interpolated by `body`, mixed with the dry signal.
- **Breath:** a sine LFO at breathHz moves the level by ±(3 dB · breath) and the cutoff by
  ±(1 octave · breath).
- **Width:** partial i is panned to `width * {0, -0.3, +0.3, -0.6, +0.6}[i]` (equal power).

**Checks:**
1. **Just fifth:** `setTarget(48)` with only Root and Fifth at level 1 and Beat 0. Via peak
   estimation (parabolic interpolation on a 2^16 FFT, or the `toneAmp` sweep), the fifth partial's
   frequency is root × 1.5 within 0.01 Hz.
2. **Beat in Hz:** with Beat 1.0, a Saw table, Root and Fifth only, and register 2, the summed
   output's amplitude envelope near harmonic 3 of the root (= harmonic 2 of the fifth) beats at
   1.0 Hz ± 0.05. Register 3 gives the same rate.
3. **Gravity:** a target change 48 → 53 with gravity 6 s: after 2 s the pitch is between 50.5 and
   52.0 semitones; after 6 s it is within 0.25 semitones of 53.
4. **Fade:** silent before `setTarget`. After `setTarget(48)` with fade 1 s, the level at 0.5 s is
   within ±3 dB of -30 dB relative to steady state. After `setTarget(-1)`, `sounding()` becomes
   false within fadeS + 0.1 s.
5. **Gains:** level 0 or mute → the output and the send are exactly 0.
6. **Stability:** cutoff 40 and 16000, body 1, breath 1 for 60 s → finite, with peak ≤ 4.

**Commit:** "Ground: one drone voice with just partials, beating in Hz, gravity, body and breath".

---

## Task 7: Bloom, the chord voices

**Files:** create `dsp/bloom.h`, `dsp/bloom.cpp`, `test/bloom_test.cpp`.

**Interface:**

```cpp
namespace af {
enum FilterMode : int { FM_LP, FM_BP, FM_HP, FM_COUNT };
enum Tail : int { TL_VOICE, TL_SPACE, TL_COUNT };

struct BloomPatch {
    int listen = LI_NOTES;
    bool mute = false;
    float level = 0.7f;
    float cutoffHz = 5000.0f;  // Tone
    float reso = 0.1f;
    int filterMode = FM_LP;
    int table = TB_FELT_PIANO;
    LifePos pos{0.6f, 0.25f, 0.07f, 0.1f};
    int tableB = TB_SINE;
    int bOctave = 0;           // -2..+2
    float blend = 0.0f;
    int couple = CP_MIX;
    float coupleAmt = 0.0f;
    int unison = 1;            // 1..2
    float detuneCents = 8.0f;  // 0..50
    float swellS = 2.5f;       // attack 0.005..30
    float releaseS = 6.0f;     // 0.01..30
    float velSens = 0.4f;      // 0..1
    float breath = 0.05f;      // band-passed noise at the note, 0..1
    int tail = TL_SPACE;
    float width = 0.6f;        // unison / chord spread across the stereo field
};

class Bloom {
public:
    static constexpr int kVoices = 6;
    static constexpr float kHandoffS = 1.5f;
    Bloom();
    void seed(uint32_t s);
    void set(const BloomPatch& p, const HarmonyPatch& h);
    // A chord (or one note) starting now. `owner`: the key that started it (noteOff(owner) releases
    // its notes); -1 for the Harmony and Free modes' chords. vel 0..1. Strum from h.strumS.
    void play(const Chord& c, int owner, float vel);
    void release(int owner);              // the voices `owner` started
    void releaseAll();
    // Harmony/Free modes: move to `c`. Notes common to both keep sounding untouched; the others
    // release, the new ones start (with the strum).
    void moveTo(const Chord& c, float vel);
    void reset();
    void render(const TableSet& tables, float* outL, float* outR, float* sendL, float* sendR,
                float spaceSend, int n);
    int active() const;                   // voices sounding (attack, sustain or release)
    float handoffBoost() const;           // what the tail handoff adds to the send now (tests)
};
}
```

**Behaviour:**
- **Voices:** each voice is `TableOsc` A on the table at its `LifeScan` position, plus `TableOsc` B
  on tableB at the same position (one frame for digital waves) at `bOctave`. The two are coupled
  (Task 5 rules). Then the breath noise (white noise → band-pass SVF at the note, Q 4). Then an SVF
  (lp/bp/hp, cutoff, reso). Then the envelope and velocity. Then the pan.
- **Unison 2:** a second A+B pair detuned by ±detune/2 cents and spread by width.
- **Envelope:** attack is linear in amplitude over swellS (an S-curve: `0.5 - 0.5 cos`); sustain
  is 1; release is exponential to -60 dB over releaseS.
- **Allocation:** a free voice. Otherwise steal, in order: the quietest releasing voice, then the
  oldest voice. A stolen voice fades over 3 ms before the new note starts in it.
- **Strum:** note k of n starts after `k * strumS / max(1, n - 1)`, counted in samples. A pending
  start is cancelled if its owner releases first.
- **Tail handoff** (`tail == TL_SPACE` and `releaseS > kHandoffS`):
  - On release the voice's dry gain follows the release curve, multiplied by a fade from 1 to 0
    over kHandoffS (`cos²`).
  - Its send gain is multiplied by `boost = min(4, sqrt(releaseS / kHandoffS))`, ramping in over
    0.2 s.
  - The voice is free at kHandoffS.
  - The engine holds the Space decay at ≥ releaseS while Tail is Space (Task 8).
- **Hold and pedal** are the engine's job: it simply doesn't call `release()`.

**Checks:**
1. **One chord:** `play({60,64,67}, owner 60)` → `active() == 3`. `release(60)` with releaseS 1
   and tail Voice → `active() == 0` within 1.1 s.
2. **Strum:** strum 0.6 s, 3 notes → onsets at 0, 0.3 and 0.6 s ± 1 block (each voice's first
   non-zero sample).
3. **Steal:** 7 notes → `active() == 6`, and the first note is the one gone. No sample-to-sample
   jump > 0.25 (the 3 ms fade).
4. **`moveTo`:** from C major (60 64 67) to A minor (60 64 69): voices 60 and 64 are not
   restarted (their envelope keeps sustaining; no new attack), 67 releases and 69 starts.
5. **Tail handoff:** releaseS 10 with tail Space → `active() == 0` within 1.6 s of release. The
   send's RMS from 0.2 to 1.5 s after release is greater than with tail Voice. With tail Voice,
   `active()` stays ≥ 1 for 9 s.
6. **Swell:** swellS 2 → the level at 1 s is within ±1.5 dB of -6 dB relative to sustain.
7. **Velocity:** velSens 0 → velocities 1 and 127 give the same level. velSens 1 → the level
   follows vel (linear amplitude) within ±1 dB.
8. **Stability:** reso 1, cutoff 40 and 20000, couple FM with amt 1, unison 2 with detune 50, at
   notes 24 and 108, for 30 s → finite, with peak ≤ 4.

**Commit:** "Bloom: six chord voices over lifetime tables, strum, voice-led moves, the tail handoff".

---

## Task 8: The engine, Listen modes, mix and output

**Files:** rewrite `dsp/engine.h` and `dsp/engine.cpp` (replacing the stub); update
`dsp/stages.h` and `test/engine_test.cpp`. Also update:
- `plugin/plugin.cpp`. Today `effMainsChanged(0)` and `effStopProcess` set `panic` and call
  `reset()`. They now call `suspend()`, and processing resuming calls `resume()`. Both reach the
  audio thread through atomics, like `panic`.
- `test/plugin_test.cpp`. Its sound checks assume the stub: exact pitches, zero 0.5 s after
  note-off, CC 123 → zero, suspend → zero in a block, the velocity ratio. Rewrite them for the
  real engine:
  - Silence checks use CC 120 or Cut.
  - Pitch checks use Ground or Bloom alone with Space off.
  - The suspend checks follow the On Stop rules below.

(From Task 1's code review.)

**Patch:**

```cpp
namespace af {
enum OnStop : int { OS_KEEP, OS_FADE, OS_CUT, OS_COUNT };
struct Patch {
    float volumeDb = -6.0f;
    float tilt = 0.0f;            // -1..1: ±6 dB at the extremes, pivot 800 Hz
    HarmonyPatch harmony;
    bool hold = false;            // latch: chords stay until the next one
    int onStop = OS_FADE;
    GroundPatch ground; float groundSpace = 0.4f, groundPan = 0.0f;
    BloomPatch bloom;   float bloomSpace = 0.5f, bloomPan = 0.0f;
    Space::Params space;
    float spaceReturn = 0.8f;     // the return's level, 0..1 (audio taper)
};
}
```

`Engine` keeps the stub's public API, plus `explicit Engine(const TableSet& tables)` and
`void suspend()` / `void resume()`.

**Routing** (CONCEPT §4.1):
1. **Map:** `noteOn(note, vel)` → `m = mapInput(h, note)`. The engine remembers `key → m`, so
   note-off finds the mapped note even if the patch changed in between.
2. **Harmony** gets `noteOn(key, m)` and `noteOff(key)`. It keeps keys and mapped notes apart itself.
3. **Bloom:**
   - **Notes:** `play(leadFrom(h, prev, buildChord(h, m)), owner = key, vel)` (or `buildChord` as is with Leading off). With
     Chord Off this is the single note. On note-off: `release(key)`, unless Hold or the pedal is
     down. With Hold, the next note-on first releases what Hold kept.
   - **Harmony:** when `Harmony::version()` changes → `moveTo(current())`. Root -1 →
     `releaseAll()`.
   - **Free:** `moveTo` the tonic chord (`buildChord(h, 48 + key)` with its Chord type, Triad if
     the type is Off) while awake. Re-voice when key or scale change.
4. **Ground:**
   - **Notes:** `setTarget(lowestHeld())`, which is -1 when no key is down (so it fades).
   - **Harmony:** `setTarget(current().root)`.
   - **Free:** `setTarget(key)` while awake.
5. **Awake:** nothing sounds before the first note-on. Free strata start at the first note-on and
   run until Stop or reset. "Awake" survives note-offs.
6. **Stop:**
   - A falling edge of `playing` from `setTransport` (playing → stopped), or `suspend()`
     followed by `resume()` within 250 ms, applies On Stop:
     - **Keep:** nothing happens.
     - **Fade:** a master fade to -inf over 8 s, then `reset()` and asleep.
     - **Cut:** `reset()` at once.
   - A suspend longer than 250 ms → `reset()`.
   - CC 123 (all notes off) releases every key, as on the siblings, but does not put the engine
     to sleep.
   - CC 120 and `reset()` silence everything and clear the harmony.
7. **Mix:**
   - Ground and Bloom each render into the dry bus and add their sends.
   - Space returns the wet. Sum = dry + spaceReturn² · wet.
   - Then tilt (two one-pole shelves around 800 Hz).
   - Then the **limiter**: no lookahead. A gain computer with a 1 ms attack and 150 ms release
     keeps the peak at or under -1 dBFS (0.891), with a tanh soft clip above 0.95 as a backstop.
   - Then `volumeDb`.
   - Then the **non-finite guard:** any non-finite sample → zero the block, `reset()` every DSP
     state, count it.
8. **Space decay hold:** with Bloom Tail Space, the reverb's effective `decayS` is
   `max(space.decayS, bloom.releaseS)`, unless the mode is Abyss (already longer).
9. **Control rate:** every 32 samples (`kControl`), the engine:
   - advances the harmony timer,
   - steps every `LifeScan`,
   - updates Ground's glide, fades and breath,
   - re-reads the table pointers (`TableSet::get`).
10. **Idle:** when asleep, or when awake with nothing sounding and `Space::silent()`, `render`
    writes zeros and skips the DSP. `activeVoices()` is Bloom's active count plus 1 if Ground
    sounds.
11. **Stage timers** (`AF_STAGE_TIMING`): ground, bloom, space, out.

**Checks** (`test/engine_test.cpp`, replacing the stub's):
1. **Silence before the first note:** with all strata Free, 2 s of render is exact zeros.
2. **Listen defaults:** Bloom Notes, Ground Harmony. Note 60 down, then up, with memory Forever →
   Bloom releases, Ground keeps sounding for 30 s.
3. **Bloom Harmony:** after the note is released, Bloom keeps sounding (moveTo held the chord).
   A new note 65 moves it (`active()` stays ≤ 6).
4. **Free:** with Ground Free, a note-on then note-off → Ground sounds on the key's tonic,
   whatever note was pressed.
5. **Snap:** key D Major, Input Snap, note 60 → Ground's target pitch class is B (11). Check
   through an `Info` accessor that reports the targets.
6. **Hold:** Hold on → note-off doesn't release. The next note-on releases the previous chord.
7. **On Stop:**
   - Fade: a playing → stopped transport → peak under -60 dBFS after 8.2 s, and asleep after that.
   - Keep: still sounding at 8.2 s.
   - Cut: zeros within 1 block.
   - A suspend/resume 100 ms apart = Stop. A suspend of 1 s = reset.
8. **Limiter:** every level at max, reso 1, shimmer 1, freeze on, chords of 6 notes → peak
   ≤ 0.8913 (-1 dBFS) at every sample over 60 s. Repeat it with **Abyss at Decay 30**: its wet
   sits about 15 dB above the send's RMS.

**Notes from the Ground and Bloom reviews:**
- **Re-voicing a held chord:** call `bloom.play(new)` before `bloom.release(old)`, so common
  notes carry on.
- **Space off:** pass Bloom a send of 0 when the Space return is 0, Space is muted, or
  bloomSpace is 0. Bloom then releases as Tail Voice, and the tail isn't lost into a silent
  reverb.
- **Muted Ground:** `Ground::sounding()` ignores mute. The engine skips a muted Ground itself.
- **Levels arrive as gains:** Ground and Bloom take `level` as a gain. The patch map (Task 9)
  squares the knob.
- **Headroom:** Ground's partial sum carries a fixed headroom gain. Bloom's voice gain is 0.25.
  Levels are matched in Task 10.

**Notes from Task 2's review:**
- **Space's `silent()` can stay false for minutes** (Abyss at long Decay). The CPU gate assumes
  Space always runs.
- **A `set()` that raises the reverb's reach** (size, predelay, shimmer on) can flip `silent()` to
  false with no input. Idle logic must tolerate running Space a little longer.
- **Rise's full-duck level** (`kFull`, -12 dBFS on the send) is absolute. Calibrate it against the
  real send levels here, or in Task 10.
- **Abyss is about 6 dB louder** than other modes at the same Decay knob, by design (it is Decay
  ×4). Level matching in Task 10 handles it; Space must not compensate.
9. **Guard:** a NaN forced into the reverb (test hook) → that block is zeros, the next blocks are
   finite, and the guard count is 1.
10. **Clock:** a transport position of 2^31 + 10 beats and an engine sample count near 2^32 (test
    hook) → renders, the timers still advance, nothing stalls.
11. **CPU smoke test** (x86, not a gate): the worst-case patch renders 10 s in under 10 s of wall
    time under ASan.

**Commit:** "Engine: Listen routing, harmony to Ground and Bloom, Space, tilt, limiter, Stop, guard".

---

## Task 9: Parameters, pages, the patch map

**Files:** modify `surface/surface.py`, `plugin/patch_map.cpp` and `plugin/patch_map.h`
(display formats), `test/plugin_test.cpp`, `test/preset_test.cpp` and `surface/skin_polish.py`
(its self-test).

**Carried over from Task 1's reviews:**
- **RANDOMIZE was removed** in Task 1, because volume was all there was to randomize. Bring it
  back here with `rand_amt` (kind `ui`) only if it can be made musical:
  - ranges per parameter that stay inside a sound (no Freeze, no Mute, no Hold, no table B on
    FM at full);
  - otherwise, leave it out and say so in ROADMAP's decisions.

  If `rand_amt` returns, restore SubForce's check "Rand Amount is the surface's: not saved, not
  reset by a preset".
- **Restore SubForce's enum and popup checks** now that those parameters exist again: one option
  per Q-Link detent; a tapped option lands exactly; the snapped value is pushed back to MPC; a
  popup closes on pick.
- **The skin_polish self-test's sub-page checks** come back now that there are several pages and
  groups.
- **Preview files are numbered from 0** (`page_0.png` …).

**Parameters** (keys, names ≤13 characters and unique, curves, defaults), appended after `status`
and `volume`:

| Group | Parameters |
|---|---|
| Harmony | `h_key` Key (C..B, C); `h_scale` Scale (12, Major); `h_tuning` Tuning (Equal/Just/Pythagorean, Just); `h_input` Input (As Played/Snap/Degrees, Snap); `h_chord` Chord (10, Triad); `h_voicing` Voicing (Close/Open/Drop 2/Spread, Open); `h_leading` Leading (Off/On, On); `h_strum` Strum (pow 0..2 s, 0); `h_memory` Memory (Off/1 Bar/2 Bars/4 Bars/8 Bars/16 Bars/32 Bars/64 Bars/Forever, Forever); `h_hold` Hold (Off/On); `h_onstop` On Stop (Keep/Fade/Cut, Fade) |
| Ground | `g_listen` Ground Listen (Notes/Harmony/Free, Harmony); `g_mute` Ground Mute; `g_level` Ground Level (0.7); `g_cutoff` Ground Tone (log 40..16000, 2500); `g_table` Ground Table (12, Cello Tasto); `g_age` Ground Age (0.5); `g_sway` Ground Sway (0.3); `g_swayrate` Gnd Sway Rate (log 0.002..2 Hz, 0.05); `g_beat` Ground Beat (lin 0..3 Hz, 0.3); `g_gravity` Gravity (pow 0..30 s, 6); `g_fade` Ground Fade (log 0.05..30 s, 4); `g_sub` Ground Sub (0.3); `g_root` Ground Root (1); `g_fifth` Ground Fifth (0.5); `g_oct` Ground Octave (0.25); `g_color` Ground Color (0); `g_colint` Color Int (m3/M3/4th/m7/9th/11th, 9th); `g_reg` Ground Reg (Low/Mid/High, Mid); `g_body` Ground Body (0); `g_breath` Ground Breath (0.3); `g_space` Ground Space (0.4); `g_width` Ground Width (0.5); `g_pan` Ground Pan (-1..1, 0) |
| Bloom | `b_listen` Bloom Listen (Notes); `b_mute` Bloom Mute; `b_level` Bloom Level (0.7); `b_cutoff` Bloom Tone (log 40..20000, 5000); `b_reso` Bloom Reso (0.1); `b_fmode` Bloom Filter (LP/BP/HP); `b_table` Bloom Table (Felt Piano); `b_age` Bloom Age (0.6); `b_sway` Bloom Sway (0.25); `b_swayrate` Blm Sway Rate (0.07); `b_smear` Bloom Smear (0.1); `b_tableb` Bloom Table B (Sine); `b_boct` Bloom B Oct (-2..+2, 0); `b_blend` Bloom Blend (0); `b_couple` Bloom Couple (Mix/FM/AM/Ring); `b_camt` Couple Amount (0); `b_unison` Bloom Unison (1/2, 1); `b_detune` Bloom Detune (0..50 cents, 8); `b_swell` Bloom Swell (log 0.005..30 s, 2.5); `b_release` Bloom Release (log 0.01..30 s, 6); `b_vel` Bloom Vel (0.4); `b_breath` Bloom Breath (0.05); `b_tail` Bloom Tail (Voice/Space, Space); `b_space` Bloom Space (0.5); `b_width` Bloom Width (0.6); `b_pan` Bloom Pan (0) |
| Space | `s_mode` Space Type (Room/Hall/Plate/Space/Haze/Abyss, Hall); `s_size` Space Size (0.6); `s_decay` Space Decay (log 0.3..30 s, 8); `s_predelay` Pre-Delay (pow 0..250 ms, 30); `s_damp` Space Damp (log 1000..20000, 6000); `s_lowcut` Space Low Cut (log 20..1000, 120); `s_mod` Space Mod (0.4); `s_width` Space Width (1); `s_freeze` Freeze (Off/On); `s_shimmer` Shimmer (0); `s_shint` Shimmer Int (+12/+7/+19/-12); `s_rise` Space Rise (0.2); `s_return` Space Level (0.8) |
| Output | `o_tilt` Tilt (-1..1, 0) |

**Display formats:** add these where missing.
- `period`: rates shown as their period below 1 Hz, e.g. "20 s", "8.3 min".
- `hz2`: Beat, "0.30 Hz".
- `cents`.
- `oct`: B Oct, "+1 Oct".

**Pages:** four groups, each page with its Q-Link set (title ≤12 characters). The first 8 Q-Links
of the stratum pages follow the shared layout: Level · Tone · Shape · Motion · Character · (slot 6:
Gravity / Swell until Echo arrives in M2) · Space · Width.

| Group | Page | Q-Links 1..8 · 9..16 |
|---|---|---|
| PLAY | PLAY | Ground Level, Bloom Level, Space Level, Freeze, Hold, Bloom Age, Bloom Swell, Volume · Key, Scale, Chord, Gravity, Bloom Tone, Ground Tone, Space Decay, Shimmer |
| PLAY | HARMONY | Key, Scale, Tuning, Input, Chord, Voicing, Leading, Strum · Memory, Hold, On Stop, Ground Listen, Bloom Listen, Volume, Gravity, Bloom Swell |
| STRATA | GROUND | Ground Level, Ground Tone, Ground Age, Ground Sway, Ground Beat, Gravity, Ground Space, Ground Width · Ground Table, Gnd Sway Rate, Ground Fade, Ground Body, Ground Breath, Ground Reg, Ground Listen, Ground Mute |
| STRATA | DRONE | Ground Sub, Ground Root, Ground Fifth, Ground Octave, Ground Color, Color Int, Ground Pan, Ground Beat · Gravity, Ground Fade, Ground Breath, Ground Body, Ground Level, Ground Tone, Ground Age, Ground Sway |
| STRATA | BLOOM | Bloom Level, Bloom Tone, Bloom Age, Bloom Sway, Bloom Blend, Bloom Swell, Bloom Space, Bloom Width · Bloom Table, Bloom Release, Bloom Reso, Bloom Filter, Bloom Tail, Bloom Vel, Bloom Listen, Bloom Mute |
| STRATA | BLOOM OSC | Bloom Table B, Bloom B Oct, Bloom Couple, Couple Amount, Bloom Smear, Blm Sway Rate, Bloom Unison, Bloom Detune · Bloom Breath, Bloom Pan, Bloom Age, Bloom Sway, Bloom Blend, Bloom Tone, Bloom Swell, Bloom Release |
| SPACE | SPACE | Space Type, Space Size, Space Decay, Pre-Delay, Space Damp, Space Low Cut, Space Mod, Space Width · Freeze, Shimmer, Shimmer Int, Space Rise, Space Level, Ground Space, Bloom Space, Tilt |
| SPACE | MIX | Ground Level, Ground Pan, Ground Space, Ground Mute, Bloom Level, Bloom Pan, Bloom Space, Bloom Mute · Space Level, Tilt, Volume, Freeze, Ground Width, Bloom Width, Space Width, Shimmer |
| BROWSE | PRESETS | as SubForce's BROWSE |

Popups for long lists: Key, Scale, Chord, Memory, Ground Table, Bloom Table, Bloom Table B, Space
Type. Segments for short ones. Every page must pass `check_layout()`: geometry, names and Q-Link
sets.

**The patch map:**
- `patchFromParams()` fills `af::Patch`. Levels use an audio taper (`v^2`, as the family does).
- `static_assert`s tie the option counts to the engine's enums (`SC_COUNT`, `CH_COUNT`,
  `LI_COUNT`, `TB_COUNT`, `Reverb::kModes`, …).

**Checks** (plugin level):
1. Every parameter's display round-trips its default.
2. Every option list count equals its enum's count.
3. A chunk save and load restores every value.
4. Setting each parameter to 0 and to 1 while a chord plays gives finite output.

Then run `make skin preview` and look at every `page_*.png`.

**Commit:** "Pages: PLAY, STRATA, SPACE and BROWSE groups; the M1 parameter set mapped to the engine".

---

## Task 10: Factory presets and level matching

**Files:**
- `presets/Factory/**/*.afp`
- Modify `tools/demos.cpp`, the demo phrase for an ambient instrument: one chord held 12 s, a
  second chord held 12 s, then 16 s of release. Loudness is measured over the whole 40 s.

**The 16 presets** (names ≤18 characters; categories ≤12):

| Category | Presets |
|---|---|
| `01_Templates` | Init |
| `02_Drones` | Low Tide Hum, Harbour at 4am, Fifth Light |
| `03_Beds` | Lydian Morning, Slow Aurora, Felt Room, Night Ferry |
| `04_Blooms` | First Snow, Glass Orchard, Tape Bloom, Sine Garden |
| `05_Choirs` | Lantern Choir, Distant Ah, Cathedral Breath, Choir in Haze |

Each preset uses its category's character (CONCEPT §10 recipes adapted to M1's two strata):
- **Drones:** Ground forward, Bloom Harmony and quiet.
- **Beds:** both strata, long swells.
- **Blooms:** Bloom forward, Ground low.
- **Choirs:** Choir Ah→Oo on Ground (with Body) and/or Bloom.

`make preset-levels` matches every preset to -16 LUFS, with peaks ≤ -1 dBFS.

**Checks:** `preset_test.cpp` loads every factory preset. Every one renders the phrase finite,
within -16 ± 1 LUFS, with a peak ≤ -1 dBFS.

**Commit:** "Presets: 16 factory sounds in four categories, matched at -16 LUFS".

---

## Task 11: Bench, PGO trainer, soak

**Files:** modify `tools/bench.cpp` and `tools/pgo_train.cpp`; create `tools/soak.cpp`; add
`make soak` to the Makefile.

**Bench cases**, each reporting avg/p99/max % of the block like SubForce's:

| Case | Load |
|---|---|
| idle | Asleep |
| init chord | Init preset, one triad held |
| drone | Ground only, every partial on, body and breath |
| bloom 6x2 | Bloom 6 voices, unison 2, Couple FM, no Ground |
| worst | 6-note chords re-struck every 2 s, unison 2, FM, Ground at full, Sway 1 at 2 Hz on Bloom and Ground, Bloom Smear 1 and Breath 1, Space Abyss with shimmer 1 and freeze off |

**`pgo_train.cpp`:** plays every factory preset through the phrase and every Space mode.

**`tools/soak.cpp`:** `make soak HOURS=1` renders hours of audio offline (x86, -O2), changing
chords every 20–90 s (seeded random), with freeze and shimmer toggling. It fails on:
- a non-finite sample,
- a peak > -1 dBFS,
- a 10-minute window's mean |DC| over -60 dBFS,
- the 10-minute loudness drifting more than ±6 LU from the first window.

The concept's 24 h soak is M2's gate. M1 runs 1 h.

**Run:**
- `make bench`: x86, a smoke test only.
- `make arm-plugin`: profile-guided.
- `make test-arm-pgo`.
- `make soak HOURS=1`: must pass.

**Commit:** "Bench cases, a PGO trainer for the presets, an offline soak test".

---

## Task 12: Docs, CI, push

**Files:**
- `README.md`: what it is, status (preview), installing, the pages.
- `docs/ARCHITECTURE.md`: the source layout, signal path (mermaid), threads, talking to MPC
  (copied rules), parameters and state.
- `docs/BUILDING.md`: from SubForce, renamed, plus `make soak`.
- `docs/ROADMAP.md`:
  - M1 ✅ once the device run is done (🔜 until then).
  - M2 to M4 from the concept.
  - The Phase 0 probe questions as an open list.
  - A Decisions log, starting with CONCEPT §17.
- `docs/PERFORMANCE.md`: the x86 numbers; the device numbers come with the device run.
- `CHANGELOG.md`: `## 0.0.1 (unreleased)`.
- `.github/workflows/build.yml`: names checked.

**Then:**
1. `make test test-arm arm-plugin` all pass.
2. Commit.
3. `git push -u origin main`.
4. Confirm CI starts with `gh run list --repo Devko/AmbientForce`.

**Commit:** "Docs for M1; CI".

---

## Task 13: Device run (with the user)

Needs the Force at `FORCE` (local.mk). Installing stops and restarts MPC, so **ask the user
first.**

1. `make bench-device`: record the cases in PERFORMANCE.md. The gate is the worst case ≤15% p99.
2. `make plugin-install`, after the user has saved their MPC project.
3. With `touch /tmp/ambientforce.trace` on the device, the user plays: pads, Stop, the Force's
   latch, CC64 if available, and a MIDI track routed to the plugin's track on channels 2–4. The
   trace answers the Phase 0 questions. Record them in `docs/PROBE.md`.
4. Listening session; fix what it finds.
5. Add a `tested.json` entry.
