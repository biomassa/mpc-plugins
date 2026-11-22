"""Shared skin parts for percSeq and scalaSeq (2026-11-22): list controls drawn with buttons.

The MPC's own list overlay (MenuOverlay) opens empty for external plugins (seen on the MPC One G2,
MPC OS 3.10.1, and reported by the Open MPC project), so list parameters are drawn here:

- pad list: a short list (2-6 choices) as amber/grey pads, always on the page (lit = current).
- pop-up list: a field that shows the current choice; a tap opens a panel with one button per
  choice (8 per column, current lit amber); a tap on a choice picks it and the plugin closes the
  panel; X closes it without a change. One UI parameter ("List Open", 0 = none) says which panel
  is open; each panel is shown with a visibility rule (IndexedEnabling) on it.

Labels are put under the buttons: the Open MPC project found that labels on top of buttons take the
taps. Button pictures carry their text (drawn here with Pillow).

The generated pages (tools/gen_ui.py) leave an "empty" slot where a list goes; inject() adds the
list there and gives the slot's Q-Link to the list parameter.
"""
import json
import os
import re

from PIL import Image, ImageDraw, ImageFont

SLOT_X, SLOT_Y, SLOT_W, SLOT_H = [23, 343, 663, 983], [45, 196, 346, 497], 278, 107
ON_C, OFF_C, DARK_C, PANEL_C = (0xd9, 0xa6, 0x4d), (0x3a, 0x3a, 0x3c), (0x1e, 0x1d, 0x1e), (0x26, 0x26, 0x28)
# text sizes on the pages other than SEQ (font height in px, as the MPC's textStyle)
NAME_PX, VALUE_PX = 34, 38               # names (grey) and values (white) of faders, switches, lists, step controls
HEAD_Y, HEAD_H = 4, 44                   # the name / value line at the top of a slot
BODY_Y = 52                              # the control under it (fader strip, pads, field, < value >)
PAD_PX, PAD2_PX = 26, 22                 # captions of 1-row and 2-row list pads (pictures)
OPT_PX, TITLE_PX = 28, 34                # pop-up list options (pictures) and the pop-up title
ARROW_PX = 34                            # < and > of the step controls (pictures)
FONT_PATHS = ["/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/HelveticaNeue.ttc",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]


def _font(size):
    for p in FONT_PATHS:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def text_png(path, w, h, text, bg, fg, size=22, border=None):
    """A button picture: a filled box (bg None = transparent) with centred text."""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0) if bg is None else bg + (255,))
    d = ImageDraw.Draw(im)
    if border:
        d.rectangle([0, 0, w - 1, h - 1], outline=border + (255,), width=2)
    if text:
        f = _font(size)
        b = d.textbbox((0, 0), text, font=f)
        d.text(((w - (b[2] - b[0])) / 2 - b[0], (h - (b[3] - b[1])) / 2 - b[1]), text, font=f, fill=fg + (255,))
    im.save(path)


def slot_xy(k):
    """Top-left of slot k (reading order) of the generator's 4 x 4 grid."""
    return SLOT_X[k % 4], SLOT_Y[k // 4]


def qlink_of_slot(k):
    return 13 + (k % 4) - 4 * (k // 4)


def _bounds(rect, visible_when=None):
    """visible_when: a handle or a list of handles; the entry is hidden while any of them is invalid."""
    b = {"version": 2 if visible_when else 1, "acceptsHWFocus": "No", "showWhenDataModelInvalid": "Show",
         "whenVisible": "Always", "boundsType": "Absolute", "bounds": rect}
    if visible_when:
        b["additionalInvalidatingHandles"] = list(visible_when) if isinstance(visible_when, (list, tuple)) else [visible_when]
    return b


def _entry(name, wtype, data, rect, visible_when=None):
    return {"version": 2, "componentData": {"version": 1, "name": name, "type": wtype, "data": data},
            "handle remapping": {"version": 1, "map": []}, "bounds": _bounds(rect, visible_when)}


def _label(name, handle, kind, rect, h, colour, just, visible_when=None, style="Regular"):
    return _entry(name, "Label", {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web",
                  "style": style, "height": float(h)}, "colour": colour, "justification": just, "case": "Original"},
                  "type": kind, "handleName": handle}, rect, visible_when)


def _button(name, handle, on_img, off_img, bid, n, rect, visible_when=None):
    return _entry(name, "Button", {"version": 2, "onImage": on_img, "offImage": off_img, "buttonId": bid,
                                   "numButtonsInGroup": n, "handleName": handle, "gestureBehaviour": "Instant"}, rect, visible_when)


