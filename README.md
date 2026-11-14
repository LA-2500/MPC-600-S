# MPC 600 S

Sampler and sequencer OS for the **Akai MPC Sample**, based on the design of the MPC 3000 operating system: page layout, sequencer structure, timing correct and swing. Runs from the SD card via the stock autorun mechanism; the internal firmware is not modified. Remove the card to return to the stock OS.

## Download

| | |
|---|---|
| **[⬇ OS v1.2 (MPC-600-S-OS-v1.2.zip)](../../releases/latest/download/MPC-600-S-OS-v1.2.zip)** | SD-card package |
| **[⬇ Quick Start (PDF)](../../releases/latest/download/MPC-600-S-Quick-Start.pdf)** | Installation, 3 pages |
| **[⬇ Operator's Manual v1.2 (PDF)](../../releases/latest/download/MPC-600-S-Manual-v1.2.pdf)** | Full reference |
| [⬇ Stems models (107 MB, optional)](../../releases/latest/download/MPC600S-stems.zip) | Required only by the Stems page |

Links resolve to the latest release. Earlier versions: [Releases](../../releases).

## Install

1. Extract **MPC-600-S-OS-v1.2.zip**. Copy the `autorun` folder to the SD card root (FAT32 or exFAT). Do not rename or edit its files: the stock runner verifies `autorun.check` (SHA-256) before execution.
2. Power on with the card inserted. The stock OS boots; the MPC 600 S stops it and takes over the display and panel within 45 s of power-on.
3. Stock OS: power off, remove the card. Update: replace the `autorun` folder. User data in `MPC600S/` is not touched.

Help for the current page: **SHIFT + SAMPLE REC**. Procedure and troubleshooting: [Quick Start](../../releases/latest/download/MPC-600-S-Quick-Start.pdf).

## Specifications

| | |
|---|---|
| Sequencer | 99 sequences × 99 tracks, 96 PPQN; songs; Step Edit; Note Repeat; arpeggiator with step patterns and pattern editor |
| Timing correct | MPC 3000, MPC60, MPC 2000 and SP12 swing |
| Metronome | Click during audio takes and after Live Looper takes selectable (default off) |
| Sampling | 7–44.1 kHz, 16/12/8/4-bit, anti-alias filtered; sources: line in, INST (left channel, mono, 40 Hz HPF, 0–30 dB gain), mic, USB audio (UAC2) |
| Programs | Drum programs; S600 programs taken from Akai's S-series samplers: envelopes, filters (incl. ladder low-pass), LFO, layers |
| Live Looper | One-pass takes dropped onto pads while the sequence keeps looping; layers without stopping; delete last take; conversion during the pass (IMMEDIATE) or after the first loop (AFTER 1ST LOOP), bit-identical either way; MIDI footswitch start/stop (AUDIO REC) |
| Footswitch | Any MIDI channel and CC, learned from one press; polarity worked out automatically (AUTO), or set |
| Recording defaults | Global start values on Additional Settings 2: sample rate (29.76 kHz; slider rates or SET to any), bit rate (12), anti-aliasing, normalize, Input Thru, mode |
| Editing | Trim, loop, zones, warp, S-series timestretch, print effects; audio tracks; resampling |
| Stem separation | Open-Unmix UMX-HQ on device: drums, bass, vocals, music; up to 20 s per sound |
| Effects | Input, track send and pad effects (reverb, delay, modulation, drive, amp and cabinet, EQ); track mixer with EQ; master EQ and compression |
| Storage | Save/Load to any folder on the card; WAV export of tracks, mixes and mixdowns |
| Display / audio | 320 × 240 DRM framebuffer; 44.1 kHz |
| Build | C; static aarch64 (musl) via `zig cc` |

## Building

Requires [Zig](https://ziglang.org/) and Python 3.

```sh
make          # bin/mpc-os and bin/mpc-os.zip (SD-card package)
make test     # host test suite
```

Manual source: `docs/manual/manual.html`; PDF via `tools/build_manual.py`. Arpeggiator presets: `tools/arp_presets.py` → `os/arp_presets.h`. Autorun packer: `tools/make_autorun.py`.

## Contact

Lanny C (LA). Questions and bug reports: doctorbnetwork@gmail.com

Video: [youtube.com/@DoctorbNetwork](https://www.youtube.com/@DoctorbNetwork)

## Licence

MIT: see [LICENSE](LICENSE).

## Credits

- Stem separation: [Open-Unmix](https://github.com/sigsep/open-unmix-pytorch) UMX-HQ (MIT). `tools/stems` generates the model files.
- MPC, MPC Sample and the S-series are trademarks of Akai Professional. This project is not affiliated with or endorsed by Akai. Machines are named only to identify the features taken from them.
