// WARP (Shift + Pad 15): the selected pad's sound, its length and pitch changed apart.
//
// Warp: a phase vocoder with identity phase locking (Laroche and Dolson, 1999): each spectral
// peak's phase advances at its own measured frequency and the bins around it keep their phase
// relation to it, which keeps the sound clear instead of "phasey". On a transient (a jump in
// spectral flux) the phases start again from the input, so drums stay sharp. Left and right
// share one phase advance, from their mid, so the stereo image holds. Pitch: stretched by the
// pitch ratio too, then resampled back (cubic).
//
// S-Series Timestretch (WARP again): the Akai S1000's TIME page and its parameters: a zone of
// the sample (Start, End) stretched by a Time Factor of 25% to 2000%, by one of two modes.
// CYCLIC: a fixed cycle length (in samples) repeated or dropped, butted together: best on a
// single-pitched sound with the cycle a multiple of its period (<AutoC> finds it).
// INTELL: the splice points chosen by matching the sound either side of each (a waveform
// similarity search), QUAL setting how many samples go in or out at a time (higher: smaller,
// more often) and WIDTH the crossfade across each splice. For speech, music, drum loops.
//
// Each change renders again in the background and plays when ready. Only one warped sound is
// kept: <To Pad> puts it on the pad in place of the original, which stays in memory.
#include "lcd.h"
#include "os.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WN 2048 // the analysis window
#define HS 512  // the synthesis hop: 75% overlap
#define WARP_PAD (-5) // the engine pad previews play on
#define PI_D 3.14159265358979323846

// ---- The phase vocoder ----

struct pv {
    float win[WN];
    float cs[WN / 2], sn[WN / 2]; // twiddles
    int rev[WN];
};

static void pv_init(struct pv *p) {
    for (int i = 0; i < WN; i++) p->win[i] = (float)(0.5 - 0.5 * cos(2 * PI_D * i / WN));
    for (int i = 0; i < WN / 2; i++) p->cs[i] = (float)cos(2 * PI_D * i / WN), p->sn[i] = (float)sin(2 * PI_D * i / WN);
    for (int i = 0; i < WN; i++) {
        int r = 0;
        for (int b = 1, x = i; b < WN; b <<= 1, x >>= 1) r = (r << 1) | (x & 1);
        p->rev[i] = r;
    }
}

static void fft(const struct pv *p, float *re, float *im, int inverse) {
    for (int i = 0; i < WN; i++)
        if (p->rev[i] > i) {
            float t = re[i]; re[i] = re[p->rev[i]]; re[p->rev[i]] = t;
            t = im[i]; im[i] = im[p->rev[i]]; im[p->rev[i]] = t;
        }
    for (int len = 2; len <= WN; len <<= 1) {
        int step = WN / len;
        for (int i = 0; i < WN; i += len)
            for (int j = 0; j < len / 2; j++) {
                float wr = p->cs[j * step], wi = inverse ? p->sn[j * step] : -p->sn[j * step];
                float *ar = &re[i + j], *ai = &im[i + j], *br = &re[i + j + len / 2], *bi = &im[i + j + len / 2];
                float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                *br = *ar - tr, *bi = *ai - ti;
                *ar += tr, *ai += ti;
            }
    }
}

static inline double wrap(double a) {
    return a - 2 * PI_D * floor((a + PI_D) / (2 * PI_D));
}

