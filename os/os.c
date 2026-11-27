#include "os.h"
#include "midi_out.h"
#include "wav.h"
#include "lcd.h"
#include "onset.h"
#include "seq_screen.h"
#include <stdlib.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PARAM_ROW 11
#define VALUE_COL 14
#define WAVE_X 8
#define WAVE_Y 66
#define WAVE_H 92
#define TOAST_MS 1500
#define STOP_DOUBLE_MS 400

enum { F_FRAMES, F_NUM, F_PCT, F_PAN, F_MS, F_SEMI, F_CENTS, F_MONO, F_GROUP, F_FILTER, F_DEPTH, F_DO, F_PAD, F_DCYMD };

static const char *const DO_NAMES[DO_COUNT] = {
    "Discard", "Normalize", "Extract", "Silence Zone", "Reverse Zone", "Delete Zone",
    "Copy Zone->Temp", "Ins Tmp->Zon St", "Ins Tmp->Zn End", "Convert 44.1k", "Purge Unused",
};

struct param {
    const char *name;
    size_t off;
    int min, max, step, fmt;
};

#define P(n, f, lo, hi, st, fm) {n, offsetof(struct pad_params, f), lo, hi, st, fm}
#define NONE {0, 0, 0, 0, 0, 0}

// `max` for F_FRAMES comes from the sample. Each row is K1, K2, K3.
static const struct param PAGES[PAGE_COUNT][3] = {
    [PAGE_TRIM] = {P("Zone Start", start, 0, 0, 0, F_FRAMES), P("Zone End", end, 0, 0, 0, F_FRAMES),
                   {"Do", 0, 0, DO_COUNT - 1, 1, F_DO}},
    [PAGE_MIX] = {P("Volume", volume, 0, 100, 1, F_PCT), P("Pan", pan, -50, 50, 1, F_PAN), NONE},
    [PAGE_AMP] = {P("Attack", attack_ms, 0, 2000, 5, F_MS), P("Decay", decay_ms, 0, 5000, 10, F_MS),
                  P("Dcy Md", decay_end, 0, 1, 1, F_DCYMD)},
    [PAGE_VEL] = {P("Attack", vel_attack, 0, 100, 1, F_PCT), P("Sft St", soft_start_ms, 0, 5000, 10, F_MS),
                  P("Volume", vel_sens, 0, 100, 1, F_PCT)},
    [PAGE_TUNE] = {P("Semi", semi, -36, 36, 1, F_SEMI), P("Fine", fine, -100, 100, 1, F_CENTS), NONE},
    [PAGE_PLAY] = {P("Poly", mono, 0, 1, 1, F_MONO), P("Mute Grp", mute_group, 0, 16, 1, F_GROUP),
                   P("Also Play", also_pad, 0, OS_PADS, 1, F_PAD)},
    [PAGE_FILTER] = {P("Cutoff", cutoff, 0, 100, 1, F_PCT), P("Reson", reso, 0, 100, 1, F_PCT),
                     P("Type", filter, 0, FILTER_TYPES - 1, 1, F_FILTER)},
    [PAGE_FLT_ENV] = {P("Attack", flt_attack_ms, 0, 2000, 5, F_MS), P("Decay", flt_decay_ms, 0, 5000, 10, F_MS),
                      P("Depth", flt_depth, -100, 100, 1, F_DEPTH)},
};

static const char *const FILTER_NAMES[FILTER_TYPES] = {"Off", "LPF", "HPF", "BPF", "LPF 4", "HPF 4", "BPF 4", "S600 LPF"};
static const struct {
    int first, count;
} GROUPS[3] = {{PAGE_TRIM, 2}, {PAGE_TUNE, 4}, {PAGE_FILTER, 2}};

static int group_of(int page) {
    for (int g = 2; g >= 0; g--)
        if (page >= GROUPS[g].first) return g;
    return 0;
}

