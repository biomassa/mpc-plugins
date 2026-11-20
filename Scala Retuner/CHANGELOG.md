# Changelog

This file records all notable changes to Scala Retuner.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
