# Changelog

This file records all notable changes to percSeq.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- SEQ page (2026-11-25, user: hits seen for a moment while setting, moving or deleting hits): each shown hit keeps its drawing slot. Before, the slots were filled in row order, so one new or deleted hit moved every later hit to another slot. That took up to about 200 display reports, which go out 30 per 50 ms pass, and the passes between showed bars in wrong places or missing. A new hit now takes 39 reports (2 passes), a delete 22, a drag 12-20 (desktop test, 31 hits shown). A new test runs the passes one by one, from every start position of the pass, and requires that no frame shows a bar that is in neither the old nor the new picture; it fails with the old code (201 reports, 17 wrong bars).

### Changed

- ROW page (2026-11-25, user): the header CHANCE OP CCs is above column 2, and its labels are REV CC, OP CC and OP REST (were Rev CC, CC Num, CC Rest). The parameter names and ids did not change. The controls and Q-Link frames moved 17 px down under the header, as on CHANCE.
- POOL page (2026-11-25, user): the header ROWS THAT SND+ / SND- / SND? CAN PICK is at the top. The pads moved 17 px down (same size); the Q-Link frames start under the header.

- Performance (2026-11-25, no change in what plays): a row that stays idle for a whole block (no hit sounds, waits or starts; the step does not change; the next hit does not start) is ticked once at the block end instead of at every sample. Not in a block with a sequence note, a jump, a pending switch, a Reset or a row start, and not while a hit uses SND NEXT. Desktop: 17 -> 2.6 us per block with short hits (median); unchanged (about 20 us) with a full-length hit on every step. A new differential test plays random patterns (all ops, swing, ratio rates, directions, Reset, switches, jumps, block sizes 32-512) in two instances, one forced to the per-sample path, and requires identical messages and steps; it fails when the idle check is broken on purpose.

- HIT page (2026-11-25): Snap is above Entry Len (was below). The Q-Link knobs 3 and 4 of that column swapped with them.

- Performance (2026-11-25, no change in what plays): the CC lanes of each row are checked once per block (before: every lane, every sample, also with the lane off); the bar line and Reset checks run their exact floor test only near the boundary (before: 3 floor divisions per sample). Measured on the desktop with 16 rows of hits: 21 -> 17 us per block (median), p99 50 -> 35-42 us. After a project load the copy for project saving is made on the UI thread; before, the audio thread also copied the whole model (1.9 MB) after the load and on the first block after the plugin was added.

- SEQ page: the selected hit is drawn as in the Max roll (user, 2026-11-25): a dim teal body (30 %) with its ratchet hits on top. Each ratchet hit starts at its place and is DUTY of a ratchet hit long; its brightness (4 steps: 30, 55, 80, 100 %, the nearest to 0.15 + 0.85 x velocity) follows its velocity (VEL with FADE, as the engine). With RATCH 1 the whole hit is one hit, its brightness by VEL. The other hits keep the ratchet gaps. New display parameters for the hit markers (292, added at the end).

- LANE header (2026-11-25, user: the buttons were too hard to hit on the MPC): the buttons are 56 px tall (were 40), the lane pads 100 px wide (were 90), the < > buttons 64 px wide (were 40), FOLLOW and SEND 80 px wide. The lane starts at y 60 and is 250 px high (was 260); its bottom did not move. The SEQ and LOCK pages keep their buttons.

- LANE (2026-11-25, after a test on the MPC One G2: the drawing knobs worked but nothing on the screen named them): the lane starts at Q-Link column 2 and runs to the right edge (x 343-1261, 918 px; was x 70-950). Q-Link column 1, left of the lane, shows the two drawing controls: STEP (Lane Step, a < value > control) and VALUE (Lane Value, a fader); with VEL shown they are named HIT / VEL. The header text STEP / VALUE is removed; the lane pads and SEND moved with the lane. New parameters Lane Prev and Lane Next (the < > buttons, added at the end).

