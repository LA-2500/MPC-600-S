// Master Effects (Shift + Pad 5): the last thing before the outputs, after the resample point,
// so nothing recorded or resampled ever has them. An EQ after the Pultec EQP-1A, then a
// compressor: after the dbx 160 (threshold, ratio, OverEasy, output), or the SP-303's Vinyl Sim.
// Set for loud, normalized material: thresholds in dB below full scale.
#include "lcd.h"
#include "os.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 44100.0f
#define PI_F 3.14159265f

static const int LOW_FREQS[4] = {20, 30, 60, 100};
static const int HIGH_FREQS[7] = {3000, 4000, 5000, 8000, 10000, 12000, 16000};
static const int ATTEN_SELS[3] = {5000, 10000, 20000};
static const float RATIOS[15] = {1.0f, 1.2f, 1.5f, 1.7f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f, 15.0f, 20.0f, 0};
#define RATIO_INF 14 // the last: infinity to one

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void master_defaults(struct master *m) {
    atomic_store(&m->eq_on, 0);
    atomic_store(&m->comp, MCOMP_OFF);
    atomic_store(&m->low_freq, 2);   // 60 Hz
    atomic_store(&m->low_boost, 20); // tenths of the knob's 0-10
    atomic_store(&m->low_atten, 10);
    atomic_store(&m->high_freq, 4); // 10 kHz
    atomic_store(&m->high_boost, 20);
    atomic_store(&m->bandwidth, 40);
    atomic_store(&m->atten_sel, 1); // 10 kHz
    atomic_store(&m->high_atten, 0);
    atomic_store(&m->thresh, -6);
    atomic_store(&m->ratio, 4); // 2:1
    atomic_store(&m->out_db, 0);
    atomic_store(&m->overeasy, 0);
    atomic_store(&m->vinyl[0], 40);
    atomic_store(&m->vinyl[1], 0);
    atomic_store(&m->vinyl[2], 10);
}

void master_init(struct master *m) {
    memset(m, 0, sizeof *m);
    master_defaults(m);
    m->vfx = fx_new();
    if (m->vfx) fx_set(m->vfx, FX_VINYL);
}

// ---- The audio ----

static void rbj(struct mbq *q, int kind, float hz, float qv, float db) { // 3 peak, 4 low shelf, 5 high shelf
    float A = powf(10.0f, db / 40.0f), w = 2 * PI_F * (hz > SR * 0.45f ? SR * 0.45f : hz) / SR, cw = cosf(w),
          sw = sinf(w), al = sw / (2 * qv), sA = sqrtf(A);
    float b0, b1, b2, a0, a1, a2;
    if (kind == 3) {
        b0 = 1 + al * A, b1 = -2 * cw, b2 = 1 - al * A, a0 = 1 + al / A, a1 = -2 * cw, a2 = 1 - al / A;
    } else if (kind == 4) {
        b0 = A * ((A + 1) - (A - 1) * cw + 2 * sA * al), b1 = 2 * A * ((A - 1) - (A + 1) * cw),
        b2 = A * ((A + 1) - (A - 1) * cw - 2 * sA * al), a0 = (A + 1) + (A - 1) * cw + 2 * sA * al,
        a1 = -2 * ((A - 1) + (A + 1) * cw), a2 = (A + 1) + (A - 1) * cw - 2 * sA * al;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cw + 2 * sA * al), b1 = -2 * A * ((A - 1) + (A + 1) * cw),
        b2 = A * ((A + 1) + (A - 1) * cw - 2 * sA * al), a0 = (A + 1) - (A - 1) * cw + 2 * sA * al,
        a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - 2 * sA * al;
    }
    q->c[0] = b0 / a0, q->c[1] = b1 / a0, q->c[2] = b2 / a0, q->c[3] = a1 / a0, q->c[4] = a2 / a0;
}

static inline float run(struct mbq *q, int ch, float x) {
    float y = q->c[0] * x + q->z[ch][0];
    q->z[ch][0] = q->c[1] * x - q->c[3] * y + q->z[ch][1];
    q->z[ch][1] = q->c[2] * x - q->c[4] * y;
    return y;
}

