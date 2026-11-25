# percSeq

percSeq is a 16-row hit sequencer for MPC drum tracks and chopped samples, for Akai MPC standalone hardware (MPC OS 3.10.1 or later). It is the percussion version of [scalaSeq](../scalaSeq/README.md). It makes no sound. It plays its hits as MIDI notes on another track.

> This software was developed with the assistance of a LLM.
>
> **The plugins are in a VERY EARLY DEVELOPMENT STAGE.** Expect bugs, missing features and changes that break saved projects. A plugin bug can freeze the MPC: save your work often.

The user guide is in [docs-percSeq.md](../docs-percSeq.md). This README has the details.

## How it works

- A sequence has 16 rows. Each row is one sound: one MIDI note (a pad of a drum track, or a chop slice). Row 1 is at the bottom.
- Each row has its own playhead with its own Rate, Steps, Range Start, Range End, Direction and Swing. Rows with different lengths and rates make polyrhythms.
- Hits of different rows can play at the same time. Hits of one row do not overlap: an edit stops at the neighbour.
- Each hit has a start and a length in steps of its row, and the lanes VEL, RATCH, FADE and DUTY.
- Each hit has 4 chance lanes, as in scalaSeq. The pitch operations of scalaSeq change the sound (the row) of a hit: SND+, SND-, SND?, SND?+, SND?-, SND HOLD, SND PREV, SND NEXT. New operations: FLAM, SMP REV and CC.
- percSeq follows the song position of the MPC. It plays when the MPC plays and stops when the MPC stops.
- percSeq sends all hits on one MIDI channel (Out Ch) through its own MIDI port. The MPC shows this port as "percSeq MIDI Out". The MPC ignores MIDI that a plugin sends to the host, so percSeq opens its own ALSA sequencer port, as scalaSeq does. This is not part of the official SDK host contract.
- There are 32 sequences. MIDI notes 60-91 on the percSeq track select them (see "Arrange the sequences").

## After an update

Remove percSeq from its track and add it again. Otherwise the MPC can keep the Q-Link assignments of the old version.

## Requirements

- An Akai MPC with MPC OS 3.10.1 or later. percSeq runs only on standalone hardware.
- Preferences → Enhancements (Beta) → Enable Enhancements and External Plugin Support set to on.

## Setup on the MPC

1. Put percSeq on a track. Set the MIDI Input Port of this track to your controller or the pads (for step entry, preview and sequence notes). Do not use "All Ports".
2. Make a drum track (or a track with a chopped sample). On this track, set:
   - MIDI Input Port: percSeq MIDI Out.
   - MIDI Input Channel: Out Ch (SETUP page, default 1), or All.
   - MIDI Monitor: In.
3. The rows send notes 36-51 (row 1 = note 36). Set the Pad Note Map of the drum track to "Chromatic C1", or set the MIDI Note of each row (ROW page) to the note of its pad. Not checked on the MPC: the default pad notes of a new drum track, and that "Chromatic C1" starts at note 36.

## Make a sequence

The SEQ page is a roll of 16 rows. From top to bottom: the header, the ruler (step numbers) and the roll. The velocity lane is on the LANE page (1.3.0). The labels at the left show the row number (grey) and its note (white), for example "1 36". The row numbers show again at the right of the roll. The Q-Link banks and the FREE button are at the right.

On the LANE page, a thin red line under the bars shows the current step: of the selected row for VEL, of the lane for CC 1 and CC 2.

When you make Steps smaller, the hits outside it are kept, not deleted: they come back when Steps goes up again. "+N kept" at the right end of the ruler shows how many hits of the selected row are kept.

The Q-Link banks react to touch. Tap a value to select its Q-Link column. Drag up or down on a value to change it. Double-tap a value (or push Enter) to open the big knob editor. Hit, Row, Move, Length and Sel Row have no drag: use the Q-Link or the big knob.

The selected hit is drawn as in the Max roll: a dim teal body with its ratchet hits on top. Each ratchet hit starts at its place in the hit and is DUTY of a ratchet hit long; its brightness (4 steps: 30, 55, 80, 100 %) follows its velocity (VEL with FADE). With RATCH 1 the whole hit is one ratchet hit, its brightness by VEL. The other hits show their ratchets as thin gaps.

Each row is drawn in its own steps: step 3 of a 1/16 row and step 3 of a 1/8T row are in the same column. The current step of each row shows as a light square behind the hits (its playhead). Each row has an end bar (grey) at its Steps.

Touch on the roll (as scalaSeq):

