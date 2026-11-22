#!/usr/bin/env python3
"""Make the hand-written skin of percSeq in skins/.

The HIT, CHANCE, ROW, SETUP and POOL pages come from ui.json through the SDK generator
(tools/gen_ui.py). This script puts the SEQ page (the 16-row roll) in front of them. It is the
scalaSeq SEQ page with fixed rows: same colours, lines and marks. Parameter indices are looked up by
name in build/params.json.

Run from the SDK root after a build (it needs build/params.json), then build again:

    python3 tools/build.py plugins/percSeq
    python3 plugins/percSeq/skin/make_skin.py
    python3 tools/build.py plugins/percSeq
"""
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "common"))
import skin_ui   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
OUT = os.path.join(PROJECT, "skins")

# ---- geometry (must match src/PercSeq.cpp) -------------------------------------------------------
GX, GY, GW, GH = 70, 88, 880, 540      # the roll (1.3: + the old velocity lane; was 476)
ROWS = 16
ROWH = GH / ROWS                        # 33.75
RY, RH = 44, 40                         # ruler
VY, VH = 50, 280                        # 1.3: the lane of the LANE page (velocity or CC bars)
PARK, OFF = 40, 1100                    # right strip for parked lines; x-park offset
NOTEH, NOTCHW, GAPW, BADGEW, NUMW, VELW = 24, 4, 2, 16, 40, 5
RULER_PAD_H = 2628
PANELX = GX + GW + PARK + 8             # 998: the Q-Link panel
QCOLW = 68
FREEX, FREEY, FREEW, FREEH = PANELX, 488, 4 * QCOLW, 50
SHOWN, PIECES, GAPS, STEPLINES, SNAPLINES, GRPLINES, NUMS = 32, 10, 7, 65, 89, 65, 64
BG = "ff1e1d1e"
ZOOMN = 4                                 # Steps/Page 8, 16, 32, 64
TOGX, TOGW = 4, 20                        # row on/off squares
SHADE_C = "a01e1d1e"                      # over an off row (63 % background)
NOTE_C, SEL_C, DARK_C = "d9a64d", "5dcaa5", "1e1d1e"

P = {}   # parameter name -> index (from build/params.json)


def prm(name):
    return f"Parameter {P[name]}"


def named(label, name):
    """A Name label shows this text (the MPC does not draw Text labels on the G2)."""
    return f"WithName[{label}]/{prm(name)}"


def png_pixels(path, w, h, pix):
    raw = b"".join(b"\x00" + b"".join(bytes(pix(x, y)) for x in range(w)) for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


DIGITS = {  # 3 x 5 pixel font
    "0": ["111", "101", "101", "101", "111"], "1": ["010", "110", "010", "010", "111"],
    "2": ["111", "001", "111", "100", "111"], "3": ["111", "001", "111", "001", "111"],
    "4": ["101", "101", "111", "001", "001"], "5": ["111", "100", "111", "001", "111"],
    "6": ["111", "100", "111", "101", "111"], "7": ["111", "001", "010", "010", "010"],
    "8": ["111", "101", "111", "101", "111"], "9": ["111", "101", "111", "001", "111"]}


def hexc(s):
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4)) + (0xff,)


def bounds(rect, focus="No", show="Show"):
    return {"version": 1, "acceptsHWFocus": focus, "showWhenDataModelInvalid": show,
            "whenVisible": "Always", "boundsType": "Absolute", "bounds": rect}


def entry(name, wtype, data, rect, remap=None):
    return {"version": 2, "componentData": {"version": 1, "name": name, "type": wtype, "data": data},
            "handle remapping": {"version": 1, "map": remap or []}, "bounds": bounds(rect)}


def text_style(h, colour, just="left verticallyCentred", style="Regular"):
    return {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": style, "height": float(h)},
            "colour": colour, "justification": just, "case": "Original"}


def name_label(nm, handle, rect, h, colour, just="left verticallyCentred", style="Regular"):
    return entry(nm, "Label", {"version": 1, "textStyle": text_style(h, colour, just, style), "type": "Name",
                               "handleName": handle}, rect)


def value_label(nm, handle, rect, h, colour="ffffffff", just="left verticallyCentred"):
    return entry(nm, "Label", {"version": 1, "textStyle": text_style(h, colour, just, "SemiBold"), "type": "Value",
                               "handleName": handle}, rect)


def xypad(nm, xh, yh, img, rect):
    return entry(nm, "XYPad", {"version": 1, "xPositionHandle": xh, "yPositionHandle": yh, "image": img}, rect)


def xpark_rect(base_x, w, y, h):
    """A pad for an x-parked marker: value 0 puts the picture off the screen (see xPark in the code)."""
    return f"{base_x - OFF} {y} {OFF + GW + w} {h}"


def image(nm, colour, rect):
    return entry(nm, "Image", {"version": 2, "imageType": "Regular", "colour": colour, "image": ""}, rect)


def row_top(r):
    """Top of row r (row 0 = row 1 at the bottom) in tab pixels."""
    return GY + GH - (r + 1) * ROWH


SHORT = {"Length": "Len", "Entry Len": "Entry", "Sel Row": "Sel", "Sequence": "Seq"}
BANKS = [("HIT", ["Hit", "Row", "Move", "Length"]),
         ("SHAPE", ["VEL", "RATCH", "FADE", "DUTY"]),
         ("ENTRY", ["Entry Len", "Snap", "Sel Row", "Rate"]),
         ("VIEW", ["Page", "Sequence", "Steps", "Nudge"])]


