#!/usr/bin/env python3
"""The arpeggiator's added presets, written as notes, made into os/arp_presets.h.

usage: python3 tools/arp_presets.py > os/arp_presets.h

Riffs: notes with octaves from C4 (the lowest pad held), "." a rest. One note a step.
Step patterns: one token a step, up to 16:
  C D# F# ...  a note, C to B (flats as Db Eb Gb Ab Bb), from the lowest pad held
  +  -         after the note: an octave up or down
  !            before the note: an accent
  ~            after the note: a slide into the next note
  _            a tie: the note before goes on
  .            a rest

seq.c includes the header twice: with ARP_PRESET_DATA for the arrays, and with ARP_PRESET_LIST
for the table's entries, which go at the end of ARPS so saved pattern numbers keep their place.
"""
import re, sys

NOTE = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8,
        "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11}

# ---- Riffs ----

def riff(text):
    out = []
    for tok in text.split():
        if tok == ".":
            out.append(-128)
            continue
        m = re.fullmatch(r"([A-G][#b]?)(-?\d)", tok)
        if not m:
            raise ValueError(f"riff note {tok!r}")
        out.append(NOTE[m[1]] + 12 * (int(m[2]) - 4))
    return out

def walking(roots, chord):
    """A walking bass, a bar to each root: root, third, fifth, then a half step into the next root."""
    out = []
    for i, r in enumerate(roots):
        nxt = roots[(i + 1) % len(roots)]
        third = 3 if chord == "m" else 4
        approach = nxt - 1 if nxt - 1 > r + 7 - 12 else nxt + 1
        out += [r, r + third, r + 7, approach]
    return out

