// The S600 screen, where a pad's S600 settings are edited (Env1, Env2, Filter, LFO and
// the Main page), and the 16 Levels screen.
#include "lcd.h"
#include "os.h"
#include <math.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Env1 and Env2 templates, after the Akai S-series: attack, decay, sustain (percent), release.
static const struct {
    const char *name;
    int amp[4], flt[4];
} TEMPLATES[] = {
    // Like Elec. Organ, it holds while the pad is held, for bass lines played on the pads:
    // a soft few ms in so a held sine or saw does not click, a release long enough to round
    // off a short note but not blur the next, and Env2 giving each note a little thump that
    // settles warm (set Env2 Depth on the Filter page to hear it).
    {"MPC Bass", {3, 10, 100, 70}, {0, 250, 55, 70}},
    {"Piano", {2, 2000, 0, 400}, {0, 1200, 20, 400}},
    // Synth brass: a snappy stab whose filter sweeps open slowly and settles low, the
    // "bwaah" of analog brass (with a low cutoff and high Env2 Depth on the Filter page).
    {"Synth Brass", {30, 500, 75, 250}, {120, 600, 40, 250}},
    // Lead lines: a quick start with a little bite that eases down to a singing sustain, a
    // release long enough to carry between legato notes, and Env2 swelling the filter open.
    {"Synth Lead", {5, 400, 80, 200}, {20, 500, 50, 200}},
    {"Bell", {0, 3000, 0, 2000}, {0, 1500, 10, 1500}},
    {"Elec. Piano", {2, 1800, 20, 400}, {0, 800, 30, 400}},
    {"Guitar/Bass", {1, 1200, 10, 150}, {0, 500, 20, 150}},
    {"Synth Bass 1", {0, 250, 60, 60}, {0, 150, 20, 60}},
    {"Synth Bass 2", {0, 400, 40, 80}, {0, 300, 0, 80}},
    {"Synth Bass 3", {5, 600, 70, 120}, {10, 600, 40, 120}},
    {"Clav/Harpsi", {0, 700, 0, 120}, {0, 300, 10, 100}},
    {"Elec. Organ", {2, 10, 100, 40}, {0, 10, 100, 40}},
    {"Pipe Organ", {60, 10, 100, 400}, {60, 10, 100, 400}},
    {"String/Vox", {250, 500, 90, 600}, {300, 600, 80, 600}},
    {"Slow String", {900, 800, 85, 1200}, {1000, 800, 70, 1200}},
    {"Woodwind", {50, 200, 85, 150}, {60, 300, 70, 150}},
    {"Panpipe", {120, 300, 80, 350}, {100, 300, 60, 350}},
    {"Brass", {60, 300, 80, 200}, {80, 400, 60, 200}},
    {"Brass Swell", {700, 400, 90, 300}, {900, 500, 80, 300}},
    {"Short Perc", {0, 150, 0, 80}, {0, 80, 0, 80}},
    {"Dry Drum", {0, 300, 0, 100}, {0, 150, 0, 100}},
    {"Long Drum", {0, 1200, 0, 600}, {0, 600, 0, 600}},
    {"Ambient Drum", {0, 2500, 0, 2000}, {0, 1500, 10, 1500}},
    {"Cymbal/Gong", {0, 4000, 0, 3000}, {0, 3000, 30, 3000}},
    {"Tuned Perc", {0, 1000, 0, 500}, {0, 500, 0, 400}},
};
#define TEMPLATE_COUNT ((int)(sizeof TEMPLATES / sizeof TEMPLATES[0]))
#define DEFAULT_TEMPLATE 0 // MPC Bass: a new S600 sound plays bass lines on the pads at once: holds while the pad is held, like an organ

static void apply_template(struct s600_params *q, int env, int t) {
    const int *v = env == 1 ? TEMPLATES[t].amp : TEMPLATES[t].flt;
    int32_t *a = env == 1 ? &q->attack : &q->f_attack;
    for (int i = 0; i < 4; i++) a[i] = v[i]; // attack, decay, sustain, release sit in that order
    if (env == 1) q->env1_tpl = t;
    else q->env2_tpl = t;
}

