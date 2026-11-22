# Scala Retuner

Scala Retuner is an instrument plugin for Akai MPC standalone hardware (MPC OS 3.10.1 or later). It retunes the notes of its track to a Scala scale (.scl) and an optional keyboard mapping (.kbm). Other tracks play the retuned notes.

> This software was developed with the assistance of a LLM.
>
> **The plugins are in a VERY EARLY DEVELOPMENT STAGE.** Expect bugs, missing features and changes that break saved projects. Many functions are not yet tested on the MPC. A plugin bug can freeze the MPC: save your work often.

The user guide is in [docs-Scala Retuner.md](../docs-Scala%20Retuner.md). This README has the details.

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
| 11-197 | Grid X, Grid 1-31, Trace 1-8, Last Deg, Last Oct, Last Cents, Col X 1-16, C1 V1 to C16 V8 | | Display parameters for the ROLL page. The plugin writes them. Do not change them. |

## ROLL page

The ROLL page is a scrolling piano roll in the pitch classes of the scale.

- Each horizontal line is one scale degree in one period. The number at the left is the degree. The bottom line (brighter) is degree 0. The top of the area is the next period. The page shows up to 31 lines.
- The colored markers are the notes. Each color is one output channel, from First Ch to First Ch + 7. A marker shows the pitch class of the note. The newest column is at the right and moves with the bend wheel.
- The markers scroll to the left by one column each sixteenth note, at the tempo of the MPC. 16 columns show one bar of 4/4. When the sequencer is stopped, the markers scroll at the last tempo (120 bpm if none).
- The top row shows the last note: scale degree (for example `7\22`), period number relative to the root (`Oct +0`), and cents above degree 0 (`381.8c`).
- Q-Links 1-4 control Scale, Mapping, Root and In Bend.

The ROLL page uses more CPU while it is on the screen (see [known issues](knownIssues.md)). Other pages do not.

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
python3 "plugins/Scala Retuner/skin/make_skin.py"
python3 tools/build.py "plugins/Scala Retuner"
.sdk-tools/host_sim "plugins/Scala Retuner/build/native/scala-retuner.dylib" test --wav "plugins/Scala Retuner/build/test.wav"
sh "plugins/Scala Retuner/tests/run.sh"
```

On Linux, the native library is `scala-retuner.so`.

The skin in `skins/` is hand-written, so the build does not run the SDK generator. `skin/make_skin.py` makes the skin again: the RETUNE page from `ui.json`, and the ROLL page. It needs `build/params.json`, so build one time before you run it. Run it after each change to `ui.json` or to the parameters.

The host simulator reports silence. This is correct, because the plugin makes no sound.

`tests/run.sh` tests:

- Scala and .kbm parsing.
- Retuning accuracy: ji_7 within 0.13 cents, 22edo within 0.04 cents (bend range ±24).
- The bend wheel in scale steps.
- CC, aftertouch and sustain forwarding.
- Channel rotation and voice stealing.
- Save and load of the project state.
- The display parameters and the scrolling of the ROLL page.

## License

MIT. The scale files come from the author's LinnStrument project. ji_7.scl is from the Scala archive (Manuel Op de Coul).
