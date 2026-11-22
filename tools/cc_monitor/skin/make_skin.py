#!/usr/bin/env python3
"""Make the hand-written skin of CC Monitor in skins/ (one page, MON).

Top row: CC NUM, CC VALUE, CHANNEL, COUNT. Second row: LAST NOTE. Names 28 px light grey,
values 60 px white. Labels only (the plugin writes the fields); no Q-Link assignments.
Parameter indices are fixed (src/CCMonitor.cpp enum). Run, then build:

    python3 plugins/tools/cc_monitor/skin/make_skin.py
    python3 tools/build.py plugins/tools/cc_monitor
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "skins")
BG, NAME_C, VALUE_C = "ff1e1d1e", "ffe8e6df", "ffffffff"
NAME_PX, VALUE_PX = 28, 60
# (title, parameter index, x, y, w)
FIELDS = [("CC NUM", 1, 23, 45, 278), ("CC VALUE", 2, 343, 45, 278), ("CHANNEL", 3, 663, 45, 278),
          ("COUNT", 4, 983, 45, 278), ("LAST NOTE", 5, 23, 245, 598)]


def label(nm, kind, handle, rect, px, colour, style):
    return {"version": 2,
            "componentData": {"version": 1, "name": nm, "type": "Label", "data": {
                "version": 1,
                "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": style,
                                                     "height": float(px)},
                              "colour": colour, "justification": "left verticallyCentred", "case": "Original"},
                "type": kind, "handleName": handle}},
            "handle remapping": {"version": 1, "map": []},
            "bounds": {"version": 1, "acceptsHWFocus": "No", "showWhenDataModelInvalid": "Show",
                       "whenVisible": "Always", "boundsType": "Absolute", "bounds": rect}}


def main():
    c = []
    for title, i, x, y, w in FIELDS:
        # a Name label with WithName (the G2 does not draw Text labels)
        c.append(label(f"n{i}", "Name", f"WithName[{title}]/Parameter {i}", f"{x} {y} {w} 40", NAME_PX, NAME_C, "Regular"))
        c.append(label(f"v{i}", "Value", f"Parameter {i}", f"{x} {y + 44} {w} 80", VALUE_PX, VALUE_C, "SemiBold"))
    bgd = {"version": 1, "focussed": {"version": 1, "colour": BG, "image": ""},
           "unfocussed": {"version": 1, "colour": BG, "image": ""}}
    tui = {"pageData": {"version": 1,
                        "componentDefinitions": {"version": 2, "importFiles": [], "localComponentDefinitions": []},
                        "replacementStrings": [],
                        "info": {"version": 1, "type": "CompleteDescription"},
                        "tabs": [{"version": 1, "tabName": "MON", "fnKeyIndex": 0, "fnKeySubIndex": 0,
                                  "componentDefinition": {"version": 2, "actions": [], "backgroundData": bgd,
                                                          "ignoreMousePresses": False, "disableCoarseDataWheel": False,
                                                          "componentsData": c},
                                  "qlinkBoundsData": []}]}}
    none = {f"Q-Link {q}": -1 for q in range(1, 17)}
    ql = {"version": 4, "info": {"version": 1, "type": "CompleteDescription"},
          "Screen Mode Q-Links": {"version": 4, "map": [{"Tab": 1, "SubTab": 1, "Bank Direction": "Column", "Q-Links": none}]},
          "Program Mode Q-Links": none}
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "TUI.json"), "w") as f:
        json.dump(tui, f, indent=1)
    with open(os.path.join(OUT, "Q-Links.json"), "w") as f:
        json.dump(ql, f, indent=1)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