C, F, G = 0, 5, 7
RIFFS = [
    # (name, group, rate, notes)
    # Jazz: chord tones, scales and lines from jazz theory, written for the MPC 600 S.
    ("MAJ7 ARP", "JAZZ", "REP_8", riff("C4 E4 G4 B4 C5 B4 G4 E4")),
    ("MIN9 ARP", "JAZZ", "REP_8", riff("C4 Eb4 G4 Bb4 D5 Bb4 G4 Eb4")),
    ("DOM13 SHELL", "JAZZ", "REP_8", riff("C4 E4 Bb4 D5 A5 D5 Bb4 E4")),
    ("II-V-I LINE", "JAZZ", "REP_8", riff("D4 F4 A4 C5 B4 G4 F4 D4 E4 G4 B4 D5 C5 . . .")),
    ("BEBOP DOM", "JAZZ", "REP_8", riff("C5 B4 Bb4 A4 G4 F4 E4 D4 C4 E4 G4 Bb4 D5 C5 . .")),
    ("BEBOP MAJOR", "JAZZ", "REP_8", riff("C5 B4 A4 Ab4 G4 F4 E4 D4 C4 D4 E4 G4 A4 B4 C5 .")),
    ("WALKING II-V", "JAZZ", "REP_4", riff("D3 F3 A3 C4 G3 B3 D4 F3 C3 E3 G3 B3 C3 D3 E3 Db3")),
    ("BLUES WALK", "JAZZ", "REP_4", walking([C, F, C, C, F, F, C, C, G, F, C, G], "7")),
    ("MINOR BLUES", "JAZZ", "REP_4", walking([C, F, C, C, F, F, C, C, 8, G, C, G], "m")),
    ("ALTERED", "JAZZ", "REP_8", riff("C4 Db4 Eb4 E4 F#4 G#4 Bb4 C5 Bb4 G#4 F#4 E4 Eb4 Db4 C4 .")),
    ("DIMINISHED", "JAZZ", "REP_8", riff("C4 Db4 Eb4 E4 F#4 G4 A4 Bb4 C5 Bb4 A4 G4 F#4 E4 Eb4 Db4")),
    ("WHOLE TONE", "JAZZ", "REP_8", riff("C4 D4 E4 F#4 G#4 A#4 C5 A#4 G#4 F#4 E4 D4")),
    ("QUARTAL", "JAZZ", "REP_8", riff("C4 F4 Bb4 Eb5 Ab5 Eb5 Bb4 F4")),
    ("BOSSA ARP", "JAZZ", "REP_16", riff("C4 . G4 B4 . D5 . E5 . D5 B4 . G4 . D4 .")),
    ("WALTZ ARP", "JAZZ", "REP_8", riff("C4 G4 B4 E5 B4 G4 A3 E4 G4 C5 G4 E4")),
    ("TRITONE SUB", "JAZZ", "REP_8", riff("D4 F4 A4 C5 Db4 F4 Ab4 B4 C4 E4 G4 B4 C5 . . .")),
    ("ENCLOSURES", "JAZZ", "REP_8", riff("D4 B3 C4 . F4 D#4 E4 . A4 F#4 G4 . C5 A#4 B4 .")),
    ("PENTA RUN", "JAZZ", "REP_16", riff("C4 D4 E4 G4 A4 C5 D5 E5 G5 E5 D5 C5 A4 G4 E4 D4")),
    ("3-TONIC CYCLE", "JAZZ", "REP_8", riff("C4 E4 G4 B4 Eb4 G4 Bb4 Db5 Ab3 C4 Eb4 G4 B3 D#4 F#4 A4 E4 G#4 B4 D#5")),
    # Famous: notes as recorded, one a step.
    ("PETER GUNN", "FAMOUS", "REP_8", riff("C4 C4 D4 C4 Eb4 C4 F4 Eb4")),
    ("SATISFACTION", "FAMOUS", "REP_8", riff("C4 C4 D4 Eb4 . . . . Eb4 D4 D4 C4 . . . .")),
    # Video games: cues as recorded.
    ("PAC-MAN START", "GAME", "REP_16", riff("C4 C5 G4 E4 C5 G4 E4 . Db4 Db5 Ab4 F4 Db5 Ab4 F4 . "
                                              "C4 C5 G4 E4 C5 G4 E4 . E4 F4 Gb4 . Gb4 G4 Ab4 . Ab4 A4 Bb4 . C5 . . .")),
    ("MARIO UNDERGRND", "GAME", "REP_8", riff("C4 C5 A3 A4 Bb3 Bb4 . . . . . . F3 F4 D3 D4 Eb3 Eb4 . . . . . .")),
    ("FF PRELUDE", "GAME", "REP_16", riff("C3 D3 E3 G3 C4 D4 E4 G4 C5 D5 E5 G5 C6 G5 E5 D5 C5 G4 E4 D4 C4 G3 E3 D3")),
    ("SECRET FOUND", "GAME", "REP_16", riff("C4 B3 Ab3 D3 Db3 A3 Db4 F4 . . . . . . . .")),
    ("1-UP", "GAME", "REP_16", riff("C4 Eb4 C5 Ab4 Bb4 Eb5 . .")),
]

# The Mario theme's main melody, in place of the intro the pattern played before (same number).
MARIO = riff("C4 . . G3 . . E3 . . A3 . B3 . Bb3 A3 . G3 . E4 G4 A4 . F4 G4 . E4 . C4 D4 B3 . .")

# ---- Step patterns ----

