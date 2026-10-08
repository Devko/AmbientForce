# Building and testing

- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Make targets](#make-targets)
- [Make variables](#make-variables)
- [Tests](#tests)
- [Benchmarking on the device](#benchmarking-on-the-device)
- [Instruction counts](#instruction-counts)
- [Soak](#soak)
- [Packaging and installing](#packaging-and-installing)
- [Release builds](#release-builds)
- [Diagnostics on the device](#diagnostics-on-the-device)
- [Binary compatibility](#binary-compatibility)

---

## Requirements

AmbientForce builds on Linux or WSL; it is developed on Ubuntu 24.04 under WSL.

| Tool | Needed for |
|---|---|
| `g++` 13 | tests, demos, the soak and the x86 bench |
| `arm-linux-gnueabihf-g++` 11 or newer | the device build (libstdc++ is linked dynamically; MPC OS ships it). Release builds come from [CI](#release-builds) |
| GNU make ≥ 4.3 | everything |
| `python3` | generating the parameter list, layout and C++ headers from `surface/surface.py` |
| `gcc` | the skin generator's C renderer |
| Python 3 with Pillow (`PY=`) | the skin, the page previews and the release package |
| `qemu-user` (`qemu-arm`) | `test-arm`, `test-arm-pgo` and the profile-guided device build (`qemu-user-static` works too; `ARM_RUN` says how ARM programs run) |
| A `qemu-arm` built with TCG plugins, and qemu's `insn` plugin | `arm-icount` only; distributions build qemu without plugins, so it is built from qemu's source ([below](#instruction-counts)) |
| `ssh`, `scp` | `bench-device`, `plugin-install` |

On Ubuntu 24.04, for example:

```sh
sudo apt install g++ g++-arm-linux-gnueabihf make python3 python3-pil qemu-user
```

`PY` must be a Python that has Pillow: the default `python3` works with Ubuntu's `python3-pil`; set it
for a virtual environment. From Windows, run everything in WSL:
`wsl -e bash -lc 'cd /mnt/d/DEV/AmbientForce && make test'`.

## Quick start

```sh
make test                      # the full suite under ASan/UBSan
make arm-plugin                # build/arm/ambientforce.so
make demos                     # every factory preset as a WAV, in build/demos-out/
make skin preview              # the skin, and every page as surface/build/page_*.png
make plugin-package            # dist/AmbientForce-<version>-mpc-armv7.zip
```

`surface/surface.py` is the single source of the parameter list and the touchscreen pages. Every
build regenerates `params.json`, `layout.conf`, `vst.json` and the C++ headers from it when it
changes; it checks the layout and every factory preset before writing anything. It needs only
`python3`, so the tests and the `.so` build anywhere; the skin needs Pillow.

## Make targets

| Target | What it does |
|---|---|
| `surface` | Regenerate parameters, layout and C++ headers from `surface/surface.py` (automatic) |
| `skin` | Build the skin (`TUI.json` + PNGs) with the vendored generator, then polish it with `surface/skin_polish.py` |
| `preview` | Render every page as `surface/build/page_*.png`, numbered from 0 in tab order (and `page_*_open.png` with the popups open) |
| `test` | The whole suite under ASan/UBSan: every dsp part on its own, then the plugin through its VST2 entry points |
| `test-arm` | The same suite built for the Force's CPU, run under `qemu-arm` |
| `test-arm-pgo` | The suite linked against the profile-guided objects the shipped `.so` is made of |
| `test-module M=<suite>` | One dsp suite on its own under ASan/UBSan, quicker to iterate on: `test/<suite>_test.cpp` with every `dsp/*.cpp` (`M=harmony`, `reverb`, `echo`, `lifeosc`, `ground`, `bloom`, `engine`) |
| `test-module-arm M=<suite>` | The same for the Force's CPU, under `qemu-arm` |
| `demos` | Render every factory preset playing the demo phrase to `build/demos-out/*.wav` (stereo, as the plugin plays), and all of them back to back as `tour.wav`; prints what each one measures ([below](#the-demo-phrase-and-the-presets-levels)) |
| `preset-levels` | Set every factory preset's volume so its demo phrase plays at `PRESET_LUFS` (default −16); the limiter holds the peaks under −1 dBFS, and a preset it takes more than 1 dB off fails, as one off its target does |
| `bench` | x86 bench: only proves the bench and the profiling build work |
| `arm-plugin` | `build/arm/ambientforce.so`; profile-guided when `qemu-arm` is installed |
| `arm-bench` | `build/arm/afbench`, the CPU bench for the device |
| `arm-bench-stages` | `build/arm/ambientforce_stages.so`, the profiling build (never shipped) |
| `arm-icount` | Each bench case's ARM instructions a block, exact and the same on every run, for measuring a change between device runs (see [below](#instruction-counts)); builds `build/arm-plain/ambientforce.so`, the device's flags without the profile |
| `bench-device` | Run the CPU bench on a device (see [below](#benchmarking-on-the-device)) |
| `soak HOURS=<n>` | Render hours of playing offline and check it stays sane (see [below](#soak)) |
| `plugin-package` | `dist/AmbientForce-<version>-mpc-armv7.zip` with the installer |
| `plugin-install` | Package, copy to the device and install without asking (`install.sh -y`: stops and restarts MPC) |
| `clean` | Remove `build/` and `surface/build/` |

`make` on its own runs the tests and builds the device `.so` and the x86 profiling build.

### The demo phrase and the presets' levels

The demo phrase (`tools/phrase.h`, shared by `demos`, `preset-levels` and `test/preset_test.cpp`)
is made for an ambient instrument: one chord held 12 s, a second chord held 12 s, then 16 s of
release, 40 s at 120 BPM with the transport playing. Every preset plays it **in its own key and
scale**, so it sounds as the preset is meant to and the levels still compare:

- the first chord on the tonic, between C3 and B3; the second on IV, a fourth above (in Lydian, whose
  IV is diminished, on II; in the other scales on the tone nearest a fourth above, the lower on a tie);
- one key per chord, the preset's Chord type building on it; with Chord Off three keys, an open
  triad (the root, the scale's tones nearest a fifth and a tenth over it); keys mapped back through
  the preset's Input, velocity 100.

The loudness is ITU-R BS.1770 / EBU R128 integrated loudness of the stereo pair over all 40 s
(`tools/loudness.h`). `preset-levels` sets each preset's volume for −16 LUFS on it without touching
the limiter: the limiter holds the peaks under −1 dBFS, and a preset that leans on it (more than
1 dB off) is one to fix, not to turn down: the match fails on it. Init, which is the engine's
`Patch{}`, lands at the default volume of −6 dB (the engine's make-up gain is set for that); the
others at −6 dB or under. `test/preset_test.cpp` plays every factory preset through the phrase (five
of them under qemu, where one takes 7.6 s) and holds it finite, within −16 ± 1 LUFS, under −1 dBFS,
with the limiter working on at most 1% of it.

`make demos` prints a map of each preset, for listening without a device: LUFS and peak; the peak
before the limiter and the most it took off (from a second render 20 dB down, the volume being the
only thing before the limiter); the spectral centroid of the holds and the release; motion (how far
the loudness and the timbre move within the holds); wet (the return against the dry, from a third
render with the return at 0); L/R correlation and side against mid; and the tail (how long until
the release is 20 and 40 dB down).

## Make variables

| Variable | Meaning |
|---|---|
| `FORCE` | The device's SSH address, `root@<ip>`; required by `bench-device` and `plugin-install` |
| `SSH_KEY` | Private key for the device's root login (default: ssh's own keys and config) |
| `PY` | Python 3 with Pillow, for `skin`, `preview` and `plugin-package` (default `python3`) |
| `PGO` | `auto` (default): profile-guided when ARM programs can run here (`qemu-arm`, or natively); `1`: always; `0`: plain build |
| `ARM_PREFIX` | The device toolchain's prefix (default `arm-linux-gnueabihf-`); empty for a native ARM build |
| `ARM_RUN` | How ARM programs run here (default `qemu-arm -L /usr/arm-linux-gnueabihf`); empty on ARM |
| `PLUGIN_VERSION` | Release version (default `0.0.1`): the zip's name, its `INSTALL.md` and the catalog manifest; CI sets it from the `vX.Y.Z` tag |
| `BENCH_ARGS` | `afbench` arguments for `bench-device` (default `-s 3`) |
| `ICOUNT_QEMU` | A `qemu-arm` built with TCG plugins, for `arm-icount`: a path, or a name on `PATH` (default `/home/user/qemu-icount/qemu-arm-plugins`, where the machine M2 was planned on keeps one; see [below](#instruction-counts)) |
| `ICOUNT_PLUGIN` | qemu's `insn` plugin built with it, a path (default `/home/user/qemu-icount/libinsn.so`) |
| `ICOUNT_SYSROOT` | The ARM libraries `arm-icount` runs its programs with, qemu's `-L` (default `/usr/arm-linux-gnueabihf`, the cross toolchain's) |
| `PRESET_LUFS` | The loudness `preset-levels` matches the factory presets to (default −16) |
| `M` | The suite for `test-module` and `test-module-arm` |
| `HOURS`, `SEED` | How long `soak` plays (default 1 hour), and its random sequence (default 1) |

Pass variables on the command line, or keep your own in `local.mk` next to the Makefile (git
ignores it):

```make
FORCE   = root@192.168.0.10
SSH_KEY = $(HOME)/.ssh/force
PY      = $(HOME)/.venvs/skin/bin/python
```

## Tests

`make test` measures every dsp part directly and drives the whole plugin through its VST2 entry
points against a fake MPC host (`test/host.h`), under AddressSanitizer and UndefinedBehaviorSanitizer
(any undefined behaviour fails the run). The host taps buttons and turns Q-Links the way a Force sends
them, and the tests give the surface a clock that moves a second per host event (a few ms within a
turn, `aft::Turn`), so stepping never depends on the machine's speed. The suites run in this order:

| File | Covers |
|---|---|
| `test/harmony_test.cpp` | The scales; Snap and Degrees (ties, black keys, two keys on one note); diatonic chords in every scale, parallel chords off the scale; the voicings and their range; voice leading (it keeps the voicing); the tunings (Just and Pythagorean ratios exact); the memory (Forever, bars at a tempo, Off, versions, held keys) |
| `test/engine_test.cpp` | Listen routing in every mode; the keys, the pedal, Hold and a full harmony; Stop (Keep, Fade, Cut, a suspend and resume, a long suspend); the mix and Space's decay hold; tilt, volume, the limiter at −1 dBFS (Abyss at Decay 30 too) and its release; the guard; the clock near 2³² samples; the synced cycles on the engine's one count, playing and stopped (a chord after a silence on its staggered places, the breath unmuted on the beat, both strata on one grid after 3 s stopped with Bloom silent); idling; a CPU smoke test |
| `test/airgen_test.cpp` | Air's generator (CONCEPT §13's `air` test): the same seed gives the same events, and no setting shifts the sequence (Density only moves it in time, a loop on and off leaves it as it was); Gravity 1 only chord tones in every pattern (Rise and Fall walking the chord tones), and a motif kept on a triad's four tones at Range 1 through Gravity's crossing and a chord change, and on two tones at Range 0.5 for every Motif, Gravity 0 their share; no repeat within two in every pattern, the Echo of figures with repeats, and the fallbacks when the weights leave too little (one chord tone: it every other note, never the note before; three candidates at least in every scale; a chord that leaves one allowed tone, and a motif drawn between Random's notes, still without repeats); Density's rate at 6, 12 and 60 a minute (one seed within ±10%, many within ±3%, counted in steps of 4096 once they are seen to give the events steps of 128 give), the gaps exponential, a change at once, 0 stopping it; the range at every Register; Rise and Fall a candidate or two at a time, wrapping; Constellation repeating, Mutate moving one note a scale step, the motif following a chord or key change and moving whole with Register; Echo learning the player's notes into the range, and keeping them through a Register change; the Loop free and synced (replays within the Rubato jitter, on the bar through a locate and a tempo change, the 120 s halving, overdubs, the 64-event recording, replays of the player's notes marked and moved into 24..108, a note at a pass's end coming back in order at Rubato 1, an empty first pass, Off forgetting, no repeat of the replays when it goes off, no room in a step, a key change and back; through Register, Range and chord changes keeping its pitch classes; where it comes round (its last note its first, or a b c a b), never the note before and, with candidates enough, neither of the two before; loops of one note and of two replaying as they are); Notes off; odd input; a Range or Gravity knob turning without working the candidates out every block |
| `test/tables_test.cpp` | Names; the band limits of every frame at every mip level; equal RMS across a life; the life curves (Felt Piano darkening, Sine Bloom brightening); size; determinism; the builder thread's handoff, its cancel, and its release at unload or exit, with and without instances alive |
| `test/lifeosc_test.cpp` | Pitch; aliasing over the keyboard (Hermite and linear reads); the position glide and the mip level's crossfade; every read against a plain model of it; Sway's reflection, a synced Sway pulled onto its clock (on the bar exactly, scans together or staggered by their offset, a locate gliding, a clock moved on only by the steps; back on Free each scan its own way again, landing on its own phase bit for bit), the division doubled past 4 Hz (1/4 at 300 BPM runs 2.5 Hz, at 239 BPM 3.98 Hz), the pulled cycle (most of a jump made up in 50 ms, landed exactly in about a second; `land()` at once; Free on its own phase every step), Smear; the Couple modes (FM against Bessel's partials, Ring, AM, Mix); `skip()`; the phase over a long run; odd and NaN input |
| `test/ground_test.cpp` | The just partials in every tuning; a synced Breath (1/4, Breath 100%) never stepping the gain more than 0.5 dB a control step across Free -> Sync, Play, a locate and a loop, and on the beat again after each; the worst jump (just under half a cycle, from 48 places in the cycle) at 120 and 300 BPM; the division doubled past 8 Hz (1/4 at 600 BPM runs 5 Hz, at 470 BPM 7.83 Hz); the linear read's images; Beat in Hz in two registers; Gravity; the root's octave and a Register change; the fade; gains and mute; no steps when the patch or the table changes; Body, Breath, Width, Tone; odd parameters and sequences; stability; the headroom sweep; determinism |
| `test/bloom_test.cpp` | A synced sway (every voice of a chord on the bar, staggered a sixth of a cycle apart, and staying there) and back to Free (off those places), and a chord after 1.3 s of silence on its places at its first step; a chord and its release; the strum to the sample; stealing; voice-led moves; the tail handoff (free at 1.5 s, the send's energy equal to Tail Voice's); Swell; velocity; unison; owners and notes pressed again; the filter, breath, width, mute; the send gate; stability; determinism; nothing allocating |
| `test/airvoices_test.cpp` | Air's voices: each sound's pitch at note 69 (1 cent) and under Just; the Kalimba's within 0.25 cents from note 24 to 108 at Decay 0.33 to 20 s and Tone 200 to 16000, and its loop's fundamental pole at every note from Decay 0.1 to 20 (0.43 cents at most); Glass's and Bar's inharmonic second modes, the decay against Decay at 2 s and 8 s, Tone (500 Hz against 16 kHz above 4 kHz), no click at a strike from note 24 to 108 and the modes' 1.5 ms rise, the level each sound is scaled to (−6 dBFS at note 72; the Kalimba's low notes, whose burst is built as it plays, against the build that made it whole), the burst built as it plays (the same whatever the render's pieces), no DC ringing in the pluck, velocity, equal-power pans and the send's glide, stealing (the quietest, a 2 ms fade, more strikes than voices within a fade), sleeping (and nothing written asleep), a note ringing as struck, Felt darkening, stability (six voices struck every 100 ms for 30 s at both ends of the keyboard; a minute's ring at Decay 20 still falling at its rate), odd input, determinism, nothing allocating |
| `test/reverb_test.cpp` | EffectForce's Reverb suite (decay against the target, damping, density, freeze, shimmer, width, robustness, fingerprints); Haze and Abyss; no mode growing over a minute; Space's Rise and `silent()`, and nothing from before a silence once the engine has skipped Space (a longer predelay, size, mode or shimmer set meanwhile, and a silence begun at `set()`) |
| `test/echo_test.cpp` | EffectForce's Delay suite (timing, spread, Ping-Pong, Mono, the feedback ratio and the cuts against theory, Tape and Fade, drive, the limiter, ducking, wow, robustness, block sizes, reset, the tail, Tape's hashes); Diffuse: 0 is EffectForce's Delay bit for bit (its own fingerprints, every mode), the smear of the third repeat, what a pass loses, the comb a long tail narrows to, no growth over 120 s at feedback 1, Mono with Diffuse, clicks, block sizes (short times too), odd values, empty diffusers after a reset or a rest at 0, the tail; nothing old after a reset, by two Delays whose lines differ only before it (a time gliding down with blocks off the 32-sample grid, a steady crossing of the reach, random settings); Echo: the Delay at mix 1 whatever the block, wet only, `silent()` within the Delay's reach, nothing old when it runs again after one (a skip begun at a `set()` too, a send in the very block), silent again at once when only a setting woke it, the duck's envelope kept |
| `test/plugin_test.cpp` | The VST2 basics, every getter at every index, playing, Stop and suspend through the plugin, the status line (a move's help line for 4 s, MPC told within 4 blocks; echoes show nothing, rounded to 1/1000 too, while MPC's smallest move, 0.001, shows its line; a control kept moving keeps it; automation changes it at most every 0.5 s; a preset's description for 6 s), `process()` against `processReplacing`, MIDI mapping, synced cycles (a Breath at 1 Bar tops on every downbeat at 120 BPM, on the bar again after a jump, on at the tempo when stopped; Free ignores the transport sample for sample), the trace (nothing written from a block; the resume and the MIDI written at the host's next call, a full ring's drops counted), a stress run of floods and random patches |
| `test/params_test.cpp` | Every default and its text, the display formats, the popups, the help lines, the patch map (Init plays what `Patch{}` plays), every option landing on its value, Free / Sync and the divisions (Breath Rate's default exactly 0.07 Hz; every factory preset that leaves them breathing and swaying as before, bit for bit), the macros (at 0 every factory preset's `Patch` bit for bit; swept from −1 to +1 each moves only its own fields, each one way and in range; Bloom Tone moved by Glow and Horizon on LP only, bit for bit still on BP and HP; under 0.5% a macro is 0, and its knob shows "0%" exactly there; a macro doesn't move the knobs), every sound value at both ends of its range while a chord sounds |
| `test/weather_test.cpp` | Weather's sources (the level at −20 dBFS RMS, the loop's seam at every level, levels 1 and 2 holding a tone, 21 kHz gone from level 1, the guard, determinism, odd lengths, signals under 8 frames, a DC's 3 dB swell across the equal-power fade); silence without a source, or with one missing its slower levels or of frames not a multiple of 4; pitch to the cent and nothing within 15 kHz above −70 dB two octaves up; To Key's pitch classes (Chord, Scale, the fold past +24); the level across grain counts; Stream's joins (a tone's envelope flat within 0.05 dB) and its level drifting or transposed; Stretch's head at 1/8 of real time, and its level; the gate's fade; Duck, and its envelope emptied while silent; a source change mid-cloud (the old source scribbled over just after: the same samples) and a mode change, without a step; `origin`; the window against the Hann over a whole grain; reverse reading a ramp backwards; backward grains born near a level's first frame (no read before it, under ASan); mute; a source freed while Weather is silent, in every mode (no grain left, none started rendered on, the level back over another source); the gate turned off while silent closing at once (also half way through its fade-out); calls of 128 against 32; determinism, 60 s at the extremes, odd parameters, nothing allocating |
| `test/fields_test.cpp` | The eight fields (each rendered under 1 s on x86, the same twice, 12 s, at −20 dBFS ± 0.5 dB RMS, no step at its loop's seam above its own 99.9th-percentile step at any level, L/R correlation under 0.9; Night's largest peak above 2 kHz within 20 cents of C8 and no cricket in the loop's crossfade; Surf's two swells 6 s apart, 6 dB over the troughs); Memory: `FrameDecimator` against `StereoDecimator` in runs of 1 to 13 pairs; 3 s of a sine remembered (3 s from frame 0, the sine 6 dB down in 16 bit within 0.1 dB at every level, each channel its own); a full ring (20 s of a ramp through every 16-bit value: 16 s from the oldest frame, exactly, the guard at every level); the seam a dip at every level, full and not yet full; Remember's rules (0.5 s and 2 s to the frame, pins counted, the generation, the old ring no longer a source, `fill()`); `reset()` keeping the remembered ring, and the recording after a reset or a Remember the same as a fresh Memory's; the same bits in pieces of 1, 33, 77, 100, 128 and at random; NaN and infinities; Weather's Stream playing a remembered ring across its end at the level recorded; pins from another thread while Remember is pressed every block; nothing allocating |
| `test/preset_test.cpp` | Saved state round trips and bad input, presets (init, save, step, the ends, missing files), user numbering, files appearing while running, the browser, favorites, stepping and the values pushed back (a Q-Link turn on the stepper: one preset per detent; a tile's release echo); every factory preset through the [demo phrase](#the-demo-phrase-and-the-presets-levels) (five under qemu): finite, −16 ± 1 LUFS, under −1 dBFS, the limiter on at most 1% of it; Init is `Patch{}`; preset descriptions (`about=`) kept out of the state; each macro at both ends on Init and the hottest preset of each Space type (x86): the limiter on at most 1%, at most 10 LU down; `AF_FULL_MACRO_SWEEP=1 build/plugin_test` sweeps every factory preset and the 16 corners of all four macros on the three hottest (minutes more: run by hand when the presets or the macros change) |

`make test-arm` runs the same suite cross-compiled for the Force's CPU under `qemu-arm` (no
sanitizers): it catches 32-bit and ARM-only paths (the FPSCR flush, NEON float code). `make
test-arm-pgo` defines `AF_PGO_OBJECTS`: the checks that pin exact output bits to one compilation
(the Reverb's fingerprints) skip there, since a profile moves the compiler's fusing of float
operations.

### Environment overrides

| Variable | Replaces |
|---|---|
| `AF_PRESET_ROOTS` | The preset roots (colon-separated list) |
| `AF_DATA_DIR` | Where favorites and recent lists are kept (empty: nothing is saved) |
| `AF_FIXED_SEED` | Set: every instance the same random numbers (the sways' phases, Smear, the breath noise, RND); the tests and demos set it |
| `AF_TRACE_DIR` | Where the [diagnostics](#diagnostics-on-the-device) flag and log are (default `/tmp`) |

## Benchmarking on the device

```sh
make bench-device FORCE=root@<ip>
```

Copies the plugin, its profiling build and the bench (`afbench`) to `/tmp` on the device, runs
pinned to core 1 while MPC keeps running (MPC's audio workers own cores 2–3), then deletes them.
The bench `dlopen()`s the `.so` like MPC and times every 128-frame block with the thread's CPU
clock; the profiling build also reports each block's time in the engine's stages (ground, bloom,
space, out). It reads no user folders, saves nothing (it looks for the [trace](#diagnostics-on-the-device)'s
flag file in a folder that doesn't exist, so a trace left on stays out of the cases), sets
`AF_FIXED_SEED`, and fails if any case fails (p99 over 15% or max over 50% of the block; up to
35% / 80% it warns).

Each case is a fresh instance set up through its parameters by index (real values, made MPC's 0..1
by `plugin/patch_map.cpp`, which the bench links), played 2 s untimed and then timed:

| Case | Load |
|---|---|
| idle | Asleep: no note yet |
| init chord | Init, one key held: its triad on Bloom, Ground on the root, the Hall |
| drone | Ground only (Bloom muted): every partial at full, Body and Breath |
| bloom 6x2 | Bloom only (Ground muted): Chord Off and six keys, unison 2, Couple FM |
| worst | Six keys let go for six others every 2 s (every voice taken from its release), unison 2, FM; Ground at full (every partial, Body, Breath); Sway at full depth and 2 Hz on both strata, Bloom's Smear and Breath at full (the read position crossing frames all the time: the dearest read); Space in Abyss with Shimmer 100%, Freeze off; Tilt on |

Before the cases, the bench opens an instance and waits until the table builder it starts has
published every table (it watches the process's threads in `/proc/self/task`), and says how long
that took: every case then reads real tables, not the sine fallback, and the builder isn't timed
with a case. No builder thread, or one still running after 300 s, fails the bench. The results are
in [Performance](PERFORMANCE.md#device-measurements).

## Instruction counts

```sh
make arm-icount
```

Prints each bench case's ARM instructions per 128-frame block, in thousands, exact and the same on
every run: the measure of a change between device runs. A task that adds DSP states its count from it;
the figures are in [Performance](PERFORMANCE.md#instruction-counts). It takes about 3 minutes.

- **The plugin** is `build/arm-plain/ambientforce.so`: the device's flags without the profile, which
  was trained on the code as it was before the change being measured. The shipped `.so` isn't touched.
- **`afbench <so> --icount <case> <blocks>`** (the bench, built by `arm-bench`) sets the case up as the
  bench does (a fresh instance, its parameters by index, 2 s played) and plays `<blocks>` more blocks,
  untimed; the timed bench plays a case the same way, through the same function. It runs under
  `ICOUNT_QEMU` with `ICOUNT_PLUGIN`, qemu's `insn` plugin, which counts instructions per vCPU index,
  modulo 8, and prints them when the program exits (`cpu 0 insns: N`). In user mode every thread is
  a vCPU, and a new one takes the index after the highest alive, so cpu 0 is the main thread's alone
  as long as no thread reaches index 8 (afbench has three alive at most).
- **Each case runs twice**, 256 and 768 blocks after its 2 s, and its line is the difference over 512:
  what comes before the blocks is the same in both runs and cancels.
- **Only the main thread is counted, and it only plays.** Some of what comes before the blocks costs
  what the machine makes it cost: the wait for the tables polls `/proc` with a sleep (as the bench
  does), and a parameter set looks for the trace's flag file once a second of wall time. So a helper
  thread loads the plugin, waits for the table builder to go and sets the case up, and the main thread
  waits for it in one call, however long that takes. No other thread may be alive while the blocks
  play, since its work would go uncounted: the helper looks before it goes and the main thread after
  the blocks, and either fails the run.
- **The same totals everywhere:** the environment and the arguments are copied onto the main
  thread's stack and move both runs' counts by hundreds of instructions. That cancels in the
  difference, but qemu runs with an empty environment (`env -i`) and paths relative to the
  repository anyway, so a run's total is the same on any machine too.
- **What is left** is the plugin's CPU meter, which tells the host when its figures change (it looks
  every 0.5 s of audio): a few dozen instructions in a run at most, under 0.1 a block, far under the
  0.1k shown. Its clock's seconds become a double through 32 bits for this: libgcc's conversion of a
  64-bit one returns early for 0, so every block cost less while the audio thread had run under a
  second, and how many blocks that was depended on the machine.

The counts belong to what made them:
- the qemu: another version, or another default CPU and with it other choices in glibc, counts
  other figures;
- the cross compiler (`ARM_CXX`) and the ARM libraries in `ICOUNT_SYSROOT` (glibc, libstdc++,
  libgcc), which the plugin calls into;
- the bench itself: its loop is in the main thread's count, and its stack frames decide where the
  plugin's fall, which can move paths that depend on alignment. Reworking `afbench`'s loop once
  moved the worst case by 40 instructions a block (0.01%), and idle by 5.

Compare counts made with the same four; `arm-icount` names the first three in its header. The
counts [Performance](PERFORMANCE.md#instruction-counts) had before `arm-icount` came from another
qemu build, and ran 7–14% over this one's for the same code.

### Building the counting qemu

Distributions build qemu without TCG plugins, so `arm-icount` needs one built from source. Without
one, `make arm-icount` stops before building anything and says where it looked. The defaults were
built from Ubuntu 24.04's qemu source package, `8.2.2+ds-0ubuntu1.18` (qemu 8.2.2 with Ubuntu's
patches), like this, with apt's `deb-src` lines enabled (Ubuntu 24.04: `Types: deb deb-src` in
`/etc/apt/sources.list.d/ubuntu.sources`):

```sh
sudo apt-get update
sudo apt-get install -y dpkg-dev build-essential ninja-build meson flex bison pkg-config libglib2.0-dev python3-venv
apt-get source qemu=1:8.2.2+ds-0ubuntu1.18
cd qemu-8.2.2+ds && mkdir build && cd build
../configure --target-list=arm-linux-user --enable-plugins --disable-system --disable-tools --disable-docs \
    --disable-werror --without-default-features --enable-tcg --disable-install-blobs
ninja qemu-arm tests/plugin/libinsn.so
mkdir -p /home/user/qemu-icount
cp qemu-arm /home/user/qemu-icount/qemu-arm-plugins && cp tests/plugin/libinsn.so /home/user/qemu-icount/
```

Elsewhere, set `ICOUNT_QEMU` and `ICOUNT_PLUGIN` (on the command line or in `local.mk`) to where they
are. Once the archive has moved past that revision, `apt-get source qemu` without the pin builds the
same way. Another qemu version works if its `insn` plugin prints a `cpu 0 insns:` line; its figures
are its own.

## Soak

```sh
make soak HOURS=1 SEED=1
```

Renders hours of audio offline on x86 (-O2), as fast as the machine allows, through the plugin's own
entry points, the way a long ambient set plays it. From one seeded random sequence: a new chord every
20–90 s (one to three keys, a quarter of the changes under the pedal); Freeze on for 10–60 s every
3–8 minutes; Shimmer every 2–6 minutes; the next factory preset (the browser's Next button) or
another Space mode every 8–16 minutes; the transport stopping and starting again, at a new tempo,
every 15–30 minutes; the host suspending and resuming, as a Stop or a reset, every 20–40 minutes. The
surface's clock runs with the audio and `AF_FIXED_SEED` is set, so a run plays the same samples every
time.

It fails on:

- a sample that isn't finite;
- any trip of the engine's non-finite guard (`af::guardTrips()`: the guard zeroes the block and
  resets the DSP, so the output looks clean, but a trip is a fault in the DSP);
- a peak over −1 dBFS;
- a 10-minute window whose mean |DC| (of 10 s means) is over −60 dBFS;
- a 10-minute window whose loudness is more than 6 LU from the first window's (BS.1770 integrated,
  gated, so a Stop's silence doesn't count: `tools/loudness.h`, a meter that clears per window and
  keeps its filters running).

It prints every window (loudness and its drift, peak, DC, **the share of time the limiter worked**,
from `af::limitedSamples()`, and what happened in it) and warns, without failing, when the limiter
worked more than 5% of a window: the presets are levelled well under the ceiling, so a limiter that
works that much means something is louder than it should be. Then the CPU time against the audio's
length (on x86: it says nothing about the device). `AF_SOAK_EVENTS=1` prints every event as it
happens. M1 runs it for an hour; the concept's 24-hour soak is M2's gate.

## Packaging and installing

```sh
make plugin-package
```

Builds `dist/AmbientForce-<version>-mpc-armv7.zip`: the plugin and its skin as one folder, the
installer and uninstaller, a generated `INSTALL.md` and checksums. Shipped scripts run under BusyBox
on the device, so the build refuses CRLF line endings in them.

```sh
make plugin-install FORCE=root@<ip>
```

Packages, copies the package to the device and runs its installer without asking (`-y`): it **stops
MPC** (save your project first), backs up and edits `MPC.settings`, and starts MPC again. A reinstall
keeps the user's presets and favorites/recent lists.

## Release builds

CI (`.github/workflows/build.yml`) builds, tests and checks the release package on every push and
pull request, the way the plugin catalog's own ports are built: the device build runs in
`arm32v7/gcc:11-bullseye` (GCC 11, glibc 2.31) under QEMU, profile-guided, with the test suite run
against the objects the `.so` is linked from; the sanitizer suite runs on x86. The zip is checked
with the catalog's own checker (`third_party/mpc-vst-plugins/tools/catalog_check.py --catalog`) and
kept as the run's artifact (`AmbientForce-mpc-armv7`).

Pushing a tag `vX.Y.Z` sets `PLUGIN_VERSION` from it and publishes the zip as a GitHub release,
with `CHANGELOG.md`'s `## X.Y.Z` section as its notes; a tag without that section fails before
anything is published. The plugin catalog lists a release with a download button and its installers
offer it. A tag with a suffix (`v0.1.0-beta`) publishes a prerelease instead: the catalog's beta
channel, which its site shows only when a visitor ticks "Show beta releases" and its installers never
offer; the plugin's version is then the tag without the suffix. To release: add the section, then
`git tag vX.Y.Z && git push origin vX.Y.Z`. The catalog finds new releases by itself (nightly), once
the plugin has an entry there.

The catalog reads the major version as the parameter list's compatibility (`param_compat` = X). 0.x
releases are previews: parameter indices may still change between them, under the same
`param_compat` 0. From v0.1 the list is append-only; should indices ever have to change after that,
bump X.

The same build outside CI, in an ARM environment: `make ARM_PREFIX= ARM_RUN= PGO=1 plugin-package`
(`ARM_PREFIX` empty: the native compiler; `ARM_RUN` empty: ARM programs run directly).

A local build with a newer distribution's cross compiler (Ubuntu 24.04: glibc 2.39, the C23
`__isoc23_sscanf`) needs glibc 2.38. That loads on the Force and other MPC OS 3.x devices, fine for
testing, but the catalog refuses it.

## Diagnostics on the device

To see what MPC sends (a control touched, turned or tapped, MIDI, a suspend and resume), create the
flag file while MPC runs (no restart):

```sh
ssh root@<ip> touch /tmp/ambientforce.trace
```

Within a second every AmbientForce instance appends to `/tmp/ambientforce.log`, each line with the
time and, for an instance's lines, the instance and the thread MPC called it on (`[tid 1234]`):

- one per `setParameter`: the parameter, the value MPC sent, the value it had read back before, and
  the plugin's value and text after;
- one per suspend and resume, with its time in ms, and one where the audio thread took the resume:
  how long after the suspend, whether the host said so or the next block did, and whether it counted
  as Stop (On Stop) or a reset;
- every MIDI event as it came in, channel and all (`midi ch 2 note-on 60 vel 100 @12`, @ its sample
  in the block). The audio thread only copies them into a ring of 256; MPC's next call into the
  plugin writes them, and a full ring's drops are counted;
- each table's build time and the total, from the builder thread, and a table or a builder that
  fails.

Nothing in `processReplacing` traces or allocates: the audio thread leaves notes, and the host's
threads write them at their next call (a parameter set, a display read, a suspend or a resume). These
lines answer the [Phase 0](ROADMAP.md#phase-0-the-probe) questions. Remove the flag file to stop. The
log stops growing at 2 MB; `/tmp` is cleared when the device restarts.

## Binary compatibility

- The `.so` exports only `VSTPluginMain` (a linker version script; the build counts every defined
  dynamic symbol and fails otherwise) and links with `--no-undefined`: an unresolved symbol would
  otherwise only show as MPC crashing on load. `-fno-gnu-unique` keeps it unloadable, which the
  table builder's teardown relies on ([Architecture](ARCHITECTURE.md#threads-and-real-time-rules)).
- The [release build](#release-builds) is linked against glibc 2.31; the device build prints the
  highest glibc version it needs, and `plugin-package` warns when it is over the catalog's 2.32.
  Built with a newer toolchain it needs that toolchain's glibc (Ubuntu 24.04: 2.38).
- libstdc++ is linked dynamically. GCC 11's (the release build's) needs `GLIBCXX_3.4.29` (the
  floating-point `from_chars` the saved state is parsed with): MPC OS 3.x ships GCC 13's, so it is
  there; whether MPC OS 2.x has it is unknown (2.x is untested, and doesn't draw the pages anyway).
  The catalog's checker reads only the glibc version.