- Redesign of the pages and Q-Links (user, 2026-11-24). The MPC One G2 has 4 Q-Links: they control one column at a time, and the Q-Link button stops on every column, also on an empty one. Each Q-Link column now holds one set of controls.
- **After this update, remove the plugin from its track and add it again.** The Q-Link assignments changed, and the MPC keeps the old ones in a project until the plugin is added again (measured before).
- SEQ page, Q-Link panel: the columns are HIT (Hit, Row, Move, Length), SHAPE (VEL, RATCH, FADE, DUTY), ENTRY (Nudge, Entry Len, Snap, Page) and ROW (Sel Row, Rate, Steps, Sequence). The bank titles are removed (the MPC's Q badge covered the title of the active bank).
- HIT page: the same first 2 columns as the SEQ panel; column 3 is step entry (Step Entry, Cursor, Snap, Entry Len); column 4 Nudge and a DELETE button (one touch, as before; no Q-Link).
- Snap (HIT page): the 6 pads are taller (36 px, were 26) and fill the slot.
- Page order: SEQ, HIT, CHANCE, LANE, LOCK, ROW, POOL, SETUP. RATE is a sub-page of ROW: push the ROW button again.
- RATE: Rate, Mode, Ratio, Fine / Steps, Range Start, Range End, Direction / Swing / Row < >. SETUP: Sequence, COPY, PASTE, CLEAR / Steps/Page, Group / PTN Count, Reset / Pad 1 Note, Out Ch, Clear Ops, Clear All Ops.
- LOCK: each Lock CC has a shorter fader with SEND at its right; Hit < > and Row < > (column 3) and Sequence (column 4).
- LAST PAD and PAD 1 moved from the SEQ header to SETUP (LAST PAD shows under the Pad 1 Note fader). The page buttons and FOLLOW moved left; FOLLOW no longer touches STEP ENTRY.
- Clear Note and Clear All are now called Clear Ops and Clear All Ops (they clear the chance lanes). The parameter ids did not change.
- The orange Q-Link frames start 36 px above the top control, so the MPC's Q badge sits above the name and does not cover it. On LANE the frames start under the red playhead line.
- LANE: the lane is 260 px high (was 280), so the Q badge fits above the settings.
- Program Mode Q-Links (the knobs when they control the plugin): Sequence, Range Start, Range End, Direction / Rate, Swing, Ratio, Fine (of the selected row) / Prob 1-4 / Sel Row, Hit, VEL, RATCH.

- SEQ page: row numbers 1-16 (from the bottom) at the right of the roll, 22 px white.
- Steps no longer deletes hits. When Steps goes down, the hits outside it are kept, and a hit that crosses the end plays cut at the end. When Steps goes up again, they come back, and a cut hit gets its full length back. An edit of a cut hit (any change, for example VEL or length) makes the edit count: it keeps its cut length. Kept hits are saved in the project, and COPY, PASTE and CLEAR include them. They count toward the limit of 64 hits per row.
- SEQ page: the right end of the ruler shows "+N kept" (amber) when the selected row has hits outside Steps. It replaces "more" while there are kept hits. The text is now drawn after the ruler, so the ruler cannot cover it.
- SEQ page, Q-Link panel: the 16 values react to touch. A tap selects the Q-Link column of the value (the MPC frames it). A vertical drag on the value changes it. A double-tap (or Enter) opens the big knob editor, also for lists such as Snap and Rate (the MPC's own list overlay opens empty for this plugin). Hit, Row, Move, Length and Sel Row have no drag (the plugin limits these values, and a drag could jump); a tap and a double-tap work on them.
- SEQ page, Q-Link panel: larger text. Names 24 px (were 22), values 34 px (were 32). The names and the 4 bank titles are light grey (e8e6df; were grey).

- LANE page: the current step is a thin red line (4 px) under the bars, as wide as the step. Before, it was a white 25 % column behind the bars, which the bars hid. The VEL lane now shows it too (the selected row's current step); the CC lanes show their own step.
- POOL pads and SEQ row labels: the row number is dim grey (8a8885) and the note is white, so the two numbers are different. They were one text in one colour. New display parameters Note Text 1-16 (added at the end; the parameter count is now 1,337).

### Added

- LANE: draw with the Q-Links. Q-Link column 1, knob 1 (Lane Step) moves a cursor (a see-through teal column as wide as the step, behind the bars) and knob 2 (Lane Value) sets the value at the cursor (0-127). A touch on the lane also moves the cursor. STEP and VALUE show right of SEND. With VEL shown, knob 1 selects the hit of the row and knob 2 sets its VEL (the text shows HIT and VEL). Each row has its own cursors (not saved). New parameters Lane Step, Lane Value and Lane Cur X 1-4 (added at the end).
- CHANCE: a header strip at the top with Hit < >, Row < >, HIT and ROW. The controls moved 17 px down.

### Fixed

- HIT page: the second row of the Snap pads was cut in half (seen on the MPC One G2, 2026-11-25). The pads' container was one slot tall (107 px) and the MPC cuts what is outside it; it is now as tall as the pads. (The first guess, the bottom of the screen, was wrong: the pads were cut in the upper slot too.)

- Pop-up lists: the MPC's orange Q-Link frame was drawn over an open list. The list panels now ask the MPC to hide the frame while they are shown (hideQLinkBounds, component version 4). Seen working on the MPC One G2: the frame is hidden only while a list is open.
- SEQ page, Q-Link panel: the values of the drag cells did not show on the MPC One G2 (the MPC's ValueSlider knob draws no text there). Each value is now a text label, with the drag knob on top of it.
- Range End (RATE page): when Range End is at the last step, it now follows Steps. Before, Steps 16 -> 1 -> 16 left Range End at 1, and only 1 step played. A Range End inside the steps stays; it is clipped when Steps goes below it. When it is clipped to the last step, it follows Steps from then on.

## [1.3.0] - 2026-11-22

### Added

- SEND buttons on LOCK (next to each Lock CC), LANE (header, for the shown lane) and ROW (next to Rev CC and CC Num): a touch starts that CC as a zigzag (0 and 127 in turn, every 250 ms) on Out Ch; SEND stays lit; a second touch, or 15 s, stops it with 64. Touch SEND, go to MIDI Learn (Learn on, +, select the row): Learn takes the CC number without typing. Nothing is sent while the CC is off.
- SETUP: COPY, PASTE and CLEAR for the edited sequence (the Sequence knob). COPY takes the sequence with all its data. PASTE puts the copy into the edited sequence; CLEAR sets it back to a new sequence. PASTE and CLEAR: the first touch arms the button (lit amber), a second touch within 3 s does it. The copy is shared by all percSeq instances (copy on one track, paste on another) and is lost when the MPC app restarts.
- Ratio rates: each row has a Rate Mode, Musical or Ratio (RATE page, Ratio lit teal). In Ratio mode the step is the row's Rate divided by Ratio + Fine. Ratio: 0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 1, 1.25, 1.33, 1.5, 1.67, 2, 2.5, 3, 4. Fine: -0.10 to +0.10 (the Fine value shows the sum, for example 1.38). Ratio rows line up again at the song start, at MPC jumps and at each Reset.
- Locks (LOCK page): each hit has 4 lock values (off or 0-127). Each row has Lock CC 1-4 (default off). A lock is sent just before its hit's note, on the Lock CC of the row whose sound plays (as the CC operation). An unlocked slot sends nothing.
- CC lanes (LANE page): 2 lanes per row. Each lane has CC Num (default off), Steps (1-64), Rate, Mode, Ratio, Fine, Direction (Fwd, Rev, Rnd) and Rest (default 64), and one value per step (default 64). A lane runs with its own playhead and sends its CC only when the value changes (the first step after a start always sends). When the MPC stops, each lane sends its Rest value. A lock on the same CC at the same time wins.
- LANE page: lane pads VEL / CC 1 / CC 2, Row < >, the page < > and FOLLOW (as on SEQ). Drag across the bars to draw the values; a tap sets one bar. The current step of the shown lane is a light square. VEL shows the velocities of the row's hits (moved from the SEQ page).
- CC numbers for Lock CC and the lanes skip 0-5, 7, 10, 11, 32, 64-68, 98-101 and 120-127 (bank select, standard controllers, NRPN/RPN, channel mode).

### Changed

- Pages (in this order): SEQ, LOCK, LANE, CHANCE, HIT, ROW, RATE, POOL, SETUP. The row timing settings (Rate, Mode, Ratio, Fine, Swing, Direction, Steps, Range Start, Range End) moved from ROW to RATE. RATE and LOCK have Row < > (selects the row).
- SEQ page: the roll is taller (540 px, was 476): the velocity lane moved to the LANE page.
- Project state format PSQ3 (PSQ1 and PSQ2 still load, with no locks and lanes).

## [1.2.0] - 2026-11-22

### Added

- Step controls on the HIT page for Hit, Row, Move and Length: the name, a < button, the value and a > button. A tap steps one value; the Q-Link still turns it. Each stops at the first and last value. Move and Row skip places where the hit does not fit and go to the next free place (step, or row) in that direction. These were faders. Reproduced offline: the faders go from 1 to 64 (the MPC reads the range of a parameter once), and percSeq wrote back the value it could use, so the handle sprang back (for example to hit 5 of 64, near the start, with 5 hits in the row).

### Changed

- Text on the pages other than SEQ is larger. Names 34 px and values 38 px (were 24 and 26) for faders, switches, list fields and step controls. On a fader the name and the value use the full width (name at the left, value at the right), so long names are not cut. Pop-up lists: title 34 px, choices 28 px (were 26 and 22); a list with a long choice (Op: SND HOLD) has wider buttons (170 px). List pads: 26 px in one row, 22 px in two rows (were 20 and 17).
- ROW, POOL pads: the row numbers on the ROW pads are 36 px (were 26), the POOL pad labels 44 px (were 30).
- The row's MIDI note setting on the ROW page is now called MIDI Note (was Note). The parameter id is the same, so projects keep the value.

## [1.1.0] - 2026-11-22

### Added

- Row on/off: a tap on a row label on the SEQ page switches the row on or off; the small square left of the label shows the state (amber = on, grey = off). The label no longer selects the row (touch a hit, or use Sel Row). Measured on the MPC One G2: taps for a 20 px toggle square at the screen edge landed on the label area (28-51 px). An off row is shaded; it stops and sends nothing, also when a sound operation of another row picks it. Switched on while the MPC plays, it starts at its Range Start at the next bar line. Each sequence has its own on and off rows (project state format PSQ2; PSQ1 still loads).

### Changed

- Controls: number parameters are faders (FADE and Nudge fill from the centre). Short lists (Snap, Steps/Page, Direction, PTN Count) are amber/grey pads, lit = current. Long lists (Op, PTN, Rate, Entry Len) are fields: a tap opens a list of buttons (current lit amber); a tap on a choice picks it and closes the list; X closes it. The MPC's own list overlay opened empty on the MPC One G2.
- Scrolling: the SEQ page shows whole pages. A sideways swipe on the ruler (60 px) or the < > buttons in the header flip one page; the View Start knob is now Page (1-8). The last page is the one that holds the end of the longest row. The header shows the page ("2/4") between < and >. "more" is now at the right end of the ruler.
- FOLLOW (SEQ header, default off): while the MPC plays, the page follows the selected row's playhead.
- Defaults: Snap 1, Steps/Page 8 (Entry Len stays 1).
- Display: text fields are reported to the MPC only when their text changes.
- Nudge: -96 to +96 /96 step (one step either way), independent of Snap (was half a snap step, for example +-11 with Snap 1/4). Each hit stores its Nudge; Move keeps it, a touch move sets it to 0. Project state: the Nudge uses two spare bytes of each hit (older projects load with Nudge 0).
- Playheads: the current step of each row is a light square (white, 25 %) behind the hits, in place of the red line.
- Steps/Page is 8, 16, 32 or 64 steps (was 1-64), so the step square is one step wide. A ruler drag of 24 px changes the size by one.

### Fixed

- Choice knobs (for example Op on the CHANCE page) showed a name one choice away from the one percSeq used. Measured on the MPC One G2: a Q-Link writes values between two choices (steps of 1/128), and the MPC and percSeq turned such a value into different choices. percSeq now writes back the exact value of the choice it uses, so both show the same name.
- Rate and Direction on the ROW page are written back on the exact choice (the host simulator's state round trip failed when a knob stopped between two choices).

## [1.0.1] - 2026-11-22

### Changed

- The SEQ page draws up to 32 hits (was 64). The plugin has 1,145 parameters (was 1,849) and the SEQ page about 1,000 components (was about 1,700).
- percSeq reports at most 30 display changes to the MPC in each 50 ms pass; the rest follow in the next passes. Measured offline before the change: 51 reports for one touch on a hit and 252 for one scroll step.

### Fixed

- POOL and ROW pads: the text is now under the touch button (the Open MPC project found that labels on top of buttons take the taps). The pad pictures are frames with a transparent middle.

## [1.0.0] - 2026-11-22

### Added

- First version: the percussion version of scalaSeq 2.5.0, a separate plugin (uid BxPq).
- 16 rows. Each row is one MIDI note (default 36-51) with its own playhead: Rate (15 note values), Steps, Range Start, Range End, Direction, Swing. Also per row: Velocity, Rev CC, CC Num, CC Rest.
- Hits of different rows play at the same time; hits of one row do not overlap.
- The scalaSeq chance operations without GLIDE and NO GLIDE. The pitch operations are sound operations: SND+, SND-, SND?+, SND?-, SND?, SND HOLD, SND PREV, SND NEXT. They pick rows from the POOL page.
- New operations: FLAM (a grace hit 1-48/96 step before the hit, half velocity), SMP REV (Rev CC for MIDI Learn to Layer Reverse) and CC (a CC value for MIDI Learn).
- Jump and repeat operations act on the row of the hit only.
- Reset: every 1-64 bars all rows start again at their Range Start.
- 32 sequences. MIDI notes 60-91 on the percSeq track select them. The Sequence knob switches at the next bar line while the MPC plays.
- SEQ page: a roll of 16 rows as in scalaSeq, each row in its own steps, a playhead and an end bar for each row, up to 64 hits drawn ("more" in the header when more hits are in view), velocity lane for the selected row, row labels with the row number and note (touch to select the row).
- Pages HIT, CHANCE, ROW, SETUP and POOL. POOL is a 4 x 4 grid of pads laid out like the MPC pads; the ROW page has 16 row pads to select the row.
- A row at Velocity 0 % sends nothing. When the MPC stops, reversed pads go back to forward and CCs to their rest value. Two rows on the same note: a note-off comes before the second note-on.
- One MIDI channel (Out Ch) through the ALSA port "percSeq MIDI Out".