// An envelope template by name (MULTI's instrument templates set both): 1 when found.
int s600_env_template(struct s600_params *q, int env, const char *name) {
    for (int t = 0; t < TEMPLATE_COUNT; t++)
        if (!strcmp(TEMPLATES[t].name, name)) {
            apply_template(q, env, t);
            return 1;
        }
    return 0;
}

void s600_defaults(struct s600_params *q) {
    memset(q, 0, sizeof *q);
    apply_template(q, 1, DEFAULT_TEMPLATE);
    apply_template(q, 2, DEFAULT_TEMPLATE);
    q->filter = FILTER_MOOG; // S600 LPF, the Minimoog ladder
    q->cutoff = 100;
    q->lfo_shape = LFO_TRI;
    q->lfo_rate = 50;
    q->bend_up = 2;
    q->bend_down = 2;
    q->mod_pitch = 30;
    q->note_on = 1; // a synth note holds while the pad or key is held
    q->layers = 1;
    q->l2_vol = 100;
    q->root = 60;
}

void s600_effective(const struct os_pad *pads, int i, struct pad_params *out, const struct sample **s2) {
    const struct os_pad *pad = &pads[i];
    const struct s600_params *q = &pad->sp;
    *out = pad->p; // trim, loop, volume, pan and tuning are shared
    out->synth = 1;
    out->fine += q->root_fine; // in tune at its ROOT
    out->gate = q->note_on;
    out->decay_end = 0;
    out->attack_ms = q->attack;
    out->decay_ms = q->decay;
    out->sustain = q->sustain;
    out->release_ms = q->release;
    out->flt_attack_ms = q->f_attack;
    out->flt_decay_ms = q->f_decay;
    out->flt_sustain = q->f_sustain;
    out->flt_release_ms = q->f_release;
    out->flt_depth = q->f_depth;
    out->filter = q->filter;
    out->cutoff = q->cutoff;
    out->reso = q->reso;
    out->lfo_shape = q->lfo_shape;
    out->lfo_rate = q->lfo_rate;
    out->lfo_delay_ms = q->lfo_delay;
    out->lfo_pitch = q->lfo_pitch;
    out->lfo_filter = q->lfo_filter;
    out->lfo_amp = q->lfo_amp;
    out->mono = q->mono;
    out->legato = q->mono && q->legato;
    out->porta_ms = q->porta;
    out->bend_up = q->bend_up;
    out->bend_down = q->bend_down;
    out->mod_pitch = q->mod_pitch;
    // Layer 2: its own sample's trim and loop, its own level and tuning.
    const struct os_pad *src = q->l2_pad > 0 && q->l2_pad <= OS_PADS ? &pads[q->l2_pad - 1] : pad;
    out->layers = q->layers == 2 && src->s && !q->multi ? 2 : 1; // MULTI: its keygroups instead
    out->l2_volume = q->l2_vol;
    out->l2_semi = q->l2_semi;
    out->l2_fine = q->l2_fine;
    out->l2_start = src->p.start;
    out->l2_end = src->p.end;
    out->l2_loop = src->p.loop;
    out->l2_loop_on = src->p.loop_on;
    if (s2) *s2 = out->layers == 2 ? src->s : 0;
}

// ---- The S600 screen ----

enum { PG_ENV1, PG_ENV2, PG_FILTER, PG_LFO, PG_MAIN, PG_LAYER, PG_COUNT };
enum { V_MS, V_PCT, V_SEMI, V_CENTS, V_DEPTH, V_TPL1, V_TPL2, V_FILTER, V_SHAPE, V_POLY, V_ONOFF, V_SOUND, V_NUM, V_NOTE };

struct s6field {
    const char *label;
    int row, shared; // shared: one of the pad's own settings, not an S600 one
    size_t off;
    int min, max, fmt, knob; // knob: 0..2 for K1..K3, -1 none
};

#define S6(l, r, f, lo, hi, fm, k) {l, r, 0, offsetof(struct s600_params, f), lo, hi, fm, k}
#define SH(l, r, f, lo, hi, fm, k) {l, r, 1, offsetof(struct pad_params, f), lo, hi, fm, k}
#define S6_FIELDS 11
#define VALUE_COL 14
#define GRAPH_Y 68
#define GRAPH_H 72

