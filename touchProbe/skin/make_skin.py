#!/usr/bin/env python3
"""Make the hand-written skin of touchProbe in skins/ (a TOUCH page before the generated pages).

Run from the SDK root after a build (it needs build/params.json):

    python3 plugins/touchProbe/skin/make_skin.py
    python3 tools/build.py plugins/touchProbe
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

# parameter indices (must match ParamIndex in src/TouchProbe.cpp)
P_TX, P_TY, P_STATE, P_PRESS, P_REL, P_MOVES, P_RATE, P_LASTX, P_LASTY, P_DOTX, P_DOTY, P_PIECE0 = range(1, 13)
AX, AY, AW, AH = 40, 150, 600, 400      # touch area 1 (left)
BX, BW = 660, 580                        # touch area 2 (right), multi-touch test
P_TX2, P_TY2, P_STATE2, P_PRESS2, P_DOT2X, P_DOT2Y = range(23, 29)
P_PIECEY0 = 29
STRIP = 40   # strip below the areas where unused bar pieces park (masked)
BG = "ff1e1d1e"
FIXED_MID = ("Fixed/displayRange 0 1 valueRange 0 1 fineIncrement 0 coarseIncrement 0 defaultValue 0.5 "
             "displayString '' name '' value 0.5")

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



def touch_tab():
    c = []
    x = 40
    for txt, p, w in (("STATE", P_STATE, 200), ("PRESS", P_PRESS, 190), ("UP", P_REL, 160), ("MOVES", P_MOVES, 220),
                      ("RATE", P_RATE, 200), ("X", P_LASTX, 140)):
        c.append(label_text(f"h_{txt}", txt, f"{x} 10 {w} 30", 18))
        c.append(label_value(f"v_{txt}", p, f"{x} 40 {w} 44", 32))
        x += w
    c.append(label_text("h_s2", "AREA 2", f"{BX} 640 120 36", 18))
    c.append(label_value("v_s2", P_STATE2, f"{BX + 120} 640 120 36", 28))
    c.append(label_text("h_p2", "PRESS", f"{BX + 260} 640 100 36", 18))
    c.append(label_value("v_p2", P_PRESS2, f"{BX + 360} 640 120 36", 28))
    c.append(label_text("hint", "Left box: press, drag, lift (bar = press point to finger). Right box: touch it at the same time.",
                        f"{AX} 100 {AW} 36", 20, "ffb4b2a9"))
    c.append(entry("area", "Image", {"version": 2, "imageType": "Regular", "colour": "ff2a2a2a", "image": ""}, f"{AX} {AY} {AW} {AH}"))
    c.append(entry("area2", "Image", {"version": 2, "imageType": "Regular", "colour": "ff2a2f35", "image": ""}, f"{BX} {AY} {BW} {AH}"))
    c.append(entry("dot2", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_DOT2X}",
                                      "yPositionHandle": f"Parameter {P_DOT2Y}", "image": "tp_dot.png"}, f"{BX} {AY} {BW} {AH}"))
    # 10 px grid lines in the first 200 px, to judge pixel accuracy
    for i in range(11):
        c.append(entry(f"tick{i}", "Image", {"version": 2, "imageType": "Regular", "colour": "ff555555", "image": ""},
                       f"{AX + i * 20} {AY} 1 {AH}"))
    # the bar: 11 pieces of 1 .. 1024 px; pad width AW + w, so the left edge = value * AW
    for k in range(11):
        w = 1 << k
        c.append(entry(f"piece{k}", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_PIECE0 + k}",
                                               "yPositionHandle": f"Parameter {P_PIECEY0 + k}", "image": f"tp_bar{k}.png"},
                       f"{AX} {AY} {AW + w} {AH + 2 * STRIP}"))
    # the dot at the finger
    c.append(entry("dot", "XYPad", {"version": 1, "xPositionHandle": f"Parameter {P_DOTX}",
                                     "yPositionHandle": f"Parameter {P_DOTY}", "image": "tp_dot.png"}, f"{AX} {AY} {AW} {AH}"))
    # mask over the parking strip below the areas
    c.append(entry("mask_strip", "Image", {"version": 2, "imageType": "Regular", "colour": BG, "image": ""},
                   f"0 {AY + AH + 1} 1280 {2 * STRIP + 4}"))
    # mask between the areas
    c.append(entry("mask", "Image", {"version": 2, "imageType": "Regular", "colour": BG, "image": ""},
                   f"{AX + AW} {AY - 2} {BX - AX - AW} {AH + 4}"))
    # the touch surface: values go back to 0 when the finger lifts
    c.append(entry("touch", "XYPad", {"version": 1, "xPositionHandle": f"ResetOnGestureEnd/Parameter {P_TX}",
                                       "yPositionHandle": f"ResetOnGestureEnd/Parameter {P_TY}", "image": "tp_none.png"},
                   f"{AX} {AY} {AW} {AH}"))
    c.append(entry("touch2", "XYPad", {"version": 1, "xPositionHandle": f"ResetOnGestureEnd/Parameter {P_TX2}",
                                        "yPositionHandle": f"ResetOnGestureEnd/Parameter {P_TY2}", "image": "tp_none.png"},
                   f"{BX} {AY} {BW} {AH}"))
    root = {"version": 2, "actions": [],
            "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": BG, "image": ""},
                               "unfocussed": {"version": 1, "colour": BG, "image": ""}},
            "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": c}
    return {"version": 1, "tabName": "TOUCH", "fnKeyIndex": 0, "fnKeySubIndex": 0,
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
        tui = re.sub(r'"fnKeyIndex": (\d+)', lambda m: f'"fnKeyIndex": {int(m.group(1)) + 1}', tui)
        key = '"tabs": ['
        assert tui.count(key) == 1
        tui = tui.replace(key, key + "\n" + json.dumps(touch_tab(), indent=4) + ",", 1)
        if os.path.isdir(OUT):
            shutil.rmtree(OUT)
        os.makedirs(OUT)
        with open(os.path.join(OUT, "TUI.json"), "w") as f:
            f.write(tui)
        ql = json.load(open(os.path.join(tmp, "Q-Links.json")))
        for m in ql["Screen Mode Q-Links"]["map"]:
            m["Tab"] += 1
        ql["Screen Mode Q-Links"]["map"].insert(0, {"Tab": 1, "SubTab": 1, "Bank Direction": "Column",
                                                    "Q-Links": {f"Q-Link {n}": -1 for n in range(1, 17)}})
        with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
            json.dump(ql, f, indent=4)
        for n in os.listdir(tmp):
            if n.lower().endswith(".png"):
                shutil.copy(os.path.join(tmp, n), OUT)
        for k in range(11):
            png_pixels(os.path.join(OUT, f"tp_bar{k}.png"), 1 << k, 24, lambda x, y: hexc("d9a64d"))
        png_pixels(os.path.join(OUT, "tp_dot.png"), 16, 16,
                   lambda x, y: hexc("5dcaa5") if (x - 7.5) ** 2 + (y - 7.5) ** 2 <= 56 else (0, 0, 0, 0))
        png_pixels(os.path.join(OUT, "tp_none.png"), 2, 2, lambda x, y: (0, 0, 0, 0))
        print(f"skin written to {OUT}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
