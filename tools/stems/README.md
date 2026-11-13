# Stem models

The Stems page (Shift + SAMPLE REC twice) runs Open-Unmix UMX-HQ (MIT), one model per stem, in
`os/stems/umx.c`. The models are not in the repository; they are made here and copied to the
card's `MPC600S/stems/` folder once.

1. A Python 3.12 environment with CPU PyTorch and `openunmix` (this repo used `uv`).
2. `python export_umx.py` downloads UMX-HQ and writes `umxhq-{drums,bass,vocals,other}.bin`
   (flat float32, batch norms folded), plus `stemtest.wav` and PyTorch reference stems.
3. `build/stems/stemtest DIR` (see `os/stems/stemtest.c`) separates the clip in C, and
   `python compare.py` (in DIR) checks it against the references: about 72 dB SNR, the
   limit of 16-bit WAV.

Measured on the MPC Sample: about 5.3 s per stem for a 15 s clip, 105 MB of memory at most.
