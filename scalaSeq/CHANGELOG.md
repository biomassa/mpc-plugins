# Changelog

This file records all notable changes to scalaSeq.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- Degree (2026-11-25, user: Q-Links seemed stuck): the knob turns like a normal knob. A Q-Link write moves a parameter by 1-3 x 1/128 of its range (measured on the MPC), less than one degree band of the knob (64 / number of degrees), and scalaSeq wrote the knob back to the middle of the degree after each write, so a turn did not change the degree. Now the knob goes to the middle of the degree only when another note is selected, or when the knob points to a degree the note cannot take. New test: 16 writes of 1/128 change the degree (fails with the old code).
- SEQ page (2026-11-25, as percSeq): each shown note keeps its drawing slot. Before, one new or deleted note moved every later note to another slot. With 15 notes on the screen, a new note now takes 38 display reports (was 128), a delete 35 (was 126). New test (fails with the old code).

### Changed

- NOTE page (2026-11-25): Snap is above Entry Len (was below). The Q-Link knobs 3 and 4 of that column swapped with them.

- Performance (2026-11-25, no change in what plays): the loop phase is a multiply and a floor with the loop length computed once per block (was std::fmod and ratioOf for every sample; measured on the desktop: 2.5 -> 1.9 us per block, playing). After a project load the copy for project saving is made on the UI thread; before, the audio thread also copied the whole model (497 KB) after the load and on the first block after the plugin was added.
- tests/bench.cpp: Pad 1 Note 37 (60 is a sequence note since 2.5, so the bench entered no notes).

- SEQ page: the selected note is drawn as in the Max roll (user, 2026-11-25): a dim teal body (30 %) with its ratchet hits on top. Each ratchet hit starts at its place and is DUTY of a ratchet hit long; its brightness (4 steps: 30, 55, 80, 100 %, the nearest to 0.15 + 0.85 x velocity) follows its velocity (VEL with FADE, as the engine). With RATCH 1 the whole note is one hit, its brightness by VEL. The other notes keep the ratchet gaps. New display parameters for the hit markers (292, added at the end).

- LANE header (2026-11-25, user: the buttons were too hard to hit on the MPC): the buttons are 56 px tall (were 40), the lane pads 100 px wide (were 90). The lane starts at y 60 and is 250 px high (was 260); its bottom did not move. The SEQ and LOCK pages keep their buttons.

- LANE (2026-11-25, after a test on the MPC One G2: the drawing knobs worked but nothing on the screen named them): the lane starts at Q-Link column 2 and runs to the right edge (x 343-1261, 918 px; was x 70-950). Q-Link column 1, left of the lane, shows the two drawing controls: STEP (Lane Step, a < value > control) and VALUE (Lane Value, a fader); with VEL shown they are named NOTE / VEL. The header text STEP / VALUE is removed; the lane pads and SEND moved with the lane. New parameters Lane Prev and Lane Next (the < > buttons, added at the end).