// in: frames x ch (1 or 2), interleaved. Returns out_frames x ch, stretched by alpha (> 1 longer).
static float *pv_stretch(const float *in, int ch, long frames, double alpha, long *out_frames, atomic_int *cancel) {
    static struct pv P; // tables only, filled once
    static int ready;
    if (!ready) pv_init(&P), ready = 1;
    long out_n = (long)(frames * alpha + 0.5);
    if (out_n < 1) out_n = 1;
    float *out = calloc((size_t)(out_n + WN) * ch, sizeof(float)), *norm = calloc((size_t)(out_n + WN), sizeof(float));
    float *re[2], *im[2];
    double *phi_prev = calloc(WN / 2 + 1, sizeof(double)), *psi = calloc(WN / 2 + 1, sizeof(double));
    float *mag = calloc(WN / 2 + 1, sizeof(float)), *mag_prev = calloc(WN / 2 + 1, sizeof(float));
    double *phi = calloc(WN / 2 + 1, sizeof(double)), *psi_new = calloc(WN / 2 + 1, sizeof(double));
    for (int c = 0; c < 2; c++) re[c] = calloc(WN, sizeof(float)), im[c] = calloc(WN, sizeof(float));
    if (!out || !norm || !phi_prev || !psi || !mag || !mag_prev || !phi || !psi_new || !re[0] || !re[1] || !im[0] || !im[1]) {
        free(out), out = 0;
        goto done;
    }
    double ha = HS / alpha;
    long pa_prev = 0;
    for (long m = 0;; m++) {
        if (atomic_load_explicit(cancel, memory_order_relaxed)) {
            free(out), out = 0;
            goto done;
        }
        long ps = m * HS; // the frame's centre in the output
        if (ps - WN / 2 >= out_n) break;
        long pa = (long)llround(m * ha); // and in the input
        long hop = m ? pa - pa_prev : 0;
        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < WN; i++) {
                long at = pa - WN / 2 + i;
                re[c][i] = at >= 0 && at < frames ? in[at * ch + c] * P.win[i] : 0;
                im[c][i] = 0;
            }
            fft(&P, re[c], im[c], 0);
        }
        // The mid's magnitude and phase lead; a transient starts the phases again.
        double flux = 0, total = 0;
        for (int k = 0; k <= WN / 2; k++) {
            double mr = 0, mi = 0;
            for (int c = 0; c < ch; c++) mr += re[c][k], mi += im[c][k];
            mag[k] = (float)hypot(mr, mi);
            phi[k] = atan2(mi, mr);
            double up = mag[k] - mag_prev[k];
            if (up > 0) flux += up;
            total += mag[k];
        }
        int reset = m == 0 || hop <= 0 || (m > 1 && total > 1e-6 && flux / total > 0.5);
        if (reset) {
            for (int k = 0; k <= WN / 2; k++) psi_new[k] = phi[k];
        } else {
            // Peaks: bigger than two bins either side, each advanced at its own frequency.
            int peaks[WN / 2], np = 0;
            for (int k = 2; k <= WN / 2 - 2; k++)
                if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1] && mag[k] > mag[k - 2] && mag[k] >= mag[k + 2]) {
                    double w = 2 * PI_D * k / WN, dev = wrap(phi[k] - phi_prev[k] - w * hop);
                    psi_new[k] = wrap(psi[k] + (w + dev / hop) * HS);
                    peaks[np++] = k;
                }
            // Every other bin keeps its phase relation to its nearest peak (identity locking).
            for (int k = 0, at = 0; k <= WN / 2; k++) {
                if (np == 0) { // no peaks at all: each bin on its own
                    double w = 2 * PI_D * k / WN, dev = wrap(phi[k] - phi_prev[k] - w * hop);
                    psi_new[k] = wrap(psi[k] + (w + dev / hop) * HS);
                    continue;
                }
                while (at + 1 < np && abs(peaks[at + 1] - k) < abs(peaks[at] - k)) at++;
                if (peaks[at] != k) psi_new[k] = wrap(psi_new[peaks[at]] + (phi[k] - phi[peaks[at]]));
            }
        }
        // Each channel turned by the mid's change of phase, back to time, windowed, added.
        for (int c = 0; c < ch; c++) {
            for (int k = 0; k <= WN / 2; k++) {
                double rot = psi_new[k] - phi[k], cr = cos(rot), sr = sin(rot);
                double r = re[c][k] * cr - im[c][k] * sr, i = re[c][k] * sr + im[c][k] * cr;
                re[c][k] = (float)r, im[c][k] = (float)i;
                if (k > 0 && k < WN / 2) re[c][WN - k] = (float)r, im[c][WN - k] = (float)-i;
            }
            fft(&P, re[c], im[c], 1);
            for (int i = 0; i < WN; i++) {
                long at = ps - WN / 2 + i;
                if (at < 0 || at >= out_n) continue;
                out[at * ch + c] += re[c][i] / WN * P.win[i];
                if (c == 0) norm[at] += P.win[i] * P.win[i];
            }
        }
        for (int k = 0; k <= WN / 2; k++) phi_prev[k] = phi[k], psi[k] = psi_new[k], mag_prev[k] = mag[k];
        pa_prev = pa;
    }
    for (long i = 0; i < out_n; i++) {
        float n = norm[i] > 0.1f ? norm[i] : 0.1f;
        for (int c = 0; c < ch; c++) out[i * ch + c] /= n;
    }
    *out_frames = out_n;
done:
    free(norm), free(phi_prev), free(psi), free(mag), free(mag_prev), free(phi), free(psi_new);
    for (int c = 0; c < 2; c++) free(re[c]), free(im[c]);
    return out;
}

