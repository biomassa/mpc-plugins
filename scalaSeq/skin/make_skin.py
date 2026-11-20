#!/usr/bin/env python3
"""Make the hand-written skin of scalaSeq in skins/.

The NOTE, CHANCE, SETUP, OUTPUT, MASK and MASK 2 pages come from ui.json through the SDK
generator (tools/gen_ui.py). This script puts the SEQ page in front of them: a roll of
16 rows x 64 film-strip cells (a quarter step each), row labels, a playhead and a cursor.

Run from the SDK root after a build (it needs build/params.json):

    python3 plugins/scalaSeq/skin/make_skin.py
    python3 tools/build.py plugins/scalaSeq
"""
import json
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

# parameter indices (must match ParamIndex in src/ScalaSeq.cpp)
P_SEQUENCE, P_NOTE, P_START, P_LENGTH, P_DEGREE, P_CURSOR, P_STEPENTRY = 1, 18, 19, 20, 21, 27, 29
P_VIEWLOW, P_PAGE = 78, 79
P_ROW0, P_PLAYX, P_CURSORX, P_LINEY = 80, 96, 97, 98
P_COLX0, P_MARKA0, P_MARKB0, P_MARKS0, P_ENDX = 99, 163, 227, 291, 355
P_TOUCHX, P_TOUCHY, P_PADNOTE, P_LASTKEY = 356, 357, 358, 359
P_ROWLINE0, P_PERLINE0, P_LAB0 = 360, 377, 380     # 17 row lines, 3 period lines, 31 degrees x 3 labels
ROWS, COLS, FRAMES = 16, 64, 7

# SEQ page geometry (tab area 1280 x 628)
GX, GY, CW, CH = 84, 52, 18, 30      # grid origin and cell size
GW, GH = COLS * CW, ROWS * CH        # 1152 x 480
PARK = 40                            # strip right of the grid where a hidden playhead / cursor waits (kGridFrac)
PARKY = 30                           # strip below the grid where unused note markers wait (kMarkPark)
BG = "ff1e1d1e"


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


def seq_tab():
    c = []
    # header
    hx = GX
    for txt, p, w in (("SEQ", P_SEQUENCE, 120), ("NOTE", P_NOTE, 170), ("DEG", P_DEGREE, 170),
                      ("PAGE", P_PAGE, 120), ("LOW", P_VIEWLOW, 130), ("CURSOR", P_CURSOR, 200)):
        c.append(label_text(f"h_{txt}", txt, f"{hx} 6 70 36"))
        c.append(label_value(f"v_{txt}", p, f"{hx + 62} 6 {w - 62} 36"))
        hx += w
    c.append(label_value("v_entry", P_STEPENTRY, f"{GX + GW - 150} 6 150 36", 20, "ff5dcaa5", "right verticallyCentred"))
    # grid background: a fixed picture with row and step lines
    c.append(entry("gridbg", "Image", {"version": 2, "imageType": "Regular", "colour": "ff1a1a1a", "image": "ssq_grid.png"},
                   f"{GX} {GY} {GW} {GH}"))
    # rows sized by cents: row lines and period lines are markers the plugin places from the .scl
    mpad = f"{GX} {GY} {GW} {GH + PARKY}"
    for i in range(17):
        c.append(entry(f"rowline{i + 1}", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_LINEY}",
                                                    "yPositionHandle": f"Parameter {P_ROWLINE0 + i}", "image": "ssq_rowline.png"}, mpad))
    for i in range(3):
        c.append(entry(f"perline{i + 1}", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_LINEY}",
                                                    "yPositionHandle": f"Parameter {P_PERLINE0 + i}", "image": "ssq_period.png"}, mpad))
    # notes: three XYPad markers for each quarter-step column (body, start notch, selection);
    # x = Col X n, y = the row of the note in that column (0 = parked under the mask)
    for setname, p0, img in (("body", P_MARKA0, "ssq_note.png"), ("sel", P_MARKS0, "ssq_sel.png"), ("start", P_MARKB0, "ssq_start.png")):
        for col in range(COLS):
            c.append(entry(f"{setname}{col + 1}", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_COLX0 + col}",
                                                            "yPositionHandle": f"Parameter {p0 + col}", "image": img}, mpad))
    # degree labels left of the grid: one picture per degree number, placed at its row (3 copies each)
    lpad = f"4 {GY} {GX - 12} {GH + PARKY}"
    for d in range(31):
        for k in range(3):
            c.append(entry(f"lab{d}_{k + 1}", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_LINEY}",
                                                         "yPositionHandle": f"Parameter {P_LAB0 + d * 3 + k}",
                                                         "image": f"ssq_deg{d}.png"}, lpad))
    c.append(entry("parkmask", "Image", {"version": 2, "imageType": "Regular", "colour": BG, "image": ""},
                   f"0 {GY + GH + 1} {GX + GW + 8} {PARKY + 24}"))
    # cursor and playhead: XYPad pucks; x = position in the grid (scaled by kGridFrac), y fixed at 0.5
    pad = f"{GX} {GY} {GW + PARK} {GH}"
    c.append(entry("cursor", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_CURSORX}",
                                        "yPositionHandle": f"Parameter {P_LINEY}", "image": "ssq_cursor.png"}, pad))
    c.append(entry("endline", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_ENDX}",
                                         "yPositionHandle": f"Parameter {P_LINEY}", "image": "ssq_end.png"}, pad))
    c.append(entry("playhead", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_PLAYX}",
                                          "yPositionHandle": f"Parameter {P_LINEY}", "image": "ssq_play.png"}, pad))
    # mask over the parking strip
    c.append(entry("mask", "Image", {"version": 2, "imageType": "Regular", "colour": BG, "image": ""},
                   f"{GX + GW + 1} {GY - 4} {PARK + 60} {GH + 8}"))
    # touch surface: an XYPad with an invisible puck over the whole grid, on top of everything
    c.append(entry("touch", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_TOUCHX}",
                                       "yPositionHandle": f"Parameter {P_TOUCHY}", "image": "ssq_none.png"},
                   f"{GX} {GY} {GW} {GH}"))
    # pads: the last received note, to set Pad 1 Note
    c.append(label_text("h_pad", "LAST PAD", f"{GX} 540 110 36"))
    c.append(label_value("v_pad", P_LASTKEY, f"{GX + 110} 540 80 36"))
    c.append(label_text("h_pad1", "PAD 1", f"{GX + 220} 540 80 36"))
    c.append(label_value("v_pad1", P_PADNOTE, f"{GX + 300} 540 80 36"))
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": BG, "image": ""},
                               "unfocussed": {"version": 1, "colour": BG, "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": c}
    return {"version": 1, "tabName": "SEQ", "fnKeyIndex": 0, "fnKeySubIndex": 0,
            "componentDefinition": root, "qlinkBoundsData": []}


