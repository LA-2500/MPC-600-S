// Track EQ: three bands (low shelf, mid bell, high shelf) on each track, set on the mixer
// with K1-K3 and in full on its EQ page (B3). Two types, after two consoles' EQs:
//   1073:   the Neve 1073's stepped frequencies, its broad mid, shelves with a little bump
//   4000 E: the SSL 4000 E's ranges in three bands, any frequency, the mid's Q set
// These are digital filters set up after those consoles, not models of their circuits.
#include "lcd.h"
#include "os.h"
#include "seq.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int LF_1073[] = {35, 60, 110, 220};
static const int MF_1073[] = {360, 700, 1600, 3200, 4800, 7200};
#define HF_1073 12000
#define Q_1073 0.8 // the 1073's mid is broad
#define SHELF_1073 1.3 // a slope past 1 gives the shelf its bump just past the corner
#define COUNT(a) ((int)(sizeof a / sizeof a[0]))

int eq_gain_max(int type) {
    return type == EQ_1073 ? 160 : 150; // tenths of a dB
}

void eq_defaults(struct track_eq *q, int type) {
    *q = (struct track_eq){.type = type, .on = 1};
    if (type == EQ_1073) q->lf = 110, q->mf = 1600, q->mq = 8, q->hf = HF_1073;
    else q->lf = 100, q->mf = 1000, q->mq = 10, q->hf = 8000;
}

void eq_freq_text(int hz, char *b, size_t n) {
    if (hz < 1000) snprintf(b, n, "%dHz", hz);
    else if (hz % 1000 == 0 || hz >= 10000) snprintf(b, n, "%dk", (hz + 500) / 1000);
    else snprintf(b, n, "%d.%dk", hz / 1000, hz % 1000 / 100);
}

static int nearest(const int *list, int n, int hz) {
    int best = list[0];
    for (int i = 1; i < n; i++)
        if (abs(list[i] - hz) < abs(best - hz)) best = list[i];
    return best;
}

// A frequency stepped d places: through the 1073's list, or about a twelfth of an octave a
// step on the 4000 E, on round numbers.
static int step_freq(int hz, int d, int band, int type) {
    if (type == EQ_1073) {
        const int *l = band == 0 ? LF_1073 : MF_1073;
        int n = band == 0 ? COUNT(LF_1073) : COUNT(MF_1073), i = 0;
        if (band == 2) return HF_1073;
        while (i < n - 1 && l[i] != nearest(l, n, hz)) i++;
        i += d;
        return l[i < 0 ? 0 : i >= n ? n - 1 : i];
    }
    static const int lo[3] = {30, 200, 1500}, hi[3] = {450, 7000, 16000};
    for (int k = 0; k < (d > 0 ? d : -d); k++) {
        double f = hz * pow(2.0, (d > 0 ? 1.0 : -1.0) / 12.0);
        int unit = f < 100 ? 1 : f < 1000 ? 10 : 100, next = (int)lround(f / unit) * unit;
        if (next == hz) next += d > 0 ? unit : -unit;
        hz = next;
    }
    return hz < lo[band] ? lo[band] : hz > hi[band] ? hi[band] : hz;
}

// RBJ biquads, normalised so a0 is 1. A shelf's slope S: 1 is the plain shelf.
static void shelf(float c[5], int high, double hz, double db, double slope) {
    double A = pow(10.0, db / 40.0), w = 2 * M_PI * hz / ENGINE_RATE, cw = cos(w), sw = sin(w);
    double arg = (A + 1 / A) * (1 / slope - 1) + 2;
    double alpha = sw / 2 * sqrt(arg > 0.001 ? arg : 0.001), r = 2 * sqrt(A) * alpha, b0, b1, b2, a0, a1, a2;
    if (!high) {
        b0 = A * ((A + 1) - (A - 1) * cw + r), b1 = 2 * A * ((A - 1) - (A + 1) * cw), b2 = A * ((A + 1) - (A - 1) * cw - r);
        a0 = (A + 1) + (A - 1) * cw + r, a1 = -2 * ((A - 1) + (A + 1) * cw), a2 = (A + 1) + (A - 1) * cw - r;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cw + r), b1 = -2 * A * ((A - 1) + (A + 1) * cw), b2 = A * ((A + 1) + (A - 1) * cw - r);
        a0 = (A + 1) - (A - 1) * cw + r, a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - r;
    }
    c[0] = (float)(b0 / a0), c[1] = (float)(b1 / a0), c[2] = (float)(b2 / a0), c[3] = (float)(a1 / a0), c[4] = (float)(a2 / a0);
}