static int64_t now_ms(const struct ui *u) {
    if (u->now_ms) return u->now_ms;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void toast(struct os *s, const struct ui *u, const char *text) {
    snprintf(s->toast, sizeof s->toast, "%s", text);
    s->toast_until = now_ms(u) + TOAST_MS;
}

void os_toast(struct os *s, const struct ui *u, const char *text) {
    toast(s, u, text);
}

static int32_t *param_ptr(struct pad_params *p, const struct param *d) {
    return (int32_t *)(void *)((char *)p + d->off);
}

static int param_max(const struct param *d, const struct os_pad *pad) {
    return d->fmt == F_FRAMES ? (pad->s ? pad->s->frames : 0) : d->max;
}

static void reset_knobs(struct os *s) {
    for (int i = 0; i < 3; i++) s->knob_caught[i] = 0;
    s->fader_caught = 0;
}

static void compute_peaks(struct os_pad *pad) {
    const struct sample *s = pad->s;
    int half = WAVE_H / 2 - 1;
    for (int x = 0; x < OS_WAVE_W; x++) {
        int64_t a = (int64_t)x * s->frames / OS_WAVE_W;
        int64_t b = (int64_t)(x + 1) * s->frames / OS_WAVE_W;
        if (b <= a) b = a + 1;
        float lo = 0, hi = 0;
        for (int64_t i = a; i < b && i < s->frames; i++) {
            for (int c = 0; c < s->channels; c++) {
                float v = s->data[i * s->channels + c];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        }
        if (lo < -1) lo = -1;
        if (hi > 1) hi = 1;
        pad->lo[x] = (signed char)(lo * (float)half);
        pad->hi[x] = (signed char)(hi * (float)half);
    }
}

static void default_params(struct pad_params *p, int32_t frames) {
    memset(p, 0, sizeof *p);
    p->end = frames;
    p->loop_lock = 1;
    p->volume = 100;
    p->mono = 1; // a new hit cuts the pad's last one
    p->vel_sens = 100; // Vel Mod Volume: soft hits play softer
    p->filter = FILTER_LP2; // LPF, open until the cutoff comes down
    p->cutoff = 100;
    p->flt_decay_ms = 500;
}

int os_pad_slot(const struct os *s, int pad) {
    return s->program * OS_PADS + pad;
}

// A pad lets go of its sample. The sample goes once no pad in any program uses it.
static void release_sample(struct os *s, struct sample *old) {
    if (old && --old->refs <= 0) engine_retire(s->engine, old);
}

// The pad MIDI keys play: an S600 MULTI program's Instrument; an S600 program's, or with
// LAST PAD the pad pressed last.
int os_instrument(const struct os *s, int program) {
    int inst = s->programs[program].inst;
    if (inst < 0) return s->programs[program].s600 == PROG_MULTI || program != s->program ? 0 : s->selected;
    return inst < OS_PADS ? inst : 0;
}

// Is pad i of a program a MULTI instrument: its keygroups played? (Its own sound is needed too.)
static int is_multi(const struct os *s, int program, int i) {
    const struct os_pad *pad = &s->programs[program].pads[i];
    return s->programs[program].s600 == PROG_MULTI && i == os_instrument(s, program) && pad->s && pad->kgs.n > 0;
}

// Is engine key map `at` still some pad's? Its owner must still be a MULTI instrument.
static int keymap_used(const struct os *s, int at) {
    int slot = s->km_owner[at] - 1;
    if (slot < ENGINE_S600) return 0;
    return is_multi(s, (slot - ENGINE_S600) / OS_PADS, (slot - ENGINE_S600) % OS_PADS);
}

// A MULTI instrument's key map to the engine (on its S600 slot), built from its keygroups and
// the pads their zones name; or, no longer MULTI, taken off.
static void keymap_push(struct os *s, int program, int i) {
    const struct os_pad *pads = s->programs[program].pads, *pad = &pads[i];
    int slot = ENGINE_S600 + program * OS_PADS + i, at = -1;
    for (int k = 0; k < ENGINE_KEYMAPS && at < 0; k++)
        if (s->km_owner[k] == slot + 1) at = k;
    int want = is_multi(s, program, i);
    if (!want) {
        if (at >= 0) engine_set_keymap(s->engine, slot, at, 0), s->km_owner[at] = 0;
        return;
    }
    for (int k = 0; k < ENGINE_KEYMAPS && at < 0; k++)
        if (!s->km_owner[k] || !keymap_used(s, k)) at = k;
    if (at < 0) return; // all 64 in use: it plays its own sample
    static struct keymap km;
    memset(&km, 0, sizeof km);
    km.kt_pan = pad->sp.kt_pan, km.kt_filter = pad->sp.kt_filter, km.kt_level = pad->sp.kt_level, km.kt_env = pad->sp.kt_env;
    for (int k = 0; k < pad->kgs.n && k < S600_KGS; k++) {
        const struct s600_kg *g = &pad->kgs.kg[k];
        int two = g->zone[1] > 0 && g->zone[1] <= OS_PADS && pads[g->zone[1] - 1].s;
        for (int z = 0; z < 2 && km.n < ENGINE_KM_ZONES; z++) {
            int src = g->zone[z] - 1;
            if (src < 0 || src >= OS_PADS || !pads[src].s) continue;
            const struct pad_params *sp = &pads[src].p;
            int split = g->split < 1 ? 1 : g->split > 126 ? 126 : g->split;
            km.z[km.n++] = (struct km_zone){pads[src].s, sp->start, sp->end, sp->loop, sp->loop_on,
                                            (int16_t)g->lo, (int16_t)g->hi, (int16_t)g->root,
                                            (int16_t)(z == 1 ? split + 1 : 1), (int16_t)(z == 0 && two ? split : 127),
                                            (int16_t)g->semi, (int16_t)g->fine, (int16_t)g->level, (int16_t)g->pan};
        }
    }
    if (engine_set_keymap(s->engine, slot, at, &km) == 0) s->km_owner[at] = slot + 1;
}

// Sends a pad's DRUM settings and its S600 ones (to the S600 slots) to the engine. With
// the sample too when it changed.
static void push_slot(struct os *s, int program, int i, int with_sample) {
    struct os_pad *pad = &s->programs[program].pads[i];
    int slot = program * OS_PADS + i;
    struct pad_params sp;
    const struct sample *s2;
    s600_effective(s->programs[program].pads, i, &sp, &s2);
    if (with_sample) engine_set_pad(s->engine, slot, pad->s, &pad->p);
    else engine_update(s->engine, slot, &pad->p);
    // The S600 slot always gets its samples again: its second layer may be another pad's.
    if (is_multi(s, program, i)) sp.fine -= pad->sp.root_fine, sp.layers = 1, s2 = 0; // its keygroups: their own tuning, no Layer 2
    engine_set_pad_layers(s->engine, ENGINE_S600 + slot, pad->s, s2, &sp);
    engine_update(s->engine, ENGINE_S600 + slot, &sp);
    keymap_push(s, program, i);
}

// A MULTI instrument whose keygroups play pad i's sample.
static int multi_uses(const struct os *s, int program, int j, int i) {
    if (!is_multi(s, program, j)) return 0;
    const struct os_pad *pad = &s->programs[program].pads[j];
    for (int k = 0; k < pad->kgs.n && k < S600_KGS; k++)
        if (pad->kgs.kg[k].zone[0] == i + 1 || pad->kgs.kg[k].zone[1] == i + 1) return 1;
    return 0;
}

// Pads whose second layer plays pad i's sample follow its changes, and MULTI instruments
// with a zone on it.
static void push_layer_users(struct os *s, int program, int i) {
    const struct os_pad *pads = s->programs[program].pads;
    for (int j = 0; j < OS_PADS; j++)
        if (j != i && pads[j].s && ((pads[j].sp.layers == 2 && pads[j].sp.l2_pad == i + 1) || multi_uses(s, program, j, i)))
            push_slot(s, program, j, 0);
}

// Every MULTI instrument of a program to the engine again: after a program is loaded or put
// back by Undo, when the pads its zones name may have come after it.
void os_multi_refresh(struct os *s, int program) {
    for (int i = 0; i < OS_PADS; i++) // the instrument, and any pad that was one (its key map taken off)
        if (s->programs[program].pads[i].s && s->programs[program].pads[i].kgs.n > 0) push_slot(s, program, i, 0);
}

// A new Instrument. In a MULTI program the keygroups go with it, so nothing is lost.
void os_set_instrument(struct os *s, int program, int pad) {
    struct os_program *g = &s->programs[program];
    int was = os_instrument(s, program);
    if (pad < -1 || pad >= OS_PADS || (pad < 0 && g->s600 == PROG_MULTI)) return;
    g->inst = pad;
    if (g->s600 == PROG_MULTI && pad != was && g->pads[was].kgs.n > 0 && g->pads[pad].kgs.n == 0) {
        g->pads[pad].kgs = g->pads[was].kgs;
        memset(&g->pads[was].kgs, 0, sizeof g->pads[was].kgs);
        g->pads[pad].sp.kt_pan = g->pads[was].sp.kt_pan, g->pads[pad].sp.kt_filter = g->pads[was].sp.kt_filter;
        g->pads[pad].sp.kt_level = g->pads[was].sp.kt_level, g->pads[pad].sp.kt_env = g->pads[was].sp.kt_env;
        if (g->pads[was].s) push_slot(s, program, was, 0);
    }
    os_multi_refresh(s, program);
}

// Auto Root: a sound just loaded or recorded onto a pad of an S600 program gets its ROOT (and
// the fine tuning that puts it in tune) from the note in its name and the pitch heard in it.
// None found: it stays at C4.
void os_auto_root(struct os *s, int slot) {
    struct os_pad *pad = &s->pads[slot];
    if (!s->auto_root || !s->programs[s->program].s600 || !pad->s) return;
    int fine, root = multi_root_of(pad, 0, &fine);
    if (root < 0) return;
    pad->sp.root = root, pad->sp.root_fine = fine;
    os_pad_push(s, slot);
}

void os_slot_refresh(struct os *s, int program, int slot) {
    if (s->programs[program].pads[slot].s) {
        push_slot(s, program, slot, 1);
    } else {
        engine_set_pad(s->engine, program * OS_PADS + slot, NULL, NULL);
        engine_set_pad(s->engine, ENGINE_S600 + program * OS_PADS + slot, NULL, NULL);
    }
}

void os_sample_release(struct os *s, struct sample *x) {
    release_sample(s, x);
}

void os_pad_push(struct os *s, int slot) {
    push_slot(s, s->program, slot, 0);
    push_layer_users(s, s->program, slot);
}

static void replace_sample(struct os *s, struct os_pad *pad, struct sample *n) {
    struct sample *old = pad->s;
    pad->s = n;
    n->refs++;
    compute_peaks(pad);
    free(pad->onsets);
    pad->onsets = malloc(ONSETS_MAX * sizeof *pad->onsets);
    pad->nonsets = pad->onsets ? onset_detect(n, pad->onsets, ONSETS_MAX) : 0;
    push_slot(s, s->program, (int)(pad - s->pads), 1); // before the old one goes
    push_layer_users(s, s->program, (int)(pad - s->pads));
    release_sample(s, old);
}

// Temp belongs to one sample. It goes when that sample is changed for another or deleted.
static void clear_temp(struct os *s) {
    sample_free(s->temp); // only the UI ever holds the temp, so it can go at once
    s->temp = 0;
    s->temp_pad = -1;
}

static int on_a_pad(const struct os *s, const struct sample *sm, int skip_prog, int skip_pad) {
    for (int p = 0; p < OS_PROGRAMS; p++)
        for (int i = 0; i < OS_PADS; i++)
            if (!(p == skip_prog && i == skip_pad) && s->programs[p].pads[i].s == sm) return 1;
    return 0;
}

// A pad is about to let go of its sound: kept in memory when no other pad plays it and memory
// does not have it yet. (The undo history holds sounds too, so how many hold one says nothing
// of whether it would be lost: a sound held only by undo went when the undo did.)
static void keep_if_last(struct os *s, struct sample *sm, const char *name, int prog, int slot) {
    if (!sm || on_a_pad(s, sm, prog, slot)) return;
    for (int m = 0; m < s->nmemory; m++)
        if (s->memory[m].s == sm) return;
    os_memory_add(s, sm, name);
}

// A sound loaded, recorded, extracted or zoned onto a pad over another: the old one, if no
// other pad has it, stays in memory (Sample: and Snd: still find it), as on the MPC2000XL.
// A pad's trim for the sound it is letting go of, kept: put that sound back on that pad (the
// Sample: field, Snd:, a load) and its start, end and loop come back, so a chopped pad (a zone of a
// longer sound) is not lost when another sound goes on it by mistake. The last TRIM_MEMO kept.
#define TRIM_MEMO 128
static struct trim_memo {
    int prog, slot;
    const struct sample *s;
    int32_t frames, start, end, loop, loop_on, loop_lock;
    char name[sizeof ((struct os_pad *)0)->name];
} trim_memo[TRIM_MEMO];
static unsigned trim_n;

static void trim_remember(const struct os *s, int slot) {
    const struct os_pad *pad = &s->pads[slot];
    if (!pad->s) return;
    for (unsigned i = 0; i < TRIM_MEMO; i++) { // this pad and sound again: its newest trim, in place
        struct trim_memo *m = &trim_memo[i];
        if (m->s == pad->s && m->prog == s->program && m->slot == slot) {
            m->start = pad->p.start, m->end = pad->p.end, m->loop = pad->p.loop, m->loop_on = pad->p.loop_on;
            m->loop_lock = pad->p.loop_lock;
            snprintf(m->name, sizeof m->name, "%s", pad->name);
            return;
        }
    }
    trim_memo[trim_n++ % TRIM_MEMO] = (struct trim_memo){s->program, slot, pad->s, pad->s->frames, pad->p.start, pad->p.end,
                                                       pad->p.loop, pad->p.loop_on, pad->p.loop_lock, ""};
    snprintf(trim_memo[(trim_n - 1) % TRIM_MEMO].name, sizeof trim_memo[0].name, "%s", pad->name);
}

static const char *trim_recall(struct os *s, int slot) { // the chop's name, when one came back
    struct os_pad *pad = &s->pads[slot];
    for (unsigned i = 0; i < TRIM_MEMO && pad->s; i++) {
        const struct trim_memo *m = &trim_memo[i];
        if (m->s != pad->s || m->prog != s->program || m->slot != slot || m->frames != pad->s->frames) continue;
        if (m->end <= m->start || m->end > pad->s->frames) return 0;
        pad->p.start = m->start, pad->p.end = m->end, pad->p.loop = m->loop, pad->p.loop_on = m->loop_on;
        pad->p.loop_lock = m->loop_lock; // a loop point set by hand stays where it was
        return m->name;
    }
    return 0;
}

void os_pad_load_over(struct os *s, int slot, struct sample *n, const char *name) {
    struct os_pad *pad = &s->pads[slot];
    if (pad->s != n) keep_if_last(s, pad->s, pad->name, s->program, slot);
    os_pad_load(s, slot, n, name);
}

void os_pad_load(struct os *s, int slot, struct sample *n, const char *name) {
    if (slot == s->temp_pad) clear_temp(s);
    struct os_pad *pad = &s->pads[slot];
    if (pad->s != n) trim_remember(s, slot);
    replace_sample(s, pad, n);
    default_params(&pad->p, n->frames);
    const char *was = trim_recall(s, slot); // this sound on this pad before: its chop back
    s600_defaults(&pad->sp);
    os_pad_push(s, slot);
    snprintf(pad->name, sizeof pad->name, "%s", was && was[0] ? was : name);
}

void os_init(struct os *s, struct engine *e) {
    memset(s, 0, sizeof *s);
    s->engine = e;
    for (int p = 0; p < OS_PROGRAMS; p++) snprintf(s->programs[p].name, sizeof s->programs[p].name, "Program%02d", p + 1);
    s->pads = s->programs[0].pads;
    s->screen = OS_TITLE;
    s->preview_on = 1;
    atomic_init(&s->main_gain, 0.5f); // the panel does not report the knob until it moves
    for (int i = 0; i < 3; i++) s->knob_last[i] = -1;
    for (int i = 0; i < 16; i++) s->sounding[i] = s->rec_note[i] = s->live_midi[i] = -1;
    for (int i = 0; i < 128; i++) s->midi_rec[i] = -1;
    s->levels_root = 7; // pad A08 plays the sound at its own pitch: seven pads below, eight above
    s->start_rate = 29760, s->start_bits = 12, s->start_aa = 1, s->start_norm = 1, s->start_thru = 1, s->start_mode = REC_STEREO;
    record_defaults(s);
    zones_defaults(s);
    master_init(&s->master);
    fx_pages_init(s);
    s->eq_seq = -1;
    midi_defaults(&s->midi);
    s->fader_last = -1;
    s->temp_pad = -1;
    s->disk_tracks = 2, s->disk_mix = 1;
    s->map_zones = 1; // AUTO MAP: one zone a keygroup, NAME+PITCH, MIDWAY, EXTEND
    s->auto_root = 1;
    s->button_lights = BUTTON_RED, s->pad_lights = PAD_LIGHT_OFF, s->screen_scheme = SCREEN_MPC3000;
    s->pads_lit = 1; // whatever the stock app left lit goes dark on the first frame
    s->start_bpm10 = 1200, s->start_bars = 2, s->start_tc = TC_16, s->start_swing = 50;
    s->inst_gain = 12, s->inst_bright = 50, s->pad_threshold = 30, s->usb_out = 0;
    s->usb_outs = 0, s->outs_by_track = 1, s->outs_keep = 1; // by track, to start
    s->padtpl_oct = 2; // pad note templates from C2
    s->led_style = 1; // colour A alone: red on the two-colour buttons
    lcd_set_scheme(SCREEN_MPC3000);
    s->erase_pad = -1;
    seq_init(&s->seq);
    s->tc_last = s->seq.tc;
    s->metro_on_mode = METRO_REC;
    e->hook = seq_audio_block;
    e->hook_ctx = &s->seq;
    s->warp_pct = 100, s->warp_bars = 1, s->warp_pad = -1;
    s->ts_factor = 100, s->ts_mode = 1, s->ts_qual = 90, s->ts_width = 90, s->ts_cycle = 1000; // INTELL at 90/90
    s->page = PAGE_MIX; // the Sample screen opens on Vol; Trim is B1 again, or Shift + Pad 13
    s->group_page[0] = PAGE_MIX;
    s->group_page[1] = PAGE_TUNE;
    s->group_page[2] = PAGE_FILTER;
}

void os_set_program(struct os *s, int program) {
    if (program < 0 || program >= OS_PROGRAMS || program == s->program) return;
    if (s->temp) clear_temp(s); // temp belongs to a sample of the old program
    s->program = program;
    s->pads = s->programs[program].pads;
    s->erase_ask = 0;
    reset_knobs(s);
}

int os_program_sounds(const struct os *s, int program) {
    int n = 0;
    for (int i = 0; i < OS_PADS; i++)
        if (s->programs[program].pads[i].s) n++;
    return n;
}

static void pad_changed(struct os *s, int slot);

void os_pad_clear(struct os *s, int slot) {
    struct os_pad *pad = &s->pads[slot];
    if (!pad->s) return;
    if (slot == s->temp_pad) clear_temp(s);
    engine_set_pad(s->engine, os_pad_slot(s, slot), NULL, NULL);
    engine_set_pad(s->engine, ENGINE_S600 + os_pad_slot(s, slot), NULL, NULL);
    release_sample(s, pad->s);
    free(pad->onsets);
    memset(pad, 0, sizeof *pad);
    push_layer_users(s, s->program, slot);
}

void os_pad_copy(struct os *s, int from, int to) {
    if (from == to || !s->pads[from].s) return;
    os_pad_clear(s, to);
    const struct os_pad *a = &s->pads[from];
    struct os_pad *b = &s->pads[to];
    *b = *a;
    b->s->refs++;
    b->onsets = malloc((size_t)(a->nonsets > 0 ? a->nonsets : 1) * sizeof *b->onsets);
    if (b->onsets) memcpy(b->onsets, a->onsets, (size_t)a->nonsets * sizeof *b->onsets);
    else b->nonsets = 0;
    push_slot(s, s->program, to, 1);
    push_layer_users(s, s->program, to);
}

// Copy and Swap with a pad of another program (Program Config: the wheel picks the program).
static void clear_in(struct os *s, int program, int slot) {
    if (program == s->program) {
        os_pad_clear(s, slot);
        return;
    }
    struct os_pad *pad = &s->programs[program].pads[slot];
    if (!pad->s) return;
    engine_set_pad(s->engine, program * OS_PADS + slot, NULL, NULL);
    engine_set_pad(s->engine, ENGINE_S600 + program * OS_PADS + slot, NULL, NULL);
    release_sample(s, pad->s);
    free(pad->onsets);
    memset(pad, 0, sizeof *pad);
    push_layer_users(s, program, slot);
}

void os_pad_copy_to(struct os *s, int from, int program, int to) {
    if (program == s->program) {
        os_pad_copy(s, from, to);
        return;
    }
    if (!s->pads[from].s) return;
    clear_in(s, program, to);
    const struct os_pad *a = &s->pads[from];
    struct os_pad *b = &s->programs[program].pads[to];
    *b = *a;
    b->s->refs++;
    b->onsets = malloc((size_t)(a->nonsets > 0 ? a->nonsets : 1) * sizeof *b->onsets);
    if (b->onsets) memcpy(b->onsets, a->onsets, (size_t)a->nonsets * sizeof *b->onsets);
    else b->nonsets = 0;
    push_slot(s, program, to, 1);
    push_layer_users(s, program, to);
}

void os_pad_swap_with(struct os *s, int a, int program, int b) {
    if (program == s->program) {
        os_pad_swap(s, a, b);
        return;
    }
    if (s->temp_pad == a) clear_temp(s);
    struct os_pad *pa = &s->pads[a], *pb = &s->programs[program].pads[b];
    struct os_pad t = *pa;
    *pa = *pb;
    *pb = t;
    for (int i = 0; i < 2; i++) {
        int prog = i ? program : s->program, slot = i ? b : a;
        if (s->programs[prog].pads[slot].s) {
            push_slot(s, prog, slot, 1);
        } else {
            engine_set_pad(s->engine, prog * OS_PADS + slot, NULL, NULL);
            engine_set_pad(s->engine, ENGINE_S600 + prog * OS_PADS + slot, NULL, NULL);
        }
        push_layer_users(s, prog, slot);
    }
}

void os_pad_swap(struct os *s, int a, int b) {
    if (a == b) return;
    if (s->temp_pad == a || s->temp_pad == b) clear_temp(s);
    struct os_pad t = s->pads[a];
    s->pads[a] = s->pads[b];
    s->pads[b] = t;
    for (int i = 0; i < 2; i++) {
        int slot = i ? b : a;
        if (s->pads[slot].s) {
            push_slot(s, s->program, slot, 1);
        } else { // now empty
            engine_set_pad(s->engine, os_pad_slot(s, slot), NULL, NULL);
            engine_set_pad(s->engine, ENGINE_S600 + os_pad_slot(s, slot), NULL, NULL);
        }
        push_layer_users(s, s->program, slot);
    }
}

// Every sound in memory, once each, leaving out the place of pad skip_pad of program skip_prog
// (-1: none).
static int sounds_except(const struct os *s, struct sample **list, const char **names, int max, int skip_prog,
                         int skip_pad) {
    int n = 0;
    for (int p = 0; p < OS_PROGRAMS; p++)
        for (int i = 0; i < OS_PADS; i++) {
            if (p == skip_prog && i == skip_pad) continue;
            const struct os_pad *pad = &s->programs[p].pads[i];
            int seen = !pad->s;
            for (int k = 0; k < n && !seen; k++) seen = list[k] == pad->s;
            if (!seen && n < max) list[n] = pad->s, names[n++] = pad->name;
        }
    for (int m = 0; m < s->nmemory; m++) {
        int seen = 0;
        for (int k = 0; k < n && !seen; k++) seen = list[k] == s->memory[m].s;
        if (!seen && n < max) list[n] = s->memory[m].s, names[n++] = s->memory[m].name;
    }
    return n;
}

int os_sounds(const struct os *s, struct sample **list, const char **names, int max) {
    return sounds_except(s, list, names, max, -1, -1);
}

void os_name_erase(char *b, int *pos, int all) {
    if (all) {
        b[0] = 0, *pos = 0;
        return;
    }
    int len = (int)strlen(b), used = len;
    while (used > 0 && b[used - 1] == ' ') used--;
    if (*pos >= used && *pos > 0) (*pos)--; // past the end of the name: the letter before
    if (*pos < len) memmove(b + *pos, b + *pos + 1, (size_t)(len - *pos));
}

void os_pad_assign(struct os *s, int slot, struct sample *n, const char *name) {
    struct os_pad *pad = &s->pads[slot];
    if (!n || pad->s == n) return;
    char keep[sizeof pad->name];
    snprintf(keep, sizeof keep, "%s", name); // name may be this pad's own, about to change
    if (!pad->s) {
        os_pad_load(s, slot, n, keep);
        return;
    }
    if (slot == s->temp_pad) clear_temp(s);
    keep_if_last(s, pad->s, pad->name, s->program, slot); // its last place: kept, not lost
    trim_remember(s, slot);
    replace_sample(s, pad, n);
    pad->p.start = 0, pad->p.end = n->frames, pad->p.loop = 0;
    const char *was = trim_recall(s, slot); // this sound on this pad before: its chop back, and its name
    snprintf(pad->name, sizeof pad->name, "%s", was && was[0] ? was : keep);
    pad_changed(s, slot);
}

void os_program_clear(struct os *s, int program) {
    for (int i = 0; i < OS_PADS; i++) {
        struct os_pad *pad = &s->programs[program].pads[i];
        if (!pad->s) continue;
        engine_set_pad(s->engine, program * OS_PADS + i, NULL, NULL);
        engine_set_pad(s->engine, ENGINE_S600 + program * OS_PADS + i, NULL, NULL);
        release_sample(s, pad->s);
        free(pad->onsets);
        memset(pad, 0, sizeof *pad);
    }
    if (program == s->program && s->temp) clear_temp(s);
    os_program_set_s600(s, program, 0); // emptied, it is a DRUM program again
}

void os_program_copy(struct os *s, int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= OS_PROGRAMS || to >= OS_PROGRAMS) return;
    os_program_clear(s, to);
    for (int i = 0; i < OS_PADS; i++) {
        const struct os_pad *src = &s->programs[from].pads[i];
        struct os_pad *dst = &s->programs[to].pads[i];
        if (!src->s) continue;
        *dst = *src;
        dst->s->refs++; // shared, not duplicated
        dst->onsets = malloc((size_t)(src->nonsets > 0 ? src->nonsets : 1) * sizeof *dst->onsets);
        if (dst->onsets) memcpy(dst->onsets, src->onsets, (size_t)src->nonsets * sizeof *dst->onsets);
        else dst->nonsets = 0;
        push_slot(s, to, i, 1);
    }
    snprintf(s->programs[to].name, sizeof s->programs[to].name, "%s", s->programs[from].name);
    os_program_set_s600(s, to, s->programs[from].s600); // an instrument copies as an instrument
    s->programs[to].inst = s->programs[from].inst;
    os_multi_refresh(s, to);
}

static void normalize_trim(struct pad_params *p, int32_t frames) {
    if (p->end > frames) p->end = frames;
    if (p->end < 1) p->end = 1;
    if (p->start < 0) p->start = 0;
    if (p->start >= p->end) p->start = p->end - 1;
    if (p->loop_lock) p->loop = p->start;
    if (p->loop < p->start) p->loop = p->start;
    if (p->loop >= p->end) p->loop = p->end - 1;
}

static void pad_changed(struct os *s, int slot) {
    struct os_pad *pad = &s->pads[slot];
    if (!pad->s) return;
    normalize_trim(&pad->p, pad->s->frames);
    os_pad_push(s, slot);
}

static void param_set(struct os *s, const struct param *d, int value) {
    struct os_pad *pad = &s->pads[s->selected];
    int max = param_max(d, pad);
    if (value < d->min) value = d->min;
    if (value > max) value = max;
    *param_ptr(&pad->p, d) = value;
    pad_changed(s, s->selected);
    // Edit All Pads: the same value on every pad with a sound. Not the trim: each sound's own.
    if (s->edit_all && d->fmt != F_FRAMES && d->fmt != F_DO)
        for (int i = 0; i < OS_PADS; i++)
            if (i != s->selected && s->pads[i].s) {
                *param_ptr(&s->pads[i].p, d) = value;
                pad_changed(s, i);
            }
}

static void param_nudge(struct os *s, const struct param *d, int steps, int fine) {
    if (d->fmt == F_DO) {
        int op = s->do_op + steps;
        s->do_op = op < 0 ? 0 : op >= DO_COUNT ? DO_COUNT - 1 : op;
        return;
    }
    struct os_pad *pad = &s->pads[s->selected];
    if (!pad->s) return;
    int step = d->step;
    if (d->fmt == F_FRAMES) {
        step = fine ? 1 : pad->s->frames / OS_WAVE_W;
        if (step < 1) step = 1;
    } else if (fine) {
        step = 1;
    }
    param_set(s, d, *param_ptr(&pad->p, d) + steps * step);
}

static int has_param(int page, int slot) {
    return PAGES[page][slot].name != 0;
}

static void first_frame(struct os *s) {
    s->frame_slot = 0;
    while (s->frame_slot < 2 && !has_param(s->page, s->frame_slot)) s->frame_slot++;
}

static void move_frame(struct os *s, int dir) {
    s->fader_caught = 0;
    for (int i = 0; i < 3; i++) {
        // -1 is Sample: above the knob rows, where the pad's sound is chosen
        s->frame_slot = (s->frame_slot + 1 + dir + 4) % 4 - 1;
        if (s->frame_slot < 0 || has_param(s->page, s->frame_slot)) return;
    }
}

static void set_page(struct os *s, int page) {
    s->page = page;
    s->group_page[group_of(page)] = page;
    reset_knobs(s);
    first_frame(s);
}

static void active_track(struct os *s, int *type, int *channel);

// The Trim page, from anywhere (the S600's, on an S600 program).
static void open_trim(struct os *s) {
    if (os_plays_s600(s)) {
        os_open_s600_trim(s);
    } else {
        s->screen = OS_SAMPLE;
        set_page(s, PAGE_TRIM);
        first_frame(s);
    }
}

static void press_tab(struct os *s, int group) {
    if (group_of(s->page) == group) {
        int rel = s->page - GROUPS[group].first;
        set_page(s, GROUPS[group].first + (rel + 1) % GROUPS[group].count);
    } else {
        set_page(s, s->group_page[group]);
    }
}

static void select_pad(struct os *s, int slot) {
    // An S600 MULTI program's S600 and MULTI screens stay on its instrument: a pad pressed there is
    // remembered (for <Add Pad> and the zones) but the instrument stays selected.
    if (s->programs[s->program].s600 == PROG_MULTI && (s->screen == OS_S600 || s->screen == OS_MULTI)) {
        s->multi_pressed = slot;
        slot = os_instrument(s, s->program);
    }
    if (slot != s->selected) {
        reset_knobs(s);
        if (s->temp) clear_temp(s); // another sample: temp cannot go into it
    }
    s->selected = slot;
}

static void normalize_sample(struct os *s, const struct ui *u) {
    struct os_pad *pad = &s->pads[s->selected];
    if (!pad->s) return;
    const struct sample *o = pad->s;
    size_t count = (size_t)o->frames * (size_t)o->channels;
    float peak = 0;
    for (size_t i = 0; i < count; i++) {
        float v = fabsf(o->data[i]);
        if (v > peak) peak = v;
    }
    if (peak <= 0) {
        toast(s, u, "Sample is silent");
        return;
    }
    struct sample *n = sample_new(o->frames, o->channels, o->rate);
    if (!n) {
        toast(s, u, "Out of memory");
        return;
    }
    for (size_t i = 0; i < count; i++) n->data[i] = o->data[i] / peak;
    replace_sample(s, pad, n);
    char name[sizeof pad->name];
    snprintf(name, sizeof name, "%.28s-Normalized", pad->name);
    memcpy(pad->name, name, sizeof name);
    toast(s, u, "Normalized");
}

// Copies `count` frames of `src` from `from` into `dst` at *at, matching dst's channels.
static void append(struct sample *dst, int32_t *at, const struct sample *src, int32_t from, int32_t count) {
    for (int32_t i = 0; i < count; i++, (*at)++) {
        const float *f = src->data + (size_t)(from + i) * (size_t)src->channels;
        float *t = dst->data + (size_t)*at * (size_t)dst->channels;
        if (dst->channels == src->channels) {
            for (int c = 0; c < dst->channels; c++) t[c] = f[c];
        } else if (dst->channels == 2) {
            t[0] = t[1] = f[0];
        } else {
            t[0] = 0.5f * (f[0] + f[1]);
        }
    }
}

// <Do> on the zone between Zone Start and Zone End. Every change builds a new sample and
// swaps it in, so a voice still playing the old one is never cut up under it.
static void zone_op(struct os *s, const struct ui *u) {
    struct os_pad *pad = &s->pads[s->selected];
    if (s->do_op == DO_PURGE) { // not the zone: memory, every program
        char msg[32];
        int n = os_purge_unused(s);
        if (n) snprintf(msg, sizeof msg, "Purged %d sound%s", n, n == 1 ? "" : "s");
        else snprintf(msg, sizeof msg, "No unused sounds");
        toast(s, u, msg);
        return;
    }
    if (!pad->s) return;
    const struct sample *o = pad->s;
    if (s->do_op != DO_COPY && s->do_op != DO_EXTRACT) os_undo_save(s, DO_NAMES[s->do_op]); // Extract: when it lands
    int32_t a = pad->p.start, b = pad->p.end, frames = o->frames;
    if (a < 0) a = 0;
    if (b > frames) b = frames;
    if (b <= a) return;
    int32_t len = b - a, at = 0;
    struct sample *n = 0;
    switch (s->do_op) {
    case DO_DISCARD: // keeps only the zone
        if (!(n = sample_new(len, o->channels, o->rate))) break;
        append(n, &at, o, a, len);
        replace_sample(s, pad, n);
        pad->p.start = 0, pad->p.end = len;
        pad->p.loop -= a; // the loop point stays on the same audio
        toast(s, u, "Discarded");
        break;
    case DO_NORMALIZE:
        normalize_sample(s, u);
        return;
    case DO_EXTRACT: // the zone as a new sound: a pad hit assigns it, ENTER keeps it in memory
        if (!(n = sample_new(len, o->channels, o->rate))) break;
        append(n, &at, o, a, len);
        sample_free(s->extract);
        s->extract = n;
        snprintf(s->extract_name, sizeof s->extract_name, "%.30s-Ext", pad->name);
        return;
    case DO_SILENCE:
    case DO_REVERSE:
        if (!(n = sample_new(frames, o->channels, o->rate))) break;
        append(n, &at, o, 0, frames);
        for (int32_t i = a; i < b; i++) {
            float *f = n->data + (size_t)i * (size_t)n->channels;
            if (s->do_op == DO_SILENCE) {
                for (int c = 0; c < n->channels; c++) f[c] = 0;
            } else if (i < a + len / 2) {
                float *g = n->data + (size_t)(b - 1 - (i - a)) * (size_t)n->channels;
                for (int c = 0; c < n->channels; c++) {
                    float t = f[c];
                    f[c] = g[c];
                    g[c] = t;
                }
            }
        }
        replace_sample(s, pad, n);
        toast(s, u, s->do_op == DO_SILENCE ? "Zone silenced" : "Zone reversed");
        break;
    case DO_DELETE:
        if (len >= frames) {
            toast(s, u, "Can't delete the whole sample");
            return;
        }
        if (!(n = sample_new(frames - len, o->channels, o->rate))) break;
        append(n, &at, o, 0, a);
        append(n, &at, o, b, frames - b);
        replace_sample(s, pad, n);
        pad->p.end = n->frames;
        toast(s, u, "Zone deleted");
        break;
    case DO_COPY:
        if (!(n = sample_new(len, o->channels, o->rate))) break;
        append(n, &at, o, a, len);
        clear_temp(s);
        s->temp = n;
        s->temp_pad = s->selected;
        toast(s, u, "Copied to temp");
        return;
    case DO_INS_START:
    case DO_INS_END: {
        if (!s->temp || s->temp_pad != s->selected) {
            toast(s, u, "Temp is empty");
            return;
        }
        int32_t where = s->do_op == DO_INS_START ? a : b, tlen = s->temp->frames;
        if (!(n = sample_new(frames + tlen, o->channels, o->rate))) break;
        append(n, &at, o, 0, where);
        append(n, &at, s->temp, 0, tlen);
        append(n, &at, o, where, frames - where);
        replace_sample(s, pad, n);
        pad->p.start = where, pad->p.end = where + tlen; // the inserted audio becomes the zone
        toast(s, u, "Temp inserted");
        break;
    }
    case DO_CONVERT: { // to 44.1 kHz, sounding just as it plays now, for other gear and files
        if (o->rate == ENGINE_RATE) {
            toast(s, u, "Already 44.1 kHz");
            return;
        }
        double k = (double)ENGINE_RATE / o->rate;
        if (!(n = rec_convert_44k(o))) break;
        pad->p.start = (int32_t)(pad->p.start * k);
        pad->p.end = (int32_t)(pad->p.end * k);
        pad->p.loop = (int32_t)(pad->p.loop * k);
        replace_sample(s, pad, n);
        toast(s, u, "Converted to 44.1 kHz");
        break;
    }
    }
    if (!n) {
        toast(s, u, "Out of memory");
        return;
    }
    pad_changed(s, s->selected);
}

static void toggle(struct os *s, const struct ui *u, size_t off, const char *on, const char *off_text) {
    struct os_pad *pad = &s->pads[s->selected];
    if (!pad->s) return;
    int32_t *v = (int32_t *)(void *)((char *)&pad->p + off);
    *v = !*v;
    pad_changed(s, s->selected);
    if (s->edit_all) // Edit All Pads: every pad with a sound the same way
        for (int i = 0; i < OS_PADS; i++)
            if (i != s->selected && s->pads[i].s) {
                *(int32_t *)(void *)((char *)&s->pads[i].p + off) = *v;
                pad_changed(s, i);
            }
    toast(s, u, *v ? on : off_text);
}

static void shift_pad(struct os *s, struct ui *u, int pad) {
    if (seq_shift_pad(s, u, pad)) return;
    switch (pad) {
    case 0:
        s->full_level = !s->full_level;
        toast(s, u, s->full_level ? "Full Level: Enabled" : "Full Level: Disabled");
        break;
    case 11: // Shift + Pad 12: Song
        song_open(s);
        break;
    case 14: // Shift + Pad 15: Warp, again the S-Series Timestretch
        warp_open(s);
        break;
    case 4: // Shift + Pad 5: Master Effects
        master_open(s);
        break;
    case 12: // Shift + Pad 13: the Trim page (the S600's, on an S600 program)
        open_trim(s);
        break;
    case 10: // Shift + Pad 11: Resample
        record_open_resample(s);
        break;
    case 7: // Shift + Pad 8: MIDI Settings
        if (s->screen == OS_MIDI) s->cursor_midi = 0; // pressed again: the first field
        else midi_open(s);
        break;
    case 15: // Shift + Pad 16: Save/Load, on the mode and type last used (SAVE PROJECT the first time)
        if (s->disk_used) disk_open(s, s->disk_mode, s->disk_type);
        else disk_open(s, DISK_SAVE, DISK_PROJECT);
        break;
    default:
        toast(s, u, "Not built yet");
        break;
    }
}

static void active_track(struct os *s, int *type, int *channel) {
    pthread_mutex_lock(&s->seq.lock);
    const struct seq_track *k = &s->seq.sequence[s->seq.current].track[s->seq.track];
    *type = k->type;
    *channel = k->channel;
    pthread_mutex_unlock(&s->seq.lock);
}

int os_plays_s600(struct os *s) {
    int type = os_track_type(s);
    return type == TRACK_DRUM && s->programs[s->program].s600;
}

void os_program_set_s600(struct os *s, int program, int on) {
    s->programs[program].s600 = on < 0 ? 0 : on >= PROG_TYPES ? PROG_S600 : on;
    pthread_mutex_lock(&s->seq.lock);
    s->seq.kit_s600[program] = (unsigned char)(on != 0);
    pthread_mutex_unlock(&s->seq.lock);
    os_multi_refresh(s, program); // MULTI on or off: the instrument's keygroups played or not
}

// Purge: the sounds kept in memory that no pad of any program plays, gone (Undo brings them back).
int os_purge_unused(struct os *s) {
    int unused = 0;
    for (int m = 0; m < s->nmemory; m++) unused += !on_a_pad(s, s->memory[m].s, -1, -1);
    if (!unused) return 0;
    os_undo_save(s, "Purge Unused");
    int k = 0, gone = 0;
    for (int m = 0; m < s->nmemory; m++) {
        if (on_a_pad(s, s->memory[m].s, -1, -1)) {
            s->memory[k++] = s->memory[m];
            continue;
        }
        os_sample_release(s, s->memory[m].s);
        gone++;
    }
    s->nmemory = k;
    return gone;
}

int os_memory_add(struct os *s, struct sample *n, const char *name) {
    if (s->nmemory >= OS_MEMORY) return -1;
    s->memory[s->nmemory].s = n;
    n->refs++;
    snprintf(s->memory[s->nmemory].name, sizeof s->memory[0].name, "%s", name);
    s->nmemory++;
    return 0;
}

int os_track_type(struct os *s) {
    int type, channel;
    active_track(s, &type, &channel);
    return type;
}

// Plays a pad of the current program `tune` semitones from its pitch, as a track of `type`
// would: an S600 track through the pad's S600 settings. The engine slot, or -1.
static int play_sound(struct os *s, int pad, int tune, int vel, int type) {
    if (pad < 0 || pad >= OS_PADS || !s->pads[pad].s) return -1;
    struct os_pad *sp = &s->pads[pad];
    int id = os_pad_slot(s, pad);
    struct pad_params p = sp->p;
    const struct sample *s2 = 0;
    if (type == TRACK_DRUM && s->programs[s->program].s600) {
        s600_effective(s->pads, pad, &p, &s2);
        id += ENGINE_S600;
    }
    pthread_mutex_lock(&s->seq.lock); // through the active track's mixer channel
    int track = s->seq.track;
    const struct seq_track *k = &s->seq.sequence[s->seq.current].track[track];
    int level = k->level, pan = k->pan;
    pthread_mutex_unlock(&s->seq.lock);
    return engine_note_on_layers(s->engine, id, vel, tune, sp->s, s2, &p, track + 1, level, pan) == 0 ? id : -1;
}

void os_audition(struct os *s, int note, int tune, int vel) {
    int type, channel, slot = -1;
    active_track(s, &type, &channel);
    for (int i = 0; i < 16 && slot < 0; i++)
        if (!s->aud[i].until) slot = i;
    if (slot < 0) return;
    if (type == TRACK_MIDI) {
        if (channel <= 0) return;
        seq_live_midi(&s->seq, channel, note, vel);
        s->aud[slot].channel = channel, s->aud[slot].note = note;
    } else {
        int id = play_sound(s, note, tune, vel, type);
        if (id < 0) return;
        s->aud[slot].channel = 0, s->aud[slot].id = id, s->aud[slot].key = tune;
    }
    s->aud[slot].until = (s->frame_ms ? s->frame_ms : 1) + 250;
}

// MIDI IN: notes play and record like pads (with 16 Levels or on an S600 track, chromatically
// from note 60 at the sound's own pitch), the bend and mod wheels reach S600 voices.
// A note's release, or held back while the sustain pedal is down: the sound with NORMAL, the
// sound and the recorded length with TO DURATION.
static void release_note(struct os *s, int id, int key, int rec_note, int rec_tune) {
    int hold = s->sus_down && s->midi.sustain != SUS_FOOT;
    if (id >= 0) {
        if (hold && s->sus_n < 64) s->sus_snd[s->sus_n].id = id, s->sus_snd[s->sus_n++].key = key;
        else engine_note_off_key(s->engine, id, key);
    }
    if (rec_note >= 0) {
        if (hold && s->midi.sustain == SUS_DURATION && s->sus_rn < 64)
            s->sus_rec[s->sus_rn].note = rec_note, s->sus_rec[s->sus_rn++].tune = rec_tune;
        else seq_record_release_tuned(&s->seq, rec_note, rec_tune);
    }
}

static void sustain_up(struct os *s) {
    for (int i = 0; i < s->sus_n; i++) engine_note_off_key(s->engine, s->sus_snd[i].id, s->sus_snd[i].key);
    for (int i = 0; i < s->sus_rn; i++) seq_record_release_tuned(&s->seq, s->sus_rec[i].note, s->sus_rec[i].tune);
    s->sus_n = s->sus_rn = 0;
}

// The pedal as a footswitch: PLAY/STOP, REC/OVERDUB (stopped, it starts an overdub;
// playing, it punches in and out), or AUDIO REC (an audio track or Live Looper take).
static void footswitch(struct os *s, struct ui *u) {
    pthread_mutex_lock(&s->seq.lock);
    int stopped = s->seq.mode == SEQ_STOPPED;
    pthread_mutex_unlock(&s->seq.lock);
    if (s->midi.foot == FOOT_AUDIO) { // <Rec> or <Stop> on Audiotrack Record or Live Looper
        record_foot(s, u);
        return;
    }
    if (s->midi.foot == FOOT_PLAY) {
        if (stopped) seq_play(&s->seq, 1);
        else seq_stop(&s->seq, 0);
        return;
    }
    os_undo_save(s, "Record");
    if (stopped) {
        pthread_mutex_lock(&s->seq.lock);
        s->seq.rec_arm = REC_ARM_OVERDUB;
        pthread_mutex_unlock(&s->seq.lock);
        seq_play(&s->seq, 1);
    } else {
        seq_rec_button(&s->seq);
    }
    toast(s, u, stopped ? "Overdub" : "Punch");
}

// Sync RECEIVE: start, continue and stop as they come; the tempo from the clocks counted over
// two seconds or so (read once a frame, so it settles rather than locks).
static void sync_in(struct os *s, struct ui *u, int byte) {
    int64_t now = now_ms(u);
    if (byte == 0xfa) seq_play(&s->seq, 1), s->clk_t0 = now, s->clk_n = 0;
    else if (byte == 0xfb) seq_play(&s->seq, 0);
    else if (byte == 0xfc) seq_stop(&s->seq, 0);
    if (byte != 0xf8) return;
    if (!s->clk_t0) s->clk_t0 = now, s->clk_n = 0;
    s->clk_n++;
    int64_t span = now - s->clk_t0;
    if (span < 2000) return;
    int tempo10 = (int)((double)s->clk_n / 24.0 * 60000.0 / (double)span * 10.0 + 0.5);
    pthread_mutex_lock(&s->seq.lock);
    int *t = &s->seq.sequence[s->seq.current].tempo10;
    if (tempo10 >= 300 && tempo10 <= 3000 && abs(tempo10 - *t) > 3) *t = tempo10;
    pthread_mutex_unlock(&s->seq.lock);
    s->clk_t0 = now, s->clk_n = 0;
}

// The footswitch: on its channel (or any) and CC, with its polarity. <Learn> takes all three from
// one press. AUTO works the polarity out as it goes: a pedal rests far longer than it is held, so
// whichever level lasts longer is "up" (before anything is known, the level it rested at until its
// first message is the other one). 1 when the message was the footswitch's.
static int foot_in(struct os *s, struct ui *u, int ch, int cc, int value) {
    struct midi_settings *M = &s->midi;
    if (cc == 64 || cc == M->foot_cc || s->foot_learn) // what a pedal sends: for the indicator
        s->foot_seen = 1, s->foot_last_ch = ch, s->foot_last_cc = cc, s->foot_last_val = value;
    if (s->foot_learn) {
        M->foot_chn = ch, M->foot_cc = cc, M->foot_pol = value >= 64 ? FPOL_NORMAL : FPOL_INVERTED;
        s->foot_learn = 0, s->foot_known = 0;
        char msg[40];
        snprintf(msg, sizeof msg, "Footswitch: Chn %d CC %d %s", ch, cc, value >= 64 ? "NORM" : "INV");
        toast(s, u, msg);
        os_save_settings(s);
        return 1;
    }
    if (M->sustain != SUS_FOOT || cc != M->foot_cc || (M->foot_chn && ch != M->foot_chn)) return 0;
    int high = value >= 64, first = !s->foot_known;
    if (!first && high == s->foot_high) return 1; // no change
    int64_t now = now_ms(u), was = first ? INT64_C(1) << 40 : now - s->foot_t;
    s->foot_dur[!high] = was; // the level that just ended (before the first message: the resting one)
    s->foot_known = 1, s->foot_high = high, s->foot_t = now;
    if (s->foot_dur[1] > s->foot_dur[0]) s->foot_inv = 1; // rests high: an inverted pedal
    else if (s->foot_dur[0] > s->foot_dur[1]) s->foot_inv = 0;
    // AUTO, and the first thing heard is "up": an inverted pedal pressed, or a keyboard resetting
    // its pedal as it connects. Taken as inverted, but not acted on: the next press tells.
    if (first && !high && M->foot_pol == FPOL_AUTO) return 1;
    int inv = M->foot_pol == FPOL_INVERTED || (M->foot_pol == FPOL_AUTO && s->foot_inv);
    if (high != inv) footswitch(s, u); // pressed
    return 1;
}

static void midi_in(struct os *s, struct ui *u, const unsigned char *m, int from) {
    const struct midi_settings *M = &s->midi;
    if (from == MIDI_PORT_EXT ? !M->ext : !M->usb) return; // that port is switched off
    if (m[0] >= 0xf8) { // Sync RECEIVE follows one port's clock, so two cannot fight
        if (M->sync == SYNC_RECEIVE && from == M->sync_in) sync_in(s, u, m[0]);
        return;
    }
    int st = m[0] & 0xf0, ch = (m[0] & 0x0f) + 1;
    if (st == 0xb0 && s->cc_learn && s->screen == OS_CCMAP) { // CC Map <Learn>
        ccmap_learned(s, u, m[1]);
        return;
    }
    if (st == 0x90 && m[2] && s->note_learn && s->screen == OS_PADNOTES) { // Pad Notes <Learn>
        seq_set_pad_note(s->pad_note_sel, m[1]);
        s->note_learn = 0;
        return;
    }
    if (M->thru && st >= 0x80 && st <= 0xe0) { // Soft Thru: out again, on the active track's channel
        int type, channel;
        active_track(s, &type, &channel);
        unsigned char o[3] = {(unsigned char)(st | ((channel > 0 ? channel : M->base) - 1)), m[1], m[2]};
        seq_send_raw(&s->seq, o, st == 0xc0 || st == 0xd0 ? 2 : 3);
    }
    if (st == 0xe0) {
        engine_set_bend(s->engine, (m[2] << 7 | m[1]) - 8192);
        return;
    }
    if (st == 0xb0 && foot_in(s, u, ch, m[1], m[2])) return;
    if (st == 0xb0) {
        if (m[1] == 1) engine_set_mod(s->engine, m[2]), infx_wah_pedal(s, m[2]); // the wah follows the mod wheel
        if (m[1] == 64) { // the sustain pedal
            int down = m[2] >= 64;
            if (M->sustain == SUS_FOOT) {
                // the footswitch is read below, on its own channel and CC
            } else if (!down && s->sus_down) {
                s->sus_down = 0;
                sustain_up(s);
            }
            s->sus_down = down;
        }
        if (M->mixer_cc) // the mixer: on the bank's strips by channel, or the selected one on Base Chn
            for (int c = 0; c < MCC_COUNT; c++) {
                if (M->cc[c] != m[1]) continue;
                int track = M->cc_target == CCT_BANK ? s->bank * 16 + ch - 1 : ch == M->base ? s->mixer_track : -1;
                if (track >= 0 && track < SEQ_TRACKS) seq_mixer_cc(s, track, c, m[2]);
            }
        return;
    }
    if (st != 0x90 && st != 0x80) return;
    if (M->recv && ch != M->recv) return; // Receive Chn
    int key = m[1] & 127, vel = st == 0x90 ? m[2] : 0;
    release_note(s, s->midi_held[key] ? s->midi_held[key] - 1 : -1, s->midi_key[key], s->midi_rec[key], s->midi_key[key]);
    s->midi_held[key] = 0;
    s->midi_rec[key] = -1;
    if (!vel) return;
    if (s->screen == OS_ARPEDIT) { // Pattern Edit: the key enters its note at the step, as a pad does
        int tune = arpedit_key(s, key);
        if (tune != ARPEDIT_SILENT) {
            int id = play_sound(s, s->selected, tune, 100, os_track_type(s));
            if (id >= 0) s->midi_held[key] = id + 1;
            s->midi_key[key] = tune;
        }
        return;
    }
    seq_wait_start(&s->seq);
    multi_note_in(s, key); // on the MULTI screen: the keygroup it falls in
    record_note_in(s, key); // the Auto Sampler asking a sample's note
    int type = os_track_type(s);
    if (type == TRACK_MIDI) return;
    int pad, tune = 0;
    if (s->levels_on || os_plays_s600(s)) {
        pad = s->levels_on ? s->levels_pad : os_instrument(s, s->program);
        // From the sound's ROOT (an S600 pad); a MULTI instrument's keygroups take the note itself.
        int root = s->levels_on || s->programs[s->program].s600 == PROG_MULTI ? 60 : s->pads[pad].sp.root;
        if (!s->levels_on || s->levels_type == LEVELS_TUNE) tune = key - root < -64 ? -64 : key - root > 63 ? 63 : key - root;
    } else {
        pad = seq_note_pad(key); // the pad note map
        if (pad < 0 || pad >= OS_PADS) return;
    }
    int id = play_sound(s, pad, tune, vel, type);
    if (id >= 0) s->midi_held[key] = id + 1;
    s->midi_key[key] = tune;
    if (seq_record_hit_tuned(&s->seq, pad, tune, vel) == 0) s->midi_rec[key] = pad;
}

static void pad_up(struct os *s, int i) {
    if (s->sounding[i] >= 0 || s->rec_note[i] >= 0) { // the sustain pedal may hold them back
        release_note(s, s->sounding[i], s->sounding_key[i], s->rec_note[i], s->rec_tune[i]);
        s->sounding[i] = s->rec_note[i] = -1;
    }
    if (s->live_midi[i] >= 0) {
        seq_live_midi(&s->seq, s->live_midi[i] >> 8, s->live_midi[i] & 0x7f, 0);
        s->live_midi[i] = -1;
    }
    if (s->rec_note[i] >= 0) {
        seq_record_release_tuned(&s->seq, s->rec_note[i], s->rec_tune[i]);
        s->rec_note[i] = -1;
    }
    if (s->erase_pad == i) {
        seq_set_erase(&s->seq, -1);
        s->erase_pad = -1;
    }
    if (s->repeat_pad[i]) {
        seq_repeat_release(&s->seq, i);
        s->repeat_pad[i] = 0;
    }
}

// A pad plays its sample, or with a MIDI track active sends its note out (except on the
// Sample screen, where it plays the sample so you can hear what you edit). On every screen
// it records into the active track while the sequencer records.
static void pad_down(struct os *s, struct ui *u, const struct input *in, int i) {
    if (in->shift) {
        shift_pad(s, u, i);
        return;
    }
    if (s->screen == OS_MUTE) { // the Track Mute page: a pad mutes, solos or queues; it does not play
        mute_pad(s, u, i);
        return;
    }
    if (s->screen == OS_SONG) { // the Song page: a pad adds its sequence as a step
        song_pad(s, u, i);
        return;
    }
    if (s->screen == OS_SEQCOPY) { // Copy Sequence: a pad picks the sequence to copy to
        seq_copy_pad(s, s->bank * 16 + i);
        return;
    }
    if (s->screen == OS_ARPEDIT) { // Pattern Edit: pads enter notes and set the step, heard on the selected pad
        int tune = arpedit_pad(s, i);
        if (tune != ARPEDIT_SILENT) {
            s->sounding[i] = play_sound(s, s->selected, tune, 100, os_track_type(s));
            s->sounding_key[i] = tune;
        }
        return;
    }
    if (record_pad(s, u, s->bank * 16 + i)) return; // a finished take waiting for its pad
    if (seq_program_pad(s, u, s->bank * 16 + i)) return; // the other pad of a Copy or Swap
    if (zones_pad(s, u, s->bank * 16 + i)) return;       // Zone Assign: starting the sound, marking zones
    if (s->extract && s->screen == OS_SAMPLE) { // Extract's prompt: the pad pressed gets the new sound
        int to = s->bank * 16 + i;
        os_undo_save(s, "Extract");
        os_pad_load_over(s, to, s->extract, s->extract_name);
        s->extract = 0;
        select_pad(s, to);
        char msg[32];
        snprintf(msg, sizeof msg, "Assigned to: Pad %c%02d", 'A' + to / 16, to % 16 + 1);
        toast(s, u, msg);
        return;
    }
    int slot = s->bank * 16 + i, type, channel;
    active_track(s, &type, &channel);
    int note = type == TRACK_MIDI ? seq_pad_note(slot) : slot, tune = 0;
    int vel = s->full_level ? 127 : in->pad_hit[i];
    if (s->screen == OS_LEVELS) { // a hit sets the root (Tune) or the sound (Velocity)
        if (s->levels_type == LEVELS_TUNE) s->levels_root = slot;
        else s->levels_pad = slot, s->levels_pad_set = 1;
    }
    if (s->levels_on && type != TRACK_MIDI) {
        note = s->levels_pad;
        if (s->levels_type == LEVELS_TUNE) tune = slot - s->levels_root < -64 ? -64 : slot - s->levels_root > 63 ? 63 : slot - s->levels_root;
        else vel = (i + 1) * 8 - 1; // pad 1 the softest, pad 16 the hardest
    }
    if (in->held & (1u << IN_ERASE)) { // erases this pad's notes as the sequence plays past them
        if (!s->erase_used) os_undo_save(s, "Erase"); // once for each hold of ERASE
        seq_set_erase(&s->seq, note);
        s->erase_used = 1;
        s->erase_pad = i;
        return;
    }
    if (s->screen == OS_MIXER || s->screen == OS_SENDS || s->screen == OS_INSERT) { // on the mixer a pad only picks its track's channel
        // ...unless a knob or the slider is being worked: then it is the panel shaking, not a press
        if (!os_ctl_busy(s)) seq_mixer_pad(s, slot);
        return;
    }
    if (s->screen == OS_TC) seq_tc_pad(s, note);
    if (s->screen == OS_ERASE) seq_erase_pad(s, note);
    if (s->screen == OS_COPYEV) seq_copyev_pad(s, note);
    if (s->screen == OS_OUTS && s->outs_by_track) { // Source TRACKS: a pad only picks its track, as on the mixer
        if (!os_ctl_busy(s)) seq_outs_pad(s, slot);
        return;
    }
    if (s->screen == OS_OUTS) seq_outs_pad(s, slot); // Source PADS: the pad picked, and heard
    if (padnotes_pad(s, u, slot)) return; // Pad Notes: a pad picks the one to set
    infx_pad_pressed(s, slot);            // Pad Effects: the chain goes to the pad pressed
    multi_pad(s, slot);                   // MULTI, the cursor on a Zone: that pad is the zone
    if (in->held & (1u << IN_SAMPLE)) { // SAMPLE + pad: selects it only; nothing plays, repeats, goes out or records
        select_pad(s, type == TRACK_MIDI ? slot : note);
        return;
    }
    // Step Edit: the Grid's pads select a row only; the list's put a note in at Now, while
    // stopped (running, a pad plays and records as anywhere, so one press is one note).
    int stepped = s->screen == OS_STEP && !s->step_grid && s->seq.mode == SEQ_STOPPED;
    if (s->screen == OS_STEP && s->step_grid) step_grid_pad(s, note, tune, vel);
    else if (stepped) step_pad(s, note, tune, vel);
    int midi = type == TRACK_MIDI && s->screen != OS_SAMPLE && s->screen != OS_LOOP && s->screen != OS_S600;
    int record = !stepped; // (SAMPLE + pad, which only selects, has returned above)
    seq_wait_start(&s->seq); // waiting for a key: this hit starts the recording
    if (!s->midi.local && type != TRACK_MIDI && !s->seq.repeat) { // Local Mode off: MIDI out, no sound here
        int ch = channel > 0 ? channel : s->midi.base, out = s->levels_on || os_plays_s600(s) ? 60 + tune : seq_pad_note(note);
        out = out < 0 ? 0 : out > 127 ? 127 : out;
        select_pad(s, note);
        seq_live_midi(&s->seq, ch, out, vel);
        s->live_midi[i] = ch << 8 | out;
        if (record && seq_record_hit_tuned(&s->seq, note, tune, vel) == 0) s->rec_note[i] = note, s->rec_tune[i] = tune;
        return;
    }
    if (s->seq.repeat) { // Note Repeat plays the pad on the grid instead of now
        if (!midi) select_pad(s, note);
        if (!midi || channel > 0) {
            seq_repeat_press_tuned(&s->seq, i, note, tune, vel, midi ? channel : 0, record);
            s->repeat_pad[i] = 1;
        }
        return;
    }
    if (midi) {
        if (channel > 0) {
            seq_live_midi(&s->seq, channel, note, vel);
            s->live_midi[i] = channel << 8 | note;
        }
    } else {
        select_pad(s, note);
        // A press with no release seen since the last (a missed release, or the bank or 16 Levels
        // root changed under a held pad): the note it was holding goes first, so none is left on.
        if (s->sounding[i] >= 0) {
            release_note(s, s->sounding[i], s->sounding_key[i], -1, 0);
            s->sounding[i] = -1;
        }
        int id = play_sound(s, note, tune, vel, type);
        if (id >= 0) s->sounding[i] = id, s->sounding_key[i] = tune;
        if (id >= 0 && s->nlive_on < 32) s->live_on[s->nlive_on].id = id, s->live_on[s->nlive_on++].key = tune;
    }
    if (!record) return;
    if (seq_record_hit_tuned(&s->seq, note, tune, vel) == 0) s->rec_note[i] = note, s->rec_tune[i] = tune;
}

static void handle_pads(struct os *s, struct ui *u, const struct input *in) {
    int playable = s->screen != OS_TITLE && s->screen != OS_SHUTDOWN; // every working screen
    for (int i = 0; i < 16; i++) { // how hard each pad is held, for the pad lights
        if (in->pad_hit[i] >= 0) s->pad_level[i] = in->pad_hit[i] > 0 ? in->pad_hit[i] : 1;
        if (in->pad_pressure[i] > 0 && s->pad_level[i]) s->pad_level[i] = in->pad_pressure[i];
        if (in->pad_release[i] && !in->pad_down[i]) s->pad_level[i] = 0;
    }
    for (int i = 0; i < 16; i++) {
        int released = in->pad_release[i];
        // A release can land before or after a hit in the same read.
        if (released && in->pad_down[i]) pad_up(s, i);
        if (in->pad_hit[i] >= 0 && playable) pad_down(s, u, in, i);
        // As on the MPC3000, pressing harder on a repeating pad makes the repeats louder.
        if (s->repeat_pad[i] && in->pad_pressure[i] > 0 && !s->full_level)
            seq_repeat_pressure(&s->seq, i, in->pad_pressure[i]);
        if (released && !in->pad_down[i]) pad_up(s, i);
    }
    // Every pad up, no key or pedal holding: each note the pads started is let go once more, so a
    // release the panel never sent cannot leave an S600 (or 16 Levels) note sounding.
    if (s->nlive_on) {
        int held = s->sus_down;
        for (int i = 0; i < 16 && !held; i++) held = in->pad_down[i] || s->sounding[i] >= 0;
        for (int k = 0; k < 128 && !held; k++) held = s->midi_held[k] != 0;
        if (!held) {
            for (int k = 0; k < s->nlive_on; k++) engine_note_off_key(s->engine, s->live_on[k].id, s->live_on[k].key);
            s->nlive_on = 0;
        }
    }
}

static int knob_position(const struct param *d, const struct os_pad *pad) {
    int max = param_max(d, pad);
    if (max <= d->min) return 0;
    return (int)(((int64_t)*param_ptr((struct pad_params *)&pad->p, d) - d->min) * 127 / (max - d->min));
}

// A knob takes over only after it reaches the current value, so it never jumps.
// Zoomed in, a Zone or Loop point's knob turns finer: each step a 127th of the view, not of the
// whole sound, moving the point from where it is (no jump). 0 when not zoomed.
static int zoom_step(const struct os *s, const struct os_pad *pad) {
    if (!s->zoom || !pad->s) return 0;
    int64_t width = (int64_t)pad->s->frames >> s->zoom;
    if (width < OS_WAVE_W / 2) width = OS_WAVE_W / 2;
    int step = (int)(width / 127);
    return step < 1 ? 1 : step;
}

static void handle_knobs(struct os *s, const struct input *in) {
    struct os_pad *pad = &s->pads[s->selected];
    for (int i = 0; i < 3; i++) {
        if (!in->knob_moved[i]) continue;
        const struct param *d = &PAGES[s->page][i];
        int last = s->knob_last[i], now = in->knob[i];
        s->knob_last[i] = now;
        if (s->page == PAGE_TRIM) {
            if (d->fmt == F_DO) { // turning K3 takes the cursor to Do and scrolls through the list
                s->frame_slot = i;
                s->do_op = now * DO_COUNT / 128;
                continue;
            }
            s->frame_slot = 0; // any other knob takes it back to Zone Start
        }
        if (!d->name || !pad->s) continue;
        int fine = d->fmt == F_FRAMES ? zoom_step(s, pad) : 0;
        if (fine && last >= 0) { // zoomed: fine steps from where the point is
            param_set(s, d, *param_ptr(&pad->p, d) + (now - last) * fine);
            s->knob_caught[i] = 0; // zoomed back out, it catches up again
            continue;
        }
        int pos = os_knob_take(s, &s->knob_caught[i], last, now, knob_position(d, pad));
        if (pos >= 0) {
            int max = param_max(d, pad);
            param_set(s, d, d->min + (int)((int64_t)(max - d->min) * pos / 127));
        }
    }
}

// The fader. On Trim it steps Start through the detected transients. On every other page
// it moves the framed parameter, taking over once it reaches the value like the knobs.
// Which recordable value the cursor is on, as AUTO_*, or -1.
static int auto_param(int page, int slot) {
    if (slot < 0 || slot > 1) return -1;
    if (page == PAGE_MIX) return slot == 0 ? AUTO_VOL : AUTO_PAN;
    if (page == PAGE_FILTER) return slot == 0 ? AUTO_CUTOFF : AUTO_RESO;
    if (page == PAGE_TUNE) return slot == 0 ? AUTO_SEMI : AUTO_FINE;
    return -1;
}

static const char *const AUTO_NAMES[AUTO_PARAMS] = {"Volume", "Pan", "Cutoff", "Reson", "Semi", "Fine"};

static void handle_fader(struct os *s, const struct input *in) {
    if (!in->fader_moved) return;
    struct os_pad *pad = &s->pads[s->selected];
    int last = s->fader_last, now = in->fader;
    s->fader_last = now;
    if (!pad->s) return;
    if (s->page == PAGE_TRIM) {
        int n = pad->nonsets;
        if (n < 1) return;
        pad->p.start = pad->onsets[(now * (n - 1) + 63) / 127];
        s->frame_slot = 0;
        pad_changed(s, s->selected);
        return;
    }
    if (s->frame_slot < 0) return;
    const struct param *d = &PAGES[s->page][s->frame_slot];
    if (!d->name) return;
    int pos = os_knob_take(s, &s->fader_caught, last, now, knob_position(d, pad));
    if (pos < 0) return;
    param_set(s, d, d->min + (int)((int64_t)(param_max(d, pad) - d->min) * pos / 127));
    // While recording, moves on Volume, Pan, Cutoff, Reson, Semi and Fine go into the sequence.
    int param = auto_param(s->page, s->frame_slot);
    if (param >= 0) seq_record_auto(&s->seq, s->selected, param, pos);
}

static void preview_schedule(struct os *s, const struct ui *u);

#define SETTING_HOME "sample_home="
#define SETTING_MIDI "midi="
#define SETTING_NOTES "pad_notes="
#define SETTING_INPUT "input_mic=" // the name kept: 0 LINE, 1 MIC, 2 USB
#define SETTING_FX "input_fx="
#define SETTING_PREFS "prefs="
#define SETTING_ARP "arp_user="

void os_load_settings(struct os *s) {
    if (!s->settings_path) return;
    FILE *f = fopen(s->settings_path, "r");
    if (!f) return;
    char line[FILES_PATH + 32];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, SETTING_HOME, strlen(SETTING_HOME)) == 0)
            snprintf(s->home_dir, sizeof s->home_dir, "%s", line + strlen(SETTING_HOME));
        if (strncmp(line, SETTING_MIDI, strlen(SETTING_MIDI)) == 0) {
            struct midi_settings m = s->midi;
            int n = sscanf(line + strlen(SETTING_MIDI), "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                           &m.ext, &m.base, &m.recv, &m.sync, &m.local, &m.thru, &m.sustain, &m.foot, &m.mixer_cc,
                           &m.cc_target, &m.cc[0], &m.cc[1], &m.cc[2], &m.cc[3], &m.cc[4], &m.cc[5], &m.cc[6], &m.usb,
                           &m.sync_in, &m.foot_chn, &m.foot_cc, &m.foot_pol);
            if (n == 17 || n == 19 || n == 22) s->midi = m; // 17: before USB MIDI; 19: before the footswitch's own
            s->midi.foot = s->midi.foot < 0 || s->midi.foot >= FOOT_MODES ? FOOT_PLAY : s->midi.foot;
            s->midi.foot_chn = s->midi.foot_chn < 0 || s->midi.foot_chn > 16 ? 0 : s->midi.foot_chn;
            s->midi.foot_cc = s->midi.foot_cc < 0 || s->midi.foot_cc > 127 ? 64 : s->midi.foot_cc;
            s->midi.foot_pol = s->midi.foot_pol < 0 || s->midi.foot_pol >= FPOL_COUNT ? FPOL_AUTO : s->midi.foot_pol;
        }
        if (strncmp(line, SETTING_FX, strlen(SETTING_FX)) == 0) infx_load_line(s, line + strlen(SETTING_FX));
        if (strncmp(line, SETTING_PREFS, strlen(SETTING_PREFS)) == 0) prefs_load_line(s, line + strlen(SETTING_PREFS));
        if (strncmp(line, SETTING_INPUT, strlen(SETTING_INPUT)) == 0) {
            int src = atoi(line + strlen(SETTING_INPUT));
            s->input_src = src >= 0 && src < INPUT_SOURCES ? src : INPUT_LINE;
        }
        if (strncmp(line, SETTING_ARP, strlen(SETTING_ARP)) == 0) { // "arp_user=3:16,..." USER 3's steps
            const char *p = line + strlen(SETTING_ARP), *c = strchr(p, ':');
            int slot = atoi(p) - 1;
            if (c && slot >= 0 && slot < ARP_USER) seq_arp_steps_load(&s->seq.arp_user[slot], c + 1);
        }
        if (strncmp(line, SETTING_NOTES, strlen(SETTING_NOTES)) == 0) {
            const char *p = line + strlen(SETTING_NOTES);
            for (int pad = 0; pad < 128 && *p; pad++) {
                seq_set_pad_note(pad, atoi(p));
                p = strchr(p, ',');
                if (!p) break;
                p++;
            }
        }
    }
    fclose(f);
}

