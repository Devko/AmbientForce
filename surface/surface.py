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
                     curves, names, options and defaults (the C++ never reads gen_vst's params.h,
                     so `make test` and the .so build need only Python, not the skin toolchain)
  build/factory_presets.h  the factory presets (presets/Factory/*/*.afp), checked and embedded
  build/skin_style.json  the palette, knob looks, primary buttons and page groups for
                     skin_polish.py, which `make skin` runs after the generator

Continuous parameters are declared to MPC as 0..1: the real range and curve (log Hz, log
seconds, ...) live in param_ids.h, and the plugin formats every value text itself, so the
knob, its label and the DSP can never disagree.

Before writing anything the layout is checked the way shadow_skin.py would (unknown keys,
option counts, when=, Q-Link sets) plus geometry with shadow_skin's own sizes (inside the plugin
area, no overlaps within a page, nothing in a card's title band, open popup lists inside the
plugin area, bitmap-font glyphs) and the parameter names MPC shows (short, unique), so a broken
page fails here instead of on the device. The layout machinery and its checks are PolyForce's; the
page groups (several pages under one tab) are EffectForce's.

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
P = []


def _add(key, name, kind, curve, lo, hi, default, fmt, **extra):
    d = dict(key=key, name=name, kind=kind, curve=curve, lo=lo, hi=hi, default=default, fmt=fmt)
    d.update(extra)
    P.append(d)


def readout(key, name):
    _add(key, name, "readout", "readout", 0, 0, 0, "none")


def meter_param(key, name):
    _add(key, name, "meter", "lin", 0, 1, 0.5, "none")


def enum(key, name, options, default, ui=False):
    _add(key, name, "ui" if ui else "synth", "enum", 0, len(options) - 1, options.index(default), "enum",
         options=options)


def num(key, name, curve, lo, hi, default, fmt, ui=False):
    _add(key, name, "ui" if ui else "synth", curve, lo, hi, default, fmt)


def stepper(key, name):
    _add(key, name, "stepper", "int", 0, STEPPER_RANGE, 0, "text")
    button(key + "_prev", name + " Prev")
    button(key + "_next", name + " Next")


def button(key, name):
    _add(key, name, "button", "int", 0, 1, 0, "none")


def tile(key, name):
    _add(key, name, "tile", "enum", 0, 1, 0, "text", options=["-", "On"])


def toggle(key, name):
    _add(key, name, "toggle", "enum", 0, 1, 0, "enum", options=["Off", "On"])


def popup_flag(of):
    src = next(p for p in P if p["key"] == of)
    _add(of + "__open", "%s List" % src["name"], "popup", "enum", 0, 1, 0, "none", options=["Closed", "Open"],
         popup_of=of)


readout("status", "Status")            # index 0 must stay a read-only readout: MPC sets it at load
num("volume", "Volume", "lin", -60, 6, -6, "db")

# --- presets ---
stepper("preset", "Preset")
button("pre_save", "Save Preset")
button("pre_init", "Init Patch")

# --- the preset browser ---
for i in range(1, BROWSER_CATS + 1):
    tile("cat_%d" % i, "Category %d" % i)
button("cat_prev", "Categories Prev")
button("cat_next", "Categories Next")
for i in range(1, BROWSER_ITEMS + 1):
    tile("item_%d" % i, "Item %d" % i)
button("item_prev", "Items Prev")
button("item_next", "Items Next")
readout("item_page", "Items Page")
readout("br_now", "Loaded")
toggle("fav", "Favorite")
button("rnd", "Random Pick")


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

S8 = [100, 252, 404, 556, 708, 860, 1012, 1164]   # 8 knob slots across a card = one Q-Link bank
L4, R4 = S8[:4], [724, 876, 1028, 1180]           # 4 slots in the left / right half card
R1, R2 = 158, 440                                 # card rows (h=270), or R1 with h=552

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

    def hseg(self, cx, cy, key, sw, label=None):
        self.add('enum_h cx=%d cy=%d sw=%d key=%s%s' % (cx, cy, sw, key, ' label="%s"' % label if label else ""))

    def vseg(self, cx, cy, key, sw=124, label=None):
        self.add('enum_v cx=%d cy=%d sw=%d key=%s%s' % (cx, cy, sw, key, ' label="%s"' % label if label else ""))

    def popup(self, cx, cy, w, key):
        self.add('popup cx=%d cy=%d w=%d h=40 key=%s' % (cx, cy, w, key))

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


def build_layout():
    """Every page, in groups (see Layout). Each page has its own Q-Link set: the Force's 8 knobs show the first 8
    keys, the next bank the other 8 (shadow_skin qlink_for_slot)."""
    L = Layout()

    # BROWSE: categories left, presets right, the loaded preset and actions below. The browser has no knobs of
    # its own: the Q-Links keep the preset stepper and the volume.
    L.group("BROWSE")
    L.page("PRESETS", ["preset", "volume"])
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
    """Advance width of s in a bundled font at px pixels, measured with Pillow as shadow_skin does. Without
    Pillow (surface.py needs only python3): from the font's own advance table, +1 px (that is within 0.75 px
    of Pillow for Titillium Web, and errs wide)."""
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
            y0 = w["cy"] - (n * 32) // 2
            return [(w["cx"] - sw // 2, y0 + i * 32, sw, 30) for i in range(n)]
        sw, rows = w.get("sw") or 117, w.get("rows", 1)
        per = -(-n // rows)
        out = []
        for i in range(n):
            r, c = divmod(i, per)
            cnt = min(per, n - r * per)
            out.append((w["cx"] - (cnt * sw + (cnt - 1) * 2) // 2 + c * (sw + 2), w["cy"] - 16 + r * 35, sw, 33))
        return out

    @staticmethod
    def enum_label(w, n):   # the TrueType group label shadow_skin draws centred at (gx, gy), 18 px
        gy = w["cy"] - 33 // 2 - 22 if w["kind"] == "enum_h" else w["cy"] - (n * 32) // 2 - 24
        tw = int(ttf_width(TITLE_FONT, 18, w["label"])) + 2
        return (w["cx"] - tw // 2, gy - 10, tw, 20)

    @staticmethod
    def text(w):   # render_conf_preview.c draw_text_c(): cx centres, cy is the TOP of the glyphs
        size = float(w.get("size", 1.5))
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
            if ttf_width(LIVE_FONT, px, name) > box - 4:
                errors.append("%s: %s %s: name %r does not fit its %d px label" % (tab["name"], w["kind"], w["key"], name, box))


def check_layout(text, groups):
    """Raise SystemExit on anything shadow_skin.py would refuse, plus geometry mistakes: outside the plugin
    area, overlaps on one screen (a page mode with everything shown in every mode), controls or text in a
    card's title band, open popup lists that leave the plugin area, unknown bitmap glyphs; and the page groups
    (Layout): every page in one group, in layout order, with exactly one Q-Link set titled like the page."""
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
       "semi": "Semi", "count": "Count", "db": "Db", "text": "Text", "lfohz": "LfoHz"}
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
    info = ",\n".join("    {%s, %s, Kind::%s, %rf, %d, %s, %d}" % (
        c_str(p["key"]), c_str(p["name"]), KIND[p["kind"]], float(round(norm(p), 6)),
        len(p.get("options", [])), "OPTS_%d" % i if "options" in p else "nullptr",
        index[p["popup_of"]] if p["kind"] == "popup" else -1) for i, p in enumerate(P))
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


def _shown(entry):
    """"02_Low_Tide_Hum.afp" -> "Low Tide Hum", "02_Drones" -> "Drones"."""
    return re.sub(r"^\d+\s+", "", re.sub(r"\.afp$", "", entry).replace("_", " "))


def factory_presets():
    """[(category, name, text)]: one folder per category, both in file order ("NN_" orders them, "_" shows as a
    space). Every line must be a sound parameter with a value in range, every name unique (keys are
    "builtin:<name>") and short enough for its tile: a typo fails the build, not the device."""
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
            for n, line in enumerate(lines[1:], 2):
                if not line.strip():
                    continue
                key, _, val = line.partition("=")
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
            name = _shown(f)
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
    check_layout(layout, build_layout().groups)
    presets = factory_presets()   # everything checked before anything is written
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
