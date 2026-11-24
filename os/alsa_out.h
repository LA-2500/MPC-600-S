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
void alsa_out_usb_release(void);         // USB mode: USB audio in and out closed
void alsa_out_usb_restore(int input_src); // and opened again as set
// The codec's output level, 0-255, for the start script: down before the stock app stops, so it
// does not click as it closes. 0, or -1 when there is no such control.
int alsa_out_codec_volume(int level);
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

// USB Audio Out: the master mix (after Master FX, before the main volume) to a computer over USB,
// through the USB audio gadget. On or off; with no computer it waits and tries again.
void alsa_out_set_usb_out(int on);
// USB individual outs: the engine's eight output channels to the outs' own USB audio device
// (see usbmode_outs), while on. Starts its thread the first time.
void alsa_out_set_usb_outs(int on);
void alsa_out_set_outs_mix(int mix); // the main mix on channels 1-2 of the eight (else outputs 1-2)
void alsa_out_outs_status(char *b, size_t n); // for the log: open, frames taken, each channel's peak since last asked
