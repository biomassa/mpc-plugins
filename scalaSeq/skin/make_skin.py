#!/usr/bin/env python3
"""Make the hand-written skin of scalaSeq 2.0 in skins/.

The NOTE, CHANCE, SETUP, OUTPUT, MASK and MASK 2 pages come from ui.json through the SDK
generator (tools/gen_ui.py). This script puts the SEQ page (the piano roll) in front of them.
Parameter indices are looked up by name in build/params.json.

Run from the SDK root after a build (it needs build/params.json):

    python3 plugins/scalaSeq/skin/make_skin.py
    python3 tools/build.py plugins/scalaSeq
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

# ---- geometry (must match src/ScalaSeq.cpp) ------------------------------------------------------
GX, GY, GW, GH = 70, 88, 880, 540      # the roll (2.8: + the old velocity lane; was 476)
RY, RH = 44, 40                         # ruler
VY, VH = 60, 250                        # 2.8: the lane of the LANE page (velocity or CC bars); 2026-11-24: 260 (was 280), room for the Q badge
PARK, PARKT, OFF = 40, 44, 1100         # right strip for parked lines; labels/rows park above; x-park offset
NOTEH, NOTCHW, GAPW, BADGEW, NUMW, VELW = 24, 4, 2, 16, 40, 5
RULER_PAD_H = 2628
PANELX = GX + GW + PARK + 8             # 998: the Q-Link panel
QCOLW = 68
FREEX, FREEY, FREEW, FREEH = PANELX, 488, 4 * QCOLW, 50   # 2.3: moved up for the chance lines
SHOWN, PIECES, GAPS, STEPLINES, SNAPLINES, GRPLINES, NUMS = 32, 11, 7, 65, 89, 65, 64
BG = "ff1e1d1e"
NOTE_C, SEL_C, DARK_C = "d9a64d", "5dcaa5", "1e1d1e"
# 2026-11-25 (user): the selected note is a dim body (teal at 30 % over the roll) with its ratchet hits on top
# (teal at 30 / 55 / 80 / 100 % over the dim body), as in the Max roll; opaque, so the amber body under it does not show
HIT_LEVELS, HIT_OPACITY, ROLL_BG, BODY_OPACITY = 4, (0.30, 0.55, 0.80, 1.0), "1a1a1a", 0.30


def mix(c0, c1, f):
    a, b = hexc(c0), hexc(c1)
    return tuple(round(a[i] + (b[i] - a[i]) * f) for i in range(3)) + (0xff,)

P = {}   # parameter name -> index (from build/params.json)
CHOICES = {}   # parameter name -> choice names


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


def label_png(path, text, rgba, w=72, h=17, scale=3):
    """A degree number, right-aligned, vertically centred (the XYPad centres the picture on the row)."""
    lit = set()
    tw = len(text) * 4 * scale - scale
    x0 = w - 6 - tw
    for i, ch in enumerate(text):
        for ry, row in enumerate(DIGITS[ch]):
            for rx, bit in enumerate(row):
                if bit == "1":
                    for dy in range(scale):
                        for dx in range(scale):
                            lit.add((x0 + i * 4 * scale + rx * scale + dx, 1 + ry * scale + dy))
    png_pixels(path, w, h, lambda x, y: rgba if (x, y) in lit else (0, 0, 0, 0))


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


def label_text(name, text, rect, h=20, colour="ff8a8885"):
    return entry(name, "Label", {"version": 1, "textStyle": text_style(h, colour), "type": "Text",
                                  "handleName": "Invalid", "text": text}, rect)


def label_value(name, p, rect, h=24, colour="ffffffff", just="left verticallyCentred"):
    return entry(name, "Label", {"version": 1, "textStyle": text_style(h, colour, just, "SemiBold"),
                                  "type": "Value", "handleName": f"Parameter {p}"}, rect)




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


SHORT = {"Degree": "Deg", "Period": "Per", "Length": "Len", "Entry Len": "Entry", "View Low": "Low",
         "View Start": "Start", "Sequence": "Seq"}
# 2026-11-24 (user): PITCH, TIME, SHAPE, VIEW; columns 1-3 as on the NOTE page. The titles are not drawn
# (the MPC's Q badge covered the title of the active bank).
BANKS = [("PITCH", ["Note", "Degree", "Period", "GLIDE"]),
         ("TIME", ["Move", "Length", "Nudge", "Entry Len"]),
         ("SHAPE", ["VEL", "RATCH", "FADE", "DUTY"]),
         ("VIEW", ["View Low", "View Start", "Sequence", "Snap"])]
# panel cells without drag: values that scalaSeq limits (they have < > step controls on the other pages)
NO_DRAG = {"Note", "Move", "Length"}


def seq_tab(lists):
    c = []
    ly = prm("Line Y")
    # header: names 24 px, values 30 px
    x = GX
    # 2026-11-24: LAST PAD and PAD 1 moved to SETUP
    for label, pname, nw, vw in (("SEQ", "Sequence", 58, 46), ("NOTE", "Note", 74, 80), ("DEG", "Deg Text", 60, 100),
                                 ("VIEW", "View Text", 70, 90)):
        c.append(name_label(f"h_{label}", named(label, pname), f"{x} 2 {nw} 40", 24, "ff8a8885"))
        c.append(value_label(f"v_{label}", prm(pname), f"{x + nw} 2 {vw} 40", 30))
        x += nw + vw + 10
    c.append(name_label("h_entry", named("STEP ENTRY", "Step Entry"), f"{PANELX} 2 170 40", 24, "ff8a8885"))
    c.append(value_label("v_entry", prm("Step Entry"), f"{PANELX + 170} 2 {1276 - PANELX - 170} 40", 30, "ff5dcaa5"))
    # Q-Link panel: the 4 banks as columns (the MPC frames the active bank). The names are labels here;
    # the values are touch cells on top of the page (end of this function)
    for b, (title, names) in enumerate(BANKS):
        x = PANELX + b * QCOLW
        for k, nm in enumerate(names):
            y = RY + 32 + k * 110
            h = named(SHORT[nm], nm) if nm in SHORT else prm(nm)
            c.append(name_label(f"q{b}_{k}_n", h, f"{x + 2} {y} {QCOLW - 4} 30", skin_ui.SEQ_NAME_PX, skin_ui.SEQ_LEGEND_C))
    # roll background, row lines and period lines (they park above the roll, under the ruler)
    c.append(image("rollbg", "ff1a1a1a", f"{GX} {GY} {GW} {GH}"))
    rpad = f"{GX} {GY - PARKT} {GW} {GH + PARKT}"
    for i in range(17):
        c.append(xypad(f"rowline{i + 1}", ly, prm(f"Row Line {i + 1}"), "ssq_rowline.png", rpad))
    for i in range(3):
        c.append(xypad(f"perline{i + 1}", ly, prm(f"Per Line {i + 1}"), "ssq_period.png", rpad))
    # vertical lines behind the notes: snap steps (dim), whole steps (brighter)
    vpad = f"{GX} {GY} {GW + PARK} {GH}"
    for i in range(SNAPLINES):
        c.append(xypad(f"snapline{i + 1}", prm(f"Snap Line {i + 1}"), ly, "ssq_snapline.png", vpad))
    for i in range(STEPLINES):
        c.append(xypad(f"stepline{i + 1}", prm(f"Step Line {i + 1}"), ly, "ssq_stepline.png", vpad))
    # notes: exact bodies from pieces of 1 .. 1024 px (each picture 1 px wider, so pieces overlap);
    # each note's marks (start notch, ratchet gaps, glide and chance badges) follow its own body,
    # so a later note covers the marks of an earlier one
    for j in range(SHOWN):
        ny = prm(f"Note Y {j + 1}")
        for k in range(PIECES):
            c.append(xypad(f"n{j + 1}p{k}", prm(f"Piece {j + 1}.{k}"), ny, f"ssq_p{k}.png",
                           xpark_rect(GX, (1 << k) + 1, GY, GH)))
        c.append(xypad(f"n{j + 1}notch", prm(f"Notch X {j + 1}"), ny, "ssq_notch.png", xpark_rect(GX, NOTCHW, GY, GH)))
        for k in range(GAPS):
            c.append(xypad(f"n{j + 1}gap{k + 1}", prm(f"Gap {j + 1}.{k + 1}"), ny, "ssq_gap.png", xpark_rect(GX, GAPW, GY, GH)))
        c.append(xypad(f"n{j + 1}glide", prm(f"Glide X {j + 1}"), ny, "ssq_glide.png", xpark_rect(GX, BADGEW, GY, GH)))
        c.append(xypad(f"n{j + 1}chance", prm(f"Chance X {j + 1}"), ny, "ssq_chance.png", xpark_rect(GX, BADGEW, GY, GH)))
    # the selected note again on top, in its own colour
    for k in range(PIECES):
        c.append(xypad(f"selp{k}", prm(f"Sel Piece {k}"), prm("Sel Y"), f"ssq_sp{k}.png", xpark_rect(GX, (1 << k) + 1, GY, GH)))
    # 2026-11-25 (user): the selected note's ratchet hits on its dim body, one marker set per brightness step
    for lv in range(HIT_LEVELS):
        for k in range(8):
            for pc in range(10 if k == 0 else 9):
                c.append(xypad(f"hit{lv + 1}_{k + 1}_{pc}", prm(f"Hit X {lv + 1}.{k + 1}.{pc}"), prm("Sel Y"), f"ssq_h{lv}_{pc}.png",
                               xpark_rect(GX, (1 << pc) + 1, GY, GH)))
    c.append(xypad("selnotch", prm("Sel Notch X"), prm("Sel Y"), "ssq_notch.png", xpark_rect(GX, NOTCHW, GY, GH)))
    # the selected note's marks again, above its highlight (2.3.1: the ring was hidden under it)
    for k in range(GAPS):
        c.append(xypad(f"selgap{k + 1}", prm(f"Sel Gap {k + 1}"), prm("Sel Y"), "ssq_gap.png", xpark_rect(GX, GAPW, GY, GH)))
    c.append(xypad("selglide", prm("Sel Glide X"), prm("Sel Y"), "ssq_glide.png", xpark_rect(GX, BADGEW, GY, GH)))
    c.append(xypad("selchance", prm("Sel Chance X"), prm("Sel Y"), "ssq_chance.png", xpark_rect(GX, BADGEW, GY, GH)))
    # degree labels left of the roll (park above, under a mask)
    lpad = f"2 {GY - PARKT} {GX - 6} {GH + PARKT}"
    for d in range(31):
        for k in range(3):
            c.append(xypad(f"lab{d}_{k + 1}", ly, prm(f"Lab {d}.{k + 1}"), f"ssq_deg{d}.png", lpad))
    c.append(image("labmask", BG, f"0 {RY - 4} {GX - 2} {GY - RY + 2}"))
    # ruler (covers the parked row lines) and its numbers
    c.append(image("ruler", "ff262626", f"{GX} {RY} {GW} {RH}"))
    for m in range(NUMS):
        c.append(xypad(f"num{m + 1}", prm(f"Num X {m + 1}"), ly, f"ssq_n{m + 1}.png", xpark_rect(GX, NUMW, RY, RH)))
    # (drawn after the ruler, so the ruler does not cover it)
    # 2026-11-22: "+N kept" (notes outside Steps) at the right end of the ruler (amber, as percSeq's "more")
    c.append(value_label("v_kept", prm("Kept Text"), f"{GX + GW - 100} {RY} 96 {RH}", 22, "ffd9a64d", "right verticallyCentred"))
    # cursor, Group lines, end line, playhead (on top of the notes)
    c.append(xypad("cursor", prm("Cursor X"), ly, "ssq_cursor.png", vpad))
    for i in range(GRPLINES):
        c.append(xypad(f"grpline{i + 1}", prm(f"Grp Line {i + 1}"), ly, "ssq_group.png", vpad))
    c.append(xypad("endline", prm("End X"), ly, "ssq_end.png", vpad))
    c.append(xypad("playhead", prm("Play X"), ly, "ssq_play.png", vpad))
    c.append(image("parkmask", BG, f"{GX + GW + 1} {GY - 4} {PANELX - (GX + GW) - 6} {GH + 8}"))
    # 2.8: the velocity lane moved to the LANE page
    # FREE button: grey, lit while held
    c.append(image("freebg", "ff3a3a3c", f"{FREEX} {FREEY} {FREEW} {FREEH}"))
    c.append(xypad("freelit", prm("Free Lit"), ly, "ssq_freelit.png", xpark_rect(FREEX, FREEW, FREEY, FREEH)))
    c.append(name_label("freetext", named("FREE", "Free Lit"), f"{FREEX} {FREEY} {FREEW} {FREEH}", 26, "ffffffff",
                        "horizontallyCentred verticallyCentred", "SemiBold"))
    # touch surfaces: values return to 0 when the finger lifts. The ruler pad is tall (drags have
    # room); the others are on top of it.
    # chance lanes of the selected note, 4 lines of 20 px under FREE
    for l in range(4):
        c.append(value_label(f"chance{l + 1}", prm(f"Chance {l + 1}"), f"{FREEX} {FREEY + FREEH + 10 + 20 * l} {FREEW} 20", 20, "ffb4b2a9"))
    # touch guard: an XYPad with constant (Fixed) values over the whole page, under the touch
    # surfaces, so a touch outside them cannot move a marker pad (marker pads are wide)
    fixed = ("Fixed/displayRange 0 1 valueRange 0 1 fineIncrement 0 coarseIncrement 0 defaultValue 0 "
             "displayString '' name '' value 0")
    c.append(xypad("touchguard", fixed, fixed, "ssq_none.png", "0 0 1280 628"))
    def touch(nm, t, rect):
        return xypad(nm, f"ResetOnGestureEnd/{prm(f'Touch {t} X')}", f"ResetOnGestureEnd/{prm(f'Touch {t} Y')}", "ssq_none.png", rect)
    c.append(touch("t_ruler", "ruler", f"{GX} {-1000} {GW} {RULER_PAD_H}"))
    c.append(touch("t_roll", "roll", f"{GX} {GY} {GW} {GH}"))
    c.append(touch("t_labels", "labels", f"0 {GY} {GX - 4} {GH}"))
    c.append(touch("t_free", "free", f"{FREEX} {FREEY} {FREEW} {FREEH}"))
    # 2026-11-22: the panel's touch cells, on top of the touch guard
    for b, (title, names) in enumerate(BANKS):
        for k, nm in enumerate(names):
            c.append(lists.seq_cell(f"q{b}_{k}_v", P[nm], f"{PANELX + b * QCOLW} {RY + 32 + k * 110} {QCOLW} 76",
                                    drag=nm not in NO_DRAG))
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": BG, "image": ""},
                               "unfocussed": {"version": 1, "colour": BG, "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": c}
    qb = [f"{PANELX + b * QCOLW} {RY - 2} {QCOLW} {32 + 3 * 110 + 78}" for b in range(4)]
    return {"version": 1, "tabName": "SEQ", "fnKeyIndex": 0, "fnKeySubIndex": 0,
            "componentDefinition": root, "qlinkBoundsData": qb}


# ---- MASK pages (2.6.0): 4 x 4 pads laid out like the MPC pads, as percSeq's POOL page ------------
SLOT_X, SLOT_Y, SLOT_W, SLOT_H = [23, 343, 663, 983], [45, 196, 346, 497], 278, 107
PADW, PADH = 270, 100
ON_C, OFF_C = "d9a64d", "3a3a3c"


def button(nm, handle, on_img, off_img, bid, n, rect):
    return entry(nm, "Button", {"version": 2, "onImage": on_img, "offImage": off_img, "buttonId": bid,
                                "numButtonsInGroup": n, "handleName": handle, "gestureBehaviour": "Instant"}, rect)


def mask_tab(name, fn_index, first, count):
    """Pads for degrees first .. first + count - 1; pad k sits where Q-Link k + 1 is (degree first at the bottom left).
    The label is under the button (Open MPC: labels on top of buttons take the taps); the button
    pictures are frames with a transparent middle."""
    c = []
    for k in range(count):
        d = first + k
        x = SLOT_X[k % 4] + (SLOT_W - PADW) // 2
        y = SLOT_Y[3 - k // 4] + (SLOT_H - PADH) // 2
        c.append(name_label(f"masklab{d}", named(str(d), f"Deg {d}"), f"{x} {y} {PADW} {PADH}", 44, "ffffffff",
                            "horizontallyCentred verticallyCentred", "SemiBold"))
        c.append(button(f"mask{d}", prm(f"Deg {d}"), "ssq_maskon.png", "ssq_maskoff.png", 1, 1, f"{x} {y} {PADW} {PADH}"))
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": BG, "image": ""},
                               "unfocussed": {"version": 1, "colour": BG, "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": True, "componentsData": c}
    qb = []
    for col in range(4):   # each Q-Link column around the pads it has, 10 px gap
        ys = [SLOT_Y[3 - k // 4] for k in range(count) if k % 4 == col]
        if ys:
            top = min(ys) - skin_ui.QFRAME_TOP   # 2026-11-24: room for the MPC's Q badge
            qb.append(f"{SLOT_X[col] - 10} {top} {SLOT_W + 20} {max(ys) + SLOT_H + 10 - top}")
    return {"version": 1, "tabName": name, "fnKeyIndex": fn_index, "fnKeySubIndex": 0,
            "componentDefinition": root, "qlinkBoundsData": qb}


MASKS = [("MASK", 0, 16), ("MASK 2", 16, 15)]
LOCK_FADER_W = 150   # 2026-11-24: Lock CC fader strip; the SEND button (90 px) sits right of it
KIT_THEME = "mono"   # as ui.json "kit"


def fine_by_default(tui):
    """Knobs, faders and the data wheel move by the fine step without Shift (user request
    2026-11-21): disableCoarseDataWheel and the documented DisableCoarseIncrement modifier on each
    control's Data handle (ui-reference 4.1, 7.2: both affect the data wheel)."""
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
    for k in range(PIECES):
        png_pixels(os.path.join(OUT, f"ssq_p{k}.png"), (1 << k) + 1, NOTEH, lambda x, y: hexc(NOTE_C))
        body = mix(ROLL_BG, SEL_C, BODY_OPACITY)
        png_pixels(os.path.join(OUT, f"ssq_sp{k}.png"), (1 << k) + 1, NOTEH, lambda x, y: body)
    body = mix(ROLL_BG, SEL_C, BODY_OPACITY)
    bodyhex = "%02x%02x%02x" % body[:3]
    for lv in range(HIT_LEVELS):
        col = mix(bodyhex, SEL_C, HIT_OPACITY[lv])
        for pc in range(10):
            png_pixels(os.path.join(OUT, f"ssq_h{lv}_{pc}.png"), (1 << pc) + 1, NOTEH, lambda x, y: col)
    png_pixels(os.path.join(OUT, "ssq_notch.png"), NOTCHW, NOTEH, lambda x, y: hexc(DARK_C))
    png_pixels(os.path.join(OUT, "ssq_gap.png"), GAPW, NOTEH, lambda x, y: hexc(DARK_C))

    def ramp(x, y):   # a line from bottom left to top right, 3 px thick
        d = abs((x - 2) * (NOTEH - 8) - (NOTEH - 4 - y) * (BADGEW - 4)) / math.hypot(NOTEH - 8, BADGEW - 4)
        return hexc(DARK_C) if d <= 1.5 and 2 <= x <= BADGEW - 2 and 3 <= y <= NOTEH - 3 else (0, 0, 0, 0)
    png_pixels(os.path.join(OUT, "ssq_glide.png"), BADGEW, NOTEH, ramp)

    def ring(x, y):
        r = math.hypot(x + 0.5 - BADGEW / 2, y + 0.5 - NOTEH / 2)
        return hexc(DARK_C) if 4.5 <= r <= 7 else (0, 0, 0, 0)
    png_pixels(os.path.join(OUT, "ssq_chance.png"), BADGEW, NOTEH, ring)
    png_pixels(os.path.join(OUT, "ssq_vel.png"), VELW, VH, lambda x, y: hexc(NOTE_C))
    png_pixels(os.path.join(OUT, "ssq_svel.png"), VELW, VH, lambda x, y: hexc(SEL_C))
    for n in range(1, NUMS + 1):
        number_png(os.path.join(OUT, f"ssq_n{n}.png"), n)
    png_pixels(os.path.join(OUT, "ssq_snapline.png"), 1, GH, lambda x, y: hexc("333333"))
    png_pixels(os.path.join(OUT, "ssq_stepline.png"), 1, GH, lambda x, y: hexc("4a4a4a"))
    png_pixels(os.path.join(OUT, "ssq_group.png"), 2, GH, lambda x, y: hexc("6a6a6c"))
    png_pixels(os.path.join(OUT, "ssq_end.png"), 3, GH, lambda x, y: hexc("8a8885"))
    png_pixels(os.path.join(OUT, "ssq_play.png"), 2, GH, lambda x, y: hexc("d31145"))
    png_pixels(os.path.join(OUT, "ssq_lnplay.png"), 2, VH, lambda x, y: hexc("d31145"))   # 2.8: LANE current step
    for i, nm in enumerate(("VEL", "CC 1", "CC 2")):
        skin_ui.text_png(os.path.join(OUT, f"ssq_lane{i}_on.png"), 100, HBH, nm, skin_ui.ON_C, skin_ui.DARK_C, 24)
        skin_ui.text_png(os.path.join(OUT, f"ssq_lane{i}_off.png"), 100, HBH, nm, skin_ui.OFF_C, (0xe4, 0xe4, 0xe6), 24)
    skin_ui.text_png(os.path.join(OUT, "ssq_lsend_on.png"), 90, HBH, "SEND", skin_ui.ON_C, skin_ui.DARK_C, 24)
    skin_ui.text_png(os.path.join(OUT, "ssq_lsend_off.png"), 90, HBH, "SEND", skin_ui.OFF_C, (0xe4, 0xe4, 0xe6), 24)
    skin_ui.text_png(os.path.join(OUT, "ssq_send_on.png"), 90, 40, "SEND", skin_ui.ON_C, skin_ui.DARK_C, 24)
    skin_ui.text_png(os.path.join(OUT, "ssq_send_off.png"), 90, 40, "SEND", skin_ui.OFF_C, (0xe4, 0xe4, 0xe6), 24)
    png_pixels(os.path.join(OUT, "ssq_cursor.png"), 13, GH, lambda x, y: (0x37, 0x8a, 0xdd, 0x60))
    png_pixels(os.path.join(OUT, "ssq_lncur.png"), 13, VH, lambda x, y: hexc(SEL_C)[:3] + (0x60,))   # 2026-11-24
    # 2026-11-24: the short fader strip of the LOCK page (the kit's fader, LOCK_FADER_W px long)
    sys.path.insert(0, os.path.join(SDK, "tools"))
    import make_ui_kit
    rows = make_ui_kit.render_fader(LOCK_FADER_W, 36, 127, make_ui_kit.make_theme(KIT_THEME), False, False)
    make_ui_kit.write_png(os.path.join(OUT, "fader_h_uni_short.png"), LOCK_FADER_W, rows)
    png_pixels(os.path.join(OUT, "ssq_rowline.png"), GW, 1, lambda x, y: hexc("303030"))
    png_pixels(os.path.join(OUT, "ssq_period.png"), GW, 2, lambda x, y: hexc("8a8885"))
    for d in range(31):
        label_png(os.path.join(OUT, f"ssq_deg{d}.png"), str(d), hexc("ffffff") if d == 0 else hexc("b4b2a9"), w=GX - 6)
    png_pixels(os.path.join(OUT, "ssq_freelit.png"), FREEW, FREEH, lambda x, y: hexc("6a6a6c"))
    png_pixels(os.path.join(OUT, "ssq_none.png"), 2, 2, lambda x, y: (0, 0, 0, 0))
    def frame(w, h, col, t=6):   # a frame with a transparent middle
        return lambda x, y: hexc(col) if (x < t or y < t or x >= w - t or y >= h - t) else (0, 0, 0, 0)
    png_pixels(os.path.join(OUT, "ssq_maskon.png"), PADW, PADH, frame(PADW, PADH, ON_C))
    png_pixels(os.path.join(OUT, "ssq_maskoff.png"), PADW, PADH, frame(PADW, PADH, OFF_C))