def _header(prefix, pidx, k, value=True, title=None):
    """Name (and value) at the top of a slot, as the kit fader's header. title: shown in place of the name."""
    x, y = slot_xy(k)
    nh = f"WithName[{title}]/Parameter {pidx}" if title else f"Parameter {pidx}"
    # 1.2 sizes: name 34, value 38, each in a full-width box (name left, value right), so nothing is clipped
    out = [_label(f"{prefix}_name", nh, "Name", f"{x + 15} {y + HEAD_Y} 248 {HEAD_H}", NAME_PX, "ff6a6a6c",
                  "left verticallyCentred")]
    if value:
        out.append(_label(f"{prefix}_value", f"Parameter {pidx}", "Value", f"{x + 15} {y + HEAD_Y} 248 {HEAD_H}", VALUE_PX,
                          "ffffffff", "right verticallyCentred"))
    return out


def _local(entries, x0, y0):
    """Children with absolute tab coordinates -> coordinates inside a container at (x0, y0)."""
    for e in entries:
        x, y, w, h = e["bounds"]["bounds"].split()
        e["bounds"]["bounds"] = f"{float(x) - x0:g} {float(y) - y0:g} {w} {h}"
    return entries


class Lists:
    """Collects the list controls of one plugin skin and writes their pictures."""

    def __init__(self, out_dir, tag, list_open_param, n_lists):
        self.out, self.tag, self.lo, self.n = out_dir, tag, list_open_param, n_lists   # n = lists + 1 (state 0 = none)
        self.images = {}
        self.defs = []   # (name, CustomComponentDefinition) for localComponentDefinitions

    def _container(self, name, children, rect, visible_when=None):
        """One entry for a list control (its parts in a custom component), so the page and its Q-Link
        frames see one control of slot size (or one full-page layer for a panel)."""
        x0, y0 = (float(v) for v in rect.split()[:2])
        self.defs.append((name, {"version": 2, "actions": [],
                                 "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "0", "image": ""},
                                                    "unfocussed": {"version": 1, "colour": "0", "image": ""}},
                                 "ignoreMousePresses": False, "disableCoarseDataWheel": True,
                                 "componentsData": _local(children, x0, y0)}))
        e = _entry(name, name, {"version": 1, "handleName": "Data"}, rect, visible_when)
        e["bounds"]["acceptsHWFocus"] = "Children"
        return [e]

    def definitions_json(self):
        return ",\n".join(json.dumps({"key": n, "value": d}) for n, d in self.defs)

    def _img(self, key, w, h, text, bg, fg, size=22, border=None):
        name = f"{self.tag}_{key}.png"
        self.images[name] = (w, h, text, bg, fg, size, border)
        return name

    def write_images(self):
        for name, args in self.images.items():
            text_png(os.path.join(self.out, name), *args)

    def pad_list(self, prefix, pidx, choices, k, title=None, visible_when=None, on_colours=None):
        """A short list in slot k: header (name) and one pad per choice (one row, or 2 rows of 3 for 6).
        on_colours: the lit colour of each choice (default amber)."""
        x, y = slot_xy(k)
        n = len(choices)
        rows = 2 if n > 4 else 1
        per = (n + rows - 1) // rows
        gap = 6
        w = (248 - gap * (per - 1)) // per
        h = 44 if rows == 1 else 26
        out = _header(prefix, pidx, k, value=False, title=title)
        for i, c in enumerate(choices):
            r, col = divmod(i, per)
            lit = on_colours[i] if on_colours else ON_C
            tag = "" if lit == ON_C else "_%02x%02x%02x" % lit
            bx, by = x + 15 + col * (w + gap), y + BODY_Y + r * (h + 2)
            px = PAD_PX if rows == 1 else PAD2_PX
            on = self._img(f"pad_{w}x{h}_{i}_{c}_on{tag}".replace("/", "-").replace(" ", "_"), w, h, c, lit, DARK_C, px)
            off = self._img(f"pad_{w}x{h}_{i}_{c}_off".replace("/", "-").replace(" ", "_"), w, h, c, OFF_C, (0xe4, 0xe4, 0xe6), px)
            out.append(_button(f"{prefix}_{i}", f"Parameter {pidx}", on, off, i, n, f"{bx} {by} {w} {h}"))
        return self._container(f"{self.tag}_{prefix}", out, f"{x} {y} {SLOT_W} {SLOT_H}", visible_when)

    def popup_field(self, prefix, pidx, list_id, k, title=None, visible_when=None):
        """The field in slot k: name, a box with the current choice, and a clear button that opens the list."""
        x, y = slot_xy(k)
        box = self._img("field_box", 248, 44, "", PANEL_C, (0, 0, 0), border=(0x6a, 0x6a, 0x6c))
        clear = self._img("clear", 278, 107, "", None, (0, 0, 0))
        out = _header(prefix, pidx, k, value=False, title=title)
        out.append(_entry(f"{prefix}_box", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": box},
                          f"{x + 15} {y + BODY_Y} 248 44"))
        out.append(_label(f"{prefix}_choice", f"Parameter {pidx}", "Value", f"{x + 15} {y + BODY_Y} 248 44", VALUE_PX, "ffffffff",
                          "horizontallyCentred verticallyCentred"))
        out.append(_button(f"{prefix}_open", f"Parameter {self.lo}", clear, clear, list_id, self.n, f"{x} {y} {SLOT_W} {SLOT_H}"))
        return self._container(f"{self.tag}_{prefix}", out, f"{x} {y} {SLOT_W} {SLOT_H}", visible_when)

    def popup_panel(self, prefix, pidx, choices, list_id, title):
        """The panel for one list: shown while List Open = list_id; placed last on the page (on top)."""
        vis = f"IndexedEnabling/{list_id}/{self.n}/Parameter {self.lo}"
        n = len(choices)
        cols = (n + 7) // 8
        rows = min(8, n)
        # buttons are 150 px wide, 170 px when the longest choice needs it (for example SND HOLD at 28 px)
        bw, bh, gap = 150, 56, 8
        if max(_font(OPT_PX).getlength(c) for c in choices) > bw - 12:
            bw = 170
        pw = cols * bw + (cols - 1) * gap + 32
        ph = 56 + rows * bh + (rows - 1) * gap + 24
        px, py = (1280 - pw) // 2, max(0, (628 - ph) // 2)
        bg = self._img(f"panel_{pw}x{ph}", pw, ph, "", PANEL_C, (0, 0, 0), border=(0x6a, 0x6a, 0x6c))
        out = [_button(f"{prefix}_panel", f"Parameter {self.lo}", bg, bg, list_id, self.n, f"{px} {py} {pw} {ph}"),
               _label(f"{prefix}_title", f"WithName[{title}]/Parameter {pidx}", "Name", f"{px + 16} {py + 6} {pw - 80} 44", TITLE_PX,
                      "ffb4b2a9", "left verticallyCentred")]
        x_img = self._img("close", 44, 44, "X", OFF_C, (0xe4, 0xe4, 0xe6), 24)
        out.append(_button(f"{prefix}_x", f"Parameter {self.lo}", x_img, x_img, 0, self.n, f"{px + pw - 52} {py + 6} 44 44"))
        for i, c in enumerate(choices):
            col, r = divmod(i, 8)
            bx, by = px + 16 + col * (bw + gap), py + 56 + r * (bh + gap)
            key = re.sub(r"[^A-Za-z0-9]+", "_", c) or "dash"
            on = self._img(f"opt_{i}_{key}_{bw}_on", bw, bh, c, ON_C, DARK_C, OPT_PX)
            off = self._img(f"opt_{i}_{key}_{bw}_off", bw, bh, c, OFF_C, (0xe4, 0xe4, 0xe6), OPT_PX)
            out.append(_button(f"{prefix}_{i}", f"Parameter {pidx}", on, off, i, n, f"{bx} {by} {bw} {bh}"))
        return self._container(f"{self.tag}_{prefix}", out, "0 0 1280 628", vis)


    def text_button(self, prefix, pidx, label, k):
        """A text button that fills slot k (grey; lit amber while its parameter is on, for example armed)."""
        x, y = slot_xy(k)
        on = self._img(f"tb_{label}_on", 248, 90, label, ON_C, DARK_C, NAME_PX)
        off = self._img(f"tb_{label}_off", 248, 90, label, OFF_C, (0xe4, 0xe4, 0xe6), NAME_PX)
        out = [_button(f"{prefix}_b", f"Parameter {pidx}", on, off, 1, 1, f"{x + 15} {y + 8} 248 90")]
        return self._container(f"{self.tag}_{prefix}", out, f"{x} {y} {SLOT_W} {SLOT_H}")

    def step_control(self, prefix, pidx, prev_idx, next_idx, k, title=None):
        """A < value > control in slot k: the name on top, then a < button, the value and a > button.
        The buttons are momentary parameters (prev_idx, next_idx) that the plugin acts on and sets back to 0.
        Used where the plugin limits the value (a fader handle would spring back)."""
        x, y = slot_xy(k)
        bw, bh = 64, 48
        lt = self._img("step_prev", bw, bh, "<", OFF_C, (0xe4, 0xe4, 0xe6), ARROW_PX)
        gt = self._img("step_next", bw, bh, ">", OFF_C, (0xe4, 0xe4, 0xe6), ARROW_PX)
        out = _header(prefix, pidx, k, value=False, title=title)
        out.append(_button(f"{prefix}_prev", f"Parameter {prev_idx}", lt, lt, 1, 1, f"{x + 15} {y + BODY_Y} {bw} {bh}"))
        out.append(_label(f"{prefix}_value", f"Parameter {pidx}", "Value", f"{x + 15 + bw} {y + BODY_Y} {248 - 2 * bw} {bh}",
                          VALUE_PX, "ffffffff", "horizontallyCentred verticallyCentred"))
        out.append(_button(f"{prefix}_next", f"Parameter {next_idx}", gt, gt, 1, 1, f"{x + 263 - bw} {y + BODY_Y} {bw} {bh}"))
        return self._container(f"{self.tag}_{prefix}", out, f"{x} {y} {SLOT_W} {SLOT_H}")


