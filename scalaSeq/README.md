# scalaSeq

scalaSeq is a monophonic pitch and gate sequencer for Scala tunings, for Akai MPC standalone hardware (MPC OS 3.10.1 or later). It is a port of `dingus.pitchGateSeq` (Max 9). It makes no sound. It plays its notes as MIDI on other tracks: keygroup tracks and OPx-4 tracks play them in tune.

> This software was developed with the assistance of a LLM.

## How it works

- Each note has a start and a length in steps, a scale degree, and the lanes VEL, GLIDE, RATCH, FADE and DUTY. Notes of different pitches can overlap: a note that starts later cuts the earlier note. Notes of the same pitch do not overlap: an edit stops at the neighbour.
- The sequencer follows the song position of the MPC. It plays when the MPC plays and stops when the MPC stops. Rate sets the note value of one step.
- For each note, scalaSeq sends the nearest 12-TET note and a pitch bend through its own MIDI port. The MPC shows this port as "scalaSeq MIDI Out". Each new note uses the next output channel, so the release of a note does not get the bend of the next note.
- GLIDE is a pitch-bend sweep from the pitch of the note before.
- The MPC ignores MIDI that a plugin sends to the host. Thus scalaSeq opens its own ALSA sequencer port, as Scala Retuner does. This is not part of the official SDK host contract.

## After an update

Remove scalaSeq from its track and add it again. Otherwise the MPC can keep the Q-Link assignments of the old version (measured: the knobs then control other parameters).

## Version 2.0.0

2.0.0 does not load projects saved with 1.x: scalaSeq starts empty in such a project.

## Requirements

- An Akai MPC with MPC OS 3.10.1 or later. scalaSeq runs only on standalone hardware.
- Preferences → Enhancements (Beta) → Enable Enhancements and External Plugin Support set to on.

## Setup on the MPC

1. Put scalaSeq on a track. Set the MIDI Input Port of this track to your controller or the pads (for step entry and preview). Do not use "All Ports".
2. Make one target track for each output channel. Voices sets the number of channels (default 4). For example, make 4 keygroup tracks with the same sample.
3. On each target track, set:
   - MIDI Input Port: scalaSeq MIDI Out.
   - MIDI Input Channel: one channel from Out Ch to Out Ch + Voices - 1.
   - MIDI Monitor: In.
4. In Preferences → Hardware → Global Pitch Bend, set the bend range. Set Bend Up and Bend Down (OUTPUT page) to the same values.
5. Set Curve (OUTPUT page): Sym for keygroups, OPx-4 for OPx-4.

## Make a sequence

The SEQ page is a piano roll. From top to bottom: the header, the ruler (step numbers), the roll (16 rows of scale degrees, sized by cents), and the velocity lane. The Q-Link banks and the FREE button are at the right.

Touch on the roll:

- Press on an empty place and drag: a new note. It starts on the nearest snap line; the drag sets its length, also to the left. It stops at a note of the same pitch.
- All touch edits snap to the lines on screen: the snap lines, or whole steps when the snap lines are hidden (closer than 10 px). Hold FREE to edit without snap. A tap gives a note of Entry Len (NOTE page).
- Press a note: select it (and hear it).
  - Drag the body: move it in time (its start lands on a snap line; its end stops at the end of the sequence) and in pitch (rows).
  - Drag the right third (12-24 px): change the length.
- Tap a note twice within 300 ms: delete it.
- Hold FREE with a second finger: drags do not snap (1/96 step).

Other touch areas:

- Ruler: drag sideways to scroll (by whole steps), drag up or down to zoom (Steps/Page: 1 step for each 12 px; up = fewer steps). The step under the finger stays in place.
- Degree labels at the left: drag up or down to scroll the rows.
- Velocity lane: drag a note's bar up or down to set its velocity.

Marks on the notes: thin gaps where the ratchets (RATCH) fall, a ramp for a note with GLIDE, a ring for a note with a chance operation. The selected note is green. The chance lanes of the selected note show as text under the FREE button; Clear Note and Clear All (SETUP page) remove them.

The Q-Links (the 4 banks show right of the roll):

| Bank | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
|---|---|---|---|---|
| NOTE | Note (select, 1-64) | Degree (in the period) | Period (-4 to +4) | Move (0-63 snap steps) |
| SHAPE | Length (1-64 snap steps) | VEL | GLIDE | RATCH |
| RATCH | FADE | DUTY | Entry Len | Snap |
| VIEW | View Low (rows, -32 to +31) | View Start (step 1-64) | Sequence (1-32) | Nudge (1/96 step, -32 to +31) |

- Note: past the last note, the knob stays on the last note.
- Degree: the full knob turn covers the degrees of the scale mask in the period. Period moves the note by whole periods.

Vertical lines: a dim line at each snap step (not shown when closer than 10 px), a brighter line at each whole step, and the brightest lines at Group (SETUP page, every N steps; 0 = off). Steps/Page (SETUP page, 1-64, default 16) sets how many steps the roll shows.

