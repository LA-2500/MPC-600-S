// MULTI: an S600 pad as a multisample instrument, after the S-series samplers (the S3000XL
// most of all), streamlined for playing instruments. The pad's keygroups spread other pads'
// samples across the keyboard, each with up to two velocity zones; the S600 envelopes, filter
// and LFO are the instrument's own and shared by every keygroup. Key tracking (KEY>PAN,
// KEY>FILTER, KEY>LEVEL, KEY>ENV) moves the sound with the note, and instrument templates set
// it all up for a kind of instrument. AUTO MAP builds the keygroups from pads, their roots from
// the sample names and the pitch heard in them.
//
// The MULTI screen (B3 from the S600 Layer page) works on the program's Instrument (Program
// Config, Type S600 MULTI): B1 KEYGROUP / KEY TRACK, B2 the Auto Sampler (SHIFT: <Remove>),
// B3 <Auto Map> (SHIFT: <Add Pad>).
#include "lcd.h"
#include "os.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MP_KG, MP_KT, MP_MAP, MP_PAGES };

// ---- Notes ----

static const char *const NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

void multi_note_name(int note, char *b, size_t n) {
    if (note < 0 || note > 127) {
        snprintf(b, n, "---");
        return;
    }
    snprintf(b, n, "%s%d", NOTE_NAMES[note % 12], note / 12 - 1); // 60 = C4
}

