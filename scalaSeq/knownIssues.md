# Known issues

## Not tested on the MPC

- **Status:** 1.2.0 and 1.3.0 show the notes on the SEQ page of the MPC One G2, and tap to place works (2026-11-21). Not yet tested on the device: drag for length, tap to delete, pads, playback to keygroup and OPx-4 tracks, chance operations.
- 1.0.0 and 1.1.0 showed no grid cells. At that time the MPC's film-strip cache (`/var/volatile/tmp/filmstrips`, in RAM) was full, so the MPC could not unpack new film strips. The MPC adds to this cache at each start of the MPC application and does not delete it. Restart the MPC (power off and on) after many plugin installs.
- Not known: if the markers are at the correct x and y positions, and the screen speed and CPU use while the SEQ page is on the screen (192 markers).
- Rows outside the scale mask are not dimmed in the grid, and the degree labels do not show the period number. Use the MASK pages and the NOTE page (Degree) for these.
- Row heights by cents: in a scale with very small steps, a row can be lower than a note marker (24 px). Then markers of neighbouring rows overlap.
- 1.3.0 (rows by cents) is tested on the MPC One G2: rows, period lines, degree labels and touch work (2026-11-21).

## Touch on the SEQ grid

- The MPC sends no event when you lift your finger. scalaSeq takes 150 ms without movement as the end of a touch. A slow drag with pauses can end the touch early.
- After each touch, the touch point goes back to the top-right corner of the grid. A tap exactly in the top-right cell can be missed.
- The header text of the SEQ page shows at the bottom of the page on the MPC One G2 (reported 2026-11-21). The cause is not known.

## Limits of version 1.0.0

- Monophonic, as the Max version.
- The modulation inputs of the Max version (length, direction, rate, range start, range end, chance scale) are not available. They are planned after a study of the modulation options of the MPC.
- No undo.
- No import or export of the `uPGseq_<name>.json` files of the Max version.
- No .kbm keyboard mappings: each row is one scale degree. Pads enter the degrees of the 16 visible rows.
- MIDI goes out once each audio block (about 1.3 ms at 64 frames), not at the exact sample.
- A GLIDE must stay inside the bend range of the target (Bend Up, Bend Down). If it goes outside, the note starts again at the new pitch.
- The display parameters (rows, cells, playhead, cursor) show in the parameter lists of the MPC. Do not change them.
- Sustain on keygroup targets: see Scala Retuner's known issues (the MPC ignores pedal up on keygroup tracks). scalaSeq does not forward CC64.