static const struct s6field PAGES6[PG_COUNT][S6_FIELDS] = {
    [PG_ENV1] = {S6("Template", 3, env1_tpl, 0, TEMPLATE_COUNT - 1, V_TPL1, -1), S6("Attack", 10, attack, 0, 5000, V_MS, 0),
                 S6("Decay", 11, decay, 0, 9999, V_MS, 1), S6("Sustain", 12, sustain, 0, 100, V_PCT, 2),
                 S6("Release", 13, release, 0, 9999, V_MS, -1)},
    [PG_ENV2] = {S6("Template", 3, env2_tpl, 0, TEMPLATE_COUNT - 1, V_TPL2, -1),
                 S6("Attack", 10, f_attack, 0, 5000, V_MS, 0), S6("Decay", 11, f_decay, 0, 9999, V_MS, 1),
                 S6("Sustain", 12, f_sustain, 0, 100, V_PCT, 2), S6("Release", 13, f_release, 0, 9999, V_MS, -1)},
    [PG_FILTER] = {S6("Type", 3, filter, 0, FILTER_TYPES - 1, V_FILTER, -1), S6("Cutoff", 5, cutoff, 0, 100, V_PCT, 0),
                   S6("Reson", 6, reso, 0, 100, V_PCT, 1), S6("Env2 Depth", 7, f_depth, -100, 100, V_DEPTH, 2)},
    [PG_LFO] = {S6("Shape", 3, lfo_shape, 0, LFO_SHAPES - 1, V_SHAPE, -1), S6("Rate", 10, lfo_rate, 0, 100, V_NUM, 0),
                S6("Pitch", 11, lfo_pitch, 0, 100, V_NUM, 1), S6("Filter", 12, lfo_filter, 0, 100, V_NUM, 2),
                S6("Level", 13, lfo_amp, 0, 100, V_NUM, -1), S6("Delay", 9, lfo_delay, 0, 5000, V_MS, -1)},
    [PG_MAIN] = {SH("Volume", 3, volume, 0, 100, V_NUM, 0), SH("Semi", 4, semi, -36, 36, V_SEMI, 1),
                 SH("Fine", 5, fine, -100, 100, V_CENTS, 2), S6("Root", 6, root, 0, 127, V_NOTE, -1),
                 S6("Poly", 7, mono, 0, 1, V_POLY, -1),
                 S6("Note On", 8, note_on, 0, 1, V_ONOFF, -1), S6("Legato", 9, legato, 0, 1, V_ONOFF, -1),
                 S6("Portamento", 10, porta, 0, 5000, V_MS, -1), S6("Bend Up", 11, bend_up, 0, 24, V_SEMI, -1),
                 S6("Bend Down", 12, bend_down, 0, 24, V_SEMI, -1), S6("Mod>Pitch", 13, mod_pitch, 0, 100, V_NUM, -1)},
    // Layer 2's fields come last, so with one layer they are simply left off.
    [PG_LAYER] = {S6("Layers", 3, layers, 1, 2, V_NUM, -1), SH("Volume", 5, volume, 0, 100, V_NUM, -1),
                  SH("Semi", 6, semi, -36, 36, V_SEMI, -1), SH("Fine", 7, fine, -100, 100, V_CENTS, -1),
                  S6("Sound", 9, l2_pad, 0, OS_PADS, V_SOUND, -1), S6("Volume", 10, l2_vol, 0, 100, V_NUM, 0),
                  S6("Semi", 11, l2_semi, -36, 36, V_DEPTH, 1), S6("Fine", 12, l2_fine, -100, 100, V_CENTS, 2)},
};
#define LAYER1_FIELDS 4
static const char *const PAGE_NAMES[PG_COUNT] = {"Env1", "Env2", "Filter", "LFO", "Main", "Layer"};
static const char *const SHAPE_NAMES[LFO_SHAPES] = {"Triangle", "Sine", "Square", "Saw Up", "Saw Down", "Random"};
static const char *const FILTER_NAMES6[FILTER_TYPES] = {"Off", "LPF", "HPF", "BPF", "LPF 4", "HPF 4", "BPF 4", "S600 LPF"};

static int field_count(int page, const struct os_pad *pad) {
    int n = 0;
    while (n < S6_FIELDS && PAGES6[page][n].label) n++;
    if (page == PG_LAYER && pad->sp.layers < 2) n = LAYER1_FIELDS;
    return n;
}

