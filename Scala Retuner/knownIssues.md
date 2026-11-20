# Known issues

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
