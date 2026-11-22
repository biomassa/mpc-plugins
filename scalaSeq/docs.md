# scalaSeq: user guide

scalaSeq is a monophonic pitch and gate sequencer for Scala tunings on Akai MPC standalone hardware. It makes no sound. It sends its notes through its own MIDI port, "scalaSeq MIDI Out". For each note it sends the nearest 12-TET note and a pitch bend, so keygroup and OPx-4 tracks play it in tune.

For the parameter tables and the technical details, see the [README](README.md). For the problems that we know, see [knownIssues.md](knownIssues.md).

## Features

- Scala tunings (.scl). Included: 12-edo, 22edo, 31-edo and ji_7. The project stores the scale.
- A scale mask: select which degrees of the scale the sequence and the pitch operations use.
- A piano roll of 16 rows of scale degrees. The row heights follow the cents of the scale.
- Each note has a start, a length, a scale degree, and the values VEL, GLIDE, RATCH (1-8 hits in the note), FADE and DUTY.
- GLIDE: a pitch-bend slide from the note before.
- 4 chance lanes for each note: mute, skip, jumps, velocity, timing, repeats and pitch operations (up, down, random in the scale, hold).
- Rate, Steps (1-64), Range Start, Range End, Direction and Swing for the sequence. Ratio rates (for example 1.33 or 0.75) with a fine adjustment.
- 4 lock values for each note (p-locks), sent as CCs just before the note.
- 2 CC lanes with their own steps, rate and direction.
- 32 sequences, selected by MIDI notes, for song arrangement.
- COPY, PASTE and CLEAR of a sequence.
- Notes outside Steps are kept. When you make Steps smaller and then larger again, the notes come back.
- Step entry with the MPC pads.
- Each new note uses the next MIDI channel (Voices), so the release of a note keeps its own bend.

## What you need

- An Akai MPC with MPC OS 3.10.1 or later. scalaSeq does not run in the MPC desktop software.
- Preferences → Enhancements (Beta): Enable Enhancements and External Plugin Support on.
- The plugin folder in `VST2 Plugins` on the internal storage.

## Set up

1. Add scalaSeq to a track (External Plugins). Set the MIDI Input Port of this track to the pads or your controller. Do not use "All Ports".
2. Make one target track for each output channel. Voices (OUTPUT page) sets the number of channels (default 4). For example, make 4 keygroup tracks with the same sample.
3. On each target track, set MIDI Input Port to "scalaSeq MIDI Out", MIDI Input Channel to one channel from Out Ch to Out Ch + Voices - 1, and MIDI Monitor to In.
4. Preferences → Hardware → Global Pitch Bend: set the bend range. Set Bend Up and Bend Down (OUTPUT page) to the same values.
5. Set Curve (OUTPUT page): Sym for keygroups, OPx-4 for OPx-4.
6. Preferences → MIDI / Sync → Input Ports, "scalaSeq MIDI Out": Global off, Track on, Control on.
7. Select a scale on the OUTPUT page. Press Play on the MPC.

After an update of scalaSeq, remove it from the track and add it again.

## The pages

| Page | What it does |
|---|---|
| SEQ | The piano roll. Draw, move and delete notes. The Q-Link banks are at the right. |
| NOTE | All values of the selected note, and step entry. |
| LOCK | The 4 lock values of the selected note, and Lock CC 1-4 of the sequence. |
| CHANCE | The 4 chance lanes of the selected note: Op, Prob, PTN, Amount. |
| RATE | Rate, Mode, Ratio, Fine, Swing, Direction, Steps, Range Start, Range End. |
| LANE | The velocity of the notes, and the 2 CC lanes. Draw the lane values with a finger. |
| SETUP | Sequence, COPY, PASTE, CLEAR, PTN Count, Velocity, Pad 1 Note, Group, Steps/Page, Clear Note, Clear All. |
| OUTPUT | Scale, Root, Root Tune, Out Ch, Voices, Bend Up, Bend Down, Curve. |
| MASK, MASK 2 | One pad for each scale degree. A lit pad is in the scale mask. |

