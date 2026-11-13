// Audio output on the device. Talks to the kernel's ALSA PCM interface directly,
// so the program stays one static binary with no alsa-lib.
#pragma once
#include "engine.h"
#include "recorder.h"
#include <stdatomic.h>
#include <stddef.h>

// Starts the audio thread. `main_gain` scales the main outs. Returns 0, or -1
// with the reason in `status`. On success `status` describes the opened stream.
// `rec` gets the audio input, when there is one (its `present` says so); NULL leaves it closed.
int alsa_out_start(struct engine *e, _Atomic float *main_gain, struct recorder *rec, char *status, size_t status_len);
void alsa_out_stop(void);
unsigned alsa_out_xruns(void);
int alsa_out_speaker_on(void); // 1 on, 0 off, -1 not known

// Turns the built-in speaker on, or off while headphones or the line out are plugged in.
// Safe to call often. It only touches the hardware when something has changed.
void alsa_out_speaker_update(void);

// The input: INPUT_LINE (the jack), INPUT_MIC (built in, never with the line) or INPUT_USB
// (audio from a computer over USB, through the device's USB audio gadget). 0, or -1 when USB
// audio is not there.
int alsa_out_set_input(int src);

struct master;
// Master Effects, run on the outputs after everything else (and after the resample point).
void alsa_out_set_master(struct master *m);