// Cubic (Catmull-Rom) read of a channel at a fractional frame.
static float cubic(const float *x, int ch, int c, long n, double at) {
    long i = (long)floor(at);
    float t = (float)(at - i), p[4];
    for (int k = 0; k < 4; k++) {
        long j = i - 1 + k;
        p[k] = j >= 0 && j < n ? x[j * ch + c] : 0;
    }
    return p[1] + 0.5f * t * (p[2] - p[0] + t * (2 * p[0] - 5 * p[1] + 4 * p[2] - p[3] + t * (3 * (p[1] - p[2]) + p[3] - p[0])));
}

// ---- The S-Series cyclic timestretch ----

static float *ts_cyclic(const float *in, int ch, long frames, double alpha, int cycle, long *out_frames) {
    long out_n = (long)(frames * alpha + 0.5);
    if (out_n < 1) out_n = 1;
    if (cycle < 2) cycle = 2;
    float *out = calloc((size_t)(out_n + cycle) * ch, sizeof(float));
    if (!out) return 0;
    // Whole cycles of the input, repeated (longer) or dropped (shorter): each splice falls on a
    // cycle boundary, so with the cycle a whole number of pitch periods the joins line up.
    for (long o = 0; o < out_n; o += cycle) {
        long src = (long)((double)(o / cycle) / alpha) * cycle;
        for (int j = 0; j < cycle && o + j < out_n; j++) {
            long at = src + j < frames ? src + j : frames - 1;
            for (int c = 0; c < ch; c++) out[(o + j) * ch + c] = at >= 0 ? in[at * ch + c] : 0;
        }
    }
    *out_frames = out_n;
    return out;
}

static float *ts_intell(const float *in, int ch, long frames, double alpha, int rate, int qual, int width, long *out_frames,
                        atomic_int *cancel) {
    long out_n = (long)(frames * alpha + 0.5);
    if (out_n < 1) out_n = 1;
    int L = (int)(rate * (0.060 - 0.045 * qual / 99.0)); // the segment: 60 ms at QUAL 0 down to 15 ms at 99
    if (L < 64) L = 64;
    int O = (int)(L * (0.05 + 0.45 * width / 99.0)); // the crossfade across each splice
    if (O < 4) O = 4;
    int hop = L - O, S = (int)(L * (0.15 + 0.35 * qual / 99.0)); // how far either side to search
    float *out = calloc((size_t)(out_n + L) * ch, sizeof(float)), *mono = malloc((size_t)(frames > 0 ? frames : 1) * sizeof(float));
    if (!out || !mono) {
        free(out), free(mono);
        return 0;
    }
    for (long i = 0; i < frames; i++) {
        float m = 0;
        for (int c = 0; c < ch; c++) m += in[i * ch + c];
        mono[i] = m;
    }
    long prev = 0;
    for (long o = 0, k = 0; o < out_n; o += hop, k++) {
        if ((k & 63) == 0 && atomic_load_explicit(cancel, memory_order_relaxed)) {
            free(out), free(mono);
            return 0;
        }
        long nominal = (long)(o / alpha), best = nominal;
        if (k > 0) { // where the input best continues what the last segment's tail was playing
            long natural = prev + hop;
            double top = -1e30;
            for (long c = nominal - S; c <= nominal + S; c += 2) {
                if (c < 0 || c + O >= frames || natural + O >= frames) continue;
                double dot = 0, e = 1e-9;
                for (int j = 0; j < O; j += 2) dot += (double)mono[c + j] * mono[natural + j], e += (double)mono[c + j] * mono[c + j];
                double score = dot / sqrt(e);
                if (score > top) top = score, best = c;
            }
        }
        if (best < 0) best = 0;
        for (int j = 0; j < L && o + j < out_n; j++) {
            float w = 1;
            if (k > 0 && j < O) w = (float)j / O;                   // in over the last one's tail
            if (j >= hop && o + hop < out_n) w = (float)(L - j) / O; // out under the next
            long at = best + j;
            for (int c = 0; c < ch; c++) out[(o + j) * ch + c] += (at < frames ? in[at * ch + c] : 0) * w;
        }
        prev = best;
    }
    free(mono);
    *out_frames = out_n;
    return out;
}