// A note named in a sample's name, as "Piano C3", "pno_F#2", "Rhodes-Bb1" or "Bass E-1": a
// note letter not inside a word, a sharp or flat, an octave. The last such in the name. 60 = C4.
int multi_name_note(const char *name, int *pitch_class) {
    static const int PC[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    int found = -1, pc_found = -1;
    for (int i = 0; name[i]; i++) {
        char c = (char)toupper((unsigned char)name[i]);
        if (c < 'A' || c > 'G') continue;
        if (i > 0 && isalpha((unsigned char)name[i - 1])) continue; // inside a word ("Bass", "Grand")
        int j = i + 1, pc = PC[c - 'A']; // Cb is B below, B# is C above: pc -1 or 12 crosses the octave
        if (name[j] == '#' || name[j] == 's') pc++, j++;
        else if (name[j] == 'b') pc--, j++;
        int neg = 0;
        if (name[j] == '-' && isdigit((unsigned char)name[j + 1])) neg = 1, j++;
        if (!isdigit((unsigned char)name[j])) continue;
        if (isdigit((unsigned char)name[j + 1])) continue; // "C10": a number, not a note ("C4v2" is a note)
        int oct = neg ? -(name[j] - '0') : name[j] - '0';
        int note = (oct + 1) * 12 + pc;
        pc = (pc + 12) % 12;
        if (note >= 0 && note <= 127) found = note, pc_found = pc;
    }
    if (pitch_class) *pitch_class = pc_found;
    return found;
}

// ---- Pitch detection ----

// The pitch heard in a sample, as a MIDI note with its cents (60.12 is C4, 12 cents sharp):
// the YIN method on a stretch after the attack. 0 when no steady pitch is found (a drum, noise).
int multi_detect_pitch(const struct sample *smp, double *note) {
    if (!smp || smp->frames < 2048 || smp->rate < 4000) return 0;
    int ch = smp->channels, rate = smp->rate;
    long n = smp->frames;
    float peak = 0;
    for (long i = 0; i < n; i++) peak = fabsf(smp->data[i * ch]) > peak ? fabsf(smp->data[i * ch]) : peak;
    if (peak < 1e-4f) return 0;
    long start = 0; // past the attack: where it first gets loud, and 60 ms on
    while (start < n && fabsf(smp->data[start * ch]) < peak * 0.1f) start++;
    start += rate * 60 / 1000;
    int w = rate / 10 > 4096 ? 4096 : rate / 10; // the window: up to 0.1 s
    int tmax = rate / 27; // down to A0
    int tmin = rate / 4200;
    if (tmin < 2) tmin = 2;
    if (start + w + tmax >= n) start = n - w - tmax - 1;
    if (start < 0) {
        start = 0;
        tmax = (int)(n - w - 1);
        if (tmax <= tmin * 2) return 0;
    }
    float *x = malloc((size_t)(w + tmax + 1) * sizeof *x);
    double *d = malloc((size_t)(tmax + 2) * sizeof *d);
    if (!x || !d) {
        free(x), free(d);
        return 0;
    }
    for (int i = 0; i < w + tmax + 1; i++) {
        const float *f = smp->data + (start + i) * ch;
        x[i] = ch == 2 ? 0.5f * (f[0] + f[1]) : f[0];
    }
    d[0] = 1;
    double run = 0;
    int tau = -1;
    for (int t = 1; t <= tmax; t++) {
        double sum = 0;
        for (int i = 0; i < w; i++) {
            double v = x[i] - x[i + t];
            sum += v * v;
        }
        run += sum;
        d[t] = run > 0 ? sum * t / run : 1; // the cumulative mean normalized difference
    }
    for (int t = tmin; t < tmax && tau < 0; t++)
        if (d[t] < 0.15) {
            while (t + 1 < tmax && d[t + 1] < d[t]) t++;
            tau = t;
        }
    if (tau < 0) { // none under the threshold: the deepest dip, if it is clear enough
        int best = tmin;
        for (int t = tmin; t < tmax; t++)
            if (d[t] < d[best]) best = t;
        if (d[best] < 0.35) tau = best;
    }
    double t = tau;
    if (tau > 0 && tau < tmax) { // between samples: the parabola through the dip
        double a = d[tau - 1], b = d[tau], c = d[tau + 1], den = a - 2 * b + c;
        if (fabs(den) > 1e-12) t = tau + 0.5 * (a - c) / den;
    }
    free(x), free(d);
    if (tau < 0 || t <= 0) return 0;
    double hz = rate / t;
    *note = 69.0 + 12.0 * log2(hz / 440.0);
    return *note >= 12 && *note <= 120;
}

static double rms_of(const struct sample *smp) {
    double sum = 0;
    long n = (long)smp->frames * smp->channels;
    for (long i = 0; i < n; i++) sum += (double)smp->data[i] * smp->data[i];
    return n ? sqrt(sum / n) : 0;
}

// ---- Instrument templates ----

// Each sets the envelopes, filter, voice and key tracking for a kind of instrument. Pan follows
// the player's view: low notes left, high right. Filter tracking keeps the brightness even
// across the keys; level and envelope tracking let high notes ring shorter and softer, as
// strings and hammers do.
static const struct inst_tpl {
    const char *name, *env1, *env2;
    int filter, cutoff, reso, note_on, mono, legato;
    int kt_pan, kt_filter, kt_level, kt_env;
} INST[] = {
    {"Flat", "MPC Bass", "MPC Bass", FILTER_LP2, 100, 0, 1, 0, 0, 0, 0, 0, 0},
    {"Piano", "Piano", "Piano", FILTER_LP2, 85, 0, 1, 0, 0, 20, 50, -20, 60},
    {"Elec. Piano", "Elec. Piano", "Elec. Piano", FILTER_LP2, 80, 10, 1, 0, 0, 15, 40, -10, 40},
    {"Organ", "Elec. Organ", "Elec. Organ", FILTER_LP2, 100, 0, 1, 0, 0, 10, 0, -10, 0},
    {"Strings", "String/Vox", "String/Vox", FILTER_LP2, 80, 0, 1, 0, 0, 25, 30, 0, 0},
    {"Choir", "String/Vox", "String/Vox", FILTER_LP2, 75, 0, 1, 0, 0, 10, 20, 0, 0},
    {"Brass", "Brass", "Brass", FILTER_LP2, 70, 10, 1, 0, 0, 10, 40, 0, 0},
    {"Woodwind", "Woodwind", "Woodwind", FILTER_LP2, 80, 0, 1, 0, 0, 5, 30, 0, 0},
    {"Guitar", "Guitar/Bass", "Guitar/Bass", FILTER_LP2, 85, 0, 1, 0, 0, 10, 40, -10, 50},
    {"Bass", "Guitar/Bass", "Guitar/Bass", FILTER_LP2, 80, 0, 1, 0, 0, 0, 30, 0, 30},
    {"Synth Pad", "Slow String", "Slow String", FILTER_MOOG, 70, 20, 1, 0, 0, 20, 30, 0, 0},
    {"Synth Lead", "Synth Lead", "Synth Lead", FILTER_MOOG, 60, 30, 1, 1, 1, 0, 50, 0, 0},
    {"Mallets", "Tuned Perc", "Tuned Perc", FILTER_LP2, 90, 0, 1, 0, 0, 20, 40, -10, 70},
    {"Bells", "Bell", "Bell", FILTER_LP2, 100, 0, 1, 0, 0, 15, 0, -10, 50},
};
#define INST_COUNT ((int)(sizeof INST / sizeof INST[0]))

static void apply_inst(struct s600_params *q, int t) {
    const struct inst_tpl *k = &INST[t];
    s600_env_template(q, 1, k->env1);
    s600_env_template(q, 2, k->env2);
    q->filter = k->filter, q->cutoff = k->cutoff, q->reso = k->reso;
    q->note_on = k->note_on, q->mono = k->mono, q->legato = k->legato;
    q->kt_pan = k->kt_pan, q->kt_filter = k->kt_filter, q->kt_level = k->kt_level, q->kt_env = k->kt_env;
    q->inst_tpl = t;
}

static int inst_matches(const struct s600_params *q) {
    const struct inst_tpl *k = &INST[q->inst_tpl >= 0 && q->inst_tpl < INST_COUNT ? q->inst_tpl : 0];
    return q->kt_pan == k->kt_pan && q->kt_filter == k->kt_filter && q->kt_level == k->kt_level && q->kt_env == k->kt_env &&
           q->filter == k->filter && q->cutoff == k->cutoff && q->reso == k->reso;
}

int multi_template_count(void) {
    return INST_COUNT;
}

const char *multi_template_name(int t) {
    return INST[t >= 0 && t < INST_COUNT ? t : 0].name;
}

void multi_apply_template(struct os *s, int pad, int t) {
    if (t < 0 || t >= INST_COUNT) return;
    apply_inst(&s->pads[pad].sp, t);
    os_pad_push(s, pad);
}

// ---- Keygroups ----

static void kg_default(struct s600_kg *g, int lo, int hi, int root, int pad) {
    memset(g, 0, sizeof *g);
    g->lo = lo, g->hi = hi, g->root = root, g->level = 100, g->split = 64;
    g->zone[0] = pad + 1;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int kg_root_cmp(const void *a, const void *b) {
    const struct s600_kg *x = a, *y = b;
    return x->root - y->root;
}

// Ranges AUTO: in root order, each keygroup reaching midway to its neighbours; the ends to the
// ends of the keyboard (Extend last note ON) or stopping at the end roots.
void multi_respan(struct os_pad *p, int extend) {
    if (p->kgs.n < 1) return;
    qsort(p->kgs.kg, (size_t)p->kgs.n, sizeof p->kgs.kg[0], kg_root_cmp);
    for (int k = 0; k < p->kgs.n; k++) {
        struct s600_kg *g = &p->kgs.kg[k];
        g->lo = k ? (p->kgs.kg[k - 1].root + g->root) / 2 + 1 : (extend ? 0 : g->root);
        g->hi = k + 1 < p->kgs.n ? (g->root + p->kgs.kg[k + 1].root) / 2 : (extend ? 127 : g->root);
        if (g->lo > g->hi) g->lo = g->hi; // two on one root: the second only from the first's top
    }
    p->sp.ranges_manual = 0;
}

int multi_kg_at(const struct os_pad *p, int note) {
    for (int k = 0; k < p->kgs.n; k++)
        if (note >= p->kgs.kg[k].lo && note <= p->kgs.kg[k].hi) return k;
    return -1;
}

static int is_multi_program(const struct os *s) {
    return s->programs[s->program].s600 == PROG_MULTI;
}

// The instrument a MULTI screen works on, and (it has none yet) its first keygroup: its own
// sound across the keyboard, at C4.
static struct os_pad *instrument(struct os *s, int *at) {
    int i = os_instrument(s, s->program);
    struct os_pad *p = &s->pads[i];
    if (p->s && p->kgs.n == 0) {
        p->kgs.n = 1;
        kg_default(&p->kgs.kg[0], 0, 127, 60, i);
        os_pad_push(s, i);
    }
    if (at) *at = i;
    return p;
}

// <Add Pad>: the pad last pressed into the instrument, at the root its name and pitch give (C4 if
// neither does). A root already there with no Zone 2 takes it as its Zone 2.
static void kg_add_pad(struct os *s, struct ui *u) {
    int inst, pad = s->multi_pressed;
    struct os_pad *p = instrument(s, &inst);
    char msg[48];
    if (pad < 0 || pad >= OS_PADS || !s->pads[pad].s) {
        os_toast(s, u, "Press a pad with a sound first");
        return;
    }
    for (int k = 0; k < p->kgs.n; k++)
        if (p->kgs.kg[k].zone[0] == pad + 1 || p->kgs.kg[k].zone[1] == pad + 1) {
            snprintf(msg, sizeof msg, "Pad %c%02d is in KG %d", 'A' + pad / 16, pad % 16 + 1, k + 1);
            os_toast(s, u, msg);
            s->multi_kg = k;
            return;
        }
    int fine, root = multi_root_of(&s->pads[pad], 0, &fine), found = root >= 0;
    if (!found) root = 60, fine = 0;
    os_undo_save(s, "Add Pad");
    int k = -1;
    for (int i = 0; i < p->kgs.n && k < 0; i++)
        if (p->kgs.kg[i].root == root && !p->kgs.kg[i].zone[1] && p->kgs.kg[i].zone[0] != inst + 1) k = i;
    char nn[8];
    multi_note_name(root, nn, sizeof nn);
    if (k >= 0) { // its loud zone
        p->kgs.kg[k].zone[1] = pad + 1;
        snprintf(msg, sizeof msg, "%c%02d: Zone 2 of %s", 'A' + pad / 16, pad % 16 + 1, nn);
    } else if (p->kgs.n == 1 && p->kgs.kg[0].zone[0] == inst + 1 && p->kgs.kg[0].root == 60 && p->kgs.kg[0].lo == 0 &&
               p->kgs.kg[0].hi == 127 && pad != inst && multi_root_of(&s->pads[inst], 0, &(int){0}) < 0) {
        kg_default(&p->kgs.kg[0], root, root, root, pad); // the starting keygroup, never set: replaced
        p->kgs.kg[0].fine = fine;
        k = 0;
        snprintf(msg, sizeof msg, "%c%02d at %s", 'A' + pad / 16, pad % 16 + 1, nn);
    } else if (p->kgs.n >= S600_KGS) {
        os_toast(s, u, "32 keygroups is the most");
        return;
    } else {
        k = p->kgs.n++;
        kg_default(&p->kgs.kg[k], root, root, root, pad);
        p->kgs.kg[k].fine = fine;
        snprintf(msg, sizeof msg, found ? "%c%02d at %s" : "%c%02d: no pitch, at %s", 'A' + pad / 16, pad % 16 + 1, nn);
    }
    if (!p->sp.ranges_manual) multi_respan(p, !s->map_exact);
    os_pad_push(s, inst);
    s->multi_kg = multi_kg_at(p, root) >= 0 ? multi_kg_at(p, root) : 0;
    os_toast(s, u, msg);
}

static void kg_remove(struct os *s, struct ui *u) {
    int inst;
    struct os_pad *p = instrument(s, &inst);
    if (p->kgs.n <= 1) {
        os_toast(s, u, "The last keygroup stays");
        return;
    }
    os_undo_save(s, "Remove Keygroup");
    int at = clampi(s->multi_kg, 0, p->kgs.n - 1);
    memmove(&p->kgs.kg[at], &p->kgs.kg[at + 1], (size_t)(p->kgs.n - at - 1) * sizeof p->kgs.kg[0]);
    p->kgs.n--;
    if (!p->sp.ranges_manual) multi_respan(p, !s->map_exact);
    if (s->multi_kg >= p->kgs.n) s->multi_kg = p->kgs.n - 1;
    os_pad_push(s, inst);
    os_toast(s, u, "Keygroup removed");
}

// ---- AUTO MAP ----

enum { ROOT_NAME_PITCH, ROOT_NAME, ROOT_PITCH, ROOT_BYS };
enum { SPAN_MIDWAY, SPAN_UP, SPAN_DOWN, SPANS };
static const char *const ROOT_BY[ROOT_BYS] = {"NAME+PITCH", "NAME", "PITCH"};
static const char *const SPAN[SPANS] = {"MIDWAY", "ROOT UP", "ROOT DOWN"};

struct found {
    int pad, root, fine;
    double rms;
};

static int found_cmp(const void *a, const void *b) {
    const struct found *x = a, *y = b;
    if (x->root != y->root) return x->root - y->root;
    return x->rms < y->rms ? -1 : x->rms > y->rms;
}

// Where a sample's root is: from its name, the pitch heard in it, or both (the name's note,
// in the octave nearest the pitch heard: sample sets number their octaves either way). The
// pitch heard also gives the fine tuning that puts it in tune. -1 when neither tells.
int multi_root_of(const struct os_pad *p, int by, int *fine) {
    int pc, named = multi_name_note(p->name, &pc);
    double heard = 0;
    int got = by != ROOT_NAME && multi_detect_pitch(p->s, &heard);
    *fine = 0;
    int root = -1;
    if (by == ROOT_NAME) root = named;
    else if (by == ROOT_PITCH) root = got ? (int)lround(heard) : -1;
    else if (named >= 0 && got) { // the name's note, in the octave nearest what is heard
        root = named;
        while (root - heard > 6) root -= 12;
        while (heard - root > 6) root += 12;
    } else {
        root = named >= 0 ? named : got ? (int)lround(heard) : -1;
    }
    if (root < 0 || root > 127) return -1;
    if (got && fabs(heard - root) < 0.5) *fine = (int)lround((root - heard) * 100); // tuned to the root
    return root;
}

// The instrument's keygroups from every pad of the program with a sound: a keygroup at each
// root, spread to meet its neighbours (MIDWAY), or from its root up to the next (ROOT UP) or
// down from it (ROOT DOWN); the ends to the ends of the keyboard (extend). With 2 ZONES, two
// samples on one root are its soft and loud zones, the quieter one soft. Returns how many
// keygroups, or -1.
int multi_auto_map(struct os *s, int inst, int from, int to, int by, int span, int zones, int extend, int *skipped) {
    struct found f[OS_PADS];
    int n = 0;
    *skipped = 0;
    if (from > to) {
        int t = from;
        from = to, to = t;
    }
    for (int i = from; i <= to; i++) {
        const struct os_pad *p = &s->pads[i];
        if (!p->s) continue;
        int fine, root = multi_root_of(p, by, &fine);
        if (root < 0) {
            (*skipped)++;
            continue;
        }
        f[n++] = (struct found){i, root, fine, rms_of(p->s)};
    }
    if (n == 0) return -1;
    qsort(f, (size_t)n, sizeof f[0], found_cmp);
    struct s600_kgs kgs;
    memset(&kgs, 0, sizeof kgs);
    for (int i = 0; i < n && kgs.n < S600_KGS; i++) {
        struct s600_kg *g = kgs.n ? &kgs.kg[kgs.n - 1] : 0;
        if (g && g->root == f[i].root) { // the same root again: its loud zone, or left out
            if (zones == 2) g->zone[1] = f[i].pad + 1; // sorted quietest first: the loudest ends here
            continue;
        }
        g = &kgs.kg[kgs.n++];
        kg_default(g, f[i].root, f[i].root, f[i].root, f[i].pad);
        g->fine = f[i].fine;
    }
    for (int k = 0; k < kgs.n; k++) { // the spans
        struct s600_kg *g = &kgs.kg[k];
        int prev = k ? kgs.kg[k - 1].root : -1, next = k + 1 < kgs.n ? kgs.kg[k + 1].root : 128;
        if (span == SPAN_MIDWAY) g->lo = k ? (prev + g->root) / 2 + 1 : g->root, g->hi = k + 1 < kgs.n ? (g->root + next) / 2 : g->root;
        else if (span == SPAN_UP) g->lo = g->root, g->hi = k + 1 < kgs.n ? next - 1 : g->root;
        else g->lo = k ? prev + 1 : g->root, g->hi = g->root;
    }
    if (extend && kgs.n) kgs.kg[0].lo = 0, kgs.kg[kgs.n - 1].hi = 127;
    os_undo_save(s, "Auto Map");
    struct os_pad *p = &s->pads[inst];
    p->kgs = kgs;
    p->sp.ranges_manual = span != SPAN_MIDWAY; // AUTO keeps them midway; the others are set
    os_pad_push(s, inst);
    return kgs.n;
}

// ---- The MULTI screen ----

enum {
    MF_KG, MF_RANGES, MF_ROOT, MF_LEVEL, MF_LO, MF_HI, MF_Z1, MF_Z2, MF_Z2VEL, MF_TUNE, MF_FINE, MF_PAN, // KEYGROUP
    MF_TPL, MF_KPAN, MF_KFLT, MF_KLVL, MF_KENV,                                                        // KEY TRACK
    MF_ROOTBY, MF_SPAN, MF_VEL, MF_ENDS,                                                               // AUTO MAP
};
struct mfield {
    int kind, row, lcol, vcol;
    const char *label;
};
static const struct mfield KG_FIELDS[] = {
    {MF_KG, 3, 0, 3, "KG:"},          {MF_RANGES, 3, 12, 19, "Ranges:"}, {MF_ROOT, 4, 0, 5, "Root:"},
    {MF_LEVEL, 4, 12, 18, "Level:"},  {MF_LO, 5, 0, 3, "Lo:"},           {MF_HI, 5, 12, 15, "Hi:"},
    {MF_Z1, 6, 0, 7, "Zone 1:"},      {MF_Z2, 7, 0, 7, "Zone 2:"},       {MF_Z2VEL, 8, 0, 21, "Zone 2 From Velocity:"},
    {MF_TUNE, 9, 0, 5, "Tune:"},      {MF_FINE, 9, 12, 17, "Fine:"},     {MF_PAN, 10, 0, 4, "Pan:"},
};
static const struct mfield KT_FIELDS[] = {
    {MF_TPL, 3, 0, 9, "Template:"},   {MF_KPAN, 5, 0, 11, "KEY>PAN:"}, {MF_KFLT, 6, 0, 11, "KEY>FILTER:"},
    {MF_KLVL, 7, 0, 11, "KEY>LEVEL:"}, {MF_KENV, 8, 0, 11, "KEY>ENV:"},
};
static const struct mfield MAP_FIELDS[] = {
    {MF_ROOTBY, 5, 0, 11, "Root From:"}, {MF_SPAN, 6, 0, 11, "Span:"}, {MF_VEL, 7, 0, 11, "Velocity:"},
    {MF_ENDS, 8, 0, 18, "Extend Last Note:"},
};
#define COUNT(a) ((int)(sizeof a / sizeof a[0]))

// The page's fields now: Zone 2 From Velocity only with a Zone 2.
static int page_fields(const struct os *s, int page, struct mfield *out) {
    const struct mfield *f = page == MP_KT ? KT_FIELDS : page == MP_MAP ? MAP_FIELDS : KG_FIELDS;
    int n = page == MP_KT ? COUNT(KT_FIELDS) : page == MP_MAP ? COUNT(MAP_FIELDS) : COUNT(KG_FIELDS), m = 0;
    const struct os_pad *p = &s->pads[os_instrument(s, s->program)];
    const struct s600_kg *g = p->kgs.n ? &p->kgs.kg[clampi(s->multi_kg, 0, p->kgs.n - 1)] : 0;
    for (int i = 0; i < n; i++)
        if (f[i].kind != MF_Z2VEL || (g && g->zone[1] > 0)) out[m++] = f[i];
    return m;
}

void multi_open(struct os *s) {
    s->screen = OS_MULTI;
    if (s->multi_page == MP_MAP) s->multi_page = MP_KG;
    s->multi_cursor = 0;
    s->multi_pressed = -1;
    const struct os_pad *p = &s->pads[os_instrument(s, s->program)];
    if (s->multi_kg >= p->kgs.n) s->multi_kg = 0;
}

static void pad_label(const struct os *s, int z, char *b, size_t n) {
    if (z <= 0 || z > OS_PADS) snprintf(b, n, "---");
    else snprintf(b, n, "%c%02d %.13s", 'A' + (z - 1) / 16, (z - 1) % 16 + 1, s->pads[z - 1].s ? s->pads[z - 1].name : "(empty)");
}

static void change(struct os *s, int kind, int d) {
    int inst;
    struct os_pad *p = instrument(s, &inst);
    struct s600_kg *g = p->kgs.n ? &p->kgs.kg[clampi(s->multi_kg, 0, p->kgs.n - 1)] : 0;
    int respan = 0;
    switch (kind) {
    case MF_KG: s->multi_kg = clampi(s->multi_kg + d, 0, p->kgs.n - 1); return;
    case MF_ROOTBY: s->map_by = clampi(s->map_by + d, 0, ROOT_BYS - 1); return;
    case MF_SPAN: s->map_span = clampi(s->map_span + d, 0, SPANS - 1); return;
    case MF_VEL: s->map_zones = d > 0 ? 2 : 1; return;
    case MF_ENDS: s->map_exact = d < 0; return;
    case MF_TPL: multi_apply_template(s, inst, clampi(p->sp.inst_tpl + d, 0, INST_COUNT - 1)); return;
    case MF_RANGES:
        p->sp.ranges_manual = d < 0;
        respan = !p->sp.ranges_manual;
        break;
    case MF_KPAN: p->sp.kt_pan = clampi(p->sp.kt_pan + d, -50, 50); break;
    case MF_KFLT: p->sp.kt_filter = clampi(p->sp.kt_filter + d, 0, 100); break;
    case MF_KLVL: p->sp.kt_level = clampi(p->sp.kt_level + d, -100, 100); break;
    case MF_KENV: p->sp.kt_env = clampi(p->sp.kt_env + d, 0, 100); break;
    default:
        if (!g) return;
        switch (kind) {
        case MF_LO: g->lo = clampi(g->lo + d, 0, g->hi), p->sp.ranges_manual = 1; break;
        case MF_HI: g->hi = clampi(g->hi + d, g->lo, 127), p->sp.ranges_manual = 1; break;
        case MF_ROOT: g->root = clampi(g->root + d, 0, 127), respan = !p->sp.ranges_manual; break;
        case MF_Z2VEL: g->split = clampi(g->split + d, 1, 126); break;
        case MF_Z1: g->zone[0] = clampi(g->zone[0] + d, 1, OS_PADS); break;
        case MF_Z2: g->zone[1] = clampi(g->zone[1] + d, 0, OS_PADS); break;
        case MF_TUNE: g->semi = clampi(g->semi + d, -36, 36); break;
        case MF_FINE: g->fine = clampi(g->fine + d, -99, 99); break;
        case MF_LEVEL: g->level = clampi(g->level + d, 0, 100); break;
        case MF_PAN: g->pan = clampi(g->pan + d, -50, 50); break;
        }
    }
    if (respan) { // the keygroup keeps the cursor as the order changes
        struct s600_kg keep = *g;
        multi_respan(p, !s->map_exact);
        for (int k = 0; k < p->kgs.n; k++)
            if (!memcmp(&p->kgs.kg[k].root, &keep.root, sizeof(int32_t)) && p->kgs.kg[k].zone[0] == keep.zone[0]) s->multi_kg = k;
    }
    os_pad_push(s, inst);
}

static void value(const struct os *s, int kind, char *b, size_t n) {
    const struct os_pad *p = &s->pads[os_instrument(s, s->program)];
    const struct s600_kg *g = p->kgs.n ? &p->kgs.kg[clampi(s->multi_kg, 0, p->kgs.n - 1)] : 0;
    char t[24];
    switch (kind) {
    case MF_KG: snprintf(b, n, " %02d/%02d ", p->kgs.n ? s->multi_kg + 1 : 0, p->kgs.n); return;
    case MF_RANGES: snprintf(b, n, " %s ", p->sp.ranges_manual ? "MANUAL" : "AUTO"); return;
    case MF_ROOTBY: snprintf(b, n, " %s ", ROOT_BY[s->map_by]); return;
    case MF_SPAN: snprintf(b, n, " %s ", SPAN[s->map_span]); return;
    case MF_VEL: snprintf(b, n, " %s ", s->map_zones == 2 ? "2 ZONES" : "1 ZONE"); return;
    case MF_ENDS: snprintf(b, n, " %s ", s->map_exact ? "OFF" : "ON"); return;
    case MF_TPL: snprintf(b, n, " %s%s ", INST[clampi(p->sp.inst_tpl, 0, INST_COUNT - 1)].name, inst_matches(&p->sp) ? "" : "*"); return;
    case MF_KPAN: snprintf(b, n, " %+d ", p->sp.kt_pan); return;
    case MF_KFLT: snprintf(b, n, " %d ", p->sp.kt_filter); return;
    case MF_KLVL: snprintf(b, n, " %+d ", p->sp.kt_level); return;
    case MF_KENV: snprintf(b, n, " %d ", p->sp.kt_env); return;
    }
    if (!g) {
        snprintf(b, n, " - ");
        return;
    }
    switch (kind) {
    case MF_LO: multi_note_name(g->lo, t, sizeof t), snprintf(b, n, " %s ", t); break;
    case MF_HI: multi_note_name(g->hi, t, sizeof t), snprintf(b, n, " %s ", t); break;
    case MF_ROOT: multi_note_name(g->root, t, sizeof t), snprintf(b, n, " %s ", t); break;
    case MF_Z2VEL: snprintf(b, n, " %d ", g->split + 1); break;
    case MF_Z1: pad_label(s, g->zone[0], t, sizeof t), snprintf(b, n, " %s ", t); break;
    case MF_Z2: pad_label(s, g->zone[1], t, sizeof t), snprintf(b, n, " %s ", t); break;
    case MF_TUNE: snprintf(b, n, " %+d ", g->semi); break;
    case MF_FINE: snprintf(b, n, " %+d ", g->fine); break;
    case MF_LEVEL: snprintf(b, n, " %d ", g->level); break;
    case MF_PAN: snprintf(b, n, " %+d ", g->pan); break;
    }
}

// ---- The keyboard ----

#define KB_OCTAVES 3
#define KB_WHITE 15 // pixels a white key: 21 of them across
#define KB_TOP (12 * CELL_H)
#define KB_H 30
#define BAR_TOP (11 * CELL_H + 1)

static const int WHITE_OF[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6}; // the white key at or below each note
static const int IS_BLACK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};

// A note's key across the screen, from the first note shown (a C): its left and right edges.
static void key_x(int note, int view, int *l, int *r) {
    int rel = note - view, oct = rel / 12, pc = ((rel % 12) + 12) % 12;
    if (rel < 0) oct = (rel - 11) / 12;
    int wx = (oct * 7 + WHITE_OF[pc]) * KB_WHITE;
    if (IS_BLACK[pc]) *l = wx + KB_WHITE - 5, *r = wx + KB_WHITE + 4;
    else *l = wx, *r = wx + KB_WHITE - 1;
}

// Three octaves of keys, the keygroups as bars over them (the selected one solid, neighbours on
// alternate rows), each root marked on its key, every C named. The view follows the selected
// keygroup.
static void draw_keyboard(struct ui *u, struct os *s, const struct os_pad *p) {
    int sel = s->multi_kg;
    if (p->kgs.n) { // keep the selected keygroup's root in view
        int root = p->kgs.kg[clampi(sel, 0, p->kgs.n - 1)].root;
        if (root < s->multi_view || root >= s->multi_view + 12 * KB_OCTAVES) s->multi_view = (root / 12 - 1) * 12;
    }
    s->multi_view = clampi(s->multi_view / 12 * 12, 0, 120 - 12 * (KB_OCTAVES - 1));
    int view = s->multi_view, last = view + 12 * KB_OCTAVES - 1, w = 7 * KB_OCTAVES * KB_WHITE;
    lcd_fill(u, 0, KB_TOP, w + 2, 2, LCD_FG); // the keyboard's top and bottom
    lcd_fill(u, 0, KB_TOP + KB_H, w + 2, 2, LCD_FG);
    for (int i = 0; i <= 7 * KB_OCTAVES; i++) lcd_fill(u, i * KB_WHITE, KB_TOP, 2, KB_H, LCD_FG); // white keys
    for (int note = view; note <= last; note++) {
        int l, r;
        key_x(note, view, &l, &r);
        if (IS_BLACK[note % 12]) lcd_fill(u, l & ~1, KB_TOP, (r - l + 2) & ~1, KB_H * 3 / 5, LCD_FG);
        else if (note % 12 == 0) { // its octave's name
            char t[4];
            snprintf(t, sizeof t, "C%d", note / 12 - 1);
            lcd_small_text(u, l + 3, KB_TOP + KB_H - 10, t, LCD_FG);
        }
    }
    for (int k = 0; k < p->kgs.n; k++) {
        const struct s600_kg *g = &p->kgs.kg[k];
        if (g->hi < view || g->lo > last) continue;
        int la, ra, lb, rb;
        key_x(g->lo < view ? view : g->lo, view, &la, &ra);
        key_x(g->hi > last ? last : g->hi, view, &lb, &rb);
        int y = BAR_TOP + (k % 2) * 7, x0 = la & ~1, x1 = (rb + 1) & ~1;
        if (k == sel) {
            lcd_fill(u, x0, y, x1 - x0, 6, LCD_FG);
        } else {
            lcd_fill(u, x0, y, x1 - x0, 2, LCD_FG), lcd_fill(u, x0, y + 4, x1 - x0, 2, LCD_FG);
            lcd_fill(u, x0, y, 2, 6, LCD_FG), lcd_fill(u, x1 - 2, y, 2, 6, LCD_FG);
        }
        if (g->root >= view && g->root <= last) { // the root, a mark on its key
            int l, r;
            key_x(g->root, view, &l, &r);
            int cx = ((l + r) / 2) & ~1, black = IS_BLACK[g->root % 12];
            lcd_fill(u, cx - 2, KB_TOP + (black ? KB_H * 3 / 5 - 8 : KB_H - 20), 4, 4, black ? LCD_BG : LCD_FG);
        }
    }
}

static const char *const KT_HELP[4][2] = {
    {"Pan with the key: + puts", "low notes left, high right"},
    {"Cutoff follows the key:", "100 opens an octave an octave"},
    {"Level with the key: - makes", "high notes softer"},
    {"Decays and releases: higher", "notes shorter, lower longer"},
};

void multi_frame(struct os *s, struct ui *u, const struct input *in) {
    int page = s->multi_page, n, inst;
    struct mfield f[16];
    if (!is_multi_program(s)) { // not an instrument program: how to make one
        if (input_down(in, IN_B2)) {
            record_open_autosampler(s);
            ui_wake(u, 1);
            return;
        }
        lcd_rule(u, TITLE_ROW, "MULTI");
        lcd_text(u, 0, 4, "A multisample instrument");
        lcd_text(u, 0, 5, "is an S600 MULTI program:");
        lcd_text(u, 0, 7, "SHIFT + SAMPLE: Program");
        lcd_text(u, 0, 8, "Config, Type: S600 MULTI");
        lcd_rule(u, BOTTOM_ROW, 0);
        return;
    }
    struct os_pad *p = instrument(s, &inst);
    if (page == MP_MAP) { // the Auto Map box: <Cancel> or <Do It>
        if (input_down(in, IN_B1)) s->multi_page = MP_KG, s->multi_cursor = 0;
        if (input_down(in, IN_B3)) {
            int skipped, got = multi_auto_map(s, inst, 0, OS_PADS - 1, s->map_by, s->map_span, s->map_zones, !s->map_exact, &skipped);
            char msg[40];
            if (got < 0) snprintf(msg, sizeof msg, "No roots found");
            else if (skipped) snprintf(msg, sizeof msg, "%d keygroups, %d skipped", got, skipped);
            else snprintf(msg, sizeof msg, "Mapped: %d keygroups", got);
            os_toast(s, u, msg);
            s->multi_page = MP_KG, s->multi_cursor = 0, s->multi_kg = 0;
        }
    } else if (in->shift) {
        if (input_down(in, IN_B2) && page == MP_KG) kg_remove(s, u);
        if (input_down(in, IN_B3)) kg_add_pad(s, u);
    } else {
        if (input_down(in, IN_B1)) s->multi_page = page == MP_KG ? MP_KT : MP_KG, s->multi_cursor = 0;
        if (input_down(in, IN_B2)) { // the Auto Sampler
            record_open_autosampler(s);
            ui_wake(u, 1);
            return;
        }
        if (input_down(in, IN_B3)) s->multi_page = MP_MAP, s->multi_cursor = 0;
    }
    page = s->multi_page;
    n = page_fields(s, page, f);
    if (s->multi_cursor >= n) s->multi_cursor = 0;
    if (in->encoder_press) s->multi_cursor = (s->multi_cursor + (in->shift ? n - 1 : 1)) % n;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d && p->s) change(s, f[s->multi_cursor].kind, d);
    if (in->knob_moved[0] && page == MP_KG && p->kgs.n) // K1: the keygroup
        s->multi_kg = clampi(in->knob[0] * p->kgs.n / 128, 0, p->kgs.n - 1);
    if (s->multi_kg >= p->kgs.n) s->multi_kg = p->kgs.n ? p->kgs.n - 1 : 0;
    n = page_fields(s, page, f);
    if (s->multi_cursor >= n) s->multi_cursor = n - 1;

    if (page == MP_MAP) {
        lcd_text(u, 0, KEY_ROW, "<Cancel>");
        lcd_text(u, 19, KEY_ROW, "<Do It>");
    } else if (in->shift) {
        if (page == MP_KG) lcd_text(u, 9, KEY_ROW, "<Remove>");
        lcd_text(u, 18, KEY_ROW, "<AddPad>");
    } else {
        lcd_inverse(u, 0, KEY_ROW, page == MP_KG ? "<KeyGrp>" : "<KeyTrk>");
        lcd_text(u, 9, KEY_ROW, "<Smplr>");
        lcd_text(u, 17, KEY_ROW, "<AutoMap>");
    }
    char title[LCD_COLS + 8];
    static const char *const NAMES[MP_PAGES] = {"Keygroup", "Key Track", "Auto Map"};
    snprintf(title, sizeof title, "%c%02d MULTI %s", 'A' + inst / 16, inst % 16 + 1, NAMES[page]);
    lcd_rule(u, TITLE_ROW, title);
    if (!p->s) {
        snprintf(title, sizeof title, "%c%02d, the Instrument, is", 'A' + inst / 16, inst % 16 + 1);
        lcd_center(u, 5, title);
        lcd_center(u, 6, "empty: load a sound there,");
        lcd_center(u, 7, "or B2: the Auto Sampler");
        lcd_rule(u, BOTTOM_ROW, 0);
        return;
    }
    if (page == MP_MAP) {
        snprintf(title, sizeof title, "Every pad into %c%02d:", 'A' + inst / 16, inst % 16 + 1);
        lcd_text(u, 0, 3, title);
    }
    for (int i = 0; i < n; i++) {
        char v[LCD_COLS + 8];
        lcd_text(u, f[i].lcol, f[i].row, f[i].label);
        value(s, f[i].kind, v, sizeof v);
        if (f[i].vcol + (int)strlen(v) > LCD_COLS) v[LCD_COLS - f[i].vcol] = 0;
        if (i == s->multi_cursor) lcd_inverse(u, f[i].vcol, f[i].row, v);
        else lcd_text(u, f[i].vcol, f[i].row, v);
    }
    if (page == MP_KG) {
        draw_keyboard(u, s, p);
    } else if (page == MP_KT) {
        int k = f[s->multi_cursor].kind - MF_KPAN;
        if (k >= 0 && k < 4) lcd_text(u, 0, 10, KT_HELP[k][0]), lcd_text(u, 0, 11, KT_HELP[k][1]);
        else lcd_text(u, 0, 10, "Sets envelopes, filter and"), lcd_text(u, 0, 11, "key tracking for the kind");
        lcd_rule(u, BOTTOM_ROW, 0);
    } else {
        lcd_text(u, 0, 10, "Rebuilds the keygroups");
        lcd_text(u, 0, 11, "from every pad with a sound");
        lcd_rule(u, BOTTOM_ROW, 0);
    }
}

