#include "umx.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NFFT 4096
#define HOP 1024
#define NBINS (NFFT / 2 + 1) // 2049
#define CH 2

struct umx_model {
    float *blob; // the whole file; everything below points into it
    int nb_bins, hidden, out_bins;
    const float *input_mean, *input_scale;
    const float *fc1, *bn1a, *bn1c;
    const float *wih[3][2], *whh[3][2], *bias[3][2];
    const float *fc2, *bn2a, *bn2c;
    const float *fc3, *bn3a, *bn3c;
    const float *output_scale, *output_mean;
};

struct umx_model *umx_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4];
    int dims[3];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "UMX1", 4) || fread(dims, sizeof(int), 3, f) != 3) {
        fclose(f);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f) - 16;
    fseek(f, 16, SEEK_SET);
    struct umx_model *m = calloc(1, sizeof *m);
    if (!m || !(m->blob = malloc((size_t)bytes)) || fread(m->blob, 1, (size_t)bytes, f) != (size_t)bytes) {
        fclose(f);
        if (m) free(m->blob);
        free(m);
        return 0;
    }
    fclose(f);
    m->nb_bins = dims[0], m->hidden = dims[1], m->out_bins = dims[2];
    int B = m->nb_bins, H = m->hidden, O = m->out_bins, L = H / 2; // L: one LSTM direction
    const float *p = m->blob;
#define TAKE(dst, count) (dst = p, p += (count))
    TAKE(m->input_mean, B), TAKE(m->input_scale, B);
    TAKE(m->fc1, (long)H * CH * B), TAKE(m->bn1a, H), TAKE(m->bn1c, H);
    for (int l = 0; l < 3; l++)
        for (int d = 0; d < 2; d++) TAKE(m->wih[l][d], 4L * L * H), TAKE(m->whh[l][d], 4L * L * L), TAKE(m->bias[l][d], 4 * L);
    TAKE(m->fc2, (long)H * 2 * H), TAKE(m->bn2a, H), TAKE(m->bn2c, H);
    TAKE(m->fc3, (long)CH * O * H), TAKE(m->bn3a, CH * O), TAKE(m->bn3c, CH * O);
    TAKE(m->output_scale, O), TAKE(m->output_mean, O);
#undef TAKE
    if ((const char *)p - (const char *)m->blob != bytes) { // the sizes do not add up: not this model
        umx_free(m);
        return 0;
    }
    return m;
}

void umx_free(struct umx_model *m) {
    if (!m) return;
    free(m->blob);
    free(m);
}

// Tiny "denormal" numbers are very slow on both ARM and x86; they make no audible difference,
// so each thread doing the work treats them as zero.
static void flush_denormals(void) {
#if defined(__aarch64__)
    unsigned long fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr | (1ul << 24)));
#elif defined(__x86_64__)
    unsigned csr;
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    csr |= 0x8040; // flush to zero, denormals are zero
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
#endif
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

// ---- A little parallel for: rows [0, n) split over the threads ----

struct job {
    void (*fn)(void *ctx, int from, int to);
    void *ctx;
    int from, to;
};

static void *run_job(void *a) {
    struct job *j = a;
    flush_denormals();
    j->fn(j->ctx, j->from, j->to);
    return 0;
}

static void parallel(int threads, int n, void (*fn)(void *, int, int), void *ctx) {
    if (threads > 16) threads = 16;
    if (threads <= 1 || n < threads) {
        fn(ctx, 0, n);
        return;
    }
    pthread_t th[16];
    struct job jobs[16];
    for (int i = 0; i < threads; i++) {
        jobs[i] = (struct job){fn, ctx, n * i / threads, n * (i + 1) / threads};
        if (i && pthread_create(&th[i], 0, run_job, &jobs[i])) jobs[i].fn(ctx, jobs[i].from, jobs[i].to), th[i] = 0;
    }
    run_job(&jobs[0]);
    for (int i = 1; i < threads; i++)
        if (th[i]) pthread_join(th[i], 0);
}

// ---- Dense layers: Y[t][o] = sum_i W[o][i] * X[t][i], 4 rows by 4 frames at a time ----

typedef float v4 __attribute__((vector_size(16)));

static inline v4 load4(const float *p) {
    v4 v;
    memcpy(&v, p, sizeof v);
    return v;
}

