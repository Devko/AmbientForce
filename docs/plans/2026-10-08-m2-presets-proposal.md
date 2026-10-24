# M2 Task 12: the presets, a proposal

*2026-10-08, for the author before Task 12 builds anything.* What it answers: the brief under
[Task 12](2026-10-07-m2-weather.md#task-12-presets) (more presets than the plan's eight, four
directions, the list and the count first). Nothing here is built.

**Approved by the author, 2026-10-08:** all 24, as listed; Horizon far darkens Air's Tone and
Echo's High Cut by half an octave, as it does Ground's and Bloom's Tones (added before the presets
are level-matched, then judged by ear on Horizon Line, Snow Constellation and Kalimba Loop, Cave
Kalimba the dark control); the Memory presets as proposed (the four in 09_Memory name `memory:` and
sound whole before any Remember; Remembered Tide starts on Surf). The other open questions below
are taken as recommended.

## 1. The count

**Task 12 builds 24 presets: the plan's eight and 16 more.** That makes 52 factory presets, up from 28.

| | Slow evolution | Played with the MPC | Dark and cinematic | Light and luminous |
|---|---|---|---|---|
| **07_Glass** (8) | Bowl Garden, Hour Glass | Kalimba Loop†, Felt Echoes† | Bell Harbour†, Cave Kalimba | Snow Constellation†, Wind Chimes |
| **08_Weather** (8) | Wind over Fifths†, Night Crickets† | Rain on the Roof†, Remembered Tide† | Gale Warning, Last Embers | Bright Water, Sun Shower |
| **09_Memory** (4, new) | Slow Recollection | Palimpsest | Undertow | Afterimage |
| **Drones, Beds, Blooms, Choirs** (+1 each) | Unequal Loops (Beds) | Horizon Line (Blooms) | Abyssal Plain (Drones) | Choir of Light (Choirs) |

† The plan's eight. They stay, with their recipes refined. Each also takes a direction.