- Press on an empty place and drag: a new hit. It starts on the nearest snap line; the drag sets its length, also to the left. A tap gives a hit of Entry Len.
- Press a hit: select it (and hear it). Drag the body: move it in time and to other rows. Drag the right third: change the length.
- Tap a hit twice within 300 ms: delete it.
- Hold FREE with a second finger: drags do not snap (1/96 step).
- Touch a row label: switch the row on or off. The small square left of the label shows the state: amber = on, grey = off. An off row is shaded. It stops and sends nothing, also when a sound operation of another row picks it. Switched on while the MPC plays, it starts at its Range Start at the next bar line. Each sequence has its own on and off rows.

Other touch areas:

- Ruler: swipe sideways (60 px) to flip one page (finger to the left = the next page); drag up or down to zoom. Steps/Page is 8 (default), 16, 32 or 64 steps; 24 px of drag changes it by one size.
- Header: < and > flip one page; the page number is between them ("2/4"). The last page holds the end of the longest row. FOLLOW (lit amber when on): while the MPC plays, the page follows the selected row's playhead. COPY takes the selected hit with all its settings; PASTE (lit while it waits) puts the copy where you touch the roll next: that row, the Snap line under the finger plus the copied Nudge, shortened if there is no room. Push PASTE again to stop waiting. The copy is shared by all percSeq instances.
- Velocity lane: it shows the hits of the selected row. Drag a bar up or down to set the VEL of the hit.

The SEQ page draws up to 32 hits. If more hits are in view, "more" shows at the right end of the ruler; zoom in or flip the page to see them.

The Q-Links (the 4 banks show right of the roll; the bank names are not shown on the screen). Columns 1-2 are the same on the HIT page:

| Bank | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
|---|---|---|---|---|
| HIT | Hit (select, in the selected row) | Row (move the hit to a row) | Move (snap steps) | Length (snap steps) |
| SHAPE | VEL | RATCH | FADE | DUTY |
| ENTRY | Nudge (-96 to +96 /96 step, from the Move position) | Entry Len | Snap | Page |
| ROW | Sel Row (select a row; shown as "Sel") | Rate (of the selected row) | Steps (of the selected row) | Sequence |

Program Mode Q-Links (the knobs when they control the plugin, not the screen), one column each: Sequence, Range Start, Range End, Direction / Rate, Swing, Ratio, Fine (of the selected row) / Prob 1-4 / Sel Row, Hit, VEL, RATCH.

With the pads:

1. Set Step Entry to on (HIT page).
2. Pads 1-16 are rows 1-16. Each pad adds a hit at the cursor in its row, and the cursor moves on by Entry Len. The pads also play the row's note.
3. If the pads do not match the rows, look at LAST PAD (SETUP page, under Pad 1 Note) while you press pad 1, and set Pad 1 Note to that value. Pad 1 Note cannot be 45-91 (the sequence notes): the knob jumps from 44 to 92.

Press Play on the MPC to play the sequence.

## Arrange the sequences

MIDI notes 60-91 on the percSeq track select the sequences: note 60 is sequence 1, note 91 is sequence 32. Record these notes in the MPC sequence of the percSeq track.

- While the MPC plays, all rows of the new sequence start at their Range Start at the time of the note. A note off does nothing.
- While the MPC is stopped, the note selects the sequence. Play starts it.
- The Sequence knob also selects a sequence. While the MPC plays, the knob's sequence starts at the next bar line.
- When the MPC jumps (Play, a loop back, a locate), each row plays from the step that the song position gives.

## Reset

Reset (SETUP page, off or 1-64 bars): every N bars of the song, all rows start again at their Range Start. With Reset off, the rows line up only at the song start and at MPC jumps.

## Max Steps and Restart

Max Steps (SETUP page, 1-64, per sequence; a new sequence has 7): no row plays more than Max Steps of its own steps, and no CC lane more than Max Steps of its lane steps. Each row keeps its own Steps, Range, Rate and Ratio. With Max Steps 7, a row with Steps 16 plays steps 1-7; set Max Steps to 16 or more and it plays 16 again. A hit that crosses the end plays cut at the end; a hit after the end does not play. The end bar of the row on the SEQ page shows where it ends; a see-through dark shade covers the row from its end to the right edge (after Steps, or after Max Steps when that is smaller). The LANE page shades the lane after the steps that play. On the RATE page, Steps shows "16 (7)" while Max Steps (7) caps a row with Steps 16.

