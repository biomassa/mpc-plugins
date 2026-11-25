# percSeq: user guide

percSeq is a step sequencer for drum tracks and chopped samples on Akai MPC standalone hardware. It has 16 rows. Each row plays one MIDI note. percSeq makes no sound. It sends its notes to other tracks through its own MIDI port, "percSeq MIDI Out".

For the parameter tables and the technical details, see the [README](percSeq/README.md). For the problems that we know, see [knownIssues.md](percSeq/knownIssues.md).

## Features

- 16 rows. Each row is one sound: a pad of a drum track or a slice of a chopped sample.
- Each row has its own playhead: Rate, Steps (1-64), Range Start, Range End, Direction (forward, reverse, random) and Swing. Rows with different lengths and rates make polyrhythms.
- Ratio rates: a row can play at its Rate multiplied by a ratio (for example 1.33 or 0.75), with a fine adjustment.
- Each hit has a start, a length, and the values VEL, RATCH (1-8 hits in the hit), FADE and DUTY.
- 4 chance lanes for each hit. Each lane has an operation, a probability and a pattern. There are 31 operations: mute, skip, jumps, velocity, timing, repeats, sound changes, FLAM, sample reverse and CC.
- A pool of sounds. The sound operations (SND+, SND?, SND NEXT and others) play a different row from the pool.
- 4 lock values for each hit (p-locks). Each row has 4 Lock CC numbers. A lock sends its CC just before its hit.
- 2 CC lanes for each row. Each lane has its own steps, rate and direction, and sends one CC.
- 32 sequences. MIDI notes on the percSeq track select them, so you can arrange a song in the MPC sequencer.
- COPY, PASTE and CLEAR of a sequence. The copy is shared by all percSeq instances.
- Hits outside Steps are kept. When you make Steps smaller and then larger again, the hits come back.
- Step entry with the MPC pads.
- percSeq follows the MPC transport and song position.

## What you need

- An Akai MPC with MPC OS 3.10.1 or later. percSeq does not run in the MPC desktop software.
- Preferences → Enhancements (Beta): Enable Enhancements and External Plugin Support on.
- The plugin folder in `VST2 Plugins` on the internal storage. On the MPC One G2 the SD card cannot load plugins.

## Set up

1. Add percSeq to a track (External Plugins). Set the MIDI Input Port of this track to the pads or your controller. Do not use "All Ports".
2. Make a drum track. Set its MIDI Input Port to "percSeq MIDI Out", its MIDI Input Channel to 1 (or All), and MIDI Monitor to In.
3. The rows play notes 36-51 (row 1 = note 36). Set the pad note map of the drum track so that these notes play your pads. Or set the MIDI Note of each row on the ROW page.
4. Preferences → MIDI / Sync → Input Ports, "percSeq MIDI Out": Global off, Track on, Control on. With Global on, the notes come back into percSeq and make a loop.
5. Press Play on the MPC. percSeq plays when the MPC plays.

After an update of percSeq, remove it from the track and add it again.

## The pages

| Page | What it does |
|---|---|
| SEQ | The grid of 16 rows. Draw, move and delete hits. COPY and PASTE (header): copy the selected hit, then push PASTE and touch where it goes. The Q-Link banks are at the right. |
| HIT | All values of the selected hit, and step entry. |
| CHANCE | The 4 chance lanes of the selected hit: Op, Prob, PTN, Amount. Hit < > and Row < > at the top select the hit. |
| LANE | The velocity of the row's hits, and the 2 CC lanes of the row. Draw the lane values with a finger or with Q-Link column 1. |
| LOCK | The 4 lock values of the selected hit, Lock CC 1-4 of the selected row (each with SEND), Hit, Row and Sequence. |
| ROW | The selected row: MIDI Note, Velocity, and under CHANCE OP CCs the CCs of the SMP REV and CC chance operations: REV CC, OP CC, OP REST (each CC with SEND, for MIDI Learn). 16 pads select the row. Push the ROW button again for RATE. |
| RATE | The timing of the selected row: Rate, Mode, Ratio, Fine, Steps, Range Start, Range End, Direction, Swing. |
| POOL | 16 pads under the header ROWS THAT SND+ / SND- / SND? CAN PICK. A lit pad is in the pool of the sound operations. |
| SETUP | Sequence, COPY, PASTE, CLEAR, Steps/Page, Group, Restart Rate, PTN Count, Reset, Max Steps, Restart, Pad 1 Note (and LAST PAD), Out Ch, Clear Ops, Clear All Ops. |

The MPC shows 5 page buttons at a time: touch > for the next pages. On every page, each Q-Link column holds one set of controls. The orange frame shows the column that the 4 Q-Links control.

## Make a pattern

On the SEQ page:

