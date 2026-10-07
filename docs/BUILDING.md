# Building and testing

- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Make targets](#make-targets)
- [Make variables](#make-variables)
- [Tests](#tests)
- [Benchmarking on the device](#benchmarking-on-the-device)
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
| `test-module M=<suite>` | One dsp suite on its own under ASan/UBSan, quicker to iterate on: `test/<suite>_test.cpp` with every `dsp/*.cpp` (`M=harmony`, `reverb`, `lifeosc`, `ground`, `bloom`, `engine`) |
| `test-module-arm M=<suite>` | The same for the Force's CPU, under `qemu-arm` |
| `demos` | Render every factory preset playing the demo phrase to `build/demos-out/*.wav` (stereo, as the plugin plays), and all of them back to back as `tour.wav`; prints each one's loudness |
| `preset-levels` | Set every factory preset's volume for `PRESET_LUFS` (default −16) on the demo phrase, never peaking over −1 dBFS |
| `bench` | x86 bench: only proves the bench and the profiling build work |
| `arm-plugin` | `build/arm/ambientforce.so`; profile-guided when `qemu-arm` is installed |
| `arm-bench` | `build/arm/afbench`, the CPU bench for the device |
| `arm-bench-stages` | `build/arm/ambientforce_stages.so`, the profiling build (never shipped) |
| `bench-device` | Run the CPU bench on a device (see [below](#benchmarking-on-the-device)) |
| `soak HOURS=<n>` | Render hours of playing offline and check it stays sane (see [below](#soak)) |
| `plugin-package` | `dist/AmbientForce-<version>-mpc-armv7.zip` with the installer |
| `plugin-install` | Package, copy to the device and install without asking (`install.sh -y`: stops and restarts MPC) |
| `clean` | Remove `build/` and `surface/build/` |

`make` on its own runs the tests and builds the device `.so` and the x86 profiling build.

The demo phrase (`tools/demos.cpp`, 120 BPM), which `preset-levels` matches the loudness on: two
held chords, then their release, the same for every preset (Task 10 sets its timing: a chord held
12 s, a second held 12 s, then 16 s of release, the loudness measured over all 40 s).

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
| `PRESET_LUFS` | The loudness `preset-levels` matches the factory presets to (default −16) |
| `M` | The suite for `test-module` and `test-module-arm` |
| `HOURS` | How long `soak` plays |

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
| `test/engine_test.cpp` | Listen routing in every mode; the keys, the pedal, Hold and a full harmony; Stop (Keep, Fade, Cut, a suspend and resume, a long suspend); the mix and Space's decay hold; tilt, volume, the limiter at −1 dBFS (Abyss at Decay 30 too) and its release; the guard; the clock near 2³² samples; idling; a CPU smoke test |
| `test/tables_test.cpp` | Names; the band limits of every frame at every mip level; equal RMS across a life; the life curves (Felt Piano darkening, Sine Bloom brightening); size; determinism; the builder thread's handoff, its cancel, and its release at unload or exit, with and without instances alive |
| `test/lifeosc_test.cpp` | Pitch; aliasing over the keyboard (Hermite and linear reads); the position glide and the mip level's crossfade; every read against a plain model of it; Sway's reflection, Smear; the Couple modes (FM against Bessel's partials, Ring, AM, Mix); `skip()`; the phase over a long run; odd and NaN input |
| `test/ground_test.cpp` | The just partials in every tuning; the linear read's images; Beat in Hz in two registers; Gravity; the root's octave and a Register change; the fade; gains and mute; no steps when the patch or the table changes; Body, Breath, Width, Tone; odd parameters and sequences; stability; the headroom sweep; determinism |
| `test/bloom_test.cpp` | A chord and its release; the strum to the sample; stealing; voice-led moves; the tail handoff (free at 1.5 s, the send's energy equal to Tail Voice's); Swell; velocity; unison; owners and notes pressed again; the filter, breath, width, mute; the send gate; stability; determinism; nothing allocating |
| `test/reverb_test.cpp` | EffectForce's Reverb suite (decay against the target, damping, density, freeze, shimmer, width, robustness, fingerprints); Haze and Abyss; no mode growing over a minute; Space's Rise and `silent()` |
| `test/plugin_test.cpp` | The VST2 basics, every getter at every index, playing, Stop and suspend through the plugin, `process()` against `processReplacing`, MIDI mapping, a stress run of floods and random patches |
| `test/params_test.cpp` | Every default and its text, the display formats, the popups, the patch map (Init plays what `Patch{}` plays), every option landing on its value, every sound value at both ends of its range while a chord sounds |
| `test/preset_test.cpp` | Saved state round trips and bad input, presets (init, save, step, the ends, missing files), user numbering, files appearing while running, the browser, favorites, stepping and the values pushed back (a Q-Link turn on the stepper: one preset per detent; a tile's release echo), every factory preset playing |

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
space, out). It reads no user folders, saves nothing, and fails if any case fails (p99 over 15% or
max over 50% of the block; up to 35% / 80% it warns). The cases (Task 11): idle (asleep); the Init
preset holding a triad; Ground alone with every partial, Body and Breath; Bloom alone, six voices
of unison 2 with FM; and the worst case, six-note chords re-struck every 2 s with unison 2 and FM,
Ground at full and Space in Abyss with shimmer. The results go in [Performance](PERFORMANCE.md).

## Soak

```sh
make soak HOURS=1
```

Renders hours of audio offline on x86 (-O2), as fast as the machine allows, through the plugin:
chords changing every 20–90 s (seeded random), with Freeze and Shimmer switching on and off. It
fails on a sample that isn't finite, a peak over −1 dBFS, a 10-minute window whose mean |DC| is
over −60 dBFS, or a 10-minute window's loudness more than ±6 LU from the first one's. M1 runs it for
an hour; the concept's 24-hour soak is M2's gate.

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

To see what MPC sends when a control is touched, turned or tapped, and when it suspends and resumes
the plugin, create the flag file while MPC runs (no restart):

```sh
ssh root@<ip> touch /tmp/ambientforce.trace
```

Within a second every AmbientForce instance appends to `/tmp/ambientforce.log`: one line per
`setParameter` (the time, the instance, the parameter, the value MPC sent, the value it had read back
before and the plugin's value and text after); one per suspend and resume with its time in ms; and
one where the audio thread takes the resume: how long after the suspend, whether the host said so
or the next block did, and whether it counted as Stop (On Stop) or a reset. What Stop does to an
instrument is a [Phase 0](ROADMAP.md#phase-0-the-probe) question. A table the
builder can't build, or a builder that can't start, is logged too. Remove the flag file to stop. The
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