Restart (On/Off) and Restart Rate (1/64 ... 1 bar, default 1/16): with Restart on, all rows and CC lanes start again at their Range Start every Max Steps x Restart Rate (7 x 1/16 = 7 sixteenths), counted from the song start. Between two restarts each row plays at its own rate, also with Ratio. A row whose loop is longer than the cycle is cut at each restart. Reset and Restart can be on together.

Projects saved before Max Steps load with Max Steps 64 and Restart off: they play as before.

## Controls on the pages

- Number parameters are faders. FADE and Nudge fill from the centre.
- Hit, Row, Move and Length (HIT page) are step controls: touch < or > to step one value (the Q-Link also turns them). They stop at the first and last value. Move and Row skip places where the hit does not fit.
- Short lists (Snap, Steps/Page, Direction, PTN Count) are pads: the lit (amber) pad is the current choice; touch another pad to choose it.
- Long lists (Op, PTN, Rate, Entry Len) are fields that show the current choice. Touch the field: a list of buttons opens over the page (the current choice lit amber). Touch a choice to pick it; the list closes. X closes the list without a change. The Q-Link of the field also steps through the choices.
- Defaults of a new instance: Snap 1, Entry Len 1, Steps/Page 8.

## The port settings on the MPC

Preferences → MIDI / Sync → Input Ports, entry "percSeq MIDI Out" (it is listed while percSeq is loaded):

- Global: OFF. With Global on, the port's MIDI goes to the current track (MPC guide p61). With the percSeq track selected, its notes come back into percSeq and go out again: a loop (seen 2026-11-22: the pads blinked fast and the sound broke up).
- Control: ON for MIDI Learn and SEND. With Control off, nothing from the port reaches MIDI Learn (guide p61).
- Track: ON, so tracks can select the port as their MIDI Input.

The MPC stores these switches under the port's ALSA number, not its name. If another plugin with a MIDI port loads first, the port can show up as a new entry with the switches reset.

## MIDI Learn with SEND

The MPC changes pad and track parameters from a CC only through MIDI Learn (MPC guide p319). percSeq and scalaSeq reach the MPC as an external MIDI port, so they cannot set pad parameters directly. To map a CC without typing its number:

1. Set the CC number in percSeq (Lock CC, lane CC Num, Rev CC or CC Num). Keep the MPC stopped.
2. Touch SEND next to that CC. It lights and sends the CC as a zigzag (0 and 127 in turn, every 250 ms).
3. Within 15 s: Menu, MIDI Learn, tap Learn (on), tap +, select the new row. The MPC fills in Type, Ch and Data.
4. Choose Source (the track or pad) and Target (the parameter).
5. Go back to percSeq and touch SEND again to stop it (it sends 64 at the end), or wait 15 s.

MIDI Learn turns off when you leave its page, and it ignores a single CC message (measured), so SEND keeps sending while you switch pages. Learn takes the first message that arrives: do not play the sequence while Learn is on.

## Pages

| Page | Contents |
|---|---|
| SEQ | The roll (see above). |
| HIT | The selected hit, one column for each set: Hit, Row, Move, Length / VEL, RATCH, FADE, DUTY / Step Entry, Cursor, Snap, Entry Len / Nudge (-96 to +96 /96 step from the Move position, independent of Snap), DELETE. DELETE removes the selected hit (one touch; it has no Q-Link). |
| CHANCE | Header: Hit < > and Row < > select the hit; HIT and ROW as on the SEQ page. 4 chance lanes for the selected hit, one column for each lane: Op, Prob, PTN, Amount. |
| LANE | Lane pads VEL / CC 1 / CC 2, Row < >, page < >, FOLLOW. VEL: the velocities of the row's hits. CC 1, CC 2: the row's CC lanes; drag across the bars to draw. The lane starts at Q-Link column 2 and runs to the right edge (918 px). Q-Link column 1, left of the lane: STEP (Lane Step, a < value > control: the cursor, a teal column as wide as the step; a touch on the lane also moves it) and VALUE (Lane Value, a fader: the value at the cursor, 0-127). With VEL shown they are HIT (selects the hit of the row) and VEL (0-127 = 0-100 %). Settings: CC Num, Ratio / Steps, Fine / Rate, Direction / Mode, Rest (sent at stop). A lane sends its CC only when the value changes. |
| LOCK | 4 lock values of the selected hit (off or 0-127) / Lock CC 1-4 of the selected row (off or a CC number), each with SEND / Hit < >, Row < > / Sequence. A lock is sent just before its hit's note. |
| ROW | The selected row: Row, MIDI Note, Velocity / CHANCE OP CCs: REV CC (Rev CC), OP CC (CC Num), OP REST (CC Rest), each CC with SEND. These are the CCs of the SMP REV and CC chance operations (see Sound operations). At the right, 16 row pads (laid out like the MPC pads, row 1 at the bottom left): the lit pad is the selected row; touch a pad to select a row. Push the ROW button again for RATE. |
| RATE | The timing of the selected row: Rate, Mode (Musical / Ratio), Ratio, Fine (Ratio mode: the step is Rate divided by Ratio + Fine) / Steps, Range Start, Range End, Direction / Swing / Row < >. |
| POOL | Header: ROWS THAT SND+ / SND- / SND? CAN PICK. 16 pads laid out like the MPC pads (row 1 at the bottom left). Each pad shows its row and note. A lit pad is in the pool: the sound operations can pick it. Touch a pad to put it in or out. Q-Link 1-16 = row 1-16. |
| SETUP | Sequence, COPY, PASTE, CLEAR (PASTE and CLEAR: touch twice within 3 s; the copy is shared by all percSeq instances) / Steps/Page, Group, Restart Rate (under Group) / PTN Count, Reset, Max Steps, Restart / Pad 1 Note (LAST PAD under it), Out Ch, Clear Ops (the chance lanes of the selected hit), Clear All Ops (of all hits; touch twice within 3 s). |