def seq_tab():
    c = []
    ly = prm("Line Y")
    # header: names 24 px, values 30 px
    x = GX
    for label, pname, nw, vw in (("SEQ", "Sequence", 58, 46), ("HIT", "Hit", 54, 80), ("ROW", "Sel Row", 62, 46),
                                 ("LAST PAD", "Last Note", 112, 56), ("PAD 1", "Pad 1 Note", 76, 56)):
        c.append(name_label(f"h_{label}", named(label, pname), f"{x} 2 {nw} 40", 24, "ff8a8885"))
        c.append(value_label(f"v_{label}", prm(pname), f"{x + nw} 2 {vw} 40", 30))
        x += nw + vw + 10
    # 1.1: page buttons < >, the page ("2/4") between them, and FOLLOW (lit amber when on)
    c.append(button("pageprev", prm("Page Prev"), "psq_prev.png", "psq_prev.png", 1, 1, f"{x} 2 40 40"))
    c.append(value_label("v_page", prm("View Text"), f"{x + 42} 2 62 40", 28, "ffffffff", "horizontallyCentred verticallyCentred"))
    c.append(button("pagenext", prm("Page Next"), "psq_next.png", "psq_next.png", 1, 1, f"{x + 106} 2 40 40"))
    c.append(button("follow", prm("Follow"), "psq_followon.png", "psq_followoff.png", 1, 1, f"{x + 152} 2 {PANELX - 6 - (x + 152)} 40"))
    # "more" (hits that are not drawn) at the right end of the ruler
    c.append(value_label("v_more", prm("More Text"), f"{GX + GW - 70} {RY} 66 {RH}", 22, "ffd9a64d", "right verticallyCentred"))
    c.append(name_label("h_entry", named("STEP ENTRY", "Step Entry"), f"{PANELX} 2 170 40", 24, "ff8a8885"))
    c.append(value_label("v_entry", prm("Step Entry"), f"{PANELX + 170} 2 {1276 - PANELX - 170} 40", 30, "ff5dcaa5"))
    # Q-Link panel: the 4 banks as columns (the MPC frames the active bank)
    for b, (title, names) in enumerate(BANKS):
        x = PANELX + b * QCOLW
        c.append(name_label(f"bank{b}", named(title, names[0]), f"{x + 2} {RY} {QCOLW - 4} 28", 20, "ff8a8885"))
        for k, nm in enumerate(names):
            y = RY + 32 + k * 110
            h = named(SHORT[nm], nm) if nm in SHORT else prm(nm)
            c.append(name_label(f"q{b}_{k}_n", h, f"{x + 2} {y} {QCOLW - 4} 30", 22, "ffb4b2a9"))
            c.append(value_label(f"q{b}_{k}_v", prm(nm), f"{x + 2} {y + 32} {QCOLW - 4} 42", 32))
    # roll background and the row lines (fixed: 16 equal rows, row 1 at the bottom)
    c.append(image("rollbg", "ff1a1a1a", f"{GX} {GY} {GW} {GH}"))
    for i in range(ROWS + 1):
        y = GY + GH - i * ROWH
        c.append(image(f"rowline{i}", "ff303030", f"{GX} {min(y, GY + GH - 1):.2f} {GW} 1"))
    # vertical lines behind the hits: snap steps (dim), whole steps (brighter)
    vpad = f"{GX} {GY} {GW + PARK} {GH}"
    for i in range(SNAPLINES):
        c.append(xypad(f"snapline{i + 1}", prm(f"Snap Line {i + 1}"), ly, "psq_snapline.png", vpad))
    for i in range(STEPLINES):
        c.append(xypad(f"stepline{i + 1}", prm(f"Step Line {i + 1}"), ly, "psq_stepline.png", vpad))
    # 1.1: each row's current step as a square behind the hits, one marker per zoom size (8/16/32/64 steps)
    for r in range(ROWS):
        for k in range(ZOOMN):
            w = round(GW / (8 << k))
            # a roll-high pad (like the hit markers); the row's height comes from a constant y value
            yv = max(0.0, min(1.0, ((r + 0.5) * ROWH - int(ROWH) / 2) / (GH - int(ROWH))))
            fixed_y = (f"Fixed/displayRange 0 1 valueRange 0 1 fineIncrement 0 coarseIncrement 0 defaultValue {yv:.5f} "
                       f"displayString '' name '' value {yv:.5f}")
            c.append(xypad(f"cell{r + 1}_{k}", prm(f"Cell X {r + 1}.{k}"), fixed_y, f"psq_cell{k}.png",
                           xpark_rect(GX, w, GY, GH)))
    # hits: exact bodies from pieces of 1 .. 512 px (each picture 1 px wider, so pieces overlap);
    # each hit's marks (start notch, ratchet gaps, chance ring) follow its own body
    for j in range(SHOWN):
        hy = prm(f"Hit Y {j + 1}")
        for k in range(PIECES):
            c.append(xypad(f"h{j + 1}p{k}", prm(f"Piece {j + 1}.{k}"), hy, f"psq_p{k}.png",
                           xpark_rect(GX, (1 << k) + 1, GY, GH)))
        c.append(xypad(f"h{j + 1}notch", prm(f"Notch X {j + 1}"), hy, "psq_notch.png", xpark_rect(GX, NOTCHW, GY, GH)))
        for k in range(GAPS):
            c.append(xypad(f"h{j + 1}gap{k + 1}", prm(f"Gap {j + 1}.{k + 1}"), hy, "psq_gap.png", xpark_rect(GX, GAPW, GY, GH)))
        c.append(xypad(f"h{j + 1}chance", prm(f"Chance X {j + 1}"), hy, "psq_chance.png", xpark_rect(GX, BADGEW, GY, GH)))
    # the selected hit again on top, in its own colour, with its marks above the highlight
    for k in range(PIECES):
        c.append(xypad(f"selp{k}", prm(f"Sel Piece {k}"), prm("Sel Y"), f"psq_sp{k}.png", xpark_rect(GX, (1 << k) + 1, GY, GH)))
    c.append(xypad("selnotch", prm("Sel Notch X"), prm("Sel Y"), "psq_notch.png", xpark_rect(GX, NOTCHW, GY, GH)))
    for k in range(GAPS):
        c.append(xypad(f"selgap{k + 1}", prm(f"Sel Gap {k + 1}"), prm("Sel Y"), "psq_gap.png", xpark_rect(GX, GAPW, GY, GH)))
    c.append(xypad("selchance", prm("Sel Chance X"), prm("Sel Y"), "psq_chance.png", xpark_rect(GX, BADGEW, GY, GH)))
    # row labels left of the roll: "row note"
    for r in range(ROWS):
        c.append(value_label(f"lab{r + 1}", prm(f"Lab {r + 1}"), f"4 {row_top(r):.2f} {GX - 8} {ROWH:.2f}", 18, "ffb4b2a9",
                             "right verticallyCentred"))
    # ruler and its numbers
    c.append(image("ruler", "ff262626", f"{GX} {RY} {GW} {RH}"))
    for m in range(NUMS):
        c.append(xypad(f"num{m + 1}", prm(f"Num X {m + 1}"), ly, f"psq_n{m + 1}.png", xpark_rect(GX, NUMW, RY, RH)))
    # cursor, Group lines, then each row's end bar and playhead (on top of the hits)
    c.append(xypad("cursor", prm("Cursor X"), ly, "psq_cursor.png", vpad))
    for i in range(GRPLINES):
        c.append(xypad(f"grpline{i + 1}", prm(f"Grp Line {i + 1}"), ly, "psq_group.png", vpad))
    for r in range(ROWS):
        rpad = f"{GX} {row_top(r):.2f} {GW + PARK} {ROWH:.2f}"
        c.append(xypad(f"end{r + 1}", prm(f"End X {r + 1}"), ly, "psq_end.png", rpad))
    c.append(image("parkmask", BG, f"{GX + GW + 1} {GY - 4} {PANELX - (GX + GW) - 6} {GH + 8}"))
    # 1.3: the velocity lane moved to the LANE page
    # FREE button: grey, lit while held
    c.append(image("freebg", "ff3a3a3c", f"{FREEX} {FREEY} {FREEW} {FREEH}"))
    c.append(xypad("freelit", prm("Free Lit"), ly, "psq_freelit.png", xpark_rect(FREEX, FREEW, FREEY, FREEH)))
    c.append(name_label("freetext", named("FREE", "Free Lit"), f"{FREEX} {FREEY} {FREEW} {FREEH}", 26, "ffffffff",
                        "horizontallyCentred verticallyCentred", "SemiBold"))
    # chance lanes of the selected hit, 4 lines of 20 px under FREE
    for l in range(4):
        c.append(value_label(f"chance{l + 1}", prm(f"Chance {l + 1}"), f"{FREEX} {FREEY + FREEH + 10 + 20 * l} {FREEW} 20", 20, "ffb4b2a9"))
    # touch guard (see scalaSeq): a fixed XYPad over the page, under the touch surfaces
    fixed = ("Fixed/displayRange 0 1 valueRange 0 1 fineIncrement 0 coarseIncrement 0 defaultValue 0 "
             "displayString '' name '' value 0")
    c.append(xypad("touchguard", fixed, fixed, "psq_none.png", "0 0 1280 628"))
    def touch(nm, t, rect):
        return xypad(nm, f"ResetOnGestureEnd/{prm(f'Touch {t} X')}", f"ResetOnGestureEnd/{prm(f'Touch {t} Y')}", "psq_none.png", rect)
    c.append(touch("t_ruler", "ruler", f"{GX} {-1000} {GW} {RULER_PAD_H}"))
    c.append(touch("t_roll", "roll", f"{GX} {GY} {GW} {GH}"))
    c.append(touch("t_labels", "labels", f"0 {GY} {GX - 4} {GH}"))   # 1.1: a tap on a label switches the row on/off
    c.append(touch("t_free", "free", f"{FREEX} {FREEY} {FREEW} {FREEH}"))
    # 1.1: row on/off. An off row is shaded (label and hits); the toggle square is on top of everything.
    for r in range(ROWS):
        shade = image(f"offshade{r + 1}", SHADE_C, f"0 {row_top(r):.2f} {GX + GW} {ROWH:.2f}")
        shade["bounds"] = {"version": 2, "acceptsHWFocus": "No", "showWhenDataModelInvalid": "Show", "whenVisible": "Always",
                           "boundsType": "Absolute", "bounds": shade["bounds"]["bounds"],
                           "additionalInvalidatingHandles": [f"IndexedEnabling/0/2/{prm(f'On {r + 1}')}"]}
        c.insert(c.index(next(e for e in c if e["componentData"]["name"] == "touchguard")), shade)
    for r in range(ROWS):
        y = row_top(r) + (ROWH - TOGW) / 2
        # the square only shows the state (under the label touch area, which takes the taps)
        c.insert(c.index(next(e for e in c if e["componentData"]["name"] == "touchguard")), entry(f"rowon{r + 1}", "Button", {"version": 2, "onImage": "psq_togon.png", "offImage": "psq_togoff.png",
                                                    "buttonId": 1, "numButtonsInGroup": 1, "handleName": prm(f"On {r + 1}"),
                                                    "gestureBehaviour": "Instant"}, f"{TOGX} {y:.2f} {TOGW} {TOGW}"))
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": BG, "image": ""},
                               "unfocussed": {"version": 1, "colour": BG, "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": c}
    qb = [f"{PANELX + b * QCOLW} {RY - 2} {QCOLW} {32 + 3 * 110 + 78}" for b in range(4)]
    return {"version": 1, "tabName": "SEQ", "fnKeyIndex": 0, "fnKeySubIndex": 0,
            "componentDefinition": root, "qlinkBoundsData": qb}