// Written straight to the card: the MPC can lose power at any moment.
static int save_settings(const struct os *s) {
    if (!s->settings_path) return -1;
    FILE *f = fopen(s->settings_path, "w");
    if (!f) return -1;
    fprintf(f, "%s%s\n", SETTING_HOME, s->home_dir);
    const struct midi_settings *m = &s->midi;
    fprintf(f, "%s%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", SETTING_MIDI, m->ext, m->base, m->recv, m->sync, m->local, m->thru,
            m->sustain, m->foot, m->mixer_cc, m->cc_target);
    for (int c = 0; c < MCC_COUNT; c++) fprintf(f, ",%d", m->cc[c]);
    fprintf(f, ",%d,%d,%d,%d,%d", m->usb, m->sync_in, m->foot_chn, m->foot_cc, m->foot_pol);
    fprintf(f, "\n%s%d", SETTING_INPUT, s->input_src);
    char fx[1600];
    infx_save_line(s, fx, sizeof fx);
    fprintf(f, "\n%s%s", SETTING_FX, fx);
    prefs_save_line(s, fx, sizeof fx);
    fprintf(f, "\n%s%s", SETTING_PREFS, fx);
    for (int a = 0; a < ARP_USER; a++) {
        seq_arp_steps_save(&s->seq.arp_user[a], fx, sizeof fx);
        fprintf(f, "\n%s%d:%s", SETTING_ARP, a + 1, fx);
    }
    fprintf(f, "\n%s", SETTING_NOTES);
    for (int p = 0; p < 128; p++) fprintf(f, p ? ",%d" : "%d", seq_pad_note(p));
    fprintf(f, "\n");
    fflush(f);
    fsync(fileno(f));
    return fclose(f);
}