// <AutoC>: a cycle length for CYCLIC: the zone's pitch period (autocorrelation), taken whole
// times to about 25 ms.
int ts_auto_cycle(const struct sample *x, long start, long end) {
    if (!x) return 1000;
    int rate = x->rate, ch = x->channels;
    long n = end - start < rate ? end - start : rate;
    int lo = rate / 1000, hi = rate / 40; // 1 kHz down to 40 Hz
    if (n < hi * 2) return rate / 40;
    double best = 0;
    int period = 0;
    for (int lag = lo; lag <= hi; lag++) {
        double dot = 0, e0 = 1e-9, e1 = 1e-9;
        for (long i = 0; i + lag < n; i += 2) {
            float a = x->data[(start + i) * ch], b = x->data[(start + i + lag) * ch];
            dot += a * b, e0 += a * a, e1 += b * b;
        }
        double r = dot / sqrt(e0 * e1);
        if (r > best + 0.02) best = r, period = lag; // the first strong one: the period, not a multiple
    }
    int target = rate / 40;
    if (period < 1 || best < 0.5) return target;
    int times = (target + period / 2) / period;
    return period * (times < 1 ? 1 : times);
}

// ---- The background render ----

struct warp_job {
    pthread_t thread;
    atomic_int cancel, done;
    float *in;
    long frames;
    int ch, rate, kind;
    double alpha, pitch;
    int ts_mode, ts_cycle, ts_qual, ts_width; // S-Series
    long ts_start, ts_end;                    // the zone stretched; the rest kept as it was
    struct sample *result;
};

static void *warp_run(void *arg) {
    struct warp_job *j = arg;
    long n = 0;
    float *y = 0;
    if (j->kind == 0) {
        float *z = pv_stretch(j->in, j->ch, j->frames, j->alpha * j->pitch, &n, &j->cancel);
        if (z && j->pitch != 1.0) { // back to the length asked for: read faster or slower (the pitch)
            long m = (long)(j->frames * j->alpha + 0.5);
            y = malloc((size_t)m * j->ch * sizeof(float));
            if (y)
                for (long i = 0; i < m; i++)
                    for (int c = 0; c < j->ch; c++) y[i * j->ch + c] = cubic(z, j->ch, c, n, i * j->pitch);
            free(z);
            n = m;
        } else {
            y = z;
        }
    } else { // the zone stretched, what is before and after it kept as it was
        long a = j->ts_start, b = j->ts_end, zn = 0;
        float *z = j->ts_mode == 0 ? ts_cyclic(j->in + a * j->ch, j->ch, b - a, j->alpha, j->ts_cycle, &zn)
                                   : ts_intell(j->in + a * j->ch, j->ch, b - a, j->alpha, j->rate, j->ts_qual, j->ts_width, &zn, &j->cancel);
        if (z) {
            n = a + zn + (j->frames - b);
            y = malloc((size_t)n * j->ch * sizeof(float));
            if (y) {
                memcpy(y, j->in, (size_t)a * j->ch * sizeof(float));
                memcpy(y + a * j->ch, z, (size_t)zn * j->ch * sizeof(float));
                memcpy(y + (a + zn) * j->ch, j->in + b * j->ch, (size_t)(j->frames - b) * j->ch * sizeof(float));
            }
            free(z);
        }
    }
    if (y && !atomic_load(&j->cancel)) {
        struct sample *s = sample_new((int32_t)n, j->ch, j->rate);
        if (s) {
            memcpy(s->data, y, (size_t)n * j->ch * sizeof(float));
            for (long i = 0; i < n * j->ch; i++) // nothing past full scale
                s->data[i] = s->data[i] > 1 ? 1 : s->data[i] < -1 ? -1 : s->data[i];
        }
        j->result = s;
    }
    free(y);
    atomic_store(&j->done, 1);
    return 0;
}

static double pitch_ratio(const struct os *s) {
    return pow(2.0, (s->warp_pitch + s->warp_fine / 100.0) / 12.0);
}