def main():
    params = os.path.join(PROJECT, "build", "params.json")
    if not os.path.exists(params):
        sys.exit("build/params.json is missing: build the plugin first")
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
        if os.path.isdir(OUT):
            shutil.rmtree(OUT)
        os.makedirs(OUT)
        with open(os.path.join(OUT, "TUI.json"), "w") as f:
            f.write(tui)
        ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
        for m in ql["Screen Mode Q-Links"]["map"]:
            m["Tab"] += 1
        qmap = {f"Q-Link {n}": -1 for n in range(1, 17)}
        # Q-Link 1: rows (View Low), Q-Link 2: step page; the rest edit the selected note
        qmap.update({"Q-Link 1": P_VIEWLOW, "Q-Link 2": P_PAGE,
                     "Q-Link 13": P_NOTE, "Q-Link 9": P_START, "Q-Link 5": P_LENGTH,
                     "Q-Link 14": P_DEGREE, "Q-Link 10": P_CURSOR, "Q-Link 6": P_SEQUENCE,
                     "Q-Link 3": P_PADNOTE})
        ql["Screen Mode Q-Links"]["map"].insert(0, {"Tab": 1, "SubTab": 1, "Bank Direction": "Column", "Q-Links": qmap})
        with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
            json.dump(ql, f, indent=4)
        for n in os.listdir(tmp):
            if n.lower().endswith(".png"):
                shutil.copy(os.path.join(tmp, n), OUT)
        def grid_pix(x, y):
            if x % (CW * 16) == 0: return hexc("6a6a6c")
            if x % (CW * 4) == 0: return hexc("3a3a3a")
            return hexc("1a1a1a")
        png_pixels(os.path.join(OUT, "ssq_grid.png"), GW, GH, grid_pix)
        png_pixels(os.path.join(OUT, "ssq_note.png"), CW, CH - 6, lambda x, y: hexc("d9a64d"))
        png_pixels(os.path.join(OUT, "ssq_sel.png"), CW, CH - 6, lambda x, y: hexc("5dcaa5"))
        png_pixels(os.path.join(OUT, "ssq_start.png"), CW, CH - 6, lambda x, y: hexc("1e1d1e") if x < 4 else (0, 0, 0, 0))
        png_pixels(os.path.join(OUT, "ssq_end.png"), 3, GH, lambda x, y: hexc("8a8885"))
        png_pixels(os.path.join(OUT, "ssq_rowline.png"), GW, 1, lambda x, y: hexc("303030"))
        png_pixels(os.path.join(OUT, "ssq_period.png"), GW, 2, lambda x, y: hexc("8a8885"))
        for d in range(31):
            label_png(os.path.join(OUT, f"ssq_deg{d}.png"), str(d), hexc("ffffff") if d == 0 else hexc("b4b2a9"))
        png_pixels(os.path.join(OUT, "ssq_play.png"), 2, GH, lambda x, y: hexc("d31145"))
        png_pixels(os.path.join(OUT, "ssq_cursor.png"), CW, GH, lambda x, y: (0x37, 0x8a, 0xdd, 0x60))
        png_pixels(os.path.join(OUT, "ssq_none.png"), 2, 2, lambda x, y: (0, 0, 0, 0))
        print(f"skin written to {OUT}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