# ---- POOL page: a 4 x 4 grid of pads laid out like the MPC pads (row 1 bottom left) ---------------
SLOT_X, SLOT_Y, SLOT_W, SLOT_H = [23, 343, 663, 983], [45, 196, 346, 497], 278, 107
PADW, PADH = 270, 100
ON_C, OFF_C = "d9a64d", "3a3a3c"


def button(nm, handle, on_img, off_img, bid, n, rect):
    return entry(nm, "Button", {"version": 2, "onImage": on_img, "offImage": off_img, "buttonId": bid,
                                "numButtonsInGroup": n, "handleName": handle, "gestureBehaviour": "Instant"}, rect)


def pool_tab(fn_index):
    c = []
    for r in range(ROWS):
        x = SLOT_X[r % 4] + (SLOT_W - PADW) // 2
        y = SLOT_Y[3 - r // 4] + (SLOT_H - PADH) // 2
        # 1.0.1: the label under the button (Open MPC: labels on top of buttons swallow taps); the
        # button pictures are frames with a transparent middle, so the text shows through
        c.append(value_label(f"poollab{r + 1}", prm(f"Lab {r + 1}"), f"{x} {y} {PADW} {PADH}", 44, "ffffffff",
                             "horizontallyCentred verticallyCentred"))
        c.append(button(f"pool{r + 1}", prm(f"Row {r + 1}"), "psq_poolon.png", "psq_pooloff.png", 1, 1, f"{x} {y} {PADW} {PADH}"))
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": BG, "image": ""},
                               "unfocussed": {"version": 1, "colour": BG, "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": True, "componentsData": c}
    qb = [f"{SLOT_X[k] - 10} 35 {SLOT_W + 20} 579" for k in range(4)]
    return {"version": 1, "tabName": "POOL", "fnKeyIndex": fn_index, "fnKeySubIndex": 0,
            "componentDefinition": root, "qlinkBoundsData": qb}


# ---- ROW page: a 4 x 4 row selector in the empty 4th column ------------------------------------------
MPADW, MPADH, MGAP = 62, 128, 6


def row_pads():
    c = []
    x0, y0 = SLOT_X[3] + 4, SLOT_Y[0]
    for r in range(ROWS):
        x = x0 + (r % 4) * (MPADW + MGAP)
        y = y0 + (3 - r // 4) * (MPADH + MGAP)
        c.append(name_label(f"rowpadlab{r + 1}", f"WithName[{r + 1}]/{prm('Sel Row')}", f"{x} {y} {MPADW} {MPADH}", 36, "ffffffff",
                            "horizontallyCentred verticallyCentred", "SemiBold"))
        c.append(button(f"rowpad{r + 1}", prm("Sel Row"), "psq_rowon.png", "psq_rowoff.png", r, ROWS, f"{x} {y} {MPADW} {MPADH}"))
    return c


def fine_by_default(tui):
    """Knobs, faders and the data wheel move by the fine step without Shift (as scalaSeq)."""
    tui = tui.replace('"disableCoarseDataWheel": false', '"disableCoarseDataWheel": true')
    return re.sub(r'("key": "Data",\s*"value": ")(Parameter \d+")', r'\1DisableCoarseIncrement/\2', tui)


def number_png(path, n):
    """A step number, left-aligned, 3 x 5 pixel font at scale 3 (15 px high)."""
    text, scale, w, h = str(n), 3, NUMW, 17
    lit = set()
    for i, ch in enumerate(text):
        for ry, row in enumerate(DIGITS[ch]):
            for rx, bit in enumerate(row):
                if bit == "1":
                    for dy in range(scale):
                        for dx in range(scale):
                            lit.add((i * 4 * scale + rx * scale + dx, 1 + ry * scale + dy))
    png_pixels(path, w, h, lambda x, y: hexc("b4b2a9") if (x, y) in lit else (0, 0, 0, 0))


def pictures():
    rh = int(ROWH)
    for k in range(PIECES):
        png_pixels(os.path.join(OUT, f"psq_p{k}.png"), (1 << k) + 1, NOTEH, lambda x, y: hexc(NOTE_C))
        png_pixels(os.path.join(OUT, f"psq_sp{k}.png"), (1 << k) + 1, NOTEH, lambda x, y: hexc(SEL_C))
    png_pixels(os.path.join(OUT, "psq_notch.png"), NOTCHW, NOTEH, lambda x, y: hexc(DARK_C))
    png_pixels(os.path.join(OUT, "psq_gap.png"), GAPW, NOTEH, lambda x, y: hexc(DARK_C))

    def ring(x, y):
        r = math.hypot(x + 0.5 - BADGEW / 2, y + 0.5 - NOTEH / 2)
        return hexc(DARK_C) if 4.5 <= r <= 7 else (0, 0, 0, 0)
    png_pixels(os.path.join(OUT, "psq_chance.png"), BADGEW, NOTEH, ring)
    png_pixels(os.path.join(OUT, "psq_vel.png"), VELW, VH, lambda x, y: hexc(NOTE_C))
    for k in range(ZOOMN):   # 1.3: CC lane bars (amber), one width per Steps/Page size
        png_pixels(os.path.join(OUT, f"psq_lnplay{k}.png"), round(GW / (8 << k)), VH, lambda x, y: (0xff, 0xff, 0xff, 0x40))
        png_pixels(os.path.join(OUT, f"psq_lnbar{k}.png"), max(2, int(GW / (8 << k)) - 2), VH, lambda x, y: hexc(NOTE_C))
    for i, nm in enumerate(("VEL", "CC 1", "CC 2")):
        skin_ui.text_png(os.path.join(OUT, f"psq_lane{i}_on.png"), 90, 40, nm, skin_ui.ON_C, skin_ui.DARK_C, 24)
        skin_ui.text_png(os.path.join(OUT, f"psq_lane{i}_off.png"), 90, 40, nm, skin_ui.OFF_C, (0xe4, 0xe4, 0xe6), 24)
    skin_ui.text_png(os.path.join(OUT, "psq_send_on.png"), 90, 40, "SEND", skin_ui.ON_C, skin_ui.DARK_C, 24)
    skin_ui.text_png(os.path.join(OUT, "psq_send_off.png"), 90, 40, "SEND", skin_ui.OFF_C, (0xe4, 0xe4, 0xe6), 24)
    png_pixels(os.path.join(OUT, "psq_svel.png"), VELW, VH, lambda x, y: hexc(SEL_C))
    for n in range(1, NUMS + 1):
        number_png(os.path.join(OUT, f"psq_n{n}.png"), n)
    png_pixels(os.path.join(OUT, "psq_snapline.png"), 1, GH, lambda x, y: hexc("333333"))
    png_pixels(os.path.join(OUT, "psq_stepline.png"), 1, GH, lambda x, y: hexc("4a4a4a"))
    png_pixels(os.path.join(OUT, "psq_group.png"), 2, GH, lambda x, y: hexc("6a6a6c"))
    png_pixels(os.path.join(OUT, "psq_end.png"), 3, rh, lambda x, y: hexc("8a8885"))
    for k in range(ZOOMN):   # the current step: white at 25 %
        png_pixels(os.path.join(OUT, f"psq_cell{k}.png"), round(GW / (8 << k)), rh, lambda x, y: (0xff, 0xff, 0xff, 0x40))
    skin_ui.text_png(os.path.join(OUT, "psq_prev.png"), 40, 40, "<", skin_ui.OFF_C, (0xe4, 0xe4, 0xe6), 26)
    skin_ui.text_png(os.path.join(OUT, "psq_next.png"), 40, 40, ">", skin_ui.OFF_C, (0xe4, 0xe4, 0xe6), 26)
    fw = PANELX - 6 - (GX + 114 + 144 + 118 + 178 + 142 + 152)
    skin_ui.text_png(os.path.join(OUT, "psq_followon.png"), fw, 40, "FOLLOW", skin_ui.ON_C, skin_ui.DARK_C, 18)
    skin_ui.text_png(os.path.join(OUT, "psq_followoff.png"), fw, 40, "FOLLOW", skin_ui.OFF_C, (0xe4, 0xe4, 0xe6), 18)
    png_pixels(os.path.join(OUT, "psq_togon.png"), TOGW, TOGW, lambda x, y: hexc(ON_C))
    png_pixels(os.path.join(OUT, "psq_togoff.png"), TOGW, TOGW, lambda x, y: hexc(OFF_C))
    png_pixels(os.path.join(OUT, "psq_cursor.png"), 13, GH, lambda x, y: (0x37, 0x8a, 0xdd, 0x60))
    png_pixels(os.path.join(OUT, "psq_freelit.png"), FREEW, FREEH, lambda x, y: hexc("6a6a6c"))
    png_pixels(os.path.join(OUT, "psq_none.png"), 2, 2, lambda x, y: (0, 0, 0, 0))
    def frame(w, h, col, t=6):   # a frame with a transparent middle
        return lambda x, y: hexc(col) if (x < t or y < t or x >= w - t or y >= h - t) else (0, 0, 0, 0)
    png_pixels(os.path.join(OUT, "psq_poolon.png"), PADW, PADH, frame(PADW, PADH, ON_C))
    png_pixels(os.path.join(OUT, "psq_pooloff.png"), PADW, PADH, frame(PADW, PADH, OFF_C))
    png_pixels(os.path.join(OUT, "psq_rowon.png"), MPADW, MPADH, frame(MPADW, MPADH, ON_C))
    png_pixels(os.path.join(OUT, "psq_rowoff.png"), MPADW, MPADH, frame(MPADW, MPADH, OFF_C))


# 1.1: the list controls (common/skin_ui.py) by tab: (slot, kind, parameter, list id or choices)
LIST_IDS = {"Entry Len": 1, "Op 1": 2, "Op 2": 3, "Op 3": 4, "Op 4": 5, "PTN 1": 6, "PTN 2": 7, "PTN 3": 8, "PTN 4": 9, "Rate": 10,
            "Ratio": 11, "Lane Rate": 12, "Lane Ratio": 13}
N_LISTS = 14   # list ids 1-13 and 0 = none (the plugin's "List Open" range)
TEAL = (0x5d, 0xca, 0xa5)
RATIOS = ["0.25", "0.33", "0.5", "0.67", "0.75", "0.8", "1", "1.25", "1.33", "1.5", "1.67", "2", "2.5", "3", "4"]
NOT_VEL = "RangedEnabling/1/2/3/{Lane}"            # LANE settings: hidden while VEL is shown
# 1.3: per list: (title shown, visibility handles, lit colour of each choice); handles use {param name}
LIST_OPTS = {
    "Rate Mode": ("Mode", [], [skin_ui.ON_C, TEAL]),
    "Ratio": (None, ["IndexedEnabling/1/2/{Rate Mode}"], None),
    "Lane Rate": ("Rate", [NOT_VEL], None),
    "Lane Mode": ("Mode", [NOT_VEL], [skin_ui.ON_C, TEAL]),
    "Lane Ratio": ("Ratio", [NOT_VEL, "IndexedEnabling/1/2/{Lane Mode}"], None),
    "Lane Dir": ("Direction", [NOT_VEL], None),
}
# 1.3: titles and visibility of generated controls (by tab)
GEN_OPTS = {
    "LANE": {"Lane CC": ("CC Num", [NOT_VEL]), "Lane Steps": ("Steps", [NOT_VEL]), "Rest": (None, [NOT_VEL]),
             "Lane Fine": ("Fine", [NOT_VEL, "IndexedEnabling/1/2/{Lane Mode}"])},
}
LISTS = {
    "HIT": [(10, "popup", "Entry Len"), (14, "pads", "Snap")],
    "CHANCE": [(0, "popup", "Op 1"), (1, "popup", "Op 2"), (2, "popup", "Op 3"), (3, "popup", "Op 4"),
               (8, "popup", "PTN 1"), (9, "popup", "PTN 2"), (10, "popup", "PTN 3"), (11, "popup", "PTN 4")],
    "RATE": [(4, "popup", "Rate"), (8, "pads", "Rate Mode"), (9, "pads", "Direction"), (12, "popup", "Ratio")],
    "LANE": [(10, "popup", "Lane Rate"), (11, "pads", "Lane Mode"), (12, "popup", "Lane Ratio"), (14, "pads", "Lane Dir")],
    "SETUP": [(8, "pads", "PTN Count"), (9, "pads", "Steps/Page")],
}


def handles(text):
    """'IndexedEnabling/1/2/{Rate Mode}' -> the handle with the parameter index."""
    return re.sub(r"\{([^}]+)\}", lambda m: prm(m.group(1)), text)


def gen_opts(tab):
    """1.3: titles (WithName) and visibility of the generated controls of a tab (GEN_OPTS)."""
    opts = GEN_OPTS.get(tab["tabName"], {})
    for e in tab["componentDefinition"]["componentsData"]:
        for m in e["handle remapping"]["map"]:
            hit = re.fullmatch(r"Parameter (\d+)", m["value"])
            if not hit:
                continue
            name = next((n for n, i in P.items() if i == int(hit.group(1))), None)
            if name not in opts:
                continue
            title, vis = opts[name]
            if title:
                m["value"] = f"DisableCoarseIncrement/WithName[{title}]/{m['value']}"
            b = e["bounds"]
            extra = [handles(v) for v in vis]
            if extra:
                b["version"] = 2
                b["additionalInvalidatingHandles"] = b.get("additionalInvalidatingHandles", []) + extra


def lane_parts():
    """1.3: the LANE page under its generated controls: header, the lane (velocity or CC bars), its
    current step, a mask under the lane, the touch guard and the lane touch pad."""
    c = []
    fixed0 = ("Fixed/displayRange 0 1 valueRange 0 1 fineIncrement 0 coarseIncrement 0 defaultValue 0 "
              "displayString '' name '' value 0")
    lane = prm("Lane")
    zoom_vis = lambda k: f"IndexedEnabling/{k}/{ZOOMN}/{prm('Steps/Page')}"
    def vis(e, hs):
        e["bounds"]["version"] = 2
        e["bounds"]["additionalInvalidatingHandles"] = hs
        return e
    c.append(image("lanebg", "ff1a1a1a", f"{GX} {VY} {GW} {VH}"))
    # the current step of the shown CC lane (white 25 %, as the SEQ playhead), behind the bars
    for k in range(ZOOMN):
        w = GW / (8 << k)
        c.append(vis(xypad(f"lnplay{k}", prm(f"Lane Play X {k + 1}"), fixed0, f"psq_lnplay{k}.png", xpark_rect(GX, w, VY, VH)),
                     [zoom_vis(k), f"RangedEnabling/1/2/3/{lane}"]))
    # CC bars: one set per Steps/Page size; bar j of a set sits in step column j of the page
    for k in range(ZOOMN):
        z = 8 << k
        colw = GW / z
        bw = max(2, int(colw) - 2)
        for j in range(z):
            x = GX + j * colw + 1
            c.append(vis(xypad(f"lnbar{k}_{j + 1}", fixed0, prm(f"Lane Bar Y {j + 1}"), f"psq_lnbar{k}.png", f"{x:.2f} {VY} {bw} {2 * VH}"),
                         [zoom_vis(k), f"RangedEnabling/1/2/3/{lane}"]))
    # velocity bars of the selected row's hits (moved from the SEQ page)
    vv = f"IndexedEnabling/0/3/{lane}"
    for j in range(SHOWN):
        c.append(vis(xypad(f"vel{j + 1}", prm(f"Vel X {j + 1}"), prm(f"Vel Y {j + 1}"), "psq_vel.png", xpark_rect(GX, VELW, VY, 2 * VH)), [vv]))
    c.append(vis(xypad("selvel", prm("Sel Vel X"), prm("Sel Vel Y"), "psq_svel.png", xpark_rect(GX, VELW, VY, 2 * VH)), [vv]))
    # the bars reach below the lane: a mask over that part (the generated controls are drawn on top)
    c.append(image("lanemask", BG, f"0 {VY + VH} 1280 {VH + 4}"))
    c.append(image("lanemaskl", BG, f"0 {VY} {GX} {VH}"))
    c.append(image("lanemaskr", BG, f"{GX + GW} {VY} {1280 - GX - GW} {VH}"))
    c.append(xypad("touchguard", fixed0, fixed0, "psq_none.png", "0 0 1280 628"))
    c.append(xypad("t_lane", f"ResetOnGestureEnd/{prm('Touch lane X')}", f"ResetOnGestureEnd/{prm('Touch lane Y')}", "psq_none.png",
                   f"{GX} {VY} {GW} {VH}"))
    # header: lane pads, Row < n >, page < n/m >, FOLLOW
    c.append(button("sendlane", prm("Send Lane"), "psq_send_on.png", "psq_send_off.png", 1, 1, f"{GX + GW - 90} 2 90 40"))
    x = GX
    for i, nm in enumerate(("VEL", "CC 1", "CC 2")):
        c.append(button(f"lanesel{i}", lane, f"psq_lane{i}_on.png", f"psq_lane{i}_off.png", i, 3, f"{x} 2 90 40"))
        x += 96
    x += 14
    c.append(name_label("h_row", named("ROW", "Sel Row"), f"{x} 2 62 40", 24, "ff8a8885"))
    x += 62
    c.append(button("rowprev", prm("Sel Row Prev"), "psq_prev.png", "psq_prev.png", 1, 1, f"{x} 2 40 40"))
    c.append(value_label("v_row", prm("Sel Row"), f"{x + 42} 2 46 40", 30, "ffffffff", "horizontallyCentred verticallyCentred"))
    c.append(button("rownext", prm("Sel Row Next"), "psq_next.png", "psq_next.png", 1, 1, f"{x + 90} 2 40 40"))
    x += 144
    c.append(button("pageprev", prm("Page Prev"), "psq_prev.png", "psq_prev.png", 1, 1, f"{x} 2 40 40"))
    c.append(value_label("v_page", prm("View Text"), f"{x + 42} 2 62 40", 28, "ffffffff", "horizontallyCentred verticallyCentred"))
    c.append(button("pagenext", prm("Page Next"), "psq_next.png", "psq_next.png", 1, 1, f"{x + 106} 2 40 40"))
    c.append(button("follow", prm("Follow"), "psq_followon.png", "psq_followoff.png", 1, 1, f"{x + 152} 2 120 40"))
    return c


def used_slots(page):
    return {k for k, c in enumerate(page["controls"]) if c != "empty"}


def qlink_bounds(slots):
    """One rectangle per Q-Link column around the slots it uses (10 px gap, as the generator)."""
    out = []
    for col in range(4):
        rows = [k // 4 for k in slots if k % 4 == col]
        if rows:
            y0, y1 = skin_ui.SLOT_Y[min(rows)], skin_ui.SLOT_Y[max(rows)]
            out.append(f"{skin_ui.SLOT_X[col] - 10} {y0 - 10} {skin_ui.SLOT_W + 20} {y1 - y0 + skin_ui.SLOT_H + 20}")
    return out


# 1.2: step controls: page -> [(slot, parameter, < parameter, > parameter)]
STEPS = {"HIT": [(0, "Hit", "Hit Prev", "Hit Next", None), (3, "Length", "Len Prev", "Len Next", None),
                 (4, "Row", "Row Prev", "Row Next", None), (8, "Move", "Move Prev", "Move Next", None)],
         "LOCK": [(2, "Hit", "Hit Prev", "Hit Next", None), (6, "Sel Row", "Sel Row Prev", "Sel Row Next", "Row")],
         "RATE": [(0, "Sel Row", "Sel Row Prev", "Sel Row Next", "Row")]}


# 1.3: page order (user, 2026-11-22)
TAB_ORDER = ["SEQ", "LOCK", "LANE", "CHANCE", "HIT", "ROW", "RATE", "POOL", "SETUP"]


def main():
    params = os.path.join(PROJECT, "build", "params.json")
    if not os.path.exists(params):
        sys.exit("build/params.json is missing: build the plugin first")
    pj = json.load(open(params))
    choices = {}
    for e in (pj if isinstance(pj, list) else pj["params"]):
        P[e["name"]] = e["index"]
        choices[e["name"]] = e.get("choices") or []
    # 1.3: the host simulator lists choices that look like numbers as a float; the ratio list (src/perc_model.h)
    for nm in ("Ratio", "Lane Ratio"):
        choices[nm] = choices[nm] or RATIOS
    ui = json.load(open(os.path.join(PROJECT, "ui.json")))
    tmp = tempfile.mkdtemp()
    try:
        subprocess.run([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params,
                        "--ui", os.path.join(PROJECT, "ui.json"), "--out", tmp,
                        "--kit-cache", os.path.join(PROJECT, "build"), "--quiet"], check=True)
        if os.path.isdir(OUT):
            shutil.rmtree(OUT)
        os.makedirs(OUT)
        tui = open(os.path.join(tmp, "TUI.json")).read()
        head, tabs, tail = skin_ui.split_tabs(tui)
        head = skin_ui.enlarge_kit(head)   # 1.2 text sizes of the generated faders and switches
        lists = skin_ui.Lists(OUT, "psq", P["List Open"], N_LISTS)
        ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
        for t in tabs:
            t["fnKeyIndex"] += 1
        for m in ql["Screen Mode Q-Links"]["map"]:
            m["Tab"] += 1
        # list controls in the empty slots of the generated pages; their panels on top
        for ti, t in enumerate(tabs):
            page = next(pg for pg in ui["pages"] if pg["name"] == t["tabName"])
            slots = used_slots(page)
            fields, panels = [], []
            for k, kind, name in LISTS.get(t["tabName"], []):
                pidx = P[name]
                title, vis, cols = LIST_OPTS.get(name, (None, [], None))
                vis = [handles(v) for v in vis] or None
                if kind == "pads":
                    fields += lists.pad_list(f"l{pidx}", pidx, choices[name], k, title, vis, cols)
                else:
                    fields += lists.popup_field(f"l{pidx}", pidx, LIST_IDS[name], k, title, vis)
                    panels += lists.popup_panel(f"p{pidx}", pidx, choices[name], LIST_IDS[name], (title or name).upper())
                skin_ui.set_qlink(ql, ti + 2, k, pidx)
                slots.add(k)
            # 1.2: < value > step controls where percSeq limits the value (a fader handle would spring back)
            for k, name, prev, nxt, title in STEPS.get(t["tabName"], []):
                fields += lists.step_control(f"s{P[name]}", P[name], P[prev], P[nxt], k, title)
                skin_ui.set_qlink(ql, ti + 2, k, P[name])
                slots.add(k)
            gen_opts(t)
            if t["tabName"] == "LANE":
                t["componentDefinition"]["componentsData"][0:0] = lane_parts()
            if t["tabName"] == "ROW":
                fields += row_pads()
                slots |= {3, 7, 11, 15}
            if t["tabName"] == "LOCK":   # SEND (one CC for MIDI Learn) next to each Lock CC
                for k in range(4):
                    fields += lists.text_button(f"t{P[f'Send Lock {k + 1}']}", P[f"Send Lock {k + 1}"], "SEND", 3 + 4 * k)
            if t["tabName"] == "ROW":   # SEND next to Rev CC and CC Num
                fields += lists.text_button(f"t{P['Send Rev CC']}", P["Send Rev CC"], "SEND", 2)
                fields += lists.text_button(f"t{P['Send CC Num']}", P["Send CC Num"], "SEND", 6)
            if t["tabName"] == "SETUP":   # COPY, PASTE, CLEAR of the edited sequence (PASTE, CLEAR: touch twice)
                for k, name, label in ((3, "Copy", "COPY"), (7, "Paste", "PASTE"), (11, "Clear Seq", "CLEAR")):
                    fields += lists.text_button(f"t{P[name]}", P[name], label, k)
            skin_ui.add_to_tab(t, fields + panels)
            t["qlinkBoundsData"] = qlink_bounds(slots)
            if t["tabName"] == "ROW":   # column 4 holds the row pads: frame them with 10 px on every side
                x0, y0 = skin_ui.SLOT_X[3] + 4, skin_ui.SLOT_Y[0]
                w, h = 4 * MPADW + 3 * MGAP, 4 * MPADH + 3 * MGAP
                t["qlinkBoundsData"][-1] = f"{x0 - 10} {y0 - 10} {w + 20} {h + 20}"
        tabs.insert(0, seq_tab())
        tabs.append(pool_tab(len(tabs)))
        # 1.3: the page order (user); each tab's old number -> its new number (also for Q-Links.json below)
        old_no = {t["tabName"]: t["fnKeyIndex"] + 1 for t in tabs}
        assert sorted(old_no) == sorted(TAB_ORDER), (sorted(old_no), TAB_ORDER)
        tabs.sort(key=lambda t: TAB_ORDER.index(t["tabName"]))
        for i, t in enumerate(tabs):
            t["fnKeyIndex"] = i
        renumber = {old_no[n]: TAB_ORDER.index(n) + 1 for n in TAB_ORDER}
        tui = skin_ui.add_definitions(head, lists) + json.dumps(tabs, indent=4) + tail
        # Sel Row steps through 16 rows (the row pads are a 16-button radio group)
        rs = '"replacementStrings": ['
        assert rs in tui, "no replacementStrings in the generated TUI.json"
        tui = tui.replace(rs, rs + "\n" + json.dumps({"key": prm("Sel Row"), "value": [str(i) for i in range(1, ROWS + 1)]}) + ",", 1)
        tui = fine_by_default(tui)
        # the ROW page shows "Sel Row" as "Row" (the generated pages are the only place with this remap)
        sel = f'"DisableCoarseIncrement/Parameter {P["Sel Row"]}"'
        assert tui.count(sel) == 1, tui.count(sel)
        tui = tui.replace(sel, f'"DisableCoarseIncrement/WithName[Row]/Parameter {P["Sel Row"]}"')
        with open(os.path.join(OUT, "TUI.json"), "w") as f:
            f.write(tui)
        qmap = {f"Q-Link {n}": -1 for n in range(1, 17)}
        # one bank per column, top to bottom = Q-Link 13, 9, 5, 1 (column 1), 14, 10, 6, 2 (column 2) ...
        for b, (title, names) in enumerate(BANKS):
            for k, nm in enumerate(names):
                qmap[f"Q-Link {13 + b - 4 * k}"] = P[nm]
        ql["Screen Mode Q-Links"]["map"].insert(0, {"Tab": 1, "SubTab": 1, "Bank Direction": "Column", "Q-Links": qmap})
        # POOL: Q-Link n = row n (the knob grid and the pad grid have the same layout)
        ql["Screen Mode Q-Links"]["map"].append({"Tab": len(tabs), "SubTab": 1, "Bank Direction": "Column",
                                                  "Q-Links": {f"Q-Link {n}": P[f"Row {n}"] for n in range(1, 17)}})
        for m in ql["Screen Mode Q-Links"]["map"]:
            m["Tab"] = renumber[m["Tab"]]
        ql["Screen Mode Q-Links"]["map"].sort(key=lambda m: m["Tab"])
        with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
            json.dump(ql, f, indent=4)
        for n in os.listdir(tmp):
            if n.lower().endswith(".png"):
                shutil.copy(os.path.join(tmp, n), OUT)
        pictures()
        lists.write_images()
        print(f"skin written to {OUT}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
