# Handover, 2026-10-07

This is for an agent continuing AmbientForce without the people and the machine it was built on.
Read it top to bottom before touching anything.

## What this is

AmbientForce is an ambient instrument: a hand-written VST2 plugin for the Akai Force (MPC OS,
ARMv7 Cortex-A17, 44.1 kHz, 128-frame blocks). It is the fourth of a family by the same author,
after PolyForce, SubForce and EffectForce, all at `github.com/Devko/<Name>`. One pad drives four
**strata** through a shared harmony brain. M1 has built two of them:
- Ground, a drone.
- Bloom, chords on "lifetime" wavetables.

These run into Space, a reverb with Haze and Abyss modes. Air and Weather are M2.

**Read these, in this order:**
1. [`docs/CONCEPT.md`](CONCEPT.md): the why, the whole design (M1 to M4), and the decisions in §17.
2. [`docs/ROADMAP.md`](ROADMAP.md): status, "What's next", and the **Decisions** log. Every
   decision made so far is recorded there with its reason; don't relitigate them.
3. [`docs/ARCHITECTURE.md`](ARCHITECTURE.md): the source layout, the signal path, threads and
   real-time rules, talking to MPC.
4. [`docs/BUILDING.md`](BUILDING.md): every make target.
5. [`docs/plans/2026-10-06-m1-first-light.md`](plans/2026-10-06-m1-first-light.md): the M1 plan.
   Its "Ground rules for every task" apply to all work. Its review notes show how tasks were
   specified and checked.
6. [`docs/USER_GUIDE.md`](USER_GUIDE.md) and [`docs/PERFORMANCE.md`](PERFORMANCE.md) as needed.

## Where things stand

- **Branches and release** (updated 2026-10-08): `main` has M1/M1.1, the M2 plan and M2's Tasks
  1–5 (Echo, Air's voices and generator, Weather's grains) as standalone DSP modules, every review
  finding on them closed, **not yet wired into the engine**. It is released as **v0.0.2**, the
  first release (tag `v0.0.2`; CI publishes the package). M2 goes on from Task 6 on `m2`;
  Devko/AmbientForce#1 has the account of Tasks 1–5 and what Task 8 must decide.
- **Tests:** `make test` passes 3504 checks (x86, ASan/UBSan) and `make test-arm` passes 3350
  (qemu). `AF_FULL_MACRO_SWEEP=1 build/plugin_test` passes 3528. A 1 h soak passes.
- **Device:** v0.0.2 is installed on the author's Force (192.168.1.116, 2026-10-08): a local
  profile-guided build of the tag (Ubuntu 24.04's cross compiler, so it needs glibc 2.38, which MPC
  OS 3.x has). CI's release package (GCC 11, glibc 2.31) is the one for `tested.json`.
- **Device bench** (MPC OS 3.9, PGO build), p99 of the 2902 µs block:
  - idle 0.13%
  - Init chord 5.4%
  - drone 4.3%
  - Bloom 6×2 8.0%
  - worst case 10.6%

  The gate is 15%. ARM instruction-count estimates over-predicted the device by about 1.6×
  (~0.6 ns per instruction measured), so don't trust ~1 ns/instruction for budgets. Space is the
  largest fixed cost, about 80 µs per block (2.7%).
- **Done in M1/M1.1:**
  - the harmony brain, lifetime tables and oscillator, Ground, Bloom, Space and the engine;
  - 151 parameters on 9 pages;
  - 28 factory presets in 6 categories, all at −16 LUFS without the limiter;
  - the help line on the status readout, preset descriptions, the four macros (Horizon, Motion,
    Glow, Density), and Breath/Sway free or synced to the bar (phase-pulled, Bloom staggered);
  - the bench, PGO trainer, soak and CI.

## Open items

1. **M2 from Task 6** (fields and Memory), in the plan's order. The re-review of `8c94203` is
   done (merged in `d1296fb`). A follow-up from M1, not planned yet: at Space Decay near 0.1 s
   with Haze's long lines, Freeze set after a silence can let out a faint ring (documented in
   `dsp/reverb.h`).
2. **Device-only, leave for the author** (no device in the cloud):
   - `make plugin-install FORCE=root@<ip>`; the Force's DHCP address changes, and was
     192.168.1.116 last.
   - `make bench-device`, the listening session, and the Phase 0 probe (ROADMAP: trace with
     `touch /tmp/ambientforce.trace`; it logs MIDI with channels, suspend/resume, table build times
     and thread ids).
   - `tested.json` for v0.0.2, once it has been played.
3. **Small ideas noted, not planned:**
   - A Decay stage for Bloom (Felt Keys sustains rather than decays like a piano).
   - "1 Bar" is 4 quarter notes (MPC's time signature is ignored).
   - `voiceLeadCost` carries a `TODO(Roland)`: the author's own contribution point. Leave it.

## Suggested work for tonight: M2 "Weather"

CONCEPT §14's M2 row covers:
- **Air** (§5.3): a modal resonator bank (Glass, Bowl, Bar, Bell), Kalimba (Karplus-Strong) and
  Felt; generative patterns (Random, Rise, Fall, Constellation with Mutate, Echo); Listen; Loop;
  Rubato.
- **Weather** (§5.4): grains over procedural fields; the user's WAVs from the SSD via a loader
  thread; Memory (a 16 s ring of the output) with Remember and Keep (writes a WAV to the SSD).
- **Echo** (§8).
- Their pages and presets.

How to go about it:
1. **Write the plan first:** `docs/plans/<date>-m2-weather.md`, in the M1 plan's format: tasks
   with exact interfaces, parameters, pages and checks. Write the budget with device numbers in
   mind: about 4.4 points of the 15% gate are left in the worst case. M2's worst case should still
   pass, so plan caps.
2. **Reuse siblings:**
   - EffectForce's `dsp/grain.*` (Cloud, Stretch) and `dsp/delay.*` (tape wow, ducking, diffusion
     to add).
   - PolyForce's `plugin/loader.*` for WAV import.
   - Put a provenance line on everything copied.
3. **Process (this is how M1 was built, and it works):**
   - Each task goes to a fresh subagent with the full task text and context.
   - Then a **spec-compliance review**, then a **code-quality review**. Both must pass, and fixes
     are re-reviewed.
   - Independent tasks run in parallel in git worktrees and are merged into the work branch.
   - The reviews found real bugs in nearly every M1 task, so don't skip them.
4. **Keep the factory presets' level contract:** −16 LUFS, the limiter idle. Also keep Patch{}
   equal to Init, and the full macro sweep green.

## Rules

- **Branches:** work on a new branch **`m2`** from `main`, and push `m2`. **Do not push to
  `main`.** Open a **draft PR `m2` → `main`** and summarise the night's work in it for the author
  to review.
- **No device:** no `bench-device` or `plugin-install`. Never invent device numbers; mark them
  "(device: pending)".
- **Real-time rules:** nothing on the audio thread allocates, locks, throws or does file I/O.
  Host callbacks come only from `processReplacing`. 64-bit counters for anything that runs for
  hours. Getters read caches only.
- **Parameters:** 0.x, so the list may change, but **append** new parameters after the existing
  sound parameters (see `surface.py`) so saved projects keep their indices. Every moved control
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
- The sibling repos are useful to read and to port from: clone `Devko/EffectForce`,
  `Devko/PolyForce` and `Devko/SubForce` next to this repo.
