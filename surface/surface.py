#!/usr/bin/env python3
# From SubForce surface/surface.py (8846421), renamed; the page groups from EffectForce's (4160e87).
"""AmbientForce touchscreen surface: the ONE place the plugin's parameters and pages are defined.

Writes, next to this file:
  params.json        ordered VST parameter list (index = position; append-only once released:
                     MPC projects store values by index)
  layout.conf        the skin (shadow_page.conf syntax, see
                     third_party/mpc-vst-plugins/tools/shadow_skin.py); coords are 1280x800
                     Force-Shadow pixels, the plugin area is y = 86..714
  vst.json           plugin identity for the vendored gen_vst.py
  build/param_ids.h  everything the C++ is compiled against: parameter ids, kinds, value
                     curves, names, options, defaults and help lines (the C++ never reads gen_vst's
                     params.h, so `make test` and the .so build need only Python, not the skin toolchain)
  build/factory_presets.h  the factory presets (presets/Factory/*/*.afp), checked and embedded
  build/skin_style.json  the palette, knob looks, primary buttons and page groups for
                     skin_polish.py, which `make skin` runs after the generator

Continuous parameters are declared to MPC as 0..1: the real range and curve (log Hz, log
seconds, ...) live in param_ids.h, and the plugin formats every value text itself, so the
knob, its label and the DSP can never disagree.

Before writing anything the layout is checked the way shadow_skin.py would (unknown keys,
option counts, when=, Q-Link sets) plus geometry with shadow_skin's own sizes (inside the plugin
area, no overlaps within a page, nothing in a card's title band, open popup lists inside the
plugin area, options that fit their popup field or segment, bitmap-font glyphs), the
parameter names MPC shows (short, unique) and the help lines and preset descriptions the status
line shows (every control a hand moves has one; each fits), so a broken page fails here instead
of on the device.
The layout machinery and its checks are PolyForce's; the page groups (several pages under one
tab) are EffectForce's.

Run: python3 surface/surface.py   (make surface does this)
"""
import json
import math
import os
import re
import shlex
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

STEPPER_RANGE = 1023        # the preset stepper's VST range: 0..1023 items (stepItem moves 1 per event)
BROWSER_CATS = 16           # category tiles on the browser page (2 x 8)
BROWSER_ITEMS = 24          # preset tiles (3 x 8)

VST = {"name": "AmbientForce", "vendor": "Devko", "uid": "AmFc", "version": 1000,
       "so": "ambientforce.so", "params": "params.json", "layout": "layout.conf"}


# --- parameters ------------------------------------------------------------------------------
# kind:
#   synth    a sound parameter: saved in the state, automatable; curve lin|log|int|pow|enum
#   ui       a value the surface keeps for itself (a page's own setting): not saved, not automatable
#   readout  text the plugin writes (status line, "PAGE 2 / 4"): read only
#   stepper  plugin-owned index into a list (presets), text = the item; moves one item per
#            Q-Link/wheel event; comes with <key>_prev / <key>_next buttons
#   button   momentary: acts on the press, springs back to 0
#   tile     a browser tile (list widget): lit = 1, text = the item; a tap acts
#   toggle   plugin-owned on/off (lit state follows the plugin), a tap acts
#   popup    the hidden "<key>__open" flag of a popup list (shadow_skin's popup_params)
#   meter    a value the plugin sets for a display-only filmstrip: not saved, not automatable
# fmt: how the plugin prints the value (see plugin/patch_map.cpp paramDisplay)
# help: what the status line says for a few seconds after the control is moved, as "NAME: help" (the name in
#   capitals): what it does and what its range means, plain and concrete. Every control a hand moves has one
#   (HELP_KINDS: the sound values, the steppers, buttons and toggles); tiles, readouts and popup flags have
#   none. check_layout() holds each line to the narrowest status readout's width.
P = []
HELP_KINDS = ("synth", "ui", "stepper", "button", "toggle")


def _add(key, name, kind, curve, lo, hi, default, fmt, **extra):
    d = dict(key=key, name=name, kind=kind, curve=curve, lo=lo, hi=hi, default=default, fmt=fmt)
    d.update(extra)
    P.append(d)


def readout(key, name):
    _add(key, name, "readout", "readout", 0, 0, 0, "none")


def meter_param(key, name):
    _add(key, name, "meter", "lin", 0, 1, 0.5, "none")


def enum(key, name, options, default, help, ui=False):
    _add(key, name, "ui" if ui else "synth", "enum", 0, len(options) - 1, options.index(default), "enum",
         options=options, help=help)


def num(key, name, curve, lo, hi, default, fmt, help, ui=False):
    assert curve != "pow" or lo == 0, "%s: a pow curve runs from 0 (patch_map.cpp: hi x n^3)" % key
    _add(key, name, "ui" if ui else "synth", curve, lo, hi, default, fmt, help=help)


def stepper(key, name, help, prev, next):
    _add(key, name, "stepper", "int", 0, STEPPER_RANGE, 0, "text", help=help)
    button(key + "_prev", name + " Prev", prev)
    button(key + "_next", name + " Next", next)


def button(key, name, help):
    _add(key, name, "button", "int", 0, 1, 0, "none", help=help)


def tile(key, name):
    _add(key, name, "tile", "enum", 0, 1, 0, "text", options=["-", "On"])


def toggle(key, name, help):
    _add(key, name, "toggle", "enum", 0, 1, 0, "enum", options=["Off", "On"], help=help)


def help_line(p):
    """What the status line shows after p is moved: "NAME: help"."""
    return "%s: %s" % (p["name"].upper(), p["help"])


def popup_flag(of):
    src = next(p for p in P if p["key"] == of)
    _add(of + "__open", "%s List" % src["name"], "popup", "enum", 0, 1, 0, "none", options=["Closed", "Open"],
         popup_of=of)


readout("status", "Status")            # index 0 must stay a read-only readout: MPC sets it at load
num("volume", "Volume", "lin", -60, 6, -6, "db", help="the output level; the limiter after it holds -1 dBFS")

# Every default and range below is the engine's own (dsp/engine.h Patch and the headers it holds), so Init
# plays what a Patch{} plays: test/params_test.cpp holds the two together. The levels and sends have the
# family's audio taper (gain = knob^2, plugin/patch_map.cpp): Patch{} holds their default knobs squared (Level
# 70% is a gain of 0.49). Every option list is the engine's names for its values, in their order
# (plugin/patch_map.cpp checks each entry as it compiles). Hold, Freeze and the mutes are segments, not toggle
# tiles: a tile's release echo is a second tap.
ON_OFF = ["Off", "On"]
LISTEN = ["Notes", "Harmony", "Free"]                                                 # dsp/harmony.h Listen
# The table library (dsp/lifetime.h kTableNames): lifetime tables, then the digital waves.
TABLES = ["Felt Piano", "Celesta", "Glass Harmonica", "Cello Tasto", "Choir Ah-Oo", "Reed Organ", "Sine Bloom",
          "Tape Strings", "Sine", "Triangle", "Saw", "Square"]

# --- the harmony brain (dsp/harmony.h HarmonyPatch; Hold and On Stop are the engine's) ---
enum("h_key", "Key", ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"], "C",
     help="the tonic every chord and the drone are built on")
popup_flag("h_key")
enum("h_scale", "Scale", ["Major", "Minor", "Dorian", "Lydian", "Mixolydian", "Phrygian", "Maj Pent", "Min Pent",
                          "Hirajoshi", "In-Sen", "Whole Tone", "Chromatic"], "Major",          # Scale
     help="the notes the keys snap to and chords are built from")
popup_flag("h_scale")
enum("h_tuning", "Tuning", ["Equal", "Just", "Pythagorean"], "Just",                           # Tuning
     help="Equal, Just (pure, still intervals) or Pythagorean")
enum("h_input", "Input", ["As Played", "Snap", "Degrees"], "Snap",                             # Input
     help="a key as played, snapped into the scale, or a degree")
enum("h_chord", "Chord", ["Off", "Triad", "Seventh", "Sus2", "Sus4", "Add9", "Quartal", "Fifths", "Cluster",
                          "Spread"], "Triad",                                                  # ChordType
     help="what each key plays, from the scale's own notes")
popup_flag("h_chord")
enum("h_voicing", "Voicing", ["Close", "Open", "Drop 2", "Spread"], "Open",                    # Voicing
     help="how a chord's notes spread, Close to wide Spread")
enum("h_leading", "Leading", ON_OFF, "On", help="On moves each new chord as little as it can")
num("h_strum", "Strum", "pow", 0, 2, 0, "time", help="a chord's notes come in one by one, over 0 to 2 s")
# memoryBars: Off 0, the bars, Forever -1 (patch_map.cpp kMemoryBars).
enum("h_memory", "Memory", ["Off", "1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars", "32 Bars", "64 Bars", "Forever"],
     "Forever", help="how long a chord is kept once the keys are up")
popup_flag("h_memory")
enum("h_hold", "Hold", ON_OFF, "Off", help="On latches the keys until the next chord takes over")
enum("h_onstop", "On Stop", ["Keep", "Fade", "Cut"], "Fade",                                   # dsp/engine.h OnStop
     help="what Stop does: Keep playing, Fade over 8 s, or Cut")

