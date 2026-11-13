#include "onset.h"
#include <math.h>
#include <stdlib.h>

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
