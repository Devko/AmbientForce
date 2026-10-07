# Performance

- [The budget](#the-budget)
- [How it is measured](#how-it-is-measured)
- [Estimates by instruction count](#estimates-by-instruction-count)
- [Device measurements](#device-measurements)
- [Memory and load time](#memory-and-load-time)
- [What keeps it cheap](#what-keeps-it-cheap)
- [If the device disagrees](#if-the-device-disagrees)

---

## The budget

MPC renders 128-frame blocks: **2902 µs per block**, per plugin instance. A plugin passes at
**p99 ≤ 15%** and **max ≤ 50%** of the block (warns up to 35% / 80%), the rule the family uses.

AmbientForce's ceiling is everything on at once, so the whole budget is planned up front
([CONCEPT §11](CONCEPT.md#11-budget)) from the siblings' measured costs. The part M1 builds:

| Block | Load | Target (p99, % of the block) |
|---|---|---|
| Bloom | 6 voices, 2 oscillators, unison ≤ 2, 1 SVF | 4.0 |
| Ground | 1 voice, 5 partials | 0.8 |
| Space | the 8-line reverb, shimmer on | 3.8 |
| The brain, the engine, idle | | 0.6 |

Air, Weather, Echo and Patina (M2, M3) take the rest, to 13.9% with everything on. **M1's gate:**
Bloom 6×2 with Ground and Space at p99 ≤ 15% on the device.

## How it is measured

- **`make bench-device`** copies the `.so`, its profiling build and `afbench` (`tools/bench.cpp`) to
  the Force, runs them pinned to core 1 while MPC keeps running, and deletes them. Each case plays
  MIDI through the plugin's own entry points and times every block with the thread's CPU clock
  ([Building](BUILDING.md#benchmarking-on-the-device)). The cases: idle (asleep), the Init preset
  holding a triad, Ground alone with every partial, Body and Breath, Bloom alone at six voices of
  unison 2 with FM, and the worst case (six-note chords re-struck every 2 s, unison 2, FM, Ground at
  full, Space in Abyss with shimmer).
- **Stage timing**: the profiling build (`-DAF_STAGE_TIMING`, `make arm-bench-stages`) laps a clock
  between the engine's stages, ground, bloom, space and out (tilt, volume, the guard, the limiter,
  Stop's fade), and `afbench` prints each case's time per stage. It reads the clock between stages,
  so it reads a little higher than the shipped build.
- **Profile-guided**: the shipped `.so` is built with a profile from `tools/pgo_train.cpp`, which
  plays phrases, chords past six voices, the pedal and the factory presets under `qemu-arm`.
- **Instruction counts**, until the device: ARM instructions per 128-sample block, counted under a
  plugin-enabled `qemu-arm` for code built with the device's flags. They are exact and repeatable,
  so every review measured its change with them.

## Estimates by instruction count

From the code's own measurements (each header in `dsp/` gives its figures), 2026-10-07. The percentages
take **PolyForce's calibration, about 1 ns an instruction on the device** (its instruction counts
against its device bench): 2902 µs is about 2.9 million instructions.

| Case | ARM instructions per block | ≈ % of the block |
|---|---|---|
| Asleep (idle) | 0.4k (about 425) | 0.01 |
| **Init holding a triad** (the knobs' defaults) | **206k**: Space 113k, Bloom 46k, Ground 35k, the engine 4.5k | **7.1** |
| **The worst case** (6 voices of unison 2 with FM, every Ground partial and Body, Abyss with shimmer) | **394k**: Bloom 224k, Space 107k, Ground 46k, the engine 7k | **13.6** |

Per stratum:

| Stratum | ARM instructions per block | ≈ % of the block | Target |
|---|---|---|---|
| Bloom, 6 voices sounding, unison 1 (Mix / FM) | 97k / 121k | 3.3 / 4.2 | 4.0 |
| Bloom, 6 voices sounding, unison 2 (Mix / FM) | 153k / 203k | 5.3 / 7.0 | 4.0 |
| Ground, every partial / the default four / Root alone | 40.0k / 34.3k / 17.0k | 1.4 / 1.2 / 0.6 | 0.8 |
| Ground's Body, Breath | +5.3k, +0.6k | 0.2, 0.02 | |
| Space (Hall at Init's settings; Abyss with shimmer) | 107k–113k | 3.7–3.9 | 3.8 |

The oscillator's inner loop, ARM instructions a sample over one frame / a pair of frames / a
position crossing frames within the render: Hermite (Bloom's A) 41 / 65 / 84, linear (Bloom's B,
Ground's partials) 20 / 34 / 52; FM adds 3 to 5. A Bloom voice at unison 1 is about 126
instructions a sample: A's read 74, the breath, SVF, pan and gains 36, the control steps and the
bus the rest.

**How far to trust them.** Instruction counts miss what the Force adds: cache misses, and VFP
divisions and flag transfers that stall. PolyForce's ~1 ns an instruction is one calibration;
EffectForce's (36k–44k instructions to a point of the block on its everything-on case) would put the
worst case nearer 9–11% on average, and its device p99 ran 1.24 times its average. Both put M1's
worst case under the gate, with Bloom's unison 2 the largest single cost and over Bloom's own share:
the device bench has the final word.

## Device measurements

(device bench: Task 13)

| Case | avg | p99 | max |
|---|---|---|---|
| Idle (asleep) | (Task 13) | (Task 13) | (Task 13) |
| Init holding a triad | (Task 13) | (Task 13) | (Task 13) |
| Ground alone: every partial, Body, Breath | (Task 13) | (Task 13) | (Task 13) |
| Bloom alone: 6 voices, unison 2, FM | (Task 13) | (Task 13) | (Task 13) |
| **The worst case** | (Task 13) | (Task 13) | (Task 13) |

Percent of the 2902 µs block, the profile-guided build, MPC running. `make bench` on x86 only proves
the bench works; the Force is far slower per sample.

## Memory and load time

- **The tables**: about 38 MB per process, shared by every instance (eight lifetime tables of 256
  frames at 4.7 MB each, 16-bit samples with a scale per frame, and four one-frame waves). They are
  all built when the first instance loads, not on demand. The concept planned at most three tables in
  use in a shared cache (≤ 13.5 MB); M1 keeps its whole library of twelve built instead.
- **An instance**: Space's buffers, about 870 KB (two of the lines' buffers doubled for Abyss);
  everything else is fixed arrays.
- **Load time**: the builder thread builds all twelve tables in 0.42 s on x86 at -O2 and 4.3 s under
  qemu (about 50 ms and 0.5–0.6 s a lifetime table); on the Force: (device: Task 13). It runs at
  nice 10 on its own thread, and the slots play a sine until their tables arrive.

## What keeps it cheap

- **Asleep is free:** before the first note, after a Fade or a Cut, and whenever nothing can be heard
  and Space is silent, `render()` writes zeros and runs no DSP.
- **One drone voice**, not one per note: Ground costs the same for any chord.
- **The tail handoff:** a long release costs a voice for 1.5 s, and Space carries the rest. Six
  voices feel like many more.
- **Pay for what is on:** a Ground partial at 0 isn't read (it is skipped along, in step); B isn't
  read at Mix with Blend 0, nor A at Blend 100%; a muted stratum renders no voices; Body at 0 costs
  nothing; Space isn't run while its return is 0.
- **Linear reads where they are enough:** Hermite only for Bloom's A, which is what you hear; B and
  Ground's partials read linearly, half the reads, their images far down.
- **Control rate:** envelopes, scans, glides, fades and filter targets are worked out once per
  32-sample step; gains ramp and coefficients glide across it.
- **NEON where the work is parallel:** the reverb's eight lines as two vectors; a voice's two unison
  halves side by side in the filter's two lanes.
- **No libm per sample:** the tunings are tables of semitone offsets; what needs libm (a glide's
  rate, Equal's ratios, the pans, the handoff's boost) is worked out again only when what it depends
  on changes.

## If the device disagrees

The caps fall in the concept's order ([§11](CONCEPT.md#11-budget)): **Bloom's unison 2 → 1** first,
then Air 6 → 4 voices, then Weather 16 → 12 grains. In M1 only the first applies; the PolyForce CPU
guard (shedding tails above 40% and 65% of a block) is planned as the second line of defence
([Roadmap](ROADMAP.md#planned-not-yet-placed)).
