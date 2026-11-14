// Sampling from the audio input. The audio thread hands each block of input to
// recorder_input: it feeds the meters, waits for the threshold and records. The UI thread
// arms it, keeps memory ready ahead of it, and turns a finished take into a sound.
//
// Input is always captured at 44.1 kHz. A take is made into a sound at the chosen sample
// rate by taking its value at the exact instants a sampler running at that rate would
// (the 44.1 kHz capture holds the whole audible input, so the value between captured
// samples is known). With Anti-Aliasing off nothing is filtered first, so it aliases just
// as an unfiltered sampler does; on, a steep low-pass at the new rate's limit comes first.
#pragma once
#include "engine.h"
#include <stdatomic.h>
#include <stdint.h>

struct fx_chain;

#define REC_CHUNK 44100          // frames per block of memory: one second
#define REC_MAX_CHUNKS 7200      // two hours, far beyond what memory allows
#define REC_PREROLL 441          // 10 ms kept from before the threshold, so the attack is whole
#define REC_AHEAD 4              // seconds of memory kept ready ahead of the recording

enum { REC_IDLE, REC_ARMED, REC_RECORDING, REC_DONE };
enum { REC_STEREO, REC_MONO_L, REC_MONO_R, REC_MONO_LR, REC_MODES };

struct recorder {
    atomic_int present;   // an input is open
    atomic_int state;     // REC_*
    atomic_int thru;      // Input Thru: the input plays out of the outputs
    atomic_int threshold; // 0..32767 on the 16-bit scale; 0 starts at once
    atomic_int stop;      // the UI asks the take to end
    atomic_int source;    // 0 the input, 1 the main output (Resample): after pad and knob FX, before master FX
    struct fx_chain *fx;  // Input Effects: the input goes through them before it is heard or recorded
    atomic_int thru_resample; // Resample's Input Thru: 0 MUTE while resampling, 1 RECORD (heard and taken)
    int16_t *chunk[REC_MAX_CHUNKS]; // allocated by the UI thread, filled by the audio thread
    atomic_int ready;     // chunks allocated
    atomic_long frames;   // frames recorded
    long max_frames;      // where the take stops on its own (memory)
    _Atomic float peak_l, peak_r; // the loudest input since the UI last looked
    long long first_clock; // the engine clock frame of the take's first frame
    int latency;           // frames from a sound leaving the outputs to its return at the input
    int16_t pre[REC_PREROLL * 2];
    int pre_at, pre_full;
};

// Audio thread: one block of interleaved stereo input, -1..1, read at engine clock `clock`.
void recorder_input(struct recorder *r, const float *in, int frames, long long clock);

// UI thread.
int recorder_arm(struct recorder *r, long max_frames); // 0 when armed
void recorder_service(struct recorder *r); // keeps memory ready ahead. Call every frame.
void recorder_stop(struct recorder *r);    // armed: back to idle. Recording: ends the take.
void recorder_discard(struct recorder *r); // frees a finished take
void recorder_frame(const struct recorder *r, long i, float *l, float *rr); // a recorded frame
float recorder_peak(struct recorder *r, int right); // and resets it

struct rec_format {
    int mode;      // REC_*
    int rate;      // Hz, 7000..44100
    int bits;      // 16, 12, 8 or 4
    int aa;        // anti-aliasing filter
    int normalize;
    int pitch;     // 0..24 semitones: the take is sped up this much before it is sampled
    long skip, length; // only the take's frames [skip, skip + length); length 0 runs to the end
};

// Makes the finished take into a sound and frees the take. NULL when memory runs out.
struct sample *recorder_take(struct recorder *r, const struct rec_format *f);

// Convert Sample: a sound sampled again at the format's rate, pitch and bits, as if it were
// played into the input (its own rate's audio, sampled at the new rate's instants).
struct sample *rec_convert(const struct sample *src, const struct rec_format *f);
// The same conversion as recorder_take, done while the take is still coming in (Live Looper:
// IMMEDIATE): rec_stream_run converts whatever the frames so far allow, and once all `n`
// frames from `skip` are in, rec_stream_finish normalizes and sets the bits, which is quick.
// The result is exactly recorder_take's. Run and finish on one thread; the recorder may record on.
struct rec_stream;
struct rec_stream *rec_stream_new(const struct rec_format *f, long skip, long n);
int rec_stream_run(struct rec_stream *st, const struct recorder *r); // 1 once every output is made
struct sample *rec_stream_finish(struct rec_stream *st);              // frees st
void rec_stream_free(struct rec_stream *st);
// The take's frames as they are (44.1 kHz, 16 bits, no pitch), in the format's Mode (Live
// Looper: AFTER 1ST LOOP plays this for one pass). The recorder must be done.
struct sample *recorder_raw(const struct recorder *r, int mode, long skip, long n);

// The sound resampled to 44.1 kHz just as the engine plays it, for files other gear can read.
struct sample *rec_convert_44k(const struct sample *s);