int os_save_settings(const struct os *s) {
    return save_settings(s);
}

static void set_home(struct os *s, const struct ui *u) {
    if (!s->browser.cwd[0]) {
        toast(s, u, "Open a folder first");
        return;
    }
    snprintf(s->home_dir, sizeof s->home_dir, "%s", s->browser.cwd);
    toast(s, u, save_settings(s) == 0 || !s->settings_path ? "Home folder set" : "Could not save to card");
}

static void open_browser(struct os *s, const struct ui *u) {
    static const char *const wav[] = {".wav", 0}; // any case
    files_init(&s->browser, s->browser_root, wav);
    // The last folder this session, else the home folder. A folder that has gone, such as a
    // removed card, leaves the storage list showing.
    if (s->browser_dir[0]) {
        if (files_open(&s->browser, s->browser_dir) == 0) {
            int last = s->browser.count - 1;
            s->browser.cursor = s->browser_cursor < last ? s->browser_cursor : last < 0 ? 0 : last;
        }
    } else if (s->home_dir[0]) {
        files_open(&s->browser, s->home_dir);
    }
    s->screen = OS_BROWSER;
    preview_schedule(s, u);
}

// The erase prompt for recorded fader moves takes the function buttons while it is open.
// 1 when it used this frame's input.
static int erase_prompt(struct os *s, const struct ui *u, const struct input *in) {
    if ((in->up & (1u << IN_ERASE)) && !s->erase_used && !s->erase_ask) {
        int param = auto_param(s->page, s->frame_slot);
        if (param >= 0 && s->pads[s->selected].s) {
            s->erase_this = seq_auto_count(&s->seq, s->selected, param) > 0;
            if (s->erase_this || seq_auto_count(&s->seq, s->selected, -1) > 0) s->erase_ask = param + 1;
        }
    }
    if (!s->erase_ask) return 0;
    int all = input_down(in, IN_B2), one = input_down(in, IN_B3) && s->erase_this;
    if (input_down(in, IN_B1) || all || one) { // <No> only closes it
        if (all || one) os_undo_save(s, "Erase");
        int erased = all || one ? seq_erase_autos(&s->seq, s->selected, all ? -1 : s->erase_ask - 1) : 0;
        char msg[40];
        if (all || one) snprintf(msg, sizeof msg, "Erased %d changes", erased);
        if (all || one) toast(s, u, msg);
        s->erase_ask = 0;
    }
    return 1;
}

// Steps a pad's sound d places through every sound in memory.
void os_pad_step_sound(struct os *s, int slot, int d) {
    static struct sample *list[OS_SOUND_LIST];
    static const char *names[OS_SOUND_LIST];
    // The list leaves out this pad's own place, so its order stays the same as the pad's sound
    // changes (in the pad's place, the sound just chosen would come first again and the next step
    // would go back: two sounds, back and forth). A sound found only on this pad goes to the end
    // of memory when it leaves, so it counts as last.
    int n = sounds_except(s, list, names, OS_SOUND_LIST, s->program, slot), at = n;
    if (n == 0) return;
    for (int i = 0; i < n && at == n; i++)
        if (list[i] == s->pads[slot].s) at = i;
    int places = n + (at == n); // with this pad's own sound, when only it has it
    int to = s->pads[slot].s ? ((at + d) % places + places) % places : d > 0 ? 0 : n - 1;
    if (to < n && list[to] != s->pads[slot].s) os_pad_assign(s, slot, list[to], names[to]);
}

// SHIFT + the wheel on Snd: (Program Config) and Sample: (the sound's page): only the sounds in
// memory that no pad of any program plays, in the order they came into memory, with this pad's
// own sound among them when it is one. The pad's sound it lets go of stays in memory, so it is
// one of them next time.

int os_pad_step_free(struct os *s, int slot, int d) {
    int list[OS_MEMORY], n = 0, at = -1;
    for (int m = 0; m < s->nmemory; m++) {
        if (on_a_pad(s, s->memory[m].s, s->program, slot)) continue;
        int dup = 0;
        for (int k = 0; k < n && !dup; k++) dup = s->memory[list[k]].s == s->memory[m].s;
        if (dup) continue;
        if (s->memory[m].s == s->pads[slot].s) at = n;
        list[n++] = m;
    }
    if (n == 0 || (n == 1 && at == 0)) return 0;
    int to = at < 0 ? (d > 0 ? 0 : n - 1) : ((at + d) % n + n) % n;
    if (to == at) return 1;
    os_pad_assign(s, slot, s->memory[list[to]].s, s->memory[list[to]].name);
    return 1;
}

static void handle_sample_screen(struct os *s, const struct ui *u, const struct input *in) {
    if (erase_prompt(s, u, in)) return;
    if (s->extract) { // the Extract prompt takes the keys until it is answered
        if (input_down(in, IN_B1)) {
            sample_free(s->extract); // never reached the engine, so it can go at once
            s->extract = 0;
        } else if (in->encoder_press || input_down(in, IN_B3)) {
            os_undo_save(s, "Extract");
            if (os_memory_add(s, s->extract, s->extract_name) != 0) {
                toast(s, u, "Memory full");
            } else {
                s->extract = 0;
                toast(s, u, "Saved to memory");
            }
        }
        return;
    }
    int zooming = (in->held & (1u << IN_SAMPLE)) != 0; // SAMPLE held turns B1 and B2 into zoom
    if (zooming && (input_down(in, IN_B1) || input_down(in, IN_B2))) {
        if (input_down(in, IN_B1) && s->zoom < OS_ZOOM_MAX) s->zoom++;
        if (input_down(in, IN_B2) && s->zoom > 0) s->zoom--;
        char msg[24];
        snprintf(msg, sizeof msg, "Zoom: x%d", 1 << s->zoom);
        toast(s, u, msg);
    }
    if (s->s600_trim && s->page != PAGE_TRIM) set_page(s, PAGE_TRIM);
    if (s->s600_trim && !zooming && !in->shift) { // S600 Trim: the keys go back to the S600 pages
        int on_do = s->frame_slot >= 0 && PAGES[s->page][s->frame_slot].fmt == F_DO;
        int to = input_down(in, IN_B1) ? 0 : input_down(in, IN_B2) ? 1 : input_down(in, IN_B3) && !on_do ? 2 : -1;
        if (to >= 0) {
            static const int pages[3] = {0, 2, 4}; // Env1, Filter, Main
            s->s600_page = pages[to];
            s->s600_trim = 0;
            s600_open(s);
            return;
        }
    }
    if (!zooming && input_down(in, IN_B1)) {
        if (in->shift) toggle(s, u, offsetof(struct pad_params, loop_lock), "Loop Lock: On", "Loop Lock: Off");
        else press_tab(s, 0);
    }
    if (!zooming && input_down(in, IN_B2)) {
        if (in->shift) {
            os_undo_save(s, "Normalize");
            normalize_sample(s, u);
        }
        else press_tab(s, 1);
    }
    int on_do = s->page == PAGE_TRIM && s->frame_slot >= 0 && PAGES[s->page][s->frame_slot].fmt == F_DO;
    if (!zooming && input_down(in, IN_B3) && in->shift) { // Edit All Pads on or off
        s->edit_all = !s->edit_all;
        toast(s, u, s->edit_all ? "Edit All Pads: On" : "Edit All Pads: Off");
    }
    if (!zooming && input_down(in, IN_B3) && !in->shift) {
        if (on_do) zone_op(s, u);
        else press_tab(s, 2);
    }
    if (input_down(in, IN_SAMPLE) && !in->shift) first_frame(s); // pressed again: the first parameter

    if (input_down(in, IN_CHOP) && in->shift)
        toggle(s, u, offsetof(struct pad_params, gate), "Note On: On", "Note On: Off");
    if (in->encoder_press) {
        move_frame(s, in->shift ? -1 : 1);
    } else if (s->frame_slot < 0) { // Sample: the wheel, - and + step the pad through the sounds in memory
        int d = in->encoder + (in->shift ? 0 : input_down(in, IN_PLUS) - input_down(in, IN_MINUS));
        if (d && in->shift && in->encoder) { // only the sounds no pad plays
            if (!os_pad_step_free(s, s->selected, in->encoder)) toast(s, u, "No unused sounds in memory");
        } else if (d) {
            os_pad_step_sound(s, s->selected, d);
        }
    }
    const struct param *framed = s->frame_slot >= 0 ? &PAGES[s->page][s->frame_slot] : &PAGES[s->page][0];
    if (s->frame_slot >= 0 && framed->name && in->encoder && !in->encoder_press) param_nudge(s, framed, in->encoder, in->shift);
    if (s->frame_slot >= 0 && framed->name) {
        if (input_down(in, IN_PLUS) && !in->shift) param_nudge(s, framed, 1, 1);
        if (input_down(in, IN_MINUS) && !in->shift) param_nudge(s, framed, -1, 1);
    }
    handle_knobs(s, in);
    handle_fader(s, in);
}

