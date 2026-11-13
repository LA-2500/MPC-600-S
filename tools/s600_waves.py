#!/usr/bin/env python3
"""Make os/s600_waves.h, the S600 waveforms built into the OS, from single-cycle WAV files.

usage: python3 tools/s600_waves.py FOLDER > os/s600_waves.h

FOLDER holds SINE.WAV, SAW.WAV, SQUARE.WAV and PULSE.WAV: one cycle each, 16-bit mono at
44.1 kHz (674 points: C2). At start-up the OS writes them to the card as MPC600S/s600/*.wav.
"""
import os, struct, sys, wave

NAMES = ["SINE", "SAW", "SQUARE", "PULSE"]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    waves = []
    for name in NAMES:
        w = wave.open(os.path.join(sys.argv[1], name + ".WAV"))
        if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != 44100:
            sys.exit(f"{name}.WAV: not 16-bit mono at 44.1 kHz")
        n = w.getnframes()
        waves.append(struct.unpack("<%dh" % n, w.readframes(n)))
    n = len(waves[0])
    if any(len(x) != n for x in waves):
        sys.exit("the waves differ in length")
    out = sys.stdout.write
    out("// The S600 waveforms: one cycle each (%d points at 44.1 kHz: C2). Made by tools/s600_waves.py.\n" % n)
    out("#pragma once\n#define S600_WAVES %d\n#define S600_WAVE_POINTS %d\n" % (len(NAMES), n))
    out("static const char *const S600_WAVE_NAMES[S600_WAVES] = {%s};\n" % ", ".join('"%s"' % x for x in NAMES))
    out("static const short S600_WAVE_DATA[S600_WAVES][S600_WAVE_POINTS] = {\n")
    for x in waves:
        out("    {" + ", ".join(map(str, x)) + "},\n")
    out("};\n")


main()
