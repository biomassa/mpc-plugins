# Known issues

## Not tested on the MPC

- **First check on the MPC:** do the pads of a drum track play the notes of "percSeq MIDI Out"? In the CC test (2026-11-21) a drum track did not play the notes of a test program on the same ALSA path (the cause is not known). scalaSeq's notes on this path play on keygroup and OPx-4 tracks. Check the Track switch of the port (Preferences → MIDI), the MIDI Input Channel and MIDI Monitor of the drum track, and which pad note 36 plays.
- **Status (1.0.0, 2026-11-22):** tested in the unit tests and the host simulator only.
- 1.0.0 on the MPC One G2 (2026-11-22, reported by the user): the SEQ page button did not react, the audio crackled at each touch or Q-Link edit, Q-Links sometimes stopped, touches sometimes gave no note, and the Q-Link status pop-up stayed open. Cause not proven. Measured offline: one touch on a hit made 51 display reports, one scroll step 252 (scalaSeq: about 23 for a jump). 1.0.1 draws 32 hits and sends at most 30 reports for each 50 ms pass. Not yet tested on the MPC.
- 1.0.1 has 1,145 parameters (scalaSeq has 1,298).
- Not known: if the row labels (Value labels at fixed places) and the per-row playheads draw on the G2.
- Not known: if the ROW page shows "Row" for the Sel Row knob (WithName and DisableCoarseIncrement together on one control).
- Not known: if the POOL pads and the ROW row pads (Button widgets; Name labels with WithName for the row numbers) draw and react on the G2. The SDK calls Button well-tested; scalaSeq does not use it.
- Not known: the default pad notes of a new drum track, and if the "Chromatic C1" pad note map starts at note 36.
- CPU on the MPC is not measured. Host simulator (desktop): about 0.5 % of a block with hits in all 16 rows.

## SMP REV and CC

- Reported by the user (2026-11-21): MIDI Learn learns a CC from an ALSA port like percSeq's, and the CC switches the Layer Reverse of a pad (the pad was played by hand).
- Not measured: if a CC that comes just before a note acts on that same hit. percSeq sends the CC in the same audio block as the note, just before it.
- The MPC stores the settings of a MIDI input port (Track, Control) under the ALSA client number of the port, not under its name (seen in /data/Settings/MPC/MPC.settings). If the client number of "percSeq MIDI Out" changes (for example another plugin with a MIDI port loads first), the MPC can show it as a new port without your settings.
- percSeq remembers the last Rev CC and CC value it sent for each row, and sends them back to forward and rest when the MPC stops. A jump or a sequence switch does not reset them.

## Timing

- Notes go out once for each audio block, as in scalaSeq (resolution one block, 2.9 ms at 128 frames).
- All rows run half a step ahead of the song. After a jump, a sequence switch or a Reset, the rows catch up over this half step in 64 sub-ticks (1/128 step).
- The FLAM grace hit of the first hit after a jump or switch is dropped (its time has passed).
- In the unit tests some messages go out one sample after their exact time (floating-point rounding at step boundaries).

## Rows

- Rows without hits move their playhead once per audio block. At 512-frame blocks and more than 320 bpm, a 1/64 row without hits can skip a step on the screen (display only).
- SND NEXT looks forward in each row's play range. It does not look at Direction, jumps or chance operations.
- Two rows that play the same note at the same time: percSeq sends a note-off before the second note-on, and one note-off after the last hit ends.

## Row on/off and step squares (1.1.0)