// The pad's trim, copied for the job to work on (so the pad can change under it).
static struct warp_job *start_job(struct os *s) {
    const struct os_pad *pad = &s->pads[s->selected];
    const struct sample *src = pad->s;
    if (!src) return 0;
    long st = pad->p.start, en = pad->p.end;
    if (en > src->frames || en <= 0) en = src->frames;
    if (st < 0 || st >= en) st = 0;
    if (s->warp_page == 1) st = 0, en = src->frames; // S-Series: the whole sample; its zone is stretched
    struct warp_job *j = calloc(1, sizeof *j);
    if (!j) return 0;
    j->frames = en - st, j->ch = src->channels, j->rate = src->rate, j->kind = s->warp_page;
    j->in = malloc((size_t)j->frames * j->ch * sizeof(float));
    if (!j->in) {
        free(j);
        return 0;
    }
    memcpy(j->in, src->data + st * src->channels, (size_t)j->frames * j->ch * sizeof(float));
    j->alpha = (s->warp_page == 1 ? s->ts_factor : s->warp_pct) / 100.0;
    j->pitch = pitch_ratio(s);
    j->ts_mode = s->ts_mode, j->ts_cycle = s->ts_cycle, j->ts_qual = s->ts_qual, j->ts_width = s->ts_width;
    j->ts_start = s->ts_start < 0 ? 0 : s->ts_start > j->frames ? j->frames : s->ts_start;
    j->ts_end = s->ts_end <= j->ts_start || s->ts_end > j->frames ? j->frames : s->ts_end;
    if (pthread_create(&j->thread, 0, warp_run, j) != 0) {
        free(j->in);
        free(j);
        return 0;
    }
    return j;
}

static void finish_job(struct warp_job *j) {
    pthread_join(j->thread, 0);
    free(j->in);
    free(j);
}

// ---- The page ----

enum { W_LEN, W_PITCH, W_FINE, W_BARS, W_FIELDS };
#define WAVE_TOP 48
#define WAVE_H 72

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static double sample_seconds(const struct os *s) {
    const struct os_pad *pad = &s->pads[s->selected];
    if (!pad->s) return 0;
    long st = pad->p.start, en = pad->p.end;
    if (en > pad->s->frames || en <= 0) en = pad->s->frames;
    if (st < 0 || st >= en) st = 0;
    return (double)(en - st) / pad->s->rate;
}

// A guess at the loop's bars: the count of four-beat bars that puts it between 80 and 160 BPM.
static int guess_bars(double seconds) {
    for (int b = 1; b <= 16; b *= 2)
        if (b * 4 * 60.0 / seconds >= 80) return b;
    return 16;
}

static double seq_bpm(const struct os *s) {
    return s->seq.sequence[s->seq.current].tempo10 / 10.0;
}

void warp_open(struct os *s) {
    if (s->screen == OS_WARP) { // pressed again: the other page
        s->warp_page = !s->warp_page;
        s->warp_cursor = s->warp_page ? 2 : 0; // S-Series: Time Factor first
        s->warp_dirty = 1;
        return;
    }
    s->warp_return = s->screen;
    s->screen = OS_WARP;
    s->warp_page = 0;
    s->warp_cursor = 0;
    s->warp_pad = -1; // a new look at the pad
    for (int i = 0; i < 3; i++) s->knob_caught[i] = 0;
}

static void play_result(struct os *s) {
    if (!s->warp_result) return;
    struct pad_params p = s->pads[s->selected].p;
    p.start = 0, p.end = s->warp_result->frames, p.loop = 0, p.loop_on = 0, p.reverse = 0, p.gate = 0;
    p.mono = 1, p.mute_group = 0, p.also_pad = 0, p.soft_start_ms = 0, p.synth = 0, p.layers = 1, p.semi = 0, p.fine = 0;
    engine_note_on(s->engine, WARP_PAD, 127, s->warp_result, &p);
}

// The warped sound onto the pad, in place of the original, which stays in memory.
static void to_pad(struct os *s, struct ui *u) {
    int slot = s->selected;
    struct os_pad *pad = &s->pads[slot];
    struct sample *old = pad->s, *n = s->warp_result;
    if (!n || !old) {
        os_toast(s, u, n ? "Pad is empty" : "Still warping");
        return;
    }
    int kept = 0;
    for (int i = 0; i < s->nmemory; i++) kept |= s->memory[i].s == old;
    if (!kept && os_memory_add(s, old, pad->name) != 0) {
        os_toast(s, u, "Memory full");
        return;
    }
    os_undo_save(s, "Warp");
    engine_kill_all(s->engine); // the preview stops; the sound now belongs to the pad
    struct pad_params keep = pad->p;
    char name[40];
    snprintf(name, sizeof name, "%.16s-%s", pad->name, s->warp_page ? "TS" : "W");
    os_pad_assign(s, slot, n, name);
    pad->p = keep;
    pad->p.start = 0, pad->p.end = n->frames, pad->p.loop = 0;
    os_pad_push(s, slot);
    s->warp_result = 0; // the pad has it now: one warped sound at a time
    s->warp_pad = slot, s->warp_pct = 100, s->warp_pitch = s->warp_fine = 0; // the new sound, as it is
    os_toast(s, u, "Done");
}

