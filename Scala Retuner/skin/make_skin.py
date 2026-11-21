#!/usr/bin/env python3
"""Make the hand-written skin of Scala Retuner in skins/.

The RETUNE page comes from ui.json through the SDK generator (tools/gen_ui.py). This script
adds the ROLL page, which uses a widget the generator does not support (XYPad).

Run from the SDK root after a build (it needs build/params.json):

    python3 "plugins/Scala Retuner/skin/make_skin.py"
    python3 tools/build.py "plugins/Scala Retuner"

The build copies skins/ as it is and does not run the generator.
"""
import json
import re
import os
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

# Parameter indices (must match ParamIndex in src/ScalaRetuner.cpp)
P_SCALE, P_MAPPING, P_ROOT, P_INBEND = 1, 2, 3, 10
P_GRIDX, P_GRID0, N_GRID = 11, 12, 31
P_TRACE0, N_TRACE = 43, 8
P_LASTDEG, P_LASTOCT, P_LASTCENTS = 51, 52, 53
P_COLX0, P_CELL0, N_COLS = 54, 70, 16

# ROLL page geometry (tab area is 1280 x 628)
ROLL_X, ROLL_Y, ROLL_W, ROLL_H = 40, 96, 1200, 480
PARK_H = 24   # masked strip under the roll where silent markers wait (kRollPark in the code)
BG = "ff1e1d1e"
TRACE_COLOURS = ["ffd31145", "ff22b8d8", "ffe0b020", "ff7ac943",
                 "ffb36ae2", "ffff7f3f", "ff3fd6a6", "fff0f0f0"]


DIGITS = {  # 3 x 5 pixel font
    "0": ["111", "101", "101", "101", "111"], "1": ["010", "110", "010", "010", "111"],
    "2": ["111", "001", "111", "100", "111"], "3": ["111", "001", "111", "001", "111"],
    "4": ["101", "101", "111", "001", "001"], "5": ["111", "100", "111", "001", "111"],
    "6": ["111", "100", "111", "101", "111"], "7": ["111", "001", "010", "010", "010"],
    "8": ["111", "101", "111", "101", "111"], "9": ["111", "101", "111", "001", "111"]}


def png_pixels(path, w, h, pix):
    """Write a PNG from a function pix(x, y) -> (r, g, b, a)."""
    raw = b"".join(b"\x00" + b"".join(bytes(pix(x, y)) for x in range(w)) for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(data)


def line_png(path, number, line_rgba, text_rgba, thick):
    """A grid line across the roll, with the scale degree number at the left. Height 16, line centred."""
    w, h, scale = ROLL_W, 16, 3
    text = str(number)
    lit = set()
    for i, ch in enumerate(text):
        for ry, row in enumerate(DIGITS[ch]):
            for rx, bit in enumerate(row):
                if bit == "1":
                    for dy in range(scale):
                        for dx in range(scale):
                            lit.add((2 + i * 4 * scale + rx * scale + dx, 1 + ry * scale + dy))
    start = 2 + len(text) * 4 * scale + 4
    top = h // 2 - thick // 2
    def pix(x, y):
        if (x, y) in lit:
            return text_rgba
        if x >= start and top <= y < top + thick:
            return line_rgba
        return (0, 0, 0, 0)
    png_pixels(path, w, h, pix)


def png(path, w, h, rgba):
    """Write a w x h PNG filled with one RGBA colour."""
    row = b"\x00" + bytes(rgba) * w
    raw = row * h
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(data)


def bounds(rect, focus="No"):
    return {"version": 1, "acceptsHWFocus": focus, "showWhenDataModelInvalid": "Show",
            "whenVisible": "Always", "boundsType": "Absolute", "bounds": rect}


def entry(name, wtype, data, rect):
    return {"version": 2,
            "componentData": {"version": 1, "name": name, "type": wtype, "data": data},
            "handle remapping": {"version": 1, "map": []},
            "bounds": bounds(rect)}


def text_style(height, colour, just="left verticallyCentred", style="Regular", font="Titillium Web"):
    return {"version": 1, "font": {"version": 1, "name": font, "style": style, "height": float(height)},
            "colour": colour, "justification": just, "case": "Original"}


def roll_tab():
    roll = f"{ROLL_X} {ROLL_Y} {ROLL_W} {ROLL_H}"
    c = []
    # header: last note
    c.append(entry("title", "Label", {"version": 1, "textStyle": text_style(26, "ff6a6a6c"),
                                       "type": "Text", "handleName": "Invalid", "text": "LAST NOTE"},
                   f"{ROLL_X} 20 200 50"))
    for i, (p, x) in enumerate(((P_LASTDEG, 260), (P_LASTOCT, 520), (P_LASTCENTS, 780))):
        c.append(entry(f"last{i}", "Label", {"version": 1, "textStyle": text_style(40, "ffffffff", style="SemiBold"),
                                              "type": "Value", "handleName": f"Parameter {p}"},
                       f"{x} 15 240 60"))
    grid = f"{ROLL_X} {ROLL_Y} {ROLL_W} {ROLL_H}"
    pads = f"{ROLL_X} {ROLL_Y} {ROLL_W} {ROLL_H + PARK_H}"
    # roll area background
    c.append(entry("rollbg", "Image", {"version": 2, "imageType": "Regular", "colour": "ff141414", "image": ""},
                   f"{ROLL_X} {ROLL_Y - 8} {ROLL_W} {ROLL_H + 14}"))
    # scale degree lines: XYPad pucks, x from Grid X, y from Grid n (position in the period)
    for g in range(N_GRID):
        c.append(entry(f"grid{g + 1}", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_GRIDX}",
                                                  "yPositionHandle": f"Parameter {P_GRID0 + g}",
                                                  "image": f"roll_line{g}.png"}, grid))
    # roll: 16 columns x 8 voices, each an XYPad marker; x = Col X n, y = cell value
    for col in range(N_COLS):
        for v in range(N_TRACE):
            c.append(entry(f"cell{col + 1}_{v + 1}", "XYPad", {
                "version": 1, "xPositionHandle": f"Parameter {P_COLX0 + col}",
                "yPositionHandle": f"Parameter {P_CELL0 + col * N_TRACE + v}",
                "image": f"roll_note{v + 1}.png"}, pads))
    # mask over the parking strip: silent markers wait under it
    c.append(entry("mask", "Image", {"version": 2, "imageType": "Regular", "colour": BG, "image": ""},
                   f"{ROLL_X - 40} {ROLL_Y + ROLL_H + 6} {ROLL_W + 80} {PARK_H + 30}"))
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": BG, "image": ""},
                               "unfocussed": {"version": 1, "colour": BG, "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": c}
    return {"version": 1, "tabName": "ROLL", "fnKeyIndex": 1, "fnKeySubIndex": 0,
            "componentDefinition": root, "qlinkBoundsData": []}