The page order is SEQ, HIT, CHANCE, LANE, LOCK, ROW (RATE), POOL, SETUP. On each page a Q-Link column holds one set of controls ("/" separates the columns above). The orange Q-Link frame starts above the top control, so that the MPC's Q badge does not cover its name (not yet seen on the MPC).

### Row settings

| Setting | Range | Function |
|---|---|---|
| MIDI Note | 0-127 (default 36-51) | The MIDI note of the row. |
| Velocity | 0-100 % (default 100) | Scales the velocity of all hits of the row. At 0 % the row sends nothing. |
| Rate | 1/64, 1/32T, 1/32, 1/16T, 1/16Q, 1/16, 1/8T, 1/8Q, 1/8D, 1/8, 1/4T, 1/4, 1/4D, 1/2, 1 bar (default 1/16) | The note value of one step. Q = quintuplet, D = dotted. |
| Steps | 1-64 | The length of the row's loop (at most Max Steps of its sequence play). |
| Max Steps | 1-64 (default 7) | Per sequence: no row or CC lane plays more steps. |
| Restart, Restart Rate | Off/On (default Off); 1/64 ... 1 bar (default 1/16) | Per sequence: all rows start again every Max Steps x Restart Rate. |
| Range Start, Range End | 1-64 | The play range. |
| Direction | Fwd, Rev, Rnd | |
| Swing | 50-75 % | Swing of pairs of steps. |
| Rev CC | 0-127 (default 102-117) | The CC that SMP REV sends. |
| CC Num, CC Rest | 0-127 (default 14-29, rest 64) | The CC that the CC operation sends, and its value after a hit without the operation. |

### Lanes

| Lane | Range | Function |
|---|---|---|
| VEL | 0-100 % | Velocity. |
| RATCH | 1-8 | Hits in the hit. |
| FADE | -50 to +50 | Velocity of the ratchet hits: below 0 rises, above 0 falls. Only with 2 or more ratchets. |
| DUTY | 0.05-0.95 | Gate length of each ratchet hit. Only with 2 or more ratchets. |

### Chance operations

Each lane has an operation (Op), a probability (Prob), a pattern (PTN) and an amount. The lanes act from lane 1 to lane 4. PTN `A:B` plays the operation on the A-th of every B loop repeats of the row (or bars, with PTN Count = Bars).

| Group | Operations |
|---|---|
| Play | MUTE, UNMUTE, SKIP, UNSKIP |
| Jump (this row only) | JMP< (steps back), JMP? (random step), JMP> (steps forward), JMP TO (step number), REVERSE (plays the row backwards until its loop restarts) |
| Velocity | VEL- , VEL+ (points), VEL MAX, VEL? (random ± points) |
| Time | GATE-, GATE+ (fractions of a step), TIE, DELAY, DELAY % |
| Repeat | RATCH (hits), REPEAT (extra plays of the step) |
| Sound | SND+, SND- (pool rows up or down; wraps in the pool), SND?+, SND?-, SND? (random pool rows; Amount "pool" = the whole pool), SND HOLD (the sound that the last hit played, any row), SND PREV (the row of the last hit, any row), SND NEXT (the row whose next hit comes first) |
| Percussion | FLAM, SMP REV, CC |