// The Pultec: the low boost a broad shelf at the chosen frequency; the low attenuation a
// shelf a little higher up, so boost and cut together give the famous lift under a dip in
// the low mids. The high boost a bell whose width is Bandwidth (sharp to broad); the high
// attenuation a shelf at its own frequency. Knobs 0-10: boosts up to about 13.5 dB low and
// 16 dB high, cuts up to 17.5 dB low and 16 dB high.
static void pultec(struct master *m, float *buf, int frames) {
    float lf = (float)LOW_FREQS[clampi(atomic_load_explicit(&m->low_freq, memory_order_relaxed), 0, 3)];
    float hf = (float)HIGH_FREQS[clampi(atomic_load_explicit(&m->high_freq, memory_order_relaxed), 0, 6)];
    float af = (float)ATTEN_SELS[clampi(atomic_load_explicit(&m->atten_sel, memory_order_relaxed), 0, 2)];
    float lb = atomic_load_explicit(&m->low_boost, memory_order_relaxed) / 100.0f;
    float la = atomic_load_explicit(&m->low_atten, memory_order_relaxed) / 100.0f;
    float hb = atomic_load_explicit(&m->high_boost, memory_order_relaxed) / 100.0f;
    float bw = atomic_load_explicit(&m->bandwidth, memory_order_relaxed) / 100.0f;
    float ha = atomic_load_explicit(&m->high_atten, memory_order_relaxed) / 100.0f;
    rbj(&m->eq[0], 4, lf, 0.55f, 13.5f * lb);
    rbj(&m->eq[1], 4, lf * 1.6f, 0.5f, -17.5f * la);
    rbj(&m->eq[2], 3, hf, 2.5f - 2.1f * bw, 16.0f * hb);
    rbj(&m->eq[3], 5, af, 0.7f, -16.0f * ha);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float x = buf[i * 2 + c];
            for (int k = 0; k < 4; k++) x = run(&m->eq[k], c, x);
            buf[i * 2 + c] = x;
        }
}

// The dbx 160: a true-RMS detector, stereo linked; above the threshold, gain reduction at the
// ratio (infinity holds the level there), OverEasy rounding the knee over 10 dB. It reaches
// the reduction quickly and lets go at 120 dB a second, as the 160 does.
static void dbx160(struct master *m, float *buf, int frames) {
    float thresh = (float)atomic_load_explicit(&m->thresh, memory_order_relaxed);
    int ri = clampi(atomic_load_explicit(&m->ratio, memory_order_relaxed), 0, RATIO_INF);
    float slope = ri == RATIO_INF ? 1.0f : 1.0f - 1.0f / RATIOS[ri];
    int soft = atomic_load_explicit(&m->overeasy, memory_order_relaxed);
    float out = powf(10, atomic_load_explicit(&m->out_db, memory_order_relaxed) / 20.0f);
    float rms_k = expf(-1 / (0.008f * SR)), att_k = expf(-1 / (0.003f * SR)), rel = 120.0f / SR;
    float most = 0;
    for (int i = 0; i < frames; i++) {
        float l = buf[i * 2], r = buf[i * 2 + 1];
        m->ms = (l * l + r * r) * 0.5f + rms_k * (m->ms - (l * l + r * r) * 0.5f);
        if (!(m->ms < 1e4f)) m->ms = 1e4f; // never NaN, never so loud it takes seconds to let go
        float level = 10 * log10f(m->ms + 1e-12f) + 3.01f; // RMS, read as a sine's peak
        float over = level - thresh, target;
        if (soft) target = over <= -5 ? 0 : over >= 5 ? over * slope : (over + 5) * (over + 5) / 20 * slope;
        else target = over > 0 ? over * slope : 0;
        if (target > m->gr) m->gr = target + att_k * (m->gr - target);
        else m->gr = m->gr - rel > target ? m->gr - rel : target;
        if (!(m->gr < 60.0f)) m->gr = 60.0f; // at most 60 dB down: back within half a second
        float g = powf(10, -m->gr / 20) * out;
        buf[i * 2] = l * g, buf[i * 2 + 1] = r * g;
        if (m->gr > most) most = m->gr;
    }
    atomic_store_explicit(&m->gr_meter, most, memory_order_relaxed);
}

void master_process(struct master *m, float *buf, int frames) {
    if (atomic_load_explicit(&m->bypass, memory_order_relaxed)) return;
    if (atomic_load_explicit(&m->eq_on, memory_order_relaxed)) pultec(m, buf, frames);
    int comp = atomic_load_explicit(&m->comp, memory_order_relaxed);
    if (comp == MCOMP_DBX) dbx160(m, buf, frames);
    else if (comp == MCOMP_VINYL && m->vfx) {
        for (int k = 0; k < 3; k++) m->vfx->p[k] = atomic_load_explicit(&m->vinyl[k], memory_order_relaxed);
        fx_process(m->vfx, buf, frames);
        atomic_store_explicit(&m->gr_meter, 0, memory_order_relaxed);
    } else {
        atomic_store_explicit(&m->gr_meter, 0, memory_order_relaxed);
    }
}