def enlarge_kit(head):
    """The generated kit components (TUI.json text before the tabs) with the 1.2 text sizes:
    faders: name and value on one line, each in a full-width box; switches: name and On/Off larger."""
    import re as _re
    def block(text, key):
        i = text.index(f'"key": "{key}"')
        j = text.find('"key": "', i + 10)
        return i, (j if j > 0 else len(text))
    def sub_label(seg, height, rect, new_h, new_rect, just=None):
        pat = _re.compile(r'("height": )' + _re.escape(height) + r'(.*?"justification": ")([^"]*)(".*?"bounds": ")' + _re.escape(rect) + '"',
                          _re.S)
        m = pat.search(seg)
        assert m, (height, rect)
        rep = m.group(1) + new_h + m.group(2) + (just or m.group(3)) + m.group(4) + new_rect + '"'
        return seg[:m.start()] + rep + seg[m.end():]
    for key in ("faderUni", "faderBi"):
        if f'"key": "{key}"' not in head:
            continue
        i, j = block(head, key)
        seg = head[i:j]
        seg = sub_label(seg, "24.0", "15 8 135 34", f"{NAME_PX}.0", f"15 {HEAD_Y} 248 {HEAD_H}", "left verticallyCentred")
        seg = sub_label(seg, "26.0", "150 8 113 34", f"{VALUE_PX}.0", f"15 {HEAD_Y} 248 {HEAD_H}", "right verticallyCentred")
        head = head[:i] + seg + head[j:]
    if '"key": "toggle"' in head:
        i, j = block(head, "toggle")
        seg = head[i:j]
        seg = sub_label(seg, "26.0", "15 12 170 36", f"{NAME_PX}.0", f"15 6 170 {HEAD_H}")
        seg = sub_label(seg, "30.0", "15 52 170 42", f"{VALUE_PX}.0", f"15 52 170 46")
        head = head[:i] + seg + head[j:]
    return head