struct mm {
    const float *w, *x;
    float *y;
    int rows, in, frames, ystride, xstride;
};

static void mm_part(void *c, int from, int to) { // from/to: blocks of 4 rows
    const struct mm *m = c;
    int I = m->in, I4 = I & ~3;
    for (int ob = from; ob < to; ob++) {
        int o0 = ob * 4, on = m->rows - o0 < 4 ? m->rows - o0 : 4;
        const float *w[4];
        for (int k = 0; k < 4; k++) w[k] = m->w + (long)(o0 + (k < on ? k : 0)) * I;
        for (int t0 = 0; t0 < m->frames; t0 += 4) {
            int tn = m->frames - t0 < 4 ? m->frames - t0 : 4;
            const float *x[4];
            for (int k = 0; k < 4; k++) x[k] = m->x + (long)(t0 + (k < tn ? k : 0)) * m->xstride;
            v4 acc[4][4] = {{{0}}};
            for (int i = 0; i < I4; i += 4) {
                v4 wv[4] = {load4(w[0] + i), load4(w[1] + i), load4(w[2] + i), load4(w[3] + i)};
                for (int b = 0; b < 4; b++) {
                    v4 xv = load4(x[b] + i);
                    for (int a = 0; a < 4; a++) acc[a][b] += wv[a] * xv;
                }
            }
            for (int a = 0; a < on; a++)
                for (int b = 0; b < tn; b++) {
                    float s = acc[a][b][0] + acc[a][b][1] + acc[a][b][2] + acc[a][b][3];
                    for (int i = I4; i < I; i++) s += w[a][i] * x[b][i];
                    m->y[(long)(t0 + b) * m->ystride + o0 + a] = s;
                }
        }
    }
}

static void dense(const float *w, int rows, int in, const float *x, int xstride, int frames, float *y, int ystride,
                  int threads) {
    struct mm m = {w, x, y, rows, in, frames, ystride, xstride};
    parallel(threads, (rows + 3) / 4, mm_part, &m);
}

// ---- LSTM: one direction of one layer, over all frames ----

struct lstm_dir {
    const float *whh;
    const float *g;  // the input projections, [T][4L], bias included
    float *out;      // [T][2L]: this direction writes its half
    int T, L, reverse;
};

static inline float sigm(float x) {
    return 1.0f / (1.0f + expf(-x));
}

static void *lstm_run(void *a) {
    struct lstm_dir *d = a;
    flush_denormals();
    int L = d->L;
    float *h = calloc((size_t)L, sizeof *h), *c = calloc((size_t)L, sizeof *c), *z = malloc(4 * (size_t)L * sizeof *z);
    if (!h || !c || !z) {
        free(h), free(c), free(z);
        return 0;
    }
    for (int s = 0; s < d->T; s++) {
        int t = d->reverse ? d->T - 1 - s : s;
        const float *g = d->g + (long)t * 4 * L;
        for (int r = 0; r < 4 * L; r++) { // z = g + Whh h
            const float *w = d->whh + (long)r * L;
            v4 acc = {0, 0, 0, 0};
            for (int i = 0; i < L; i += 4) acc += load4(w + i) * load4(h + i);
            z[r] = g[r] + acc[0] + acc[1] + acc[2] + acc[3];
        }
        float *o = d->out + (long)t * 2 * L + (d->reverse ? L : 0);
        for (int k = 0; k < L; k++) { // PyTorch's gate order: input, forget, cell, output
            float ig = sigm(z[k]), fg = sigm(z[L + k]), cg = tanhf(z[2 * L + k]), og = sigm(z[3 * L + k]);
            c[k] = fg * c[k] + ig * cg;
            h[k] = og * tanhf(c[k]);
            o[k] = h[k];
        }
    }
    free(h), free(c), free(z);
    return 0;
}

// ---- FFT: radix 2, in place, 4096 points ----

struct fft {
    float cs[NFFT / 2], sn[NFFT / 2];
    int rev[NFFT];
    float win[NFFT];
};