// ---- The page ----

enum { ME_ON, ME_LF, ME_LB, ME_LA, ME_HF, ME_HB, ME_BW, ME_AS, ME_HA, ME_FIELDS };
enum { MC_TYPE, MC_1, MC_2, MC_3, MC_EASY, MC_FIELDS };

void master_open(struct os *s) {
    if (s->screen == OS_MASTER) {
        s->cursor_master = 0; // pressed again: the first field
        return;
    }
    s->master_return = s->screen;
    s->screen = OS_MASTER;
    s->cursor_master = 0;
    for (int i = 0; i < 3; i++) s->knob_caught[i] = 0;
}

// A field's value moved by d; (v) its stored value. Returns the new one.
static int step_eq(int f, int v, int d) {
    switch (f) {
    case ME_LF: return clampi(v + d, 0, 3);
    case ME_HF: return clampi(v + d, 0, 6);
    case ME_AS: return clampi(v + d, 0, 2);
    default: return clampi(v + d * 5, 0, 100); // the knobs, half a step at a time
    }
}

static atomic_int *eq_field(struct master *m, int f) {
    atomic_int *const all[ME_FIELDS] = {&m->eq_on, &m->low_freq, &m->low_boost, &m->low_atten, &m->high_freq,
                                        &m->high_boost, &m->bandwidth, &m->atten_sel, &m->high_atten};
    return all[f];
}

// The comp page's knob fields: dbx Threshold, Ratio, Output; Vinyl Comp, Noise, Wow Flut.
static void comp_range(int comp, int k, int *lo, int *hi) {
    if (comp == MCOMP_VINYL) *lo = 0, *hi = 127;
    else if (k == 0) *lo = -40, *hi = 0;
    else if (k == 1) *lo = 0, *hi = RATIO_INF;
    else *lo = -20, *hi = 20;
}

static atomic_int *comp_field(struct master *m, int comp, int k) {
    if (comp == MCOMP_VINYL) return &m->vinyl[k];
    return k == 0 ? &m->thresh : k == 1 ? &m->ratio : &m->out_db;
}

static void knob(struct os *s, const struct input *in, int k, atomic_int *v, int lo, int hi) {
    if (!in->knob_moved[k]) return;
    int at = (atomic_load(v) - lo) * 127 / (hi - lo), last = s->knob_last[k], now = in->knob[k];
    s->knob_last[k] = now;
    if (!s->knob_caught[k]) {
        int crossed = last >= 0 && ((last <= at && now >= at) || (last >= at && now <= at));
        if (crossed || abs(now - at) <= 2) s->knob_caught[k] = 1;
    }
    if (s->knob_caught[k]) {
        int val = lo + (hi - lo) * now / 127;
        if (hi == 100 && lo == 0) val = val / 5 * 5; // the Pultec knobs in half steps
        atomic_store(v, val);
        s->master_dirty = 1;
    }
}

static void row(struct ui *u, int r, const char *label, const char *value, int on) {
    char b[32];
    lcd_text(u, 0, r, label);
    snprintf(b, sizeof b, " %s ", value);
    if (on) lcd_inverse(u, 14, r, b);
    else lcd_text(u, 14, r, b);
}

static void freq_text(int hz, char *b, size_t n) {
    if (hz >= 1000) snprintf(b, n, "%dkHz", hz / 1000);
    else snprintf(b, n, "%dHz", hz);
}