- Redesign of the pages and Q-Links (user, 2026-11-24). The MPC One G2 has 4 Q-Links: they control one column at a time, and the Q-Link button stops on every column, also on an empty one. Each Q-Link column now holds one set of controls.
- **After this update, remove the plugin from its track and add it again.** The Q-Link assignments changed, and the MPC keeps the old ones in a project until the plugin is added again (measured before).
- SEQ page, Q-Link panel: the columns are PITCH (Note, Degree, Period, GLIDE), TIME (Move, Length, Nudge, Entry Len), SHAPE (VEL, RATCH, FADE, DUTY) and VIEW (View Low, View Start, Sequence, Snap). The bank titles are removed (the MPC's Q badge covered the title of the active bank).
- NOTE page: the same first 3 columns as the SEQ panel; column 4 is step entry (Step Entry, Cursor, Snap, Entry Len). Delete is a DELETE button (one touch, as before; no Q-Link).
- Snap (NOTE page): the 6 pads are taller (36 px, were 26) and fill the slot.
- Page order: SEQ, NOTE, CHANCE, LANE, LOCK, RATE, SETUP, OUTPUT, MASK. MASK 2 is a sub-page of MASK: push the MASK button again.
- RATE: Rate, Mode, Ratio, Fine / Steps, Range Start, Range End, Direction / Swing. OUTPUT: Scale, Root, Root Tune / Out Ch, Voices / Bend Up, Bend Down, Curve. SETUP: Sequence, COPY, PASTE, CLEAR / Steps/Page, Group / PTN Count, Velocity / Pad 1 Note, Clear Ops, Clear All Ops.
- LOCK: each Lock CC has a shorter fader with SEND at its right; Note < > (column 3) and Sequence (column 4).
- LAST PAD and PAD 1 moved from the SEQ header to SETUP (LAST PAD shows under the Pad 1 Note fader).
- Clear Note and Clear All are now called Clear Ops and Clear All Ops (they clear the chance lanes). The parameter ids did not change.
- The orange Q-Link frames start 36 px above the top control, so the MPC's Q badge sits above the name and does not cover it.
- LANE: the lane is 260 px high (was 280), so the Q badge fits above the settings.
- Program Mode Q-Links (the knobs when they control the plugin): Sequence, Range Start, Range End, Direction / Rate, Swing, Ratio, Fine / Prob 1-4 / VEL, RATCH, GLIDE, Nudge.

- Steps no longer deletes notes. When Steps goes down, the notes outside it are kept, and a note that crosses the end plays cut at the end. When Steps goes up again, they come back, and a cut note gets its full length back. An edit of a cut note (any change, for example VEL or length) makes the edit count: it keeps its cut length. Kept notes are saved in the project, and COPY, PASTE and CLEAR include them. They count toward the limit of 256 notes per sequence.
- SEQ page: the right end of the ruler shows "+N kept" (amber) when the sequence has notes outside Steps. New display parameter Kept Text (added at the end).
- SEQ page, Q-Link panel: the 16 values react to touch. A tap selects the Q-Link column of the value (the MPC frames it). A vertical drag on the value changes it. A double-tap (or Enter) opens the big knob editor, also for lists such as Snap and Rate (the MPC's own list overlay opens empty for this plugin). Note, Move and Length have no drag (the plugin limits these values, and a drag could jump); a tap and a double-tap work on them.
- SEQ page, Q-Link panel: larger text. Names 24 px (were 22), values 34 px (were 32). The names and the 4 bank titles are light grey (e8e6df; were grey).

### Fixed

- NOTE page: the second row of the Snap pads was cut in half (seen on the MPC One G2, 2026-11-25). The pads' container was one slot tall (107 px) and the MPC cuts what is outside it; it is now as tall as the pads. (The first guess, the bottom of the screen, was wrong: the pads were cut in the upper slot too.)

- Pop-up lists: the MPC's orange Q-Link frame was drawn over an open list. The list panels now ask the MPC to hide the frame while they are shown (hideQLinkBounds, component version 4). Seen working on the MPC One G2: the frame is hidden only while a list is open.
- SEQ page, Q-Link panel: the values of the drag cells did not show on the MPC One G2 (the MPC's ValueSlider knob draws no text there). Each value is now a text label, with the drag knob on top of it.
- Range End (RATE page): when Range End is at the last step, it now follows Steps. Before, Steps 16 -> 1 -> 16 left Range End at 1, and only 1 step played. A Range End inside the steps stays; it is clipped when Steps goes below it. When it is clipped to the last step, it follows Steps from then on.

### Added

- LANE: draw with the Q-Links. Q-Link column 1, knob 1 (Lane Step) moves a cursor (a see-through teal column behind the bars) and knob 2 (Lane Value) sets the value at the cursor (0-127). A touch on the lane also moves the cursor. The header shows STEP and VALUE. With VEL shown, knob 1 selects the note and knob 2 sets its VEL (the header shows NOTE and VEL). New parameters Lane Step, Lane Value and Lane Cur X (added at the end).
- CHANCE: a header strip at the top with < > (select the note), NOTE and DEG. The controls moved 17 px down.
- A trace build for diagnosis (`python tools/build.py plugins/scalaSeq --define SCALASEQ_TRACE=1`). It records every call between the MPC and scalaSeq to /tmp/ssq_trace.bin on the MPC. tests/trace_report.py makes a report for each second. The trace build can also move the rows by itself to test the display reports. Do not release it.

## [2.8.0] - 2026-11-22

### Added

- SEND buttons on LOCK (next to each Lock CC) and LANE (header, for the shown lane): a touch starts that CC as a zigzag (0 and 127 in turn, every 250 ms) on First Ch; SEND stays lit; a second touch, or 15 s, stops it with 64. Touch SEND, go to MIDI Learn (Learn on, +, select the row): Learn takes the CC number without typing. Nothing is sent while the CC is off.
- SETUP: COPY, PASTE and CLEAR for the edited sequence (the Sequence knob). COPY takes the sequence with all its data. PASTE puts the copy into the edited sequence; CLEAR sets it back to a new sequence. PASTE and CLEAR: the first touch arms the button (lit amber), a second touch within 3 s does it. The copy is shared by all scalaSeq instances (copy on one track, paste on another) and is lost when the MPC app restarts.
- Ratio rates: each sequence has a Rate Mode, Musical or Ratio (RATE page, Ratio lit teal). In Ratio mode the step is Rate divided by Ratio + Fine. Ratio: 0.25 ... 4 (15 values). Fine: -0.10 to +0.10 (the value shows the sum).
- Locks (LOCK page): each note has 4 lock values (off or 0-127). Each sequence has Lock CC 1-4 (default off). A lock is sent just before its note.
- CC lanes (LANE page): 2 lanes per sequence, with CC Num (default off), Steps, Rate, Mode, Ratio, Fine, Direction and Rest; one value per step (default 64). A lane sends its CC only when the value changes; at stop it sends Rest. A lock on the same CC at the same time wins.
- LANE page: lane pads VEL / CC 1 / CC 2. The lane follows the SEQ view (View Start, Steps/Page): each step is a thin bar; drag across to draw. The current step of the shown lane is a red line. VEL shows the note velocities (moved from the SEQ page).
- Lock and lane CCs go out on First Ch only (one MIDI Learn assignment per CC). CC numbers skip 0-5, 7, 10, 11, 32, 64-68, 98-101, 120-127.

### Changed

- Pages: SEQ, NOTE, LOCK, CHANCE, RATE, LANE, SETUP, OUTPUT, MASK, MASK 2. Rate, Direction, Steps, Swing, Range Start and Range End moved from SETUP to RATE.
- SEQ page: the roll is taller (540 px, was 476).
- Project state format SSQ5 (SSQ4 still loads, with no locks and lanes).

## [2.7.0] - 2026-11-22

### Added

- Step controls on the NOTE page for Note, Move and Length: the name, a < button, the value and a > button. A tap steps one value; the Q-Link still turns it. Each stops at the first and last value. Move skips places where the note does not fit and goes to the next free place in that direction. These were faders. Reproduced offline: the Note fader goes from 1 to 64, and with 3 notes scalaSeq wrote back 3, so the handle sprang back near the start; the Move fader sprang back at the sequence end.

### Changed

- Text on the pages other than SEQ is larger. Names 34 px and values 38 px (were 24 and 26) for faders, switches, list fields and step controls. On a fader the name and the value use the full width (name at the left, value at the right), so long names are not cut. Pop-up lists: title 34 px, choices 28 px (were 26 and 22); a list with a long choice (Op: SND HOLD) has wider buttons (170 px). List pads: 26 px in one row, 22 px in two rows (were 20 and 17).
- MASK pads: the degree numbers are 44 px (were 30).

## [2.6.0] - 2026-11-22

### Changed

- Controls: number parameters are faders (Period, FADE, Nudge and Root Tune fill from the centre). Short lists (Snap, Direction, PTN Count, Curve) are amber/grey pads, lit = current. Long lists (Op, PTN, Rate, Entry Len, Scale) are fields: a tap opens a list of buttons (current lit amber); a tap on a choice picks it and closes the list; X closes it. The MPC's own list overlay opened empty on the MPC One G2 (tested with percSeq).

### Fixed

- Choice knobs (for example Op on the CHANCE page) could show a name one choice away from the one scalaSeq used: a Q-Link writes values between two choices (measured with percSeq on the MPC One G2: steps of 1/128). scalaSeq now writes back the exact value of the choice it uses.

### Changed (pages)

- MASK and MASK 2 pages: pads laid out like the MPC pads (as the POOL page of percSeq) in place of switches. Degree 0 is at the bottom left of MASK; degrees 16-30 are on MASK 2. Each pad shows its degree number; a lit pad (amber frame) is in the scale mask. Q-Link n is the pad at the same place.

## [2.5.0] - 2026-11-21

### Added

- Sequence notes: MIDI notes 60-91 on the scalaSeq track select sequences 1-32 (note 60 = sequence 1, note 91 = sequence 32). Thus a sequence on the MPC can arrange the 32 sequences.
  - While the MPC plays, the new sequence starts at its Range Start at the sample of the note. It plays until the next sequence note. A note off does nothing.
  - While the MPC is stopped, the note selects the sequence. Play starts it.
  - The SEQ page and the Sequence knob show the selected sequence.
  - A sequence note does not sound and does not enter a note.
  - A jump of the MPC (Play, a loop back, a locate) goes back to the song grid: the sequence plays from the step that the song position gives, as before, until the next sequence note.

### Changed

- Pad 1 Note cannot be 45-91, so that the 16 pad rows do not overlap the sequence notes. The knob jumps from 44 to 92 and back. A project with a Pad 1 Note of 45-91 changes it to 92 when it loads.

## [2.4.1] - 2026-11-21

### Changed

- The plugin tells the MPC about display changes every 50 ms at most (was 16 ms). The roll moves in steps of up to 50 ms during a drag.
- Measured on the MPC One G2: this does not stop the crackle during a drag (see knownIssues.md).

## [2.4.0] - 2026-11-21

### Added

- Chance operation GLIDE: when it fires, the hit glides from the previous pitch over a share of its length (Amount 10-100 %, default 50 %). It replaces the note's own GLIDE for that hit and uses the hit's final length (after GATE- / GATE+).
- Chance operation NO GLIDE: when it fires, the hit plays without the note's GLIDE.
- Chance operation DELAY %: when it fires, the hit starts later by 1-100 % of a step (default 25 %). DELAY (from the Max original) stays, with its step fractions 1/16 .. 1 step. On the Op knob, DELAY % comes right after DELAY.
- If both are in a note's lanes, the later lane wins (as MUTE / UNMUTE). GLIDE and NO GLIDE are not in the Max original.

### Changed

- After a project load, the CHANCE knobs show the selected note's lanes (the list of operations is longer now).

## [2.3.2] - 2026-11-21

### Changed

- The chance lines on the SEQ page also show each lane's Amount, as its Amount knob shows it (for example "1 JMP< 20% 3"). The value is the one the engine uses: an Amount of 0 means the operation's default, and amounts are limited to the sequence (a jump to the play range).

## [2.3.1] - 2026-11-21

### Fixed

- Choice values (Entry Len, Rate, Snap, Direction, Scale, Curve, PTN Count, Op and PTN lanes) could differ from what the MPC shows. A Q-Link can leave a value between two choices; the MPC shows the lower choice, scalaSeq used the nearest (measured: Entry Len 0.6171875 showed "1" but gave 1.5 steps). scalaSeq now reads the choice the MPC shows.
- The chance ring, glide ramp and ratchet gaps of the selected note were hidden under its highlight. They are drawn above it now.

### Changed

- A moved note stops where its end meets the end of the sequence (touch and the Move knob). Before, it could run past the end, and only its first part played and showed.
- Setting a chance operation keeps Prob at 0 (before: 100 % when Prob was 0).
- The chance ring shows for every chance operation that is set, also at 0 %.

## [2.3.0] - 2026-11-21

### Added

- SEQ page: the 4 chance lanes of the selected note as text under the FREE button (for example "1 REPEAT 25%", "2 -"). The FREE button is higher.
- SETUP page: Clear Note clears the 4 chance lanes of the selected note. Clear All clears every chance operation of the sequence: the first touch arms it (the switch is lit), a second touch within 3 s clears; after 3 s it switches off without clearing.

## [2.2.0] - 2026-11-21

### Changed

- Touch editing snaps to the lines on screen. Moving a note puts its start on the nearest snap line (its Nudge offset is dropped); hold FREE to move it freely. When the snap lines are hidden (closer than 10 px), placing, drawing, moving and the length snap to whole steps.

### Fixed

- Notes could keep a wrong pitch when one synth track takes all of scalaSeq's channels (MIDI input channel All): scalaSeq skipped the pitch bend when the note's channel already had that value, so the synth used another channel's bend. scalaSeq now sends the pitch bend before every note-on.

## [2.1.3] - 2026-11-21

### Removed

- The timing trace (diagnostics of 1.6.0-2.1.2). The timing is measured and correct (see knownIssues.md).

## [2.1.2] - 2026-11-21

Measured with this version: scalaSeq's MIDI reaches a receiver within one audio block (spread 2.7 ms). The 10-13 ms jitter reported for 2.0.3 and 2.1.0 was a fault of the recording tool (arecordmidi, 4 ms timer). See knownIssues.md.

### Added

- Diagnostics: the timing trace also records when the MIDI thread wakes and when it sends each note (only while /tmp/ssq_trace_on exists). Measured with 2.1.1: the MPC computes audio blocks evenly (at most 0.53 ms late), but a note reaches its receiver 0-12.6 ms after its block; this finds where in the MIDI path the delay is.

## [2.1.1] - 2026-11-21

### Removed

- Scheduled MIDI output of 2.1.0 is off again, and MIDI Delay is hidden. Measured on the MPC One G2 with the test pattern, it was worse than direct output: notes -9.0 to +11.9 ms around the grid (20.9 ms spread; direct output: 13.7 ms), and the pitch bend of a note could arrive just after its note-on (a pitch jump at the note start). The design assumed that the MPC never computes an audio block earlier than its real time; this was not measured.

### Added

- The timing trace records the system time of each audio block, to measure how the MPC computes blocks before scheduling is designed again.

## [2.1.0] - 2026-11-21

### Added

- MIDI Delay (OUTPUT page, 0-30 ms, default 12 ms). MIDI now goes out scheduled (an ALSA queue) at the exact time of its sample plus this delay. Before, each block's MIDI went out when the MPC computed the block; measured on the MPC One G2 with a test pattern, this added about 10 ms of jitter (notes -10.2 to +3.4 ms around the grid, while the engine itself was within -0.6 to +2.3 ms).

### Measured

- Pitch: with the test pattern, every note had exactly its tuned pitch and there was no pitch bend while a note sounded (205 notes, 2026-11-21). A pitch "fluctuation" heard on an OPx-4 track does not come from scalaSeq's MIDI.

## [2.0.3] - 2026-11-21

### Removed

- The Q-Link action change of 2.0.2. It was not based on the SDK documentation, and it changed nothing on the MPC. The cause of the wrong Q-Link assignments is not known yet; see knownIssues.md.

## [2.0.2] - 2026-11-21

### Fixed

- Touching a control did not select its Q-Link bank, so the Q-Links changed other parameters than the touched one. The fine knob step (1.3.1 / 1.2.1) wrapped each control's handle in DisableCoarseIncrement, and the MPC did not find the Q-Link bank of a wrapped handle. The Q-Link action now uses the plain parameter.

## [2.0.1] - 2026-11-21

### Fixed

- Timing: notes between steps started early and some loops lost or added notes. The MPC reports the song position a little ahead or behind from block to block (measured: up to 0.0007 quarter back). At a step boundary the engine then counted the boundary twice. scalaSeq now runs its own steady clock and follows the MPC only slowly; a start, loop or locate still re-syncs at once.
- Touches outside the touch areas (for example on the Q-Link panel) moved hidden marker pads: notes and highlights showed at wrong places, over the panel. A touch guard now covers the page under the touch areas, and the plugin puts back any marker that was moved.
- A project saved with 1.x set some 2.0 parameters to old values (for example Range End). Such a project now sets all parameters to their defaults.
- Thin dark lines inside notes: the pieces of a note body could leave 1 px gaps, and the start notches and ratchet gaps of a covered note showed through the note on top. The pieces now overlap by 1 px, and each note's marks are drawn with its own body.
- Drawing a note: the note starts one snap step long and follows the drag. Only a tap gives Entry Len (before, the note showed at Entry Len during the drag).

### Changed

- Notes of the same pitch do not overlap. Drawing and lengthening stop at the next note of the same pitch; a move stops where it last fitted; Degree and Period do not move a note onto a same-pitch note; step entry does not put a note inside one. Notes of different pitches can still overlap.

### Added

- The timing trace also records the touch values and gestures (only while /tmp/ssq_trace_on exists).

## [2.0.0] - 2026-11-21

### Added

- A piano roll on the SEQ page with real touch: the MPC reports when the finger lifts (ResetOnGestureEnd), so press, drag and release are exact.
  - Press-drag on an empty place draws a note (both directions); a tap gives Entry Len.
  - Drag a note's body to move it in time and pitch; drag its right third to change the length.
  - Double tap (300 ms) deletes a note.
  - FREE button: hold it with a second finger for drags without snap.
- Ruler with step numbers: drag sideways to scroll, up or down to zoom.
- Drag the degree labels to scroll the rows.
- Velocity lane: one bar for each note; drag a bar to set the velocity.
- Notes are drawn at their exact start and length, with ratchet gaps, a glide ramp and a chance ring.
- A short preview when a note is placed, selected or moved to another row.

### Changed

- Notes can overlap. A note that starts later cuts the earlier note.
- The view scrolls by whole steps: View Start (step 1-64) replaces Step Page.
- Header: larger text (24 px names, 30 px values); LAST PAD and PAD 1 moved into it. The header names are Name labels now (Text labels did not show on the G2).
- Q-Link panel: columns of 68 px, names 22 px, values 32 px, short names (Deg, Per, Len, Entry, Low, Start, Seq). The bank titles are Name labels now.
- Pad 1 Note is on the SETUP page.
- New parameter list and project format (SSQ4).

### Removed

- Loading of projects saved with 1.x.
- The hidden parameters of 1.x (jog knobs, Start*, Length*, the 1.x display parameters).

## [1.6.0] - 2026-11-21

### Added

- Period: moves the selected note by whole periods (-4 to +4). Q-Link bank NOTE and the NOTE page.
- Steps/Page (SETUP page): the number of steps on a page of the roll, 1-64 (default 16).
- A start marker at the exact start of each note.

### Changed

- The knobs Note, Degree, Move, Length, View Low, Step Page, Sequence and Nudge are normal knobs again, with ranges of 64 values or fewer, so that a click does not skip a value. Note 1-64, Move 0-63 and Length 1-64 snap steps, View Low -32 to +31, Step Page 1-16, Sequence 1-32, Nudge -32 to +31 (1/96 step).
- Degree is the degree inside the period. The full knob turn covers the degrees of the scale mask.
- Vertical lines: a dim line at each snap step (hidden when closer than 10 px), a brighter line at each whole step, the Group lines brightest. Before, each of the 48 columns was one snap step.
- A tap places a note on the snap line nearest to the finger.
- Q-Link banks: NOTE = Note, Degree, Period, Move. VIEW = View Low, Step Page, Sequence, Nudge (Steps is on the SETUP page only).
- NOTE page: column 1 = Note, Degree, Period, Move (as bank NOTE); Length and Nudge are in column 4.
- The Q-Link panel is narrower (columns of 80 px) with larger text (names 20 px, values 28 px). The roll is wider (832 px).

### Fixed

- A project saved with an older version showed the notes, the end line and other markers at their old positions, and the playhead did not match them. The project now stores the screen layout (state format SSQ3); with an older layout, the plugin draws all markers again. SSQ1 and SSQ2 projects still load.

### Removed

- The jog knobs of 1.5.0. They gave two steps for each click on the MPC (Note went 1, 3, 5, 7). The old parameters stay hidden, so projects still load.

## [1.5.0] - 2026-11-21

### Added

- Jog knobs: Note, Degree, Move, Length, View Low, Step Page and Sequence change by exactly one step for each click and have no end stop. The value text shows the real value (for example `2 / 12`). The MPC gives a knob about 128 positions for its whole range, so a large range skipped values (Note went from 1 to 3).
- The Group parameter (SETUP page): a brighter line every N steps. 0 = off (the default), 1-64 steps.

### Changed

- The roll has 48 columns, and each column is one snap step. The number of steps on a page follows Snap: 1 = 48, 1/2 = 24, 1/3 = 16, 1/4 = 12, 1/6 = 8, 1/8 = 6. All column lines are equal; there is no 4/4 emphasis.
- A tap places the new note exactly on the snap grid. A nudged note shows in its grid column.
- MIDI goes out on its own thread with real-time priority. A second thread sends the display updates to the MPC, at most about 30 times each second. In 1.4.0 one thread did both, and display work delayed the notes (measured: up to about 60 ms).
- The note markers, row lines, degree labels, cursor and playhead are at the correct positions. The MPC keeps an XYPad picture inside its area; the plugin now includes this.
- The project stores the selected sequence, note, page and lowest row (state format SSQ2). SSQ1 projects still load.

## [1.4.0] - 2026-11-21

### Added

- Move: moves the selected note in snap steps. The microtiming stays.
- Nudge: microtiming of the selected note in 1/96 step, up to half a snap step earlier or later.
- Length (knob): the length of the selected note in snap steps.
- The SEQ page shows the 4 Q-Link banks right of the roll, with the name and the value of each knob. The MPC frames the active bank.
  - Bank NOTE: Note, Degree, Move, Nudge.
  - Bank SHAPE: Length, VEL, GLIDE, RATCH.
  - Bank RATCH: FADE, DUTY, Entry Len, Snap.
  - Bank VIEW: View Low (rows), Step Page, Sequence, Steps.

### Changed

- Touch on the grid only places, selects and deletes notes. Dragging no longer changes the length.
- The blue cursor shows only while Step Entry is on.
- The roll is narrower (12 px cells) to make space for the Q-Link panel.
- The NOTE page uses Move, Nudge and Length. The old Start and Length parameters are hidden (renamed Start* and Length*).

### Removed

- Drag to set the note length.

## [1.3.1] - 2026-11-21

### Changed

- Knobs, faders and the data wheel move by the fine step without Shift. Before, the normal step was too coarse. Shift gives no finer step.
- Degree is -96 to +96 (was -256 to +255), View Low is -96 to +80. The selected-note view follows inside this range.

## [1.3.0] - 2026-11-21

### Added

- Rows sized by cents on the SEQ page, as in the Max version: the height of a row is the interval from its degree to the next degree. In ji_7, for example, the 10/9 row (182 cents) is taller than the 128/105 row (160 cents). In an EDO all rows have the same height.
- A brighter line at the start of each period (degree 0).
- Parameters 360-472: Row Line 1-17, Per Line 1-3, and the degree labels (31 degrees, 3 copies each). The plugin writes them.

### Changed

- The degree numbers left of the grid are markers that move with their rows. A row's number shows its scale degree; degree 0 is white.
- A tap on the grid finds its row from the cents layout.
- The background picture has only the step lines. The row lines are markers.

## [1.2.0] - 2026-11-21

### Changed

- The SEQ page draws the notes with XYPad markers, not with film-strip cells. On the MPC One G2, film-strip cells that a plugin changes did not show (1.0.0: Knob film strips, 1.1.0: Image film strips). XYPad markers show (as on the ROLL page of Scala Retuner). For each quarter-step column of the page there are 3 markers: the note, the start of the note, and the selection. scalaSeq is monophonic, so one column has one note at most.
- The row and step lines are a fixed background picture. A grey line shows the end of the sequence (Steps).
- scalaSeq has 360 parameters (1127 before). The parameter order changed: projects made with 1.0.0 or 1.1.0 do not load their sequences correctly.

### Removed

- The 1024 grid cell parameters.

## [1.1.0] - 2026-11-21

### Added

- Touch on the SEQ grid. Tap an empty cell: a new note there (the degree of the row, the start of the cell, length Entry Len), and the cursor moves there. Tap a note: select it. Tap the selected note again: delete it. Drag sideways after the touch: the end of the note follows your finger.
- Pads 1-16 are the 16 visible rows (pad 1 = the bottom row). The Pad 1 Note parameter sets the note of pad 1 (default 37). The SEQ page shows the last received note.
- Parameters 1123-1126: Touch X, Touch Y, Pad 1 Note, Last Note.

### Changed

- The grid cells use the Image film-strip widget. In 1.0.0 the cells did not show on the MPC One G2.
- SEQ page Q-Links: Q-Link 1 scrolls the rows (View Low), Q-Link 2 changes the step page, Q-Link 3 sets Pad 1 Note.
- Keys and pads no longer map to degrees from Root. Only the 16 pads (from Pad 1 Note) enter notes.
- The playhead and the cursor do not move when you touch the grid.

## [1.0.0] - 2026-11-21

### Added

- A monophonic pitch and gate sequencer for Scala tunings. It is a port of `dingus.pitchGateSeq` (Max 9: the `dingus.pgRoll.js` editor and the gen~ engine).
- The engine in C++: steps, notes with any start and length, swing, play range, direction (forward, reverse, random), and the lanes VEL, GLIDE, RATCH, FADE and DUTY.
- 4 chance lanes for each note with all 28 operations of the Max version and PTN patterns. PTN counts loop repeats or bars.
- 32 sequences. A new sequence starts at the end of the loop while the MPC plays, and at once when it is stopped.
- The clock is the song position of the MPC. The Rate parameter sets the note value of one step.
- MIDI output through an ALSA port ("scalaSeq MIDI Out"): the nearest 12-TET note and a pitch bend, each new note on the next output channel. GLIDE is a pitch-bend sweep. Curve: Symmetric for keygroups, OPx-4 for OPx-4.
- Step entry: with Step Entry on, a pad or key adds a note at the cursor with the scale degree of the key. The cursor moves on by Entry Len.
- Preview: a pad or key plays its tuned note.
- Pages: SEQ (a roll of 16 degree rows x 16 steps, in quarter-step cells), NOTE, CHANCE, SETUP, OUTPUT, MASK, MASK 2.
- The project stores the 32 sequences, the scale mask and the scale.
