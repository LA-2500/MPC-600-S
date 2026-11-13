#include "recorder.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int16_t to16(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return (int16_t)lrintf(x * 32767.0f);
}

static void put(struct recorder *r, long at, int16_t l, int16_t rr) {
    int16_t *c = r->chunk[at / REC_CHUNK];
    long i = at % REC_CHUNK;
    c[i * 2] = l;
    c[i * 2 + 1] = rr;
}

void recorder_input(struct recorder *r, const float *in, int frames, long long clock) {
    float pl = atomic_load_explicit(&r->peak_l, memory_order_relaxed);
    float pr = atomic_load_explicit(&r->peak_r, memory_order_relaxed);
    for (int i = 0; i < frames; i++) {
        float l = fabsf(in[i * 2]), rr = fabsf(in[i * 2 + 1]);
        if (l > pl) pl = l;
        if (rr > pr) pr = rr;
    }
    atomic_store_explicit(&r->peak_l, pl, memory_order_relaxed);
    atomic_store_explicit(&r->peak_r, pr, memory_order_relaxed);

    int state = atomic_load_explicit(&r->state, memory_order_acquire);
    if (state == REC_ARMED) {
        if (atomic_load_explicit(&r->stop, memory_order_relaxed)) {
            atomic_store_explicit(&r->state, REC_IDLE, memory_order_release);
            return;
        }
        int threshold = atomic_load_explicit(&r->threshold, memory_order_relaxed), start = -1;
        for (int i = 0; i < frames && start < 0; i++) {
            int16_t l = to16(in[i * 2]), rr = to16(in[i * 2 + 1]);
            if (abs(l) >= threshold || abs(rr) >= threshold) start = i;
            else { // kept for the pre-roll
                r->pre[r->pre_at * 2] = l;
                r->pre[r->pre_at * 2 + 1] = rr;
                r->pre_at = (r->pre_at + 1) % REC_PREROLL;
                if (r->pre_at == 0) r->pre_full = 1;
            }
        }
        if (start < 0 || atomic_load_explicit(&r->ready, memory_order_acquire) < 1) return;
        long at = 0; // the moments before the threshold, so the attack is whole
        int have = r->pre_full ? REC_PREROLL : r->pre_at;
        for (int k = 0; k < have; k++) {
            int j = (r->pre_at - have + k + REC_PREROLL) % REC_PREROLL;
            put(r, at++, r->pre[j * 2], r->pre[j * 2 + 1]);
        }
        r->first_clock = clock + start - have;
        atomic_store_explicit(&r->frames, at, memory_order_release);
        atomic_store_explicit(&r->state, REC_RECORDING, memory_order_release);
        in += start * 2;
        frames -= start;
        state = REC_RECORDING;
    }
    if (state != REC_RECORDING) return;
    long at = atomic_load_explicit(&r->frames, memory_order_relaxed);
    long ready = (long)atomic_load_explicit(&r->ready, memory_order_acquire) * REC_CHUNK;
    int done = atomic_load_explicit(&r->stop, memory_order_relaxed);
    for (int i = 0; i < frames && !done; i++) {
        if (at >= ready || at >= r->max_frames) { // out of memory made ready: the take ends here
            done = 1;
            break;
        }
        put(r, at++, to16(in[i * 2]), to16(in[i * 2 + 1]));
    }
    atomic_store_explicit(&r->frames, at, memory_order_release);
    if (done) atomic_store_explicit(&r->state, REC_DONE, memory_order_release);
}

static void free_chunks(struct recorder *r) {
    int n = atomic_load(&r->ready);
    for (int i = 0; i < n; i++) {
        free(r->chunk[i]);
        r->chunk[i] = 0;
    }
    atomic_store(&r->ready, 0);
}

void recorder_service(struct recorder *r) {
    int state = atomic_load_explicit(&r->state, memory_order_acquire);
    if (state != REC_ARMED && state != REC_RECORDING) return;
    long need = atomic_load_explicit(&r->frames, memory_order_acquire) / REC_CHUNK + REC_AHEAD;
    long most = (r->max_frames + REC_CHUNK - 1) / REC_CHUNK;
    if (need > most) need = most;
    if (need > REC_MAX_CHUNKS) need = REC_MAX_CHUNKS;
    int n = atomic_load_explicit(&r->ready, memory_order_relaxed);
    while (n < need) {
        int16_t *c = malloc((size_t)REC_CHUNK * 2 * sizeof *c);
        if (!c) break;
        r->chunk[n++] = c;
        atomic_store_explicit(&r->ready, n, memory_order_release);
    }
}

