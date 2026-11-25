# Known issues

## Not tested on the MPC

- **First check on the MPC:** do the pads of a drum track play the notes of "percSeq MIDI Out"? In the CC test (2026-11-21) a drum track did not play the notes of a test program on the same ALSA path (the cause is not known). scalaSeq's notes on this path play on keygroup and OPx-4 tracks. Check the Track switch of the port (Preferences → MIDI), the MIDI Input Channel and MIDI Monitor of the drum track, and which pad note 36 plays.
- **Status (1.0.0, 2026-11-22):** tested in the unit tests and the host simulator only.
- 1.0.0 on the MPC One G2 (2026-11-22, reported by the user): the SEQ page button did not react, the audio crackled at each touch or Q-Link edit, Q-Links sometimes stopped, touches sometimes gave no note, and the Q-Link status pop-up stayed open. Cause not proven. Measured offline: one touch on a hit made 51 display reports, one scroll step 252 (scalaSeq: about 23 for a jump). 1.0.1 draws 32 hits and sends at most 30 reports for each 50 ms pass. Not yet tested on the MPC.
- 1.0.1 has 1,145 parameters (scalaSeq has 1,298).
- Seen on the MPC One G2 (user photo, 2026-11-24): the row labels draw (the square and the note). The grey row number does not show at the left of the label (not known why: covered by the square, or cut). The row numbers at the right of the roll show. Not checked: the per-row playheads.
- Seen on the MPC One G2 (user photo, 2026-11-24): the ROW page shows "Row" for the Sel Row knob (WithName and DisableCoarseIncrement together work).
- Seen on the MPC One G2 (user photo, 2026-11-24): the ROW row pads draw with their numbers, and the selected row is lit. Not checked: a touch on them, and the POOL pads.
- Not yet seen on the MPC (2026-11-25): the ROW header CHANCE OP CCs with the labels REV CC, OP CC, OP REST, and the POOL header.
- Not yet seen on the MPC (2026-11-25): the fix for hits seen for a moment while editing on the SEQ page. The desktop test shows no wrong bars between display passes. A scroll, a zoom or a sequence switch still moves most bars and takes several passes (30 reports each 50 ms), so the screen can show a mix of old and new bars for up to a few hundred ms.
- Q-Links that seem stuck (user, 2026-11-25: on every page, mostly just after a page change, a turn changes nothing for a while; then the knob works): cause unknown. A trace on the MPC (2026-11-25, after the plugin was removed and added again) showed every turn arriving at percSeq at once, on every page tested (HIT, SEQ, CHANCE, LANE, LOCK; 21 parameters, about 1,700 writes), and the user saw no dead knobs in that test. Not proven: that the old Q-Link assignments in the project caused it. If it comes back: note the page and knob and whether the plugin was added again after the update. A desktop sweep (2026-11-25) set each of the 88 Q-Link parameters one step up and down, stopped and playing: all held, except intended limits: Row does not move a hit to a row where it does not fit; Amount stays 0 for an operation without an amount; Clear Ops is a button. Not yet known which page and knob, and whether the project still has the Q-Link assignments from before the 2026-11-24 redesign (remove the plugin and add it again).
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

## Page and Q-Link redesign (Unreleased, 2026-11-24)

- Measured on the MPC One G2 (user, 2026-11-24): the Q-Link button stops on every column of a page, also on a column with no controls (its 4 knobs then do nothing). The tab bar shows 5 page buttons; the other pages are behind > and <.
- Seen on the MPC One G2 (user photos, 2026-11-24): the MPC draws an orange "Q" badge at the top-left corner of the active Q-Link frame. It covered the bank title on the SEQ panel and the first letter of the top control on the other pages. The badge size (about 45 x 34 px) is estimated from a photo, not measured. The frames now start 36 px above the top control. Not yet seen on the MPC.
- Sub-pages (RATE under ROW: push the same page button again) work on the MPC One G2 (user, 2026-11-25). The skin scripts keep a switch (SUBTABS = False) that gives each page its own button.
- Not tested on the MPC: the LANE drawing knobs (Lane Step, Lane Value), the cursor marker and the STEP / VALUE (or HIT / VEL) controls in column 1 (2026-11-25; before, the knobs worked on the MPC One G2 but had no names on the screen); the short Lock CC fader (a film strip rendered with the SDK kit code, 150 px) with SEND at its right; the taller Snap pads; the CHANCE header strip; the DELETE button.
- "Clear All Ops" measures 178 px in Titillium Web at 34 px; the switch name box is 170 px. "RATCH" measures 71 px in the 64 px boxes of the SEQ panel and shows in full on the MPC (user photo), so the name probably shows too. Not seen on the MPC.
- The Q-Link frames do not have the same gap on every side (the validator warns "sits unevenly"): the room above is for the Q badge. On LANE, the frame of column 1 encloses only the settings; knobs 1-2 (Lane Step, Lane Value) have no control on the screen.
- DELETE has no Q-Link (a knob turn must not delete), so knob 4 of its column does nothing.
- Host simulator: with some random seeds the state round trip still reports a one-touch button or Range End as not restored (seed 1234: Send Rev CC). By design, as before (see "Ratio rates, locks and lanes").
- 2026-11-25: the LANE lane has its own x scale (lane x = roll x * 918 / 880, from x 343). Not seen on the MPC.

## Ratchet hits of the selected hit (Unreleased, 2026-11-25)

- Not tested on the MPC. The hits are 292 new markers (4 brightness steps x 8 hits x 9-10 pieces), each a display parameter. Not measured: the display traffic when the selection or RATCH, DUTY or FADE changes (only the markers that move are sent).
- 4 brightness steps are coarse: with FADE +50 and RATCH 4, hits 2-4 (velocities 21, 5 and 1 %) all show at the dimmest step.

## Performance (Unreleased, 2026-11-25)

- CPU on the MPC is not measured. Desktop, 16 rows of hits: 17 us per block (median). The MPC One G2 is aarch64 (Cortex-A55); the SDK gives no factor for it (its factor of 15-25 is for the 32-bit Cortex-A17). Since 2026-11-25 an idle row ticks once per block; a row with a hit sounding (or with SND NEXT anywhere in the sequence) still ticks every sample, so a dense pattern costs as before (desktop: about 20 us per block). Skipping inside a sounding hit would sum its time differently and could move a gate edge by one sample: not done.
- Seen on the MPC One G2 (2026-11-25): the MPC cuts a control at the edge of its container (the tall Snap pads ran 19 px below their slot-sized container and were cut in half). Fixed by a taller container. Not known: whether the page itself shows all 628 px; the CHANCE page's bottom faders (moved 17 px down) end at about y 621: not yet seen on the MPC.