static void knob(struct os *s, const struct input *in, int k, int *v, int lo, int hi, int log_scale) {
    if (!in->knob_moved[k]) return;
    int at = log_scale ? (int)(log((double)*v / lo) / log((double)hi / lo) * 127 + 0.5) : (*v - lo) * 127 / (hi - lo);
    int last = s->knob_last[k], now = in->knob[k];
    s->knob_last[k] = now;
    if (!s->knob_caught[k]) {
        int crossed = last >= 0 && ((last <= at && now >= at) || (last >= at && now <= at));
        if (crossed || abs(now - at) <= 2) s->knob_caught[k] = 1;
    }
    if (!s->knob_caught[k]) return;
    int val = log_scale ? (int)(lo * pow((double)hi / lo, now / 127.0) + 0.5) : lo + (hi - lo) * now / 127;
    if (val != *v) *v = val, s->warp_dirty = 1;
    s->warp_cursor = k;
}

static void draw_wave(struct ui *u, const struct sample *x) {
    if (!x || x->frames < 1) return;
    int mid = WAVE_TOP + WAVE_H / 2;
    for (int col = 0; col < UI_W - 8; col++) {
        long a = (long)col * x->frames / (UI_W - 8), b = (long)(col + 1) * x->frames / (UI_W - 8);
        float lo = 0, hi = 0;
        for (long i = a; i < b || i == a; i++) {
            if (i >= x->frames) break;
            float v = x->data[i * x->channels];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        int y0 = mid - (int)(hi * WAVE_H / 2), y1 = mid - (int)(lo * WAVE_H / 2);
        lcd_fill(u, 4 + col, y0, 1, y1 - y0 + 1, LCD_FG);
    }
}

// ---- The S-Series Timestretch page: the S1000's TIME parameters, no waveform ----

enum { TS_START, TS_END, TS_FACTOR, TS_MODE, TS_A, TS_B, TS_FIELDS }; // A: Cycle Len or Qual; B: Width (INTELL)

static long pad_frames(const struct os *s) {
    const struct os_pad *pad = &s->pads[s->selected];
    return pad->s ? pad->s->frames : 0;
}

static void ts_frame(struct os *s, struct ui *u, const struct input *in) {
    struct os_pad *pad = &s->pads[s->selected];
    long frames = pad_frames(s);
    int intell = s->ts_mode == 1, fields = intell ? TS_FIELDS : TS_B;
    if (input_down(in, IN_B2) && !intell && pad->s) { // <AutoC>: the cycle from the zone's pitch
        s->ts_cycle = ts_auto_cycle(pad->s, s->ts_start, s->ts_end > s->ts_start ? s->ts_end : frames);
        s->warp_dirty = 1;
    }
    if (s->warp_cursor >= fields) s->warp_cursor = 0;
    if (in->encoder_press) s->warp_cursor = (s->warp_cursor + (in->shift ? fields - 1 : 1)) % fields;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d) {
        long step = in->shift || frames < 500 ? 1 : frames / 500; // Shift: a sample at a time
        switch (s->warp_cursor) {
        case TS_START: s->ts_start = s->ts_start + d * step < 0 ? 0 : s->ts_start + d * step >= s->ts_end ? s->ts_end - 1 : s->ts_start + d * step; break;
        case TS_END: s->ts_end = s->ts_end + d * step > frames ? frames : s->ts_end + d * step <= s->ts_start ? s->ts_start + 1 : s->ts_end + d * step; break;
        case TS_FACTOR: s->ts_factor = clampi(s->ts_factor + d * (in->shift ? 10 : 1), 25, 2000); break;
        case TS_MODE: s->ts_mode = d > 0; break;
        case TS_A:
            if (intell) s->ts_qual = clampi(s->ts_qual + d, 0, 99);
            else s->ts_cycle = clampi(s->ts_cycle + d * (in->shift ? 10 : 1), 16, 16384);
            break;
        case TS_B: s->ts_width = clampi(s->ts_width + d, 0, 99); break;
        }
        s->warp_dirty = 1;
    }
    knob(s, in, 0, &s->ts_factor, 25, 2000, 1);
    if (intell) {
        knob(s, in, 1, &s->ts_qual, 0, 99, 0);
        knob(s, in, 2, &s->ts_width, 0, 99, 0);
    } else {
        knob(s, in, 1, &s->ts_cycle, 16, 16384, 1);
    }
    for (int k = 0; k < 3; k++)
        if (in->knob_moved[k] && (k == 0 || intell || k == 1)) s->warp_cursor = k == 0 ? TS_FACTOR : k == 1 ? TS_A : TS_B;

    char line[48], v[24];
    lcd_text(u, 0, KEY_ROW, "<Play>");
    if (!intell) lcd_text(u, 8, KEY_ROW, "<AutoC>");
    lcd_text(u, 20, KEY_ROW, "<GO>");
    lcd_rule(u, TITLE_ROW, "S-Series Timestretch");
    if (!pad->s) {
        lcd_center(u, 6, "Press a pad with a sound");
        lcd_rule(u, BOTTOM_ROW, 0);
        return;
    }
    snprintf(line, sizeof line, "Sample: %c%02d %.13s", 'A' + s->selected / 16, s->selected % 16 + 1, pad->name);
    lcd_text(u, 0, 3, line);
    struct {
        int f, row, col;
        const char *label;
        char val[20];
    } r[TS_FIELDS] = {
        {TS_START, 4, 7, "Start:", ""}, {TS_END, 5, 7, "End:", ""}, {TS_FACTOR, 6, 13, "Time Factor:", ""},
        {TS_MODE, 9, 6, "Mode:", ""}, {TS_A, 10, 12, intell ? "Qual:" : "Cycle Len:", ""}, {TS_B, 11, 12, "Width:", ""},
    };
    snprintf(r[TS_START].val, 20, "%07ld", s->ts_start);
    snprintf(r[TS_END].val, 20, "%07ld", s->ts_end);
    snprintf(r[TS_FACTOR].val, 20, "%d%%", s->ts_factor);
    snprintf(r[TS_MODE].val, 20, "%s", intell ? "INTELL" : "CYCLIC");
    if (intell) snprintf(r[TS_A].val, 20, "%02d", s->ts_qual);
    else snprintf(r[TS_A].val, 20, "%d", s->ts_cycle);
    snprintf(r[TS_B].val, 20, "%02d", s->ts_width);
    for (int i = 0; i < fields; i++) {
        lcd_text(u, 0, r[i].row, r[i].label);
        snprintf(v, sizeof v, " %s ", r[i].val);
        if (s->warp_cursor == i) lcd_inverse(u, r[i].col, r[i].row, v);
        else lcd_text(u, r[i].col, r[i].row, v);
    }
    if (!intell) lcd_text(u, 19, 10, "smpls");
    long zone = s->ts_end - s->ts_start, out = (long)(zone * (s->ts_factor / 100.0) + 0.5) + (frames - zone);
    snprintf(line, sizeof line, "New: %ld pts %.2fs", out, (double)out / pad->s->rate);
    lcd_text(u, 0, 7, line);
    lcd_text(u, 0, 13, s->warp_job || s->warp_dirty ? "Stretching..." : intell ? "K1 Factor K2 Qual K3 Width" : "K1 Factor  K2 Cycle Len");
    lcd_rule(u, BOTTOM_ROW, 0);
    if (s->warp_job || s->warp_dirty) ui_wake(u, 30);
}