- Not tested on the MPC. The off-row shade appears through a visibility rule (IndexedEnabling on the row's On parameter) and the step squares use constant XYPad y values (Fixed handles with values other than 0); both are documented in the SDK but not yet used by scalaSeq or percSeq on the G2.
- The step squares move once per step of each row, as the red lines did: about 120 screen updates per second while playing (offline, 8 rows of 1/16 at 120 bpm). Cadenza, another plugin, keeps this under 80 per second.

## Lists, faders and pages (1.1.0)

- Not tested on the MPC: the drawn lists (Button radio groups in custom components, shown with a visibility rule on "List Open"), the < > and FOLLOW buttons and the page flips. The button texts are pictures drawn with the Mac's Helvetica font.
- The MPC's own list overlay (double-tap on a list control) opens empty for this plugin (seen on the MPC One G2; also reported by the Open MPC project). The drawn lists replace it.
- A page flip redraws all hits in view: about 210 display reports for 8 rows with hits; at 30 reports per 50 ms the page needs about 0.35 s to draw completely.

## Text sizes and step controls (1.2.0)

- Not tested on the MPC. The SDK docs give the font height in pixels but do not say if it is the em size or the ascent plus descent of the font. The text widths were measured with Titillium Web (the MPC's font) for both cases. In the em case, the fader names and values fit only because each uses the full slot width.
- The < > buttons are momentary parameters (Hit Prev/Next, Row Prev/Next, Move Prev/Next, Len Prev/Next): the button sets the parameter on, the plugin steps the value and sets it back to off. Not known: if a fast double tap on the MPC gives two steps.
- A Q-Link turn on Hit, Move, Length or Row can still give a value the plugin does not use; the plugin writes back the value it uses (as before). There is no fader handle now, so nothing springs back on the screen.
- Host simulator: with some random seeds the state round trip reports a parameter that the plugin sets on load by design (seed 1234: List Open, which closes on load; seed 1: Range End, which is limited to Steps). Not caused by this version as far as the code shows; the 1.1.0 build was not run again with the same seeds to compare.

## Ratio rates, locks and lanes (1.3.0)

- Not tested on the MPC.
- The lane bars and the velocity bars are XYPad markers with pads twice the lane height (as the velocity bars before); the lower half is under a mask and under the LANE controls. The validator warns that these pads overlap the Q-Link frames (accepted).
- COPY / PASTE / CLEAR: the copy is held in memory only (not in the project). PASTE waits for the next audio block if another instance is copying at the same moment.
- Host simulator: with some seeds the state round trip reports a one-touch button (Copy, Paste, Clear Seq, SEND, the < > buttons) as not restored: the plugin sets them back to off on load (by design).
- Measured 2026-11-22 (percSeq, MPC One G2): a single CC message reached the MPC (MIDI Monitor) but MIDI Learn ignored it; a lane playing changing CC 74 values with no notes was learned. While notes also play, Learn takes the first note. Also measured: MIDI Learn turns off when its page is left. So SEND keeps sending a zigzag until a second touch or 15 s. Confirmed on the MPC One G2 (2026-11-22, percSeq): MIDI Learn takes the CC from SEND. Not separated: whether Learn needs changing values or only several messages.
- Not known: if WithName titles on generated faders (CC Num, Steps, Fine on LANE) show on the G2.
- The host simulator lists choices that look like numbers (Ratio) as a float; the skin script uses its own ratio list.
- Not measured on the MPC: the display traffic of a lane redraw (up to 64 bars per page).
- A lock uses the Lock CC of the row whose sound plays (a SND operation can change the row); my choice, as the CC operation.

## Port settings (2026-11-22)

- Measured on the MPC One G2: with Global on for "percSeq MIDI Out", selecting the percSeq track made a MIDI loop (pads blinking, broken sound); with Control off, SEND did not reach MIDI Learn. See "The port settings on the MPC" in the README. SEND with Control on is not yet confirmed.

## SEQ page touch cells (Unreleased, 2026-11-22)

- Not tested on the MPC. The cells have the same structure as the generated faders (a container that selects the Q-Link column at a touch, with a knob inside). On the G2 those faders select their column.
- Measured on the G2 (user photo, 2026-11-22): a Knob of type ValueSlider draws no value text. The value is now a Label under the knob.
- Not known: if a vertical drag on the (invisible) ValueSlider knob changes the value on the G2.
- Not known: if a drag on a value that the plugin limits jumps. Thus Hit, Row, Move, Length and Sel Row have no drag.
- Not known: if 34 px values fit the 68 px columns. Long values (for example 1/16T) can be clipped.
- Not known: if the row number and the note (two labels side by side) fit on the SEQ row labels at the left of the roll (30 px for the note). Measure on the MPC.

## Kept hits (Unreleased, 2026-11-22)

- Kept hits count toward the limit of 64. When the limit is reached, a new hit cannot be added, and a cut hit may stay cut (its full-length copy does not fit).
- A kept hit that comes back over a hit you added meanwhile at the same place can overlap it. Not tested.
- Not tested on the MPC.
- "+N kept" uses the 66 px "more" label: "+10 kept" and longer can be clipped. Not measured.