# 2.6: list controls (common/skin_ui.py) in the empty slots of the generated pages
LIST_IDS = {"Entry Len": 1, "Op 1": 2, "Op 2": 3, "Op 3": 4, "Op 4": 5, "PTN 1": 6, "PTN 2": 7, "PTN 3": 8, "PTN 4": 9,
            "Rate": 10, "Scale": 11, "Ratio": 12, "Lane Rate": 13, "Lane Ratio": 14}
N_LISTS = 15   # list ids 1-14 and 0 = none (the plugin's "List Open" range)
TEAL = (0x5d, 0xca, 0xa5)
RATIOS = ["0.25", "0.33", "0.5", "0.67", "0.75", "0.8", "1", "1.25", "1.33", "1.5", "1.67", "2", "2.5", "3", "4"]
NOT_VEL = "RangedEnabling/1/2/3/{Lane}"            # LANE settings: hidden while VEL is shown
# 2.8: per list: (title shown, visibility handles, lit colour of each choice); handles use {param name}
LIST_OPTS = {
    "Rate Mode": ("Mode", [], [skin_ui.ON_C, TEAL]),
    "Ratio": (None, ["IndexedEnabling/1/2/{Rate Mode}"], None),
    "Lane Rate": ("Rate", [NOT_VEL], None),
    "Lane Mode": ("Mode", [NOT_VEL], [skin_ui.ON_C, TEAL]),
    "Lane Ratio": ("Ratio", [NOT_VEL, "IndexedEnabling/1/2/{Lane Mode}"], None),
    "Lane Dir": ("Direction", [NOT_VEL], None),
}
GEN_OPTS = {
    "LANE": {"Lane CC": ("CC Num", [NOT_VEL]), "Lane Steps": ("Steps", [NOT_VEL]), "Rest": (None, [NOT_VEL]),
             "Lane Fine": ("Fine", [NOT_VEL, "IndexedEnabling/1/2/{Lane Mode}"])},
}
# 2026-11-24 (user): new slots (slot k = 4 * row + column)
LISTS = {
    # 2026-11-25 (user): Snap above Entry Len; the tall pads were cut in the bottom slot (seen on the MPC One G2)
    "NOTE": [(11, "tallpads", "Snap"), (15, "popup", "Entry Len")],
    "CHANCE": [(0, "popup", "Op 1"), (1, "popup", "Op 2"), (2, "popup", "Op 3"), (3, "popup", "Op 4"),
               (8, "popup", "PTN 1"), (9, "popup", "PTN 2"), (10, "popup", "PTN 3"), (11, "popup", "PTN 4")],
    "SETUP": [(2, "pads", "PTN Count")],
    "RATE": [(0, "popup", "Rate"), (4, "pads", "Rate Mode"), (8, "popup", "Ratio"), (13, "pads", "Direction")],
    "LANE": [(10, "popup", "Lane Rate"), (11, "pads", "Lane Mode"), (12, "popup", "Lane Ratio"), (14, "pads", "Lane Dir")],
    "OUTPUT": [(0, "popup", "Scale"), (10, "pads", "Curve")],
}


