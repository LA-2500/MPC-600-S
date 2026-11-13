# MPC 600 S

A pad sampler and sequencer for the **Akai MPC Sample**. It runs from the SD card: put the card in and it starts; take it out and the MPC Sample is as it came.

## Download

| | |
|---|---|
| **[⬇ Download the OS (mpc-os.zip)](../../releases/latest/download/mpc-os.zip)** | The MPC 600 S, ready for the SD card |
| **[⬇ Quick Start guide (PDF)](../../releases/latest/download/MPC-600-S-Quick-Start.pdf)** | Installing, in two pages |
| **[⬇ Operator's Manual (PDF)](../../releases/latest/download/MPC-600-S-Manual.pdf)** | Everything, in detail |
| [⬇ Stems models (optional, 107 MB)](../../releases/latest/download/MPC600S-stems.zip) | Only for the Stems page |

These always get the newest version. Earlier versions are on the [Releases](../../releases) page.

## Install

1. Unzip **mpc-os.zip**. Copy the `autorun` folder inside it to the **top level** of an SD card (FAT32 or exFAT), with its name and files unchanged.
2. With the MPC Sample off, put the card in and turn it on. A few seconds after the MPC Sample starts, the MPC 600 S takes over the screen.
3. To go back to the stock MPC Sample, turn it off and take the card out. To update, replace the `autorun` folder with the new one; your work, in the card's `MPC600S` folder, is kept.

The [Quick Start guide](../../releases/latest/download/MPC-600-S-Quick-Start.pdf) has the details and what to check if it does not start. On the MPC Sample, **SHIFT + SAMPLE REC** shows the help for any page.

## What It Does

- **Sequencer**: 99 sequences of 99 tracks, 96 PPQN, Timing Correct with MPC 3000, MPC60, MPC 2000 and SP12 swing; songs; Step Edit; Note Repeat and an arpeggiator with step patterns and a pattern editor.
- **Sampling** at real low sample rates and bit depths (7–44.1 kHz, 16/12/8/4 bits), from the line input, a guitar or bass (INST), the mic or a computer over USB; audio tracks; resampling; stem separation.
- **Programs**: drum programs, and S600 programs laid out after Akai's S-series samplers, with envelopes, filters (including a ladder low-pass), LFO and layers.
- **Editing**: trim, loop, zones, warp and S-series-style timestretch, print effects.
- **Effects**: input, track send and pad effects (reverbs, delays, modulation, drives, amps and cabinets, EQs), a track mixer with EQ, and master EQ and compression.
- **Save/Load** anywhere on the card, and export to WAV (tracks, mixes or mixdowns).

## Building

The OS is C, cross-compiled with [Zig](https://ziglang.org/) (`zig cc`). From this folder:

```sh
make          # bin/mpc-os and bin/mpc-os.zip, the SD-card package
make test     # the tests, on this computer
```

The manual is `docs/manual/manual.html`; `tools/build_manual.py` makes the PDF. The arpeggiator's added presets are written in `tools/arp_presets.py`, which makes `os/arp_presets.h`.

## Contact

By Lanny C (LA). Questions and bug reports: doctorbnetwork@gmail.com

Videos: [youtube.com/@DoctorbNetwork](https://www.youtube.com/@DoctorbNetwork)

## Licence

MIT: see [LICENSE](LICENSE).

## Credits

- Stem separation runs [Open-Unmix](https://github.com/sigsep/open-unmix-pytorch) UMX-HQ (MIT licence); `tools/stems` makes the model files.
- MPC, MPC Sample and the S-series are Akai Professional's. This project is not made or endorsed by Akai, and the machines named in it are named only for the features taken from them.
