# Handover, 2026-10-24

This is for an agent continuing AmbientForce without the people and the machine it was built on.
Read it top to bottom before touching anything.

## What this is

AmbientForce is an ambient instrument: a hand-written VST2 plugin for the Akai Force (MPC OS,
ARMv7 Cortex-A17, 44.1 kHz, 128-frame blocks). It is the fourth of a family by the same author,
after PolyForce, SubForce and EffectForce, all at `github.com/Devko/<Name>`. One pad drives four
**strata** through a shared harmony brain. M1 has built two of them:
- Ground, a drone.
- Bloom, chords on "lifetime" wavetables.

These run into Space, a reverb with Haze and Abyss modes. M2 adds Air (generative melodic voices),
Weather (grains over procedural fields, your WAVs or Memory) and Echo. They are in the engine now,
on branch `m2`.

**Read these, in this order:**
1. [`docs/CONCEPT.md`](CONCEPT.md): the why, the whole design (M1 to M4), and the decisions in §17.
2. [`docs/ROADMAP.md`](ROADMAP.md): status, "What's next", and the **Decisions** log. Every
   decision made so far is recorded there with its reason; don't relitigate them.
3. [`docs/ARCHITECTURE.md`](ARCHITECTURE.md): the source layout, the signal path, threads and
   real-time rules, talking to MPC.
4. [`docs/BUILDING.md`](BUILDING.md): every make target.
5. [`docs/plans/2026-10-07-m2-weather.md`](plans/2026-10-07-m2-weather.md): the M2 plan.
   - Its "Ground rules for every task" apply to all work.
   - Each finished task has an **"As built"** block saying what was decided and measured.
   - The M1 plan, [`2026-10-06-m1-first-light.md`](plans/2026-10-06-m1-first-light.md), shows
     how M1 was done.
6. [`docs/plans/2026-10-08-m2-presets-proposal.md`](plans/2026-10-08-m2-presets-proposal.md):
   Task 12's 24 presets, **approved by the author**.
7. [`docs/USER_GUIDE.md`](USER_GUIDE.md) and [`docs/PERFORMANCE.md`](PERFORMANCE.md) as needed.
8. Devko/AmbientForce#2 (`m2` → `main`, merged on 2026-10-24): M2's status through Task 11, task
   by task. A new draft PR (`m2` → `main`) tracks Tasks 12 and 13.

## Where things stand

- **`main`** is released as **v0.0.2** (tag `v0.0.2`, 2026-10-08, the first release; CI publishes
  the package). It holds:
  - M1/M1.1;
  - the M2 plan and M2's Tasks 1–5 as standalone modules;
  - the fix for a Space ghost that 0.0.2 would otherwise have shipped with.

  **On 2026-10-24 the author had `m2` merged into `main`**: `main` now also has everything below
  through Task 11. It is unreleased; 0.0.3 comes after Task 13.
- **`m2`** (pushed) has M2's Tasks 1–12 merged. Task 13 goes on `m2` too.
  - **Task 12 (2026-10-24):** the 24 approved presets, 52 factory presets in all. Three macro changes
    the author decided: Horizon far darkens Air and Echo, sparse Density quiets Weather with its
    grains, and Motion toward +1 trims the volume 1.5 dB. The plan's Task 12 As-built block has every
    departure from the recipes and the measurements.
  - **The Q-Link fix** from the siblings (sd88me/mpc-vst-plugins #486–#489, 2026-10-24):
    - at most one `audioMasterUpdateDisplay` per 50 ms (`Surface::kTextGap`), with the CPU meter
      going through it;
    - `surface/params.lock`, which pins the parameters' order. Task 10 had moved the preset stepper
      and browser after M2's parameters, so the lock starts from M2's order (CHANGELOG, Unreleased).
  - Every task was reviewed twice, once for the spec and once adversarially, and every fix was
    re-reviewed.
  - **Air, Weather, Echo and Memory sound in the engine and have their pages.** Weather's Source
    picks a field, a WAV or Memory; Remember and Keep work (Task 10).
  - Init and the 28 factory presets still play bit for bit as 0.0.2; their fingerprints are pinned
    in the tests.
  - Task 10's fix round passed its re-review on 2026-10-09 with no must-fix. Its one should-fix, a
    Remember racing `updateMemory()`, is fixed in `d1ccf43`. The plan's As-built block has the rest.
- **The author's Force** (`local.mk`'s `FORCE`): its address changes; it was 192.168.1.113 on
  2026-10-24. It runs a PGO build of `m2` with Task 12 and the Q-Link fix, packaged as 0.0.3 (`.so`
  md5 `383d5bbd…`), for the author to play. v0.0.2's `.so` is kept on the author's machine to go back
  to.