static void fft_init(struct fft *f) {
    for (int i = 0; i < NFFT / 2; i++) f->cs[i] = (float)cos(2 * M_PI * i / NFFT), f->sn[i] = (float)sin(2 * M_PI * i / NFFT);
    for (int i = 0; i < NFFT; i++) {
        int r = 0;
        for (int b = 1, k = i; b < NFFT; b <<= 1, k >>= 1) r = (r << 1) | (k & 1);
        f->rev[i] = r;
        f->win[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / NFFT)); // periodic Hann, as torch.hann_window
    }
}

static void fft_run(const struct fft *f, float *re, float *im, int inverse) {
    for (int i = 0; i < NFFT; i++)
        if (i < f->rev[i]) {
            float t = re[i]; re[i] = re[f->rev[i]]; re[f->rev[i]] = t;
            t = im[i]; im[i] = im[f->rev[i]]; im[f->rev[i]] = t;
        }
    for (int len = 2; len <= NFFT; len <<= 1) {
        int half = len / 2, step = NFFT / len;
        for (int i = 0; i < NFFT; i += len)
            for (int k = 0; k < half; k++) {
                float wr = f->cs[k * step], wi = inverse ? f->sn[k * step] : -f->sn[k * step];
                int a = i + k, b = a + half;
                float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr, im[b] = im[a] - xi;
                re[a] += xr, im[a] += xi;
            }
    }
}

// A sample of channel c at padded position j: centered frames, reflected at both ends.
static float padded(const float *in, long n, int c, long j) {
    long i = j - NFFT / 2;
    if (i < 0) i = -i;
    if (i >= n) i = 2 * (n - 1) - i;
    if (i < 0) i = 0;
    return in[i * CH + c];
}