static int32_t *field_ptr(struct os_pad *pad, const struct s6field *f) {
    char *base = f->shared ? (char *)&pad->p : (char *)&pad->sp;
    return (int32_t *)(void *)(base + f->off);
}

static void set_one(struct os *s, int slot, const struct s6field *f, int v) {
    struct os_pad *pad = &s->pads[slot];
    if (f->fmt == V_TPL1 || f->fmt == V_TPL2) apply_template(&pad->sp, f->fmt == V_TPL1 ? 1 : 2, v);
    else *field_ptr(pad, f) = v;
    os_pad_push(s, slot);
}

static void set_field(struct os *s, const struct s6field *f, int v) {
    if (!s->pads[s->selected].s) return;
    v = v < f->min ? f->min : v > f->max ? f->max : v;
    set_one(s, s->selected, f, v);
    if (s->edit_all) // Edit All Pads: the same on every pad with a sound
        for (int i = 0; i < OS_PADS; i++)
            if (i != s->selected && s->pads[i].s) set_one(s, i, f, v);
}

static void nudge_field(struct os *s, const struct s6field *f, int steps, int fine) {
    struct os_pad *pad = &s->pads[s->selected];
    if (!pad->s || !steps) return;
    int v = *field_ptr(pad, f), step = 1;
    if (f->fmt == V_MS && !fine) step = v < 100 ? 5 : v < 1000 ? 20 : 100;
    set_field(s, f, v + steps * step);
}

// Knob and fader positions 0..127. Times follow a curve, so short ones get most of the travel.
// Knob and slider moves on Cutoff, Reson, Volume, Semi and Fine are recorded into the sequence
// while it records, as on the Sample screen; played back, they move the S600 sound. pos 0..127.
static void record_move(struct os *s, const struct s6field *f, int pos) {
    int param = -1;
    if (!f->shared && f->off == offsetof(struct s600_params, cutoff)) param = AUTO_CUTOFF;
    else if (!f->shared && f->off == offsetof(struct s600_params, reso)) param = AUTO_RESO;
    else if (f->shared && f->off == offsetof(struct pad_params, volume)) param = AUTO_VOL;
    else if (f->shared && f->off == offsetof(struct pad_params, semi)) param = AUTO_SEMI;
    else if (f->shared && f->off == offsetof(struct pad_params, fine)) param = AUTO_FINE;
    if (param >= 0) seq_record_auto(&s->seq, s->selected, param, pos);
}

static int to_value(const struct s6field *f, int pos) {
    double x = pos / 127.0;
    if (f->fmt == V_MS) x = x * x;
    return f->min + (int)lround(x * (f->max - f->min));
}

static int to_position(const struct s6field *f, int v) {
    if (f->max <= f->min) return 0;
    double x = (double)(v - f->min) / (f->max - f->min);
    if (f->fmt == V_MS) x = sqrt(x);
    return (int)lround(x * 127);
}

static int catch_up(int *caught, int last, int now, int at) {
    if (!*caught) {
        int crossed = last >= 0 && ((last <= at && now >= at) || (last >= at && now <= at));
        if (crossed || (now - at <= 2 && at - now <= 2)) *caught = 1;
    }
    return *caught;
}

// Where the cursor starts on a page. Main's first three are on K1-K3, so it starts on Root;
// Filter starts on Cutoff.
static int first_field(int page) {
    return page == PG_MAIN ? 3 : page == PG_FILTER ? 1 : 0;
}

void s600_open(struct os *s) {
    s->screen = OS_S600;
    s->s600_cursor = first_field(s->s600_page);
    for (int i = 0; i < 3; i++) s->knob_caught[i] = 0;
    s->fader_caught = 0;
}

static void set_page(struct os *s, int page) {
    s->s600_page = page;
    s->s600_cursor = first_field(page);
    for (int i = 0; i < 3; i++) s->knob_caught[i] = 0;
    s->fader_caught = 0;
}

static int tpl_matches(const struct s600_params *q, int env) {
    int t = env == 1 ? q->env1_tpl : q->env2_tpl;
    const int *v = env == 1 ? TEMPLATES[t].amp : TEMPLATES[t].flt;
    const int32_t *a = env == 1 ? &q->attack : &q->f_attack;
    for (int i = 0; i < 4; i++)
        if (a[i] != v[i]) return 0;
    return 1;
}

