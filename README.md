# AmbientForce

**An ambient instrument that runs natively inside MPC on the Akai Force.**

AmbientForce is a VST2 instrument for MPC OS's built-in plugin host, with its own touchscreen pages
and Q-Link sets, built for slow music that keeps breathing after you stop playing. One gesture
drives several layers of sound, the **strata**, through a shared **harmony brain**: press one pad
and a drone moves under a chord, both in key and in just intonation, into a long reverb. It is
built on the same groundwork as its siblings [PolyForce](https://github.com/Devko/PolyForce),
[SubForce](https://github.com/Devko/SubForce) and [EffectForce](https://github.com/Devko/EffectForce).
What it is meant to become is in the [concept](docs/CONCEPT.md).

> [!NOTE]
> **Preview (0.0.1), milestone M1 "First light".** The harmony brain, two of the four strata
> (Ground and Bloom), Space, the pages and 28 factory presets are built and pass the full test suite
> on x86 and under ARM emulation. On a Force (MPC OS 3.9) it installs and benches, at 4–5% of a
> block for a held chord and 10.6% at its heaviest; playing it there is next. It is not in the plugin
> catalog, and nothing is released yet. Air, Weather, motion and scenes come in later milestones
> ([Roadmap](docs/ROADMAP.md)). The parameter list may still change before v0.1: sounds are saved by
> name and survive that, but recorded automation (stored by parameter index) could then move a
> different control.

![AmbientForce's PLAY page](docs/img/play.png)

*The PLAY page, rendered offline from the skin (on the device MPC fills in the values).*

## Highlights

- **One gesture, several strata.** Every key goes through the harmony brain first, and each stratum
  hears it in its own way (**Listen**: the notes themselves, the harmony's memory of them, or only
  the key). By default your hands play Bloom's chords and Ground holds the drone under them, and
  keeps holding it after you let go.
- **Four macros, and a screen that explains itself.** On PLAY, **Horizon** (near to far),
  **Motion** (still to moving), **Glow** (dark to bright) and **Density** (sparse to thick) each
  bend the loaded preset in one gesture, without moving its knobs; at 0 it plays as saved. Move any
  control and the status line says what it does for a few seconds; load a preset and it says what
  the preset is.
- **The harmony brain:** a key and one of 12 scales; Equal, **Just** (5-limit) or Pythagorean
  tuning; Input **As Played**, **Snap** (to the nearest scale tone) or **Degrees** (the white keys
  play the scale's degrees, so every key is in key); 10 chord types, always diatonic; Close, Open,
  Drop 2 and Spread voicings with voice leading; a strum; a **memory** that keeps the harmony for
  1 to 64 bars or forever; Hold.
- **Ground, the drone:** one voice of five partials (sub, root, fifth, octave and a colour
  interval) in exact ratios under Just, beating against each other at a rate set in **Hz**, the same
  in every register; it leans into a new root over up to 30 s (**Gravity**), and a vowel filter
  (**Body**) and a **Breath**, from a tide to a pulse, make it sing. The breath and both strata's
  sways run free (cycles that never line up again) or **on the bar**, locked to MPC's position.
- **Bloom, the chords:** six voices over **lifetime tables**, 256 frames of an instrument's note from
  the strike to the deep tail. **Age** picks the moment, **Sway** and **Smear** move through it. A
  second table couples to the first (Mix, FM, AM, Ring); unison 2; swells and releases of up to
  30 s; and the **tail handoff**, which gives a long release to the reverb so the voice is free
  again after 1.5 s.
- **Space:** an 8-line feedback delay network with Room, Hall, Plate, Space and two voicings of its
  own, **Haze** (veiled and distant) and **Abyss** (near-endless); Freeze, Shimmer and **Rise**,
  which lets the reverb bloom after the note instead of blurring it.
- **Every factory sound is computed.** Eight lifetime tables and four digital waves, built when the
  plugin loads: no sample data ships.
- **Built for MPC:** nine pages in four tabs, a 16-knob Q-Link set on each; a limiter that holds
  the output at −1 dBFS and a guard against non-finite samples, for nights of running; what Stop
  does is a setting (Keep, an 8-second Fade, or Cut).
- **28 factory presets**: Init and 27 in five categories (Drones, Beds, Blooms, Choirs and Contrasts,
  which each answer playing in their own way), each in its own key and scale, level-matched at
  −16 LUFS, each saying what it is when it loads; user presets, favorites, a browser.
- **Light enough on the CPU:** on the Force, Init holding a chord takes 4.2% of a block on average
  (p99 5.4%), and everything M1 has at its heaviest p99 10.6%, inside the 15% budget with room for
  the strata still to come ([Performance](docs/PERFORMANCE.md)).

## Documentation

| Document | What's in it |
|---|---|
| [User guide](docs/USER_GUIDE.md) | Playing it: the strata and Listen, the harmony page, Ground, Bloom, Space, the pages, presets |
| [Concept](docs/CONCEPT.md) | What AmbientForce is meant to become, and why |
| [Building](docs/BUILDING.md) | Toolchain, make targets, tests, device bench, packaging, release builds |
| [Architecture](docs/ARCHITECTURE.md) | Source layout, the signal path, threads and real-time rules, saved state |
| [Performance](docs/PERFORMANCE.md) | The CPU budget, what each case costs on the Force, where the time goes |
| [Roadmap](docs/ROADMAP.md) | What's done, what's next, decisions |
| [Changelog](CHANGELOG.md) | What changed in each release |

## Requirements

- An **Akai Force**. Other first-generation (32-bit ARM) MPC OS devices may work but are untested.
- **Root SSH access** to the device (for example through MockbaMod). Stock MPC OS has no way to
  install third-party plugins.
- **MPC OS 3.x** for the pages. Release builds are made against glibc 2.31 but need GCC 11's
  libstdc++, and MPC OS 2.x doesn't draw a plugin's pages: 2.x is untested. A local build with a
  newer cross toolchain needs glibc 2.38 (3.x only; see
  [Building](docs/BUILDING.md#release-builds)).

## Installation

There is no release yet. Once there is, download the package (`AmbientForce-<version>-mpc-armv7.zip`)
from [Releases](https://github.com/Devko/AmbientForce/releases), unzip it and follow the
`INSTALL.md` inside. In short:

```sh
scp -r AmbientForce-<version> root@<device-ip>:/tmp/
ssh -t root@<device-ip> sh /tmp/AmbientForce-<version>/install.sh
```

The installer asks for confirmation (`-y` skips it), **stops MPC** (save your project first),
copies the plugin to `/sdcard/Synths/Devko - VST - AmbientForce/`, backs up and edits
`MPC.settings`, and starts MPC again. Running it again upgrades in place and keeps your own presets
(`Presets/` in that folder) and favorites. Then add **AmbientForce** to a track from MPC's
instrument plugins.

From source, `make plugin-install` builds the package, copies it to the device named by `FORCE`
(in `local.mk`) and runs the installer without asking ([Building](docs/BUILDING.md)).

To uninstall, run the package's `uninstall.sh` the same way: it stops MPC, removes the plugin and its
`MPC.settings` entry (after a backup) and starts MPC again; your own presets and favorites stay in
the plugin folder (delete it to remove them too).

## The pages

Four tabs; tapping a tab again shows its next page, each with its own Q-Link set.

| Tab | Pages |
|---|---|
| PLAY | **PLAY**: the four macros, Freeze, Hold, Bloom's Age, the volume; the levels, Bloom's Swell, key, scale, chord and Gravity; the preset stepper. **HARMONY**: the harmony brain, its memory, On Stop, who listens to what |
| STRATA | **GROUND** and **DRONE**: the drone, its table, its partials, its breath and sway (free or on the bar). **BLOOM** and **BLOOM OSC**: the chord voices, the second table, Couple and unison, the sway |
| SPACE | **SPACE**: the reverb, its tail and the sends. **MIX**: levels, pans, sends, mutes, the return, tilt and the widths |
| BROWSE | **PRESETS**: the preset browser |

The [user guide](docs/USER_GUIDE.md#the-screen) shows them all.

## Factory presets

28 factory presets, each in its own key and scale, between them every Space type, the three tunings,
all twelve scales and all twelve keys. All are level-matched at −16 LUFS on a phrase of two held chords
and their release, peaks under −1 dBFS:

| Category | Presets | |
|---|---|---|
| Templates | Init | The defaults, level-matched: Felt Piano chords over a Cello Tasto drone, into a hall |
| Drones | Low Tide Hum, Harbour at 4am, Fifth Light | Ground forward, Bloom quiet and following the harmony |
| Beds | Lydian Morning, Slow Aurora, Felt Room, Night Ferry | Both strata, long swells |
| Blooms | First Snow, Glass Orchard, Tape Bloom, Sine Garden | Bloom forward, Ground low |
| Choirs | Lantern Choir, Distant Ah, Cathedral Breath, Choir in Haze | Choir Ah-Oo on both strata, Ground with Body |
| Contrasts | Felt Keys, Glass Tines, Ring Bells, Bare Strings, Old Tape, Sub Monolith, Pulse Drone, Overtone Choir, Cluster Fog, Tonic Halo, Afterglow, Frozen Sky | One idea each in how they answer playing: melodies, a bass, bells to let ring, a latch pad, a drone throbbing on the beat, a chord that plays itself, a bloom after you let go |

Each one in a line: [User guide](docs/USER_GUIDE.md#presets).

Your own presets (SAVE writes `User NNN.afp`) go to `Presets/User/` in the plugin folder; presets
in `/media/AkaiForce/AmbientForce Presets/` on the Force's drive show up too, a folder for each
category ([User guide](docs/USER_GUIDE.md#presets)).

## Listen

From source, on Linux or WSL: `make demos` renders every factory preset playing its demo phrase
through the plugin's own entry points into `build/demos-out/` (stereo WAVs, and all of them back to
back as `tour.wav`), the way MPC plays it, and prints what each one measures (loudness, how hard the
limiter works, brightness, motion, how wet and wide, how long the tail): no device needed.

## Building from source

On Linux or WSL (developed on Ubuntu 24.04):

```sh
make test            # the full test suite under ASan/UBSan
make arm-plugin      # build/arm/ambientforce.so for the device
make skin preview    # the skin, and every page as surface/build/page_*.png
make plugin-package  # dist/AmbientForce-<version>-mpc-armv7.zip
```

Release packages come from CI (glibc 2.31, profile-guided, checked with the plugin catalog's own
checker): see [Building](docs/BUILDING.md#release-builds).

## Status

| Stage | |
|---|---|
| M1 in code: the harmony brain, Ground, Bloom, the lifetime tables and oscillator, Space with Haze and Abyss, the engine, nine pages, tests | ✅ |
| M1: 16 factory presets, bench cases, a profile-guided build trained on them, the soak | ✅ |
| Playability (0.0.2): four macros on PLAY, every control's help on the status line, preset descriptions; 12 Contrasts presets; the breath and the sways free or on the bar | ✅ |
| On the device: installs; benches (`make bench-device`: worst case p99 10.6%, [Performance](docs/PERFORMANCE.md)) | ✅ |
| On the device: playing it, listening, and what MPC sends an instrument ([Roadmap](docs/ROADMAP.md#phase-0-the-probe)) | 🔜 |
| 0.0.1, the first release; then the plugin catalog | ⬜ |
| M2 Weather, M3 Long time, M4 v0.1 (parameter list frozen, append-only from then on) | ⬜ |

Details in the [roadmap](docs/ROADMAP.md); what changed in the [changelog](CHANGELOG.md).

## License

AmbientForce is released under the [MIT License](LICENSE). Third-party components keep their own
licenses (below).

## Credits

- Plugin groundwork (VST2 glue, touchscreen logic, preset library, build and bench tooling):
  [PolyForce](https://github.com/Devko/PolyForce), [SubForce](https://github.com/Devko/SubForce) and
  [EffectForce](https://github.com/Devko/EffectForce), MIT. The wavetable layout is PolyForce's,
  the reverb EffectForce's.
- DSP from the literature: Andrew Simper's linear trapezoidal state-variable filter (Cytomic);
  Jean-Marc Jot's decay gains for the reverb's feedback delay network; Jon Dattorro's input
  diffusion for the Plate.
- Skin generator, previews, installer and the catalog checker:
  [sd88me/mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) (MIT, Copyright (c) 2026
  sd88me), vendored in `third_party/mpc-vst-plugins` with a few small, marked patches.
- Interface font: [Titillium Web](https://fonts.google.com/specimen/Titillium+Web), SIL Open Font
  License 1.1 (`surface/fonts/OFL.txt`).

AmbientForce is an independent project, not affiliated with or endorsed by Akai Professional /
inMusic or Steinberg. Akai, Force and MPC are trademarks of inMusic Brands; VST is a trademark of
Steinberg Media Technologies GmbH.