// A key played (MIDI in) on the Keygroup page: with the cursor on Root, Lo or Hi, sets it;
// otherwise selects the keygroup it falls in, as on the S3000XL.
void multi_note_in(struct os *s, int note) {
    if (s->screen != OS_MULTI || s->multi_page != MP_KG || !is_multi_program(s)) return;
    int inst;
    struct os_pad *p = instrument(s, &inst);
    struct mfield f[16];
    int n = page_fields(s, MP_KG, f), kind = s->multi_cursor < n ? f[s->multi_cursor].kind : -1;
    if (p->kgs.n && (kind == MF_ROOT || kind == MF_LO || kind == MF_HI)) {
        struct s600_kg *g = &p->kgs.kg[clampi(s->multi_kg, 0, p->kgs.n - 1)];
        if (kind == MF_ROOT) g->root = note;
        else if (kind == MF_LO) g->lo = note <= g->hi ? note : g->hi, p->sp.ranges_manual = 1;
        else g->hi = note >= g->lo ? note : g->lo, p->sp.ranges_manual = 1;
        if (kind == MF_ROOT && !p->sp.ranges_manual) {
            int z = g->zone[0];
            multi_respan(p, !s->map_exact);
            for (int k = 0; k < p->kgs.n; k++)
                if (p->kgs.kg[k].root == note && p->kgs.kg[k].zone[0] == z) s->multi_kg = k;
        }
        os_pad_push(s, inst);
        return;
    }
    int k = multi_kg_at(p, note);
    if (k >= 0) s->multi_kg = k;
}

// A pad pressed on the Keygroup page with the cursor on Zone 1 or Zone 2: that pad is the zone.
void multi_pad(struct os *s, int slot) {
    if (s->screen != OS_MULTI || s->multi_page != MP_KG || !is_multi_program(s)) return;
    int inst;
    struct os_pad *p = instrument(s, &inst);
    struct mfield f[16];
    int n = page_fields(s, MP_KG, f), kind = s->multi_cursor < n ? f[s->multi_cursor].kind : -1;
    if (!p->kgs.n || (kind != MF_Z1 && kind != MF_Z2)) return;
    p->kgs.kg[clampi(s->multi_kg, 0, p->kgs.n - 1)].zone[kind == MF_Z2] = slot + 1;
    os_pad_push(s, inst);
}