static void format6(const struct os_pad *pads, const struct os_pad *pad, const struct s6field *f, int v, char *b, size_t n) {
    switch (f->fmt) {
    case V_MS:
        if (v == 0 && f->off == offsetof(struct s600_params, porta) && !f->shared) snprintf(b, n, "Off");
        else snprintf(b, n, "%dms", v);
        break;
    case V_PCT: snprintf(b, n, "%d", v); break; // no %: the S600 pages show plain numbers
    case V_SEMI: snprintf(b, n, f->shared ? "%+d" : "%d", v); break;
    case V_CENTS:
    case V_DEPTH: snprintf(b, n, "%+d", v); break;
    case V_TPL1:
    case V_TPL2: { // a * once the envelope has been changed from the template
        int env = f->fmt == V_TPL1 ? 1 : 2;
        snprintf(b, n, "%s%s", TEMPLATES[v].name, tpl_matches(&pad->sp, env) ? "" : "*");
        break;
    }
    case V_FILTER: snprintf(b, n, "%s", FILTER_NAMES6[v >= 0 && v < FILTER_TYPES ? v : 0]); break;
    case V_SHAPE: snprintf(b, n, "%s", SHAPE_NAMES[v >= 0 && v < LFO_SHAPES ? v : 0]); break;
    case V_POLY: snprintf(b, n, "%s", v ? "MONO" : "POLY"); break;
    case V_NOTE: multi_note_name(v, b, n); break; // ROOT: the key that plays it at its own pitch
    case V_ONOFF: snprintf(b, n, "%s", v ? "ON" : "OFF"); break;
    case V_SOUND:
        if (v <= 0) snprintf(b, n, "Same");
        else snprintf(b, n, "%c%02d %.7s", 'A' + (v - 1) / 16, (v - 1) % 16 + 1, pads[v - 1].s ? pads[v - 1].name : "empty");
        break;
    default: snprintf(b, n, "%d", v); break;
    }
}

// A line of LCD dots from (x0, y0) to (x1, y1), in screen pixels.
static void line(struct ui *u, int x0, int y0, int x1, int y1) {
    int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    steps = steps / 2 + 1;
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (x1 - x0) * i / steps, y = y0 + (y1 - y0) * i / steps;
        lcd_dot(u, x & ~1, y & ~1, LCD_FG);
    }
}

// The envelope's shape: attack, decay, a stretch held at the sustain level, release.
static void draw_env(struct ui *u, int a, int d, int sus, int r) {
    int hold = 400, total = a + d + hold + r;
    if (total < 1) total = 1;
    int x0 = 16, w = 288, top = GRAPH_Y, bottom = GRAPH_Y + GRAPH_H;
    int xa = x0 + a * w / total, xd = xa + d * w / total, xh = xd + hold * w / total, xr = x0 + w;
    int ys = bottom - sus * GRAPH_H / 100;
    line(u, x0, bottom, xa, top);
    line(u, xa, top, xd, ys);
    line(u, xd, ys, xh, ys);
    line(u, xh, ys, xr, bottom);
    for (int x = x0; x <= xr; x += 4) lcd_dot(u, x, bottom + 2, LCD_FG);
}

static void draw_lfo(struct ui *u, int shape) {
    int x0 = 16, w = 288, mid = GRAPH_Y + GRAPH_H / 2, amp = GRAPH_H / 2 - 2;
    int py = mid;
    uint32_t r = 12345;
    float held = 0;
    for (int x = 0; x <= w; x += 2) {
        float ph = (float)x / (w / 2.0f);
        ph -= floorf(ph);
        float v;
        switch (shape) {
        case LFO_SINE: v = sinf(2.0f * 3.14159265f * ph); break;
        case LFO_SQUARE: v = ph < 0.5f ? 1.0f : -1.0f; break;
        case LFO_SAW_UP: v = 2.0f * ph - 1.0f; break;
        case LFO_SAW_DOWN: v = 1.0f - 2.0f * ph; break;
        case LFO_RANDOM:
            if (x % 36 == 0) {
                r = r * 1664525u + 1013904223u;
                held = (float)(r >> 8) / (float)(1u << 24) * 2.0f - 1.0f;
            }
            v = held;
            break;
        default: v = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; break;
        }
        int y = mid - (int)(v * amp);
        if (x > 0) line(u, x0 + x - 2, py, x0 + x, y);
        py = y;
    }
}