// rate is the sample's rate, for showing frame positions as time.
static void format_value(const struct param *d, int v, int rate, char *buf, size_t n) {
    switch (d->fmt) {
    case F_FRAMES: {
        // seconds.milliseconds.hundredths of a millisecond, like 000.500.00 for half a second
        int64_t units = rate > 0 ? (int64_t)v * 100000 / rate : 0;
        snprintf(buf, n, "%03d.%03d.%02d", (int)(units / 100000 % 1000), (int)(units / 100 % 1000), (int)(units % 100));
        break;
    }
    case F_PAN:
        if (v == 0) snprintf(buf, n, "C");
        else snprintf(buf, n, "%c%d", v < 0 ? 'L' : 'R', v < 0 ? -v : v);
        break;
    case F_MS:
        if (v == 0 && d->off == offsetof(struct pad_params, decay_ms)) snprintf(buf, n, "Off");
        else snprintf(buf, n, "%dms", v);
        break;
    case F_SEMI:
    case F_CENTS:
    case F_DEPTH:
        snprintf(buf, n, "%+d", v);
        break;
    case F_MONO:
        snprintf(buf, n, "%s", v ? "MONO" : "POLY");
        break;
    case F_GROUP:
        if (v == 0) snprintf(buf, n, "Off");
        else snprintf(buf, n, "%d", v);
        break;
    case F_FILTER:
        snprintf(buf, n, "%s", FILTER_NAMES[v >= 0 && v < FILTER_TYPES ? v : 0]);
        break;
    case F_PAD:
        if (v <= 0) snprintf(buf, n, "Off");
        else snprintf(buf, n, "%c%02d", 'A' + (v - 1) / 16, (v - 1) % 16 + 1);
        break;
    case F_DCYMD:
        snprintf(buf, n, "%s", v ? "End" : "Start");
        break;
    default:
        snprintf(buf, n, "%d", v);
        break;
    }
}

static void draw_flag(struct ui *u, int col, const char *label, int on) {
    if (on) lcd_inverse(u, col, 3, label);
    else lcd_text(u, col, 3, "-");
}

static void marker(struct ui *u, int x, int gap, uint32_t color) {
    x &= ~1;
    for (int y = WAVE_Y; y < WAVE_Y + WAVE_H; y += 2)
        if (!gap || ((y - WAVE_Y) / 2) % 3 != 2) lcd_dot(u, WAVE_X + x, y, color);
}

// The lowest and highest level in frames [a, b), scaled to the waveform's half height.
static void column_peaks(const struct sample *sm, int64_t a, int64_t b, int *lo, int *hi) {
    float l = 0, h = 0;
    if (b <= a) b = a + 1;
    if (a < 0) a = 0; // past either end of the sound (the zoomed view keeps its point in the middle): nothing
    for (int64_t i = a; i < b && i < sm->frames; i++) {
        for (int c = 0; c < sm->channels; c++) {
            float v = sm->data[i * sm->channels + c];
            if (v < l) l = v;
            if (v > h) h = v;
        }
    }
    int half = WAVE_H / 2 - 1;
    *lo = (int)((l < -1 ? -1 : l) * (float)half);
    *hi = (int)((h > 1 ? 1 : h) * (float)half);
}

// Which point the zoomed view follows: the one that moved last (start or end), else the one the
// cursor is on. Remembered between frames.
static int64_t follow_point(int64_t a, int64_t b, int cursor_on_b) {
    static int64_t last_a = -1, last_b = -1;
    static int on_b;
    if (last_a >= 0 && a != last_a) on_b = 0;
    else if (last_b >= 0 && b != last_b) on_b = 1;
    else if (last_a < 0) on_b = cursor_on_b;
    last_a = a, last_b = b;
    return on_b ? b : a;
}

// Where the selected pad's sound is playing now (its drum or its S600 note), or -1.
int64_t os_playhead(const struct os *s) {
    if (!s->engine || s->selected < 0 || s->selected >= ENGINE_PADS) return -1;
    int at = engine_play_pos(s->engine, s->selected);
    if (at < 0) at = engine_play_pos(s->engine, ENGINE_S600 + s->selected);
    return at;
}

// The playhead: a solid line down the waveform (the markers are dotted), and the screen drawn
// again soon while it moves.
void os_draw_playhead(struct ui *u, int x, int top, int h) {
    lcd_fill(u, x & ~1, top, 2, h, LCD_FG);
    ui_wake(u, 30);
}

// Frames [a, b) are drawn solid, the rest dotted, with solid markers at a and b and a dashed
// one at `dashed` (or none when it is negative). Zoomed in, the view centres on `focus`. The
// pad's sound playing: a solid line where it is.
static void draw_wave(struct ui *u, const struct os *s, const struct os_pad *pad, int64_t a, int64_t b,
                      int64_t focus, int64_t dashed) {
    if (!pad->s) {
        lcd_center(u, 6, "Empty pad");
        lcd_center(u, 8, "SAMPLE SELECT loads WAV");
        return;
    }
    const struct sample *sm = pad->s;
    int64_t frames = sm->frames, mid = WAVE_Y + WAVE_H / 2;
    int64_t width = frames >> s->zoom;
    if (width < OS_WAVE_W / 2) width = OS_WAVE_W / 2;
    if (width > frames) width = frames;
    int64_t w0 = focus - width / 2; // zoomed, the point stays in the middle, even near the ends
    if (width == frames) w0 = 0;
#define XPOS(f) ((int)(((int64_t)(f) - w0) * OS_WAVE_W / width))
    int start_x = XPOS(a), end_x = XPOS(b);
    for (int x = 0; x < OS_WAVE_W; x += 2) {
        int lo, hi;
        if (width == frames) {
            lo = pad->lo[x], hi = pad->hi[x];
        } else if (w0 + (int64_t)(x + 2) * width / OS_WAVE_W <= 0 || w0 + (int64_t)x * width / OS_WAVE_W >= frames) {
            continue; // before the sound starts or after it ends: blank
        } else {
            column_peaks(sm, w0 + (int64_t)x * width / OS_WAVE_W, w0 + (int64_t)(x + 2) * width / OS_WAVE_W, &lo, &hi);
        }
        int inside = x >= start_x && x < end_x;
        // The LCD has one color, so audio outside the zone is drawn dotted.
        for (int y = (int)(mid - hi) & ~1; y <= mid - lo; y += 2)
            if (inside || (x % 4 == 0 && y % 4 == 0)) lcd_dot(u, WAVE_X + x, y, LCD_FG);
    }
    for (int i = 1; i < pad->nonsets; i++) { // a short tick over each detected transient
        int x = XPOS(pad->onsets[i]);
        if (x >= 0 && x < OS_WAVE_W) lcd_dot(u, WAVE_X + (x & ~1), WAVE_Y - 4, LCD_FG);
    }
    if (start_x >= 0 && start_x < OS_WAVE_W) marker(u, start_x, 0, LCD_FG);
    if (end_x >= 0 && end_x <= OS_WAVE_W) marker(u, end_x < OS_WAVE_W ? end_x : OS_WAVE_W - 2, 0, LCD_FG);
    int dashed_x = XPOS(dashed);
    if (dashed >= 0 && dashed_x >= 0 && dashed_x < OS_WAVE_W) marker(u, dashed_x, 1, LCD_FG);
    int64_t head = os_playhead(s);
    int head_x = XPOS(head);
    if (head >= 0 && head < frames && head_x >= 0 && head_x < OS_WAVE_W) os_draw_playhead(u, WAVE_X + head_x, WAVE_Y, WAVE_H);
#undef XPOS
}

// One row per knob. `slot` is the framed one; do_name is what a Do field shows.
// The amp envelope's shape, as the XL draws it beside its envelope: up over the attack, level,
// then down over the decay (from the end of the attack, or timed to the end of the sound).
static void draw_envelope(struct ui *u, const struct os_pad *pad) {
    int x0 = 232, x1 = 314, top = PARAM_ROW * CELL_H + 2, bottom = (PARAM_ROW + 3) * CELL_H - 4;
    lcd_fill(u, x0, top, 2, bottom - top + 2, LCD_FG); // the frame's left side and floor, as on the XL
    lcd_fill(u, x0, bottom, x1 - x0, 2, LCD_FG);
    const struct pad_params *p = &pad->p;
    int a = x0 + 2 + p->attack_ms * 24 / 2000, d = p->decay_ms > 0 ? 8 + p->decay_ms * 40 / 5000 : 0;
    int fall = d ? (p->decay_end && !p->gate ? x1 - d : a) : x1; // where the level starts down
    if (fall < a) fall = a;
    int end = d ? fall + d : x1;
    if (end > x1) end = x1;
    lcd_line(u, x0 + 2, bottom, a, top + 2);
    lcd_line(u, a, top + 2, fall, top + 2);
    if (d) lcd_line(u, fall, top + 2, end, bottom);
}

static void draw_params(struct ui *u, const struct param *params, int slot, const char *do_name,
                        const struct os_pad *pad) {
    // The 2000XL layout's envelope page: the values just after their names, the shape beside them.
    int env = lcd_layout == LAYOUT_2000XL && params == PAGES[PAGE_AMP];
    if (env && pad->s) draw_envelope(u, pad);
    for (int i = 0; i < 3; i++) {
        const struct param *d = &params[i];
        int row = PARAM_ROW + i;
        char label[16];
        snprintf(label, sizeof label, "K%d", i + 1);
        lcd_text(u, 0, row, label);
        if (!d->name) {
            lcd_text(u, 3, row, "--");
            continue;
        }
        if (d->fmt == F_DO) {
            lcd_text(u, 3, row, "Do:");
            char field[LCD_COLS + 1];
            snprintf(field, sizeof field, " %s ", do_name);
            if (i == slot) lcd_inverse(u, 6, row, field);
            else lcd_text(u, 6, row, field);
            continue;
        }
        const char *name = d->name;
        if (d->off == offsetof(struct pad_params, decay_ms) && pad->p.gate) name = "Release";
        snprintf(label, sizeof label, "%s:", name);
        lcd_text(u, 3, row, label);
        char value[16] = "--";
        if (pad->s)
            format_value(d, *(const int32_t *)(const void *)((const char *)&pad->p + d->off), pad->s->rate, value,
                         sizeof value);
        char field[LCD_COLS + 1];
        snprintf(field, sizeof field, " %s ", value);
        int col = env ? 3 + (int)strlen(label) : VALUE_COL;
        if (i == slot) lcd_inverse(u, col, row, field);
        else lcd_text(u, col, row, field);
    }
}

static void draw_keys(struct ui *u, const struct os *s, const struct input *in) {
    static const char *const shifted[3] = {"<LpLock>", "<Normal>", "<AllPads>"};
    if (s->s600_trim && !(in->held & (1u << IN_SAMPLE))) { // the S600's keys: B1 its Env1, Env2, Trim
        int on_do = s->frame_slot >= 0 && PAGES[s->page][s->frame_slot].fmt == F_DO;
        if (in->shift) {
            for (int g = 0; g < 3; g++) lcd_text(u, g * 9, KEY_ROW, shifted[g]);
            return;
        }
        lcd_inverse(u, 0, KEY_ROW, "<Trim>");
        lcd_text(u, 9, KEY_ROW, "<Filter>");
        lcd_text(u, 18, KEY_ROW, on_do ? "<Do>" : "<Main>");
        return;
    }
    static const char *const names[PAGE_COUNT] = {"Trim", "Vol", "Tune", "Poly", "Envel", "Vel Mod", "Filter", "FltEnv"};
    if (in->held & (1u << IN_SAMPLE)) { // SAMPLE held: the keys zoom the waveform
        lcd_text(u, 0, KEY_ROW, "<Zoom In>");
        lcd_text(u, 10, KEY_ROW, "<Zoom Out>");
        return;
    }
    for (int g = 0; g < 3; g++) {
        int selected = group_of(s->page) == g, col = g * 9;
        char key[16];
        snprintf(key, sizeof key, "<%s>", names[selected ? s->page : s->group_page[g]]);
        if (g == 2 && s->page == PAGE_TRIM && s->frame_slot >= 0 && PAGES[s->page][s->frame_slot].fmt == F_DO)
            snprintf(key, sizeof key, "<Do>");
        if (in->shift) lcd_text(u, col, KEY_ROW, shifted[g]);
        else if (selected) lcd_inverse(u, col, KEY_ROW, key);
        else lcd_text(u, col, KEY_ROW, key);
    }
}

static void draw_sample_screen(struct ui *u, const struct os *s, const struct input *in) {
    const struct os_pad *pad = &s->pads[s->selected];
    char line[LCD_COLS + 8];
    if (s->edit_all) snprintf(line, sizeof line, "P%02d ALL Edit Snd", s->program + 1);
    else snprintf(line, sizeof line, "P%02d %c%02d Edit Snd", s->program + 1, 'A' + s->selected / 16, s->selected % 16 + 1);
    if (s->extract) {
        lcd_text(u, 0, KEY_ROW, "<Cancel>");
        lcd_text(u, 16, KEY_ROW, "<Save Snd>");
    } else if (s->erase_ask) {
        lcd_text(u, 0, KEY_ROW, "<No>");
        lcd_text(u, 10, KEY_ROW, "<All>");
        if (s->erase_this) lcd_text(u, 21, KEY_ROW, "<Yes>");
    } else {
        draw_keys(u, s, in);
    }
    lcd_rule(u, TITLE_ROW, line);
    lcd_text(u, 0, 3, "Sample:");
    snprintf(line, sizeof line, "%.12s", pad->s ? pad->name : "(empty)");
    if (s->frame_slot < 0) lcd_inverse(u, 7, 3, line);
    else lcd_text(u, 7, 3, line);
    draw_flag(u, 21, "N", pad->s && pad->p.gate);
    draw_flag(u, 23, "R", pad->s && pad->p.reverse);
    draw_flag(u, 25, "L", pad->s && pad->p.loop_on);
    if (pad->s) {
        const struct pad_params *p = &pad->p;
        draw_wave(u, s, pad, p->start, p->end, follow_point(p->start, p->end, s->page == PAGE_TRIM && s->frame_slot == 1),
                  p->loop_on ? p->loop : -1);
    } else {
        draw_wave(u, s, pad, 0, 0, 0, -1);
    }
    lcd_rule(u, PARAM_ROW - 1, "Sound Params");
    draw_params(u, PAGES[s->page], s->frame_slot, DO_NAMES[s->do_op], pad);
    if (s->extract) { // the Extract prompt sits over the waveform
        lcd_inverse(u, 0, 5, " Press pad to assign, or  ");
        lcd_inverse(u, 0, 6, " ENTER to save to memory  ");
    }
    if (s->erase_ask) { // the erase prompt sits over the waveform
        char lines[3][LCD_COLS + 1], id[8];
        snprintf(id, sizeof id, "%c%02d", 'A' + s->selected / 16, s->selected % 16 + 1);
        snprintf(lines[0], sizeof lines[0], "%-*s", LCD_COLS, " Erase all event changes?");
        if (s->erase_this)
            snprintf(lines[1], sizeof lines[1], " <Yes>: %-*s", LCD_COLS - 8, AUTO_NAMES[s->erase_ask - 1]);
        else
            snprintf(lines[1], sizeof lines[1], " (none for %s)%*s", AUTO_NAMES[s->erase_ask - 1], LCD_COLS, "");
        snprintf(lines[2], sizeof lines[2], " <All>: every value, %-*s", LCD_COLS - 21, id);
        for (int i = 0; i < 3; i++) lcd_inverse(u, 0, 5 + i, lines[i]);
    }
    lcd_rule(u, BOTTOM_ROW, s->edit_all ? "ALL PADS" : 0); // Edit All Pads on: said where it cannot be missed
}

// The Loop screen: LOOP opens it for the selected pad. The loop plays from Loop St to Loop End,
// and Loop End is also where the sound ends. K1 and K2 move them, K3 picks a <Do>.
enum { LOOP_FIND_ST, LOOP_FIND_END, LOOP_XFADE, LOOP_DISCARD, LOOP_OPS };
static const char *const LOOP_OP_NAMES[LOOP_OPS] = {"Find St", "Find End", "X-Fade", "Discard"};
static const struct param LOOP_PARAMS[3] = {
    P("Loop St", loop, 0, 0, 0, F_FRAMES), P("Loop End", end, 0, 0, 0, F_FRAMES), {"Do", 0, 0, LOOP_OPS - 1, 1, F_DO}};
#define LOOP_FIELDS 4 // Loop on/off, then the three knob rows
#define LOOP_MIN 16   // the shortest loop Find makes, in frames
#define LOOP_WINDOW 256 // frames compared each side of the splice

static void open_loop(struct os *s) {
    s->screen = OS_LOOP;
    s->cursor_loop = 0;
    reset_knobs(s);
}

static float mono_at(const struct sample *sm, int64_t i) {
    const float *f = sm->data + i * sm->channels;
    return sm->channels == 2 ? 0.5f * (f[0] + f[1]) : f[0];
}

// How different the audio around frames a and b is, weighted toward the splice itself.
// Playing to b then jumping to a sounds smooth when this is low. -1 when too little overlaps.
static double splice_cost(const struct sample *sm, int64_t a, int64_t b, int w) {
    double sum = 0, wsum = 0;
    for (int k = -w; k < w; k++) {
        int64_t i = a + k, j = b + k;
        if (i < 0 || j < 0 || i >= sm->frames || j >= sm->frames) continue;
        double wt = k < 0 ? w + k + 1 : w - k, d = (double)mono_at(sm, i) - mono_at(sm, j);
        sum += wt * d * d;
        wsum += wt;
    }
    return wsum * 4 >= (double)w * (w + 1) ? sum / wsum : -1;
}

// The point in [from, to] that best splices against `fixed`, preferring ones near `near`.
static int32_t splice_find(const struct sample *sm, int32_t fixed, int32_t from, int32_t to, int32_t near, int w) {
    int32_t best = -1;
    double best_cost = 0;
    int32_t radius = to - from > 0 ? to - from : 1;
    for (int32_t c = from; c <= to; c++) {
        double cost = splice_cost(sm, c, fixed, w);
        if (cost < 0) continue;
        int32_t dist = c > near ? c - near : near - c;
        cost = cost * (1.0 + (double)dist / radius) + 1e-12 * dist;
        if (best < 0 || cost < best_cost) best = c, best_cost = cost;
    }
    return best;
}

