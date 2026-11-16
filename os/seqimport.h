// Sequences from other machines: a Standard MIDI File (.mid), or an MPC 1000 / 2500 / 500 .ALL
// file (all its sequences and songs). Read into a list of sequences and their tracks, and then
// the tracks chosen go onto the first empty tracks of a sequence.
#ifndef SEQIMPORT_H
#define SEQIMPORT_H
#include <stdint.h>

#define IMP_TRACKS 64
enum { IMP_MIDI, IMP_MPC };

struct imp_note {
    uint32_t tick; // at 96 PPQN
    uint16_t dur;
    uint8_t note, vel; // the note number, as the file has it
};

struct imp_track {
    char name[17];
    int src;     // the file's own track number, for their order
    char prog[17]; // an MPC track's program, by name
    int midi;    // 1: a melodic part (a MIDI track on the MPC, or a MIDI file part off channel 10)
    int channel; // 1..16, 0 not known
    int count, cap;
    struct imp_note *notes;
    int has_map;              // a drum part's notes to pads: from the MPC's program file beside it
    unsigned char map[128];   // note -> pad (0..63), 255 none
};

struct imp_seq {
    char name[17];
    int tempo10, bars, sig_num, sig_den;
    int ntracks;
    struct imp_track track[IMP_TRACKS];
};

struct seq_import {
    int kind, nseq;
    struct imp_seq *seq;
    int pick;                     // the sequence chosen
    unsigned char sel[IMP_TRACKS]; // its tracks chosen
    int cursor;
    char file[40];
    uint32_t fresh; // programs just loaded with the project: a track's program is looked for there first
};

int imp_midi_load(struct seq_import *im, const char *path); // 0, or -1 not a MIDI file / nothing in it
int imp_mpc_load(struct seq_import *im, const char *path);  // 0, or -1 not an MPC .ALL or .SEQ file / nothing in it
void imp_free(struct seq_import *im);
void imp_select_all(struct seq_import *im, int on);

struct os;
// The chosen tracks of the chosen sequence into sequence `to`, onto its first empty tracks, each
// playing the program of its own program's name if one is loaded (an MPC program), else the one in
// use. An
// empty sequence also takes the file's tempo, time signature and length; otherwise it is made long
// enough. Returns the tracks that went in (0: none were free); *first the first of them, *dropped
// drum notes with no pad to go to.
int imp_apply(struct os *s, struct seq_import *im, int to, int *first, int *dropped);
#endif