void master_frame(struct os *s, struct ui *u, const struct input *in) {
    struct master *m = &s->master;
    if (input_down(in, IN_B1) && s->master_page != 0) s->master_page = 0, s->cursor_master = 0;
    else if (input_down(in, IN_B1)) s->cursor_master = 0;
    if (input_down(in, IN_B2) && s->master_page != 1) s->master_page = 1, s->cursor_master = 0;
    else if (input_down(in, IN_B2)) s->cursor_master = 0;
    if (input_down(in, IN_B1) || input_down(in, IN_B2))
        for (int i = 0; i < 3; i++) s->knob_caught[i] = 0;
    if (input_down(in, IN_B3)) atomic_store(&m->bypass, !atomic_load(&m->bypass)), s->master_dirty = 1;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    int comp = atomic_load(&m->comp);
    char v[32];
    lcd_text(u, 0, KEY_ROW, "<EQ>");
    lcd_text(u, 9, KEY_ROW, "<Comp>");
    if (atomic_load(&m->bypass)) lcd_inverse(u, 18, KEY_ROW, "<Bypass>");
    else lcd_text(u, 18, KEY_ROW, "<Bypass>");
    if (s->master_page == 0) {
        lcd_inverse(u, 0, KEY_ROW, "<EQ>");
        if (in->encoder_press) s->cursor_master = (s->cursor_master + (in->shift ? ME_FIELDS - 1 : 1)) % ME_FIELDS;
        int f = s->cursor_master;
        if (d) {
            atomic_int *p = eq_field(m, f);
            atomic_store(p, f == ME_ON ? d > 0 : step_eq(f, atomic_load(p), d));
            s->master_dirty = 1;
        }
        knob(s, in, 0, &m->low_boost, 0, 100);
        knob(s, in, 1, &m->low_atten, 0, 100);
        knob(s, in, 2, &m->high_boost, 0, 100);
        for (int k = 0; k < 3; k++)
            if (in->knob_moved[k]) s->cursor_master = k == 0 ? ME_LB : k == 1 ? ME_LA : ME_HB;
        lcd_rule(u, TITLE_ROW, "Master EQ");
        row(u, 3, "EQ:", atomic_load(&m->eq_on) ? "ON" : "OFF", s->cursor_master == ME_ON);
        freq_text(LOW_FREQS[clampi(atomic_load(&m->low_freq), 0, 3)], v, sizeof v);
        row(u, 5, "Low Freq:", v, s->cursor_master == ME_LF);
        snprintf(v, sizeof v, "%d.%d", atomic_load(&m->low_boost) / 10, atomic_load(&m->low_boost) % 10);
        row(u, 6, "K1 Low Boost:", v, s->cursor_master == ME_LB);
        snprintf(v, sizeof v, "%d.%d", atomic_load(&m->low_atten) / 10, atomic_load(&m->low_atten) % 10);
        row(u, 7, "K2 Low Atten:", v, s->cursor_master == ME_LA);
        freq_text(HIGH_FREQS[clampi(atomic_load(&m->high_freq), 0, 6)], v, sizeof v);
        row(u, 8, "High Freq:", v, s->cursor_master == ME_HF);
        snprintf(v, sizeof v, "%d.%d", atomic_load(&m->high_boost) / 10, atomic_load(&m->high_boost) % 10);
        row(u, 9, "K3 High Boost:", v, s->cursor_master == ME_HB);
        snprintf(v, sizeof v, "%d.%d", atomic_load(&m->bandwidth) / 10, atomic_load(&m->bandwidth) % 10);
        row(u, 10, "Bandwidth:", v, s->cursor_master == ME_BW);
        freq_text(ATTEN_SELS[clampi(atomic_load(&m->atten_sel), 0, 2)], v, sizeof v);
        row(u, 11, "Atten Sel:", v, s->cursor_master == ME_AS);
        snprintf(v, sizeof v, "%d.%d", atomic_load(&m->high_atten) / 10, atomic_load(&m->high_atten) % 10);
        row(u, 12, "High Atten:", v, s->cursor_master == ME_HA);
    } else {
        lcd_inverse(u, 9, KEY_ROW, "<Comp>");
        int n = comp == MCOMP_DBX ? MC_FIELDS : comp == MCOMP_VINYL ? MC_EASY : 1;
        if (s->cursor_master >= n) s->cursor_master = 0;
        if (in->encoder_press) s->cursor_master = (s->cursor_master + (in->shift ? n - 1 : 1)) % n;
        int f = s->cursor_master;
        if (d && f == MC_TYPE) atomic_store(&m->comp, clampi(comp + d, 0, MCOMP_TYPES - 1)), s->master_dirty = 1;
        else if (d && f == MC_EASY) atomic_store(&m->overeasy, d > 0), s->master_dirty = 1;
        else if (d) {
            int lo, hi;
            comp_range(comp, f - MC_1, &lo, &hi);
            atomic_int *p = comp_field(m, comp, f - MC_1);
            atomic_store(p, clampi(atomic_load(p) + d, lo, hi));
            s->master_dirty = 1;
        }
        comp = atomic_load(&m->comp);
        for (int k = 0; k < 3 && comp != MCOMP_OFF; k++) {
            int lo, hi;
            comp_range(comp, k, &lo, &hi);
            knob(s, in, k, comp_field(m, comp, k), lo, hi);
            if (in->knob_moved[k]) s->cursor_master = MC_1 + k;
        }
        static const char *const TYPES[MCOMP_TYPES] = {"OFF", "DBX 160", "VINYL SIM"};
        lcd_rule(u, TITLE_ROW, "Master Comp");
        row(u, 3, "Comp:", TYPES[comp], s->cursor_master == MC_TYPE);
        if (comp == MCOMP_DBX) {
            snprintf(v, sizeof v, "%ddB", atomic_load(&m->thresh));
            row(u, 5, "K1 Threshold:", v, s->cursor_master == MC_1);
            int ri = clampi(atomic_load(&m->ratio), 0, RATIO_INF);
            if (ri == RATIO_INF) snprintf(v, sizeof v, "INF:1");
            else snprintf(v, sizeof v, "%.1f:1", RATIOS[ri]);
            row(u, 6, "K2 Ratio:", v, s->cursor_master == MC_2);
            snprintf(v, sizeof v, "%+ddB", atomic_load(&m->out_db));
            row(u, 7, "K3 Output:", v, s->cursor_master == MC_3);
            row(u, 8, "OverEasy:", atomic_load(&m->overeasy) ? "ON" : "OFF", s->cursor_master == MC_EASY);
        } else if (comp == MCOMP_VINYL) {
            static const char *const L[3] = {"K1 Comp:", "K2 Noise:", "K3 Wow Flut:"};
            for (int k = 0; k < 3; k++) {
                snprintf(v, sizeof v, "%d", atomic_load(&m->vinyl[k]));
                row(u, 5 + k, L[k], v, s->cursor_master == MC_1 + k);
            }
        }
        if (comp == MCOMP_DBX) { // gain reduction, a bar a dB
            float gr = atomic_load(&m->gr_meter);
            lcd_text(u, 0, 10, "Gain Reduction:");
            snprintf(v, sizeof v, "-%.1fdB", gr);
            lcd_text(u, 16, 10, v);
            int bars = (int)(gr + 0.5f) > 24 ? 24 : (int)(gr + 0.5f);
            for (int i = 0; i < bars; i++) lcd_fill(u, LCD_X + i * 13, 11 * CELL_H + 3, 10, 10, LCD_FG);
            ui_wake(u, 60);
        }
    }
    lcd_text(u, 0, 13, atomic_load(&m->bypass) ? "Master FX bypassed" : "EQ > Comp > Output");
    lcd_rule(u, BOTTOM_ROW, 0);
}

