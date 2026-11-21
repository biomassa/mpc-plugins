# Known issues

## Knob steps

- All controls use the fine step without Shift (the `DisableCoarseIncrement` modifier and `disableCoarseDataWheel` in the skin). The SDK documents these for touch controls and the data wheel. Not known: if the hardware Q-Links also use the fine step.
- Shift gives no finer step. Whole-number parameters already move by 1 for each click. For continuous parameters, a finer Shift step needs the MPC to read step sizes from the plugin (VST2 parameter properties); not tested.

## Not tested on the MPC

- **Status:** 1.2.0 and 1.3.0 show the notes on the SEQ page of the MPC One G2, and tap to place works (2026-11-21). Not yet tested on the device: drag for length, tap to delete, pads, playback to keygroup and OPx-4 tracks, chance operations.
- 1.0.0 and 1.1.0 showed no grid cells. At that time the MPC's film-strip cache (`/var/volatile/tmp/filmstrips`, in RAM) was full, so the MPC could not unpack new film strips. The MPC adds to this cache at each start of the MPC application and does not delete it. Restart the MPC (power off and on) after many plugin installs.
- Not known: if the markers are at the correct x and y positions, and the screen speed and CPU use while the SEQ page is on the screen (192 markers).
- Rows outside the scale mask are not dimmed in the grid, and the degree labels do not show the period number. Use the MASK pages and the NOTE page (Degree) for these.
- Row heights by cents: in a scale with very small steps, a row can be lower than a note marker (24 px). Then markers of neighbouring rows overlap.
- 1.3.0 (rows by cents) is tested on the MPC One G2: rows, period lines, degree labels and touch work (2026-11-21).

## Timing

- Measured on the MPC One G2 (2026-11-21, test pattern of 10 notes on and between steps, 122 bpm):
  - The engine is within -0.6 to +2.3 ms of the grid (trace).
  - The MPC computes audio blocks evenly (one each 2.90 ms, at most 0.53 ms late).
  - scalaSeq hands each note to ALSA 0.02-0.08 ms after its block; a receiving program reads it 0.03-0.11 ms later (midi_probe, exact system clock).
  - Received notes: all within a spread of 2.7 ms (one audio block). Notes are sent once per audio block, so the resolution is one block (2.9 ms at 128 frames); the MPC's sequencer timer has 4 ms resolution, so scheduling cannot do better.
- arecordmidi is not a valid timing tool on the MPC: it stamps with the 4 ms system timer (earlier "10-13 ms jitter" numbers came from it).
- Not measured: how the MPC itself schedules incoming MIDI for the target track (the audio output).
- Many notes at the same start: each later note cuts the earlier one (monophonic). This sounds irregular by design.

## Pitch-bend range of the target

- Tuning and GLIDE are pitch bends computed for Bend Up / Bend Down (OUTPUT page). The target synth's pitch-bend range must be the same (for keygroups: Preferences > Hardware > Global Pitch Bend; for plugin synths: their own setting or preset). Also switch off the synth's own glide/portamento. Reported 2026-11-21: glides landed too high with one synth preset and were right after changing the preset (not measured).

## One synth track on all channels

- Confirmed 2026-11-21: a synth track whose MIDI input channel is All receives every output channel of scalaSeq, with one pitch bend for all of them. From 2.2.0 each note-on carries its own bend, so each note starts at its right pitch. A note that still sounds (release) follows the next note's bend. One track per channel (Out Ch .. Out Ch + Voices - 1) avoids this; Voices = 1 also works.
- Chance operations that the user did not set were found on notes (Pipka, 2026-11-21: REPEAT 25%, P+ 44%). Most probable source: Q-Link knobs that still used the 1.6 map (before the plugin was re-added) changed chance-lane parameters. Not proven.

## Pitch on an OPx-4 target

- Measured 2026-11-21: scalaSeq sends the correct pitch for every note, with no bend while a note sounds. In the test project the OPx-4 track takes "scalaSeq MIDI Out" with MIDI input channel value 0 (read from the project file; probably "All"). scalaSeq sends each new note on the next of Voices channels, each with its own pitch bend. If one OPx-4 track receives all channels, every new bend also bends the notes that still sound. Use one target track per output channel (see Setup), or set Voices to 1. To confirm on the device.

