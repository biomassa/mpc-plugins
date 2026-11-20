# Changelog

This file records all notable changes to scalaSeq.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