void master_save_line(const struct master *m, char *b, size_t n) {
    struct master *w = (struct master *)m;
    snprintf(b, n, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", atomic_load(&w->eq_on), atomic_load(&w->comp),
             atomic_load(&w->low_freq), atomic_load(&w->low_boost), atomic_load(&w->low_atten), atomic_load(&w->high_freq),
             atomic_load(&w->high_boost), atomic_load(&w->bandwidth), atomic_load(&w->atten_sel), atomic_load(&w->high_atten),
             atomic_load(&w->thresh), atomic_load(&w->ratio), atomic_load(&w->out_db), atomic_load(&w->overeasy),
             atomic_load(&w->vinyl[0]), atomic_load(&w->vinyl[1]), atomic_load(&w->vinyl[2]), atomic_load(&w->bypass));
}

void master_load_line(struct master *m, const char *line) {
    int v[18];
    if (sscanf(line, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
               &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16], &v[17]) != 18)
        return;
    atomic_store(&m->eq_on, v[0] != 0);
    atomic_store(&m->comp, clampi(v[1], 0, MCOMP_TYPES - 1));
    atomic_store(&m->low_freq, clampi(v[2], 0, 3));
    atomic_store(&m->low_boost, clampi(v[3], 0, 100));
    atomic_store(&m->low_atten, clampi(v[4], 0, 100));
    atomic_store(&m->high_freq, clampi(v[5], 0, 6));
    atomic_store(&m->high_boost, clampi(v[6], 0, 100));
    atomic_store(&m->bandwidth, clampi(v[7], 0, 100));
    atomic_store(&m->atten_sel, clampi(v[8], 0, 2));
    atomic_store(&m->high_atten, clampi(v[9], 0, 100));
    atomic_store(&m->thresh, clampi(v[10], -40, 0));
    atomic_store(&m->ratio, clampi(v[11], 0, RATIO_INF));
    atomic_store(&m->out_db, clampi(v[12], -20, 20));
    atomic_store(&m->overeasy, v[13] != 0);
    for (int k = 0; k < 3; k++) atomic_store(&m->vinyl[k], clampi(v[14 + k], 0, 127));
    atomic_store(&m->bypass, v[17] != 0);
}