static void loop_op(struct os *s, const struct ui *u) {
    struct os_pad *pad = &s->pads[s->selected];
    if (!pad->s) return;
    os_undo_save(s, LOOP_OP_NAMES[s->loop_op]);
    const struct sample *o = pad->s;
    struct pad_params *p = &pad->p;
    int32_t len = p->end - p->loop, radius = len / 2;
    if (radius < 64) radius = 64;
    if (radius > o->rate / 10) radius = o->rate / 10; // within 100 ms of where it is now
    if (s->loop_op == LOOP_DISCARD) { // the Trim page's Discard: keeps Zone Start to Loop End
        int keep = s->do_op;
        s->do_op = DO_DISCARD;
        zone_op(s, u);
        s->do_op = keep;
        return;
    }
    int w = len / 2 < LOOP_WINDOW ? len / 2 : LOOP_WINDOW;
    if (w < 8) w = 8;
    if (s->loop_op == LOOP_FIND_ST || s->loop_op == LOOP_FIND_END) {
        int st = s->loop_op == LOOP_FIND_ST;
        int32_t from, to, near = st ? p->loop : p->end;
        if (st) {
            from = p->loop - radius < p->start ? p->start : p->loop - radius;
            to = p->loop + radius > p->end - LOOP_MIN ? p->end - LOOP_MIN : p->loop + radius;
        } else {
            from = p->end - radius < p->loop + LOOP_MIN ? p->loop + LOOP_MIN : p->end - radius;
            to = p->end + radius > o->frames ? o->frames : p->end + radius;
        }
        int32_t found = from <= to ? splice_find(o, st ? p->end : p->loop, from, to, near, w) : -1;
        if (found < 0) {
            toast(s, u, "Loop too short");
            return;
        }
        if (st) p->loop = found, p->loop_lock = 0;
        else p->end = found;
        pad_changed(s, s->selected);
        toast(s, u, found == near ? "Already the best point" : st ? "Loop start found" : "Loop end found");
        return;
    }
    // X-Fade: the last stretch before Loop End fades into the audio just before Loop St, so the
    // jump back lands where that audio would have gone on. It needs audio before Loop St.
    int32_t fade = len / 4;
    if (fade > o->rate / 10) fade = o->rate / 10;
    if (fade > p->loop) fade = p->loop;
    if (fade < 2) {
        toast(s, u, p->loop < 2 ? "Needs audio before Loop St" : "Loop too short");
        return;
    }
    struct sample *n = sample_new(o->frames, o->channels, o->rate);
    if (!n) {
        toast(s, u, "Out of memory");
        return;
    }
    int32_t at = 0;
    append(n, &at, o, 0, o->frames);
    for (int32_t i = 0; i < fade; i++) {
        float t = (float)(i + 1) / (float)fade, in = sinf(t * 1.5707963f), out = cosf(t * 1.5707963f);
        float *d = n->data + (size_t)(p->end - fade + i) * (size_t)n->channels;
        const float *f = o->data + (size_t)(p->loop - fade + i) * (size_t)o->channels;
        for (int c = 0; c < n->channels; c++) d[c] = d[c] * out + f[c] * in;
    }
    replace_sample(s, pad, n);
    pad_changed(s, s->selected);
    char msg[32];
    snprintf(msg, sizeof msg, "Crossfaded %dms", (int)((int64_t)fade * 1000 / o->rate));
    toast(s, u, msg);
}

// Moves Loop St or Loop End. Setting Loop St by hand lets go of Loop Lock.
static void loop_set(struct os *s, const struct param *d, int value) {
    if (d->off == offsetof(struct pad_params, loop)) s->pads[s->selected].p.loop_lock = 0;
    param_set(s, d, value);
}

static void loop_nudge(struct os *s, const struct ui *u, int steps, int fine) {
    struct os_pad *pad = &s->pads[s->selected];
    if (!steps) return;
    if (s->cursor_loop == 0) { // right turns it on, left off
        if (!pad->s || pad->p.loop_on == (steps > 0)) return;
        toggle(s, u, offsetof(struct pad_params, loop_on), "Loop: On", "Loop: Off");
        return;
    }
    const struct param *d = &LOOP_PARAMS[s->cursor_loop - 1];
    if (d->fmt == F_DO) {
        int op = s->loop_op + steps;
        s->loop_op = op < 0 ? 0 : op >= LOOP_OPS ? LOOP_OPS - 1 : op;
        return;
    }
    if (d->off == offsetof(struct pad_params, loop)) pad->p.loop_lock = 0;
    param_nudge(s, d, steps, fine);
}

// Knob Mode. CATCH: a knob or the slider takes over once it reaches the value, so it never jumps.
// JUMP: the value goes to where the knob is, at once. SCALE: the value moves from where it is, in
// proportion to the travel left, so the knob and the value meet at the ends.
int os_knob_take(const struct os *s, int *caught, int last, int now, int at) {
    at = at < 0 ? 0 : at > 127 ? 127 : at;
    if (s->knob_mode == KNOB_JUMP) {
        *caught = 1;
        return now;
    }
    if (s->knob_mode == KNOB_SCALE) {
        *caught = 0;
        if (last < 0 || now == last) return -1; // the first reading: only where the knob is
        int to;
        if (now > last) to = last >= 127 ? 127 : at + (int)(((int64_t)(now - last) * (127 - at) + (127 - last) / 2) / (127 - last));
        else to = last <= 0 ? 0 : at - (int)(((int64_t)(last - now) * at + last / 2) / last);
        if (to == at && at > 0 && now < last) to = at - 1; // a small turn still moves it
        if (to == at && at < 127 && now > last) to = at + 1;
        return to < 0 ? 0 : to > 127 ? 127 : to;
    }
    if (!*caught) {
        int crossed = last >= 0 && ((last <= at && now >= at) || (last >= at && now <= at));
        if (crossed || (now - at <= 2 && at - now <= 2)) *caught = 1;
    }
    return *caught ? now : -1;
}

static int catch_value(const struct os *s, int *caught, int last, int now, int at) { // the position, or -1
    return os_knob_take(s, caught, last, now, at);
}

static void handle_loop_screen(struct os *s, const struct ui *u, const struct input *in) {
    struct os_pad *pad = &s->pads[s->selected];
    if (input_down(in, IN_B1) || input_down(in, IN_B2)) {
        if (input_down(in, IN_B1) && s->zoom < OS_ZOOM_MAX) s->zoom++;
        if (input_down(in, IN_B2) && s->zoom > 0) s->zoom--;
        char msg[24];
        snprintf(msg, sizeof msg, "Zoom: x%d", 1 << s->zoom);
        toast(s, u, msg);
    }
    if (input_down(in, IN_B3) && s->cursor_loop == 3) loop_op(s, u);
    if (input_down(in, IN_CHOP) && in->shift)
        toggle(s, u, offsetof(struct pad_params, gate), "Note On: On", "Note On: Off");
    if (in->encoder_press) s->cursor_loop = (s->cursor_loop + (in->shift ? LOOP_FIELDS - 1 : 1)) % LOOP_FIELDS;
    else loop_nudge(s, u, in->encoder, in->shift);
    if (!in->shift) loop_nudge(s, u, input_down(in, IN_PLUS) - input_down(in, IN_MINUS), 1);
    for (int i = 0; i < 3; i++) { // K1 Loop St, K2 Loop End, K3 Do
        if (!in->knob_moved[i]) continue;
        int last = s->knob_last[i], now = in->knob[i];
        s->knob_last[i] = now;
        s->cursor_loop = i + 1;
        const struct param *d = &LOOP_PARAMS[i];
        if (d->fmt == F_DO) {
            s->loop_op = now * LOOP_OPS / 128;
            continue;
        }
        int fine = zoom_step(s, pad);
        if (pad->s && fine && last >= 0) { // zoomed: fine steps from where the point is
            loop_set(s, d, *param_ptr(&pad->p, d) + (now - last) * fine);
            s->knob_caught[i] = 0;
        } else if (pad->s) {
            int pos = catch_value(s, &s->knob_caught[i], last, now, knob_position(d, pad));
            if (pos >= 0) loop_set(s, d, (int)((int64_t)param_max(d, pad) * pos / 127));
        }
    }
    if (in->fader_moved) { // the fader moves Loop St or Loop End, whichever is framed
        int last = s->fader_last, now = in->fader;
        s->fader_last = now;
        const struct param *d = s->cursor_loop == 1 || s->cursor_loop == 2 ? &LOOP_PARAMS[s->cursor_loop - 1] : 0;
        int pos = d && pad->s ? catch_value(s, &s->fader_caught, last, now, knob_position(d, pad)) : -1;
        if (pos >= 0) loop_set(s, d, (int)((int64_t)param_max(d, pad) * pos / 127));
    }
}

static void draw_loop_screen(struct ui *u, const struct os *s, const struct input *in) {
    (void)in;
    const struct os_pad *pad = &s->pads[s->selected];
    char line[LCD_COLS + 8];
    lcd_text(u, 0, KEY_ROW, "<Zoom In>");
    lcd_text(u, 10, KEY_ROW, "<Zoom Out>");
    if (s->cursor_loop == 3) lcd_text(u, 22, KEY_ROW, "<Do>");
    snprintf(line, sizeof line, "P%02d %c%02d Edit Loop", s->program + 1, 'A' + s->selected / 16, s->selected % 16 + 1);
    lcd_rule(u, TITLE_ROW, line);
    lcd_text(u, 0, 3, "Loop:");
    const char *on = pad->s && pad->p.loop_on ? " On " : " Off ";
    if (s->cursor_loop == 0) lcd_inverse(u, 5, 3, on);
    else lcd_text(u, 5, 3, on);
    snprintf(line, sizeof line, "Snd:%.11s", pad->s ? pad->name : "(empty)");
    lcd_text(u, 11, 3, line);
    if (pad->s) {
        const struct pad_params *p = &pad->p;
        draw_wave(u, s, pad, p->loop, p->end, follow_point(p->loop, p->end, s->cursor_loop == 2), p->start != p->loop ? p->start : -1);
        char len[16];
        format_value(&LOOP_PARAMS[0], p->end - p->loop, pad->s->rate, len, sizeof len);
        snprintf(line, sizeof line, "Loop Len %s", len);
        lcd_rule(u, PARAM_ROW - 1, line);
    } else {
        draw_wave(u, s, pad, 0, 0, 0, -1);
        lcd_rule(u, PARAM_ROW - 1, "Loop Params");
    }
    draw_params(u, LOOP_PARAMS, s->cursor_loop - 1, LOOP_OP_NAMES[s->loop_op], pad);
    lcd_rule(u, BOTTOM_ROW, 0);
}

static void draw_project(struct ui *u, const struct os *s, const struct input *in) {
    static const char *const styles[3] = {"Both colors", "Color A", "Color B"};
    struct os *w = (struct os *)s;
    if (input_down(in, IN_B2)) { // tries the other colour of the two-colour button lights
        w->led_style = (w->led_style + 1) % 3;
        if (w->set_led_style) w->set_led_style(w->led_style);
    }
    lcd_text(u, 0, KEY_ROW, "<Back>");
    lcd_text(u, 9, KEY_ROW, "<Lights>");
    lcd_text(u, 18, KEY_ROW, "<Exit>");
    lcd_rule(u, TITLE_ROW, "Project");
    lcd_text(u, 0, 4, "Audio:");
    const char *status = s->audio_status ? s->audio_status : "off";
    lcd_text(u, 0, 5, status);
    if (strlen(status) > LCD_COLS) lcd_text(u, 0, 6, status + LCD_COLS);
    char line[48];
    snprintf(line, sizeof line, "Dropouts: %u", s->xruns ? s->xruns() : 0u);
    lcd_text(u, 0, 8, line);
    if (s->power_status) lcd_text(u, 0, 9, s->power_status);
    snprintf(line, sizeof line, "Button lights: %s", styles[s->led_style]);
    lcd_text(u, 0, 11, line);
    lcd_rule(u, BOTTOM_ROW, 0);
    if (input_down(in, IN_B1) || (input_down(in, IN_SEQ) && !in->shift)) {
        seq_go_main((struct os *)s);
        ui_wake(u, 1); // draw the main screen on the next frame
    }
    if (input_down(in, IN_B3)) ui_quit(u);
}

static int meter_mask(float peak) {
    static const float steps[5] = {0.016f, 0.063f, 0.25f, 0.5f, 0.89f};
    int mask = 0;
    for (int i = 0; i < 5; i++)
        if (peak >= steps[i]) mask |= 1 << i;
    return mask;
}

static void update_lights(struct ui *u, struct os *s) {
    float pl = atomic_exchange_explicit(&s->engine->peak_l, 0.0f, memory_order_relaxed);
    float pr = atomic_exchange_explicit(&s->engine->peak_r, 0.0f, memory_order_relaxed);
    s->meter_l = pl > s->meter_l ? pl : s->meter_l * 0.8f;
    s->meter_r = pr > s->meter_r ? pr : s->meter_r * 0.8f;
    ui_meter(u, meter_mask(s->meter_l), meter_mask(s->meter_r));
    u->track_led = s->bank;
    const struct os_pad *pad = &s->pads[s->selected];
    ui_button_led(u, IN_SAMPLE, s->screen == OS_SAMPLE ? 2 : 1);
    ui_button_led(u, IN_LOOP, pad->s && pad->p.loop_on ? 2 : 1);
    ui_button_led(u, IN_SAMPLE_SELECT, s->screen == OS_BROWSER ? 2 : 1);
    ui_button_led(u, IN_PAD_BANK, 1);
    ui_button_led(u, IN_LEVELS, s->levels_on ? 2 : 1);
    int rec_state = atomic_load(&s->rec.state); // SAMPLE REC: flashing while armed, lit while recording
    ui_button_led(u, IN_SAMPLE_REC, rec_state == REC_RECORDING ? 2 : rec_state == REC_ARMED ? (now_ms(u) / 250 % 2 ? 2 : 0) : 1);
    atomic_store_explicit(&s->rec.thru, s->input_thru, memory_order_relaxed); // Input Thru, on every page
    ui_button_led(u, IN_NOTE_REPEAT, s->seq.repeat ? 2 : 1);
    ui_button_led(u, IN_PAD_FX, s->screen == OS_PRINT ? 2 : 1);
    ui_button_led(u, IN_MUTE, s->screen == OS_MUTE ? 2 : 1);
    int fx_any = 0; // KNOB FX lit while the chain has an effect in it and is on
    for (int i = 0; i < FX_SLOTS; i++) fx_any |= atomic_load_explicit(&s->fx.type[i], memory_order_relaxed) != FX_OFF;
    ui_button_led(u, IN_KNOB_FX, fx_any && atomic_load_explicit(&s->fx.on, memory_order_relaxed) ? 2 : 1);
    seq_lights(u, s);
    if (s->button_lights == BUTTON_OFF) // Button Lights OFF: every button dark
        for (int b = 0; b < IN_N; b++) ui_button_led(u, b, 0);
    prefs_pad_lights(s, u);
    if (atomic_load_explicit(&s->engine->voices, memory_order_relaxed) > 0 || s->meter_l > 0.01f || s->meter_r > 0.01f)
        ui_wake(u, 33);
}

#define BROWSE_FIRST_ROW 4
#define BROWSE_ROWS 10 // two fewer while the default folder hint shows
#define PREVIEW_PAD (-1) // not a real pad, so it never lands on a loaded sample
#define PREVIEW_DELAY_MS 180 // scrolling past a file does not load it
#define PREVIEW_MAX_BYTES (4u * 1024u * 1024u)

// Each folder keeps its place in the list for the session: going in and back out again lands
// where the list was, not at the top. The oldest folder is forgotten first.
#define BROWSE_KEEP 32
static struct { char path[FILES_PATH]; int cursor; } browse_kept[BROWSE_KEEP];
static int browse_next;

static void browser_keep(const struct files *fb) {
    if (!fb->cwd[0]) return; // the storage list is short; it is not kept
    if (fb->count > 0 && fb->entries[fb->cursor].up) return; // on ..: going up by it, so the place stays as it was
    int slot = -1;
    for (int i = 0; i < BROWSE_KEEP && slot < 0; i++)
        if (strcmp(browse_kept[i].path, fb->cwd) == 0) slot = i;
    if (slot < 0) slot = browse_next, browse_next = (browse_next + 1) % BROWSE_KEEP;
    snprintf(browse_kept[slot].path, FILES_PATH, "%s", fb->cwd);
    browse_kept[slot].cursor = fb->cursor;
}

static void browser_restore(struct files *fb) {
    if (!fb->cwd[0]) return;
    for (int i = 0; i < BROWSE_KEEP; i++)
        if (strcmp(browse_kept[i].path, fb->cwd) == 0) {
            int c = browse_kept[i].cursor;
            fb->cursor = c < 0 ? 0 : c >= fb->count ? (fb->count > 0 ? fb->count - 1 : 0) : c;
            return;
        }
}

static int browser_enter(struct os *s, const struct ui *u, const char *path) {
    browser_keep(&s->browser);
    if (files_open(&s->browser, path) == 0) {
        browser_restore(&s->browser);
        return 0;
    }
    toast(s, u, "Could not open folder");
    return -1;
}

// The entry with this name in the open folder, or -1.
static int browser_find(const struct files *fb, const char *name) {
    for (int i = 0; i < fb->count; i++)
        if (!fb->entries[i].up && strcmp(fb->entries[i].name, name) == 0) return i;
    return -1;
}

void os_open_s600_trim(struct os *s) {
    s->screen = OS_SAMPLE;
    s->s600_trim = 1;
    set_page(s, PAGE_TRIM);
    first_frame(s);
    s->fader_caught = 0;
    reset_knobs(s);
}

// Leaving Load Files: the Sample screen, or the S600 screen on an S600 track.
static void sound_screen(struct os *s) {
    if (os_plays_s600(s)) s600_open(s);
    else s->screen = OS_SAMPLE;
}

static void browser_back(struct os *s, const struct ui *u) {
    struct files *fb = &s->browser;
    if (!fb->cwd[0]) {
        sound_screen(s);
        return;
    }
    int place = files_place_of(fb, fb->cwd);
    if (place >= 0) { // the top of a card or drive: back to the places, on that one
        if (browser_enter(s, u, "") == 0 && place < fb->count) fb->cursor = place;
        return;
    }
    char parent[FILES_PATH], left[FILES_NAME];
    snprintf(parent, sizeof parent, "%s", fb->cwd);
    char *slash = strrchr(parent, '/');
    snprintf(left, sizeof left, "%s", slash ? slash + 1 : "");
    if (slash && slash != parent) *slash = 0;
    if (browser_enter(s, u, parent) == 0) { // on the folder just left
        int at = browser_find(fb, left);
        if (at >= 0) fb->cursor = at;
    }
}

static void browser_load(struct os *s, const struct ui *u, const char *path, const char *file) {
    struct sample *n = wav_load(path);
    sound_screen(s);
    if (!n) {
        toast(s, u, "Could not read that WAV");
        return;
    }
    char name[sizeof s->pads[0].name];
    snprintf(name, sizeof name, "%s", file);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    os_undo_save(s, "Load");
    os_pad_load_over(s, s->selected, n, name);
    os_auto_root(s, s->selected);
    reset_knobs(s);
}

static void browser_open(struct os *s, const struct ui *u) {
    struct files *fb = &s->browser;
    if (fb->count <= 0) return;
    const struct file_entry *e = &fb->entries[fb->cursor];
    if (e->up) {
        browser_back(s, u);
    } else if (e->place) {
        browser_enter(s, u, fb->places[fb->cursor]);
    } else {
        char path[FILES_PATH];
        int n = snprintf(path, sizeof path, "%s/%s", fb->cwd, e->name);
        if (n < 0 || n >= (int)sizeof path) {
            toast(s, u, "Name is too long");
        } else if (e->dir) {
            browser_enter(s, u, path);
        } else {
            char file[FILES_NAME];
            snprintf(file, sizeof file, "%s", e->name);
            browser_load(s, u, path, file);
        }
    }
}

static void browser_draw(struct ui *u, struct os *s, int shift) {
    struct files *fb = &s->browser;
    if (shift) {
        lcd_text(u, 0, KEY_ROW, "<Set Home>");
    } else {
        lcd_text(u, 0, KEY_ROW, "<Back>");
        lcd_text(u, 8, KEY_ROW, s->preview_on ? "<Prev On>" : "<PrevOff>");
        int is_file = fb->count > 0 && !fb->entries[fb->cursor].dir;
        lcd_text(u, 20, KEY_ROW, is_file ? "<Load>" : "<Open>");
    }
    char title[32]; // where a load goes: the program the active track plays, the selected pad
    snprintf(title, sizeof title, "Load to P%02d %c%02d", s->program + 1, 'A' + s->selected / 16, s->selected % 16 + 1);
    lcd_rule(u, TITLE_ROW, title);
    const char *where = fb->cwd[0] ? fb->cwd : "Storage";
    size_t len = strlen(where);
    lcd_text(u, 0, 3, len > LCD_COLS ? where + len - LCD_COLS : where);
    if (fb->cursor < fb->top) fb->top = fb->cursor;
    int hint = !s->home_dir[0]; // only until a default load folder has been set
    int rows = hint ? BROWSE_ROWS - 2 : BROWSE_ROWS;
    if (fb->cursor >= fb->top + rows) fb->top = fb->cursor - rows + 1;
    if (fb->count == 0) lcd_center(u, 7, "No WAV files here");
    for (int i = 0; i < rows && fb->top + i < fb->count; i++) {
        int index = fb->top + i;
        const struct file_entry *e = &fb->entries[index];
        char line[LCD_COLS + 1];
        snprintf(line, sizeof line, "%.24s%s", e->name, e->dir && !e->up && !e->place ? "/" : "");
        if (index == fb->cursor) {
            char bar[LCD_COLS + 1];
            snprintf(bar, sizeof bar, "%-*s", LCD_COLS, line);
            lcd_inverse(u, 0, BROWSE_FIRST_ROW + i, bar);
        } else {
            lcd_text(u, 0, BROWSE_FIRST_ROW + i, line);
        }
    }
    if (!hint) {
        lcd_rule(u, BOTTOM_ROW, 0);
        return;
    }
    lcd_rule(u, BOTTOM_ROW - 2, 0);
    lcd_text(u, 0, BOTTOM_ROW - 1, shift ? "<Set Home> makes this the" : "Press SHIFT to assign");
    lcd_text(u, 0, BOTTOM_ROW, "default load folder");
}

