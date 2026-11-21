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

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
OUT = os.path.join(PROJECT, "skins")

# ---- geometry (must match src/ScalaSeq.cpp) ------------------------------------------------------
GX, GY, GW, GH = 70, 88, 880, 476      # the roll
RY, RH = 44, 40                         # ruler
VY, VH = 568, 60                        # velocity lane (its bottom is the bottom of the tab area)
PARK, PARKT, OFF = 40, 44, 1100         # right strip for parked lines; labels/rows park above; x-park offset
NOTEH, NOTCHW, GAPW, BADGEW, NUMW, VELW = 24, 4, 2, 16, 40, 5
RULER_PAD_H = 2628
PANELX = GX + GW + PARK + 8             # 998: the Q-Link panel
QCOLW = 68
FREEX, FREEY, FREEW, FREEH = PANELX, 488, 4 * QCOLW, 50   # 2.3: moved up for the chance lines
SHOWN, PIECES, GAPS, STEPLINES, SNAPLINES, GRPLINES, NUMS = 32, 11, 7, 65, 89, 65, 64
BG = "ff1e1d1e"
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
BANKS = [("NOTE", ["Note", "Degree", "Period", "Move"]),
         ("SHAPE", ["Length", "VEL", "GLIDE", "RATCH"]),
         ("RATCH", ["FADE", "DUTY", "Entry Len", "Snap"]),
         ("VIEW", ["View Low", "View Start", "Sequence", "Nudge"])]


def seq_tab():
    c = []
    ly = prm("Line Y")
    # header: names 24 px, values 30 px
    x = GX
    for label, pname, nw, vw in (("SEQ", "Sequence", 58, 46), ("NOTE", "Note", 74, 80), ("DEG", "Deg Text", 60, 100),
                                 ("VIEW", "View Text", 70, 90), ("LAST PAD", "Last Note", 112, 56), ("PAD 1", "Pad 1 Note", 76, 56)):
        c.append(name_label(f"h_{label}", named(label, pname), f"{x} 2 {nw} 40", 24, "ff8a8885"))
        c.append(value_label(f"v_{label}", prm(pname), f"{x + nw} 2 {vw} 40", 30))
        x += nw + vw + 10
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
    # cursor, Group lines, end line, playhead (on top of the notes)
    c.append(xypad("cursor", prm("Cursor X"), ly, "ssq_cursor.png", vpad))
    for i in range(GRPLINES):
        c.append(xypad(f"grpline{i + 1}", prm(f"Grp Line {i + 1}"), ly, "ssq_group.png", vpad))
    c.append(xypad("endline", prm("End X"), ly, "ssq_end.png", vpad))
    c.append(xypad("playhead", prm("Play X"), ly, "ssq_play.png", vpad))
    c.append(image("parkmask", BG, f"{GX + GW + 1} {GY - 4} {PANELX - (GX + GW) - 6} {GH + 8}"))
    # velocity lane: a bar at each note start (the part below the lane is outside the tab area)
    c.append(image("lanebg", "ff151515", f"{GX} {VY} {GW} {VH}"))
    c.append(name_label("h_vel", named("VEL", "VEL"), f"4 {VY + 10} {GX - 8} 40", 20, "ff8a8885"))
    for j in range(SHOWN):
        c.append(xypad(f"vel{j + 1}", prm(f"Vel X {j + 1}"), prm(f"Vel Y {j + 1}"), "ssq_vel.png", xpark_rect(GX, VELW, VY, 2 * VH)))
    c.append(xypad("selvel", prm("Sel Vel X"), prm("Sel Vel Y"), "ssq_svel.png", xpark_rect(GX, VELW, VY, 2 * VH)))
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
    c.append(touch("t_lane", "lane", f"{GX} {VY} {GW} {VH}"))
    c.append(touch("t_free", "free", f"{FREEX} {FREEY} {FREEW} {FREEH}"))
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": BG, "image": ""},
                               "unfocussed": {"version": 1, "colour": BG, "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": c}
    qb = [f"{PANELX + b * QCOLW} {RY - 2} {QCOLW} {32 + 3 * 110 + 78}" for b in range(4)]
    return {"version": 1, "tabName": "SEQ", "fnKeyIndex": 0, "fnKeySubIndex": 0,
            "componentDefinition": root, "qlinkBoundsData": qb}


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
        png_pixels(os.path.join(OUT, f"ssq_sp{k}.png"), (1 << k) + 1, NOTEH, lambda x, y: hexc(SEL_C))
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
    png_pixels(os.path.join(OUT, "ssq_cursor.png"), 13, GH, lambda x, y: (0x37, 0x8a, 0xdd, 0x60))
    png_pixels(os.path.join(OUT, "ssq_rowline.png"), GW, 1, lambda x, y: hexc("303030"))
    png_pixels(os.path.join(OUT, "ssq_period.png"), GW, 2, lambda x, y: hexc("8a8885"))
    for d in range(31):
        label_png(os.path.join(OUT, f"ssq_deg{d}.png"), str(d), hexc("ffffff") if d == 0 else hexc("b4b2a9"), w=GX - 6)
    png_pixels(os.path.join(OUT, "ssq_freelit.png"), FREEW, FREEH, lambda x, y: hexc("6a6a6c"))
    png_pixels(os.path.join(OUT, "ssq_none.png"), 2, 2, lambda x, y: (0, 0, 0, 0))


def main():
    params = os.path.join(PROJECT, "build", "params.json")
    if not os.path.exists(params):
        sys.exit("build/params.json is missing: build the plugin first")
    pj = json.load(open(params))
    for e in (pj if isinstance(pj, list) else pj["params"]):
        P[e["name"]] = e["index"]
    tmp = tempfile.mkdtemp()
    try:
        subprocess.run([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params,
                        "--ui", os.path.join(PROJECT, "ui.json"), "--out", tmp,
                        "--kit-cache", os.path.join(PROJECT, "build"), "--quiet"], check=True)
        tui = open(os.path.join(tmp, "TUI.json")).read()
        # text edits only: the generated file has duplicate "version" keys a JSON round trip would lose
        tui = re.sub(r'"fnKeyIndex": (\d+)', lambda m: f'"fnKeyIndex": {int(m.group(1)) + 1}', tui)
        key = '"tabs": ['
        assert tui.count(key) == 1
        tui = tui.replace(key, key + "\n" + json.dumps(seq_tab(), indent=4) + ",", 1)
        tui = fine_by_default(tui)
        if os.path.isdir(OUT):
            shutil.rmtree(OUT)
        os.makedirs(OUT)
        with open(os.path.join(OUT, "TUI.json"), "w") as f:
            f.write(tui)
        ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
        for m in ql["Screen Mode Q-Links"]["map"]:
            m["Tab"] += 1
        qmap = {f"Q-Link {n}": -1 for n in range(1, 17)}
        # one bank per column, top to bottom = Q-Link 13, 9, 5, 1 (column 1), 14, 10, 6, 2 (column 2) ...
        for b, (title, names) in enumerate(BANKS):
            for k, nm in enumerate(names):
                qmap[f"Q-Link {13 + b - 4 * k}"] = P[nm]
        ql["Screen Mode Q-Links"]["map"].insert(0, {"Tab": 1, "SubTab": 1, "Bank Direction": "Column", "Q-Links": qmap})
        with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
            json.dump(ql, f, indent=4)
        for n in os.listdir(tmp):
            if n.lower().endswith(".png"):
                shutil.copy(os.path.join(tmp, n), OUT)
        pictures()
        print(f"skin written to {OUT}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
