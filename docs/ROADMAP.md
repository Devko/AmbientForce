# Roadmap

Status: ✅ done · 🔜 next · ⬜ planned · 💤 deferred

- [What's next](#whats-next)
- [M1: First light](#m1-first-light)
- [Phase 0: the probe](#phase-0-the-probe)
- [M2 to M4](#m2-to-m4)
- [Planned, not yet placed](#planned-not-yet-placed)
- [Deferred and not planned](#deferred-and-not-planned)
- [Decisions](#decisions)

The milestones come from the [concept](CONCEPT.md#14-roadmap); M1's tasks from
[its plan](plans/2026-10-06-m1-first-light.md).

---

## What's next

- 🔜 **Playing it on the Force** (M1 Task 13): the pads, Stop, the pad latch, a pedal, MIDI tracks
  on other channels, with the trace on: the [Phase 0](#phase-0-the-probe) answers, written up in
  `docs/PROBE.md`.
- 🔜 **A listening session** on real speakers: every factory preset, Init, the strata's ranges; the
  presets and voicing constants tuned from it.
- ✅ **0.0.2, the first release** (2026-10-08, tag `v0.0.2`; 0.0.1 was never released): on the
  Force from a local build of the tag. 🔜 Played, then a `tested.json` entry for CI's release
  package and the plugin catalog's.
- 🔜 **M2: Weather**, from [its plan](plans/2026-10-07-m2-weather.md), on branch `m2`:
  - **Done:** Tasks 1–11 (2026-10-08 and 09). Air, Weather, Echo and Memory are in the engine, with
    their pages and the macros, and Weather's source picker, Remember and Keep.
  - **On the Force:** a build of `m2`, packaged as 0.0.3, for the author to play.
  - **Task 12's 24 presets:** approved
    ([proposal](plans/2026-10-08-m2-presets-proposal.md)).
  - **Task 13** comes last. Where it stands: [Handover](HANDOVER.md).

## M1: First light

🔜 Built, installed and benched (2026-10-07): the gate's CPU half passes on the device (everything
M1 has at its heaviest, p99 10.6% of the block against 15%: [Performance](PERFORMANCE.md)).
Playing it on the device and the listening session are next. The gate: playable on the device, and
Bloom 6×2 with Ground and Space at p99 ≤ 15% of the block.

| Task | | |
|---|---|---|
| 1 | Scaffold: SubForce's plugin side renamed, EffectForce's page groups, a stub engine | ✅ |
| 2 | Space: EffectForce's reverb with Haze and Abyss, as a send / return with Rise | ✅ |
| 3 | The harmony brain: scales, Input, diatonic chords, voicings, voice leading, tunings, memory | ✅ |
| 4 | Tables: PolyForce's wavetable layout, 8 lifetime models, 4 digital waves, a builder thread | ✅ |
| 5 | The lifetime oscillator: frame crossfade, mip choice, Sway and Smear, the Couple modes | ✅ |
| 6 | Ground: one drone voice, just partials, beating in Hz, Gravity, Body and Breath | ✅ |
| 7 | Bloom: six chord voices, strum, voice-led moves, unison, the tail handoff | ✅ |
| 8 | The engine: Listen routing, the pedal and Hold, Space, tilt, limiter, Stop, the guard | ✅ |
| 9 | Parameters, nine pages in four tabs, the patch map | ✅ |
| 10 | 16 factory presets, each in its own key, level-matched at −16 LUFS on an ambient phrase (`tools/phrase.h`); `make demos` maps each one | ✅ |
| 11 | Bench cases (the worst with motion), a PGO trainer for the presets, an offline soak that fails on the guard and watches the limiter | ✅ |
| 12 | Docs | ✅ |
| 13 | The device run: installed (a snapshot of m1, 2026-10-07; v0.0.2, 2026-10-08) ✅; `make bench-device` ✅ (all five cases pass, worst p99 10.6%; the tables built in 1.6–1.7 s); playing it, the probe questions, listening 🔜; `tested.json` with the first release ⬜ | 🔜 |

Every task was implemented by a subagent and reviewed twice, for the spec and then for code
quality; each finding was checked against the code and fixed with a check that fails without it
(the commits of 2026-10-06 and 07 say what and by how much).

## Phase 0: the probe

What MPC does with an **instrument** plugin, measured on the device. Folded into M1's device run:
with `/tmp/ambientforce.trace` present the M1 build logs every parameter change, every suspend and
resume and where the audio thread took the resume, every MIDI event as it came in (channel and
all), each table's build time, and which of MPC's threads made each call
([diagnostics](BUILDING.md#diagnostics-on-the-device)). The answers go in `docs/PROBE.md`, and then
the concept becomes `docs/DESIGN.md`.

- ⬜ **What Stop sends to an instrument**: all-notes-off, a suspend (as it does to an insert, within
  ~100 ms), or nothing? The engine takes a falling transport or a suspend under 250 ms as Stop; the
  trace shows which comes.
- ⬜ **CC 64 and the Force's pad latch**: whether a sustain pedal reaches a plugin as CC 64, and what
  the pads' latch sends (held notes, or note-offs on release).
- ⬜ **MIDI tracks into a plugin track on separate channels**: if they can, a MIDI channel per
  stratum (Omni / 1–4) is cheap to add.
- ⬜ **The crossfader learned on an instrument track** (the scenes' morph in M3).
- ⬜ **The plugin writing a WAV to the SSD** (Memory's Keep in M2).
- ⬜ **Two instances at once**: the shared tables, the CPU, the pages.
- ✅ **How fast the lifetime tables build on the A17**: all twelve in 1.6–1.7 s when the first
  instance loads (0.4 s on x86, 4.3–4.9 s under qemu), at nice 10 on their own thread; until a table
  is built its slots play a sine.

## M2 to M4

From the concept ([§14](CONCEPT.md#14-roadmap)):

| Milestone | Contents | Gate | |
|---|---|---|---|
| **M2: Weather** | Air (modal resonators, pluck, patterns, Loop); Weather (procedural fields, your WAVs, Memory, Keep); Echo; 32 presets | Worst case ≤ 15% p99; the 24-hour `soak` passes | 🔜 Tasks 1–11 of 13 ([plan](plans/2026-10-07-m2-weather.md)) |
| **M3: Long time** | Drift, Swell, Gust, LFOs, the matrix, macro mappings per preset (the four fixed macros came in 0.0.2: [below](#playability)); Phase; scenes with Evolve and Autopilot; Patina; the Sky view; lifetime import from your WAVs | Hands off for an hour without a dull minute (a listening session) | ⬜ |
| **M4: v0.1** | 64 presets, 24 tables, 12 fields; the parameter list frozen, append-only from here; a CI release | A `tested.json` entry | ⬜ |

## Planned, not yet placed

- ⬜ **Tuning from a file** (`.scl` / `.tun`, PolyForce's) and **Tune Drift** (each pitch class
  wandering on its own, 0–15 cents): in the concept's harmony, not in M1.
- ⬜ **Split**: pads below a split note set the harmony and play Bloom, pads above play Air as a
  melody (with Air, M2).
- ⬜ **The CPU guard** (PolyForce's: shedding tails above 40% and 65% of a block), the tail
  handoff's second line of defence.
- ⬜ **Voice leading's taste**: `voiceLeadCost` is the plan's baseline (the least total movement); a
  `TODO(Roland)` in `dsp/harmony.cpp` names the trade-offs (parallel fifths, the upper voices, common
  tones held still).

## Deferred and not planned

| Feature | Why not (now) |
|---|---|
| Randomize | An instant jump of every value under a drone that holds for minutes isn't music; Evolve (M3) glides to a mutation over minutes instead |
| Separate outputs per stratum | MPC hears only a plugin's first stereo pair |
| Air driving other tracks | MPC drops a plugin's MIDI output |
| Instances sharing one harmony ("Link") | A stratum per track, each with its own mixer channel and clips: after v0.1 |
| Echo's reverse mode | After v0.1 |
| Step sequencers, an arpeggiator | The Force's sequencer is better; PolyForce has an arp |
| Crush, overdrive, more reverb algorithms | EffectForce's (the scope rule: a feature a sibling has, not tied to the strata or to long time, stays there) |
| Third-party sample content | Every factory sound is computed; your own sounds come from the SSD |

## Decisions

### From the concept

- 2026-10-06 — **Space, Echo and Patina are built in**, reusing EffectForce's code: the tail handoff,
  Memory and complete presets all need Space inside the instrument (CONCEPT §17.1).
- 2026-10-06 — **Memory is kept as an SSD WAV** (Keep), not inside the project chunk (5.6 MB per
  instance per save) (§17.2).
- 2026-10-06 — **Stop fades** over 8 s by default; On Stop: Keep, Fade or Cut (§17.3).
- 2026-10-06 — **Bloom has 6 voices**: 8 would cost about 1.3% more at p99 (§17.4).
- 2026-10-06 — **Listen modes per stratum**, defaults Bloom Notes, Ground and Air Harmony, Weather
  Free; Split off by default (§17.5). **No Off** among them: silence is the Mute's job, and nothing
  sounds before the first note, Free strata included.
- 2026-10-06 — **No Focus selector**: the stratum pages share one knob layout instead, because MPC
  records automation per parameter (§17.6).
- 2026-10-06 — **Hardware product names stay in CONCEPT §2 only**: code comments and user docs
  describe AmbientForce on its own terms (§17.7).
- 2026-10-06 — **Every task implemented by a subagent and reviewed twice**: for the spec, then for
  code quality.

### Harmony

- 2026-10-06 — **Drop 2 is the standard rule**: the close voicing's second-highest note an octave
  down (C major on C4: E3 C4 G4; the plan's 55 60 64 was an error).
- 2026-10-06 — **Voice leading keeps the voicing**: each inversion of the close voicing is voiced by
  the Voicing's rule before it is tried; Spread never inverts. Before, every chord after the first
  fell back to a close inversion.
- 2026-10-06 — **Snap goes to the nearest scale tone**, the lower one on a tie.
- 2026-10-06 — **Degrees under Chromatic** plays each key as played, moved up by the key.
- 2026-10-06 — **A root outside the scale gets a parallel chord**: the stack of the degree below it,
  moved up to it, so a played key always gets a chord on itself.
- 2026-10-06 — **The harmony tracks physical keys**, each with the note it mapped to: two keys may
  map to one note (Degrees: C and C#), and the note is held until both are up. Key releases change
  nothing until the last key is up.
- 2026-10-06 — **The Spread chord** is the triad's tones, always voiced Spread (else it would be the
  Triad again).

### Tables

- 2026-10-06 — **Every factory table is computed**, from additive life models: no sample data ships.
- 2026-10-06 — **Frames are equal-RMS**: a lifetime table carries timbre, not level, so Age 1 is as
  loud as Age 0.
- 2026-10-06 — **The builder thread is joined at unload**, owned by a static rather than detached (a
  thread still building would run on in unmapped code); **the tables are freed only with no instance
  alive**; it runs **at nice 10**, off the UI's back. One lock for counting instances, starting and
  releasing.
- 2026-10-06 — **Each life model has its own seed**, so the browser's order can change without
  changing a sound.

### The oscillator

- 2026-10-06 — **Hermite for Bloom's A, linear for B**: B is mostly a digital wave used as a blend or
  a modulator, and the linear read's images (−64 dB under a saw's fundamental at worst) aren't worth
  a second Hermite read per voice. 2026-10-07: **Ground's partials read linearly too** (a drone
  lives low; its images measured −66 dB or further down).
- 2026-10-06 — **An oscillator nobody hears is skipped but advanced**: B at Mix with Blend 0, A at
  Blend 1. Its phase against the other's matters whenever the two are in tune, and must not depend on
  how long the blend sat at an end.
- 2026-10-06 — **Sway reflects at the ends** of the table rather than clamping, so it keeps moving at
  the depth asked.

### Ground

- 2026-10-06 — **Beat is the root–fifth beat rate in Hz**: the plan's offsets scaled by 0.4, so Root's
  3rd harmonic and Fifth's 2nd beat at exactly Beat; the other meetings keep the plan's shape.
- 2026-10-06 — **A fixed −18 dB headroom** (0.125, exact) on the partials' sum, with **Body lifting a
  formant 4.7 dB at most** (twice the formants over 0.7 of the dry lifted one by up to 9 dB): the
  loudest Ground there is peaks at 1.26 before the limiter. Level matching sets the presets.
- 2026-10-07 — **A new root takes the nearest octave** of its pitch class (the shortest glide), within
  Register's range; **a Register change dips out and jumps** (40 ms each way), no glide.

### Bloom

- 2026-10-06 — **The tail handoff's send boost is energy-matched to Tail Voice**: the send over the
  1.5 s handoff carries what the whole release would have (1.10 at Release 10, 1.50 at 30, at most
  4). The plan's √(Release / 1.5) sent 6.9 dB too much at 10 and 8.3 dB at 30. The send fades out over
  the handoff's last 0.2 s, so it never steps.
- 2026-10-06 — **Unison 2 under 1 cent of Detune plays as unison 1**: two halves that close only comb.
- 2026-10-06 — **The unison's second half starts a quarter cycle behind** the first, so the sum never
  depends on the seed.
- 2026-10-06 — **A note pressed again during its handoff swells again in place**, in its voice: one
  key pressed again and again keeps to one voice.
- 2026-10-06 — **No handoff when Space can't carry it**: with no send, Tail Space releases as Tail
  Voice.
- 2026-10-06 — **Table changes crossfade over 20 ms**, in Bloom and Ground, the sine's replacement by
  a newly built table included.

### Space

- 2026-10-06 — **Haze and Abyss are added** after EffectForce's four modes, which stay bit for bit as
  they were.
- 2026-10-06 — **Abyss is deliberately louder** (Decay × 4): Space doesn't compensate; level
  matching handles it.
- 2026-10-07 — **Rise's full duck is −28 dBFS**, calibrated against the real send levels (a triad at
  Init's levels sits at −25 dBFS); the first guess, −12 dBFS, barely ducked.

### The engine

- 2026-10-07 — **Volume before the limiter**, so −1 dBFS holds at every volume (after it, +6 dB would
  put a limited peak at +5 dBFS).
- 2026-10-07 — **A fixed make-up gain (+8.9 dB) at the output**, after the mix: Init, which is
  `Patch{}`, plays at −16 LUFS at the default volume of −6 dB, and the presets sit at −6 dB or under
  with 12 dB of the knob above them (without it Init needed +2.9 dB, 3 dB from the knob's end). The
  strata, their sends and Rise keep their own calibration.
- 2026-10-07 — **One first-order tilt shelf**, pivoting at 800 Hz, instead of the plan's two
  one-poles.
- 2026-10-07 — **Keys held by the pedal or Hold count as held for the harmony**: the memory counts
  from when the chord is let go. A full harmony (16 keys) lets the oldest pedal- or Hold-held key go
  first.
- 2026-10-07 — **Hold's latch starts a new chord only after all fingers have left**; a key pressed
  while a finger is still down joins the chord.
- 2026-10-07 — **Freeze makes Bloom release as Tail Voice**: a frozen reverb takes no input, so a
  handoff would lose the tail.
- 2026-10-07 — **A Fade turns round on a note-on** (or the transport starting); On Stop changed
  during a fade applies at once (Keep turns it round, Cut resets).
- 2026-10-07 — **Abyss holds its decay at a quarter of Bloom's Release**: it rings four times its
  Decay.
- 2026-10-07 — **The guard keeps the keys and the harmony**: it resets every DSP state, and the
  strata that follow the harmony come back by themselves.
- 2026-10-07 — **Volume, return and pans glide 10 ms in straight lines**, carried across pieces, so
  an event that cuts a piece short never makes them jump.
- 2026-10-07 — **Bloom switched back to Notes replays the held keys' chords**, so Bloom and Ground
  agree.

### The surface

- 2026-10-07 — **No RANDOMIZE**: an instant jump under a drone that holds for minutes isn't music;
  Evolve in M3 is the musical answer.
- 2026-10-07 — **Names shortened to fit MPC's name box** (126 px of a knob's 130): Ground Rate, Bloom
  Rate, Gnd Octave, Gnd Breath, Blm Release, Couple Amt, Low Cut.
- 2026-10-07 — **Text measured deterministically**, from the font's own advance table with 3 px to
  spare, not with whatever Pillow a machine has (the same name passed on one machine and failed on
  another).
- 2026-10-07 — **Hold, Freeze and the mutes are segments**, not toggle tiles: a tile's release echo
  is a second tap.
- 2026-10-07 — **`Patch{}`'s defaults are what Init plays**: the levels as their default knobs
  squared, Space an ambient hall; `test/params_test.cpp` holds the two together.
- 2026-10-07 — **Option lists are the engine's names**, checked entry by entry as
  `plugin/patch_map.cpp` compiles; Color Int's options read min3, maj3 (segments show capitals, so
  m3 and M3 would look alike).

### Playability

After the first listen on the device: "we have a ton of features, but nobody knows what all these
knobs are doing".

- 2026-10-07 — **Four macros brought forward from M3, as fixed relative macros.** The concept plans
  HORIZON with a mapping per preset, with the matrix in M3 (CONCEPT §7.1). Horizon, Motion, Glow and
  Density come now, with one mapping for every preset, relative to what the preset has: 0 is the
  preset bit for bit, ±1 moves each field it owns one way, clamped to its range. They bend the
  `Patch` the knobs make (`plugin/patch_map.cpp` `applyMacros`), never the knobs, so the engine is
  unchanged and a knob's value and text stay what the preset says. Mappings per preset stay M3's.
- 2026-10-07 — **Help on the status line**, not a page of its own: every control a hand moves has a
  one-line help text beside it in `surface.py`, shown on the status readout (on every page) for 4 s
  after a move, a preset's `about=` description for 6 s after it loads. Each line is measured to fit
  the narrowest status readout (PLAY's, 684 px of text at MPC's 26 px): about 60 characters.
- 2026-10-07 — **The status line is timed on the audio thread's samples**: the UI side only notes the
  last move and counts preset loads (atomics); `processReplacing` decides which line shows (another
  control takes over only after half a second, so automation doesn't flicker) and pushes
  `audioMasterUpdateDisplay`; `statusText()` reads what it decided. A move is a set that changes the
  value by more than MPC's rounding (0.0006: MPC sends values to 1/1000, so a preset's value echoed
  back rounded isn't one); preset and project loads show nothing. While automation plays, the help
  lines show: they are moves too.
- 2026-10-07 — **The macros keep the level.** Horizon's far half steps the dry back 3 dB and raises
  the sends only 3 dB (+6 dB put Cathedral Breath, Abyss with shimmer, on the limiter for 14% of its
  phrase); Glow's tilt is ±0.3 (±0.6 made the dark half up to 3 LU louder); dark and thick trim the
  volume by 1.5 and 1 dB. Over the 16 presets at both ends: no limiting, at most 4.2 LU down (far).
  2026-10-07 (review), the full sweep over all 28 at both ends (`AF_FULL_MACRO_SWEEP=1`) found two:
  Glow moved Overtone Choir's band-pass off the overtone it sings (dark +3.1 LU and on the
  limiter 2.5% of the phrase, bright 11 LU down), and Horizon's near end took Frozen Sky, nearly
  all reverb, 11.1 LU down. Now Bloom Tone moves with Glow and Horizon only on a low-pass (on BP or
  HP it picks a band, not a brightness), and near takes the sends down 6 dB (was 9) and leaves Space
  Level alone (it took 3 dB off too). The sweep then: no limiting, at most 8.0 LU down (Frozen Sky,
  near); the 16 corners of all four on the three hottest presets at most 5.4 LU down, no limiting.
- 2026-10-07 — **Density leaves Blend alone**: crossfading toward Table B dipped the level 2–5 LU, and
  with B a sine (the default) it thinned the sound rather than thickening it. Unison and the chord
  aren't macro targets either: switching them would jump.
- 2026-10-07 — **The breath and the sways can sync to the bar; Free stays the default.** CONCEPT
  §7.1 plans Free / Sync rates; asked for now, for the breath and both sways: Sync runs one cycle per
  division (1/4 to 64 bars of 4/4) on a beat clock per stratum, locked to MPC's position while it
  plays (SubForce's synced busses' rule) and running on at the tempo while it is stopped.
  (Re-review: each stratum's clock ran on by itself only while it was rendered, so a silent Bloom's
  stood still while a sounding Ground's ran on: 3 s stopped put a chord 0.17 of the table off the
  grid the drone breathed on. Now the engine keeps one count, MPC's position while it plays and on
  at the tempo while it is stopped, on the sample count asleep or awake, and hands it to both
  strata before every piece: one grid.) Free is
  the default everywhere, because phasing (CONCEPT §7.2) needs cycles that never line up. A synced
  breath tops on each division's downbeat, one pulse. Synced, Bloom's voices are staggered on the
  bar (decided at the review): voice i sways a sixth of a cycle times i after the bar's phase, the
  same place from note to note, so a synced chord still shimmers while it stays locked; in lockstep
  a chord's notes moved as one, on their own random phases they wouldn't be on the bar. Breath got a
  rate knob of its own, its default the
  knob's middle (0.07 Hz, 128 times either way), so it reads back bit for bit as the breath every
  preset was made with. Motion bends the free rates, Breath Rate among them, never a division
  (that would jump).
- 2026-10-07 (review) — **A synced cycle is pulled to the bar, never set.** Setting the phase to the
  clock's stepped the breath's level up to 6 dB when MPC started, located or looped, or on Free ->
  Sync, and collapsed Bloom's voices onto one phase for good. Now every cycle keeps its own free
  phase, moving on whenever the cycle is stepped (per voice in Bloom), and the phase it plays is
  pulled toward its target (the clock's synced, plus a Bloom voice's stagger; its own free) with a
  50 ms time constant, the short way round, landing exactly on it within 1e-9 (about a second after
  a jump of half a cycle): across those events the gain moves at most about 0.2 dB a control step at
  Breath 100% (re-review, measured: a jump of just under half a cycle landing at the breath's
  steepest point, 0.16 dB at 120 BPM and 0.20 dB at 300; 0.10 dB, said here before, was the
  measured events', not the worst), a locked cycle is exactly the clock's, Free stays bit for bit
  what it was, and back on Free Bloom's voices drift apart again. A division faster than 4 Hz (a
  sway) or 8 Hz (a breath) at the tempo doubles. (Re-review: a cycle nobody steps stood still, a
  Ground the engine skips or a Bloom voice not in use, and then glided from where it stood: a chord
  after 1.3 s of silence swayed up to 0.41 of the table off its place at its first step, the breath
  unmuted 3.1 dB off the beat. Now the first step from silence lands on the target at once, where
  no jump can be heard. A free cycle pauses with its stratum or voice, as it always did, and goes
  on from there: it has no other place to start on. One `PulledCycle` in `dsp/common.h` now serves
  the breath and the sways, each passing its own free advance so Free keeps its arithmetic; a full
  step's share of the pull is a constant, and a free cycle on its own phase skips the pull, so its
  step costs about what it did before the pull.)
- 2026-10-07 (review) — **A macro under 0.5% is 0**, as its knob reads: a knob turned back by hand
  lands within MPC's 1/1000 of the middle, and the preset then plays as saved (no shimmer of 0.0006
  woken up). Loading a preset sets the macros to what it holds with every other control (a preset
  is a complete sound): 0 unless it was saved with a macro moved; the factory presets keep them at
  0. (Re-review: the knob's "0%" and the macro's 0 are one constant, `kBipolarZero`, so they can't
  drift apart.)
- 2026-10-07 — **The DRONE and BLOOM OSC pages traded repeats for motion**: DRONE's second bank,
  which repeated GROUND's, is the Breath (depth, rate, Sync, Div) and the Sway (the same); BLOOM
  OSC's is the Sway and the voice's Age, Smear, Pan and Width, Blend and Breath moving up to the
  oscillators.
- 2026-10-07 — **The PLAY page gave the two Tones, Space Decay and Shimmer to the macros**: its first
  bank is the macros, Freeze, Hold, Bloom Age and the volume; the second the levels, Bloom Swell, key,
  scale, chord and Gravity. The macros' parameters come after Tilt, so every earlier sound
  parameter keeps its index.

### Presets, bench, soak, the device

- 2026-10-07 — **The demo phrase plays each preset in its own key and scale** (the tonic chord, then
  IV, in Lydian II; held 12 s each, 16 s of release), so a preset sounds as it is meant to and the
  levels still compare.
- 2026-10-07 — **Presets are matched without touching the limiter**: `preset-levels` sets each
  preset's volume for −16 LUFS on the phrase and the limiter only holds the peaks under −1 dBFS; how
  hard it works is printed, and a preset that leans on it is one to fix, not to turn down.
- 2026-10-07 — **The worst bench case includes motion**: Sway at full depth and 2 Hz on both strata,
  Bloom's Smear and Breath at full, so the read position crosses frames in every render (the dearest
  read: 423k ARM instructions a block against 404k without).
- 2026-10-07 — **The bench waits for the tables**: no case plays the sine fallback or times the
  builder.
- 2026-10-07 — **The soak fails on a trip of the guard**, whose zeroed block would otherwise look
  clean, and warns when the limiter works more than 5% of a window.
- 2026-10-07 — **The trace never writes from the audio thread**: it leaves notes (the resume, a ring
  of MIDI events) that MPC's own threads write at their next call.
- 2026-10-07 — **Bloom's unison 2 stays**: the device bench puts everything M1 has at its heaviest at
  p99 10.6%. The instruction counts at PolyForce's ~1 ns an instruction had put it at 14.6–14.9%; the
  Force ran AmbientForce's code at about 0.6 ns an instruction.