# --- Ground, the drone (dsp/ground.h GroundPatch) ---
enum("g_listen", "Ground Listen", LISTEN, "Harmony", help="lowest key, the harmony's root, or the tonic")
enum("g_mute", "Ground Mute", ON_OFF, "Off", help="silences the drone, gliding, at next to no CPU")
num("g_level", "Ground Level", "lin", 0, 1, 0.7, "pct", help="the drone's level; 70% is -6 dB")
num("g_cutoff", "Ground Tone", "log", 40, 16000, 2500, "hz", help="the drone's brightness, a gentle low-pass")
enum("g_table", "Ground Table", TABLES, "Cello Tasto", help="the instrument all the drone's partials play")
popup_flag("g_table")
num("g_age", "Ground Age", "lin", 0, 1, 0.5, "pct", help="the moment of its note, from struck to fading")
num("g_sway", "Ground Sway", "lin", 0, 1, 0.3, "pct", help="how far it drifts around its Age; 0 holds still")
num("g_swayrate", "Ground Rate", "log", 0.002, 2, 0.05, "period", help="how fast the drone sways: one cycle's time")
num("g_beat", "Ground Beat", "lin", 0, 3, 0.3, "hz2", help="how fast the partials beat together; 0 is still")
num("g_gravity", "Gravity", "pow", 0, 30, 6, "time", help="how long the drone glides to a new root; 0 jumps")
num("g_fade", "Ground Fade", "log", 0.05, 30, 4, "time", help="how slowly the drone fades in, and out")
num("g_sub", "Ground Sub", "lin", 0, 1, 0.3, "pct", help="the partial an octave under the root")
num("g_root", "Ground Root", "lin", 0, 1, 1, "pct", help="the drone's root")
num("g_fifth", "Ground Fifth", "lin", 0, 1, 0.5, "pct", help="the fifth over the root, pure under Just")
num("g_oct", "Gnd Octave", "lin", 0, 1, 0.25, "pct", help="the partial an octave over the root")
num("g_color", "Ground Color", "lin", 0, 1, 0, "pct", help="a fifth partial, at the Color Int interval")
enum("g_colint", "Color Int", ["min3", "maj3", "4th", "min7", "9th", "11th"], "9th",           # ColorInterval
     help="the Color partial's interval over the root")
enum("g_reg", "Ground Reg", ["Low", "Mid", "High"], "Mid",                                     # registerOct 1..3
     help="the octave the drone starts in: C1, C2 or C3")
num("g_body", "Ground Body", "lin", 0, 1, 0, "pct", help="a vowel on the drone: off, then a, o and u")
num("g_breath", "Gnd Breath", "lin", 0, 1, 0.3, "pct", help="a slow swell of level and tone, 14 s a cycle")
num("g_space", "Ground Space", "lin", 0, 1, 0.4, "pct", help="how much of the drone goes into the reverb")
num("g_width", "Ground Width", "lin", 0, 1, 0.5, "pct", help="spreads the partials across the stereo field")
num("g_pan", "Ground Pan", "lin", -1, 1, 0, "pan", help="where the drone sits, left to right")

# --- Bloom, the chords (dsp/bloom.h BloomPatch) ---
enum("b_listen", "Bloom Listen", LISTEN, "Notes", help="a chord per key, the harmony's, or the tonic's")
enum("b_mute", "Bloom Mute", ON_OFF, "Off", help="silences the chords, gliding, at next to no CPU")
num("b_level", "Bloom Level", "lin", 0, 1, 0.7, "pct", help="the chords' level; 70% is -6 dB")
num("b_cutoff", "Bloom Tone", "log", 20, 20000, 5000, "hz", help="the filter frequency: darker down, brighter up")
num("b_reso", "Bloom Reso", "lin", 0, 1, 0.1, "pct", help="the filter's resonance, from gentle to singing")
enum("b_fmode", "Bloom Filter", ["LP", "BP", "HP"], "LP",                                      # FilterMode
     help="low-, band- or high-pass at Bloom Tone")
enum("b_table", "Bloom Table", TABLES, "Felt Piano", help="the instrument whose note the chords play")
popup_flag("b_table")
num("b_age", "Bloom Age", "lin", 0, 1, 0.6, "pct", help="where in a note's life you listen, struck to fading")
num("b_sway", "Bloom Sway", "lin", 0, 1, 0.25, "pct", help="how far the voices drift around Age; 0 is still")
num("b_swayrate", "Bloom Rate", "log", 0.002, 2, 0.07, "period", help="how fast the voices sway: one cycle's time")
num("b_smear", "Bloom Smear", "lin", 0, 1, 0.1, "pct", help="a fast flicker of the tone; 0 is steady")
enum("b_tableb", "Bloom Table B", TABLES, "Sine", help="a second table at the same Age, for Blend")
popup_flag("b_tableb")
num("b_boct", "Bloom B Oct", "int", -2, 2, 0, "oct", help="Table B's octave, from two down to two up")
num("b_blend", "Bloom Blend", "lin", 0, 1, 0, "pct", help="Table A alone (0) to Table B alone (100%)")
enum("b_couple", "Bloom Couple", ["Mix", "FM", "AM", "Ring"], "Mix",                           # dsp/lifeosc.h Couple
     help="how B meets A: mixed, FM, AM or Ring")
num("b_camt", "Couple Amt", "lin", 0, 1, 0, "pct", help="how deeply B works on A in FM, AM or Ring")
enum("b_unison", "Bloom Unison", ["1", "2"], "1", help="2 doubles each voice, detuned; costs CPU")
num("b_detune", "Bloom Detune", "lin", 0, 50, 8, "cents", help="how far apart the unison pair is, in cents")
num("b_swell", "Bloom Swell", "log", 0.005, 30, 2.5, "time", help="how slowly a chord fades in, 5 ms to 30 s")
num("b_release", "Blm Release", "log", 0.01, 30, 6, "time", help="how slowly a chord dies away once let go")
num("b_vel", "Bloom Vel", "lin", 0, 1, 0.4, "pct", help="how much velocity sets the level; 0: all alike")
num("b_breath", "Bloom Breath", "lin", 0, 1, 0.05, "pct", help="noise at each note's pitch, a breath or bow")
enum("b_tail", "Bloom Tail", ["Voice", "Space"], "Space",                                      # Tail
     help="Space hands long releases to the reverb")
num("b_space", "Bloom Space", "lin", 0, 1, 0.5, "pct", help="how much of the chords goes into the reverb")
num("b_width", "Bloom Width", "lin", 0, 1, 0.6, "pct", help="spreads the chord's notes from left to right")
num("b_pan", "Bloom Pan", "lin", -1, 1, 0, "pan", help="where the chords sit, left to right")

# --- Space, the reverb (dsp/space.h Params, dsp/reverb.h Reverb::Params), and the output ---
enum("s_mode", "Space Type", ["Room", "Hall", "Plate", "Space", "Haze", "Abyss"], "Hall",       # Reverb::Mode
     help="the reverb, from Room to the endless Abyss")
popup_flag("s_mode")
num("s_size", "Space Size", "lin", 0, 1, 0.6, "pct", help="how big the space is; moving it bends the tail")
num("s_decay", "Space Decay", "log", 0.1, 30, 8, "time", help="how long the reverb rings, 0.1 to 30 s")
num("s_predelay", "Pre-Delay", "pow", 0, 250, 30, "ms", help="the gap before the reverb comes in, 0 to 250 ms")
num("s_damp", "Space Damp", "log", 1000, 20000, 6000, "hz", help="above it the tail dies faster; lower is darker")
num("s_lowcut", "Low Cut", "log", 20, 1000, 120, "hz", help="keeps the lows out of the reverb; 20 Hz is off")
num("s_mod", "Space Mod", "lin", 0, 1, 0.4, "pct", help="gentle movement in the tail, lush, never metallic")
num("s_width", "Space Width", "lin", 0, 1, 1, "pct", help="the reverb's stereo width; 0 is mono")
enum("s_freeze", "Freeze", ON_OFF, "Off", help="holds the reverb's tail forever, lets nothing new in")
num("s_shimmer", "Shimmer", "lin", 0, 1, 0, "pct", help="the tail climbs by Shimmer Int with every pass")
enum("s_shint", "Shimmer Int", ["+12", "+7", "+19", "-12"], "+12",                             # Reverb::Interval
     help="the climb: up an octave, fifth, twelfth or down")
num("s_rise", "Space Rise", "lin", 0, 1, 0.2, "pct", help="the reverb ducks while you play, blooms after")
num("s_return", "Space Level", "lin", 0, 1, 0.8, "pct", help="how loud the reverb comes back; 0 turns it off")
num("o_tilt", "Tilt", "lin", -1, 1, 0, "bipct", help="tips the whole sound darker (-) or brighter (+)")

# --- the macros (plugin/patch_map.cpp applyMacros) ---
# Four knobs that bend the sound the other knobs make, without moving them: at 0 the preset plays exactly as
# saved, and each one moves its fields (in the engine's Patch, never the parameters) monotonically, every
# result clamped to its parameter's range. Fixed and relative to the preset, brought forward from M3's
# per-preset macro mappings (docs/CONCEPT.md 7.1). Saved like every sound value; the factory presets keep them at 0.
num("m_horizon", "Horizon", "lin", -1, 1, 0, "bipct", help="near and dry (-) to far and vast (+); 0 is the preset")
num("m_motion", "Motion", "lin", -1, 1, 0, "bipct", help="still (-) to drifting and alive (+); 0 is the preset")
num("m_glow", "Glow", "lin", -1, 1, 0, "bipct", help="dark, warm (-) to bright, airy (+); 0 is the preset")
num("m_density", "Density", "lin", -1, 1, 0, "bipct", help="sparse, clear (-) to thick, full (+); 0 is the preset")

# --- free or synced: Ground's Breath and the two sways (dsp/common.h kSyncNames, kBarDivBeats; BeatClock) ---
# Free (the default everywhere: phasing, docs/CONCEPT.md 7.2, needs free cycles) runs at the rate knob; Sync runs one
# cycle per division of 4/4 bars, locked to MPC's bar position while the transport plays and on at the tempo while it
# is stopped. After the macros, so every earlier parameter keeps its index. Breath Rate runs from 128 times slower to
# 128 times faster than Ground's breath always ran (0.07 Hz: 30.5 min to 0.11 s), its default the knob's middle, so
# the default reads back as exactly 0.07 Hz and every preset breathes as it did (test/params_test.cpp).
SYNC = ["Free", "Sync"]
BAR_DIVS = ["1/4", "1/2", "1 Bar", "2 Bars", "4 Bars", "8 Bars", "16 Bars", "32 Bars", "64 Bars"]
num("g_breathrate", "Breath Rate", "log", 0.07 / 128, 0.07 * 128, 0.07, "period",
    help="one free breath's length, 0.1 s to 30 min")
