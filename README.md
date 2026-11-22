# mpc-plugins

Plugins for Akai MPC standalone hardware, made with the official [MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk). The manufacturer name of the plugins is blackdingus.

> This software was developed with the assistance of a LLM.
>
> **The plugins are in a VERY EARLY DEVELOPMENT STAGE.** Expect bugs, missing features and changes that break saved projects. Many functions are not yet tested on the MPC. A plugin bug can freeze the MPC: save your work often.

## Plugins

| Plugin | Type | Description |
|---|---|---|
| [Scala Retuner](Scala%20Retuner/) | Instrument (silent) | Retunes the notes of its track to a Scala scale. Keygroup and OPx-4 tracks play the retuned notes. |
| [scalaSeq](scalaSeq/) | Instrument (silent) | A monophonic pitch and gate sequencer for Scala tunings, with lanes, chance operations, locks and CC lanes. It plays MIDI on keygroup and OPx-4 tracks. |
| [percSeq](percSeq/) | Instrument (silent) | A 16-row step sequencer for drum tracks and chopped samples. Each row has its own playhead, rate and length. Chance operations with a sound pool, FLAM, sample reverse, locks and CC lanes. It plays MIDI on a drum track. |

The user guides (features and basic use) are in the root of this repository: [docs-percSeq.md](docs-percSeq.md), [docs-scalaSeq.md](docs-scalaSeq.md) and [docs-Scala Retuner.md](docs-Scala%20Retuner.md).

Each plugin folder has:

- `README.md`: the details (parameters, setup, build and test).
- `CHANGELOG.md` and `knownIssues.md`.

The plugins send their MIDI through their own ALSA port, for example "percSeq MIDI Out". This is not part of the official SDK host contract.

Code that more than one plugin uses is in `common/`.

## Tools

Test tools for the MPC. They are not for music.

| Tool | Description |
|---|---|
| [tools/cc_monitor](tools/cc_monitor/) | CC Monitor, a silent plugin. It shows the last CC, a CC count and the last note that the MPC gives to a plugin on its track. |
| `tools/midi_probe` | A program that logs each event of an ALSA port with its time. |
| `tools/midi_send` | A program that sends a CC and a note through an ALSA port at a fixed interval. |
| `touchProbe` | A silent plugin that measures touch on the MPC screen. |

## Requirements

- An Akai MPC with MPC OS 3.10.1 or later. The plugins run only on standalone hardware, not in MPC desktop software.
- Preferences → Enhancements (Beta) → Enable Enhancements and External Plugin Support set to on.
- To build: a clone of the MPC Plugin SDK and Python 3. The SDK installs its toolchain (Zig) at the first build.

## Build

This repository is the `plugins/` folder of an SDK clone. The SDK ignores this folder.

1. Clone the SDK:

   ```sh
   git clone https://github.com/inmusic-software/mpc-plugin-builder-sdk.git sdk
   ```

2. Get this repository into `sdk/plugins`. The folder already exists in the SDK clone, so use `git init` and `git pull`, not `git clone`:

   ```sh
   cd sdk/plugins
   git init -b main
   git remote add origin git@github.com:biomassa/mpc-plugins.git
   git pull origin main
   cd ..
   ```

3. Build a plugin from the SDK root. The build makes the native, 32-bit ARM and 64-bit ARM libraries:

   ```sh
   python3 tools/build.py "plugins/Scala Retuner"
   ```

The output is in `plugins/<Name>/build/blackdingus - VST - <Name>/`. Copy this folder into a `VST2 Plugins` folder on the internal storage of the MPC, then restart the MPC. On the MPC One G2 the SD card cannot load plugins (it is mounted `noexec`).

## License

MIT. See [LICENSE](LICENSE).
