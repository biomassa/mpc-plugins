# Known issues

## Knob steps

- All controls use the fine step without Shift (the `DisableCoarseIncrement` modifier and `disableCoarseDataWheel` in the skin). The SDK documents these for touch controls and the data wheel. Not known: if the hardware Q-Links also use the fine step.
- Shift gives no finer step. Whole-number parameters already move by 1 for each click. For continuous parameters, a finer Shift step needs the MPC to read step sizes from the plugin (VST2 parameter properties); not tested.

## ROLL page

- **CPU:** while the ROLL page is on the screen, the MPC uses more CPU to draw the markers: about 35% of one core with a dense pattern (MPC One G2). With other pages on the screen there is no measurable increase.
- **Automation:** the display parameters change while you play. If the track records automation, the MPC can record these changes. Not tested.
- The display parameters (11-197) show in the parameter and automation lists of the MPC.
- If you touch the roll area, the touched marker or line can move. The plugin puts it back at the next update.
- The MPC One G2 does not draw the Plotter widget, and the SDK documents it as "less tested". The ROLL page uses XYPad markers instead.

## Sustain does not release on keygroup tracks

- **Found:** 2026-11-20, MPC One G2, MPC 3.10.1.12, Scala Retuner 1.1.0.
- **Symptom:** On a keygroup track, the sustain pedal holds a note
  correctly. When you release the pedal, the note does not stop. It
  stops only when the same note plays again.
- **Cause:** The MPC. The keygroup track ignores CC64 value 0. The
  problem is the same when the pedal messages come directly over
  USB, without Scala Retuner. Scala Retuner sends the pedal-up
  message correctly (recorded on its port with `arecordmidi`).
- **Workaround:** Do not use the sustain pedal with keygroup
  targets.
- **Possible fix in the plugin (not done):** The plugin keeps the
  note-offs while the pedal is down and sends them on pedal up. It
  does not forward CC64.

## The MPC filters repeated sustain values

- The MPC sends CC64 to the plugin only when the pedal state
  changes. A second "pedal up" message does not reach the plugin.
  This has no effect on the sound.

## OPx-4 presets with vibrato or drift

- Some OPx-4 presets have vibrato or pitch drift. The same note
  can be 10-15 cents different each time. This is not a retuning
  error. Use a preset without pitch modulation to check the tuning.

## Not tested or not supported

- The MIDI channel of the input is ignored. All input channels are
  merged. LinnStrument channel-per-note input is not supported yet.
- Program change and SysEx are not forwarded.
- Akai synths other than OPx-4 (TubeSynth, Bassline and others):
  their bend curve is not measured.
- If you increase Voices, the new channels get only the CCs sent
  after the change.
- The output port is not part of the official SDK host contract.
  A future MPC OS can change this behavior.