- **Tests on `m2`** at `d0eda13` (Task 12, the Q-Link fix and the fields' DC fix merged), all passing:
  - `make test` 9447, the full macro sweep 9495 over 52 presets;
  - `make test-arm` 9125, `make arm-plugin` with PGO and its suite (`test-arm-pgo`) 9113;
  - `make soak HOURS=1`, seeds 1 and 2: no window's DC above −60 dBFS. Before the fields' fix,
    Abyssal Plain's Surf took seed 1's last window to −45 dBFS.
  - `make arm-icount`: every case as in PERFORMANCE.md's M2 table. M1's "worst" is 399.5k (was
    399.4k).
- **Budget** (`make arm-icount`, the counting qemu): M2's worst case is 528.7k ARM instructions a
  block, about **14.2% of p99** at 0.0268 points a thousand (device: pending). The gate is 15%, and
  the plan's caps begin at 14.5%.
  - Air is 35.9k against its 32k share, Echo 32.4k against 28k, Weather 60.9k against 60k.
  - M3 has about 0.8 points left to the gate.
- **Device bench** (M1.1, MPC OS 3.9, PGO build), p99 of the 2902 µs block: idle 0.13%, Init chord
  5.4%, drone 4.3%, Bloom 6×2 8.0%, worst case 10.6%. ARM instruction counts over-predicted the
  device by about 1.6× (~0.6 ns per instruction), so don't trust ~1 ns/instruction for budgets.

## Next, in order

1. **Task 13:**
   - the soak with the new strata on, walking all 52 presets (24 h is M2's gate; 1 h for "done");
   - the PGO trainer over the new presets;
   - the docs:
     - USER_GUIDE: the new pages, the 24 presets' rows, Memory and Keep, and that a project naming
       Memory reloads with nothing remembered;
     - ARCHITECTURE;
     - ROADMAP and its Decisions log, from the plan's As-built blocks;
     - CHANGELOG 0.0.3 (it takes the Unreleased section);
     - PERFORMANCE, with the measured budget table.
2. **The device** (the author): a build of `m2` with the 24 presets is on the Force. Play it, bench
   it (`make bench-device`), and answer the device questions below.

## Decisions the author took on 2026-10-08

All are recorded in the plan, in its As-built blocks and Decisions.
- **The Kalimba's burst:**
  - Long bursts are normalised by the expected RMS, the exact mean is taken at the strike, and the
    mean is weighted by the loop's DC pole.
  - Check 7 counts from the end of the excitation.
  - Air's 32k share is for ringing; strike blocks are documented spikes.
- **Air's Loop** follows Register and Range, and the loop seam keeps out the last two notes.
- **Memory keeps recording across Stop and suspend.** It is sealed at a sleep so the join doesn't
  click; only the guard and CC 120 restart it.
- **Air lets its voices go** when its level reaches 0.
- **Keep** never reuses a deleted file's number, and Memory is pinned only while Keep copies.
- **Names:** "Echo Repeats", "1 Bar" in every division list, and Weather's names shortened only
  where they don't fit.
- **Macros:**
  - Levels and sends are only multiplied, so 0 stays 0.
  - Air Density snaps to off under the knob's mark.
  - Horizon far darkens Air and Echo (done in Task 12).
- **Task 12:** the 24 presets in the proposal. The Memory presets name `memory:` and sound whole
  before any Remember.
- **Offline tools** (demos, preset levels, the trainer, the soak, the test host) wait for Weather's
  source to load, so renders are deterministic.

## Decisions the author took on 2026-10-24

- **`m2` merged into `main`** through Task 11, unreleased.
- **Sparse Density quiets Weather with its grains** (Cloud and Stretch: each remaining grain as loud
  as one grain of the preset's own cloud), rather than a floor of 3 grains or lower preset levels.
- **Motion toward +1 trims the volume, 1.5 dB at +1**, as thick Density trims 1 dB, rather than
  reworking two presets or a gentler Motion breath.
- **Glow keeps no trim:** Kalimba Loop's Air (60%) and Night Crickets' Weather (42%) stay below their
  recipes instead.
- **The help text stays on a control's first move**, though it asks MPC for one redraw. The author
  checks on the Force whether that costs a Q-Link its first step on track edit → automation.

## How the work is done (agreed with the author)

- **Models:** Sonnet implements well-specified tasks (ports, pages, macros, docs) and Opus the
  subtle DSP and engine work.
  - Reviews run in their own subagents: Sonnet for the spec review, Opus for the adversarial review
    (probes, fuzz, model checks).
  - The coordinating session judges the findings, asks the author the design questions (batched,
    once per task), makes small fixes itself and merges.
- **Parallel:** a task starts as soon as the interfaces it needs are fixed in the plan. It builds
  against the planned header with untracked stand-ins, which are never committed.
- **Lighter tiers:**
  - "Fails without it" is shown for must-fix items and behaviour changes only.
  - Agents run module suites and one `make test`.
  - The coordinator runs `test-arm`, `arm-plugin`, `arm-icount` and the soak once per merge batch.
  - Docs-only fix rounds aren't re-reviewed.
- **Lessons the reviews kept finding:**
  - A condition the engine skips on can begin at `set()` as well as at `process()`.
  - A bound computed from targets must also count the glide segment under way.
  - Test with blocks off the 32-sample grid (1, 33, 77, 100) and with MIDI events cutting pieces.
  - Anything a skipped module keeps must be safe to keep: no dangling source, no stale ring.

## Device questions for the author (Phase 0 and Task 10)

Trace with `touch /tmp/ambientforce.trace` ([diagnostics](BUILDING.md#diagnostics-on-the-device)).
- **Keep:** can the plugin write `/media/AkaiForce/AmbientForce/Memories/Memory NNN.wav`?
  - How long does the fsync'ed 2.8 MB write take, and does the audio stay clean during it?
  - Does a missing or read-only SSD give "KEEP: couldn't write to the SSD" without a hang?
- **Parameters:**
  - Which thread calls `setParameter`?
  - Do the Remember and Keep Q-Links send a press (a value over 0.5)?
  - At project load, does MPC call `setParameter` on plugin-owned parameters by index? That would
    step the Source stepper.
- **Q-Links on track edit → automation** (the 50 ms redraw gate): does a Q-Link keep its value while
  it turns, and does a control's first step survive the help text's one redraw?
- **Task 12's presets:**
  - a listen and a CPU check per preset;
  - Felt Echoes' Split at C5 against the pad rows;
  - Horizon by ear on Horizon Line, Snow Constellation and Kalimba Loop, with Cave Kalimba as the
    dark control.
- **The Source stepper:** one item per detent; "Surf ..." becoming "Surf"; load times in the trace
  (a field takes 202–368M ARM instructions to build).
- **ROADMAP's Phase 0 list:** what Stop sends to an instrument, CC 64 and the pad latch, MIDI
  channels, two instances at once.
- **Bench and release:** `make bench-device` on M2, and `tested.json` for v0.0.2 (from CI's
  package).

## Loose ends

- **Space's short-Decay network ghost (M1):** at Space Decay near 0.1 s with Haze's long lines,
  Freeze set after a silence can let out a faint ring. It is documented in `dsp/reverb.h` and not
  planned.
- **Small ideas, noted but not planned:**
  - a Decay stage for Bloom;
  - "1 Bar" is 4 quarter notes, because MPC's time signature is ignored;
  - `voiceLeadCost` carries a `TODO(Roland)`. It is the author's own contribution point; leave it.

## Rules

- **Branches:** work on **`m2`** and push it.
  - Keep a draft PR (`m2` → `main`) saying what's done, decided and open. Devko/AmbientForce#2
    was merged with Tasks 1–11; open a new one when Task 12 starts.
  - **Merge into `main` or tag only when the author asks.**
- **No device:** no `bench-device` or `plugin-install`. Never invent device numbers; mark them
  "(device: pending)".
- **Real-time rules:** nothing on the audio thread allocates, locks, throws or does file I/O.
  Host callbacks come only from `processReplacing`. 64-bit counters for anything that runs for
  hours. Getters read caches only.
- **Parameters:** 0.x, so the list may change, but **append** new parameters after the existing
  sound parameters (see `surface.py`) so saved projects keep their indices (`surface/params.lock`
  stops the build when a key moves; `GEN_VST_RELOCK=1` accepts a deliberate break). Every moved control
  needs a `help=` line (the build checks it). Names are ≤ 13 characters and unique; the build
  checks text fit deterministically.
- **Voice:**
  - Comments and docs follow the family's voice: plain sentences that say why.
  - No hardware product names, except in CONCEPT §2.
  - Commit messages: a subject line, then a body that explains what and why, ending with
    `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Done means:** `make test`, `make test-arm`, `make arm-plugin` ("exported symbols: 1"),
  `make skin preview` (look at the pages) and `make soak HOURS=1` all pass. Docs (USER_GUIDE,
  ROADMAP decisions, CHANGELOG 0.0.3 section, PERFORMANCE estimates) are true to the code.

## Setting up the build (Linux)

```bash
sudo apt-get install -y build-essential make git python3 python3-pil g++-arm-linux-gnueabihf qemu-user
printf 'PY = python3\n' > local.mk           # git-ignored; FORCE/SSH_KEY stay empty without a device
make test && make test-arm && make arm-plugin && make skin preview
```

- `ARM_RUN` defaults to `qemu-arm -L /usr/arm-linux-gnueabihf`.
- The PGO build runs its trainer under qemu, which takes about 1.5 min.
- A distribution's cross toolchain links against a newer glibc, which is fine for tests;
  catalog-ready release builds come only from CI's `arm32v7/gcc:11-bullseye`.
- Builds are incremental and parallel: one object per source and flag set, with `-j` on all
  cores, so a rebuild after one change takes seconds ([Building](BUILDING.md#how-builds-run)).
- `make arm-icount` needs a qemu built with TCG plugins
  ([Building](BUILDING.md#building-the-counting-qemu)).
  - On the author's machine it is `~/qemu-icount/` in WSL, set in `local.mk`.
  - Run WSL commands in a login shell (`wsl.exe -e bash -lc ...`), so `~/.local/bin`'s
    `qemu-arm` is on the PATH.
- The sibling repos are useful to read and to port from: clone `Devko/EffectForce`,
  `Devko/PolyForce` and `Devko/SubForce` next to this repo.
