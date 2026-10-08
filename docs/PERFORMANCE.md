# Performance

- [The budget](#the-budget)
- [Device measurements](#device-measurements)
- [How it is measured](#how-it-is-measured)
- [Instruction counts](#instruction-counts)
- [Memory and load time](#memory-and-load-time)
- [What keeps it cheap](#what-keeps-it-cheap)
- [If it runs over](#if-it-runs-over)

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

## Device measurements

2026-10-07, the Force (Cortex-A17, MPC OS 3.9), `afbench -s 3 -c 1` while MPC runs, the
profile-guided build, percent of the 2902 µs block:

| Case | avg | p99 | max | |
|---|---|---|---|---|
| idle (asleep) | 0.11 | 0.13 | 0.58 | PASS |
| init chord: Init, one key (its triad, Ground on the root, the Hall) | 4.20 | 5.36 | 6.67 | PASS |
| drone: Ground alone, every partial at full, Body and Breath | 3.42 | 4.34 | 5.40 | PASS |
| bloom 6x2: Bloom alone, six voices of unison 2 with FM | 6.68 | 7.99 | 8.66 | PASS |
| **worst** (below) | **8.95** | **10.63** | 11.84 | **PASS** |

The worst case: six keys let go for six others every 2 s (every voice taken from its release),
unison 2 with FM, Ground at full (every partial, Body, Breath), Sway at full depth and 2 Hz on both
strata, Bloom's Smear and Breath at full (the read position crossing frames in every render, the
dearest read), Space in Abyss with Shimmer 100%, Tilt on. No preset comes near it.

**M1 passes its gate with room:** everything M1 has, at its heaviest, is p99 10.6%, which leaves
about 4.4 points of the 15% for Air, Weather, Echo and Patina (the concept's estimate for them: 4.7).
Bloom's unison 2, planned as the first cap to fall, stays.

Where the time goes, from the profiling build (it reads the clock between stages, so its totals run
a little over the shipped build's), µs per block:

| Case | ground | bloom | space | out |
|---|---|---|---|---|
| init chord | 22.3 | 35.6 | 79.2 | 2.8 |
| drone | 28.3 | 6.8 | 78.9 | 3.0 |
| bloom 6x2 | 2.3 | 123.7 | 79.2 | 2.9 |
| worst | 37.9 | 139.1 | 97.2 | 6.0 |

- **Space is the largest fixed cost**, about 80 µs (2.7% of the block) in the Hall whatever plays,
  97 µs (3.3%) in Abyss with shimmer: under its 3.8% share.
- **Bloom**: six voices of unison 2 with FM, 124 µs (4.3%), 139 µs (4.8%) with the worst case's
  motion, against its 4.0% share; a triad at Init's settings, 36 µs (1.2%).
- **Ground**: every partial with Body and Breath, 28 µs (1.0%), 38 µs (1.3%) with a fast Sway;
  the default drone 22 µs (0.8%), its share.
- **The output** (tilt, volume, the guard, the limiter, Stop's fade): 3 to 6 µs.

## How it is measured

- **`make bench-device`** copies the `.so`, its profiling build and `afbench` (`tools/bench.cpp`) to
  the Force, runs them pinned to core 1 while MPC keeps running, and deletes them
  ([Building](BUILDING.md#benchmarking-on-the-device)). Each case is a fresh instance set up through
  its parameters by index, played 2 s untimed (the voices sounding, the reverb full) and then timed
  for `-s` seconds, every `processReplacing` call on the thread's CPU clock. Before the first case
  the bench waits for the table builder to finish, so no case plays the sine fallback or times the
  builder.
- **Stage timing**: the profiling build (`-DAF_STAGE_TIMING`, `make arm-bench-stages`) laps a clock
  between the engine's stages: ground, bloom, air, weather, echo, space and out.
- **Profile-guided**: the shipped `.so` is built with a profile from `tools/pgo_train.cpp`, which
  plays an ambient phrase through every Space mode, Couple mode, Listen pair, tuning, chord type and
  voicing, Hold and the pedal, Stop and a suspend, then every factory preset, under `qemu-arm`.
- **Instruction counts**, between device runs: **`make arm-icount`** prints each bench case's ARM
  instructions per 128-sample block, for the plugin built with the device's flags but no profile
  (the profile was trained on the code before the change), counted by qemu's `insn` plugin under a
  `qemu-arm` built with TCG plugins. Each case plays twice, 256 and 768 blocks after its 2 s, and
  the difference over 512 is its figure; only the thread that plays is counted, and everything
  whose cost depends on the machine (loading the plugin, the wait for the tables, setting the case
  up) runs on another. So the figures are exact and the same on every run with the same qemu, cross
  compiler and ARM libraries, and every change is measured with them ([below](#instruction-counts);
  [Building](BUILDING.md#instruction-counts) has the details and how to build that qemu).

`make bench` on x86 only proves the bench works; the Force is far slower per sample.

## Instruction counts

ARM instructions per block, the whole plugin through `VSTPluginMain` (the bench's cases, from
`make arm-icount`: the plain build of 0.0.2 as released, `d037d2d`, by Ubuntu 24.04's
`arm-linux-gnueabihf-g++` 13.3.0, run with its glibc 2.39 and libstdc++, counted by qemu 8.2.2's
`insn` plugin; counts compare only with counts from the same qemu, compiler, libraries and bench,
[Building](BUILDING.md#instruction-counts) says why), and each stratum on its own (from the headers
in `dsp/`, which give the figures):

| Case | ARM instructions per block | By ~1 ns an instruction | On the device (avg, M1.1 `4788a5d`) |
|---|---|---|---|
| idle | 1.2k | 0.04% | 0.11% |
| init chord | 197.2k | 6.8% | 4.20% |
| drone | 162.2k | 5.6% | 3.42% |
| bloom 6x2 | 313.0k | 10.8% | 6.68% |
| worst (its re-strike's block) | 394.5k (411.7k) | 13.6% (14.2%) | 8.95% |

The worst case's figure is the 512 blocks' average, one re-strike among them. The re-strike's own
block (the twelve keys' events, six voices taken from their release) counts 411.7k, the 64 blocks
before it 397.9k a block. Both come from the same main-thread counts, run by hand: the re-strike's
keys go in before block 689 after the 2 s (counting from 0), so `afbench <so> --icount worst 690`
less `689` is that block alone, and `689` less `625`, over 64, the blocks before it. The
profile-guided build, which the device runs, counts 1–3% either side of these: 195.0k, 157.6k,
318.0k and 399.7k (idle 1.1k). The re-review of `8c94203` took 0.8k to 1.6k a block off every case
(`pullPhase` without `exp` on a full step); the Space fix of 0.0.2 put back 30 instructions a block
(4 idle).

These figures replace the ones this table had through M1 (init chord 210k, drone 175k, bloom 6x2
355k, worst 423k, 433k where it re-strikes), which came from another qemu build and ran 7–14% over
`arm-icount`'s for the same code: the commit they were taken at (`9183bd8`) counts 196.7k, 161.7k,
312.5k and 392.3k here. Today's code counts under 1% over that, for the synced breath and sways.
**With M2's engine** (Task 8: Air, Weather, Echo and Memory wired in, all off in these cases), the
same cases count 4.7k to 4.8k a block more: Memory recording the output (3.6k) and the new routing.
M2's own cases play the engine itself, linked into the bench, until the plugin maps their parameters
(`worst engine`, the `worst` case played so, counts 0.4k under the plugin's):

| Case (M2's engine) | ARM instructions per block | Its spike, counted by hand |
|---|---|---|
| init chord / drone / bloom 6x2 | 202.0k / 167.0k / 317.7k | |
| worst | 399.3k | its re-strike's block 417.2k |
| air | 146.6k | |
| weather | 180.9k | |
| worst m2 | 528.7k | a re-strike's block with its Remember 586.7k; with the Remember refused 569.0k |
| air strike | 147.5k | the strike's block 357.5k, the ten after it 156.9k |
| echo resume | 141.2k | the phrase's first block 215.2k, the 34 after it 199.3k, the silence 114.9k |

M2's worst case, part by part (the case less each part): Weather 60.9k (in Stretch, the dearer mode;
59.7k in Cloud), Air 35.9k, Echo 32.4k with its sends, and the 4.8k above; 134.2k over 0.0.2's worst,
where the plan budgeted 128k. At the plan's 0.0268 points of p99 a thousand, 528.7k is **14.17%**
(device: pending), under the 14.5% at which the plan's caps start. The spikes are a block or ten in
2 s: `afbench <so> --icount <case> 690` less `689` is the re-strike's (or the strike's, or the
phrase's) block alone. In `worst m2` a Remember is asked with every re-strike and taken with every
second one (Memory takes one after 2 s of recording, 88200 samples; the re-strikes are 88192 apart),
the first at block 0: `1379` less `1378` is a re-strike with its Remember (the dearest block, one in
4 s), `690` less `689` one whose Remember is refused.

Task 8's review moved Air and Weather's pans and Echo sends into their own output pass, wrote Memory
straight from the bus and gave Air its clock only when it renders: from the counts first made with
Task 6 merged (`61642fe`: init chord 203.0k, worst 400.4k, air 149.0k, weather 181.5k, worst m2
532.0k in Cloud) about 1k came off every case and 4.5k off M2's worst (Cloud for Cloud; Air's part
38.4k to 35.9k, Weather's 60.5k to 59.7k).

The strata's figures below and the oscillator's were counted before `arm-icount` too: compare them
with each other, not with the table above.

| Stratum on its own | ARM instructions per block |
|---|---|
| Bloom, 6 voices sounding, unison 1 (Mix / FM) | 97k / 121k |
| Bloom, 6 voices sounding, unison 2 (Mix / FM) | 153k / 203k |
| Ground, every partial / the default four / Root alone | 40.0k / 34.3k / 17.0k |
| Ground's Body, Breath | +5.3k, +0.6k |
| Space (Hall at Init's settings; Abyss with shimmer) | 113k; 107k |

The oscillator's inner loop, ARM instructions a sample over one frame / a pair of frames / a
position crossing frames within the render: Hermite (Bloom's A) 41 / 65 / 84, linear (Bloom's B,
Ground's partials) 20 / 34 / 52; FM adds 3 to 5. A Bloom voice at unison 1 is about 126
instructions a sample: A's read 74, the breath, SVF, pan and gains 36, the control steps and the
bus the rest.

**The calibration.** Before the device run the percentages took PolyForce's figure, about 1 ns an
instruction on the Force, and put the worst case at 14.6–14.9% (with the earlier counts), inside the
gate with little room for the device's jitter. Against `arm-icount`'s counts the device ran
AmbientForce's code at **about 0.61–0.66 ns an instruction** on average (init chord 0.62, drone 0.61,
bloom 6x2 0.62, worst 0.66), with p99 1.2–1.3 times the average: 1 ns an instruction over-predicts
by 50–65%. The counts stay the measure of a change between device runs; for the budget, the
device's figures count.

## Memory and load time

- **The tables**: about 38 MB per process, shared by every instance (eight lifetime tables of 256
  frames at 4.7 MB each, 16-bit samples with a scale per frame, and four one-frame waves). They are
  all built when the first instance loads, not on demand. The concept planned at most three tables in
  use in a shared cache (≤ 13.5 MB); M1 keeps its whole library of twelve built instead.
- **An instance**: Space's buffers, about 870 KB (two of the lines' buffers doubled for Abyss);
  everything else is fixed arrays.
- **Load time**: the builder thread builds all twelve tables in **1.6–1.7 s on the Force** (0.4 s on
  x86, 4.3–4.9 s under qemu). It runs at nice 10 on its own thread, and the slots play a sine until
  their tables arrive.

## What keeps it cheap

- **Asleep is free:** before the first note, after a Fade or a Cut, and whenever nothing can be heard
  and Space is silent, `render()` writes zeros and runs no DSP (0.11% of a block, the plugin's own
  glue).
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

## If it runs over

The caps fall in the concept's order ([§11](CONCEPT.md#11-budget)): **Bloom's unison 2 → 1** first,
then Air 6 → 4 voices, then Weather 16 → 12 grains. M1 needs none of them (its worst case is p99
10.6%); M2's device bench decides for Air and Weather. The PolyForce CPU guard (shedding tails above
40% and 65% of a block) is planned as the second line of defence
([Roadmap](ROADMAP.md#planned-not-yet-placed)).