With the pads:

1. Set Step Entry to on (NOTE page).
2. Pads 1-16 are the 16 visible rows: pad 1 is the bottom row. Each pad adds a note at the cursor, and the cursor moves on by Entry Len. The pads also play the note.
3. If the pads do not match the rows, look at LAST PAD in the header of the SEQ page while you press pad 1, and set Pad 1 Note (SETUP page) to that value.

On the NOTE page, select a note with Note and change it with Degree, Period, Move, Length, Nudge and the lanes. Delete removes the selected note.

Press Play on the MPC to play the sequence.

## Pages

| Page | Contents |
|---|---|
| SEQ | The piano roll (see above). |
| NOTE | The selected note: Note, Degree, Period, Move, Length, Nudge, VEL, GLIDE, RATCH, FADE, DUTY. Step entry: Cursor, Entry Len, Snap, Step Entry, Delete. |
| CHANCE | 4 chance lanes for the selected note, one column for each lane: Op, Prob, PTN, Amount. The lanes run in order 1-4; UNMUTE and UNSKIP undo an earlier lane's MUTE or SKIP (for example MUTE 100 % + UNMUTE 50 % on PTN 1:4). GLIDE (Amount = share of the note) and NO GLIDE switch the glide for one hit. DELAY delays a hit by 1/16 .. 1 step, DELAY % by 1-100 % of a step. |
| SETUP | Sequence (1-32), Steps (1-64), Range Start, Range End, Swing (50-75 %), Rate, Direction, PTN Count, Velocity, Pad 1 Note, Group, Steps/Page, Clear Note, Clear All (touch twice within 3 s). |
| OUTPUT | Scale, Root, Root Tune, Out Ch, Voices, Bend Up, Bend Down, Curve. |
| MASK, MASK 2 | One switch for each scale degree (0-30): the degrees of the scale mask. |

### Lanes

| Lane | Range | Function |
|---|---|---|
| VEL | 0-100 % | Velocity. |
| GLIDE | 0-1 step | The note slides from the pitch before it. |
| RATCH | 1-8 | Hits in the note. |
| FADE | -50 to +50 | Velocity of the ratchet hits: below 0 rises, above 0 falls. At ±50 the quietest hit is 1 % of VEL. Only with 2 or more ratchets. |
| DUTY | 0.05-0.95 | Gate length of each ratchet hit. Only with 2 or more ratchets. |

### Chance operations

Each lane has an operation (Op), a probability (Prob), a pattern (PTN) and an amount. The lanes act from lane 1 to lane 4. PTN `A:B` plays the operation on the A-th of every B loop repeats (or bars, with PTN Count = Bars).

| Group | Operations |
|---|---|
| Play | MUTE, UNMUTE, SKIP, UNSKIP |
| Jump | JMP< (steps back), JMP? (random step), JMP> (steps forward), JMP TO (step number), REVERSE |
| Velocity | VEL- , VEL+ (points), VEL MAX, VEL? (random ± points) |
| Time | GATE-, GATE+ (fractions of a step), TIE, DELAY |
| Repeat | RATCH (hits), REPEAT (extra plays of the step) |
| Pitch | P+, P- (scale degrees), P?+, P?-, P? (random degrees, 0 = up to one period), HOLD, P PREV, P NEXT |

## Scales

scalaSeq reads `.scl` files from `Content/Scales/` in the plugin folder: 12-edo (the default), 22edo, 31-edo and ji_7. The project stores the scale, so a project keeps its tuning if you rename or delete the file.

## CPU

- The engine runs for each audio sample with comparisons and a few multiplications. Chance rolls and pitch tables are calculated one time for each note or edit.
- A worker thread sends the MIDI messages and writes the display parameters. No system calls occur on the audio thread.
- Host simulator: about 0.01 % CPU for each block (desktop).

## Build and test

From the SDK root (`sdk/`):

```sh
python3 tools/build.py plugins/scalaSeq
python3 plugins/scalaSeq/skin/make_skin.py
python3 tools/build.py plugins/scalaSeq
.sdk-tools/host_sim plugins/scalaSeq/build/native/scalaseq.dylib test --wav plugins/scalaSeq/build/test.wav
sh plugins/scalaSeq/tests/run.sh
```

The skin in `skins/` is hand-written. `skin/make_skin.py` makes it again: the SEQ page, and the other pages from `ui.json`. Run it after each change to `ui.json` or to the parameters.

`tests/run.sh` tests the engine (timing, ratchets and FADE, reverse, play range, SKIP and MUTE, P+ in a scale mask, glide, swing) and the plugin (step entry with the pads, preview, note edits, chance lanes, playback to MIDI, delete, the grid display, touch: tap, drag and tap-to-delete, project state).

The shared code (Scala parser, bend values, ALSA port, real-time helpers) is in `../common/`.

See the [changelog](CHANGELOG.md) and the [known issues](knownIssues.md).

## License

MIT. Based on `dingus.pitchGateSeq` from the dingus Max package by the same author.
