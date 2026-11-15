#include "onset.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HOP 256          // 5.8 ms analysis steps
#define AIR 132          // 3 ms of air before each hit
#define SNAP 88          // look up to 2 ms further back for a zero crossing
#define MIN_GAP 10       // hits closer than about 58 ms count as one
#define WINDOW 8         // steps either side for the local average
#define SPAN 4           // steps of energy compared before and after, about 23 ms
#define RISE 0.5f        // log10 energy rise needed on top of the local average, about 5 dB
#define GATE 1e-4f       // ignore anything 40 dB under the loudest step
#define LOW_COEF 0.028f  // one-pole low-pass near 200 Hz

static float mono(const struct sample *s, int32_t i) {
    const float *f = s->data + (size_t)i * (size_t)s->channels;
    return s->channels == 2 ? 0.5f * (f[0] + f[1]) : f[0];
}

// Back from a detected step to where the hit actually begins, then a little before it.
static int32_t refine(const struct sample *s, int step) {
    int32_t peak_end = (int32_t)(step + 6) * HOP;
    if (peak_end > s->frames) peak_end = s->frames;
    float peak = 0;
    for (int32_t i = (int32_t)step * HOP; i < peak_end; i++) {
        float v = fabsf(mono(s, i));
        if (v > peak) peak = v;
    }
    int32_t from = (int32_t)(step - 2) * HOP;
    if (from < 0) from = 0;
    int32_t at = (int32_t)step * HOP;
    for (int32_t i = from; i < peak_end; i++) {
        if (fabsf(mono(s, i)) >= 0.15f * peak) {
            at = i;
            break;
        }
    }
    at -= AIR;
    if (at < 0) return 0;
    float v = mono(s, at);
    for (int32_t i = at; i > 0 && at - i < SNAP; i--) {
        float w = mono(s, i - 1);
        if ((v >= 0) != (w >= 0) || v == 0) return i;
        v = w;
    }
    return at;
}

int onset_detect(const struct sample *s, int32_t *out, int max) {
    return onset_detect_rise(s, out, max, RISE);
}

int onset_detect_rise(const struct sample *s, int32_t *out, int max, float rise) {
    if (!s || !out || max < 1) return 0;
    int n = 0;
    out[n++] = 0;
    int steps = s->frames / HOP;
    if (steps < 3) return n;
    float *low = malloc((size_t)steps * sizeof *low);
    float *high = malloc((size_t)steps * sizeof *high);
    float *d = malloc((size_t)steps * sizeof *d);
    if (!low || !high || !d) {
        free(low);
        free(high);
        free(d);
        return n;
    }
    float lp = 0, prev = 0, loudest = 0;
    for (int h = 0; h < steps; h++) {
        float el = 0, eh = 0;
        for (int32_t i = (int32_t)h * HOP; i < (int32_t)(h + 1) * HOP; i++) {
            float x = mono(s, i);
            lp += LOW_COEF * (x - lp);
            float hi = x - prev;
            prev = x;
            el += lp * lp;
            eh += hi * hi;
        }
        low[h] = el;
        high[h] = eh;
        if (el + eh > loudest) loudest = el + eh;
    }
    // How much each band's energy over the next SPAN steps beats the SPAN steps before, in
    // log10 units. Whole spans, not single steps, so a noisy tail or a slow bass wave does
    // not look like a new hit.
    for (int h = 0; h < steps; h++) {
        float before_l = 0, before_h = 0, after_l = 0, after_h = 0;
        for (int k = 1; k <= SPAN; k++) {
            if (h - k >= 0) before_l += low[h - k], before_h += high[h - k];
            if (h + k - 1 < steps) after_l += low[h + k - 1], after_h += high[h + k - 1];
        }
        float rl = log10f(after_l + 1e-9f) - log10f(before_l + 1e-9f);
        float rh = log10f(after_h + 1e-9f) - log10f(before_h + 1e-9f);
        d[h] = (rl > 0 ? rl : 0) + (rh > 0 ? rh : 0);
    }
    int last = -MIN_GAP;
    for (int h = 1; h < steps - 1 && n < max; h++) {
        if (d[h] < d[h - 1] || d[h] <= d[h + 1]) continue;
        if (low[h] + high[h] < loudest * GATE && low[h + 1] + high[h + 1] < loudest * GATE) continue;
        float sum = 0;
        int count = 0;
        for (int k = h - WINDOW; k <= h + WINDOW; k++) {
            if (k < 0 || k >= steps || k == h) continue;
            sum += d[k];
            count++;
        }
        if (d[h] < (count ? sum / count : 0) * 1.5f + rise) continue;
        if (h - last < MIN_GAP) continue;
        int32_t at = refine(s, h);
        if (at <= out[n - 1] + HOP) continue;
        out[n++] = at;
        last = h;
    }
    free(low);
    free(high);
    free(d);
    return n;
}