- FLAM: a grace hit before the hit. Amount = the distance in 1/96 of a step of the row (1-48, default 8). The grace hit has half the velocity and the same sound.
- SMP REV: percSeq sends Rev CC = 127 of the sound's row before the hit, and Rev CC = 0 before the next hit of that row without SMP REV. Map the Rev CC of each row with MIDI Learn to the Layer Reverse of its pad (see below).
- CC: percSeq sends CC Num = Amount (0-127) of the sound's row before the hit, and CC Num = CC Rest before the next hit of that row without the operation. Map it with MIDI Learn to any pad parameter, for example Amp Env Decay.
- percSeq sends these CCs only when the value changes. A sound operation changes the row whose Rev CC and CC Num are used.
- When the MPC stops, percSeq sends Rev CC = 0 and CC Num = CC Rest for each row that is not at rest.
- SND NEXT looks at the next hit start of each row forward in its play range. It does not look at Direction, jumps or chance operations.

### MIDI Learn for SMP REV and CC

1. Preferences → MIDI: switch on Track and Control for the "percSeq MIDI Out" input.
2. Menu → MIDI Learn: tap Learn, then +. When percSeq plays a hit with SMP REV or CC, the MPC fills in Ch and Data.
3. Set Source to the drum track and the pad, and Target to Layer Reverse (1-8) or Amp Env Decay. Tap Learn again.

Reported by the user (2026-11-21, MPC One G2): a CC from a test program on the same ALSA path, learned with MIDI Learn, switched the Layer Reverse of a pad (the pad was played by hand). Not measured: if a CC that comes just before a note acts on that same hit.

## CPU

- Each row runs the scalaSeq engine per audio sample. Rows without hits move only their playhead (once per block).
- Host simulator (desktop): 0.04 % of a block with an empty pattern, about 0.5 % with hits in all 16 rows. Not measured on the MPC.
- Measured 2026-11-25 (desktop, 128-frame blocks, 4 hits in each of the 16 rows, lanes off, playing): process() median 17 us (0.6 % of a block), p99 35-42 us (was 21 us / 50 us before the 2026-11-25 changes). Not measured on the MPC One G2 (aarch64, Cortex-A55); the SDK gives no factor for this CPU. Most of it is the engine of each row, every sample (profile: Engine::tick and tickRow).
- 2026-11-25: a row that stays idle for a whole block (no hit sounds, waits or starts, and its step does not change) is ticked once at the block end instead of every sample. Measured on the desktop: 4 short hits per row 17 -> 2.6 us per block (median), p99 about 23 us; a dense pattern (a full-length hit on every step of every row) about 20 us, as before. Not while a hit uses SND NEXT (it reads every row's position). A differential test checks that every message and step is identical to the per-sample path.
- After a project load the copy for project saving is made on the UI thread (2026-11-25). The audio thread copies the loaded model into place once (reset()), and after an edit only the edited sequence (60.8 KB).
- All rows run half a step ahead of the song, so that a FLAM grace hit can come before its hit. The hits wait in a queue until the song reaches them.

## Build and test

From the SDK root (`sdk/`):

```sh
python3 tools/build.py plugins/percSeq
python3 plugins/percSeq/skin/make_skin.py
python3 tools/build.py plugins/percSeq
.sdk-tools/host_sim plugins/percSeq/build/native/percseq.dylib test --wav plugins/percSeq/build/test.wav
sh plugins/percSeq/tests/run.sh
```

The skin in `skins/` is hand-written. `skin/make_skin.py` makes it again: the SEQ page, and the other pages from `ui.json`. Run it after each change to `ui.json` or to the parameters, then build again.

`tests/run.sh` tests step entry and the Pad 1 Note limit, playback with own rates and lengths per row, the time of each MIDI message, row Velocity, FLAM, SMP REV, CC, SND+ with the pool, SND PREV, sequence notes, the Sequence knob at the bar line, Reset, a jump, touch (tap, row label), the Row knob, the ROW page, the step squares, row on/off (silent, next-bar start, as a sound-op target, per sequence) and the project state.

The shared code (ALSA port, real-time helpers) is in `../common/`.

See the [changelog](CHANGELOG.md) and the [known issues](knownIssues.md).

## License

MIT. Based on scalaSeq and `dingus.pitchGateSeq` from the dingus Max package by the same author.