- **Why 16 more.** Glass and Weather each reach CONCEPT's eight. Memory, the most distinctive idea in
  M2, gets the category CONCEPT gives it. Each M1 category, now holding three or four of its eight,
  gets one preset that shows the new strata in its own style. Every direction gets six presets.
  Between them the 24 use every Air sound and pattern, every field, every Weather mode and Listen,
  both Memory taps, every Echo mode, every Space type and all three tunings. Each key appears twice.
  The 24 key-and-scale pairs are new to the factory set and cover 11 scales (Chromatic stays Felt
  Keys' alone).
- **Why not more.** Past about 24, the combinations that are really different run out (6 sounds × 5
  patterns, 8 fields × 3 modes, 2 taps). A further preset would differ from an existing one mainly
  by its key. Autopilot, CONCEPT's eighth category, waits for M3.
- **Why not fewer.** With 8 more instead of 16, some direction ends up with four or fewer presets,
  and Memory gets no category. If device time is short, build rows 1–16 now (Glass and Weather) and
  leave rows 17–24 for later.
- **Cost of the count.** `make preset-levels` does the matching. On the device each preset needs a
  listen and a CPU check, a few minutes each: about 1.5–2 h for 24. The full macro sweep grows from
  36 presets × 8 ends to 52 × 8. Task 13's soak walks 52 presets (its text says 36), and
  USER_GUIDE's preset table gains 24 rows.

## 2. The presets

How to read a recipe:
- Levels and sends are knob values, so 70% is −6 dB and 50% is −12 dB. "→Echo" and "→Space" are a
  stratum's sends.
- Whatever isn't named stays at Init's value. So: Air, Weather and the Echo sends off, Memory
  Forever, Just tuning, Weather on Listen Free, Bloom on Notes, Ground on Harmony.
- The macros stay at 0, which the build enforces. "Macros" says what turning them does.
- Each about= line says its direction in words: *dark*; *bright* or *luminous*; *slow* or *for
  hours*; or a move (*play*, *press*, *ride*, *Remember*). All 24 were measured with
  `surface.py`'s own `status_misfit()`. They come out at 563.9–663.8 px, against a 684 px status
  line that needs 3 px spare.

| # | Category | Preset | Direction | Key, scale | Recipe | about= |
|---|---|---|---|---|---|---|
| 1 | 07_Glass | Snow Constellation† | Light | G Lydian | **Air 60%** Glass, Constellation, 10/min, Gravity 0.7, Reg High, Range 2, Motif 6, Mutate 0.25, Decay 7 s, Tone 10 kHz, Width 0.9, →Space 0.7, →Echo 0.2. **Bloom 50%** on Harmony, Celesta Age 0.85, Smear 0.2, Add9 Spread, Swell 3 s, Release 12 s. **Ground 25%** Glass Harmonica, Reg High, Root and Octave. **Echo** Stereo, Sync 1/4., Repeats 0.35, High Cut 7 kHz, Diffuse 0.6. **Space** Space, Decay 12 s, Damp 10 kHz, Shimmer 0.2 (+12). First Snow also has Celesta at 0.85; the difference is that here Air leads and Bloom follows the harmony behind it. Macros: Glow (strike, shimmer), Density (none to 20/min). | luminous glass stars over celesta |
| 2 | 07_Glass | Bell Harbour† | Dark | A Phrygian | **Air 65%** Bell, Random, 4/min, Reg Low (the hum, an octave under, rings longest), Range 1.5, Gravity 0.9, Decay 14 s, Tone 2.5 kHz, →Echo 0.15. **Ground 85%** Free (on the tonic), Reed Organ, Reg Low, Body 0.6 ("o"), Sub 0.4, Fifth 0.35, Tone 700 Hz, Breath 0.5 over 40 s: a foghorn. **Bloom 30%** on Harmony, Tape Strings Age 0.8, Tone 1.2 kHz, Swell 8 s. **Weather 30%** Surf, Stream, HP 120 Hz, Tilt −0.4. **Echo** Mono, Free 1100 ms, Repeats 0.5, High Cut 1.8 kHz, Diffuse 0.7. **Space** Haze, Decay 14 s, Pre-Delay 80 ms. Unlike Harbour at 4am (a cello drone with Body), the bells and the water lead and the drone is a reed. Macros: Horizon far sinks the bells into the fog. | low bells toll through a dark harbour fog |
| 3 | 07_Glass | Kalimba Loop† | Played | E Min Pent | **Air 70%** Kalimba, Random, 30/min, **Loop On, Sync, 4 Bars** (about 4 notes a pass at 120 BPM), Rubato 0.3, Reg Mid, Range 1.5, Gravity 0.8, Decay 2.5 s, Tone 4 kHz, →Echo 0.45. **Bloom 45%** Sine Bloom Age 0.4, Sus2, Swell 1.5 s, Release 4 s. **Ground 40%** Root and Fifth, Breath 0.4 **Sync 1 Bar**. **Echo** Ping-Pong, **Sync 1/8.**, Repeats 0.4, Low Cut 300 Hz, Duck 0.4, Diffuse 0.1. **Space** Plate, Decay 4 s. Moves: each pad re-chords the loop (its notes move to the new chord); turning Air Loop off and on records a new loop. Macros: Motion loosens it; Density shapes only the first pass, since a loop that is replaying generates nothing. | a kalimba loop on the bar; pads re-chord it |
| 4 | 07_Glass | Felt Echoes† | Played | A Minor | **Split C5 (72)**: keys from C5 up play Air, the keys below play chords. **Air 65%** Felt, **Echo** pattern, Harmony, 10/min, Motif 5 (the motif is your last five notes over Split; until you give three it plays as a Constellation), Mutate 0.2, Reg Mid, Range 2, Decay 5 s, Vel 0.6, Rubato 0.3, →Echo 0.5. **Bloom 45%** Sine Bloom Age 0.4, Seventh, Drop 2, Swell 4 s, Release 6 s. **Ground 35%**. **Echo** Stereo, Sync 1/4., Repeats 0.55, High Cut 3.5 kHz, Wow 0.25, Diffuse 0.75, Duck 0.4. **Space** Hall, Decay 6 s. The plan's recipe needs Split: the Echo pattern learns only notes given to Air, and on Notes Air generates nothing. Check C5 against the Force's pad rows on the device. Macros: Motion varies the answers (Mutate, Rubato). | play above C5: Air answers with your notes |
| 5 | 07_Glass | Wind Chimes | Light | G# Maj Pent | **Air 60%** Bar, Random, 24/min, **Free** (it plays from the scale on its own, so the chimes don't follow the chords), Gravity 0.3, Reg High, Range 1.5, Decay 6 s, Tone 12 kHz, Rubato 0.6, Width 1. **Weather 40%** Wind High, Cloud, Grains 6, Size 0.6 s, Spray 0.5, Drift 0.5, HP 200 Hz, Tilt +0.2, Width 1. **Bloom 40%** on Harmony, Glass Harmonica Age 0.5, Sus2 Open, Swell 6 s, Release 5 s. Ground muted, Echo off. **Space** Plate, Decay 5 s, Damp 12 kHz. Macros: Density (none to 48 chimes a minute, 1–12 grains), Motion (the breeze wanders). | bright bar chimes swing in a high breeze |
| 6 | 07_Glass | Cave Kalimba | Dark | F In-Sen | **Air 65%** Kalimba, **Fall**, 8/min, Reg Low, Range 2, Gravity 0.6, Decay 8 s, Tone 900 Hz, Rubato 0.4, →Echo 0.5. **Ground 70%** Cello Tasto, Reg Low, Sub 0.5, Fifth 0, Color min7 0.3, Beat 0.8 Hz, Tone 500 Hz. **Bloom 35%** on Harmony, Tape Strings Age 0.7, Tone 1 kHz, Sus4, Swell 12 s, Release 15 s. **Echo** Mono, Free 900 ms, Repeats 0.65, High Cut 1.5 kHz, Wow 0.35, Diffuse 0.85, Echo Space 0.5: drips. **Space** Abyss, Decay 12 s, Damp 3 kHz. Weather off. Macros: Glow dark dulls the plucks; Horizon far deepens the cave. | dark plucks fall into a dripping cave |
| 7 | 07_Glass | Bowl Garden | Slow | A# Hirajoshi | **Air 60%** Bowl, Random, 4/min, Reg Low, Range 2, Gravity 0.8, Decay 18 s, Tone 3 kHz, Rubato 0.5, Width 0.8 (each bowl's close pair of modes beats at 1–2 Hz). **Ground 55%** Glass Harmonica, Root, Fifth and Octave, Beat 0.15 Hz, Breath 0.4 over 5 min, Sway 0.4 at 3 min. **Bloom 40%** on Harmony, Sine Bloom Age 0.5, Sway 0.8 at 8 min, Sus2, Swell 15 s, Release 16 s. **Space** Space, Decay 16 s, Mod 0.6. Weather and Echo off. One pad, then hands off: free cycles of 3, 5 and 8 minutes under bowls that never repeat. Macros: Motion (how fast the cycles turn), Density (none to 8 bowls a minute). | bowls ring and beat, changing for hours |
| 8 | 07_Glass | Hour Glass | Slow | F# Mixolydian, Pythagorean | **Air 55%** Glass, Constellation, 6/min, Motif 8, **Mutate 0.6** (a pass lasts about 80 s and most passes move one note, so after an hour it plays another melody), Reg Mid, Range 3, Gravity 0.5, Decay 9 s, Tone 7 kHz, →Echo 0.15. **Bloom 50%** **Free** (the tonic chord plays itself), Glass Harmonica Age 0.3, Sway 0.6 at 6 min, Add9 Spread, Swell 20 s. **Ground 45%** Sine Bloom, Sway 0.5 at 8 min (the slowest Rate), Breath 0.3 over 4 min. **Echo** Stereo, Free 1700 ms, Repeats 0.45, High Cut 4 kHz, Diffuse 0.9. **Space** Haze, Decay 9 s. Macros: Motion (Mutate up to 0.8, so it changes faster), Density. | a glass figure that slowly becomes another |
| 9 | 08_Weather | Rain on the Roof† | Played | D Mixolydian | **Weather 60%** Rain on Roof, Cloud, **Listen Harmony** (so To Key Chord takes each chord's notes; on Free it would keep the tonic's), To Key Chord, Grains 8, Size 0.3 s, Spray 0.4, Drift 0.3, **Duck 0.4** (the rain steps back under the chords and returns in the gaps). **Bloom 55%** Felt Piano Age 0.6, Add9 Open, Strum 0.6 s, Swell 3 s, Release 5 s. **Ground 40%**. Air and Echo off. **Space** Plate, Decay 5 s. Macros: Density (1–16 grains, from single drops to a downpour). | the rain tunes to every chord you play |
| 10 | 08_Weather | Wind over Fifths† | Slow | D Dorian | **Ground 85%** Sine Bloom Age 0.6, Reg Low, Sub 0.3, Fifth 0.9, Octave 0.5, Beat 0.12 Hz (in Just the fifths are all but still), Breath 0.4 over 2 min, Sway 0.3 at 6 min. Chord Fifths, which gives Air its chord tones. **Weather 55%** Wind Low, **Stream**, Drift 0.4, Tilt −0.2, Width 0.9. **Air 45%** Bowl, Random, 2/min, Reg Mid, Range 1, Gravity 0.9, Decay 16 s, Tone 2.5 kHz. Bloom muted, Echo off. **Space** Space, Decay 10 s. Macros: Motion (the gusts wander, the drone sways), Horizon. | still fifths under the wind, for hours |
| 11 | 08_Weather | Night Crickets† | Slow | C Lydian | **Weather 50%** Night, **Stretch**, Grains 6, Size 0.25 s, Spray 0.2, Drift 0.2, To Key Off (its crickets, on C8 and G7, are already in C Lydian), HP 300 Hz, Width 1. **Air 45%** Glass, Random, 3/min, Reg High, Range 1.5, Decay 10 s, Tone 5 kHz. **Ground 55%** Cello Tasto, Reg Low, Root and Fifth, Tone 600 Hz, Breath 0.4 over 90 s. **Bloom 35%** on Harmony, Sine Bloom Age 0.7, Tone 1.5 kHz, Sus2, Swell 10 s. Echo off. **Space** Hall, Decay 7 s. Macros: Motion, Density (1–12 grains). | a slow summer night, crickets and glass |
| 12 | 08_Weather | Remembered Tide† | Played | D# Mixolydian | **Weather 50%** source **Surf** (it sounds from the first note), **Stream** (plays the source as it is), Memory From Output, Drift 0.2, Duck 0.3, HP 60 Hz. **Remember** makes the last 16 s the source: the sea turns into your own playing, coming back as a 16 s tide, and each Remember folds the last tide in. **Bloom 60%** Felt Piano Age 0.5, Seventh Drop 2, Strum 0.4 s, Swell 2 s, Release 8 s. **Ground 45%** Root and Fifth. Air and Echo off. **Space** Hall, Decay 8 s. The plan's about= line ("press Remember, the tide plays you back") measures 690.6 px, which the build would refuse. This one keeps its words. | Remember: the tide plays you back |
| 13 | 08_Weather | Bright Water | Light | F Major | **Weather 55%** Stream, Cloud, **Pitch +12**, To Key Off (the field's bubbles are C pentatonic, which sits inside F major; To Key would move them off the scale), Grains 8, Size 0.15 s, Spray 0.5, Drift 0.4, Reverse 0.2, Tilt +0.3, HP 400 Hz, Width 1, →Echo 0.2. **Bloom 50%** on Harmony, Glass Harmonica Age 0.3, Tone 9 kHz, Sus2 Spread, Swell 5 s, Release 12 s. **Ground 30%** Sine, Reg High, Root and Octave. **Echo** Stereo, Sync 1/8., Repeats 0.3, High Cut 9 kHz, Diffuse 0.5. **Space** Haze, Decay 12 s, Shimmer 0.3 (+12). Air off. Macros: Glow, Density (1–16 grains). | a luminous stream, glinting an octave up |
| 14 | 08_Weather | Sun Shower | Light | A# Lydian | **Weather 55%** Light Rain, Cloud, Listen Harmony, To Key Chord, **Grains 12**, Size 0.07 s, Spray 0.6, Drift 0.5, Tilt +0.4, HP 600 Hz, Duck 0.2, →Echo 0.25. **Air 40%** Bar, **Rise**, 10/min, Gravity 1 (it climbs the chord's tones), Reg High, Range 2, Decay 6 s, Tone 9 kHz, →Echo 0.2. **Bloom 55%** Sine Bloom Age 0.25, Tone 8 kHz, Add9 Spread, Swell 2 s, Release 8 s. **Ground 25%** Sine, Reg High. **Echo** Ping-Pong, Sync 1/8, Repeats 0.35, High Cut 10 kHz, Diffuse 0.4. **Space** Space, Decay 8 s, Shimmer 0.35 (+12). **Deliberately dense:** 12 grains (16 at Density +100%) with Air, Echo and shimmer; Bloom stays at unison 1 to pay for it. | bright rain falls in each chord, bars rising |
| 15 | 08_Weather | Gale Warning | Dark | C# Phrygian | **Weather 60%** Wind High, Cloud, Pitch −7, Grains 10, Size 0.8 s, Spray 0.6, Drift 0.6, Tilt −0.5, HP 60 Hz, Width 1. **Ground 80%** Cello Tasto, Reg Low, Sub 0.6, Fifth 0, Color min3 0.3, **Beat 1.2 Hz** (the tension), Breath 0.6 over 20 s, Tone 400 Hz, →Echo 0.2. **Bloom 45%** Tape Strings Age 0.5, Reso 0.3, Tone 1.2 kHz, Cluster Spread, Swell 10 s, Release 14 s, →Echo 0.2. **Echo** Mono, Free 1600 ms, Repeats 0.6, High Cut 1.2 kHz, Wow 0.4, Diffuse 1: thunder rolling off. **Space** Abyss, Decay 15 s, Rise 0.5. Air off. Macros: Horizon far carries the storm away; Motion. Medium-dense: 10 grains, Diffuse 1 and Abyss, but no Air. | a dark gale howls over a tense, low drone |
| 16 | 08_Weather | Last Embers | Dark | G Hirajoshi | **Weather 50%** Embers, **Stretch**, Grains 6, Size 0.3 s, **Pitch −12** (a slower, deeper crackle), Spray 0.4, Drift 0.15, Reverse 0.3, Tilt −0.3, →Echo 0.3. **Ground 75%** Felt Piano Age 0.8, Reg Low, Sub 0.4, Fifth 0.5, Tone 600 Hz, Beat 0.6 Hz, Breath 0.6 over 45 s. **Bloom 45%** on Harmony, Felt Piano Age 0.9 (the deep tail), Tone 1.1 kHz, Seventh, Swell 12 s, Release 12 s. **Echo** Stereo, Free 750 ms, Repeats 0.5, High Cut 1.5 kHz, Wow 0.5, Diffuse 0.6. **Space** Hall, Decay 12 s, Damp 2.5 kHz. Air off. Macros: Glow dark (it tilts Weather down too), Density (the fire dies down to a single grain). | dying embers crackle low in the dark |
| 17 | 09_Memory | Slow Recollection | Slow | G# Min Pent | **Weather 45%** memory: (silent until the first Remember), **Stretch** (the remembered 16 s crawl past at 1/8 speed, about two minutes), Memory From **Strata**, Grains 6, Size 0.8 s, Spray 0.4, Drift 0.25, Reverse 0.2, Duck 0.3. It sounds whole before any Remember: **Bloom 60%** on Harmony, Tape Strings Age 0.5, Sway 0.5 at 90 s, Seventh Spread, Swell 8 s, Release 14 s; **Ground 45%**; **Air 40%** Felt, Constellation, 6/min, Mutate 0.4, Decay 6 s. Echo off. **Space** Space, Decay 14 s. Macros: Motion (how much the crawl wanders), Horizon. | Remember: 16 s return over minutes |
| 18 | 09_Memory | Palimpsest | Played | C Min Pent | **Weather 55%** memory:, Cloud, **Listen Notes** (it sounds only while pads are held, which makes the past an instrument), Memory From **Output** (each Remember also takes what Weather played, so the past is written over), Grains 8, Size 0.4 s, Spray 0.5, Drift 0.3, Reverse 0.4. **Bloom 65%** Felt Piano Age 0.4, Seventh, Swell 1 s, Release 6 s, **Tail Voice** (Tail Space would hold the room's decay at 6 s), →Echo 0.25. **Ground 45%** on Notes (your lowest key). **Echo** Stereo, Sync 1/4, Repeats 0.4, Diffuse 0.3. **Space** Room, Decay 1.5 s, close so the past stays clear. Air off. Moves: Remember, then hold pads; Hold On keeps the past sounding. Macros: Density (grains), Motion. | Remember, then hold pads to play your past |
| 19 | 09_Memory | Afterimage | Light | B Whole Tone, Equal | **Weather 45%** memory:, Cloud, Memory From **Strata**, **Pitch +12** (an octave keeps the key), Grains 10, Size 0.2 s, Spray 0.6, Drift 0.4, Reverse 0.5, Tilt +0.4, HP 300 Hz, Width 1, →Space 0.8. **Bloom 60%** Celesta Age 0.5, Triad (augmented), Spread, Strum 0.8 s, Swell 2 s, Release 12 s. **Air 40%** Glass, Constellation, 8/min, Reg High. **Ground 30%** Sine, Reg High. **Echo** Stereo, Sync 1/4, Repeats 0.3, High Cut 9 kHz, Diffuse 0.5. **Space** Haze, Decay 12 s, Shimmer 0.3 (+12). Equal, because a whole-tone scale comes out uneven in Just. Macros: Glow, Density. | Remember: your notes glow an octave up |
| 20 | 09_Memory | Undertow | Dark | D# Minor | **Weather 50%** memory:, Cloud, Memory From Output, **Pitch −12**, Grains 6, Size 1.2 s, Spray 0.5, Drift 0.3, Reverse 0.5, Tilt −0.5, Duck 0.2. **Ground 80%** Cello Tasto, Reg Low, Sub 0.5, Fifth 0.6, Beat 0.5 Hz, Tone 500 Hz. **Bloom 50%** Tape Strings Age 0.6, Tone 1.4 kHz, Triad Drop 2, Swell 6 s, Release 14 s. **Air 30%** Bell, Random, 3/min, Reg Low, Decay 12 s, Tone 1.5 kHz. Echo off. **Space** Abyss, Decay 10 s, Rise 0.3. Macros: Horizon, Glow. | Remember: your past sinks an octave, dark |
| 21 | 02_Drones | Abyssal Plain | Dark | B Phrygian | **Ground 100%** Tape Strings, Reg Low, Sub 0.6, Fifth 0.3, Octave 0.2, Tone 350 Hz, Beat 0.4 Hz, Breath 0.6 over 2 min, →Echo 0.2. **Weather 40%** Surf, Cloud, **Pitch −12** (the swells an octave down), Grains 5, Size 1 s, Spray 0.6, Drift 0.3, Tilt −0.6. **Bloom 30%** on Harmony, Sine Bloom Age 0.3, Tone 900 Hz, Fifths, Swell 15 s. **Echo** Mono, Free 1400 ms, Repeats 0.7, Low Cut 60 Hz, High Cut 900 Hz, Wow 0.4, Diffuse 1. **Space** Abyss, Decay 20 s, Damp 2 kHz, Rise 0.3. Air off. Medium-dense: Diffuse 1 into an Abyss that keeps running for minutes after the last note. | the dark sea heard from far below |
| 22 | 03_Beds | Unequal Loops | Slow | C# Major | CONCEPT §7.2's phasing, set by hand (the Phase control is M3), with B = 20 s: Ground Breath 20 s, Bloom Sway 28.3 s (B√2), **Air Loop 32.4 s** (Bφ), Ground Sway 44.7 s (B√5). Four cycles that never line up again. **Air 55%** Felt, Random, 8/min, **Loop On, Free, 32.4 s** (about 4 notes a pass), Rubato 0.3, Reg Mid, Range 2, Gravity 0.7, Decay 6 s, →Echo 0.2. **Bloom 55%** on Harmony, Choir Ah-Oo Age 0.3, Sway 0.4, Add9 Spread, Swell 10 s, Release 9 s. **Ground 50%** Breath 0.4, Sway 0.3, Beat 0.1 Hz. **Echo** Stereo, Free 1300 ms, Repeats 0.35, High Cut 3 kHz, Diffuse 0.5. **Space** Hall, Decay 9 s. Macros: Motion bends the free rates but not the loop's length. | unequal loops that drift apart for hours |
| 23 | 04_Blooms | Horizon Line | Played | F# Dorian | Built for one knob: every stratum sends to Echo and Space, so **Horizon** at −100% is a dry, close room and at +100% is far away and echoing (the sends from half to 1.4×, Space Decay from 0.41× to 2.46×, Rise up, the dry 3 dB back). **Bloom 75%** Felt Piano Age 0.45, Tone 4 kHz, Seventh Open, Strum 0.3 s, Swell 1.5 s, Release 6 s, **Tail Voice** (on Tail Space the room's decay is held at the release, and near couldn't shorten it), →Space 0.55, →Echo 0.35. **Air 40%** Glass, Constellation, 8/min, Reg High, Decay 5 s, →Echo 0.35. **Ground 35%**, →Echo 0.15. **Echo** Stereo, Sync 1/4., Repeats 0.5, High Cut 4.5 kHz, Diffuse 0.5, Duck 0.5, Echo Space 0.4. **Space** Hall, Decay 6 s, Rise 0.3. **Hold On**: tap a chord and it stays; Freeze keeps the far room. This is the first preset to judge Task 11's Horizon-far question on. | ride Horizon: from a close room to far away |
| 24 | 05_Choirs | Choir of Light | Light | E Major | **Bloom 70%** Choir Ah-Oo Age 0.2 ("ah"), Unison 2, Detune 8 ct, Add9 Spread, Strum 0.6 s, Swell 4 s, Release 12 s. **Ground 45%** Choir Ah-Oo, Body 0.25, Fifth 0.7. **Air 45%** Bell, **Rise**, Gravity 1 (it climbs the chord), 9/min, Reg High, Range 2, Decay 8 s, Tone 10 kHz, →Space 0.8, →Echo 0.3. **Echo** Stereo, Sync 1/2, Repeats 0.4, High Cut 8 kHz, Diffuse 0.8. **Space** Space, Decay 12 s, Shimmer 0.35 (+19). Weather off. Medium-dense: the only unison 2 in the set, so no Weather. | a bright choir crowned by rising bells |

**Read before building.** These facts from the code shaped the recipes:
- **Air's Echo pattern learns only notes given to Air.** That means a key at or above Split, or Air
  on Notes, and on Notes Air generates nothing. Felt Echoes therefore needs Split.
- **To Key Chord follows the chords only on Listen Harmony or Notes.** On Free it stays on the
  tonic chord. Rain on the Roof and Sun Shower are on Harmony for that reason.
- **To Key moves a whole grain so that the source's C lands on a scale or chord tone.** That works
  for the unpitched fields. Stream (C pentatonic) and Night (C and G) have pitches of their own, so
  Bright Water and Night Crickets sit in keys that contain those pitches and leave To Key Off. For
  Memory, see question 2.
- **A loop that is replaying generates nothing,** so Density bends only its first pass.
- **With Bloom's Tail on Space, Space's decay is held at Bloom's Release or longer.** A preset that
  wants a short Room must therefore set Tail Voice, as Palimpsest does. So must a preset whose room
  Horizon near should shorten (Horizon Line). Everywhere else the recipes keep Space Decay at or
  above Bloom's Release, so the Decay written is the one heard.
- **CPU.** No preset stacks every stratum at its heaviest, and M2's worst case (14.2% p99, estimated)
  is far away.
  - The densest are Sun Shower (Weather 12 grains with Air, Echo and shimmer) and Choir of Light
    (unison 2 with Air and Diffuse 0.8, and no Weather). Next come Gale Warning and Abyssal Plain:
    Diffuse 1 into Abyss with Weather at 10 and 5 grains, and no Air.
  - Only Sun Shower goes above 10 grains, and only Choir of Light uses unison 2.
  - Felt never rings longer than 6 s or plays faster than 10/min.
  - Neither Kalimba preset takes the player's notes, which keeps away the pluck's low-note spike
    (30k at C1).
- **Thick Density (Task 11's open item).** The full sweep's report should name the presets it moves
  most: Sun Shower, Wind Chimes, Rain on the Roof and Bright Water.

## 3. What each direction shows

- **Slow evolution:** music that changes over an hour with no hands on it, where the 28 M1 presets
  hold a chord. A Constellation turns into another melody (Hour Glass). Four loops of unequal length
  phase against each other (Unequal Loops). Bowls drift over free cycles of 3 to 8 minutes. A field
  wanders or stretches (Wind over Fifths, Night Crickets). Memory's 16 s crawl back over two minutes
  (Slow Recollection).
- **Played with the MPC:** the instrument answering the player.
  - Kalimba Loop has a loop locked to MPC's bar that each pad re-chords.
  - In Felt Echoes, Air plays your melody back to you over Split.
  - In Rain on the Roof the rain tunes to every chord and steps back while you play.
  - Remember is a gesture in two ways: the tide replays you in Remembered Tide, and in Palimpsest
    held pads play the past.
  - Horizon Line is built to be ridden on one knob, with Hold and Freeze as moves.
- **Dark and cinematic:** the new strata's dark side. Fields pitched down under low drones (Surf
  −12, Embers −12, Wind −7). Echo diffused into Abyss as rolling thunder. Bells, plucks and Memory
  sinking low. Tension from Beat, Cluster, Phrygian, In-Sen and Hirajoshi.
- **Light and luminous:** brightness that doesn't come from Bloom alone. Fields an octave up with
  shimmer (Bright Water, Sun Shower). Bars and bells climbing the chord with Rise. Chimes in a high
  breeze. Your own past glowing an octave up (Afterimage). Haze and Space with shimmer at +12 or +19,
  over open voicings (Add9, Sus2, Spread).

## 4. Open questions

1. **Is the count right, and are these the right homes?** The proposal is 16 more (24 in Task 12),
   a new 09_Memory category, and one preset each in Drones, Beds, Blooms and Choirs. Remembered Tide
   stays in Weather, because its source is a field until Remember. The plan's Task 12 file list
   names only 07_Glass and 08_Weather.
   *Recommendation:* yes. If device time is short, build rows 1–16 now and rows 17–24 when M3 brings
   Autopilot.
2. **What should To Key do on Memory?** Decision 5 roots every source on C, and To Key transposes
   whole grains. On a remembered phrase (or a WAV of music) every grain becomes a parallel
   transposition, and most of them land off the scale in any key.
   *Recommendation:* the Memory presets keep To Key Off and move only by octaves, as above. Task 13's
   USER_GUIDE says To Key is for the fields and Pitch is for Memory. No engine change in M2.
3. **How do Memory presets meet the level contract?** The phrase plays with nothing remembered, so
   their Weather is never measured.
   *Recommendation:* match them on the phrase as it is, and keep Memory's Weather at 45–55%. That
   plays the remembered mix 10–14 dB under the mix itself, which should add about half an LU. Task
   12 then reports the LU and peak each one adds from a scratch run with Remember at 24 s, and fails
   any preset that adds more than 2 LU or passes −1 dBFS. Make that run part of preset_test only if
   you want it guarded for good.
4. **Should Horizon far darken Air Tone and Echo High Cut too?** This is Task 11's open item.
   *Recommendation:* yes, by `kHorizonToneOct`. The macro table already says far is darker, and
   without it the bright strikes and repeats stay close while the bed recedes. Judge it on Horizon
   Line, Snow Constellation and Kalimba Loop, with Cave Kalimba as the dark control. Felt has no Tone,
   so in Felt Echoes and Unequal Loops only the repeats would darken.
5. **Should Memory presets name memory: or start from a field?** A preset that names memory: is
   silent in Weather until the first Remember. Remembered Tide starts on Surf instead.
   *Recommendation:* memory: for the four in 09_Memory. The category is about the instrument's own
   past; each preset sounds whole without Weather, and each about= line opens with "Remember".
   Remembered Tide shows the field-first way. An instance that has already remembered something
   plays it at once (decision 10).