def fine_by_default(tui):
    """Knobs, faders and the data wheel move by the fine step without Shift (user request
    2026-11-21): disableCoarseDataWheel and the documented DisableCoarseIncrement modifier on each
    control's Data handle (ui-reference 4.1, 7.2: both affect the data wheel)."""
    tui = tui.replace('"disableCoarseDataWheel": false', '"disableCoarseDataWheel": true')
    return re.sub(r'("key": "Data",\s*"value": ")(Parameter \d+")', r'\1DisableCoarseIncrement/\2', tui)


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
        # Append the ROLL tab as text: the generated file has duplicate "version" keys
        # (overlay widgets) that a JSON round trip would lose.
        end = tui.rstrip()
        assert end.endswith("]\n    }\n}") or end.endswith("]\n  }\n}") or end.endswith("]}}"), "unexpected TUI.json end"
        cut = end.rstrip("}").rstrip().rstrip("}").rstrip()
        assert cut.endswith("]")
        tab = json.dumps(roll_tab(), indent=4)
        tui = cut[:-1].rstrip() + ",\n" + tab + "\n]\n}\n}\n"
        tui = fine_by_default(tui)
        if os.path.isdir(OUT):
            shutil.rmtree(OUT)
        os.makedirs(OUT)
        with open(os.path.join(OUT, "TUI.json"), "w") as f:
            f.write(tui)
        ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
        ql["Screen Mode Q-Links"]["map"].append({
            "Tab": 2, "SubTab": 1, "Bank Direction": "Column",
            "Q-Links": {f"Q-Link {n}": -1 for n in range(1, 17)} | {
                "Q-Link 13": P_SCALE, "Q-Link 9": P_MAPPING, "Q-Link 5": P_ROOT, "Q-Link 1": P_INBEND}})
        with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
            json.dump(ql, f, indent=4)
        for n in os.listdir(tmp):
            if n.lower().endswith(".png"):
                shutil.copy(os.path.join(tmp, n), OUT)
        for g in range(N_GRID):
            if g == 0:
                line_png(os.path.join(OUT, "roll_line0.png"), 0, (0xff, 0xff, 0xff, 0xd0), (0xff, 0xff, 0xff, 0xff), 3)
            else:
                line_png(os.path.join(OUT, f"roll_line{g}.png"), g, (0x6a, 0x6a, 0x6c, 0xa0), (0x9a, 0x9a, 0x9c, 0xff), 2)
        cw = ROLL_W // N_COLS - 6
        for v, col in enumerate(TRACE_COLOURS):
            rgba = tuple(int(col[k:k + 2], 16) for k in (2, 4, 6)) + (0xff,)
            png(os.path.join(OUT, f"roll_note{v + 1}.png"), cw, 8, rgba)
        print(f"skin written to {OUT}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
