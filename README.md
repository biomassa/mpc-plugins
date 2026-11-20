# mpc-plugins

Plugins for Akai MPC standalone hardware, made with the official [MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk). The manufacturer name of the plugins is blackdingus.

> This software was developed with the assistance of a LLM.

## Plugins

| Plugin | Type | Description |
|---|---|---|
| [Scala Retuner](Scala%20Retuner/) | Instrument (silent) | Retunes the notes of its track to a Scala scale. Keygroup and OPx-4 tracks play the retuned notes. |
| [scalaSeq](scalaSeq/) | Instrument (silent) | A monophonic pitch and gate sequencer for Scala tunings, with lanes and chance operations. It plays MIDI on keygroup and OPx-4 tracks. |

Each plugin folder has a README, a CHANGELOG and a list of known issues. Code that more than one plugin uses is in `common/`.

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

The output is in `plugins/<Name>/build/blackdingus - VST - <Name>/`. Copy this folder into a `VST2 Plugins` folder on the MPC, then restart the MPC.

## License

MIT. See [LICENSE](LICENSE).