// ---- TRANSIENT: drum breaks ----

#define T_HOP 128       // 2.9 ms steps
#define T_MEDIAN 20     // steps either side for the threshold's median (about 58 ms)
#define T_GAP_MS 40     // hits closer than this are one
#define T_AIR_MS 1      // before the hit
#define T_GATE 1e-5f    // ignore anything 50 dB under the loudest step

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

// How sharp the sound is at i: its change from the sample before. A hit's attack is sharp, a
// ringing kick before it is not, so this finds where a hit starts even under another's tail.
static float sharp(const struct sample *s, int32_t i) {
    return i > 0 ? fabsf(mono(s, i) - mono(s, i - 1)) : fabsf(mono(s, i));
}

// Back from a detected step to where the hit begins: where its sharpness first rises clearly
// over what came before, a millisecond before that, then a zero crossing.
static int32_t refine_transient(const struct sample *s, int step, int32_t after) {
    int32_t peak_end = (int32_t)(step + 4) * T_HOP;
    if (peak_end > s->frames) peak_end = s->frames;
    float peak = 0;
    for (int32_t i = (int32_t)step * T_HOP; i < peak_end; i++) {
        float v = sharp(s, i);
        if (v > peak) peak = v;
    }
    // Back as far as the followers lag (a kick shows a few ms late), not past the hit before.
    int32_t from = (int32_t)(step - 6) * T_HOP;
    if (from <= after) from = after + 1;
    if (from < 0) from = 0;
    // The sound before this hit: what is still ringing from the last. The hit starts where it
    // rises clearly over that.
    float floor = 0;
    for (int32_t i = from; i < from + T_HOP && i < s->frames; i++) floor = fmaxf(floor, sharp(s, i));
    float need = fmaxf(0.15f * peak, floor * 1.6f);
    int32_t at = (int32_t)step * T_HOP;
    for (int32_t i = from + T_HOP; i < peak_end; i++)
        if (sharp(s, i) >= need) {
            at = i;
            break;
        }
    int32_t air = s->rate * T_AIR_MS / 1000;
    at -= air;
    if (at <= 0) return 0;
    float v = mono(s, at);
    for (int32_t i = at; i > 0 && at - i < air; i--) {
        float w = mono(s, i - 1);
        if ((v >= 0) != (w >= 0) || v == 0) return i;
        v = w;
    }
    return at;
}