enum("g_breathsync", "Breath Sync", SYNC, "Free", help="Free at Breath Rate, or Sync to MPC's bars")
enum("g_breathdiv", "Breath Div", BAR_DIVS, "8 Bars", help="one synced breath: a quarter note to 64 bars")
popup_flag("g_breathdiv")
enum("g_swaysync", "Gnd Rate Sync", SYNC, "Free", help="Free at Ground Rate, or Sync to MPC's bars")
enum("g_swaydiv", "Gnd Rate Div", BAR_DIVS, "8 Bars", help="one synced drone sway, 1/4 to 64 bars")
popup_flag("g_swaydiv")
enum("b_swaysync", "Blm Rate Sync", SYNC, "Free", help="Free at Bloom Rate, or all voices on the bars")
enum("b_swaydiv", "Blm Rate Div", BAR_DIVS, "8 Bars", help="one synced chord sway, 1/4 to 64 bars")
popup_flag("b_swaydiv")

# No RANDOMIZE: an instant jump of every sound value under a drone that holds for minutes is not music. The
# instrument's answer is Evolve (docs/CONCEPT.md 7.3): mutation ranges declared here, taken from the preset's
# own state and glided to through a scene, never jumped.

# --- presets ---
stepper("preset", "Preset", help="turn to walk through every preset, one per step",
        prev="loads the preset before this one", next="loads the preset after this one")
button("pre_save", "Save Preset", help="saves the sound as User NNN, in Presets/User")
button("pre_init", "Init Patch", help="loads Init, the template")

# --- the preset browser ---
for i in range(1, BROWSER_CATS + 1):
    tile("cat_%d" % i, "Category %d" % i)
button("cat_prev", "Categories Prev", help="the page of categories before this one")
button("cat_next", "Categories Next", help="the page of categories after this one")
for i in range(1, BROWSER_ITEMS + 1):
    tile("item_%d" % i, "Item %d" % i)
button("item_prev", "Items Prev", help="the page of presets before this one")
button("item_next", "Items Next", help="the page of presets after this one")
readout("item_page", "Items Page")
readout("br_now", "Loaded")
toggle("fav", "Favorite", help="marks or unmarks the loaded preset as a favorite")
button("rnd", "Random Pick", help="loads a random preset of the category shown")


def norm(p):
    """The default as MPC's 0..1 value."""
    lo, hi, d = p["lo"], p["hi"], p["default"]
    if p["curve"] == "log":
        return math.log(d / lo) / math.log(hi / lo)
    if p["curve"] == "pow":
        return (d / hi) ** (1.0 / 3.0) if hi > 0 else 0.0
    return (d - lo) / (hi - lo) if hi > lo else 0.0


def params_json():
    out = []
    for p in P:
        k = p["kind"]
        e = {"key": p["key"], "name": p["name"]}
        if k == "readout":
            e.update(min=0, max=0, display="string", type="readout")
        elif k == "stepper":
            e.update(min=0, max=1, display="string", type="stepper")
        elif k == "button":
            e.update(min=0, max=1, momentary=True, type="trigger")
        elif k == "popup":
            e.update(options=p["options"], default=p["options"][0], popup_of=p["popup_of"], type="enum")
        elif k == "tile":
            e.update(options=p["options"], default=p["options"][0], display="string")
        elif "options" in p:
            e.update(options=p["options"], default=p["options"][p["default"]])
        else:
            e.update(min=0, max=1, default=round(norm(p), 6), display="string")
        out.append(e)
    return {"name": VST["name"], "params": out}




# --- touchscreen pages -------------------------------------------------------------------------
# PolyForce's look (style=td3 rounded cards on one flat colour; bg and box the same, so the opaque image of a
# control never shows a box behind it), in AmbientForce's mist blue. Plugin area 1280x628 at y = 86..714. Every
# page: a header row (the status line from x=24), then cards at y=158 and y=440 (h=270) or one full-height card
# (h=552), x=24 w=1232 or halves at x=24 / 648 (w=608). Nothing may sit in a card's title band (y .. y+44: td3
# draws the title rule at y+38). skin_polish.py (run by `make skin` after the generator) redraws the knob
# strips, the trigger buttons and the stepper arrows, and makes the pages of a group sub-pages of one tab.
PALETTE = {
    "bg": "15171c", "box": "15171c", "line": "2d3038", "ink": "e9e9f0", "ink_dim": "9a9aa8",
    "ink_faint": "282a31", "accent": "8ab4f8", "accent_hi": "c6dcff", "lcd": "0c0d10",
    "seg_inactive": "212329", "seg_active": "8ab4f8", "seg_active_tx": "0d1b33", "tile_on": "1f2d45",
    "btn_bg": "2b2d34", "title": "aaa8b8", "knob_face": "26282e", "knob_ring": "3c3f47", "knob_dot": "8ab4f8",
}
FONT_LABEL = "fonts/TitilliumWeb-SemiBold.ttf"   # font_label=: shadow_skin sizes the buttons with it
LIVE_FONT = "fonts/TitilliumWeb-SemiBold.ttf"    # the face MPC draws live text in (names, values)
TITLE_FONT = "fonts/TitilliumWeb-Bold.ttf"       # = SHADOW_TITLE_FONT in the Makefile: card titles, enum
                                                 # labels and segment, popup-option and button text
TITLE_SIZE = 17
THEME = ("style=td3\nfont_label=%s\ntitle_size=%d\n" % (FONT_LABEL, TITLE_SIZE)
         + "".join("theme_%s=%s\n" % kv for kv in PALETTE.items()))
TEXT_INK = PALETTE["ink_dim"]   # free bitmap text (column headers, hints)

R1, R2 = 158, 440                                 # card rows (h=270), or R1 with h=552

# shadow_skin's geometry of the widgets this layout places (seg_rects, popup fields, the live labels): the page
# code and check_layout() both build on these.
SEG_V_H, SEG_V_STEP = 30, 32                      # enum_v: a segment's height, and the step to the next
SEG_H_H, SEG_H_STEP, SEG_GAP = 33, 35, 2          # enum_h: a segment's height, the step a row down, the gap across
SEG_V_LABEL, SEG_H_LABEL = 24, 22                 # a segment group's label: its centre this far over the first row
SEG_TEXT, SEG_PAD = 0.42, 6                       # segment text: the title font at 0.42 of the height; its padding
VALUE_PX = 26                                     # MPC's live value text (a knob's value, a popup's field)
POPUP_H, POPUP_CHEVRON = 40, 44                   # a popup's field: its height, and the chevron's share of its width
TEXT_SIZE = 1.5                                   # free bitmap text (text=): its scale, 9 px glyphs at 1
TEXT_H = int(9 * TEXT_SIZE + 0.5)

# Knobs: shadow_skin bakes ONE filmstrip per radius, so the radius picks the look. A bipolar knob (its arc
# grows from 12 o'clock) is one pixel smaller than a unipolar knob of the same size. This table is the only
# place that says so: check_layout() holds every knob to it and skin_polish.py draws the strips from it
# (exported as build/skin_style.json).
KNOB_SIZES = {"big": 30, "small": 22}
KNOB_STYLES = {r - b: {"bipolar": bool(b), "track": 4 if r - b >= 28 else 3, "pointer": 3.0 if r - b >= 28 else 2.5}
               for r in KNOB_SIZES.values() for b in (0, 1)}
BIPOLAR_EXTRA = ()                                # every bipolar knob here has a symmetric range
PRIMARY_BUTTONS = ("pre_save",)                   # drawn in the accent colour by skin_polish.py
FRAMES = 128                                      # shadow_skin: every filmstrip has 128 frames
PARAMS = {p["key"]: p for p in P}


def bipolar(key):
    """A knob whose value runs both ways from the middle (pan, tilt, amounts): symmetric range."""
    p = PARAMS[key]
    return p["lo"] == -p["hi"] or key in BIPOLAR_EXTRA


def knob_radius(key, size="big"):
    return KNOB_SIZES[size] - (1 if bipolar(key) else 0)