int recorder_arm(struct recorder *r, long max_frames) {
    int state = atomic_load(&r->state);
    if (state == REC_ARMED || state == REC_RECORDING) return -1;
    free_chunks(r);
    r->max_frames = max_frames > REC_PREROLL ? max_frames : REC_PREROLL;
    r->pre_at = r->pre_full = 0;
    atomic_store(&r->frames, 0);
    atomic_store(&r->stop, 0);
    atomic_store(&r->state, REC_ARMED);
    recorder_service(r); // memory for the first seconds before anything can arrive
    return 0;
}

void recorder_stop(struct recorder *r) {
    atomic_store(&r->stop, 1);
}

void recorder_discard(struct recorder *r) {
    if (atomic_load(&r->state) != REC_DONE) return;
    free_chunks(r);
    atomic_store(&r->frames, 0);
    atomic_store(&r->state, REC_IDLE);
}

void recorder_frame(const struct recorder *r, long i, float *l, float *rr) {
    const int16_t *c = r->chunk[i / REC_CHUNK];
    long k = i % REC_CHUNK;
    *l = c[k * 2] / 32767.0f;
    *rr = c[k * 2 + 1] / 32767.0f;
}

float recorder_peak(struct recorder *r, int right) {
    return atomic_exchange_explicit(right ? &r->peak_r : &r->peak_l, 0.0f, memory_order_relaxed);
}

// A windowed-sinc kernel, tabulated finely, for reading a signal between its samples.
// fc is the cutoff in cycles per input sample.
#define PHASES 256
struct kernel {
    float *table; // taps * PHASES + 1 values over [-half, half]
    int half;
};

static int kernel_make(struct kernel *k, double fc) {
    k->half = (int)ceil(4.0 / fc);
    if (k->half < 4) k->half = 4;
    int n = 2 * k->half * PHASES + 1;
    k->table = malloc((size_t)n * sizeof *k->table);
    if (!k->table) return -1;
    for (int i = 0; i < n; i++) {
        double t = (double)(i - k->half * PHASES) / PHASES; // in input samples
        double x = 2.0 * fc * t, sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
        double w = (t + k->half) / (2.0 * k->half); // Blackman window over the span
        double win = 0.42 - 0.5 * cos(2 * M_PI * w) + 0.08 * cos(4 * M_PI * w);
        k->table[i] = (float)(2.0 * fc * sinc * win);
    }
    return 0;
}

static float kernel_at(const struct kernel *k, double t) {
    double p = (t + k->half) * PHASES;
    if (p < 0 || p >= 2.0 * k->half * PHASES) return 0;
    int i = (int)p;
    float f = (float)(p - i);
    return k->table[i] + f * (k->table[i + 1] - k->table[i]);
}

// The signal's value at input time `t` (in samples), from its samples around it.
static float value_at(const float *x, long n, const struct kernel *k, double t) {
    long first = (long)floor(t) - k->half + 1, last = (long)floor(t) + k->half;
    if (first < 0) first = 0;
    if (last >= n) last = n - 1;
    float sum = 0, wsum = 0;
    for (long i = first; i <= last; i++) {
        float w = kernel_at(k, (double)i - t);
        sum += w * x[i];
        wsum += w;
    }
    return wsum > 1e-6f ? sum / wsum : 0;
}

static struct sample *make_sound(float *x[2], long n, int in_rate, int ch, const struct rec_format *f);

struct sample *recorder_take(struct recorder *r, const struct rec_format *f) {
    if (atomic_load(&r->state) != REC_DONE) return 0;
    long total = atomic_load(&r->frames), skip = f->skip < 0 ? 0 : f->skip > total ? total : f->skip;
    long n = f->length > 0 && f->length < total - skip ? f->length : total - skip;
    int stereo = f->mode == REC_STEREO, ch = stereo ? 2 : 1;
    // The take as one or two plain signals at 44.1 kHz, then its memory goes.
    float *x[2] = {malloc((size_t)(n > 0 ? n : 1) * sizeof(float)), stereo ? malloc((size_t)(n > 0 ? n : 1) * sizeof(float)) : 0};
    if (!x[0] || (stereo && !x[1])) {
        free(x[0]);
        free(x[1]);
        return 0;
    }
    for (long i = 0; i < n; i++) {
        float l, rr;
        recorder_frame(r, skip + i, &l, &rr);
        if (stereo) x[0][i] = l, x[1][i] = rr;
        else x[0][i] = f->mode == REC_MONO_L ? l : f->mode == REC_MONO_R ? rr : 0.5f * (l + rr);
    }
    recorder_discard(r);
    return make_sound(x, n, ENGINE_RATE, ch, f);
}

