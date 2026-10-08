# Changelog

Releases are built by CI from a `vX.Y.Z` tag (see [Building](docs/BUILDING.md#release-builds)); the
section for the tag's version becomes the release's notes. While the version is 0.x the parameter list
may still change between releases.

## 0.0.2 (unreleased)

Playability: after hearing M1 on the device, "a ton of features, but nobody knows what all these
knobs are doing". So the screen now explains itself, four knobs play the whole instrument, twelve
new presets each answer playing in their own way, and the drone's breath and the sways can run on
the bar.

- **Four macros on PLAY** (and on PRESETS' Q-Links): **Horizon** (near and dry to far, wet and
  long), **Motion** (still to drifting), **Glow** (dark to bright) and **Density** (sparse to thick),
  −100% to +100%. Each bends several controls of the loaded preset at once without moving their
  knobs, every result inside its control's range; at 0 (under 0.5% either way, as the knob reads) the
  preset plays bit for bit as saved. They keep the level near the preset's: over all 28 factory
  presets with each macro at either end, the limiter never works on more than 0.05% of the demo
  phrase and no preset drops more than 8.0 LU (Frozen Sky, nearly all reverb, brought near); all
  four at once in their 16 corners on the three hottest presets, no limiting and at most 5.4 LU
  down (`AF_FULL_MACRO_SWEEP=1`). A Bloom Tone on a band-pass or high-pass is left where it is
  (it picks a band, not a brightness). Saved with the sound; loading a preset sets them to what it
  holds, as every other control (a preset is a complete sound): 0 unless it was saved with a macro
  moved. The factory presets keep them at 0. What each one moves:
  [User guide](docs/USER_GUIDE.md#the-macros).
- **The help line:** move any control and for 4 s the status line at the top of every page says
  what it does and what its range means (`BLOOM AGE: where in a note's life you listen, struck to
  fading`), then shows the meter again. A control kept moving keeps its line; another takes over no
  sooner than half a second after, so automation doesn't make it flicker (automation playing shows
  help lines too: it moves controls). MPC echoing a value back, even rounded to its 1/1000, and
  preset and project loads show nothing.
- **Preset descriptions:** a preset file may carry an `about=` line; when a preset loads (a tile,
  the stepper, NEXT, RND, INIT) the status line shows `NAME: description` for 6 s. All 28 factory
  presets have one. SAVE writes none.
- **12 Contrasts presets**, each one idea in how it answers playing, in all twelve keys (In-Sen,
  Whole Tone and Chromatic for the first time): Felt Keys (melodies, one felt-piano note a key),
  Glass Tines (a struck FM seventh on every pad), Ring Bells (whole-tone bells to let ring), Bare
  Strings (no reverb at all), Old Tape (a warbling cassette), Sub Monolith (a bass on your lowest
  key), Pulse Drone (a drone throbbing on every beat), Overtone Choir (an overtone whistling over
  fifths), Cluster Fog (a latch pad), Tonic Halo (a chord that plays itself), Afterglow (blooming
  after you let go) and Frozen Sky (notes that hang for minutes). 28 factory presets in all,
  every scale and key among them.
- **Free or on the bar:** Ground's Breath has a rate of its own (**Breath Rate**, 0.11 s to
  30.5 min, by default the 14 s it always had), and the Breath and both strata's sways can run
  **Sync**ed: one cycle per **Div** (1/4, 1/2, 1 to 64 bars of 4/4), locked to MPC's position while
  it plays, at the tempo while it is stopped, both strata on one count whether they sound or not; a
  synced breath tops on each division's downbeat, and Bloom's voices sway staggered on the bar, each
  a sixth of a cycle after the one before, so a chord still shimmers. A cycle never jumps while it
  is heard: MPC starting, locating or looping, and Free <-> Sync, glide, most of the way within
  about 50 ms and exactly onto the bar within about a second (at Breath 100% the level moves at
  most about 0.2 dB a control step); one starting from silence (a chord after a silence, the drone
  starting or unmuted) starts on its place. Back on Free each voice drifts back to its own phase. A
  division too fast for the tempo doubles (a sway at most 4 Hz, a breath 8 Hz). **Free** stays the
  default everywhere (phasing needs free cycles), and a preset that leaves them plays bit for bit as
  before. On the DRONE page (Breath and Sway cards) and BLOOM OSC (Sway). Motion bends the free
  rates, Breath Rate among them, and leaves synced ones on their bars. Pulse Drone breathes on every
  beat.
- **The PLAY page** has the macros, Freeze, Hold, Bloom Age and the volume on its first Q-Link bank;
  the levels, Bloom Swell, key, scale, chord and Gravity on the second. The two Tones, Space Decay
  and Shimmer, which Glow and Horizon now bend, left it (they are on their strata's pages).
- **Parameters:** eleven new sound parameters after Tilt (the four macros, then `g_breathrate`,
  `g_breathsync`, `g_breathdiv`, `g_swaysync`, `g_swaydiv`, `b_swaysync`, `b_swaydiv`), so every
  earlier sound parameter keeps its index; 151 in all. The plugin's own parameters after them (the
  preset stepper and its buttons, the browser's tiles, buttons and readouts) moved up by 14 (the
  eleven and the three Divs' popup flags): nothing a project keeps moved, the sound values being
  saved by name and automated by their unchanged indices.

## 0.0.1 (unreleased)

The first preview, milestone M1 "First light": one pad plays a chord over a drone that follows the
harmony, in just intonation, into a long reverb.

- **The harmony brain:** a key and 12 scales; Equal, Just (5-limit, relative to the key) or
  Pythagorean tuning; Input As Played, Snap (to the nearest scale tone) or Degrees (the white keys
  play the scale's degrees); 10 chord types, always diatonic to the scale (a parallel chord on a root
  outside it); Close, Open, Drop 2 and Spread voicings with voice leading; a strum of up to 2 s; a
  memory that keeps the harmony for 1 to 64 bars or forever after the keys are up; Hold, and the
  sustain pedal.
- **Listen:** each stratum follows the notes played, the harmony's memory, or only the key (Free),
  so one gesture plays both strata in different ways. Nothing sounds before the first note.
- **Ground, the drone:** one voice of five partials (sub, root, fifth, octave and a colour interval)
  in exact ratios under Just, beating against each other at a rate set in Hz, the same in every
  register; Gravity glides to a new root over up to 30 s, the nearest octave; a fade; Body, a vowel
  from a to o to u; Breath, a slow swell of level and brightness; width and pan.
- **Bloom, the chords:** six voices over lifetime tables, an instrument's note from the strike to the
  deep tail in 256 frames: Age, Sway and Smear move through it. A second table coupled to the first
  (Mix, FM, AM, Ring), unison 2, breath noise at the note, a low-, band- or high-pass filter, Swell
  and Release up to 30 s, and the tail handoff: a long release handed to the reverb, the voice free
  again after 1.5 s.
- **Twelve computed tables:** Felt Piano, Celesta, Glass Harmonica, Cello Tasto, Choir Ah-Oo, Reed
  Organ, Sine Bloom, Tape Strings, and Sine, Triangle, Saw and Square. No sample data ships.
- **Space:** an 8-line reverb with Room, Hall, Plate and Space, and two new voicings, Haze (veiled
  and distant) and Abyss (near-endless, four times the decay); Freeze; Shimmer at +12, +7, +19 or
  −12; Rise, which lets the reverb bloom after the note.
- **Output:** tilt, volume, a limiter that never passes −1 dBFS, and a guard against non-finite
  samples. On Stop: Keep, an 8-second Fade, or Cut.
- **Nine touchscreen pages** in four tabs (PLAY, STRATA, SPACE, BROWSE), a 16-knob Q-Link set each;
  user presets, favorites, a browser.
- **16 factory presets:** Init, and Drones (Low Tide Hum, Harbour at 4am, Fifth Light), Beds
  (Lydian Morning, Slow Aurora, Felt Room, Night Ferry), Blooms (First Snow, Glass Orchard, Tape
  Bloom, Sine Garden) and Choirs (Lantern Choir, Distant Ah, Cathedral Breath, Choir in Haze), each
  in its own key and scale, level-matched at −16 LUFS on a phrase of two held chords and their
  release, peaks under −1 dBFS.
- **On the Force** (MPC OS 3.9, measured on the device): Init holding a chord 4.2% of a block on
  average (p99 5.4%), the heaviest patch there is p99 10.6%, inside the 15% budget; the tables built
  in 1.6–1.7 s when the first instance loads.
- **Builds:** armhf against glibc 2.31, profile-guided (trained on every factory preset), the test
  suite run against the shipped objects, checked with the plugin catalog's `catalog_check.py`.
  Installed and benched on an Akai Force with MPC OS 3.9; MPC OS 2.x is untested (it doesn't draw a
  plugin's pages).