static void preview_stop(struct os *s) {
    engine_retire(s->engine, s->preview);
    s->preview = 0;
    s->preview_at = 0;
}

static void preview_schedule(struct os *s, const struct ui *u) {
    preview_stop(s);
    if (s->preview_on) s->preview_at = now_ms(u) + PREVIEW_DELAY_MS;
}

static void preview_start(struct os *s) {
    struct files *fb = &s->browser;
    if (fb->count <= 0 || fb->entries[fb->cursor].dir) return;
    char path[FILES_PATH];
    int len = snprintf(path, sizeof path, "%s/%s", fb->cwd, fb->entries[fb->cursor].name);
    if (len < 0 || len >= (int)sizeof path) return;
    struct sample *n = wav_load_start(path, PREVIEW_MAX_BYTES);
    if (!n) return;
    struct pad_params p;
    default_params(&p, n->frames);
    s->preview = n;
    engine_note_on(s->engine, PREVIEW_PAD, 127, n, &p);
}

static void handle_browser(struct os *s, struct ui *u, const struct input *in) {
    struct files *fb = &s->browser;
    int before = fb->cursor, navigated = 1;
    fb->cursor += in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (fb->cursor >= fb->count) fb->cursor = fb->count - 1;
    if (fb->cursor < 0) fb->cursor = 0;
    if (input_down(in, IN_SAMPLE)) sound_screen(s);
    else if (input_down(in, IN_SEQ) && !in->shift) seq_go_main(s);
    else if (input_down(in, IN_B1) && in->shift) set_home(s, u);
    else if (input_down(in, IN_B1)) browser_back(s, u);
    else if (in->encoder_press || input_down(in, IN_B3)) browser_open(s, u);
    else if (input_down(in, IN_B2)) s->preview_on = !s->preview_on;
    else if ((navigated = fb->cursor != before)) browser_keep(fb); // the bar's place in this folder, as it moves
    if (fb->cwd[0]) {
        snprintf(s->browser_dir, sizeof s->browser_dir, "%s", fb->cwd);
        s->browser_cursor = fb->cursor;
    }
    if (s->screen != OS_BROWSER) {
        preview_stop(s);
        if (s->screen == OS_SAMPLE) draw_sample_screen(u, s, in);
        else ui_wake(u, 1); // draw the screen being opened on the next frame
        return;
    }
    if (navigated) preview_schedule(s, u);
    if (s->preview_at) {
        int64_t wait = s->preview_at - now_ms(u);
        if (wait <= 0) {
            s->preview_at = 0;
            preview_start(s);
        } else {
            ui_wake(u, (int)wait);
        }
    }
    browser_draw(u, s, in->shift);
}

static void draw_toast(struct ui *u, struct os *s) {
    int64_t now = now_ms(u);
    if (now >= s->toast_until) return;
    // One line; too long for one (the box leaves 24 columns), two, broken at a space.
    char line[2][LCD_COLS + 1];
    int n = (int)strlen(s->toast), rows = 1, w = LCD_COLS - 2;
    snprintf(line[0], sizeof line[0], "%.*s", w, s->toast);
    if (n > w) {
        int cut = w;
        while (cut > 0 && s->toast[cut] != ' ') cut--;
        if (cut == 0) cut = w;
        snprintf(line[0], sizeof line[0], "%.*s", cut, s->toast);
        snprintf(line[1], sizeof line[1], "%.*s", w, s->toast + cut + (s->toast[cut] == ' '));
        rows = 2;
    }
    int widest = (int)strlen(line[0]);
    if (rows == 2 && (int)strlen(line[1]) > widest) widest = (int)strlen(line[1]);
    int col = (LCD_COLS - widest) / 2;
    lcd_fill(u, LCD_X + (col - 1) * CELL_W, 5 * CELL_H - 4, (widest + 2) * CELL_W, rows * CELL_H + 8, LCD_FG);
    for (int r = 0; r < rows; r++) lcd_inverse(u, col + (widest - (int)strlen(line[r])) / 2, 5 + r, line[r]);
    ui_wake(u, (int)(s->toast_until - now));
}

static void shutdown_prompt(struct os *s) {
    s->return_screen = s->screen;
    s->screen = OS_SHUTDOWN;
    preview_stop(s);
    seq_stop(&s->seq, 0);
    for (int i = 0; i < 16; i++) pad_up(s, i); // their releases will not be seen here
    engine_kill_all(s->engine);
}

// ---- USB DATA MODE: the card as a USB drive on a computer (usbmode.h) ----
// The MPC is locked on this page until it is over: only its keys and the power key work.

#define USBM_SLOW_MS 90000 // a step taking longer than this gets <Leave>, so the page never holds you

void usbmode_open(struct os *s, struct ui *u) {
    if (!s->usbmode) {
        toast(s, u, "Not on this device");
        return;
    }
    preview_stop(s);
    seq_stop(&s->seq, 0);
    for (int i = 0; i < 16; i++) pad_up(s, i);
    engine_kill_all(s->engine);
    if (s->usbmode->begin(USBM_CONNECT) != 0) {
        toast(s, u, "Still busy: try again");
        return;
    }
    s->usbm_op = USBM_CONNECT, s->usbm_ask = 0;
    s->screen = OS_USBMODE;
    ui_wake(u, 1);
}

static void usbmode_leave(struct os *s, struct ui *u) {
    s->usbm_ask = 0;
    s->usbm_watch = 1; // a step left running: if it ends connected, the page comes back
    disk_open(s, DISK_USB, s->disk_type); // Save/Load again, its lists read afresh
    ui_wake(u, 1);
}

static void usbmode_lines(struct ui *u, int row, const char *const *lines, int n) {
    for (int i = 0; i < n; i++) lcd_text(u, 0, row + i, lines[i]);
}

void usbmode_frame(struct ui *u, struct os *s, const struct input *in) {
    int flags = 0;
    char msg[96];
    int st = s->usbmode ? s->usbmode->state(&flags, msg, sizeof msg) : USBM_IDLE;
    ui_clear(u, LCD_BG);
    lcd_rule(u, TITLE_ROW, "USB Data Mode");
    switch (st) {
    case USBM_WORKING: {
        lcd_center(u, 5, s->usbm_op == USBM_CONNECT ? "Starting USB Data Mode" : s->usbm_op == USBM_REPAIR ? "Repairing the card" : "Taking the card back");
        lcd_center(u, 7, s->usbm_op == USBM_EXIT ? "and checking it" : "");
        lcd_center(u, 9, "Please wait...");
        if (s->usbmode->elapsed_ms() > USBM_SLOW_MS) { // never stuck here
            lcd_center(u, 11, "This is taking too long.");
            lcd_center(u, 12, "<Leave>: turn off and on");
            lcd_center(u, 13, "if the card is missing");
            lcd_text(u, 19, KEY_ROW, "<Leave>");
            if (input_down(in, IN_B3)) usbmode_leave(s, u);
        }
        ui_wake(u, 200);
        break;
    }
    case USBM_CONNECTED: {
        if (s->usbm_ask) { // the computer still has the drive
            lcd_text(u, 0, KEY_ROW, "<No>");
            lcd_text(u, 20, KEY_ROW, "<Yes>");
            static const char *const ASK[] = {
                "The computer has not",
                "ejected the drive.",
                "",
                "Exit anyway? Files being",
                "copied now may be",
                "damaged.",
            };
            usbmode_lines(u, 4, ASK, 6);
            if (input_down(in, IN_B1)) s->usbm_ask = 0;
            else if (input_down(in, IN_B3) && s->usbmode->begin(USBM_EXIT) == 0) s->usbm_op = USBM_EXIT, s->usbm_ask = 0;
            ui_wake(u, 300);
            break;
        }
        const char *now = flags & USBM_CARD_OUT ? "! The card was taken out"
                        : flags & USBM_EJECTED  ? "Ejected: safe to exit"
                        : flags & USBM_HOST     ? "On the computer"
                        : flags & USBM_CABLE    ? "Waiting for the computer"
                                                : "Connect the USB cable";
        lcd_inverse(u, 0, 3, " ");
        lcd_inverse(u, 1, 3, now);
        static const char *const STEPS[] = {
            "1 Copy files on computer",
            "2 Eject the drive there",
            "3 Press <Exit> here",
        };
        usbmode_lines(u, 5, STEPS, 3);
        if (!(flags & USBM_HOST) && msg[0]) lcd_text(u, 0, 8, msg); // the port's own state, to tell a cable from a port
        static const char *const WARN[] = {
            "! Do not turn the MPC off",
            "  or pull the cable while",
            "  files copy. Turning off",
            "  loses unsaved work.",
        };
        usbmode_lines(u, 9, WARN, 4);
        lcd_text(u, 20, KEY_ROW, "<Exit>");
        if (input_down(in, IN_B3)) {
            int safe = flags & (USBM_EJECTED | USBM_CARD_OUT) || !(flags & USBM_CABLE);
            if (!safe) s->usbm_ask = 1;
            else if (s->usbmode->begin(USBM_EXIT) == 0) s->usbm_op = USBM_EXIT;
        }
        ui_wake(u, 300); // follows the computer
        break;
    }
    case USBM_ERRORS: {
        lcd_center(u, 4, msg[0] ? msg : "The card has errors");
        static const char *const RO[] = {
            "It is read-only for now:",
            "nothing can be saved to",
            "it. <Repair> fixes it, or",
            "check it on a computer.",
        };
        usbmode_lines(u, 6, RO, 4);
        lcd_text(u, 0, KEY_ROW, "<Repair>");
        lcd_text(u, 22, KEY_ROW, "<OK>");
        if (input_down(in, IN_B1) && s->usbmode->begin(USBM_REPAIR) == 0) s->usbm_op = USBM_REPAIR;
        else if (input_down(in, IN_B3)) usbmode_leave(s, u);
        break;
    }
    case USBM_FAILED:
    case USBM_DONE:
    case USBM_NO_CARD:
    default: {
        const char *head = st == USBM_FAILED ? (msg[0] ? msg : "Could not start")
                         : st == USBM_NO_CARD ? "The card was taken out"
                         : st == USBM_DONE ? "The card is back" : "";
        char first[96];
        const char *nl = strchr(head, '\n'); // a second line: the step that failed, and why
        snprintf(first, sizeof first, "%.*s", nl ? (int)(nl - head) : (int)strlen(head), head);
        lcd_center(u, 5, first);
        if (nl) lcd_center(u, 9, nl + 1);
        if (st == USBM_FAILED) lcd_center(u, 7, "Nothing was changed.");
        if (st == USBM_DONE && msg[0]) lcd_center(u, 7, msg);
        if (st == USBM_NO_CARD) lcd_center(u, 7, "Put it back to use it.");
        lcd_text(u, 22, KEY_ROW, "<OK>");
        if (input_down(in, IN_B3) || st == USBM_IDLE) usbmode_leave(s, u);
        break;
    }
    }
    lcd_rule(u, BOTTOM_ROW, 0);
}

static void shutdown_frame(struct ui *u, struct os *s, const struct input *in) {
    int64_t now = now_ms(u);
    ui_clear(u, LCD_BG);
    if (!s->shutdown_at) {
        if (input_down(in, IN_B1)) {
            s->screen = s->return_screen;
            if (s->screen == OS_BROWSER) preview_schedule(s, u);
            ui_wake(u, 1); // draw the screen being returned to
            return;
        }
        if (input_down(in, IN_B3)) {
            s->shutdown_at = now;
            ui_wake(u, 100); // let the message reach the panel first
        }
    }
    if (s->shutdown_at) {
        lcd_rule(u, TITLE_ROW, "Power Off");
        lcd_center(u, 7, "Powering off...");
        lcd_rule(u, BOTTOM_ROW, 0);
        if (s->power_off && s->shutdown_stage == 0 && now > s->shutdown_at) {
            s->shutdown_stage = 1;
            s->power_off(0);
        } else if (s->power_off && s->shutdown_stage == 1 && now - s->shutdown_at >= OS_POWER_FORCE_MS) {
            s->shutdown_stage = 2;
            s->power_off(1);
        }
        ui_wake(u, 250);
        return;
    }
    lcd_text(u, 0, KEY_ROW, "<No>");
    lcd_text(u, 18, KEY_ROW, "<Yes>");
    lcd_rule(u, TITLE_ROW, "Power Off");
    lcd_center(u, 7, "Do you want to power off?");
    if (s->return_screen == OS_USBMODE) { // the card is on the computer
        lcd_center(u, 9, "Eject the drive on the");
        lcd_center(u, 10, "computer first.");
    }
    lcd_rule(u, BOTTOM_ROW, 0);
}

// The credit under the title, in the small font, centred, from pixel row y.
static void title_credit(struct ui *u, int y) {
    static const char *const LINES[] = {"Original MPC design by Roger Linn,", "Akai Electric Co., Ltd."};
    for (int i = 0; i < 2; i++) {
        int w = (int)strlen(LINES[i]) * 6 - 1;
        lcd_small_text(u, LCD_X + (LCD_COLS * CELL_W - w) / 2, y + i * 10, LINES[i], LCD_FG);
    }
}

// Shows the startup screen. 1 while it stays up. Any control skips it.
static int title_frame(struct ui *u, struct os *s, const struct input *in) {
    int64_t now = now_ms(u);
    if (!s->title_until) s->title_until = now + OS_TITLE_MS;
    int touched = in->down != 0 || in->encoder != 0 || in->encoder_press;
    for (int i = 0; i < 16; i++)
        if (in->pad_hit[i] >= 0) touched = 1;
    if (touched || now >= s->title_until) {
        s->screen = OS_MAIN; // the MPC3000 starts on its main screen too
        s->cursor_play = 0;
        return 0;
    }
    ui_clear(u, LCD_BG);
    if (lcd_layout == LAYOUT_2000XL) { // the XL's way: the name and version, alone in the middle
        lcd_center(u, 7, OS_NAME " Version " OS_NUMBER);
        update_lights(u, s);
        ui_wake(u, (int)(s->title_until - now));
        return 1;
    }
    lcd_text(u, 18, KEY_ROW, "<Skip>");
    lcd_rule(u, TITLE_ROW, OS_NAME);
    lcd_center(u, 6, "Copyright 1987-2027");
    lcd_center(u, 7, "Midi Production Center");
    lcd_center(u, 8, OS_VERSION);
    title_credit(u, 9 * CELL_H + 6);
    lcd_rule(u, BOTTOM_ROW - 1, 0);
    lcd_text(u, 0, BOTTOM_ROW, "Searching for hard disk...");
    update_lights(u, s);
    ui_wake(u, (int)(s->title_until - now));
    return 1;
}

// The pads play and edit the active track's program. A MIDI track has none, so the last
// one stays.
static void follow_track(struct os *s) {
    pthread_mutex_lock(&s->seq.lock);
    const struct seq_track *k = &s->seq.sequence[s->seq.current].track[s->seq.track];
    int kit = k->type == TRACK_MIDI ? -1 : k->kit;
    pthread_mutex_unlock(&s->seq.lock);
    if (kit >= 0) os_set_program(s, kit);
}