static void draw_keys6(struct ui *u, const struct os *s) {
    static const int first[3] = {PG_ENV1, PG_FILTER, PG_MAIN};
    for (int g = 0; g < 3; g++) {
        int page = s->s600_page, in_group = g == 0 ? page <= PG_ENV2 : g == 1 ? page == PG_FILTER || page == PG_LFO : page >= PG_MAIN;
        char key[16];
        snprintf(key, sizeof key, "<%s>", PAGE_NAMES[in_group ? page : first[g]]);
        if (in_group) lcd_inverse(u, g * 9, KEY_ROW, key);
        else lcd_text(u, g * 9, KEY_ROW, key);
    }
}

void s600_frame(struct os *s, struct ui *u, const struct input *in) {
    struct os_pad *pad = &s->pads[s->selected];
    // An S600 MULTI program has no Layer page: its velocity layers are its keygroups' zones.
    int multi_prog = s->programs[s->program].s600 == PROG_MULTI;
    if (multi_prog && s->s600_page == PG_LAYER) s->s600_page = PG_MAIN;
    int page = s->s600_page, n = field_count(page, pad);
    if (s->s600_cursor >= n) s->s600_cursor = 0;
    // B1 Env1, Env2, Trim (the Sample screen's), B2 Filter/LFO, B3 Main/Layer. No way from here
    // to the other DRUM pages.
    if (input_down(in, IN_B1) && page == PG_ENV2) {
        os_open_s600_trim(s);
        ui_wake(u, 1); // drawn next frame
        return;
    }
    if (input_down(in, IN_B1)) set_page(s, page == PG_ENV1 ? PG_ENV2 : PG_ENV1);
    if (input_down(in, IN_B2)) set_page(s, page == PG_FILTER ? PG_LFO : PG_FILTER);
    if (input_down(in, IN_B3) && in->shift) { // Edit All Pads on or off
        s->edit_all = !s->edit_all;
        os_toast(s, u, s->edit_all ? "Edit All Pads: On" : "Edit All Pads: Off");
    } else if (input_down(in, IN_B3) && page == PG_MAIN && multi_prog) {
        // An S600 MULTI program: Main, then MULTI (an S600 program: Main and Layer); SAMPLE comes back to Main
        set_page(s, PG_MAIN);
        multi_open(s);
        ui_wake(u, 1);
        return;
    } else if (input_down(in, IN_B3)) {
        set_page(s, page == PG_MAIN ? PG_LAYER : PG_MAIN);
    }
    if (input_down(in, IN_CHOP) && in->shift && pad->s) { // as on the Sample screen
        pad->sp.note_on = !pad->sp.note_on;
        os_pad_push(s, s->selected);
        os_toast(s, u, pad->sp.note_on ? "Note On: On" : "Note On: Off");
    }
    page = s->s600_page;
    n = field_count(page, pad);
    const struct s6field *fields = PAGES6[page];
    if (in->encoder_press) s->s600_cursor = (s->s600_cursor + (in->shift ? n - 1 : 1)) % n;
    else nudge_field(s, &fields[s->s600_cursor], in->encoder, in->shift);
    if (!in->shift) nudge_field(s, &fields[s->s600_cursor], input_down(in, IN_PLUS) - input_down(in, IN_MINUS), 1);
    for (int k = 0; k < 3; k++) {
        if (!in->knob_moved[k]) continue;
        int last = s->knob_last[k], now = in->knob[k];
        s->knob_last[k] = now;
        for (int i = 0; i < n; i++) {
            if (fields[i].knob != k) continue;
            s->s600_cursor = i;
            if (pad->s && catch_up(&s->knob_caught[k], last, now, to_position(&fields[i], *field_ptr(pad, &fields[i]))))
                set_field(s, &fields[i], to_value(&fields[i], now)), record_move(s, &fields[i], now);
        }
    }
    if (in->fader_moved) { // the fader moves the framed field
        int last = s->fader_last, now = in->fader;
        s->fader_last = now;
        const struct s6field *f = &fields[s->s600_cursor];
        if (pad->s && catch_up(&s->fader_caught, last, now, to_position(f, *field_ptr(pad, f))))
            set_field(s, f, to_value(f, now)), record_move(s, f, now);
    }

    draw_keys6(u, s);
    if (in->shift) { // held: B3 turns Edit All Pads on or off
        lcd_fill(u, 0, KEY_ROW * CELL_H, UI_W, CELL_H, LCD_BG);
        lcd_text(u, 17, KEY_ROW, "<AllPads>");
    }
    char title[LCD_COLS + 8];
    if (s->edit_all) snprintf(title, sizeof title, "P%02d ALL S600 %s", s->program + 1, PAGE_NAMES[page]);
    else snprintf(title, sizeof title, "P%02d %c%02d S600 %s", s->program + 1, 'A' + s->selected / 16, s->selected % 16 + 1,
             PAGE_NAMES[page]);
    lcd_rule(u, TITLE_ROW, title);
    if (!pad->s) {
        lcd_center(u, 6, "Empty pad");
        lcd_center(u, 8, "SAMPLE SELECT loads WAV");
        lcd_rule(u, BOTTOM_ROW, s->edit_all ? "ALL PADS" : 0);
        return;
    }
    const struct s600_params *q = &pad->sp;
    if (page == PG_ENV1) draw_env(u, q->attack, q->decay, q->sustain, q->release);
    if (page == PG_ENV2) draw_env(u, q->f_attack, q->f_decay, q->f_sustain, q->f_release);
    if (page == PG_LFO) draw_lfo(u, q->lfo_shape);
    (void)0; // Main: the knobs' Volume, Semi, Fine, then Root and the rest
    if (page == PG_LAYER) {
        lcd_rule(u, 4, "Layer 1");
        if (pad->sp.layers == 2) lcd_rule(u, 8, "Layer 2");
    }
    if (page == PG_ENV1 || page == PG_ENV2) lcd_rule(u, 9, page == PG_ENV1 ? "Env1: Level" : "Env2: Filter");
    if (page == PG_FILTER) {
        snprintf(title, sizeof title, "Sample:%.18s", pad->name);
        lcd_text(u, 0, 10, title);
    }
    for (int i = 0; i < n; i++) {
        const struct s6field *f = &fields[i];
        char label[24], value[24], cell[LCD_COLS + 1];
        if (f->knob >= 0) snprintf(label, sizeof label, "K%d %s:", f->knob + 1, f->label);
        else snprintf(label, sizeof label, "   %s:", f->label);
        int col = VALUE_COL;
        if (f->row == 3 && f->knob < 0) { // the page's top field: no knob, so no K column
            snprintf(label, sizeof label, "%s:", f->label);
            col = (int)strlen(label);
        }
        lcd_text(u, 0, f->row, label);
        format6(s->pads, pad, f, *field_ptr(pad, f), value, sizeof value);
        snprintf(cell, sizeof cell, " %s ", value);
        if (col + (int)strlen(cell) > LCD_COLS) cell[LCD_COLS - col] = 0;
        if (i == s->s600_cursor) lcd_inverse(u, col, f->row, cell);
        else lcd_text(u, col, f->row, cell);
    }
    lcd_rule(u, BOTTOM_ROW, s->edit_all ? "ALL PADS" : 0);
}

