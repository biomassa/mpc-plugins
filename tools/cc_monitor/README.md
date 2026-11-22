# CC Monitor

A silent test instrument for the MPC One G2. It shows which MIDI messages the MPC gives to a plugin on a track.

## What it shows (page MON)

| Field | Meaning |
|---|---|
| CC NUM | The number of the last control change (CC). "-" until the first CC. |
| CC VALUE | The value of that CC. |
| CHANNEL | The MIDI channel of that CC (1-16). |
| COUNT | How many CCs the plugin received since it was loaded. |
| LAST NOTE | The last note-on: note number and channel. This shows that the MIDI path works, also when no CC arrives. |

The plugin makes no sound. The fields update at most every 50 ms.

## How to use it

1. Put CC Monitor as the plugin program of a track, in place of the plugin that you want to test.
2. Set the MIDI input of the track as for that plugin (for example the port "percSeq MIDI Out").
3. Play the source (for example a percSeq CC lane). Look at the fields.
4. If LAST NOTE changes but COUNT stays at 0, the MPC gives notes but not CCs to the plugin on this path.

## Build and test

From the SDK root:

    python3 plugins/tools/cc_monitor/skin/make_skin.py
    python3 tools/build.py plugins/tools/cc_monitor
    sh plugins/tools/cc_monitor/tests/run.sh

The skin in `skins/` is hand-written by `skin/make_skin.py` (Label widgets only; no Q-Link assignments).

Unique id: BxCm. Licence: MIT.