def qlink_bounds(slots, tall=(), top_min=0):
    """One frame per column around its controls. 2026-11-24: the frame starts QFRAME_TOP px above the top
    control, so the MPC's Q badge sits above the name; tall: slots whose pads run TALL_EXTRA px lower;
    top_min: no frame starts above this y (the CHANCE header strip)."""
    out = []
    for col in range(4):
        rows = [k // 4 for k in slots if k % 4 == col]
        if rows:
            y0, y1 = skin_ui.SLOT_Y[min(rows)], skin_ui.SLOT_Y[max(rows)]
            top = max(top_min, y0 - skin_ui.QFRAME_TOP)
            # tall pads add room only when they are the column's bottom control
            bottom = min(627, y1 + skin_ui.SLOT_H + 10 + (skin_ui.TALL_EXTRA if any(k % 4 == col and k // 4 == max(rows) for k in tall) else 0))
            out.append(f"{skin_ui.SLOT_X[col] - 10} {top} {skin_ui.SLOT_W + 20} {bottom - top}")
    return out


# 2.7: step controls: page -> [(slot, parameter, < parameter, > parameter)]
STEPS = {"NOTE": [(0, "Note", "Note Prev", "Note Next"), (1, "Move", "Move Prev", "Move Next"),
                  (5, "Length", "Len Prev", "Len Next")],
         "LOCK": [(2, "Note", "Note Prev", "Note Next")]}


def handles(text):
    """'IndexedEnabling/1/2/{Rate Mode}' -> the handle with the parameter index."""
    return re.sub(r"\{([^}]+)\}", lambda m: prm(m.group(1)), text)


def gen_opts(tab):
    """2.8: titles (WithName) and visibility of the generated controls of a tab (GEN_OPTS)."""
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
            extra = [handles(v) for v in vis]
            if extra:
                e["bounds"]["version"] = 2
                e["bounds"]["additionalInvalidatingHandles"] = e["bounds"].get("additionalInvalidatingHandles", []) + extra


HBH = 56            # 2026-11-25 (user): LANE header button height
LX, LW = 343, 918   # 2026-11-25 (user): the LANE page lane starts at Q-Link column 2 and runs to the right edge


def xpark_lane(base_x, w, y, h):
    """A pad for an x-parked marker on the LANE page lane (as xpark_rect, with the lane width)."""
    return f"{base_x - OFF} {y} {OFF + LW + w} {h}"


def lane_parts():
    """2.8: the LANE page under its generated controls: lane pads, the lane (velocity bars or thin CC
    bars at the steps of the SEQ view), the lane's current step (a line), a mask under the lane, the
    touch guard and the lane touch pad. 2026-11-25: the lane is at x LX .. LX + LW."""
    c = []
    fixed0 = ("Fixed/displayRange 0 1 valueRange 0 1 fineIncrement 0 coarseIncrement 0 defaultValue 0 "
              "displayString '' name '' value 0")
    lane = prm("Lane")
    def vis(e, hs):
        e["bounds"]["version"] = 2
        e["bounds"]["additionalInvalidatingHandles"] = hs
        return e
    c.append(image("lanebg", "ff1a1a1a", f"{LX} {VY} {LW} {VH}"))
    # 2026-11-24: the cursor of the CC lane (Lane Step, or the last touch), see-through teal, behind the bars
    c.append(vis(xypad("lncur", prm("Lane Cur X"), prm("Line Y"), "ssq_lncur.png", xpark_lane(LX, 13, VY, VH)),
                 [f"RangedEnabling/1/2/3/{lane}"]))
    c.append(vis(xypad("lnplay", prm("Lane Play X"), fixed0, "ssq_lnplay.png", f"{LX} {VY} {LW + PARK} {VH}"),
                 [f"RangedEnabling/1/2/3/{lane}"]))
    for j in range(64):
        c.append(vis(xypad(f"lnbar{j + 1}", prm(f"Lane Bar X {j + 1}"), prm(f"Lane Bar Y {j + 1}"), "ssq_vel.png",
                           xpark_lane(LX, VELW, VY, 2 * VH)), [f"RangedEnabling/1/2/3/{lane}"]))
    vv = f"IndexedEnabling/0/3/{lane}"
    for j in range(SHOWN):
        c.append(vis(xypad(f"vel{j + 1}", prm(f"Vel X {j + 1}"), prm(f"Vel Y {j + 1}"), "ssq_vel.png", xpark_lane(LX, VELW, VY, 2 * VH)), [vv]))
    c.append(vis(xypad("selvel", prm("Sel Vel X"), prm("Sel Vel Y"), "ssq_svel.png", xpark_lane(LX, VELW, VY, 2 * VH)), [vv]))
    c.append(image("lanemask", BG, f"0 {VY + VH} 1280 {VH + 4}"))
    c.append(image("lanemaskl", BG, f"0 {VY} {LX} {VH}"))
    c.append(image("lanemaskr", BG, f"{LX + LW} {VY} {1280 - LX - LW} {VH}"))
    c.append(xypad("touchguard", fixed0, fixed0, "ssq_none.png", "0 0 1280 628"))
    c.append(xypad("t_lane", f"ResetOnGestureEnd/{prm('Touch lane X')}", f"ResetOnGestureEnd/{prm('Touch lane Y')}", "ssq_none.png",
                   f"{LX} {VY} {LW} {VH}"))
    # 2026-11-25 (user): bigger header buttons, 56 px tall (were 40); lane pads 100 wide (were 90)
    c.append(button("sendlane", prm("Send Lane"), "ssq_lsend_on.png", "ssq_lsend_off.png", 1, 1, f"{LX + LW - 90} 0 90 {HBH}"))
    x = LX
    for i, nm in enumerate(("VEL", "CC 1", "CC 2")):
        c.append(button(f"lanesel{i}", lane, f"ssq_lane{i}_on.png", f"ssq_lane{i}_off.png", i, 3, f"{x} 0 100 {HBH}"))
        x += 106
    return c


def lane_controls(lists):
    """2026-11-25 (user): Q-Link column 1, knobs 1-2 as controls in slots 0 and 4 (left of the lane):
    Lane Step as a < value > control and Lane Value as a fader, named STEP / VALUE (CC lane) or NOTE / VEL."""
    lane = prm("Lane")
    out = []
    for tag, vis, t_step, t_val in (("cc", f"RangedEnabling/1/2/3/{lane}", "STEP", "VALUE"),
                                    ("vel", f"IndexedEnabling/0/3/{lane}", "NOTE", "VEL")):
        out += lists.step_control(f"ls_{tag}", P["Lane Step"], P["Lane Prev"], P["Lane Next"], 0, t_step, [vis])
        x, y = skin_ui.slot_xy(4)
        e = entry(f"lv_{tag}", "faderUni", {"version": 1, "handleName": "Data"}, f"{x} {y} {skin_ui.SLOT_W} {skin_ui.SLOT_H}",
                  [{"key": "Data", "value": f"DisableCoarseIncrement/WithName[{t_val}]/{prm('Lane Value')}"}])
        e["bounds"].update({"version": 2, "acceptsHWFocus": "Yes", "showWhenDataModelInvalid": "Hide",
                            "additionalInvalidatingHandles": [vis]})
        out.append(e)
    return out


# 2026-11-24 (user): the order of the function keys; a tuple = one key with sub-tabs (press the key again).
# SUBTABS = False gives each page its own key in the same order (the fallback if sub-tabs fail on the MPC).
SUBTABS = True
TAB_ORDER = ["SEQ", "NOTE", "CHANCE", "LANE", "LOCK", "RATE", "SETUP", "OUTPUT", ("MASK", "MASK 2")]
# Program Mode Q-Links (knobs banked to the plugin), one column per set, top to bottom (user, 2026-11-24)
PROGRAM_COLUMNS = [["Sequence", "Range Start", "Range End", "Direction"], ["Rate", "Swing", "Ratio", "Fine"],
                   ["Prob 1", "Prob 2", "Prob 3", "Prob 4"], ["VEL", "RATCH", "GLIDE", "Nudge"]]
HDR_Y, HDR_H, CHANCE_SHIFT = 0, 30, 17   # CHANCE: header strip, and how far the controls move down


def chance_header(lists):
    """2026-11-24: CHANCE header strip (touch only): < > select the note; NOTE 3/3 and DEG 4(+1) as on SEQ."""
    lt = lists._img("hdr_prev", 56, HDR_H, "<", skin_ui.OFF_C, (0xe4, 0xe4, 0xe6), 26)
    gt = lists._img("hdr_next", 56, HDR_H, ">", skin_ui.OFF_C, (0xe4, 0xe4, 0xe6), 26)
    c = [button("hdr_prev", prm("Note Prev"), lt, lt, 1, 1, f"23 {HDR_Y} 56 {HDR_H}"),
         button("hdr_next", prm("Note Next"), gt, gt, 1, 1, f"85 {HDR_Y} 56 {HDR_H}")]
    x = 160
    for label, pname, nw, vw in (("NOTE", "Note", 74, 80), ("DEG", "Deg Text", 60, 100)):
        c.append(name_label(f"ch_{label}", named(label, pname), f"{x} {HDR_Y} {nw} {HDR_H}", 24, "ff8a8885"))
        c.append(value_label(f"cv_{label}", prm(pname), f"{x + nw} {HDR_Y} {vw} {HDR_H}", 26))
        x += nw + vw + 10
    return c


def lock_cc_short(tab):
    """2026-11-24: the Lock CC faders get the short strip (the header stays full width), for SEND at the right."""
    ids = {f"Parameter {P[f'Lock CC {k + 1}']}" for k in range(4)}
    for e in tab["componentDefinition"]["componentsData"]:
        vals = [m["value"].split("/")[-1] for m in e["handle remapping"]["map"]]
        if e["componentData"]["type"] == "faderUni" and any(v in ids for v in vals):
            e["componentData"]["type"] = "faderUniShort"


def main():
    params = os.path.join(PROJECT, "build", "params.json")
    if not os.path.exists(params):
        sys.exit("build/params.json is missing: build the plugin first")
    pj = json.load(open(params))
    for e in (pj if isinstance(pj, list) else pj["params"]):
        P[e["name"]] = e["index"]
        CHOICES[e["name"]] = e.get("choices") or []
    # 2.8: the host simulator lists choices that look like numbers as a float; the ratio list (src/seq_model.h)
    for nm in ("Ratio", "Lane Ratio"):
        CHOICES[nm] = CHOICES[nm] or RATIOS
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
        head = skin_ui.enlarge_kit(head)   # 2.7 text sizes of the generated faders and switches
        head = skin_ui.short_fader_definition(head, LOCK_FADER_W)
        ui = json.load(open(os.path.join(PROJECT, "ui.json")))
        lists = skin_ui.Lists(OUT, "ssq", P["List Open"], N_LISTS)
        ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
        # the generated Q-Link map of each page, by page name (generated Tab n = ui.json page n)
        qmap = {t["tabName"]: next(m["Q-Links"] for m in ql["Screen Mode Q-Links"]["map"] if m["Tab"] == ti + 1)
                for ti, t in enumerate(tabs)}
        def set_q(page, k, pidx):
            qmap[page][f"Q-Link {skin_ui.qlink_of_slot(k)}"] = pidx
        for t in tabs:
            name = t["tabName"]
            page = next(pg for pg in ui["pages"] if pg["name"] == name)
            slots = {k for k, c in enumerate(page["controls"]) if c != "empty"}
            tall = set()
            fields, panels = [], []
            for k, kind, pname in LISTS.get(name, []):
                pidx = P[pname]
                title, vis, cols = LIST_OPTS.get(pname, (None, [], None))
                vis = [handles(v) for v in vis] or None
                if kind in ("pads", "tallpads"):
                    fields += lists.pad_list(f"l{pidx}", pidx, CHOICES[pname], k, title, vis, cols, tall=kind == "tallpads")
                    if kind == "tallpads":
                        tall.add(k)
                else:
                    fields += lists.popup_field(f"l{pidx}", pidx, LIST_IDS[pname], k, title, vis)
                    panels += lists.popup_panel(f"p{pidx}", pidx, CHOICES[pname], LIST_IDS[pname], (title or pname).upper())
                set_q(name, k, pidx)
                slots.add(k)
            # 2.7: < value > step controls where scalaSeq limits the value (a fader handle would spring back)
            for k, pname, prev, nxt in STEPS.get(name, []):
                fields += lists.step_control(f"s{P[pname]}", P[pname], P[prev], P[nxt], k)
                set_q(name, k, P[pname])
                slots.add(k)
            gen_opts(t)
            if name == "LANE":
                t["componentDefinition"]["componentsData"][0:0] = lane_parts()
                set_q(name, 0, P["Lane Step"])    # 2026-11-24: drawing knobs; 2026-11-25: their controls left of the lane
                set_q(name, 4, P["Lane Value"])
                fields += lane_controls(lists)
                slots |= {0, 4}
            if name == "LOCK":   # SEND (one CC for MIDI Learn) right of each short Lock CC fader
                lock_cc_short(t)
                for k in range(4):
                    sk = 1 + 4 * k
                    x, y = skin_ui.slot_xy(sk)
                    fields.append(button(f"sendlock{k + 1}", prm(f"Send Lock {k + 1}"), "ssq_send_on.png", "ssq_send_off.png", 1, 1,
                                         f"{x + 15 + LOCK_FADER_W + 8} {y + 52} 90 40"))
            if name == "SETUP":   # COPY, PASTE, CLEAR of the edited sequence (PASTE, CLEAR: touch twice)
                for k, pname, label in ((4, "Copy", "COPY"), (8, "Paste", "PASTE"), (12, "Clear Seq", "CLEAR")):
                    fields += lists.text_button(f"t{P[pname]}", P[pname], label, k)
                x, y = skin_ui.slot_xy(3)   # LAST PAD under the Pad 1 Note fader (moved from the SEQ header)
                fields.append(name_label("lastpad_n", named("LAST PAD", "Last Note"), f"{x + 15} {y + 90} 110 18", 20, "ff8a8885"))
                fields.append(value_label("lastpad_v", prm("Last Note"), f"{x + 125} {y + 90} 60 18", 20))
            if name == "NOTE":   # 2026-11-24: Delete as a text button (one touch; no Q-Link, a knob must not delete)
                fields += lists.text_button(f"t{P['Delete']}", P["Delete"], "DELETE", 13)
            if name == "CHANCE":   # 2026-11-24: header strip; the controls and frames move down under it (not the list panels)
                skin_ui.add_to_tab(t, fields)
                skin_ui.shift_tab(t, CHANCE_SHIFT)
                skin_ui.add_to_tab(t, chance_header(lists) + panels)
            else:
                skin_ui.add_to_tab(t, fields + panels)
            if name == "CHANCE":
                qb = qlink_bounds(slots, top_min=HDR_H + 1)
                t["qlinkBoundsData"] = [" ".join([r.split()[0], str(int(r.split()[1]) + (CHANCE_SHIFT if int(r.split()[1]) > HDR_H + 1 else 0)),
                                                  r.split()[2], str(min(int(r.split()[3]) + CHANCE_SHIFT, 627 - max(HDR_H + 1, int(r.split()[1]))))])
                                        for r in qb]
            else:
                t["qlinkBoundsData"] = qlink_bounds(slots, tall)
        by_name = {t["tabName"]: t for t in tabs}
        by_name["SEQ"] = seq_tab(lists)
        seq_q = {f"Q-Link {n}": -1 for n in range(1, 17)}
        # one bank per column, top to bottom = Q-Link 13, 9, 5, 1 (column 1), 14, 10, 6, 2 (column 2) ...
        for b, (title, names) in enumerate(BANKS):
            for k, nm in enumerate(names):
                seq_q[f"Q-Link {13 + b - 4 * k}"] = P[nm]
        qmap["SEQ"] = seq_q
        for nm, first, cnt in MASKS:
            by_name[nm] = mask_tab(nm, 0, first, cnt)
            # MASK pages: Q-Link k + 1 = the pad at the same place
            qmap[nm] = {f"Q-Link {k + 1}": (P[f"Deg {first + k}"] if k < cnt else -1) for k in range(16)}
        order = TAB_ORDER if SUBTABS else [n for e in TAB_ORDER for n in (e if isinstance(e, tuple) else (e,))]
        final, qentries = [], []
        for key, e in enumerate(order):
            for sub, nm in enumerate(e if isinstance(e, tuple) else (e,)):
                t = by_name.pop(nm)
                t["fnKeyIndex"], t["fnKeySubIndex"] = key, sub
                final.append(t)
                qentries.append({"Tab": key + 1, "SubTab": sub + 1, "Bank Direction": "Column", "Q-Links": qmap[nm]})
        assert not by_name, f"pages missing from TAB_ORDER: {list(by_name)}"
        ql["Screen Mode Q-Links"]["map"] = qentries
        prog = {f"Q-Link {n}": -1 for n in range(1, 17)}
        for col, names in enumerate(PROGRAM_COLUMNS):
            for r, nm in enumerate(names):
                prog[f"Q-Link {13 + col - 4 * r}"] = P[nm]
        ql["Program Mode Q-Links"] = prog
        tui = skin_ui.add_definitions(head, lists) + json.dumps(final, indent=4) + tail
        tui = fine_by_default(tui)
        with open(os.path.join(OUT, "TUI.json"), "w") as f:
            f.write(tui)
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