int umx_separate(const struct umx_model *m, const float *in, long n, float *out, int remove, int threads,
                 struct umx_times *times) {
    int B = m->nb_bins, H = m->hidden, O = m->out_bins, L = H / 2;
    int T = (int)(1 + n / HOP);
    struct umx_times tm = {0};
    double t0 = now_s();
    struct fft *f = malloc(sizeof *f);
    float *Xre = malloc(sizeof(float) * CH * (size_t)T * NBINS), *Xim = malloc(sizeof(float) * CH * (size_t)T * NBINS);
    float *xin = malloc(sizeof(float) * (size_t)T * CH * B), *h1 = malloc(sizeof(float) * (size_t)T * 2 * H);
    float *g = malloc(sizeof(float) * (size_t)T * 4 * L * 2), *la = malloc(sizeof(float) * (size_t)T * H);
    float *lb = malloc(sizeof(float) * (size_t)T * H), *h2 = malloc(sizeof(float) * (size_t)T * H);
    float *y = malloc(sizeof(float) * (size_t)T * CH * O), *acc = calloc((size_t)(n + NFFT) * CH, sizeof(float));
    float *env = calloc((size_t)(n + NFFT), sizeof(float));
    int ok = f && Xre && Xim && xin && h1 && g && la && lb && h2 && y && acc && env;
    if (!ok) goto done;
    fft_init(f);

    // The mix's spectrum, both channels: the model's input, and the phase the stem keeps.
    float re[NFFT], im[NFFT];
    for (int c = 0; c < CH; c++)
        for (int t = 0; t < T; t++) {
            for (int k = 0; k < NFFT; k++) re[k] = padded(in, n, c, (long)t * HOP + k) * f->win[k], im[k] = 0;
            fft_run(f, re, im, 0);
            float *xr = Xre + ((long)c * T + t) * NBINS, *xi = Xim + ((long)c * T + t) * NBINS;
            memcpy(xr, re, NBINS * sizeof(float));
            memcpy(xi, im, NBINS * sizeof(float));
        }
    for (int t = 0; t < T; t++)
        for (int c = 0; c < CH; c++)
            for (int b = 0; b < B; b++) {
                long at = ((long)c * T + t) * NBINS + b;
                float mag = sqrtf(Xre[at] * Xre[at] + Xim[at] * Xim[at]);
                xin[((long)t * CH + c) * B + b] = (mag + m->input_mean[b]) * m->input_scale[b];
            }
    tm.stft = now_s() - t0;

    // fc1, bn1, tanh: into the first half of h1, whose second half the LSTM fills.
    t0 = now_s();
    dense(m->fc1, H, CH * B, xin, CH * B, T, h1, 2 * H, threads);
    for (int t = 0; t < T; t++)
        for (int k = 0; k < H; k++) {
            float *v = &h1[(long)t * 2 * H + k];
            *v = tanhf(*v * m->bn1a[k] + m->bn1c[k]);
        }
    tm.fc1 = now_s() - t0;

    // Three bidirectional layers. Each direction's input projections for every frame at once,
    // then the two directions side by side, a thread each.
    t0 = now_s();
    const float *lin = h1;
    int lstride = 2 * H;
    for (int l = 0; l < 3; l++) {
        float *lout = l == 2 ? h1 + H : (l == 0 ? la : lb); // the last layer lands beside fc1's output
        for (int d = 0; d < 2; d++) {
            float *gd = g + (long)d * T * 4 * L;
            dense(m->wih[l][d], 4 * L, H, lin, lstride, T, gd, 4 * L, threads);
            for (int t = 0; t < T; t++)
                for (int r = 0; r < 4 * L; r++) gd[(long)t * 4 * L + r] += m->bias[l][d][r];
        }
        float *tmp = l == 2 ? malloc(sizeof(float) * (size_t)T * H) : lout;
        if (!tmp) {
            ok = 0;
            goto done;
        }
        struct lstm_dir dirs[2] = {{m->whh[l][0], g, tmp, T, L, 0}, {m->whh[l][1], g + (long)T * 4 * L, tmp, T, L, 1}};
        pthread_t th;
        int two = threads > 1 && pthread_create(&th, 0, lstm_run, &dirs[1]) == 0;
        lstm_run(&dirs[0]);
        if (two) pthread_join(th, 0);
        else lstm_run(&dirs[1]);
        if (l == 2) {
            for (int t = 0; t < T; t++) memcpy(h1 + (long)t * 2 * H + H, tmp + (long)t * H, H * sizeof(float));
            free(tmp);
        }
        lin = lout == h1 + H ? 0 : lout;
        lstride = H;
    }
    tm.lstm = now_s() - t0;

    // fc2, bn2, relu, then fc3, bn3, the output scaling and relu: the stem's magnitude, as a mask.
    t0 = now_s();
    dense(m->fc2, H, 2 * H, h1, 2 * H, T, h2, H, threads);
    for (long i = 0; i < (long)T * H; i++) {
        int k = (int)(i % H);
        float v = h2[i] * m->bn2a[k] + m->bn2c[k];
        h2[i] = v > 0 ? v : 0;
    }
    dense(m->fc3, CH * O, H, h2, H, T, y, CH * O, threads);
    tm.fc23 = now_s() - t0;

    // Back to sound: the stem's magnitude on the mix's phase (or the mix less it), overlapped.
    t0 = now_s();
    for (int c = 0; c < CH; c++)
        for (int t = 0; t < T; t++) {
            const float *xr = Xre + ((long)c * T + t) * NBINS, *xi = Xim + ((long)c * T + t) * NBINS;
            const float *yo = y + ((long)t * CH + c) * O;
            for (int b = 0; b < NBINS; b++) {
                int k = c * O + b;
                float v = yo[b] * m->bn3a[k] + m->bn3c[k];
                v = v * m->output_scale[b] + m->output_mean[b];
                float mask = v > 0 ? v : 0; // the stem is mask x |mix|, on the mix's phase: mask x mix
                if (remove) mask = 1 - mask;
                re[b] = xr[b] * mask, im[b] = xi[b] * mask;
            }
            for (int b = 1; b < NFFT / 2; b++) re[NFFT - b] = re[b], im[NFFT - b] = -im[b]; // a real signal
            fft_run(f, re, im, 1);
            for (int k = 0; k < NFFT; k++) {
                long at = (long)t * HOP + k;
                acc[at * CH + c] += re[k] / NFFT * f->win[k];
                if (c == 0) env[at] += f->win[k] * f->win[k];
            }
        }
    for (long i = 0; i < n; i++) {
        float e = env[i + NFFT / 2];
        for (int c = 0; c < CH; c++) out[i * CH + c] = e > 1e-11f ? acc[(i + NFFT / 2) * CH + c] / e : 0;
    }
    tm.istft = now_s() - t0;

done:
    free(f), free(Xre), free(Xim), free(xin), free(h1), free(g), free(la), free(lb), free(h2), free(y), free(acc), free(env);
    if (times) *times = tm;
    return ok ? 0 : -1;
}