// ---- 16 Levels ----

void levels_open(struct os *s) {
    if (!s->levels_on) { // opening the page turns 16 Levels on, with the last pad played
        s->levels_on = 1;
        if (!s->levels_pad_set) s->levels_pad = s->selected;
    }
    if (s->screen != OS_LEVELS) s->levels_return = s->screen;
    s->screen = OS_LEVELS;
    s->cursor_levels = 0;
}

static void pad_name(int pad, char *b, size_t n) {
    snprintf(b, n, "%c%02d", 'A' + pad / 16, pad % 16 + 1);
}

// The fields, by what they set, and the order each type shows them in: on TUNE the root pad
// second, since the sound is mostly picked with a pad anyway.
enum { LV_TYPE, LV_SOUND, LV_ON, LV_ROOT };
static const int LV_ORDER[2][4] = {{LV_TYPE, LV_SOUND, LV_ON, -1}, {LV_TYPE, LV_ROOT, LV_SOUND, LV_ON}};

void levels_frame(struct os *s, struct ui *u, const struct input *in) {
    int tune = s->levels_type == LEVELS_TUNE, n = tune ? 4 : 3;
    if (s->cursor_levels >= n) s->cursor_levels = 0;
    if (input_down(in, IN_B1)) s->levels_type = LEVELS_VELOCITY;
    if (input_down(in, IN_B2)) s->levels_type = LEVELS_TUNE;
    if (input_down(in, IN_B3)) {
        s->screen = s->levels_return;
        ui_wake(u, 1);
        return;
    }
    tune = s->levels_type == LEVELS_TUNE;
    n = tune ? 4 : 3;
    if (in->encoder_press) s->cursor_levels = (s->cursor_levels + (in->shift ? n - 1 : 1)) % n;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d) {
        switch (LV_ORDER[tune][s->cursor_levels]) {
        case LV_TYPE: s->levels_type = d > 0 ? LEVELS_TUNE : LEVELS_VELOCITY; break;
        case LV_ON: s->levels_on = d > 0; break;
        case LV_SOUND:
            s->levels_pad = s->levels_pad + d < 0 ? 0 : s->levels_pad + d >= OS_PADS ? OS_PADS - 1 : s->levels_pad + d;
            s->levels_pad_set = 1;
            break;
        case LV_ROOT: s->levels_root = s->levels_root + d < 0 ? 0 : s->levels_root + d >= OS_PADS ? OS_PADS - 1 : s->levels_root + d; break;
        }
    }
    tune = s->levels_type == LEVELS_TUNE;
    lcd_text(u, 0, KEY_ROW, "<Velocity>");
    lcd_text(u, 11, KEY_ROW, "<Tune>");
    lcd_text(u, 20, KEY_ROW, "<Exit>");
    if (tune) lcd_inverse(u, 11, KEY_ROW, "<Tune>");
    else lcd_inverse(u, 0, KEY_ROW, "<Velocity>");
    lcd_rule(u, TITLE_ROW, "16 Levels");
    char v[4][32], b[LCD_COLS + 8], p[8];
    // Type first: LEVELS itself already turns 16 Levels on and off.
    snprintf(v[0], sizeof v[0], " %s ", tune ? "TUNE" : "VELOCITY");
    pad_name(s->levels_pad, p, sizeof p);
    snprintf(v[1], sizeof v[1], " %s ", p);
    snprintf(v[2], sizeof v[2], " %s ", s->levels_on ? "ON" : "OFF");
    pad_name(s->levels_root, p, sizeof p);
    snprintf(v[3], sizeof v[3], " %s ", p);
    static const char *const labels[4] = {"Type:", "Sound:", "16 Levels:", "Root pad:"};
    static const int cols[4] = {5, 6, 10, 9};
    int sound_row = 4;
    for (int i = 0; i < n; i++) {
        int f = LV_ORDER[tune][i];
        lcd_text(u, 0, 3 + i, labels[f]);
        if (i == s->cursor_levels) lcd_inverse(u, cols[f], 3 + i, v[f]);
        else lcd_text(u, cols[f], 3 + i, v[f]);
        if (f == LV_SOUND) sound_row = 3 + i;
    }
    // Switching type keeps the cursor on the same field, wherever it now shows.
    const struct os_pad *sound = &s->pads[s->levels_pad];
    snprintf(b, sizeof b, "%.15s", sound->s ? sound->name : "(empty)");
    lcd_text(u, 11, sound_row, b);
    if (tune) {
        int lo = -s->levels_root, hi = OS_PADS - 1 - s->levels_root;
        lo = lo < -64 ? -64 : lo;
        hi = hi > 63 ? 63 : hi;
        snprintf(b, sizeof b, "Range: %+d to %+d semis", lo, hi);
        lcd_text(u, 0, 8, b);
        lcd_text(u, 0, 10, "MIDI IN: middle C is root");
    } else {
        lcd_text(u, 0, 8, "Pad 1 lowest velocity,");
        lcd_text(u, 0, 9, "pad 16 highest velocity");
        lcd_text(u, 0, 11, "Press pad to select sound");
    }
    lcd_rule(u, BOTTOM_ROW, 0);
}