// One or two signals at in_rate made into a sound: pitched, sampled at the format's rate,
// normalized and cut to its bits. Frees x.
static struct sample *make_sound(float *x[2], long n, int in_rate, int ch, const struct rec_format *f) {
    // Pitch speeds the signal up; the rate sets how often it is sampled. step: input samples
    // between the new sound's samples.
    int rate = f->rate < 4000 ? 4000 : f->rate > ENGINE_RATE ? ENGINE_RATE : f->rate;
    double speed = pow(2.0, (f->pitch < 0 ? 0 : f->pitch > 24 ? 24 : f->pitch) / 12.0);
    double step = speed * in_rate / rate;
    long out = (long)floor((n - 1) / step) + 1;
    if (n < 1) out = 1;
    struct sample *s = sample_new((int32_t)out, ch, rate);
    struct kernel k = {0};
    // Anti-aliasing on: nothing above the new sound's limit gets through. Off: only what the
    // signal already holds, so the rest folds back as aliasing.
    double fc = f->aa ? 0.45 / step : 0.45;
    if (fc > 0.45) fc = 0.45;
    if (!s || (step != 1.0 && kernel_make(&k, fc) != 0)) {
        sample_free(s);
        free(x[0]);
        free(x[1]);
        return 0;
    }
    for (int c = 0; c < ch; c++)
        for (long i = 0; i < out; i++)
            s->data[i * ch + c] = step == 1.0 ? x[c][i] : value_at(x[c], n, &k, i * step);
    free(k.table);
    free(x[0]);
    free(x[1]);

    float top = (float)((1 << (f->bits - 1)) - 1) / (float)(1 << (f->bits - 1));
    if (f->normalize) {
        float peak = 0;
        for (long i = 0; i < out * ch; i++) peak = fabsf(s->data[i]) > peak ? fabsf(s->data[i]) : peak;
        if (peak > 0)
            for (long i = 0; i < out * ch; i++) s->data[i] *= top / peak;
    }
    // The bit rate: each sample rounded to the steps the converter had. No dither, as then.
    float q = (float)(1 << (f->bits - 1));
    for (long i = 0; i < out * ch; i++) {
        float v = roundf(s->data[i] * q) / q;
        s->data[i] = v > top ? top : v < -1.0f ? -1.0f : v;
    }
    return s;
}

struct sample *rec_convert(const struct sample *src, const struct rec_format *f) {
    if (!src || src->frames < 1) return 0;
    long n = src->frames;
    int stereo = f->mode == REC_STEREO && src->channels == 2, ch = stereo ? 2 : 1;
    float *x[2] = {malloc((size_t)n * sizeof(float)), stereo ? malloc((size_t)n * sizeof(float)) : 0};
    if (!x[0] || (stereo && !x[1])) {
        free(x[0]);
        free(x[1]);
        return 0;
    }
    for (long i = 0; i < n; i++) {
        const float *fr = src->data + i * src->channels;
        float l = fr[0], rr = fr[src->channels - 1];
        if (stereo) x[0][i] = l, x[1][i] = rr;
        else x[0][i] = src->channels == 1 || f->mode == REC_MONO_L ? l : f->mode == REC_MONO_R ? rr : 0.5f * (l + rr);
    }
    return make_sound(x, n, src->rate, ch, f);
}

struct sample *rec_convert_44k(const struct sample *s) {
    if (!s || s->rate <= 0) return 0;
    // Each sample held until the next one's time, as the engine plays a low-rate sound.
    long out = (long)((double)s->frames * ENGINE_RATE / s->rate);
    if (out < 1) out = 1;
    struct sample *n = sample_new((int32_t)out, s->channels, ENGINE_RATE);
    if (!n) return 0;
    for (long i = 0; i < out; i++) {
        long j = (long)((double)i * s->rate / ENGINE_RATE);
        if (j >= s->frames) j = s->frames - 1;
        for (int c = 0; c < s->channels; c++) n->data[i * s->channels + c] = s->data[j * s->channels + c];
    }
    return n;
}