## Make a sequence

On the SEQ page:

- Touch an empty place and drag: a new note. The drag sets its length. A tap gives a note of Entry Len.
- Touch a note: select it. Drag it to move it in time or in pitch. Drag its right end to change its length.
- Tap a note two times quickly: delete it.
- Hold FREE with a second finger: the drag does not snap.
- On the ruler: drag sideways to scroll, and drag up or down to zoom.
- On the degree labels at the left: drag up or down to scroll the rows.

The selected note is green. Marks on a note show its ratchets, its glide and its chance lanes.

The Q-Link panel at the right has 4 banks: NOTE, SHAPE, RATCH and VIEW. Tap a value to put the Q-Links on its column. Drag a value up or down to change it. Double-tap a value to open the big editor.

With the pads:

1. On the NOTE page, set Step Entry to on.
2. Pads 1-16 are the 16 visible rows (pad 1 = the bottom row). A pad adds a note at the cursor and moves the cursor by Entry Len.
3. If the pads do not match the rows, see LAST PAD in the SEQ header while you push pad 1. Set Pad 1 Note (SETUP page) to that value.

## Pitch

- A note has a degree and a period. Degree selects the degree in the period. Period moves the note by whole periods (octaves in most scales).
- The scale mask (MASK pages) limits the degrees. The Degree knob and the pitch operations use only the degrees in the mask.
- Root and Root Tune (OUTPUT page) set the pitch of degree 0.

## Chance

Each note has 4 chance lanes (CHANCE page). For each lane, set:

- Op: what happens (for example MUTE, JMP?, VEL?, RATCH, P?+, GLIDE).
- Prob: how often it happens.
- PTN: on which loop repeats it can happen. `1:4` = only in the first of every 4 repeats.
- Amount: how much (steps, points, hits or degrees, as the operation needs).

The lanes act in the order 1 to 4.

## Locks and CC lanes

- LOCK page: set Lock CC 1-4 to CC numbers. Then set the lock values of a note. The note sends these CCs just before it plays, on the first output channel.
- LANE page: select CC 1 or CC 2. Set CC Num, Steps and Rate. Draw the values with a finger. The lane sends its CC only when the value changes. When the MPC stops, it sends the Rest value.
- The CC numbers skip 0-5, 7, 10, 11, 32, 64-68, 98-101 and 120-127.

To connect a CC to a parameter of the MPC, use MIDI Learn:

1. Keep the MPC stopped. Touch SEND next to the CC. It sends the CC for 15 s.
2. Menu → MIDI Learn. Tap Learn, then +. The MPC finds the CC.
3. Set Source (the track or pad) and Target (the parameter).
4. Touch SEND again to stop it.

## Sequences and song arrangement

- There are 32 sequences. Select one with the Sequence knob (VIEW bank or SETUP page).
- MIDI notes 60-91 on the scalaSeq track select sequences 1-32. Record these notes in the MPC sequence of the scalaSeq track to arrange a song.
- While the MPC plays, a sequence note starts the new sequence at once. The Sequence knob starts it at the end of the current loop.
- COPY copies the sequence. PASTE (touch two times) puts it into the selected sequence. CLEAR (touch two times) empties the sequence.

## Steps and kept notes

- When you make Steps smaller, the notes after the new end are kept. A note that crosses the end plays cut.
- When you make Steps larger again, the kept notes come back. A cut note gets its full length back, if you did not edit it.
- "+N kept" at the right end of the ruler shows how many notes are kept.
- Kept notes count toward the limit of 256 notes in a sequence.
- A Range End at the last step follows Steps.

## Limits

- 256 notes in a sequence (kept notes included).
- One note at a time (monophonic). Notes of different pitches can overlap: the later note cuts the earlier note.
- The pitch accuracy depends on the target: keygroups within 0.3 cents, OPx-4 within 3.3 cents (measured with Scala Retuner, which uses the same bend values).
- scalaSeq sends MIDI through its own ALSA port. This is not part of the official SDK, and a later MPC OS can change it.