STEPS = [
    # Basslines: one bar of 1/16 from the lowest pad held, written for the MPC 600 S.
    # Pop
    ("POP EIGHTHS", "BASS", "!C _ C _ C _ C _ !A- _ A- _ G- _ G- _"),
    ("POP PULSE", "BASS", "!C C C C C C C C !G- G- G- G- A- A- A- A-"),
    ("POP OCTAVE", "BASS", "!C- . C . C- . C . !G- . G . A- . A ."),
    ("ROOT FIFTH", "BASS", "!C _ . G- !C _ . G- !F- _ . C !G- _ . D"),
    ("POP BALLAD", "BASS", "!C _ _ _ _ _ _ G- !A- _ _ _ F- _ G- _"),
    ("POP SYNCOPATE", "BASS", "!C _ . C . . !C . . C _ . !G- . A- ."),
    ("POP ANTHEM", "BASS", "!C _ C . !C _ C . !A- _ A- . !F- _ G- ."),
    ("POP WALK-UP", "BASS", "!C _ . . C . D . !E _ . . G . A ."),
    ("POP MINOR", "BASS", "!C _ . C . D# . F !G _ . F . D# D ."),
    ("POP STAB", "BASS", "!C . . . . . C . !C . . . G- . A#- ."),
    # Funk
    ("FUNK ONE", "BASS", "!C . . C+ . C . D# . F . F# !G . D# ."),
    ("FUNK OCTAVES", "BASS", "!C . C+ . C . C+ C . C+ . C !C+ . A# ."),
    ("FUNK GHOST", "BASS", "!C . C C . !C+ . C . C D# . !F . D# C"),
    ("FUNK SLIDE", "BASS", "!C . . G-~ C . A#- . !C . . C+~ A# . G ."),
    ("FUNK MINOR", "BASS", "!C _ . D# . F . F# !G . F . D# . C ."),
    ("FUNK SYNC", "BASS", "!C . . . C+ . . C . . A#- . !C . G- ."),
    ("FUNK DIRTY", "BASS", "!C . C . . !C . D# . . F . !F# . G ."),
    ("FUNK STRUT", "BASS", "!C _ . C . . !A#- . . C _ . !D# . C ."),
    ("FUNK BOUNCE", "BASS", "!C . C+ C . A#- . C !C . C+ C . G- A#- ."),
    ("FUNK TIGHT", "BASS", "!C . . C . . !C . C . . C !D# . D ."),
    # Disco
    ("DISCO OCTAVES", "BASS", "!C- . C . C- . C . C- . C . C- . C ."),
    ("DISCO WALK", "BASS", "!C- . C . D- . D . D#- . D# . E- . E ."),
    ("DISCO MINOR", "BASS", "!C- . C . C- . C . !G#- . G# . A#- . A# ."),
    ("DISCO GALLOP", "BASS", "!C- C C- . !C- C C- . !G- G G- . !A#- A# A#- ."),
    ("DISCO PUMP", "BASS", "!C- . C . . C C- . !C- . C . G- . A#- ."),
    ("DISCO RUN", "BASS", "!C- . C . C- . C . C- . C . A#- G- F- E-"),
    ("DISCO FUNKY", "BASS", "!C- . C C- . C . C- !D#- . D# D#- . D# . F"),
    ("DISCO NIGHT", "BASS", "!A- . A . A- . A . !F- . F . G- . G ."),
    ("DISCO SYNC", "BASS", "!C- . C . . C . C- !C- . C . . A# . G"),
    ("DISCO STRINGS", "BASS", "!C- . G- . C . G- . !A#- . F- . A#- . F- ."),
    # R&B
    ("R&B SMOOTH", "BASS", "!C _ _ _ . . G- _ !A#- _ _ _ . . A- _"),
    ("R&B SLIDE", "BASS", "!C _ _ . . . . D#~ !F _ _ . D#~ C _ ."),
    ("R&B 90S", "BASS", "!C _ . . . C D# . !F _ . . D# . C ."),
    ("NEO SOUL", "BASS", "!C . . . . . A#- . . C . . D# . . ."),
    ("R&B GROOVE", "BASS", "!C _ . C . . D# _ !F . . F# !G _ . ."),
    ("R&B BALLAD", "BASS", "!C _ _ _ _ _ _ _ !A- _ _ _ G- _ _ _"),
    ("R&B SKIP", "BASS", "!C . . C . . !D# . . F . . !G . . ."),
    ("R&B SUB", "BASS", "!C- _ _ _ _ _ _ _ . . . . C- _ _ ."),
    ("R&B HOOK", "BASS", "!C _ . G- . A#- . C !D# _ . C . A#- G- ."),
    ("R&B LATE", "BASS", ". . !C _ . . . C . . !D# _ . . F ."),
    # Rap
    ("BOOM BAP", "BASS", "!C _ _ . . . . . . . !C _ . . G- ."),
    ("BOOM BAP 2", "BASS", "!C _ . . . . !C . . . . . !D# _ C ."),
    ("808 GLIDE", "BASS", "!C- _ _ _ . . . . !C~ _ . . C- _ . ."),
    ("808 TRAP", "BASS", "!C- _ _ . . . C- . . . !C- _ . . . ."),
    ("808 SLIDES", "BASS", "!C- _ _ . G-~ _ C- . !C- _ . . D#~ _ C- ."),
    ("G-FUNK", "BASS", "!C _ . C . . A#- . !G- _ . G- . A#- C ."),
    ("WEST COAST", "BASS", "!C . . C . . C . !D# . . D# . . F ."),
    ("DRILL", "BASS", "!C- . . C- . . . D#-~ C- . . . !G#- . . G-"),
    ("LO-FI", "BASS", "!C _ _ . . . A#- _ !G#- _ _ . . . G- _"),
    ("OLD SCHOOL", "BASS", "!C . C . . . C . !C . C . A#- . G- ."),
    ("BOUNCE", "BASS", "!C . . C . C . . !C . . C . A#- . ."),
    ("DIRTY SOUTH", "BASS", "!C- _ _ . . . C- . !C- _ . . C . . ."),
    # More STEP patterns
    ("SLIDER", "STEP", "!C C+~ C . D#~ F . G~ !A# . G . F~ D# C ."),
    ("ROBOT", "STEP", "!C . C . !C . C+ . !C . C . !A#- . C ."),
    ("STAIRS", "STEP", "C D D# F G G# A# C+ !C+ A# G# G F D# D C"),
    ("OFF KILTER", "STEP", "!C . C+ C . D# . !C . C+ . F . D# ."),
    ("PING", "STEP", "!C+ . . C . . !C+ . . C . . !C+ . C ."),
    ("SNAKE", "STEP", "C~ C#~ C~ B-~ !C . G- . C~ C#~ D~ C# !C . . ."),
    ("SIREN", "STEP", "!C _ _ C+~ _ _ C~ _ !C _ _ G~ _ _ C~ _"),
    ("HYPNO", "STEP", "!C C C+ C D# C F C !C+ G"),
    # Added later, at the end so saved pattern numbers keep their place.
    # Hip-hop, after producers' and regions' styles: written for the MPC 600 S, not their records.
    ("DILLA FEEL 1", "BASS", "!C _ _ _ . . . C- . . !A#- _ . G- _ ."),
    ("DILLA FEEL 2", "BASS", ". !C _ . . . D# . . !F _ . . D# C ."),
    ("DILLA FEEL 3", "BASS", "!C _ . . . G- . . !A#- _ _ . A- _ G#- ."),
    ("DILLA FEEL 4", "BASS", "!C- _ _ . . C . . . !D# _ . C . A#- ."),
    ("DILLA FEEL 5", "BASS", "!F- _ _ . . . G- . !G#- _ . . A#- _ C ."),
    ("DILLA FEEL 6", "BASS", "!C _ . C- . . . A#- . . !G- _ . . F- ."),
    ("DILLA FEEL 7", "BASS", ". . !C _ _ . D# . . . !G _ . F . D#"),
    ("DILLA FEEL 8", "BASS", "!C _ . . A#- . . G- . . !F- _ _ . E-~ F-"),
    ("PETE ROCK FEEL 1", "BASS", "!C . . C . . D# . !F . . F . . G ."),
    ("PETE ROCK FEEL 2", "BASS", "!C _ . A#- . . G- . !G#- _ . G- . . F- ."),
    ("PETE ROCK FEEL 3", "BASS", "!C . D# . F . F# . !G . F . D# . C ."),
    ("PETE ROCK FEEL 4", "BASS", "!C- _ . C . . A#- . . !G- _ . A#- . C ."),
    ("PETE ROCK FEEL 5", "BASS", "!D# _ . D . . C . !A#- _ . G- . A#- . ."),
    ("PETE ROCK FEEL 6", "BASS", "!C . . . !C . A#- . . G- . . !F- . G- ."),
    ("PETE ROCK FEEL 7", "BASS", "!C _ C . D# _ . F . !G _ . D# . C ."),
    ("PETE ROCK FEEL 8", "BASS", "!A#- _ . C . D . D# . !F _ . D# . D ."),
    ("GOLDEN ERA", "BASS", "!C . . C . . . C . . !D# . . . C ."),
    ("JAZZ HOP", "BASS", "!C _ . E . . G . !A _ . G . E . D"),
    ("MEMPHIS", "BASS", "!C- . C- . . . C- . !C# . . . C- . . ."),
    ("CRUNK", "BASS", "!C- _ _ . C- _ _ . !C- _ . . D#- _ . ."),
    ("BAY SLAP", "BASS", "!C . C+ . . A#- . C !C . . D# . F . ."),
    ("NY GRIT", "BASS", "!C . . . . C . . !C# . . . C . . ."),
    ("SOUL CHOP", "BASS", "!C _ _ . D# _ . . F _ . . D# . C ."),
    ("TRAP SLIDE", "BASS", "!C- _ _ _ . . G-~ _ !C- _ . . A#-~ _ C- ."),
]