- Touch an empty place and drag: a new hit. The drag sets its length. A tap gives a hit of Entry Len.
- Touch a hit: select it. Drag it to move it in time or to another row. Drag its right end to change its length.
- Tap a hit two times quickly: delete it.
- Hold FREE with a second finger: the drag does not snap.
- Touch a row label at the left: switch the row on or off.
- On the ruler: swipe sideways to go to the next or previous page. Drag up or down to zoom.

The labels at the left show the row number (grey) and its note (white). The row numbers also show at the right of the grid. Each row shows its current step as a light square and its end as a grey bar.

The Q-Link panel at the right has 4 columns: HIT (Hit, Row, Move, Length), SHAPE (VEL, RATCH, FADE, DUTY), ENTRY (Nudge, Entry Len, Snap, Page) and ROW (Sel Row, Rate, Steps, Sequence). The HIT page has the same first 2 columns. Tap a value to put the Q-Links on its column. Drag a value up or down to change it. Double-tap a value to open the big editor.

With the pads:

1. On the HIT page, set Step Entry to on.
2. Pad 1 is row 1, pad 16 is row 16. A pad adds a hit at the cursor and moves the cursor by Entry Len.
3. If the pads do not match the rows, see LAST PAD (SETUP page, under Pad 1 Note) while you push pad 1. Set Pad 1 Note to that value.

## Shape the hits

- VEL, RATCH, FADE and DUTY are on the HIT page and in the SHAPE bank.
- RATCH plays 2-8 short hits in the length of the hit. FADE makes them louder or softer. DUTY sets their gate length.
- Velocity of a row (ROW page) scales all hits of the row.

## Chance

Each hit has 4 chance lanes (CHANCE page). For each lane, set:

- Op: what happens (for example MUTE, JMP?, VEL?, RATCH, SND?, FLAM).
- Prob: how often it happens.
- PTN: on which loop repeats it can happen. `1:4` = only in the first of every 4 repeats.
- Amount: how much (steps, points, hits or a CC value, as the operation needs).

The lanes act in the order 1 to 4. On the SEQ page, a hit with a chance lane has a mark.

Sound operations play another row of the pool. Put rows in the pool on the POOL page.

## Locks and CC lanes

- LOCK page: set Lock CC 1-4 of the row to CC numbers. Then set the lock values of a hit. The hit sends these CCs just before its note.
- LANE page: select CC 1 or CC 2. Set CC Num, Steps and Rate. Draw the values with a finger. A red line under the bars shows the current step. The lane sends its CC only when the value changes. When the MPC stops, it sends the Rest value.
- Draw with the Q-Links: on the LANE page, Q-Link column 1 (left of the lane) has STEP and VALUE. STEP (knob 1, or < >) moves the cursor (a teal column); VALUE (knob 2) sets the value at the cursor. A touch on the lane also moves the cursor. With VEL shown they are HIT (select the hit of the row) and VEL.
- The CC numbers skip 0-5, 7, 10, 11, 32, 64-68, 98-101 and 120-127. MIDI uses these numbers for other functions.

To connect a CC to a pad parameter of the MPC, use MIDI Learn:

1. Keep the MPC stopped. Touch SEND next to the CC. It sends the CC for 15 s.
2. Menu → MIDI Learn. Tap Learn, then +. The MPC finds the CC.
3. Set Source (the track or pad) and Target (the parameter).
4. Touch SEND again to stop it.

## Sequences and song arrangement

- There are 32 sequences. Select one with the Sequence knob (ROW bank, LOCK page or SETUP page).
- MIDI notes 60-91 on the percSeq track select sequences 1-32. Record these notes in the MPC sequence of the percSeq track to arrange a song.
- While the MPC plays, a sequence note starts the new sequence at once. The Sequence knob starts it at the next bar line.
- Reset (SETUP page) starts all rows again every N bars.
- Max Steps (SETUP page, per sequence, default 7): no row plays more than this many of its own steps. The row keeps its Steps.
- Restart (SETUP page) with Restart Rate: all rows start again every Max Steps x Restart Rate. Each row keeps its own rate.
- COPY copies the sequence. PASTE (touch two times) puts it into the selected sequence. CLEAR (touch two times) empties the sequence.

## Steps and kept hits

- When you make Steps smaller, the hits after the new end are kept. A hit that crosses the end plays cut.
- When you make Steps larger again, the kept hits come back. A cut hit gets its full length back, if you did not edit it.
- "+N kept" at the right end of the ruler shows how many hits of the selected row are kept.
- Kept hits count toward the limit of 64 hits in a row.
- A Range End at the last step follows Steps.

## Limits

- 64 hits in a row (kept hits included).
- The SEQ page draws 32 hits at a time. "more" shows when more hits are in view.
- percSeq sends MIDI through its own ALSA port. This is not part of the official SDK, and a later MPC OS can change it.