static void bell(float c[5], double hz, double db, double q) {
    double A = pow(10.0, db / 40.0), w = 2 * M_PI * hz / ENGINE_RATE, cw = cos(w), alpha = sin(w) / (2 * q);
    double a0 = 1 + alpha / A;
    c[0] = (float)((1 + alpha * A) / a0), c[1] = (float)(-2 * cw / a0), c[2] = (float)((1 - alpha * A) / a0);
    c[3] = (float)(-2 * cw / a0), c[4] = (float)((1 - alpha / A) / a0);
}

void eq_coeffs(const struct track_eq *q, float c[3][5]) {
    int n = q->type == EQ_1073;
    shelf(c[0], 0, q->lf, q->lg / 10.0, n ? SHELF_1073 : 1.0);
    bell(c[1], q->mf, q->mg / 10.0, n ? Q_1073 : q->mq / 10.0);
    shelf(c[2], 1, n ? HF_1073 : q->hf, q->hg / 10.0, n ? SHELF_1073 : 1.0);
}

// Tells the engine a track's EQ. Flat or off, it takes no bus and costs nothing.
void os_eq_push(struct os *s, int t) {
    pthread_mutex_lock(&s->seq.lock);
    struct track_eq q = s->seq.sequence[s->seq.current].track[t].eq;
    pthread_mutex_unlock(&s->seq.lock);
    float c[3][5];
    int on = q.on && (q.lg || q.mg || q.hg);
    if (on) eq_coeffs(&q, c);
    engine_track_eq(s->engine, t + 1, on, on ? c : 0);
}

void os_eq_push_all(struct os *s) {
    for (int t = 0; t < SEQ_TRACKS; t++) os_eq_push(s, t);
    s->eq_seq = s->seq.current;
}

// ---- The EQ page (B3 on the mixer) ----

enum { E_TYPE, E_LF, E_LG, E_MF, E_MG, E_MQ, E_HF, E_HG, E_ON, E_FIELDS };
static const int ROW[E_FIELDS] = {3, 5, 5, 6, 6, 8, 7, 7, 10};
static const int COL[E_FIELDS] = {5, 5, 13, 5, 13, 6, 5, 13, 5};

static int shown(const struct track_eq *q, int f) {
    return q->type == EQ_4000E || (f != E_MQ && f != E_HF); // the 1073's mid Q and high shelf are fixed
}

void eq_open(struct os *s) {
    s->eq_return = s->screen;
    s->screen = OS_EQ;
    s->cursor_eq = 0;
}

static void gain_text(int g, char *b, size_t n) {
    snprintf(b, n, "%+d.%ddB", g / 10, (g < 0 ? -g : g) % 10);
}

void eq_change(struct os *s, struct track_eq *q, int f, int d) {
    int max = eq_gain_max(q->type);
    switch (f) {
    case E_TYPE: { // the gains carry over, the frequencies go to the nearest the new type has
        struct track_eq keep = *q;
        eq_defaults(q, d > 0 ? EQ_4000E : EQ_1073);
        q->on = keep.on;
        q->lg = keep.lg, q->mg = keep.mg, q->hg = keep.hg;
        int m = eq_gain_max(q->type);
        q->lg = q->lg > m ? m : q->lg < -m ? -m : q->lg;
        q->mg = q->mg > m ? m : q->mg < -m ? -m : q->mg;
        q->hg = q->hg > m ? m : q->hg < -m ? -m : q->hg;
        if (q->type == EQ_1073) q->lf = nearest(LF_1073, COUNT(LF_1073), keep.lf), q->mf = nearest(MF_1073, COUNT(MF_1073), keep.mf);
        else q->lf = keep.lf, q->mf = keep.mf;
        break;
    }
    case E_LF: q->lf = step_freq(q->lf, d, 0, q->type); break;
    case E_MF: q->mf = step_freq(q->mf, d, 1, q->type); break;
    case E_HF: q->hf = step_freq(q->hf, d, 2, q->type); break;
    case E_LG: q->lg = q->lg + d * 5 > max ? max : q->lg + d * 5 < -max ? -max : q->lg + d * 5; break;
    case E_MG: q->mg = q->mg + d * 5 > max ? max : q->mg + d * 5 < -max ? -max : q->mg + d * 5; break;
    case E_HG: q->hg = q->hg + d * 5 > max ? max : q->hg + d * 5 < -max ? -max : q->hg + d * 5; break;
    case E_MQ: q->mq = q->mq + d < 5 ? 5 : q->mq + d > 25 ? 25 : q->mq + d; break;
    case E_ON: q->on = d > 0; break;
    }
    (void)s;
}