## Q-Link assignments (2.0.x)

- Measured on the MPC One G2 (2026-11-21, 2.0.1): on the SETUP page, the knobs controlled parameters 7, 17 and 535. These are the 1.6 indices of Direction, PTN Count and Steps/Page, so the MPC used the 1.6 Q-Link map for a scalaSeq instance that was on the track before the update. The SDK says: after an update, remove the plugin from the track and add it again. Where the MPC keeps the old map is not known. Verified 2026-11-21 (2.0.3): after the plugin was removed and added again, the Q-Links were correct.
- After each update of scalaSeq, remove it from its track and add it again.

## 2.0.0, not tested on the MPC yet

- The piano roll and all touch gestures are tested on a computer only. The touch method (ResetOnGestureEnd) and two-finger touch are tested on the MPC One G2 with the touchProbe test plugin.
- The header names and the bank titles of the SEQ page are Name labels with a fixed name (WithName). Check that they show on the G2.
- At most 32 notes on the screen are drawn. Zoom in to see the others.
- The velocity bars use a picture that goes below the lane; the MPC must clip it at the bottom of the page. Not checked on the device.
- The knob ranges (64 values or fewer) must not skip values. Check: turn Note one click at a time.
- With more than 64 degrees in the scale mask, the Degree knob cannot reach each degree. Use Period and a smaller mask, or touch.
- Move covers 64 snap steps from the start of the sequence (Snap 1/8: 8 steps). Touch moves a note anywhere.
- A press at the exact left or bottom edge of a touch area counts only when the finger moves (the value 0 means "no finger").
- Knob lag (reported with 1.5.0): not checked again.
- CPU: the engine runs per sample. An event-based engine is planned after the timing fix.

## Limits of version 1.0.0

- Monophonic, as the Max version. A separate 3-voice version is planned.
- The modulation inputs of the Max version (length, direction, rate, range start, range end, chance scale) are not available. They are planned after a study of the modulation options of the MPC.
- No undo.
- No import or export of the `uPGseq_<name>.json` files of the Max version.
- No .kbm keyboard mappings: each row is one scale degree. Pads enter the degrees of the 16 visible rows.
- MIDI goes out once each audio block (about 1.3 ms at 64 frames), not at the exact sample.
- A GLIDE must stay inside the bend range of the target (Bend Up, Bend Down). If it goes outside, the note starts again at the new pitch.
- The display and touch parameters (about 1,200) show in the parameter lists of the MPC. Do not change them.
- Sustain on keygroup targets: see Scala Retuner's known issues (the MPC ignores pedal up on keygroup tracks). scalaSeq does not forward CC64.

## The audio can crackle while you drag the roll

- When you drag the roll (for example the row labels), the MPC redraws the screen. Measured on the MPC One G2: the MPC's screen thread used 30-56 % of a core, and its audio thread did up to twice its normal work. scalaSeq's own audio work stayed at 6 % of a block or less.
- 2.4.1 sends display changes every 50 ms at most (was 16 ms). Measured: the MPC's screen thread stayed at 12-56 % of a core and its audio thread at 8-19 % during the drag, and the audio still crackled. So the update rate of scalaSeq is not the cause.
- Measured with the trace build (`--define SCALASEQ_TRACE=1`, see src/trace.h and tests/trace_report.py):
  - scalaSeq's own audio work stayed at 0.11 ms or less for each block, also during a drag.
  - Each display change that scalaSeq reports to the MPC makes the MPC call the next audio blocks later. A test without a drag (the rows moved 20 times each second, about 1,000 reports each second) gave 18-20 late blocks each second (4.8 ms instead of 2.9 ms), and no audible crackle.
  - A real drag gave late blocks of up to 6.3 ms and a crackle. Late blocks of about 4.8 ms were not audible.
  - One row step sends about 147 display changes with 16 notes, because the notes change display slots when a note scrolls in or out of view.
- Possible fixes, not done: keep each note in the same display slot (no visible change), redraw the notes only when the finger lifts, or send the roll updates less often during a drag.
- Not known: if a drag on other controls (another plugin, the MPC's own screens) also makes the audio crackle.