void warp_frame(struct os *s, struct ui *u, const struct input *in) {
    struct os_pad *pad = &s->pads[s->selected];
    if (s->warp_pad != s->selected) { // another pad: its own length, its bars guessed, its zone all of it
        s->warp_pad = s->selected;
        s->warp_pct = 100, s->warp_pitch = 0, s->warp_fine = 0;
        s->warp_bars = pad->s ? guess_bars(sample_seconds(s)) : 1;
        s->ts_start = 0, s->ts_end = pad_frames(s);
        s->ts_cycle = pad->s ? ts_auto_cycle(pad->s, 0, s->ts_end) : 1000;
        s->warp_dirty = 1;
    }
    // A finished render: kept and played, unless it was overtaken.
    if (s->warp_job && atomic_load(&s->warp_job->done)) {
        struct warp_job *j = s->warp_job;
        s->warp_job = 0;
        if (!atomic_load(&j->cancel) && j->result) {
            if (s->warp_result) engine_retire(s->engine, s->warp_result);
            s->warp_result = j->result;
            play_result(s);
        } else if (j->result) {
            sample_free(j->result);
        }
        finish_job(j);
    }
    if (input_down(in, IN_B1)) play_result(s); // <Play>
    if (input_down(in, IN_B3)) to_pad(s, u);   // <To Pad>, <GO>
    if (s->warp_page == 1) {
        ts_frame(s, u, in);
    } else {
        if (input_down(in, IN_B2)) { // <Seq BPM>: the loop's length made to fit the sequence's tempo
            double secs = sample_seconds(s), own = secs > 0 ? s->warp_bars * 4 * 60.0 / secs : 0;
            if (own > 0) s->warp_pct = clampi((int)lround(own / seq_bpm(s) * 100), 25, 400), s->warp_dirty = 1;
        }
        if (in->encoder_press) s->warp_cursor = (s->warp_cursor + (in->shift ? W_FIELDS - 1 : 1)) % W_FIELDS;
        int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
        if (d) {
            switch (s->warp_cursor) {
            case W_LEN: s->warp_pct = clampi(s->warp_pct + d, 25, 400); break;
            case W_PITCH: s->warp_pitch = clampi(s->warp_pitch + d, -12, 12); break;
            case W_FINE: s->warp_fine = clampi(s->warp_fine + d, -100, 100); break;
            case W_BARS: s->warp_bars = clampi(s->warp_bars + d, 1, 64); break;
            }
            s->warp_dirty = 1;
        }
        knob(s, in, 0, &s->warp_pct, 25, 400, 1);
        knob(s, in, 1, &s->warp_pitch, -12, 12, 0);
        knob(s, in, 2, &s->warp_fine, -100, 100, 0);
    }
    // A change: the render under way is dropped, a new one starts.
    if (s->warp_dirty && pad->s) {
        if (s->warp_job) atomic_store(&s->warp_job->cancel, 1);
        else {
            s->warp_job = start_job(s);
            s->warp_dirty = 0;
        }
    }
    if (s->warp_page == 1) return;

    // ---- Drawing: Warp ----
    char line[48], v[24];
    lcd_text(u, 0, KEY_ROW, "<Play>");
    lcd_text(u, 8, KEY_ROW, "<Seq BPM>");
    lcd_text(u, 18, KEY_ROW, "<To Pad>");
    lcd_rule(u, TITLE_ROW, "Warp");
    if (!pad->s) {
        lcd_center(u, 6, "Press a pad with a sound");
        lcd_rule(u, BOTTOM_ROW, 0);
        return;
    }
    draw_wave(u, s->warp_result ? s->warp_result : pad->s);
    snprintf(line, sizeof line, "Pad: %c%02d %.14s", 'A' + s->selected / 16, s->selected % 16 + 1, pad->name);
    lcd_text(u, 0, 8, line);
    struct {
        const char *label;
        char val[16];
    } f[W_FIELDS];
    f[W_LEN].label = "K1 Length:";
    snprintf(f[W_LEN].val, 16, "%d%%", s->warp_pct);
    f[W_PITCH].label = "K2 Pitch:";
    snprintf(f[W_PITCH].val, 16, "%+d", s->warp_pitch);
    f[W_FINE].label = "K3 Fine:";
    snprintf(f[W_FINE].val, 16, "%+d", s->warp_fine);
    f[W_BARS].label = "Bars:";
    snprintf(f[W_BARS].val, 16, "%d", s->warp_bars);
    for (int i = 0; i < W_FIELDS; i++) {
        lcd_text(u, 0, 9 + i, f[i].label);
        snprintf(v, sizeof v, " %s ", f[i].val);
        if (s->warp_cursor == i) lcd_inverse(u, 12, 9 + i, v);
        else lcd_text(u, 12, 9 + i, v);
    }
    double secs = sample_seconds(s), own = secs > 0 ? s->warp_bars * 4 * 60.0 / secs : 0;
    snprintf(line, sizeof line, "Loop %.1f>%.1f Seq %.1f", own, own * 100.0 / s->warp_pct, seq_bpm(s));
    lcd_text(u, 0, 13, line);
    if (s->warp_job || s->warp_dirty) lcd_inverse(u, 19, 8, "Warping");
    lcd_rule(u, BOTTOM_ROW, 0);
    if (s->warp_job || s->warp_dirty) ui_wake(u, 30);
}

// Leaving the page (or a test): any render stopped, the warped sound let go.
void warp_close(struct os *s) {
    if (s->warp_job) {
        atomic_store(&s->warp_job->cancel, 1);
        while (!atomic_load(&s->warp_job->done)) {
            struct timespec t = {0, 1000000};
            nanosleep(&t, 0);
        }
        if (s->warp_job->result) sample_free(s->warp_job->result);
        finish_job(s->warp_job);
        s->warp_job = 0;
    }
    if (s->warp_result) engine_retire(s->engine, s->warp_result), s->warp_result = 0;
}

// Tests: whether a render is under way.
int warp_busy(const struct os *s) {
    return s->warp_job != 0 || s->warp_dirty;
}