void eq_frame(struct os *s, struct ui *u, const struct input *in) {
    int t = s->mixer_track;
    if (input_down(in, IN_B1)) {
        s->screen = s->eq_return;
        ui_wake(u, 1);
        return;
    }
    pthread_mutex_lock(&s->seq.lock);
    struct track_eq *q = &s->seq.sequence[s->seq.current].track[t].eq;
    if (!shown(q, s->cursor_eq)) s->cursor_eq = 0;
    if (in->encoder_press) {
        do s->cursor_eq = (s->cursor_eq + (in->shift ? E_FIELDS - 1 : 1)) % E_FIELDS;
        while (!shown(q, s->cursor_eq));
    }
    // The wheel, not while a knob is being worked (turning one can shake the wheel a step).
    int d = (os_ctl_busy(s) ? 0 : in->encoder) + input_down(in, IN_PLUS) - input_down(in, IN_MINUS), changed = 0;
    if (d) eq_change(s, q, s->cursor_eq, d), changed = 1;
    for (int k = 0; k < 3; k++) { // K1-K3: the three gains, as on the mixer
        if (!in->knob_moved[k]) continue;
        int *g = k == 0 ? &q->lg : k == 1 ? &q->mg : &q->hg, max = eq_gain_max(q->type);
        int at = (*g + max) * 127 / (2 * max), last = s->knob_last[k], now = in->knob[k];
        s->knob_last[k] = now;
        int pos = os_knob_take(s, &s->knob_caught[k], last, now, at);
        if (pos >= 0) *g = (pos * 2 * max / 127 - max) / 5 * 5, changed = 1;
        s->cursor_eq = k == 0 ? E_LG : k == 1 ? E_MG : E_HG;
    }
    struct track_eq show = *q;
    pthread_mutex_unlock(&s->seq.lock);
    if (changed) os_eq_push(s, t);

    char v[E_FIELDS][16];
    lcd_text(u, 0, KEY_ROW, "<Back>");
    lcd_rule(u, TITLE_ROW, "EQ Settings");
    lcd_text(u, 0, 3, "Type:");
    lcd_text(u, 0, 5, "Low:");
    lcd_text(u, 0, 6, "Mid:");
    lcd_text(u, 0, 7, "High:");
    lcd_text(u, 0, 8, "Mid Q:");
    lcd_text(u, 0, 10, "EQ:");
    snprintf(v[E_TYPE], 16, " %s ", show.type == EQ_1073 ? "1073" : "4000 E");
    char f[12], g[12];
    eq_freq_text(show.lf, f, sizeof f), snprintf(v[E_LF], 16, " %s ", f);
    eq_freq_text(show.mf, f, sizeof f), snprintf(v[E_MF], 16, " %s ", f);
    eq_freq_text(show.type == EQ_1073 ? HF_1073 : show.hf, f, sizeof f), snprintf(v[E_HF], 16, " %s ", f);
    gain_text(show.lg, g, sizeof g), snprintf(v[E_LG], 16, " %s ", g);
    gain_text(show.mg, g, sizeof g), snprintf(v[E_MG], 16, " %s ", g);
    gain_text(show.hg, g, sizeof g), snprintf(v[E_HG], 16, " %s ", g);
    if (show.type == EQ_1073) snprintf(v[E_MQ], 16, " -- ");
    else snprintf(v[E_MQ], 16, " %d.%d ", show.mq / 10, show.mq % 10);
    snprintf(v[E_ON], 16, " %s ", show.on ? "ON" : "OFF");
    for (int i = 0; i < E_FIELDS; i++) {
        if (i == s->cursor_eq) lcd_inverse(u, COL[i], ROW[i], v[i]);
        else lcd_text(u, COL[i], ROW[i], v[i]);
    }
    if (show.type == EQ_1073) lcd_text(u, 0, 12, "1073: stepped frequencies");
    else lcd_text(u, 0, 12, "4000 E: any frequency");
    lcd_rule(u, BOTTOM_ROW, 0);
}
