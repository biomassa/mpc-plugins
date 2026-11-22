# Scala Retuner: user guide

Scala Retuner retunes the notes that you play to a Scala scale on Akai MPC standalone hardware. It makes no sound. It sends each note through its own MIDI port, "Scala Retuner MIDI Out", as the nearest 12-TET note plus a pitch bend. Keygroup and OPx-4 tracks that receive from this port play the notes in tune.

For the parameter table and the technical details, see the [README](README.md). For the problems that we know, see [knownIssues.md](knownIssues.md).

## Features

- Scala scales (.scl) and keyboard mappings (.kbm). Included: 12-edo, 22edo, 31-edo, ji_7 and a 31edo mapping.
- Accuracy measured on the MPC One G2: keygroups within 0.3 cents, OPx-4 within 3.3 cents (22edo).
- Up to 16 voices. Each note gets its own MIDI channel, so each note has its own bend.
- Bend wheel range in scale steps (In Bend).
- CCs, channel aftertouch and polyphonic aftertouch go through to the targets.
- The project stores the scale and the mapping.
- ROLL page: a scrolling piano roll in the pitch classes of the scale.
- Low CPU use: no work for each audio sample.

## What you need

- An Akai MPC with MPC OS 3.10.1 or later. Scala Retuner does not run in the MPC desktop software.
- Preferences → Enhancements (Beta): Enable Enhancements and External Plugin Support on.
- The plugin folder in `VST2 Plugins` on the internal storage. On the MPC One G2 the SD card cannot load plugins.

## Set up

1. Add Scala Retuner to a track (External Plugins). Set the MIDI Input Port of this track to your controller. Do not use "All Ports".
2. Make one target track for each voice. For example, for 4 voices, make 4 keygroup tracks with the same sample. For a monophonic line, use 1 target track and set Voices to 1.
3. On each target track, set MIDI Input Port to "Scala Retuner MIDI Out", MIDI Input Channel to one channel from First Ch to First Ch + Voices - 1 (a different channel for each track), and MIDI Monitor to In.
4. Preferences → Hardware → Global Pitch Bend: set the bend range. Set Bend Up and Bend Down to the same values.
5. Set Curve: Symmetric for keygroups, OPx-4 for OPx-4.
6. Select a Scale. Play.

If the MIDI Monitor of a target track is Auto and you select that track, you hear each note two times. Use In.

## The pages

| Page | What it does |
|---|---|
| RETUNE | Velocity, Scale, Mapping, Root, Root Tune, Bend Up, Bend Down, Curve, First Ch, Voices, In Bend. |
| ROLL | A scrolling piano roll of the notes, in the pitch classes of the scale. Q-Links 1-4 control Scale, Mapping, Root and In Bend. |

## Tuning

- Mapping Linear: degree 0 of the scale is on the Root key. Each next key is the next degree.
- Root Tune moves the pitch of Root in cents, relative to 12-TET.
- A .kbm mapping sets which key plays which degree, and the reference pitch. With a .kbm file, Root and Root Tune have no effect.
- Put more .scl and .kbm files in `Content/Scales/` in the plugin folder. The plugin reads the list when it loads.

## ROLL page

- Each line is one degree of the scale in one period. The number at the left is the degree. The bottom line is degree 0.
- Each colour is one output channel.
- The notes scroll to the left by one column for each sixteenth note, at the MPC tempo. 16 columns show one bar of 4/4.
- The top row shows the last note: its degree (for example `7\22`), its period relative to the root, and its cents above degree 0.
- The ROLL page uses more CPU while it is on the screen.

## Limits

- 16 voices. When all voices are in use, a new note takes the voice of the oldest note.
- The output comes about one audio block after the input.
- Scala Retuner sends MIDI through its own ALSA port. This is not part of the official SDK, and a later MPC OS can change it.
