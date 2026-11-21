# Changelog

This file records all notable changes to Scala Retuner.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.2.3] - 2026-11-21

### Removed

- The Q-Link action change of 1.2.2. It was not based on the SDK documentation, and it changed nothing on the MPC. The cause of the wrong Q-Link assignments is not known yet; see knownIssues.md.

## [1.2.2] - 2026-11-21

### Fixed

- Touching a control did not select its Q-Link bank, so the Q-Links changed other parameters than the touched one. The fine knob step (1.3.1 / 1.2.1) wrapped each control's handle in DisableCoarseIncrement, and the MPC did not find the Q-Link bank of a wrapped handle. The Q-Link action now uses the plain parameter.

## [1.2.1] - 2026-11-21

### Changed

- Knobs, faders and the data wheel move by the fine step without Shift. Before, the normal step was too coarse. Shift gives no finer step.

## [1.2.0] - 2026-11-21

### Added

- The ROLL page: a scrolling piano roll in the pitch classes of the scale.
  - One horizontal line for each scale degree in one period, with the degree number at the left. Degree 0 is the brighter bottom line. Up to 31 lines.
  - 16 columns of note markers, one color for each of the first 8 output channels. The markers scroll to the left by one column each sixteenth note, at the tempo of the MPC. When the sequencer is stopped, they scroll at the last tempo (120 bpm if none).
  - The newest column (at the right) follows the notes and the bend wheel at once. A note that is shorter than one step also gets a marker.
  - The top row shows the last note as scale degree, period number and cents (for example `7\22`, `Oct +0`, `381.8c`).
  - Q-Links 1-4 control Scale, Mapping, Root and In Bend.
- Display parameters 11-197: Grid X, Grid 1-31, Trace 1-8, Last Deg, Last Oct, Last Cents, Col X 1-16 and 128 roll cells (C1 V1 to C16 V8). The plugin writes them. If you change them, the plugin writes them again.
- `skin/make_skin.py` makes the skin in `skins/`: the RETUNE page from `ui.json` and the ROLL page.
- CPU, measured on the MPC One G2 with a dense pattern: the ROLL page adds about 35% of one CPU core (about 4% of all 8 cores) while it is on the screen. Other pages add nothing measurable. The plugin itself uses less than 0.01% CPU.

### Changed

- The skin is hand-written now (`skins/`). After a change to `ui.json` or to the parameters, run `skin/make_skin.py` before the build.
- The worker thread sends display updates at most each 10 ms, and only for the values that changed.

## [1.1.0] - 2026-11-20

### Added

- The In Bend parameter (0-24 scale steps, default 2). The bend wheel moves the pitch in scale steps. Wheel fully up gives the pitch of the key that is In Bend steps higher. Wheel fully down gives the pitch of the key that is In Bend steps lower. Values between are interpolated in cents. 0 disables the wheel.
- Pitch bend forwarding. The plugin adds the wheel bend to the retuning bend of each note. It sends the result on all channels that played a note, also during the release.
- All CCs go to all output channels in use.
- Channel aftertouch goes to all output channels in use. Polyphonic aftertouch goes to the channel of its note.
- Sustain (CC64). A note that you release while the pedal is down keeps its channel until the pedal is up. New notes do not use that channel. Keygroup tracks do not release on pedal up: see [knownIssues.md](knownIssues.md).
- All Notes Off (CC120, CC123) releases all notes. The plugin also forwards it.
- `knownIssues.md`.

### Changed

- Version 1.1.0 is tested on the MPC One G2 (MPC 3.10.1), with a keygroup with a sine sample and 22edo. All wheel positions are within 0.1 cents. CC1, CC11 and channel aftertouch arrive 1:1.

## [1.0.0] - 2026-11-20

### Added

- A silent instrument plugin. It retunes the notes of its track to a Scala scale (.scl) and an optional keyboard mapping (.kbm).
- For each note, the plugin sends the nearest 12-TET note and a pitch bend through its own ALSA MIDI port, "Scala Retuner MIDI Out". Each note gets its own MIDI channel.
- Parameters: Velocity, Scale, Mapping, Root, Root Tune, Bend Up, Bend Down, Curve, First Ch, Voices.
- Curve: Symmetric for keygroups, OPx-4 for OPx-4.
- Scales: 12-edo, 22edo, 31-edo, ji_7, 31edo.kbm.
- Tested on the MPC One G2 (MPC 3.10.1), 22edo, keys 50-82. A keygroup with a sine sample is within 0.3 cents. OPx-4 is within 3.3 cents.