class Layout:
    """layout.conf lines, one generator [tab] per page. Pages are grouped: a group is one button of MPC's tab strip
    and its pages are that button's sub-pages (the dots under it; a tap on the button again shows the next), each
    with its own screen and its own Q-Link set. skin_polish.py renumbers the generator's tabs into these groups
    (skin_style.json "tab_groups"). mode() tags every widget that follows with when= until the next page or mode()."""

    def __init__(self):
        self.lines, self.when, self.groups = [THEME], None, []

    def group(self, name):
        self.groups.append({"name": name, "pages": []})

    def page(self, name, qlinks):
        """A page of the current group. Its name is also its Q-Link set's title, which MPC shows in the tab strip
        while the page is up."""
        self.lines.append("[tab %s]" % name)
        self.lines.append('qlinks "%s" = %s' % (name, ",".join(qlinks)))
        self.groups[-1]["pages"].append(name)
        self.when = None

    def mode(self, when):
        self.when = when

    def add(self, line):
        self.lines.append(line + (' when="%s"' % self.when if self.when else ""))

    def header(self, modes=None, status_w=None):
        """The status line from x=24 and the page-mode selector right-aligned to x=1256."""
        n = len(PARAMS[modes]["options"]) if modes else 0
        w = status_w or (1232 - n * 124 - 16 if n else 1232)
        self.readout(24 + w // 2, 121, w, "status")
        if n:
            self.hseg(1256 - (n * 124 - 2) // 2, 121, modes, 122)

    def card(self, x, y, w, h, title):
        self.add('frame x=%d y=%d w=%d h=%d title="%s"' % (x, y, w, h, title))

    def knob(self, cx, cy, key, size="big"):
        self.add('knob cx=%d cy=%d r=%d label="%s" key=%s' % (cx, cy, knob_radius(key, size), PARAMS[key]["name"], key))

    def hseg(self, cx, cy, key, sw, label=None, rows=1):   # rows > 1: the first row centred on cy, the rest below
        self.add('enum_h cx=%d cy=%d sw=%d%s key=%s%s' % (cx, cy, sw, " rows=%d" % rows if rows > 1 else "", key,
                                                         ' label="%s"' % label if label else ""))

    def vseg(self, cx, cy, key, sw=124, label=None):
        self.add('enum_v cx=%d cy=%d sw=%d key=%s%s' % (cx, cy, sw, key, ' label="%s"' % label if label else ""))

    def popup(self, cx, cy, w, key):
        self.add('popup cx=%d cy=%d w=%d h=%d key=%s' % (cx, cy, w, POPUP_H, key))

    def stepper(self, cx, cy, w, key):
        self.add('stepper cx=%d cy=%d w=%d h=40 key=%s' % (cx, cy, w, key))

    def readout(self, cx, cy, w, key, h=40):
        self.add('readout cx=%d cy=%d w=%d h=%d key=%s' % (cx, cy, w, h, key))

    def button(self, cx, cy, label, key):
        self.add('button cx=%d cy=%d label="%s" key=%s' % (cx, cy, label, key))

    def text(self, cx, top, label):   # bitmap font; top = the top of the glyphs (render_conf_preview.c)
        self.add('text cx=%d cy=%d label="%s" color=%s' % (cx, top, label, TEXT_INK))

    def slider(self, cx, cy, w, h, cw, key):
        self.add('slider_v cx=%d cy=%d w=%d h=%d cw=%d label="%s" key=%s' % (cx, cy, w, h, cw, PARAMS[key]["name"], key))

    def toggle(self, cx, cy, key):
        self.add('toggle cx=%d cy=%d label="%s" key=%s' % (cx, cy, PARAMS[key]["name"], key))

    def meter(self, cx, cy, w, h, key):   # display only: shadow_skin's filmstrip meter, no look (PolyForce patch)
        self.add('meter cx=%d cy=%d w=%d h=%d key=%s' % (cx, cy, w, h, key))

    def tiles(self, x, y, w, cols, rows, th, gap, key):
        self.add('list x=%d y=%d w=%d cols=%d rows=%d th=%d gap=%d key=%s' % (x, y, w, cols, rows, th, gap, key))


# A card's row of controls: one Q-Link bank, left to right in Q-Link order. Each control is as wide as it needs (a
# knob's box with its labels; a popup's field or a list's segments, sized to the longest option); row() spaces
# them evenly, so 8 knobs stand 152 px apart from x=100 as on the siblings' pages, 4 in a half card likewise. The
# knobs' centres share a line, and so do the lists' labels, the segments and popup fields starting under them.
# A list is labelled with its parameter's name without the stratum (the card says which), unless a page says
# otherwise.
KNOB_W = 130                                      # a knob's box with its labels (shadow_skin: max(130, 2r + 10))
KNOB_Y = 126                                      # a knob's centre, below its card's top
LABEL_Y = 89                                      # the centre of a list's label, below its card's top
LIST_Y = LABEL_Y + SEG_V_LABEL                    # where its segments or field start
SEG_MIN, CELL_MIN = 80, 44                        # a segment's width at least: a finger's; a cell in a row of them
SPANS = {"full": (35, 1229), "left": (35, 621), "right": (659, 1245)}   # x0..x1 of the controls in a card
STRATA = ("Ground", "Bloom", "Space")             # the strata whose cards name them
SEG_ROWS = {"g_colint": 2}                        # six short options: two rows of three
LIST_LABELS = {"g_reg": "REGISTER", "g_colint": "COLOR", "s_shint": "INTERVAL", "s_mode": "TYPE",
               "b_tableb": "TABLE B", "b_fmode": "FILTER", "g_breathsync": "SYNC", "g_breathdiv": "DIV",
               "g_swaysync": "SYNC", "g_swaydiv": "DIV", "b_swaysync": "SYNC", "b_swaydiv": "DIV"}


def list_label(key):
    name = PARAMS[key]["name"]
    for stratum in STRATA:
        if name.startswith(stratum + " "):
            name = name[len(stratum) + 1:]
    return LIST_LABELS.get(key, name.upper())


def popup_w(key):
    """A long list's field: its longest option in MPC's value text, TEXT_MARGIN to spare, and the chevron."""
    longest = max(text_w(LIVE_FONT, VALUE_PX, o) for o in PARAMS[key]["options"])
    return 4 * int(math.ceil((longest + TEXT_MARGIN + POPUP_CHEVRON) / 4.0))


def seg_px(kind):
    """The size of a segment's text (shadow_skin's label overlays)."""
    return max(10, int((SEG_V_H if kind == "enum_v" else SEG_H_H) * SEG_TEXT))


def seg_w(key, kind):
    """A short list's segment: its longest option, in capitals as shadow_skin draws it, TEXT_MARGIN to spare."""
    longest = max(text_w(TITLE_FONT, seg_px(kind), o.upper()) for o in PARAMS[key]["options"])
    return max(SEG_MIN if kind == "enum_v" else CELL_MIN, 2 * int(math.ceil((longest + TEXT_MARGIN + SEG_PAD) / 2.0)))


def control_w(key):
    p = PARAMS[key]
    if "options" not in p:
        return KNOB_W
    if key + "__open" in PARAMS:
        return popup_w(key)
    if key in SEG_ROWS:
        per = -(-len(p["options"]) // SEG_ROWS[key])
        return per * seg_w(key, "enum_h") + (per - 1) * SEG_GAP
    return seg_w(key, "enum_v")


def control(L, cx, top, key, label=None):
    """A knob, a long list's popup (its label above) or a short list's segments (under their label)."""
    p = PARAMS[key]
    label = label or list_label(key)
    if "options" not in p:
        L.knob(cx, top + KNOB_Y, key)
    elif key + "__open" in PARAMS:
        L.text(cx, top + LABEL_Y - TEXT_H // 2, label)
        L.popup(cx, top + LIST_Y + POPUP_H // 2, popup_w(key), key)
    elif key in SEG_ROWS:
        L.hseg(cx, top + LABEL_Y + SEG_H_LABEL + SEG_H_H // 2, key, seg_w(key, "enum_h"), label, rows=SEG_ROWS[key])
    else:
        L.vseg(cx, top + LIST_Y + len(p["options"]) * SEG_V_STEP // 2, key, seg_w(key, "enum_v"), label)


def row(L, top, keys, span="full", labels=None):
    x0, x1 = SPANS[span]
    widths = [control_w(k) for k in keys]
    gap = (x1 - x0 - sum(widths)) / max(1, len(keys) - 1)
    x = float(x0)
    for k, w in zip(keys, widths):
        control(L, int(round(x + w / 2.0)), top, k, (labels or {}).get(k))
        x += w + gap


def bank_card(L, top, title, keys, labels=None):
    """A full-width card holding one Q-Link bank."""
    L.card(24, top, 1232, 270, title)
    row(L, top, keys, "full", labels)


def bank_halves(L, top, left, right):
    """A Q-Link bank as two half cards of four: (title, keys) each."""
    for x, span, (title, keys) in ((24, "left", left), (648, "right", right)):
        L.card(x, top, 608, 270, title)
        row(L, top, keys, span)


def build_layout():
    """Every page, in groups (see Layout). Each page has its own Q-Link set: the Force's 8 knobs show the first 8
    keys, the next bank the other 8 (shadow_skin qlink_for_slot). The first bank is a page's top card, the second
    its bottom card, both in Q-Link order: the knob under your hand is the control in the same place on the screen."""
    L = Layout()

    # PLAY: the page you live on (docs/CONCEPT.md 9): the four macros first, under the first four Q-Links, then
    # Freeze and Hold, Bloom's Age and the volume; below, the levels and Bloom's swell, and the harmony. The
    # preset in the header. The tones, Decay and Shimmer the page had before are what Glow and Horizon bend.
    L.group("PLAY")
    macros, perform = ["m_horizon", "m_motion", "m_glow", "m_density"], ["s_freeze", "h_hold", "b_age", "volume"]
    levels, harmony = ["g_level", "b_level", "s_return", "b_swell"], ["h_key", "h_scale", "h_chord", "g_gravity"]
    L.page("PLAY", macros + perform + levels + harmony)
    L.header(status_w=700)
    L.stepper(998, 121, 516, "preset")
    bank_halves(L, R1, ("MACROS", macros), ("PERFORM", perform))
    bank_halves(L, R2, ("LEVELS AND SWELL", levels), ("HARMONY", harmony))

    # HARMONY: the harmony brain (CONCEPT 6); then how long it remembers, what Stop does, and who listens.
    harmony = ["h_key", "h_scale", "h_tuning", "h_input", "h_chord", "h_voicing", "h_leading", "h_strum"]
    memory = ["h_memory", "h_hold", "h_onstop", "g_listen", "b_listen", "volume", "g_gravity", "b_swell"]
    L.page("HARMONY", harmony + memory)
    L.header()
    bank_card(L, R1, "HARMONY", harmony)
    bank_card(L, R2, "MEMORY AND LISTEN", memory, {"g_listen": "GROUND", "b_listen": "BLOOM"})

    # STRATA: a page per stratum, then one for its details. The stratum pages share their first bank (CONCEPT 9):
    # Level, Tone, Shape, Motion, Character, then Gravity / Swell where Echo goes in M2, Space, Width.
    L.group("STRATA")
    ground = ["g_level", "g_cutoff", "g_age", "g_sway", "g_beat", "g_gravity", "g_space", "g_width"]
    ground2 = ["g_table", "g_swayrate", "g_fade", "g_body", "g_breath", "g_reg", "g_listen", "g_mute"]
    L.page("GROUND", ground + ground2)
    L.header()
    bank_card(L, R1, "GROUND", ground)
    bank_card(L, R2, "TABLE AND VOICE", ground2)

    # DRONE: Ground's five partials, its place and its beating; then its motion: the breath and the sway, each with
    # its depth, its free rate and whether it runs free or on the bars.
    partials = ["g_sub", "g_root", "g_fifth", "g_oct", "g_color", "g_colint", "g_pan", "g_beat"]
    breath, sway = ["g_breath", "g_breathrate", "g_breathsync", "g_breathdiv"], ["g_sway", "g_swayrate", "g_swaysync",
                                                                                 "g_swaydiv"]
    L.page("DRONE", partials + breath + sway)
    L.header()
    bank_card(L, R1, "PARTIALS", partials)
    bank_halves(L, R2, ("BREATH", breath), ("SWAY", sway))

    bloom = ["b_level", "b_cutoff", "b_age", "b_sway", "b_blend", "b_swell", "b_space", "b_width"]
    bloom2 = ["b_table", "b_release", "b_reso", "b_fmode", "b_tail", "b_vel", "b_listen", "b_mute"]
    L.page("BLOOM", bloom + bloom2)
    L.header()
    bank_card(L, R1, "BLOOM", bloom)
    bank_card(L, R2, "TABLE, FILTER AND TAIL", bloom2)

    # BLOOM OSC: Table B and how it couples to the first table and blends with it, unison, the breath noise; then
    # the sway (depth, free rate, free or on the bars) and the voice's place in its life and in the stereo field.
    osc = ["b_tableb", "b_boct", "b_couple", "b_camt", "b_blend", "b_unison", "b_detune", "b_breath"]
    sway, voice = ["b_sway", "b_swayrate", "b_swaysync", "b_swaydiv"], ["b_age", "b_smear", "b_pan", "b_width"]
    L.page("BLOOM OSC", osc + sway + voice)
    L.header()
    bank_card(L, R1, "OSCILLATORS", osc)
    bank_halves(L, R2, ("SWAY", sway), ("VOICE", voice))

    # SPACE: the reverb; then its tail (Freeze, Shimmer, Rise) and the levels into and out of it.
    L.group("SPACE")
    space = ["s_mode", "s_size", "s_decay", "s_predelay", "s_damp", "s_lowcut", "s_mod", "s_width"]
    tail, levels = ["s_freeze", "s_shimmer", "s_shint", "s_rise"], ["s_return", "g_space", "b_space", "o_tilt"]
    L.page("SPACE", space + tail + levels)
    L.header()
    bank_card(L, R1, "SPACE", space)
    bank_halves(L, R2, ("TAIL", tail), ("LEVELS AND TILT", levels))

    # MIX: each stratum's level, place, send and mute; then the return, the output, and the widths.
    mix_g, mix_b = ["g_level", "g_pan", "g_space", "g_mute"], ["b_level", "b_pan", "b_space", "b_mute"]
    out, width = ["s_return", "o_tilt", "volume", "s_freeze"], ["g_width", "b_width", "s_width", "s_shimmer"]
    L.page("MIX", mix_g + mix_b + out + width)
    L.header()
    bank_halves(L, R1, ("GROUND", mix_g), ("BLOOM", mix_b))
    bank_halves(L, R2, ("RETURN AND OUTPUT", out), ("WIDTH AND SHIMMER", width))

    # BROWSE: categories left, presets right, the loaded preset and actions below. The browser has no knobs of
    # its own: the Q-Links keep the preset stepper, the macros (to bend a preset while auditioning it), the
    # volume and PLAY's other main controls between them.
    L.group("BROWSE")
    L.page("PRESETS", ["preset", "m_horizon", "m_motion", "m_glow", "m_density", "b_age", "s_freeze", "volume",
                       "h_key", "h_scale", "h_chord", "g_gravity", "g_level", "b_level", "s_return", "b_swell"])
    L.header()
    L.card(24, R1, 360, 552, "CATEGORIES")
    L.tiles(44, 206, 320, 2, 8, 48, 8, "cat")
    L.button(124, 676, "< PREV", "cat_prev")
    L.button(304, 676, "NEXT >", "cat_next")
    L.card(400, R1, 856, 474, "PRESETS")
    L.tiles(420, 206, 816, 3, 8, 38, 8, "item")
    L.button(476, 596, "< PREV", "item_prev")
    L.readout(828, 596, 240, "item_page", h=36)
    L.button(1180, 596, "NEXT >", "item_next")
    L.readout(590, 676, 380, "br_now")   # the row: 400..1256, 8 px apart
    L.toggle(848, 668, "fav")
    L.button(968, 676, "RND", "rnd")
    L.button(1083, 676, "SAVE", "pre_save")
    L.button(1197, 676, "INIT", "pre_init")
    return L


def pages():
    """layout.conf."""
    return "\n".join(build_layout().lines) + "\n"


def skin_style():
    """What skin_polish.py needs (build/skin_style.json): the palette, knob looks and primary buttons to redraw the
    knob strips, buttons and stepper arrows, and the page groups to renumber the generator's tabs into sub-pages."""
    return {"palette": PALETTE, "title_font": TITLE_FONT, "frames": FRAMES,
            "knobs": {str(r): s for r, s in sorted(KNOB_STYLES.items())}, "primary_buttons": list(PRIMARY_BUTTONS),
            "tab_groups": build_layout().groups}


# --- layout check (offline, no skin toolchain) ---------------------------------------------------
# Geometry mirrors third_party/mpc-vst-plugins/tools/shadow_skin.py (component boxes, button_rect,
# seg_rects, popup_layout) and render_conf_preview.c (the bitmap font of `text`).
X0, Y0, X1, Y1 = 0, 86, 1280, 714
TITLE_BAND = 44              # a card's title band: y .. y+44
NAME_MAX = 13                # MPC shows a knob/slider's effGetParamName at ~19.5 px in a 130 px box
MAX_IMAGE_H = 16384          # taller skin images draw wrongly (sd88me/mpc-vst-plugins catalog_check.py warns)
POP_ROW, POP_GAP, POP_PAD, POP_GROUP_ROWS = 40, 2, 6, 8
BITMAP_GLYPHS = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-/_>%+:#"   # font8x8.h font_chars
BITMAP_ADVANCE = {" ": 4, "J": 9, "j": 9, ".": 9, "-": 9, ":": 9}   # font_glyph_width(): last lit column + 2; else 10
INT_KEYS = ("x", "y", "w", "h", "cx", "cy", "r", "sw", "rows", "cols", "th", "gap", "cw")


def _widget(line):
    toks = shlex.split(line)
    w = {"kind": toks[0]}
    for t in toks[1:]:
        k, _, v = t.partition("=")
        w[k] = int(v) if k in INT_KEYS else v
    return w


def bitmap_width(s, scale):
    """render_conf_preview.c text_width()."""
    return int(sum(BITMAP_ADVANCE.get(c, 10) * scale for c in s))


_FONTS = {}


def _ttf_advances(path):
    """(unitsPerEm, {char: advance}) from a TrueType font's cmap (format 4) and hmtx tables."""
    import struct
    d = open(path, "rb").read()
    tabs = {}
    for i in range(struct.unpack(">H", d[4:6])[0]):
        tag, _, off, ln = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
        tabs[tag.decode("latin-1")] = off
    upem = struct.unpack(">H", d[tabs["head"] + 18:tabs["head"] + 20])[0]
    nhm = struct.unpack(">H", d[tabs["hhea"] + 34:tabs["hhea"] + 36])[0]
    adv = [struct.unpack(">H", d[tabs["hmtx"] + 4 * i:tabs["hmtx"] + 4 * i + 2])[0] for i in range(nhm)]
    co = tabs["cmap"]
    for i in range(struct.unpack(">H", d[co + 2:co + 4])[0]):
        pid, eid, off = struct.unpack(">HHI", d[co + 4 + 8 * i:co + 12 + 8 * i])
        so = co + off
        if struct.unpack(">H", d[so:so + 2])[0] != 4 or (pid, eid) not in ((3, 1), (0, 3), (0, 4)):
            continue
        n2 = struct.unpack(">H", d[so + 6:so + 8])[0]
        ends = struct.unpack(">%dH" % (n2 // 2), d[so + 14:so + 14 + n2])
        starts = struct.unpack(">%dH" % (n2 // 2), d[so + 16 + n2:so + 16 + 2 * n2])
        deltas = struct.unpack(">%dh" % (n2 // 2), d[so + 16 + 2 * n2:so + 16 + 3 * n2])
        ro = so + 16 + 3 * n2
        ranges = struct.unpack(">%dH" % (n2 // 2), d[ro:ro + n2])
        out = {}
        for s in range(n2 // 2):
            for c in range(starts[s], min(ends[s], 0x7e) + 1):
                if ranges[s]:
                    gi = ro + 2 * s + ranges[s] + 2 * (c - starts[s])
                    g = struct.unpack(">H", d[gi:gi + 2])[0]
                    g = (g + deltas[s]) & 0xFFFF if g else 0
                else:
                    g = (c + deltas[s]) & 0xFFFF
                out[chr(c)] = adv[min(g, nhm - 1)]
        return upem, out
    raise SystemExit("%s: no Unicode cmap" % path)


def ttf_width(font, px, s):
    """Advance width of s in a bundled font at px pixels, measured with Pillow as shadow_skin does when it sizes
    a button or places a group label (Geometry mirrors it). Without Pillow (surface.py needs only python3): the
    font's own advance table, +1 px. Neither is an upper bound: Pillow's layouts differ by a few pixels among
    themselves (its hinted BASIC layout is the widest), and the table sits between them. Whether a text fits its
    box is text_w()'s to say, the same everywhere."""
    key = (font, px)
    if key not in _FONTS:
        path = os.path.join(HERE, font)
        try:
            from PIL import ImageFont
            _FONTS[key] = ImageFont.truetype(path, px).getlength
        except ImportError:
            upem, adv = _ttf_advances(path)
            _FONTS[key] = lambda t: sum(adv.get(c, upem) for c in t) * px / upem + 1.0
    return _FONTS[key](s)


TEXT_MARGIN = 3   # px a text keeps to spare in its box, by text_w()
_ADVANCES = {}


def text_w(font, px, s):
    """The width of s at px pixels by the font's own advance table, unhinted: the same with or without Pillow, on
    any machine, so a text fits or doesn't everywhere alike. What draws it lands near: over this layout's names,
    Pillow's hinted BASIC layout came out up to 2.8 px wider, raqm a little narrower, and MPC draws its own. So a
    text fits when it has TEXT_MARGIN to spare."""
    if font not in _ADVANCES:
        _ADVANCES[font] = _ttf_advances(os.path.join(HERE, font))
    upem, adv = _ADVANCES[font]
    return sum(adv.get(c, upem) for c in s) * px / upem


def _top_level(text):
    """The style keys before the first [tab] (shadow_skin apply_theme)."""
    top = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("["):
            break
        k, eq, v = line.partition("=")
        if eq and not line.startswith("#"):
            top[k.strip()] = v.strip()
    return top


class Geometry:
    """Where shadow_skin puts each widget, for this layout's style keys."""

    def __init__(self, top):
        self.td3 = top.get("style") == "td3"
        self.font_label = top.get("font_label")
        self.ls = float(top.get("label_scale", 1.15))

    def knob(self, w):   # shadow_skin build(): the filmstrip, the Name and Value labels under it
        r = w["r"]
        s = 2 * r + 10
        cw = max(130, s)
        name_y = s // 2 + r + 2
        ch = name_y + round(20 * self.ls) + 2 + round(26 * self.ls) + 6
        return (w["cx"] - cw // 2, w["cy"] - s // 2, cw, ch)

    def slider(self, w):
        sq = max(w["w"], w["h"])
        cw = w.get("cw", max(130, sq))
        ch = (sq - w["h"]) // 2 + w["h"] + 2 + 20 + 2 + 26 + 6
        return (w["cx"] - cw // 2, w["cy"] - sq // 2, cw, ch)

    def text_width(self, s):   # shadow_skin text_width(): sizes a button
        if self.font_label:
            return int(ttf_width(self.font_label, round(9 * 1.15 * 1.6), s) * 1.2)
        return int(len(s) * 10 * 1.15 - 1.15)

    def button(self, w):   # shadow_skin button_rect()
        bw, bh = self.text_width(w["label"]) + 36, 39
        if self.td3:
            bw, bh = bw + 24 + 4, 48 + 4
        return (w["cx"] - bw // 2, w["cy"] - bh // 2, bw, bh)

    @staticmethod
    def segs(w, n):   # shadow_skin seg_rects()
        if w["kind"] == "enum_v":
            sw = w.get("sw") or 135
            y0 = w["cy"] - (n * SEG_V_STEP) // 2
            return [(w["cx"] - sw // 2, y0 + i * SEG_V_STEP, sw, SEG_V_H) for i in range(n)]
        sw, rows = w.get("sw") or 117, w.get("rows", 1)
        per = -(-n // rows)
        out = []
        for i in range(n):
            r, c = divmod(i, per)
            cnt = min(per, n - r * per)
            out.append((w["cx"] - (cnt * sw + (cnt - 1) * SEG_GAP) // 2 + c * (sw + SEG_GAP),
                        w["cy"] - SEG_H_H // 2 + r * SEG_H_STEP, sw, SEG_H_H))
        return out

    @staticmethod
    def enum_label(w, n):   # the TrueType group label shadow_skin draws centred at (gx, gy), 18 px
        gy = (w["cy"] - SEG_H_H // 2 - SEG_H_LABEL if w["kind"] == "enum_h"
              else w["cy"] - (n * SEG_V_STEP) // 2 - SEG_V_LABEL)
        tw = int(ttf_width(TITLE_FONT, 18, w["label"])) + 2
        return (w["cx"] - tw // 2, gy - 10, tw, 20)

    @staticmethod
    def text(w):   # render_conf_preview.c draw_text_c(): cx centres, cy is the TOP of the glyphs
        size = float(w.get("size", TEXT_SIZE))
        tw = bitmap_width(w["label"], size)
        return (w["cx"] - tw // 2, w["cy"], tw + 1, int(9 * size + 0.5))

    @staticmethod
    def popup_panel(w, n):   # shadow_skin popup_layout(): the open list
        fx, fy, fw, fh = w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["w"], w["h"]
        below, above = Y1 - (fy + fh + 4), fy - 4 - Y0
        groups = [(t, int(c)) for t, _, c in (g.rpartition(":") for g in w["groups"].split(","))] \
            if w.get("groups") else None
        if groups:
            rows = min(POP_GROUP_ROWS, max(c for _, c in groups))
            cols = sum(-(-c // rows) for _, c in groups)
            ph = (rows + 1) * (POP_ROW + POP_GAP) - POP_GAP + 2 * POP_PAD
        else:
            for cols in ([int(w["cols"])] if w.get("cols") else range(1, n + 1)):
                rows = -(-n // cols)
                ph = rows * (POP_ROW + POP_GAP) - POP_GAP + 2 * POP_PAD
                if ph <= max(below, above):
                    break
        pw = cols * fw + (cols - 1) * POP_GAP + 2 * POP_PAD
        py = fy + fh + 4 if ph <= below else fy - 4 - ph if ph <= above else Y0
        return (max(0, min(fx, X1 - pw)), py, pw, ph)

    def rects(self, w, params):
        """[(x, y, w, h)] of a control, as shadow_skin places it (stepper: arrows and text together)."""
        k = w["kind"]
        if k == "knob":
            return [self.knob(w)]
        if k in ("slider_v", "slider_h"):
            return [self.slider(w)]
        if k == "toggle":
            return [(w["cx"] - 60, w["cy"] - 18, 120, 58)]
        if k == "button":
            return [self.button(w)]
        if k in ("readout", "stepper", "popup", "menu"):
            return [(w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["w"], w["h"])]
        if k == "list":
            tw = (w["w"] - (w["cols"] - 1) * w["gap"]) // w["cols"]
            return [(w["x"] + c * (tw + w["gap"]), w["y"] + r * (w["th"] + w["gap"]), tw, w["th"])
                    for r in range(w["rows"]) for c in range(w["cols"])]
        if k in ("enum_h", "enum_v"):
            return self.segs(w, len(params[w["key"]]["options"]))
        if k == "meter":   # shadow_skin: a square of the larger side, centred (transparent padding)
            sq = max(w["w"], w["h"])
            return [(w["cx"] - sq // 2, w["cy"] - sq // 2, sq, sq)]
        return []


def _overlap(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def _inside(r):
    return r[0] >= X0 and r[1] >= Y0 and r[0] + r[2] <= X1 and r[1] + r[3] <= Y1


def _same_screen(m1, m2):
    return m1 is None or m2 is None or m1 == m2


def check_names(layout_tabs, geo, errors):
    """Parameter names: MPC shows them under knobs and sliders and in its Q-Link overlay, without page context."""
    seen = {}
    for p in P:
        if len(p["name"]) > 24:
            errors.append("parameter %s: name %r is longer than 24 characters" % (p["key"], p["name"]))
        if p["name"].lower() in seen:
            errors.append("parameters %s and %s have the same name %r" % (seen[p["name"].lower()], p["key"], p["name"]))
        seen[p["name"].lower()] = p["key"]
    for tab in layout_tabs:
        for w in tab["widgets"]:
            if w["kind"] not in ("knob", "slider_v", "slider_h", "toggle") or w.get("key") not in PARAMS:
                continue
            name = PARAMS[w["key"]]["name"]
            if len(name) > NAME_MAX:
                errors.append("%s: %s %s: name %r is longer than %d characters" % (tab["name"], w["kind"], w["key"], name,
                                                                                    NAME_MAX))
            # the live Name label: knob 17 x label_scale px in max(130, 2r+10); slider 17 px in cw; toggle 15 px in 120
            px, box = ((math.ceil(17 * geo.ls), max(130, 2 * w["r"] + 10)) if w["kind"] == "knob" else
                       (15, 120) if w["kind"] == "toggle" else (17, w.get("cw") or max(130, w["w"], w["h"])))
            if text_w(LIVE_FONT, px, name) + TEXT_MARGIN > box - 4:
                errors.append("%s: %s %s: name %r does not fit its %d px label (%.1f px and %d to spare)" % (
                    tab["name"], w["kind"], w["key"], name, box, text_w(LIVE_FONT, px, name), TEXT_MARGIN))


READOUT_INSET = 8                         # shadow_skin's readout: its live text 8 px in from each side, at VALUE_PX
STATUS_TEXT = re.compile(r"^[ -~]+$")     # printable ASCII: what MPC's font and text_w()'s advance table both have


def status_box(tabs):
    """The width the status line's text has on its narrowest page: what a help line or a preset's description
    must fit, wherever it shows."""
    return min(w["w"] for t in tabs for w in t["widgets"] if w["kind"] == "readout" and w.get("key") == "status") \
        - 2 * READOUT_INSET


def status_misfit(line, box):
    """Why `line` can't be the status line's text (None: it can): plain ASCII, no spaces at the ends, and room to
    spare in its box."""
    if not STATUS_TEXT.match(line) or line != line.strip():
        return "is not one line of plain ASCII without spaces at the ends"
    w = text_w(LIVE_FONT, VALUE_PX, line)
    if w + TEXT_MARGIN > box:
        return "is %.1f px; the status line has %d px, %d to spare" % (w, box, TEXT_MARGIN)
    return None


def check_help(tabs, errors):
    """The help lines (surface.py "help"): every control a hand moves has one and nothing else does, and each fits
    the status line on every page, which every page has."""
    bare = ["%s: no status line (the help lines and preset descriptions show there)" % t["name"] for t in tabs
            if not any(w["kind"] == "readout" and w.get("key") == "status" for w in t["widgets"])]
    errors += bare
    if bare:
        return
    box = status_box(tabs)
    for p in P:
        if p["kind"] in HELP_KINDS and not p.get("help"):
            errors.append("parameter %s: a %s needs a help line (help=...)" % (p["key"], p["kind"]))
        elif p["kind"] not in HELP_KINDS and p.get("help"):
            errors.append("parameter %s: a %s shows no help line" % (p["key"], p["kind"]))
        elif p.get("help"):
            why = status_misfit(help_line(p), box)
            if why:
                errors.append("parameter %s: help %r %s" % (p["key"], help_line(p), why))


def check_layout(text, groups):
    """Raise SystemExit on anything shadow_skin.py would refuse, plus geometry mistakes: outside the plugin
    area, overlaps on one screen (a page mode with everything shown in every mode), controls or text in a
    card's title band, open popup lists that leave the plugin area, options too long for their popup field or
    segment, unknown bitmap glyphs; the page groups (Layout): every page in one group, in layout order, with
    exactly one Q-Link set titled like the page; and the help lines (check_help)."""
    params = PARAMS
    geo = Geometry(_top_level(text))
    errors = []
    tabs = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or (not tabs and "=" in line and not line.startswith("[")):
            continue
        m = re.match(r"\[tab (.+)\]$", line)
        if m:
            tabs.append({"name": m.group(1), "widgets": [], "qlinks": []})
            continue
        if line.startswith("qlinks"):
            m = re.match(r'qlinks\s+"([^"]+)"\s*=\s*(.+)$', line)
            keys = [k.strip() for k in m.group(2).split(",") if k.strip()]
            tabs[-1]["qlinks"].append((m.group(1), keys))
            continue
        tabs[-1]["widgets"].append(_widget(line))
    if len(groups) > 7:
        errors.append("%d page groups: MPC's tab strip shows five plus a pager; keep it to seven" % len(groups))
    errors += ["page group %s has no pages" % g["name"] for g in groups if not g["pages"]]
    grouped, names = [n for g in groups for n in g["pages"]], [t["name"] for t in tabs]
    if grouped != names:
        errors.append("the page groups list %s, the layout has the pages %s" % (grouped, names))
    errors += ["page %r: the name is taken (MPC tells pages apart by it)" % n for n in sorted(set(names))
               if names.count(n) > 1]
    for t in tabs:
        if [title for title, _ in t["qlinks"]] != [t["name"]]:
            errors.append("%s: a page has exactly one Q-Link set, titled like the page" % t["name"])
    check_names(tabs, geo, errors)
    check_help(tabs, errors)
    errors += ["PRIMARY_BUTTONS: %r is not a button parameter" % k for k in PRIMARY_BUTTONS
               if PARAMS.get(k, {}).get("kind") != "button"]
    errors += ["BIPOLAR_EXTRA: %r is not a parameter" % k for k in BIPOLAR_EXTRA if k not in PARAMS]
    seg_images = {}   # shadow_skin names enum images sh_seg_<key>_<n> for the whole skin: one size per key
    for tab in tabs:
        T = tab["name"]
        frames, placed = [], []   # (rect, title, mode); (rect, what, mode)
        for w in tab["widgets"]:
            kind, key, mode = w["kind"], w.get("key"), w.get("when")
            if mode:
                mk, _, mo = mode.partition(":")
                opts = [o.lower() for o in params.get(mk, {}).get("options", [])]
                if len(opts) < 2 or mo.lower() not in opts:
                    errors.append("%s: when=%s is not an option of an option parameter" % (T, mode))
            if kind == "frame":
                r = (w["x"], w["y"], w["w"], w["h"])
                if not _inside(r):
                    errors.append("%s: frame %r at %s leaves the plugin area" % (T, w.get("title"), r))
                frames.append((r, w.get("title", ""), mode))
                continue
            if kind == "text":
                lab = w.get("label", "")
                bad = sorted(set(c for c in lab if c not in BITMAP_GLYPHS))
                if not lab or bad:
                    errors.append("%s: text %r: %s" % (T, lab, "the bitmap font has no %r" % "".join(bad) if bad
                                                       else "an empty label fails shadow_art"))
                placed.append((Geometry.text(w), "text %r" % lab, mode))
                continue
            if kind == "art":
                errors.append("%s: art needs the browser renderer" % T)
                continue
            need = ["%s_%d" % (key, i + 1) for i in range(w["cols"] * w["rows"])] if kind == "list" else [key]
            if kind == "stepper":
                need += [key + "_prev", key + "_next"]
            if kind == "popup":
                need.append(key + "__open")
            missing = [k for k in need if k not in params]
            for k in missing:
                errors.append("%s: %s key %r is not a parameter" % (T, kind, k))
            if missing:
                continue
            p = params[need[0]]
            if kind in ("enum_h", "enum_v", "popup") and "options" not in p:
                errors.append("%s: %s %r is not an option parameter" % (T, kind, key))
                continue
            if kind == "list" and p["kind"] != "tile":
                errors.append("%s: list %r tiles must be tile parameters" % (T, key))
            if kind == "stepper" and p["kind"] != "stepper":
                errors.append("%s: stepper %r is not a stepper parameter" % (T, key))
            if kind == "meter" and p["kind"] != "meter":
                errors.append("%s: meter %r is not a meter parameter" % (T, key))
            if kind == "button" and not w.get("label"):
                errors.append("%s: button %r needs a label" % (T, key))
                continue
            strip = {"knob": lambda: 2 * w["r"] + 10, "slider_v": lambda: max(w["w"], w["h"]),
                     "slider_h": lambda: max(w["w"], w["h"]), "meter": lambda: max(w["w"], w["h"])}.get(kind)
            if strip and strip() * FRAMES > MAX_IMAGE_H:   # FRAMES square frames stacked: one tall image
                errors.append("%s: %s %s: its filmstrip is %d px tall; MPC draws images over %d px wrongly" % (
                    T, kind, key, strip() * FRAMES, MAX_IMAGE_H))
            if kind == "knob":
                style = KNOB_STYLES.get(w["r"])
                if not style:
                    errors.append("%s: knob %s: r=%d has no look in KNOB_STYLES" % (T, key, w["r"]))
                elif style["bipolar"] != bipolar(key):
                    errors.append("%s: knob %s: r=%d is a %s look, the parameter is %s" % (
                        T, key, w["r"], "bipolar" if style["bipolar"] else "unipolar",
                        "bipolar" if bipolar(key) else "unipolar"))
            if kind in ("readout", "stepper", "popup") and w["h"] < 36:
                errors.append("%s: %s %s: h=%d clips its 26 px live text (min 36)" % (T, kind, key, w["h"]))
            if kind in ("enum_h", "enum_v"):
                n = len(p["options"])
                size = (kind, w.get("sw"), n)
                if seg_images.setdefault(key, size) != size:
                    errors.append("%s: enum %s is drawn as %s and %s: its segment images are shared" % (
                        T, key, seg_images[key], size))
                if w.get("label"):
                    placed.append((Geometry.enum_label(w, n), "%s label" % key, mode))
            if kind == "popup":
                panel = Geometry.popup_panel(w, len(p["options"]))
                if not _inside(panel):
                    errors.append("%s: popup %s: its open list %s leaves the plugin area" % (T, key, panel))
                # The field shows the option in MPC's value text, beside the chevron.
                for o in p["options"]:
                    if text_w(LIVE_FONT, VALUE_PX, o) + TEXT_MARGIN > w["w"] - POPUP_CHEVRON:
                        errors.append("%s: popup %s: %r does not fit its %d px field" % (T, key, o, w["w"]))
            if kind in ("enum_h", "enum_v"):
                # Segment text: the option in capitals (shadow_skin's label overlays). Capitals: "m3" and "M3"
                # would both read "M3".
                sw = w.get("sw") or (135 if kind == "enum_v" else 117)
                shown = [o.upper() for o in p["options"]]
                for o in shown:
                    if text_w(TITLE_FONT, seg_px(kind), o) + TEXT_MARGIN > sw - SEG_PAD:
                        errors.append("%s: %s %s: %r does not fit its %d px segment" % (T, kind, key, o, sw))
                if len(set(shown)) < len(shown):
                    errors.append("%s: %s %s: options that differ only in case read the same in capitals" % (T, kind, key))
            for r in geo.rects(w, params):
                placed.append((r, "%s %s" % (kind, key), mode))
        for i, (r, what, mode) in enumerate(placed):
            if not _inside(r):
                errors.append("%s: %s at %s leaves the plugin area" % (T, what, r))
            for o_r, o_what, o_mode in placed[:i]:
                if what.startswith("meter ") and o_what.startswith("meter "):
                    continue   # a row of meters: their padded squares overlap, transparent and untouchable
                if _same_screen(mode, o_mode) and _overlap(r, o_r) and o_what != what:
                    errors.append("%s: %s overlaps %s" % (T, what, o_what))
            for f_r, title, f_mode in frames:
                band = (f_r[0], f_r[1], f_r[2], TITLE_BAND)
                if _same_screen(mode, f_mode) and _overlap(r, band):
                    errors.append("%s: %s at %s is in the title band of card %r" % (T, what, r, title))
        titles = [t for t, _ in tab["qlinks"]]
        for title, keys in tab["qlinks"]:
            if len(keys) > 16:
                errors.append("%s: qlinks %r has %d keys (max 16)" % (T, title, len(keys)))
            if len(title) > 12 or titles.count(title) > 1:
                errors.append("%s: qlinks title %r: keep it unique and at most 12 characters (MPC's tab strip)" % (T, title))
            for k in keys:
                if k not in params:
                    errors.append("%s: qlinks %r key %r is not a parameter" % (T, title, k))
    if errors:
        raise SystemExit("layout check failed:\n  " + "\n  ".join(errors))
    return tabs


# --- C++ header --------------------------------------------------------------------------------
CURVE = {"readout": "Readout", "enum": "Enum", "lin": "Lin", "log": "Log", "int": "Int", "pow": "Pow"}
FMT = {"none": "None", "enum": "Enum", "pct": "Percent", "bipct": "Bipolar", "hz": "Hz", "time": "Time",
       "semi": "Semi", "count": "Count", "db": "Db", "text": "Text", "lfohz": "LfoHz", "period": "Period",
       "hz2": "Hz2", "cents": "Cents", "oct": "Oct", "ms": "Ms", "pan": "Pan"}
KIND = {"synth": "Synth", "ui": "Ui", "readout": "Readout", "stepper": "Stepper", "button": "Button",
        "tile": "Tile", "toggle": "Toggle", "popup": "Popup", "meter": "Meter"}


def c_str(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def header():
    index = {p["key"]: i for i, p in enumerate(P)}
    ids = ",\n".join("    P_%s%s" % (p["key"].upper(), " = 0" if i == 0 else "") for i, p in enumerate(P))
    specs = ",\n".join("    {Curve::%s, Fmt::%s, %sf, %sf}  /* %s */" % (
        CURVE[p["curve"]], FMT[p["fmt"]], float(p["lo"]), float(p["hi"]), p["key"]) for p in P)
    opts = []
    for i, p in enumerate(P):
        if "options" in p:
            opts.append("static constexpr const char* OPTS_%d[] = {%s};" % (i, ", ".join(c_str(o) for o in p["options"])))
    info = ",\n".join("    {%s, %s, Kind::%s, %rf, %d, %s, %d,\n     %s}" % (
        c_str(p["key"]), c_str(p["name"]), KIND[p["kind"]], float(round(norm(p), 6)),
        len(p.get("options", [])), "OPTS_%d" % i if "options" in p else "nullptr",
        index[p["popup_of"]] if p["kind"] == "popup" else -1,
        c_str(help_line(p)) if p.get("help") else "nullptr") for i, p in enumerate(P))
    uid = int.from_bytes(VST["uid"].encode(), "big")
    return """// generated by surface/surface.py: do not edit
#pragma once
#include <cstdint>

namespace af {

enum ParamId : int {
%s,
    P_COUNT
};

enum class Curve : unsigned char { Readout, Enum, Lin, Log, Int, Pow };
enum class Fmt : unsigned char { %s };
// Who owns the value and what a set does: see surface.py "kind".
enum class Kind : unsigned char { Synth, Ui, Readout, Stepper, Button, Tile, Toggle, Popup, Meter };

struct ParamSpec { Curve curve; Fmt fmt; float lo, hi; };
struct ParamInfo {
    const char* key;
    const char* name;
    Kind kind;
    float def;                  // MPC's 0..1 default
    int nopts;
    const char* const* opts;
    int popupOf;                // Kind::Popup: the parameter whose list it opens, else -1
    const char* help;           // the status line after a move: "NAME: what it does" (nullptr: none)
};

static constexpr ParamSpec PARAM_SPECS[P_COUNT] = {
%s
};

%s

static constexpr ParamInfo PARAM_INFO[P_COUNT] = {
%s
};

constexpr const char* kPlugName = %s;
constexpr const char* kPlugVendor = %s;
constexpr int32_t kPlugUid = 0x%08x;   // '%s'
constexpr int32_t kPlugVersion = %d;

constexpr int kStepperRange = %d;
constexpr int kBrowserCats = %d;
constexpr int kBrowserItems = %d;

} // namespace af
""" % (ids, ", ".join(dict.fromkeys(FMT.values())), specs, "\n".join(opts), info, c_str(VST["name"]),
       c_str(VST["vendor"]), uid, VST["uid"], VST["version"], STEPPER_RANGE, BROWSER_CATS, BROWSER_ITEMS)


# --- factory presets: presets/Factory/<NN_Category>/<NN_Name>.afp, embedded in the .so ---------
PRESET_DIR = os.path.join(HERE, "..", "presets", "Factory")
PRESET_MAGIC = "ambientforce "
NUMBER = re.compile(r"^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$")
PRESET_NAME_MAX = 18      # an item tile on the browser page (816 px / 3 columns)
CATEGORY_NAME_MAX = 12    # a category tile (320 px / 2 columns), shown in capitals
PRESET_ABOUT = "about"    # a preset's one-line description, shown on the status line when it loads
MACROS = ("m_horizon", "m_motion", "m_glow", "m_density")


def _shown(entry):
    """"02_Low_Tide_Hum.afp" -> "Low Tide Hum", "02_Drones" -> "Drones"."""
    return re.sub(r"^\d+\s+", "", re.sub(r"\.afp$", "", entry).replace("_", " "))


def about_line(name, about):
    """What the status line shows when a preset loads: "NAME: its description" (plugin/surface.cpp loadPreset)."""
    return "%s: %s" % (name.upper(), about)


def factory_presets(status_w):
    """[(category, name, text)]: one folder per category, both in file order ("NN_" orders them, "_" shows as a
    space). Every line must be a sound parameter with a value in range (the macros at 0: they bend a preset as
    saved), every name unique (keys are "builtin:<name>") and short enough for its tile, and the description
    (an optional about= line) short enough for the status line (status_w px, status_box()): a typo fails the
    build, not the device."""
    params = {p["key"]: p for p in P}
    out, errors, seen = [], [], {}
    for d in sorted(os.listdir(PRESET_DIR)):
        folder = os.path.join(PRESET_DIR, d)
        if d.endswith(".afp"):
            errors.append("%s: put it in a category folder (presets/Factory/NN_Category/)" % d)
            continue
        if not os.path.isdir(folder):
            continue
        category = _shown(d)
        if not category or len(category) > CATEGORY_NAME_MAX:
            errors.append("%s: a category name of 1..%d characters" % (d, CATEGORY_NAME_MAX))
        for f in sorted(os.listdir(folder)):
            if not f.endswith(".afp"):
                continue
            where = "%s/%s" % (d, f)
            text = open(os.path.join(folder, f), encoding="utf-8").read().replace("\r\n", "\n")
            lines = text.split("\n")
            if lines[0] != PRESET_MAGIC + "1":
                errors.append("%s: the first line must be '%s1'" % (where, PRESET_MAGIC))
            keys = set()
            name = _shown(f)
            for n, line in enumerate(lines[1:], 2):
                if not line.strip():
                    continue
                key, _, val = line.partition("=")
                if key == PRESET_ABOUT:   # the description: not a parameter (plugin/presets.cpp presetAbout)
                    why = status_misfit(about_line(name, val), status_w) if val else "is empty"
                    if key in keys:
                        errors.append("%s:%d: about given twice" % (where, n))
                    elif why:
                        errors.append("%s:%d: the status line %r %s" % (where, n, about_line(name, val), why))
                    keys.add(key)
                    continue
                p = params.get(key)
                if not p or p["kind"] != "synth":
                    errors.append("%s:%d: %r is not a sound parameter" % (where, n, key))
                    continue
                if key in keys:
                    errors.append("%s:%d: %s given twice" % (where, n, key))
                keys.add(key)
                if not NUMBER.match(val):   # what the plugin's parser (std::from_chars) reads, no more
                    errors.append("%s:%d: %r is not a number" % (where, n, val))
                    continue
                v = float(val)
                lo, hi = p["lo"], p["hi"]
                if not (min(lo, hi) - 1e-9 <= v <= max(lo, hi) + 1e-9):
                    errors.append("%s:%d: %s=%s outside %s..%s" % (where, n, key, val, lo, hi))
                if p["curve"] in ("int", "enum") and v != round(v):
                    errors.append("%s:%d: %s=%s is not a whole number" % (where, n, key, val))
                if key in MACROS and v != 0:
                    errors.append("%s:%d: %s=%s: a factory preset keeps the macros at 0" % (where, n, key, val))
            if name in seen:
                errors.append("%s: the name %r is taken by %s" % (where, name, seen[name]))
            seen[name] = where
            if len(name) > PRESET_NAME_MAX:
                errors.append("%s: %r is longer than %d characters" % (where, name, PRESET_NAME_MAX))
            out.append((category, name, text))
    if "Init" not in seen:
        errors.append("no Init preset (the INIT button loads builtin:Init)")
    if errors:
        raise SystemExit("factory presets:\n  " + "\n  ".join(errors))
    return out


def presets_header(presets):
    rows = ",\n".join("    {%s, %s, %s}" % (c_str(c), c_str(n), c_str(t).replace("\n", "\\n")) for c, n, t in presets)
    return """// generated by surface/surface.py from presets/Factory/*/*.afp: do not edit
#pragma once

namespace af {

struct FactoryPreset { const char* category; const char* name; const char* text; };
static const FactoryPreset kFactoryPresets[] = {
%s
};
constexpr int kNumFactoryPresets = %d;

} // namespace af
""" % (rows, len(presets))


def main():
    keys = [p["key"] for p in P]
    assert len(set(keys)) == len(keys), "duplicate parameter key"
    assert P[0]["kind"] == "readout", "parameter 0 must stay a read-only readout"
    layout = pages()
    tabs = check_layout(layout, build_layout().groups)
    presets = factory_presets(status_box(tabs))   # everything checked before anything is written
    outputs = [
        ("params.json", json.dumps(params_json(), indent=1)),
        ("layout.conf", layout),
        ("vst.json", json.dumps(VST, indent=1)),
        (os.path.join("build", "skin_style.json"), json.dumps(skin_style(), indent=1)),
        (os.path.join("build", "factory_presets.h"), presets_header(presets)),
        (os.path.join("build", "param_ids.h"), header()),   # last: make's target, newer than the rest
    ]
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    for name, text in outputs:
        path = os.path.join(HERE, name)
        with open(path + ".tmp", "w", newline="\n") as f:
            f.write(text)
        os.replace(path + ".tmp", path)
    print("surface: %d parameters, layout ok, %d factory presets" % (len(P), len(presets)))


if __name__ == "__main__":
    sys.exit(main())
