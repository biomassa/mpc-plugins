# Scala Retuner

Scala Retuner is an instrument plugin for Akai MPC standalone hardware (MPC OS 3.10.1 or later). It retunes the notes of its track to a Scala scale (.scl) and an optional keyboard mapping (.kbm). Other tracks play the retuned notes.

> This software was developed with the assistance of a LLM.

## How it works

- The plugin makes no sound.
- For each note on its track, the plugin finds the nearest 12-TET note and the difference in cents.
- It sends a pitch bend for the difference, then the note, through its own MIDI port. The MPC shows this port as "Scala Retuner MIDI Out".
- Each note gets its own MIDI channel. One pitch bend per channel gives each note its own tuning.
- Keygroup tracks and OPx-4 tracks that receive from this port play the retuned notes.

The MPC ignores MIDI that a plugin sends to the host. Thus the plugin opens its own ALSA sequencer port. This is not part of the official SDK host contract. The plugin loads `libasound.so.2` at run time with `dlopen`, so it does not link other libraries. [sd88me/mpc-vst-maze](https://github.com/sd88me/mpc-vst-maze) (MIT) uses the same method.

## Features

- Scala .scl scales and .kbm keyboard mappings.
- Accuracy on the MPC One G2: a keygroup is within 0.3 cents, OPx-4 is within 3.3 cents (22edo, keys 50-82).
- Up to 16 voices. Each voice uses one MIDI channel and one target track.
- Bend wheel range in scale steps.
- Forwards all CCs, channel aftertouch and polyphonic aftertouch.
- The project stores the scale and the mapping. A project keeps its tuning if you rename or delete the files.
- Low CPU use. The plugin does no work for each audio sample.

See the [changelog](CHANGELOG.md) and the [known issues](knownIssues.md).

## Requirements

- An Akai MPC with MPC OS 3.10.1 or later. The plugin runs only on standalone hardware, not in MPC desktop software.
- Preferences → Enhancements (Beta) → Enable Enhancements and External Plugin Support set to on.

## Installation

1. Build the plugin (see [Build and test](#build-and-test)).
2. Copy the folder `build/blackdingus - VST - Scala Retuner/` into a `VST2 Plugins` folder on a drive of the MPC. On the MPC One G2, use the internal storage (`/storage/VST2 Plugins`). The SD card is mounted `noexec`, and plugins on it do not load.
3. Restart the MPC. The plugin is in the plugin browser under External Plugins.

## Setup on the MPC

1. Put Scala Retuner on a track. Set the MIDI Input Port of this track to your controller. Do not use "All Ports". With "All Ports", the track also receives the output of the plugin.
2. Make one target track for each voice. For example, for 4 voices, make 4 keygroup tracks with the same sample.
3. On each target track, set:
   - MIDI Input Port: Scala Retuner MIDI Out.
   - MIDI Input Channel: one channel from First Ch to First Ch + Voices - 1. Each target track gets a different channel.
   - MIDI Monitor: In.
4. In Preferences → Hardware → Global Pitch Bend, set the bend range. Set Bend Up and Bend Down in the plugin to the same values.
5. Set Curve: Symmetric for keygroups, OPx-4 for OPx-4.

For a monophonic line, use one target track and set Voices to 1.

If the MIDI Monitor of a target track is Auto and you select that track, the track also receives the notes of your controller. Then you hear each note two times. Use MIDI Monitor In.

## Parameters

| # | Name | Range | Function |
|---|---|---|---|
| 0 | Velocity | 1-200 % | Scales the velocity of the output notes. |
| 1 | Scale | .scl files | The scale. |
| 2 | Mapping | Linear, .kbm files | Linear: scale degree 0 is on Root, and each key is the next scale degree. |
| 3 | Root | 0-127 | The key of scale degree 0. Only for Linear. |
| 4 | Root Tune | -100 to +100 cents | The pitch of Root, relative to 12-TET. Only for Linear. |
| 5 | Bend Up | 1-24 semitones | Must be equal to the bend range up of the target. |
| 6 | Bend Down | 1-24 semitones | Must be equal to the bend range down of the target. |
| 7 | Curve | Symmetric, OPx-4 | The bend curve of the target. |
| 8 | First Ch | 1-16 | The first output MIDI channel. |
| 9 | Voices | 1-16 | The number of output channels. This is the polyphony. |
| 10 | In Bend | 0-24 scale steps | The range of your bend wheel in scale steps. 0 disables the wheel. |

### Bend range and accuracy

- One bend step is the bend range divided by 8192. At ±2 semitones, one step is 0.02 cents. At ±24 semitones, one step is 0.3 cents.
- The retuning bend is at most ±0.5 semitone. The wheel bend adds to it. Thus Bend Up and Bend Down must be larger than the largest wheel bend plus 0.5 semitone. Else the plugin limits the bend.
- Curve: on MPC 3.10.1, keygroups use value/8192 of the range in both directions. OPx-4 uses value/8064 of the range up and value/8192 down. Other Akai synths are not measured.

### Bend wheel

- Wheel fully up gives the pitch of the key that is In Bend steps higher. Wheel fully down gives the pitch of the key that is In Bend steps lower.
- Values between are interpolated in cents.
- The wheel bend applies to all channels that played a note, also during the release.

### Forwarded messages

- All CCs go to all output channels in use.
- Channel aftertouch goes to all output channels in use.
- Polyphonic aftertouch goes to the channel of its note.
- Sustain (CC64): a note that you release while the pedal is down keeps its channel until the pedal is up. Keygroup tracks do not release on pedal up. See [known issues](knownIssues.md).
- All Notes Off (CC120, CC123) releases all notes.
- Program change and SysEx are not forwarded.

### Voice allocation

- A new note uses the free channel that was released first. Thus the release of a note does not get the bend of the next note.
- If all channels are in use, the oldest note stops and its channel plays the new note.

## Scales

The plugin reads `.scl` and `.kbm` files from `Content/Scales/` in the plugin folder. It reads the list when the plugin loads.

| File | Description |
|---|---|
| 12-edo.scl | 12 equal divisions of the octave. The default. |
| 22edo.scl | 22 equal divisions of the octave. |
| 31-edo.scl | 31 equal divisions of the octave. |
| ji_7.scl | 7-limit just intonation interpretation of 7-tET (Manuel Op de Coul). |
| 31edo.kbm | Scale degree 0 on MIDI note 60, 261.63 Hz. |

## CPU

- The plugin does no work for each audio sample.
- For each note, it reads one entry of a table with 128 entries and puts two messages in a lock-free queue.
- A worker thread reads the scale files and calculates the table when a parameter changes. The audio thread then uses the new table. It does not wait.
- The worker thread sends the MIDI messages. It sleeps until there are messages or parameter changes. No system calls occur on the audio thread.
- The output is approximately one audio block later than the input.

## Build and test

From the SDK root (`sdk/`):

```sh
python3 tools/build.py "plugins/Scala Retuner"
.sdk-tools/host_sim "plugins/Scala Retuner/build/native/scala-retuner.dylib" test --wav "plugins/Scala Retuner/build/test.wav"
sh "plugins/Scala Retuner/tests/run.sh"
```

On Linux, the native library is `scala-retuner.so`.

The host simulator reports silence. This is correct, because the plugin makes no sound.

`tests/run.sh` tests:

- Scala and .kbm parsing.
- Retuning accuracy: ji_7 within 0.13 cents, 22edo within 0.04 cents (bend range ±24).
- The bend wheel in scale steps.
- CC, aftertouch and sustain forwarding.
- Channel rotation and voice stealing.
- Save and load of the project state.

## License

MIT. The scale files come from the author's LinnStrument project. ji_7.scl is from the Scala archive (Manuel Op de Coul).