def split_tabs(tui):
    """The generated TUI.json text -> (text before the tabs array, tabs as Python objects, text after).
    The tabs hold no duplicate keys (only the overlay definitions do), so they parse as JSON."""
    i = tui.index('"tabs": [') + len('"tabs": ')
    depth, j = 0, i
    while True:
        ch = tui[j]
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
            if depth == 0:
                break
        elif ch == '"':
            j += 1
            while tui[j] != '"':
                j += 2 if tui[j] == "\\" else 1
        j += 1
    def no_dups(pairs):
        keys = [k for k, _ in pairs]
        assert len(keys) == len(set(keys)), f"duplicate keys in a tab: {keys}"
        return dict(pairs)
    return tui[:i], json.loads(tui[i:j + 1], object_pairs_hook=no_dups), tui[j + 1:]


def add_to_tab(tab, entries):
    """Append entries on top of a tab (version-1 tab with an inline root)."""
    tab["componentDefinition"]["componentsData"].extend(entries)


def add_definitions(head, lists):
    """Put the containers' definitions into the TUI.json text before the tabs."""
    key = '"localComponentDefinitions": ['
    assert head.count(key) == 1
    return head.replace(key, key + "\n" + lists.definitions_json() + ",", 1) if lists.defs else head


def set_qlink(qlinks, tab_number, k, pidx):
    for m in qlinks["Screen Mode Q-Links"]["map"]:
        if m["Tab"] == tab_number:
            m["Q-Links"][f"Q-Link {qlink_of_slot(k)}"] = pidx
