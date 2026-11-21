# Changelog

This file records all notable changes to scalaSeq.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- A trace build for diagnosis (`python tools/build.py plugins/scalaSeq --define SCALASEQ_TRACE=1`). It records every call between the MPC and scalaSeq to /tmp/ssq_trace.bin on the MPC. tests/trace_report.py makes a report for each second. The trace build can also move the rows by itself to test the display reports. Do not release it.

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