def steps(text):
    toks = text.split()
    if not 1 <= len(toks) <= 16:
        raise ValueError(f"{len(toks)} steps: {text}")
    out = []
    for tok in toks:
        if tok == ".":
            out.append((0, 0, "ARP_REST", 0, 0))
            continue
        if tok == "_":
            out.append((0, 0, "ARP_TIE", 0, 0))
            continue
        m = re.fullmatch(r"(!?)([A-G][#b]?)([+-]?)(~?)", tok)
        if not m:
            raise ValueError(f"step {tok!r}")
        out.append((NOTE[m[2]], {"": 0, "+": 1, "-": -1}[m[3]], "ARP_NOTE", int(bool(m[1])), int(bool(m[4]))))
    return out

def cname(prefix, name):
    return prefix + re.sub(r"[^A-Z0-9]", "_", name.upper())

def main():
    w = sys.stdout.write
    w("// Made by tools/arp_presets.py from its notes; edit that and run it again.\n")
    w("#ifdef ARP_PRESET_DATA\n")
    w("static const signed char RIFF_MARIO_MAIN[] = {%s};\n" % ", ".join(map(str, MARIO)))
    for name, group, rate, notes in RIFFS:
        if any(n != -128 and not -64 <= n <= 63 for n in notes):
            raise ValueError(name)
        w("static const signed char %s[] = {%s};\n" % (cname("RX_", name), ", ".join(map(str, notes))))
    for name, group, text in STEPS:
        st = steps(text)
        body = ", ".join("{%d, %d, %s, %d, %d}" % s for s in st)
        w("static const struct seq_arp_steps %s = {%d, {%s}};\n" % (cname("SX_", name), len(st), body))
    w("#endif\n#ifdef ARP_PRESET_LIST\n")
    for name, group, rate, notes in RIFFS:
        c = cname("RX_", name)
        w('    {"%s", ARP_GROUP_%s, A_RIFF, %s, %s, (int)sizeof %s, 0},\n' % (name, group, rate, c, c))
    for name, group, text in STEPS:
        w('    {"%s", ARP_GROUP_%s, A_STEPS, REP_16, 0, 0, &%s},\n' % (name, group, cname("SX_", name)))
    w("#endif\n")
    names = [r[0] for r in RIFFS] + [s[0] for s in STEPS]
    if len(set(names)) != len(names) or any(len(n) > 16 for n in names):
        raise ValueError("names: duplicate or longer than 16")

main()