// STOP pressed while a note still sounds with no pad or key held: a record of it, written to
// held-note-log.txt on the card (each time, over the last), to find out why a note was left on.
// The voices sounding, the releases the sequencer still has waiting, the pads' held notes, and
// the engine's last note events.
static void held_note_log(struct os *s, const struct input *in) {
    if (!s->data_dir) return;
    struct engine *e = s->engine;
    int stuck = 0;
    for (int i = 0; i < ENGINE_VOICES; i++) {
        const struct voice *v = &e->voice[i];
        stuck += v->stage != 0 && v->stage != 4 && !v->fading && (v->p.gate || v->p.loop_on); // 4: releasing
    }
    for (int i = 0; i < 16; i++) stuck = in->pad_down[i] ? 0 : stuck;
    for (int k = 0; k < 128; k++) stuck = s->midi_held[k] ? 0 : stuck;
    if (!stuck) return;
    char path[600];
    snprintf(path, sizeof path, "%s/held-note-log.txt", s->data_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    long long now = atomic_load(&e->clock);
    fprintf(f, "%s\nSTOP pressed with %d note(s) sounding and no pad held.\n", OS_VERSION, stuck);
    fprintf(f, "Program %d (type %d), 16 Levels %s type %d pad %d root %d, bank %c, screen %d\n", s->program + 1,
            s->programs[s->program].s600, s->levels_on ? "ON" : "off", s->levels_type, s->levels_pad, s->levels_root,
            'A' + s->bank, s->screen);
    pthread_mutex_lock(&s->seq.lock);
    struct seq *sq = &s->seq;
    fprintf(f, "Sequencer mode %d, pos %.1f, abs %.1f, tempo %.1f, TC %d, track %d\n", sq->mode, sq->pos, sq->abs,
            sq->sequence[sq->current].tempo10 / 10.0, sq->tc, sq->track + 1);
    fprintf(f, "\nReleases waiting (%d): slot key, ticks from now\n", sq->npending);
    for (int i = 0; i < sq->npending && i < 200; i++)
        fprintf(f, "  %s slot %d key %d in %.1f\n", sq->pending[i].midi ? "MIDI" : "pad", sq->pending[i].note, sq->pending[i].key,
                sq->pending[i].at - sq->abs);
    pthread_mutex_unlock(&s->seq.lock);
    fprintf(f, "\nPads holding notes: ");
    for (int i = 0; i < 16; i++)
        if (s->sounding[i] >= 0) fprintf(f, "pad %d slot %d key %d; ", i + 1, s->sounding[i], s->sounding_key[i]);
    fprintf(f, "\nNotes the pads started since all were last up: %d\n", s->nlive_on);
    fprintf(f, "\nVoices (stage 1 attack, 2 hold, 3 decay, 4 release):\n");
    for (int i = 0; i < ENGINE_VOICES; i++) {
        const struct voice *v = &e->voice[i];
        if (!v->stage) continue;
        fprintf(f, "  voice %d: slot %d key %d layer %d stage %d gate %d synth %d mono %d legato %d loop %d fading %d age %u\n", i,
                v->pad, v->key, v->layer, v->stage, v->p.gate, v->p.synth, v->p.mono, v->p.legato, v->p.loop_on, v->fading, v->age);
    }
    static const char *const KIND[] = {"", "PAD ON", "SEQ ON", "PAD OFF", "SEQ OFF", "LEGATO", "STEAL", "MONO CUT", "KILL"};
    unsigned n = atomic_load(&e->log_n), from = n > ENGINE_LOG ? n - ENGINE_LOG : 0;
    fprintf(f, "\nLast %u note events, oldest first (ms before STOP):\n", n - from);
    for (unsigned i = from; i < n; i++) {
        const struct engine_log *l = &e->log[i % ENGINE_LOG];
        int k = l->kind >= 0 && l->kind <= ELOG_KILL ? l->kind : 0;
        fprintf(f, "  %8.1f  %-8s slot %d key %d", (now - l->frame) * 1000.0 / ENGINE_RATE, KIND[k], l->pad, l->key);
        if (k == ELOG_LIVE_ON || k == ELOG_SEQ_ON) fprintf(f, "  voice %d offset %d", l->a, l->b);
        else if (k == ELOG_LIVE_OFF || k == ELOG_SEQ_OFF) fprintf(f, "  released %d", l->a);
        else if (k == ELOG_LEGATO) fprintf(f, "  voice %d from key %d", l->a, l->b);
        else fprintf(f, "  voice %d stage %d", l->a, l->b);
        fputc('\n', f);
    }
    fclose(f);
}

int os_frame(struct ui *u, const struct input *in, void *state) {
    struct os *s = state;
    engine_reap(s->engine);
    if (s->eq_seq != s->seq.current) os_eq_push_all(s), os_sends_push(s); // another sequence: its tracks' EQs and sends
    if (s->screen != OS_SAMPLE) s->s600_trim = 0; // the S600's Trim lasts while it shows
    if (s->screen != OS_MAIN) s->seq_ask = SEQ_ASK_NONE; // a question on the main page goes with it
    if (s->screen != OS_WARP && (s->warp_job || s->warp_result)) warp_close(s); // one warped sound, only while warping
    s->frame_ms = now_ms(u);
    if (in->knob_moved[0] || in->knob_moved[1] || in->knob_moved[2] || in->fader_moved) s->ctl_ms = s->frame_ms;
    // USB individual outs: the engine follows the settings and every pad's and track's output.
    // On once the outputs' USB device is up (always, with no device to wait for, as in the tests).
    if (s->engine) {
        struct engine *e = s->engine;
        int on = s->usb_outs && (s->usb_outs_state == 1 || !s->set_usb_outs);
        atomic_store_explicit(&e->outs_on, on, memory_order_relaxed);
        atomic_store_explicit(&e->outs_by_track, s->outs_by_track, memory_order_relaxed);
        atomic_store_explicit(&e->outs_keep, 1, memory_order_relaxed); // the MPC's own mix keeps every track
        if (s->set_outs_mix) s->set_outs_mix(0); // the eight are outputs: no main mix among them
        const struct seq_sequence *q = &s->seq.sequence[s->seq.current];
        for (int t = 0; on && t < SEQ_TRACKS && t + 1 < ENGINE_TRACKS; t++)
            atomic_store_explicit(&e->trk_out[t + 1], (unsigned char)q->track[t].usb_out, memory_order_relaxed),
            atomic_store_explicit(&e->trk_ovol[t + 1], (unsigned char)(100 - q->track[t].usb_vol_cut), memory_order_relaxed);
    }
    if (!s->start_ms) s->start_ms = s->frame_ms ? s->frame_ms : 1;
    for (int i = 0; i < 16; i++) // Step Edit's auditions, let go after a moment
        if (s->aud[i].until && s->frame_ms >= s->aud[i].until) {
            if (s->aud[i].channel > 0) seq_live_midi(&s->seq, s->aud[i].channel, s->aud[i].note, 0);
            else engine_note_off_key(s->engine, s->aud[i].id, s->aud[i].key);
            s->aud[i].until = 0;
        }
    recorder_service(&s->rec); // a take keeps going while other pages are shown
    looper_service(s, u);      // and the Live Looper's, and its drop-in
    int taking = s->at_running || (s->lp_state >= LP_COUNT && s->lp_state <= LP_ENDING); // the click: Audio Rec
    if (s->seq.mode == SEQ_STOPPED && s->lp_state == LP_IDLE) s->lp_session = 0;
    int after = s->lp_session && !taking; // and After Loop, between and after Live Looper takes
    if (taking != s->seq.audio_take || after != s->seq.loop_after) {
        pthread_mutex_lock(&s->seq.lock);
        s->seq.audio_take = taking, s->seq.loop_after = after;
        pthread_mutex_unlock(&s->seq.lock);
    }
    atomic_store_explicit(&s->fx.bpm10, s->seq.sequence[s->seq.current].tempo10, memory_order_relaxed); // synced delays
    atomic_store_explicit(&s->ins.bpm10, s->seq.sequence[s->seq.current].tempo10, memory_order_relaxed); // and the inserts'
    infx_follow_input(s); // LINE/MIC set on a record page
    s->master_dirty = 0; // Master Effects go with a project (and Autoload), not the settings: a blank start has them off
    if (s->fx_save_at && now_ms(u) >= s->fx_save_at) { // the Input Effects chain, a moment after the last change
        s->fx_save_at = 0;
        save_settings(s);
    }
    seq_drain(&s->seq);
    follow_track(s);
    if (in->main_moved) {
        float v = (float)in->main / 127.0f;
        atomic_store_explicit(&s->main_gain, v * v, memory_order_relaxed);
    }
    if (s->audio_poll) {
        int64_t now = now_ms(u);
        ui_wake(u, 300);
        if (now >= s->audio_poll_at) {
            s->audio_poll_at = now + 300;
            s->audio_poll();
        }
    }
    if (s->power_pressed) {
        ui_wake(u, 150); // the power key does not arrive through the panel, so look for it
        if (s->power_pressed() && s->screen != OS_SHUTDOWN) shutdown_prompt(s);
    }
    if (s->screen == OS_SHUTDOWN) {
        shutdown_frame(u, s, in);
        update_lights(u, s);
        return 0;
    }
    if (s->usbm_watch && s->usbmode && s->screen != OS_SHUTDOWN) {
        int st = s->usbmode->state(0, 0, 0);
        if (st == USBM_CONNECTED) s->screen = OS_USBMODE; // the card went to the computer after all
        if (st != USBM_WORKING) s->usbm_watch = 0;
    }
    if (s->screen == OS_USBMODE) { // locked: this page, and nothing else, until it is over
        unsigned char drop[3];
        for (int guard = 0; s->midi_in_read && guard < 256 && s->midi_in_read(drop, 0); guard++) {}
        usbmode_frame(u, s, in);
        update_lights(u, s);
        return 0;
    }
    struct input idle;
    if (s->disk_job) { // an export running: its progress shows, and nothing changes under it
        s->screen = OS_DISK;
        input_begin(&idle, 0);
        in = &idle;
    }
    if (s->screen == OS_TITLE) {
        if (title_frame(u, s, in)) return 0;
        input_begin(&idle, 0); // the press that skipped the title does nothing else
        in = &idle;
    }
    if (s->screen == OS_BROWSER && input_down(in, IN_SAMPLE_SELECT) && !in->shift && s->browser.cursor) {
        s->browser.cursor = 0; // pressed again: the top of the list
        preview_schedule(s, u);
    }
    if (input_down(in, IN_STOP)) {
        held_note_log(s, in);
        int64_t now = now_ms(u);
        if (now - s->last_stop_ms < STOP_DOUBLE_MS) engine_kill_all(s->engine);
        s->last_stop_ms = now;
    }
    // A note on MIDI IN starts a recording that waits for a key. Read every frame, so notes
    // that came in before the wait do not start it.
    if (s->midi_note_in && s->midi_note_in()) seq_wait_start(&s->seq);
    unsigned char midi_msg[3];
    if (s->midi_ports) s->midi_ports(s->midi.ext, s->midi.usb); // the MIDI settings, for both ports
    int midi_from = MIDI_PORT_EXT;
    for (int guard = 0; s->midi_in_read && guard < 256 && s->midi_in_read(midi_msg, &midi_from); guard++)
        midi_in(s, u, midi_msg, midi_from);
    s->seq.midi_off = !s->midi.ext && !s->midi.usb;
    s->seq.sync_send = s->midi.sync == SYNC_SEND;
    if (s->screen == OS_SAMPLE || s->screen == OS_MAIN || s->screen == OS_TC || s->screen == OS_METRO ||
        s->screen == OS_PROGRAM || s->screen == OS_ERASE || s->screen == OS_LOOP || s->screen == OS_S600 ||
        s->screen == OS_LEVELS || s->screen == OS_MIXER || s->screen == OS_RECORD ||
        s->screen == OS_REPEAT || s->screen == OS_ZONES ||
        s->screen == OS_EQ || s->screen == OS_MIDI || s->screen == OS_CCMAP || s->screen == OS_PADNOTES ||
        s->screen == OS_PRINT || s->screen == OS_INFX || s->screen == OS_MUTE || s->screen == OS_HELP ||
        s->screen == OS_MASTER || s->screen == OS_SENDS || s->screen == OS_SONG || s->screen == OS_STEP ||
        s->screen == OS_WARP || s->screen == OS_PREFS || s->screen == OS_DISK || s->screen == OS_SEQCOPY ||
        s->screen == OS_ARPEDIT || s->screen == OS_MULTI || s->screen == OS_INSERT || s->screen == OS_COPYEV ||
        s->screen == OS_COPYBARS || s->screen == OS_OUTS) {
        if (input_down(in, IN_MUTE) && !in->shift) mute_open(s); // the Track Mute page
        if (input_down(in, IN_MUTE) && in->shift) mute_unmute_all(s, u); // UNMUTE ALL
        if (input_down(in, IN_SAMPLE_REC) && in->shift) { // the main page: back a bar. Elsewhere: help
            if (s->screen == OS_MAIN) seq_locate_bar(&s->seq, -1);
            else if (!help_open(s, 0)) toast(s, u, "No help for this page");
        }
        if (input_down(in, IN_PAD_FX) && !in->shift) { // Print Effects
            if (s->screen == OS_PRINT) s->cursor_print = 0; // pressed again: the first field
            else print_open(s);
        }
        if (input_down(in, IN_KNOB_FX) && !in->shift) { // Input Effects
            if (s->screen == OS_INFX) infx_cursor_effect(s); // pressed again: Effect #
            else infx_open(s);
        }
        if (input_down(in, IN_SEQ)) {
            if (in->shift) step_open(s);
            else seq_go_main(s);
        } else if (input_down(in, IN_SAMPLE) && in->shift) { // the Program screen
            int type, channel;
            active_track(s, &type, &channel);
            if (type == TRACK_MIDI) toast(s, u, "MIDI track: no program");
            else if (s->screen == OS_PROGRAM) s->cursor_program = 0; // pressed again: the first field
            else s->screen = OS_PROGRAM;
        } else if (input_down(in, IN_SAMPLE) && !in->shift) {
            // On an S600 track SAMPLE opens the S600 screen (its <Trim> shows the Sample screen).
            if (s->screen == OS_S600) s600_open(s); // pressed again: the page's first field
            else if (os_plays_s600(s)) s600_open(s);
            else if (s->screen != OS_SAMPLE) s->screen = OS_SAMPLE;
        } else if (input_down(in, IN_SAMPLE_SELECT) && !in->shift) {
            open_browser(s, u);
        } else if (input_down(in, IN_SAMPLE_SELECT) && in->shift) { // Save/Load, ready to save the pad's sound
            disk_open(s, DISK_SAVE, DISK_SAMPLE);
        }
        if (input_down(in, IN_LOOP)) {
            if (in->shift) toggle(s, u, offsetof(struct pad_params, reverse), "Reverse: On", "Reverse: Off");
            else if (s->screen == OS_LOOP) s->cursor_loop = 0; // pressed again: the first field
            else open_loop(s);
        }
        if (input_down(in, IN_CHOP) && !in->shift) { // Zone Assign
            if (s->screen == OS_ZONES) s->cursor_zones = 0; // pressed again: the first field
            else zones_open(s);
        }
        if (input_down(in, IN_SAMPLE_REC) && !in->shift) record_open(s); // its four pages in turn
        if (input_down(in, IN_LEVELS)) {
            if (in->shift) { // the 16 Levels screen
                if (s->screen == OS_LEVELS) s->cursor_levels = 0;
                else levels_open(s);
            } else { // Velocity, then Tune, then off
                if (!s->levels_on) {
                    s->levels_on = 1;
                    s->levels_type = LEVELS_VELOCITY;
                    // The page's Sound and Root pad, or with no Sound picked there, the last pad played.
                    if (!s->levels_pad_set) s->levels_pad = s->selected;
                } else if (s->levels_type == LEVELS_VELOCITY) {
                    s->levels_type = LEVELS_TUNE;
                } else {
                    s->levels_on = 0;
                }
                toast(s, u, !s->levels_on ? "16 Levels: Off"
                            : s->levels_type == LEVELS_TUNE ? "16 Levels: Tune" : "16 Levels: Velocity");
            }
        }
        if (input_down(in, IN_PAD_BANK)) { // the mixer's banks stop at G, track 99 (and Track Mute's tracks, sequences)
            int tracks = s->screen == OS_MIXER || (s->screen == OS_MUTE && s->mute_type != MUTE_PAD && s->mute_type != MUTE_PAD_SOLO);
            int banks = tracks ? (SEQ_TRACKS + 15) / 16 : OS_BANKS;
            s->bank = (s->bank + (in->shift ? banks - 1 : 1)) % banks;
        }
        if (in->bank_jump) { // PAD BANK held, pad 1-8: straight to bank A-H (the mixer's last is G)
            int tracks = s->screen == OS_MIXER || (s->screen == OS_MUTE && s->mute_type != MUTE_PAD && s->mute_type != MUTE_PAD_SOLO);
            int banks = tracks ? (SEQ_TRACKS + 15) / 16 : OS_BANKS;
            s->bank = in->bank_jump - 1 < banks ? in->bank_jump - 1 : banks - 1;
        }
    }
    if (s->screen == OS_BROWSER && input_down(in, IN_SAMPLE_REC) && in->shift) help_open(s, 0); // its help too
    seq_transport(s, u, in);
    if (input_down(in, IN_ERASE)) s->erase_used = 0; // set again if a pad is hit with ERASE held
    int naming = s->rename_what != RENAME_NONE || disk_naming(s); // ERASE takes letters off the name instead
    if (input_down(in, IN_ERASE) && naming) s->erase_used = 1;
    if (input_down(in, IN_ERASE) && in->shift && !naming && s->screen != OS_TITLE && s->screen != OS_SHUTDOWN) {
        s->erase_used = 1; // COPY, not a tap of ERASE
        if (s->screen == OS_COPYEV) s->screen = s->ce_return; // pressed again: back
        else seq_copyev_open(s);
        ui_wake(u, 1);
    }
    // Note Repeat works while NOTE REPEAT is held. Shift + NOTE REPEAT: the page for its rate.
    if (input_down(in, IN_NOTE_REPEAT) && in->shift && s->screen != OS_TITLE) seq_repeat_open(s);
    else if (input_down(in, IN_NOTE_REPEAT)) seq_set_repeat(&s->seq, 1);
    else if (in->up & (1u << IN_NOTE_REPEAT)) seq_set_repeat(&s->seq, 0);
    // Held with it, B1-B3 switch to another rate for as long as they are held. The screen
    // underneath does not see them.
    struct input no_keys;
    unsigned keys = (1u << IN_B1) | (1u << IN_B2) | (1u << IN_B3);
    if (s->seq.repeat || (in->held & (1u << IN_NOTE_REPEAT))) {
        pthread_mutex_lock(&s->seq.lock);
        for (int b = 0; b < 3; b++) {
            int alt = seq_repeat_alt(s->seq.rep_rate, b);
            if (input_down(in, IN_B1 + b)) s->seq.rep_alt = s->rep_toggle && s->seq.rep_alt == alt ? -1 : alt;
            if ((in->up & (1u << (IN_B1 + b))) && !s->rep_toggle && s->seq.rep_alt == alt) s->seq.rep_alt = -1;
        }
        pthread_mutex_unlock(&s->seq.lock);
        no_keys = *in;
        no_keys.down &= ~keys;
        no_keys.up &= ~keys;
        in = &no_keys;
    }
    // Shift + -: Undo. Shift + +: Redo. The screen underneath does not see them.
    struct input no_undo;
    if (in->shift && (input_down(in, IN_MINUS) || input_down(in, IN_PLUS))) {
        if (input_down(in, IN_MINUS)) os_undo(s, u);
        else os_redo(s, u);
        no_undo = *in;
        no_undo.down &= ~((1u << IN_MINUS) | (1u << IN_PLUS));
        in = &no_undo;
    }
    if (!s->seq.repeat && s->seq.rep_alt >= 0 && !s->rep_toggle) { // NOTE REPEAT let go first (TOGGLE keeps it)
        pthread_mutex_lock(&s->seq.lock);
        s->seq.rep_alt = -1;
        pthread_mutex_unlock(&s->seq.lock);
    }
    if (!s->seq.repeat)
        for (int i = 0; i < 16; i++) s->repeat_pad[i] = 0;
    handle_pads(s, u, in);
    ui_clear(u, LCD_BG);
    if (s->usb_missing) { // USB chosen with no USB audio from a computer
        s->usb_missing = 0;
        toast(s, u, "USB audio not found");
    }
    if (s->arpedit_play && s->screen != OS_ARPEDIT) arpedit_leave(s); // left Pattern Edit: its preview stops
    if (s->rename_what == RENAME_SOUND && s->screen != OS_SAMPLE) s->rename_what = RENAME_NONE; // left the page: closed
    if (s->rename_what == RENAME_SONG && s->screen != OS_SONG) s->rename_what = RENAME_NONE;
    switch (s->screen) {
    case OS_MAIN:
        seq_screen_frame(s, u, in);
        break;
    case OS_TC:
        seq_tc_frame(s, u, in);
        break;
    case OS_METRO:
        seq_metro_frame(s, u, in);
        break;
    case OS_PROGRAM:
        seq_program_frame(s, u, in);
        break;
    case OS_ERASE:
        seq_erase_frame(s, u, in);
        break;
    case OS_SEQCOPY:
        seq_copy_frame(s, u, in);
        break;
    case OS_COPYEV:
        seq_copyev_frame(s, u, in);
        break;
    case OS_COPYBARS:
        seq_copybars_frame(s, u, in);
        break;
    case OS_OUTS:
        seq_outs_frame(s, u, in);
        break;
    case OS_ARPEDIT:
        arpedit_frame(s, u, in);
        break;
    case OS_MIXER:
        seq_mixer_frame(s, u, in);
        break;
    case OS_RECORD:
        record_frame(s, u, in);
        break;
    case OS_REPEAT:
        seq_repeat_frame(s, u, in);
        break;
    case OS_ZONES:
        zones_frame(s, u, in);
        break;
    case OS_EQ:
        eq_frame(s, u, in);
        break;
    case OS_MIDI:
        midi_frame(s, u, in);
        break;
    case OS_CCMAP:
        ccmap_frame(s, u, in);
        break;
    case OS_PADNOTES:
        padnotes_frame(s, u, in);
        break;
    case OS_PRINT:
        print_frame(s, u, in);
        break;
    case OS_INFX:
        infx_frame(s, u, in);
        break;
    case OS_MUTE:
        mute_frame(s, u, in);
        break;
    case OS_HELP:
        help_frame(s, u, in);
        break;
    case OS_MASTER:
        master_frame(s, u, in);
        break;
    case OS_SONG:
        song_frame(s, u, in);
        break;
    case OS_PREFS:
        prefs_frame(s, u, in);
        break;
    case OS_DISK:
        disk_frame(s, u, in);
        break;
    case OS_STEP:
        step_frame(s, u, in);
        break;
    case OS_WARP:
        warp_frame(s, u, in);
        break;
    case OS_SENDS:
        sends_frame(s, u, in);
        break;
    case OS_SAMPLE: {
        // ERASE on Sample: renames the sound (not the Erase page); the box takes the keys while open.
        struct input none;
        input_begin(&none, 0);
        if (s->rename_what == RENAME_SOUND) {
            draw_sample_screen(u, s, &none);
            seq_rename_overlay(s, u, in);
            break;
        }
        if ((in->up & (1u << IN_ERASE)) && !s->erase_used && s->frame_slot < 0 && !s->erase_ask && !s->extract) {
            s->erase_used = 1; // not the Erase page
            if (s->pads[s->selected].s) {
                seq_rename_sound(s);
                draw_sample_screen(u, s, &none);
                seq_rename_overlay(s, u, &none);
            } else {
                toast(s, u, "Pad is empty");
                draw_sample_screen(u, s, &none);
            }
            break;
        }
        handle_sample_screen(s, u, in);
        if (s->screen == OS_SAMPLE) draw_sample_screen(u, s, in);
        else ui_clear(u, LCD_BG);
        break;
    }
    case OS_S600:
        s600_frame(s, u, in);
        break;
    case OS_MULTI:
        multi_frame(s, u, in);
        break;
    case OS_INSERT:
        inserts_frame(s, u, in);
        break;
    case OS_LEVELS:
        levels_frame(s, u, in);
        break;
    case OS_LOOP:
        handle_loop_screen(s, u, in);
        if (s->screen == OS_LOOP) draw_loop_screen(u, s, in);
        break;
    case OS_BROWSER:
        handle_browser(s, u, in);
        break;
    case OS_PROJECT:
        draw_project(u, s, in);
        break;
    }
    // A tap of ERASE, stopped, opens the Erase screen (unless the Sample screen asked about
    // recorded changes instead). Held with pads while playing, it erases as it plays.
    int stopped = s->seq.mode == SEQ_STOPPED;
    if ((in->up & (1u << IN_ERASE)) && !s->erase_used && !s->erase_ask && stopped && s->screen != OS_ERASE &&
        s->screen != OS_BROWSER && s->screen != OS_PROJECT) {
        seq_erase_open(s);
        ui_wake(u, 1);
    } else if ((in->up & (1u << IN_ERASE)) && !s->erase_used && !s->erase_ask && stopped && s->screen == OS_ERASE) {
        s->cursor_erase = 0; // pressed again: the first field
        ui_wake(u, 1);
    }
    int shown = s->program; // a track change this frame switches the pads now, and redraws
    follow_track(s);
    if (s->program != shown) ui_wake(u, 1);
    // While NOTE REPEAT is held, the keys show the rates B1-B3 switch to, the held one lit.
    if (s->seq.repeat && s->screen != OS_SHUTDOWN) {
        lcd_fill(u, 0, KEY_ROW * CELL_H, UI_W, CELL_H, LCD_BG);
        for (int b = 0; b < 3; b++) {
            int alt = seq_repeat_alt(s->seq.rep_rate, b);
            char key[12];
            snprintf(key, sizeof key, "<%s>", seq_repeat_name(alt));
            if (alt >= 0 && alt == s->seq.rep_alt) lcd_inverse(u, b * 9, KEY_ROW, key);
            else lcd_text(u, b * 9, KEY_ROW, key);
        }
    }
    draw_toast(u, s);
    update_lights(u, s);
    return 0;
}