int onset_detect_transient(const struct sample *s, int32_t *out, float *strength, int max, int sens) {
    if (!s || !out || max < 1 || s->rate <= 0) return 0;
    int steps = s->frames / T_HOP;
    if (steps < 4) {
        out[0] = 0;
        if (strength) strength[0] = 0;
        return 1;
    }
    float *le[3], *flux = malloc((size_t)steps * sizeof *flux), *tot = malloc((size_t)steps * sizeof *tot);
    float *win = malloc((size_t)(2 * T_MEDIAN + 1) * sizeof *win);
    int32_t *at = malloc((size_t)steps * sizeof *at);
    float *st = malloc((size_t)steps * sizeof *st);
    for (int b = 0; b < 3; b++) le[b] = malloc((size_t)steps * sizeof *le[b]);
    if (!flux || !tot || !win || !at || !st || !le[0] || !le[1] || !le[2]) {
        free(flux), free(tot), free(win), free(at), free(st);
        for (int b = 0; b < 3; b++) free(le[b]);
        out[0] = 0;
        return 1;
    }
    // The bands: low (kick) under 150 Hz, mid (snare body) 150 Hz to 2 kHz, high (hats, cymbals,
    // the snare's crack) over 5 kHz, from one-pole filters.
    float c150 = 1.0f - expf(-2.0f * 3.14159265f * 150.0f / s->rate);
    float c2k = 1.0f - expf(-2.0f * 3.14159265f * 2000.0f / s->rate);
    float c5k = 1.0f - expf(-2.0f * 3.14159265f * 5000.0f / s->rate);
    // Each band's level through an envelope follower as slow as its lowest wave (a kick's 50 Hz
    // would otherwise ripple step to step and look like hits): 12 ms low, 4 ms mid, 1.5 ms high.
    const float TAU[3] = {0.012f, 0.004f, 0.0015f};
    float a[3], env[3] = {0, 0, 0};
    for (int b = 0; b < 3; b++) a[b] = 1.0f - expf(-1.0f / (TAU[b] * (float)s->rate));
    float l150 = 0, l2k = 0, l5k = 0, loudest = 0;
    for (int h = 0; h < steps; h++) {
        for (int32_t i = (int32_t)h * T_HOP; i < (int32_t)(h + 1) * T_HOP; i++) {
            float x = mono(s, i);
            l150 += c150 * (x - l150), l2k += c2k * (x - l2k), l5k += c5k * (x - l5k);
            float lo = l150, mid = l2k - l150, hi = x - l5k;
            env[0] += a[0] * (lo * lo - env[0]), env[1] += a[1] * (mid * mid - env[1]), env[2] += a[2] * (hi * hi - env[2]);
        }
        for (int b = 0; b < 3; b++) le[b][h] = log10f(env[b] + 1e-10f);
        tot[h] = env[0] + env[1] + env[2];
        if (tot[h] > loudest) loudest = tot[h];
    }
    // How much each band rises over the louder of the two steps before (so a hit's own
    // second step is not a new one), the high band counted a little less (hats are many).
    static const float W[3] = {1.0f, 1.0f, 0.7f};
    for (int h = 0; h < steps; h++) {
        float f = 0;
        for (int b = 0; b < 3; b++) {
            float before = h >= 2 ? fmaxf(le[b][h - 1], le[b][h - 2]) : h >= 1 ? le[b][h - 1] : le[b][h];
            float r = le[b][h] - before;
            if (r > 0) f += W[b] * r;
        }
        flux[h] = f;
    }
    int s10 = sens < 1 ? 1 : sens > 10 ? 10 : sens;
    float delta = 0.55f - 0.045f * (float)s10; // 5: about 3 dB over the local median
    int gap = s->rate * T_GAP_MS / 1000 / T_HOP, n = 0, last = -1000000;
    for (int h = 1; h + 1 < steps; h++) {
        if (flux[h] < flux[h - 1] || flux[h] <= flux[h + 1]) continue; // a peak
        if (tot[h] < loudest * T_GATE && tot[h + 1] < loudest * T_GATE) continue;
        int m = 0;
        for (int k = h - T_MEDIAN; k <= h + T_MEDIAN; k++)
            if (k >= 0 && k < steps) win[m++] = flux[k];
        qsort(win, (size_t)m, sizeof *win, cmp_float);
        if (flux[h] < win[m / 2] + delta) continue;
        if (h - last < gap) { // too close: the stronger one stands
            if (n && flux[h] > st[n - 1]) at[n - 1] = refine_transient(s, h, n > 1 ? at[n - 2] : -1), st[n - 1] = flux[h], last = h;
            continue;
        }
        at[n] = refine_transient(s, h, n ? at[n - 1] : -1), st[n] = flux[h];
        if (n && at[n] <= at[n - 1]) continue;
        n++, last = h;
    }
    while (n > max) { // too many: the weakest goes
        int weakest = 0;
        for (int i = 1; i < n; i++)
            if (st[i] < st[weakest]) weakest = i;
        memmove(at + weakest, at + weakest + 1, (size_t)(n - weakest - 1) * sizeof *at);
        memmove(st + weakest, st + weakest + 1, (size_t)(n - weakest - 1) * sizeof *st);
        n--;
    }
    if (n == 0) at[0] = 0, st[0] = 0, n = 1; // no hits: one zone, the whole sound
    for (int i = 0; i < n; i++) {
        out[i] = at[i];
        if (strength) strength[i] = st[i];
    }
    free(flux), free(tot), free(win), free(at), free(st);
    for (int b = 0; b < 3; b++) free(le[b]);
    return n;
}
