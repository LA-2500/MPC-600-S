// PAD FX: Print Effects. One effect printed onto the selected pad's sound: what the pad plays
// (its trim) goes through the effect, its tail is added, and the result replaces the sound
// on the pad. The original stays in memory, and Undo brings it back.
//
// KNOB FX: Input Effects. A chain of up to seven effects on the live input, heard through
// Input Thru and recorded with them (Rec: WET) or clean (DRY). Presets for guitar and
// vintage keys start it off. The chain is kept on the card with the settings.
#include "lcd.h"
#include "os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PRINT_PAD (-4)    // the engine pad <Play> plays the print on
#define PRINT_TAIL_MAX 100 // tenths of a second
#define SAVE_DELAY_MS 2000 // the chain is written this long after the last change

static double tempo(const struct os *s) {
    int t = s->seq.sequence[s->seq.current].tempo10;
    return t > 0 ? t / 10.0 : 120;
}

static void knobs_free(struct os *s) { // after a new effect, the knobs catch up with it again
    for (int k = 0; k < 3; k++) s->knob_caught[k] = 0;
}

// K1-K3 on 0..127 controls: each takes over once it reaches the value, like everywhere.
// Returns which knob moved (0..2), or -1.
static int knob_params(struct os *s, const struct input *in, int *p) {
    int moved = -1;
    for (int k = 0; k < 3; k++) {
        if (!in->knob_moved[k]) continue;
        int last = s->knob_last[k], now = in->knob[k], at = p[k];
        s->knob_last[k] = now;
        if (!s->knob_caught[k]) {
            int crossed = last >= 0 && ((last <= at && now >= at) || (last >= at && now <= at));
            if (crossed || abs(now - at) <= 2) s->knob_caught[k] = 1;
        }
        if (s->knob_caught[k]) p[k] = now;
        moved = k;
    }
    return moved;
}

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void draw_field(struct ui *u, int col, int row, const char *v, int on) {
    char b[32];
    snprintf(b, sizeof b, " %s ", v);
    if (on) lcd_inverse(u, col, row, b);
    else lcd_text(u, col, row, b);
}

// ---- Print Effects ----

enum { P_CAT, P_FX, P_1, P_2, P_3, P_TAIL, P_PAD, P_FIELDS };

static void print_set_type(struct os *s, int type) {
    s->print_type = type;
    for (int i = 0; i < 3; i++) s->print_p[i] = fx_param_default(type, i);
    // A printed sound starts with a short tail: reverbs at a third of their Decay, delays
    // with fewer repeats. The controls still reach as long as on Input Effects.
    int c = fx_cat_of(type);
    if (c == FXC_REVERB) s->print_p[0] = s->print_p[0] / 3;
    if (c == FXC_DELAY && type != FX_SLAP && type != FX_TAPE) s->print_p[1] = s->print_p[1] / 2;
    if (type == FX_TAPE) s->print_p[1] = s->print_p[1] / 2;
    knobs_free(s);
}

void print_open(struct os *s) {
    if (s->screen != OS_PRINT && s->screen != OS_INFX) s->print_return = s->screen;
    s->screen = OS_PRINT;
    s->cursor_print = 0;
    knobs_free(s);
}

// The selected pad's sound through the effect, at 44.1 kHz in stereo: its trim played as the
// engine plays it (a lower rate's samples held), then the tail. NULL with the reason in *why.
static struct sample *print_render(struct os *s, const char **why) {
    const struct os_pad *pad = &s->pads[s->selected];
    const struct sample *src = pad->s;
    if (!src) {
        *why = "Pad is empty";
        return 0;
    }
    if (!s->print_fx && !(s->print_fx = fx_new())) {
        *why = "Out of memory";
        return 0;
    }
    struct fx *f = s->print_fx;
    fx_set(f, s->print_type);
    for (int i = 0; i < 3; i++) f->p[i] = s->print_p[i];
    f->bpm = tempo(s);
    long st = pad->p.start, en = pad->p.end;
    if (en > src->frames || en <= 0) en = src->frames;
    if (st < 0 || st >= en) st = 0;
    long n = (long)((double)(en - st) * ENGINE_RATE / src->rate);
    if (n < 1) n = 1;
    double tail_s = s->print_tail ? s->print_tail / 10.0 : fx_tail_seconds(f);
    long tail = (long)(tail_s * ENGINE_RATE), total = n + tail;
    struct sample *out = sample_new((int32_t)total, 2, ENGINE_RATE);
    if (!out) {
        *why = "Out of memory";
        return 0;
    }
    for (long i = 0; i < total; i++) {
        float l = 0, r = 0;
        if (i < n) {
            long j = st + (long)((double)i * src->rate / ENGINE_RATE);
            if (j >= en) j = en - 1;
            l = src->data[j * src->channels];
            r = src->channels > 1 ? src->data[j * src->channels + 1] : l;
        }
        out->data[i * 2] = l, out->data[i * 2 + 1] = r;
    }
    for (long i = 0; i < total; i += 1024) fx_process(f, out->data + i * 2, (int)(total - i < 1024 ? total - i : 1024));
    if (!s->print_tail) { // AUTO: the tail ends where it falls below -54 dB
        long last = n;
        for (long i = total - 1; i >= n; i--)
            if (out->data[i * 2] > 0.002f || out->data[i * 2] < -0.002f || out->data[i * 2 + 1] > 0.002f ||
                out->data[i * 2 + 1] < -0.002f) {
                last = i + 1;
                break;
            }
        out->frames = (int32_t)last;
    }
    return out;
}

static void print_preview(struct os *s, struct ui *u) {
    const char *why = "";
    struct sample *n = print_render(s, &why);
    if (!n) {
        os_toast(s, u, why);
        return;
    }
    if (s->print_preview) engine_retire(s->engine, s->print_preview);
    s->print_preview = n;
    struct pad_params p = s->pads[s->selected].p; // as the pad would play it
    p.start = 0, p.end = n->frames, p.loop = 0, p.loop_on = 0, p.reverse = 0, p.gate = 0;
    p.mono = 1, p.mute_group = 0, p.also_pad = 0, p.soft_start_ms = 0, p.synth = 0, p.layers = 1;
    engine_note_on(s->engine, PRINT_PAD, 127, n, &p);
}

static void print_now(struct os *s, struct ui *u) {
    int slot = s->selected;
    struct os_pad *pad = &s->pads[slot];
    const char *why = "";
    struct sample *old = pad->s, *n = print_render(s, &why);
    if (!n) {
        os_toast(s, u, why);
        return;
    }
    int kept = 0;
    for (int i = 0; i < s->nmemory; i++) kept |= s->memory[i].s == old;
    if (!kept && os_memory_add(s, old, pad->name) != 0) {
        sample_free(n);
        os_toast(s, u, "Memory full");
        return;
    }
    os_undo_save(s, "Print FX");
    struct pad_params keep = pad->p;
    long st = keep.start < 0 || keep.start >= old->frames ? 0 : keep.start;
    char name[40];
    snprintf(name, sizeof name, "%.16s-%s", pad->name, fx_short(s->print_type));
    os_pad_assign(s, slot, n, name);
    pad->p = keep; // the trim is the whole print now; a loop keeps its place in the sound
    pad->p.start = 0;
    pad->p.end = n->frames;
    long loop = (long)((double)(keep.loop - st) * ENGINE_RATE / old->rate);
    pad->p.loop = (int32_t)(loop < 0 || loop >= n->frames ? 0 : loop);
    os_pad_push(s, slot);
    os_toast(s, u, "Done"); // the original is in memory
}

void print_frame(struct os *s, struct ui *u, const struct input *in) {
    if (input_down(in, IN_B1)) { // <Exit>
        s->screen = s->print_return == OS_PRINT || s->print_return == OS_INFX ? OS_MAIN : s->print_return;
        ui_wake(u, 1);
        return;
    }
    if (input_down(in, IN_B2)) print_preview(s, u);
    if (input_down(in, IN_B3)) print_now(s, u);
    if (s->print_type <= FX_OFF || s->print_type >= FX_COUNT) print_set_type(s, FX_ASR_HALL);
    if (in->encoder_press) s->cursor_print = (s->cursor_print + (in->shift ? P_FIELDS - 1 : 1)) % P_FIELDS;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    int cat = fx_cat_of(s->print_type);
    if (d) switch (s->cursor_print) {
        case P_CAT: {
            int c = clamp(cat + d, 0, FX_CATS - 1);
            if (c != cat) print_set_type(s, fx_cat_first(c));
            break;
        }
        case P_FX: { // within the category
            int t = fx_step(s->print_type, d, cat);
            if (t != s->print_type) print_set_type(s, t);
            break;
        }
        case P_1: case P_2: case P_3: {
            int *v = &s->print_p[s->cursor_print - P_1];
            *v = clamp(*v + d, 0, 127);
            break;
        }
        case P_TAIL: s->print_tail = clamp(s->print_tail + d * (s->print_tail >= 20 ? 5 : 1), 0, PRINT_TAIL_MAX); break;
        case P_PAD: s->selected = clamp(s->selected + d, 0, OS_PADS - 1); break;
        }
    int k = knob_params(s, in, s->print_p);
    if (k >= 0) s->cursor_print = P_1 + k;

    char v[40];
    lcd_text(u, 0, KEY_ROW, "<Exit>");
    lcd_text(u, 9, KEY_ROW, "<Play>");
    lcd_text(u, 18, KEY_ROW, "<Print>");
    lcd_rule(u, TITLE_ROW, "Print Effects");
    lcd_text(u, 0, 4, "Category:");
    draw_field(u, 9, 4, fx_cat_name(cat), s->cursor_print == P_CAT);
    lcd_text(u, 0, 5, "Effect:");
    draw_field(u, 9, 5, fx_name(s->print_type), s->cursor_print == P_FX);
    for (int i = 0; i < 3; i++) {
        snprintf(v, sizeof v, "K%d %s:", i + 1, fx_param_name(s->print_type, i));
        lcd_text(u, 0, 7 + i, v);
        fx_param_text(s->print_type, i, s->print_p[i], tempo(s), v, sizeof v);
        draw_field(u, 15, 7 + i, v, s->cursor_print == P_1 + i);
    }
    lcd_text(u, 0, 11, "Tail:");
    if (s->print_tail) {
        snprintf(v, sizeof v, "%d.%ds", s->print_tail / 10, s->print_tail % 10);
    } else {
        struct fx f = {s->print_type, {s->print_p[0], s->print_p[1], s->print_p[2]}, tempo(s), 0};
        double t = fx_tail_seconds(&f);
        snprintf(v, sizeof v, "AUTO (%.1fs)", t);
    }
    draw_field(u, 9, 11, v, s->cursor_print == P_TAIL);
    lcd_text(u, 0, 12, "Pad:");
    const struct os_pad *p = &s->pads[s->selected];
    snprintf(v, sizeof v, "%c%02d %.11s", 'A' + s->selected / 16, s->selected % 16 + 1, p->s ? p->name : "(empty)");
    draw_field(u, 9, 12, v, s->cursor_print == P_PAD);
    lcd_rule(u, BOTTOM_ROW, 0);
}

// ---- Input Effects ----

// Each kind of chain keeps its effects in fixed places, so one left out of a preset can be put
// in later where it belongs, without moving the rest.
enum { G_GATE, G_COMP, G_WAH, G_OD, G_AMP, G_EQ, G_MOD, G_TIME, G_SPKR };   // guitar and bass
enum { K_EQ, K_COMP, K_WAH, K_OD, K_MOD, K_TIME };                        // keyboard
enum { T_GUITAR, T_BASS, T_KEYS, T_OTHER, T_MIX, T_COUNT };
static const char *const TYPE_NAMES[T_COUNT] = {"GUITAR", "BASS", "KEYBOARD", "OTHER", "MIX"};

// Speaker cabinets, as the Cab control's values.
#define CAB_1X12 0
#define CAB_2X12 40
#define CAB_4X10 80
#define CAB_4X12 110

struct slot_preset {
    int type, p[3];
};
// Levels: each effect sits near unity gain at its defaults, so these set the sound, not the
// volume. Drives stay light to medium; reverbs and delays sit behind the playing.
static const struct {
    int kind;
    const char *name;
    struct slot_preset slot[FX_SLOTS];
} PRESETS[] = {
    {T_GUITAR, "EMPTY", {{0}}},
    {T_GUITAR, "SPARKLE CLEAN", // a blackface clean: bright, open, a little room
     {[G_AMP] = {FX_BASSMAN, {30, 72, 45}}, [G_TIME] = {FX_ROOM, {45, 70, 20}}, [G_SPKR] = {FX_SPEAKER, {CAB_1X12, 72, 64}}}},
    {T_GUITAR, "CLEAN CHORUS", // compressed and wide, Juno I, a plate behind it
     {[G_COMP] = {FX_CS3, {45, 30, 70}}, [G_AMP] = {FX_BASSMAN, {28, 68, 40}}, [G_MOD] = {FX_JUNO, {0, 90, 50}},
      [G_TIME] = {FX_PLATE, {55, 85, 22}}, [G_SPKR] = {FX_SPEAKER, {CAB_1X12, 70, 64}}}},
    {T_GUITAR, "SURF", // drippy spring, bright amp, 4x10
     {[G_AMP] = {FX_BASSMAN, {32, 85, 60}}, [G_TIME] = {FX_SPRING, {100, 85, 55}}, [G_SPKR] = {FX_SPEAKER, {CAB_4X10, 75, 64}}}},
    {T_GUITAR, "TREM SPRING", // amp tremolo at a slow sway, spring under it
     {[G_AMP] = {FX_BASSMAN, {30, 64, 45}}, [G_MOD] = {FX_RHODES, {48, 55, 0}}, [G_TIME] = {FX_SPRING, {75, 70, 35}},
      [G_SPKR] = {FX_SPEAKER, {CAB_1X12, 64, 64}}}},
    {T_GUITAR, "VIBE DRIVE", // a touch of drive into a slow throbbing vibe
     {[G_GATE] = {FX_GATE, {32, 40, 100}}, [G_OD] = {FX_OD1, {35, 70, 127}}, [G_AMP] = {FX_BASSMAN, {35, 60, 50}},
      [G_MOD] = {FX_UNIVIBE, {38, 85, 0}}, [G_TIME] = {FX_ROOM, {40, 70, 18}}, [G_SPKR] = {FX_SPEAKER, {CAB_4X12, 60, 64}}}},
    {T_GUITAR, "FUNK AUTO-WAH", // squashed, quacking with the picking, dry
     {[G_COMP] = {FX_DYNACOMP, {65, 60, 10}}, [G_WAH] = {FX_AUTOWAH, {75, 80, 60}}, [G_AMP] = {FX_BASSMAN, {25, 70, 40}},
      [G_TIME] = {FX_ROOM, {35, 80, 15}}, [G_SPKR] = {FX_SPEAKER, {CAB_1X12, 70, 64}}}},
    {T_GUITAR, "WAH PEDAL", // the wah on the mod wheel or the slider, into a pushed amp
     {[G_GATE] = {FX_GATE, {34, 40, 110}}, [G_WAH] = {FX_WAH, {64, 64, 64}}, [G_OD] = {FX_OD1, {50, 70, 127}},
      [G_AMP] = {FX_BASSMAN, {40, 60, 50}}, [G_TIME] = {FX_ROOM, {40, 70, 18}}, [G_SPKR] = {FX_SPEAKER, {CAB_4X12, 64, 64}}}},
    {T_GUITAR, "CHIME", // a clean boost into a top-boost amp, dotted-eighth echoes
     {[G_OD] = {FX_KLON, {20, 72, 70}}, [G_AMP] = {FX_AC30, {35, 40, 80}}, [G_TIME] = {FX_DDL, {104, 30, 22}},
      [G_SPKR] = {FX_SPEAKER, {CAB_2X12, 70, 64}}}},
    {T_GUITAR, "EDGE OF BREAKUP", // clean when soft, breaking up when dug in
     {[G_OD] = {FX_KLON, {35, 64, 70}}, [G_AMP] = {FX_AC30, {50, 45, 80}}, [G_TIME] = {FX_PLATE, {55, 80, 20}},
      [G_SPKR] = {FX_SPEAKER, {CAB_2X12, 64, 64}}}},
    {T_GUITAR, "BLUES OD", // an OD-1 into a tweed, a short tape slap
     {[G_GATE] = {FX_GATE, {30, 40, 100}}, [G_OD] = {FX_OD1, {50, 70, 127}}, [G_AMP] = {FX_BASSMAN, {40, 58, 55}},
      [G_TIME] = {FX_TAPE, {45, 30, 25}}, [G_SPKR] = {FX_SPEAKER, {CAB_4X10, 64, 64}}}},
    {T_GUITAR, "BRIT CRUNCH", // medium gain, a closed 4x12
     {[G_GATE] = {FX_GATE, {36, 40, 110}}, [G_OD] = {FX_OD1, {60, 70, 127}}, [G_AMP] = {FX_AC30, {55, 45, 75}},
      [G_TIME] = {FX_ROOM, {40, 70, 18}}, [G_SPKR] = {FX_SPEAKER, {CAB_4X12, 64, 64}}}},
    {T_GUITAR, "PHASE 90", // a slow Phase 90 swirl over a warm amp
     {[G_AMP] = {FX_BASSMAN, {38, 60, 50}}, [G_MOD] = {FX_MXR_PH, {30, 100, 64}}, [G_TIME] = {FX_ROOM, {40, 70, 18}},
      [G_SPKR] = {FX_SPEAKER, {CAB_4X12, 64, 64}}}},
    {T_GUITAR, "AMBIENT CLEAN", // Juno II and a long hall: chords that float
     {[G_COMP] = {FX_CS3, {55, 40, 60}}, [G_AMP] = {FX_BASSMAN, {25, 64, 40}}, [G_MOD] = {FX_JUNO, {64, 80, 45}},
      [G_TIME] = {FX_ASR_HALL, {100, 55, 40}}, [G_SPKR] = {FX_SPEAKER, {CAB_1X12, 64, 64}}}},
    // Jazz: the amp barely working, the top rolled off, the mic off the cone's axis.
    {T_GUITAR, "JAZZ CLEAN",
     {[G_AMP] = {FX_BASSMAN, {18, 40, 20}}, [G_EQ] = {FX_EQ3, {70, 64, 45}}, [G_TIME] = {FX_ROOM, {45, 55, 18}},
      [G_SPKR] = {FX_SPEAKER, {CAB_1X12, 40, 64}}}},
    {T_GUITAR, "JAZZ CHORUS",
     {[G_COMP] = {FX_CS3, {35, 50, 50}}, [G_AMP] = {FX_BASSMAN, {18, 45, 20}}, [G_EQ] = {FX_EQ3, {68, 64, 48}},
      [G_MOD] = {FX_JUNO, {0, 60, 35}}, [G_TIME] = {FX_PLATE, {55, 65, 20}}, [G_SPKR] = {FX_SPEAKER, {CAB_1X12, 45, 64}}}},
    {T_GUITAR, "JAZZ ECHO",
     {[G_AMP] = {FX_BASSMAN, {18, 45, 20}}, [G_EQ] = {FX_EQ3, {68, 62, 46}}, [G_TIME] = {FX_ANALOG, {50, 25, 18}},
      [G_SPKR] = {FX_SPEAKER, {CAB_1X12, 45, 64}}}},
    {T_GUITAR, "JAZZ BALLAD",
     {[G_COMP] = {FX_CS3, {45, 60, 50}}, [G_AMP] = {FX_BASSMAN, {18, 45, 20}}, [G_EQ] = {FX_EQ3, {66, 64, 50}},
      [G_TIME] = {FX_ASR_HALL, {75, 50, 28}}, [G_SPKR] = {FX_SPEAKER, {CAB_1X12, 50, 64}}}},

    {T_BASS, "EMPTY", {{0}}},
    {T_BASS, "DI", // straight into the desk: a compressor and EQ only
     {[G_COMP] = {FX_CS3, {55, 30, 64}}, [G_EQ] = {FX_EQ3, {70, 58, 66}}}},
    {T_BASS, "AMP", // a tweed with a 4x10, warm
     {[G_COMP] = {FX_CS3, {45, 40, 64}}, [G_AMP] = {FX_BASSMAN, {30, 50, 30}}, [G_EQ] = {FX_EQ3, {68, 64, 60}},
      [G_SPKR] = {FX_SPEAKER, {CAB_4X10, 50, 64}}}},
    {T_BASS, "FUNK", // squashed, an envelope filter on the attack
     {[G_COMP] = {FX_DYNACOMP, {60, 60, 15}}, [G_WAH] = {FX_AUTOWAH, {65, 60, 55}}, [G_EQ] = {FX_EQ3, {70, 60, 68}}}},

    {T_KEYS, "EMPTY", {{0}}},
    {T_KEYS, "RHODES TREM", // the Suitcase's stereo pan
     {[K_EQ] = {FX_EQ3, {68, 62, 60}}, [K_MOD] = {FX_RHODES, {52, 60, 127}}, [K_TIME] = {FX_ROOM, {45, 75, 20}}}},
    {T_KEYS, "RHODES CHORUS",
     {[K_EQ] = {FX_EQ3, {68, 64, 58}}, [K_MOD] = {FX_JUNO, {0, 75, 45}}, [K_TIME] = {FX_PLATE, {55, 75, 22}}}},
    {T_KEYS, "RHODES PHASE", // a Small Stone, slow
     {[K_EQ] = {FX_EQ3, {66, 64, 60}}, [K_MOD] = {FX_SMALLSTONE, {20, 80, 55}}, [K_TIME] = {FX_PLATE, {55, 80, 22}}}},
    {T_KEYS, "WURLI", // a little bark, a gentle trem, spring
     {[K_EQ] = {FX_EQ3, {66, 68, 62}}, [K_OD] = {FX_OD1, {25, 70, 70}}, [K_MOD] = {FX_RHODES, {45, 40, 0}},
      [K_TIME] = {FX_SPRING, {60, 70, 25}}}},
    {T_KEYS, "ORGAN VIBE", // a light drive and a vibe spinning fast
     {[K_OD] = {FX_OD1, {30, 70, 80}}, [K_MOD] = {FX_UNIVIBE, {65, 75, 0}}, [K_TIME] = {FX_ROOM, {40, 70, 18}}}},
    {T_KEYS, "CLAV", // compressed, an auto-wah on every note
     {[K_EQ] = {FX_EQ3, {60, 66, 70}}, [K_COMP] = {FX_DYNACOMP, {55, 60, 10}}, [K_WAH] = {FX_AUTOWAH, {80, 80, 55}},
      [K_TIME] = {FX_ROOM, {30, 80, 12}}}},
    {T_KEYS, "JUNO PAD", {[K_MOD] = {FX_JUNO, {127, 85, 64}}, [K_TIME] = {FX_ASR_HALL, {90, 55, 35}}}},
    {T_KEYS, "STRINGS", // string machine through a slow phaser
     {[K_MOD] = {FX_DP4, {15, 85, 60}}, [K_TIME] = {FX_ASR_HALL, {85, 65, 32}}}},
    {T_KEYS, "DUB", {[K_EQ] = {FX_EQ3, {64, 70, 56}}, [K_TIME] = {FX_DUB, {103, 75, 35}}}},
    {T_KEYS, "JAZZ RHODES",
     {[K_EQ] = {FX_EQ3, {68, 62, 56}}, [K_COMP] = {FX_CS3, {35, 40, 60}}, [K_MOD] = {FX_JUNO, {0, 70, 25}},
      [K_TIME] = {FX_ROOM, {45, 65, 18}}}},
    {T_KEYS, "JAZZ TREM",
     {[K_EQ] = {FX_EQ3, {66, 64, 58}}, [K_MOD] = {FX_RHODES, {40, 30, 127}}, [K_TIME] = {FX_PLATE, {50, 70, 20}}}},
    {T_KEYS, "JAZZ ORGAN",
     {[K_OD] = {FX_OD1, {20, 70, 50}}, [K_MOD] = {FX_UNIVIBE, {50, 70, 0}}, [K_TIME] = {FX_ROOM, {40, 65, 18}}}},
    {T_KEYS, "JAZZ BALLAD",
     {[K_EQ] = {FX_EQ3, {70, 60, 54}}, [K_COMP] = {FX_CS3, {40, 40, 60}}, [K_TIME] = {FX_ASR_HALL, {75, 50, 28}}}},

    {T_OTHER, "EMPTY", {{0}}},
    {T_OTHER, "VOCAL", {{FX_GATE, {30, 50, 90}}, {FX_CS3, {55, 25, 70}}, {FX_EQ3, {58, 66, 70}}, {FX_PLATE, {55, 85, 25}}}},
    {T_OTHER, "SYNTH WIDE", {{FX_JUNO, {64, 90, 64}}, {FX_PINGPONG, {93, 45, 30}}}},
    {T_OTHER, "DRUM ROOM", {{FX_DYNACOMP, {50, 64, 40}}, {FX_EQ3, {70, 60, 66}}, {FX_ROOM, {40, 80, 25}}}},

    // MIX: for Track/Mixer, where each effect is a send of its own, so these are nine side by
    // side, not a chain. Reverbs and delays are all effect (Mix up full): the send sets how
    // much. Compressors and drives come back whole, for parallel squash and grit.
    {T_MIX, "USER TEMPLATE", {{0}}}, // blank: nine sends to build your own, then save as TRACK FX
    {T_MIX, "STUDIO",
     {{FX_ROOM, {45, 75, 127}}, {FX_PLATE, {65, 85, 127}}, {FX_ASR_HALL, {85, 60, 127}}, {FX_DDL, {104, 35, 127}},
      {FX_TAPE, {109, 40, 30}}, {FX_JUNO, {0, 90, 127}}, {FX_CS3, {100, 10, 64}}, {FX_OD1, {60, 70, 127}},
      {FX_VINYL, {60, 15, 15}}}},
    {T_MIX, "HIP-HOP",
     {{FX_ROOM, {35, 70, 127}}, {FX_PLATE, {55, 80, 127}}, {FX_SPRING, {60, 65, 127}}, {FX_TAPE, {93, 35, 25}},
      {FX_DUB, {119, 70, 127}}, {FX_VINYL, {70, 30, 25}}, {FX_CS3, {100, 5, 70}}, {FX_JUNO, {64, 85, 127}},
      {FX_GATED, {50, 80, 127}}}},
    {T_MIX, "DUB",
     {{FX_SPRING, {85, 75, 127}}, {FX_DUB, {119, 85, 127}}, {FX_TAPE, {104, 60, 40}}, {FX_ANALOG, {93, 55, 127}},
      {FX_ROOM, {40, 70, 127}}, {FX_DP4, {20, 90, 70}}, {FX_FLANGER, {15, 80, 95}}, {FX_PLATE, {70, 70, 127}}}},
    {T_MIX, "AMBIENT",
     {{FX_ASR_HALL, {127, 50, 127}}, {FX_PLATE, {90, 70, 127}}, {FX_PINGPONG, {109, 65, 127}}, {FX_TAPE, {109, 55, 50}},
      {FX_JUNO, {127, 70, 127}}, {FX_SMALLSTONE, {15, 80, 127}}, {FX_ROOM, {50, 60, 127}}}},
    {T_MIX, "LO-FI",
     {{FX_VINYL, {80, 60, 40}}, {FX_ROOM, {30, 40, 127}}, {FX_TAPE, {93, 40, 80}}, {FX_SPRING, {60, 55, 127}},
      {FX_OD1, {70, 50, 127}}, {FX_EQ_808, {64, 30, 90}}}},
    // Added later, at the end so saved preset numbers stay valid; each shows in its Category.
    {T_GUITAR, "VOX CLEAN", // a top-boost amp just under breakup, 2x12 Blues, a little room
     {[G_AMP] = {FX_AC30, {28, 35, 80}}, [G_TIME] = {FX_ROOM, {40, 75, 16}}, [G_SPKR] = {FX_SPEAKER, {CAB_2X12, 70, 64}}}},
    {T_GUITAR, "VOX JANGLE", // compressed and bright for chiming chords, a short plate
     {[G_COMP] = {FX_CS3, {55, 25, 80}}, [G_AMP] = {FX_AC30, {35, 25, 80}}, [G_TIME] = {FX_PLATE, {50, 90, 18}},
      [G_SPKR] = {FX_SPEAKER, {CAB_2X12, 80, 64}}}},
    {T_GUITAR, "VOX CRUNCH", // a Klon pushing the top-boost amp into crunch
     {[G_GATE] = {FX_GATE, {34, 40, 110}}, [G_OD] = {FX_KLON, {45, 60, 70}}, [G_AMP] = {FX_AC30, {65, 40, 75}},
      [G_TIME] = {FX_ROOM, {40, 70, 16}}, [G_SPKR] = {FX_SPEAKER, {CAB_2X12, 64, 64}}}},
    {T_GUITAR, "TWEED CRUNCH", // the tweed turned up on its own, 4x10, a tape slap
     {[G_AMP] = {FX_BASSMAN, {70, 55, 60}}, [G_TIME] = {FX_TAPE, {40, 25, 20}}, [G_SPKR] = {FX_SPEAKER, {CAB_4X10, 64, 64}}}},
    {T_GUITAR, "CRUNCH RHYTHM", // an OD-1 into a warm amp and a closed 4x12, tight and dry
     {[G_GATE] = {FX_GATE, {36, 40, 110}}, [G_OD] = {FX_OD1, {55, 65, 127}}, [G_AMP] = {FX_BASSMAN, {50, 55, 55}},
      [G_TIME] = {FX_ROOM, {35, 70, 14}}, [G_SPKR] = {FX_SPEAKER, {CAB_4X12, 60, 64}}}},
    {T_GUITAR, "HOT CRUNCH", // a DS-1 at low gain into the top-boost amp: more bite
     {[G_GATE] = {FX_GATE, {38, 40, 110}}, [G_OD] = {FX_DS1, {45, 55, 60}}, [G_AMP] = {FX_AC30, {55, 45, 70}},
      [G_TIME] = {FX_ROOM, {35, 70, 14}}, [G_SPKR] = {FX_SPEAKER, {CAB_4X12, 64, 64}}}},
    {T_GUITAR, "PLEXI", // the JTM45 turned up, a closed 4x12 of Greenbacks, a little room
     {[G_GATE] = {FX_GATE, {36, 40, 110}}, [G_AMP] = {FX_JTM45, {70, 60, 55}}, [G_TIME] = {FX_ROOM, {40, 70, 15}},
      [G_SPKR] = {FX_SPEAKER, {CAB_4X12, 64, 64}}}},
    {T_GUITAR, "JC CLEAN", // the solid-state clean with its chorus, bright, a plate behind
     {[G_COMP] = {FX_CS3, {45, 30, 70}}, [G_AMP] = {FX_JC, {55, 70, 64}}, [G_TIME] = {FX_PLATE, {50, 85, 16}}}},
    {T_GUITAR, "FUZZ FACE", // the fuzz into a clean-ish JTM45: thick, spitting when dug in
     {[G_OD] = {FX_FUZZ, {90, 64, 64}}, [G_AMP] = {FX_JTM45, {30, 55, 45}}, [G_TIME] = {FX_ROOM, {40, 70, 15}},
      [G_SPKR] = {FX_SPEAKER, {CAB_4X12, 60, 64}}}},
    {T_GUITAR, "DC30 DRIVE", // the class A amp pushed, 2x12, a short echo
     {[G_GATE] = {FX_GATE, {34, 40, 110}}, [G_AMP] = {FX_DC30, {70, 70, 35}}, [G_TIME] = {FX_DDL, {50, 20, 15}},
      [G_SPKR] = {FX_SPEAKER, {CAB_2X12, 64, 64}}}},
    {T_BASS, "JAMERSON", // a P-bass with flatwounds and a foam mute into a B-15: round, dark and short
     {[G_COMP] = {FX_CS3, {40, 40, 40}}, [G_AMP] = {FX_B15, {76, 40, 55}}, [G_EQ] = {FX_EQ3, {66, 66, 46}}}},
    {T_KEYS, "MFC42 SWEEP", // the filter swept slowly by its LFO, resonance up
     {[K_MOD] = {FX_MFC42, {45, 70, 115}}, [K_TIME] = {FX_ROOM, {45, 70, 18}}}},
};
#define NPRESETS ((int)(sizeof PRESETS / sizeof PRESETS[0]))

int infx_presets(void) {
    return NPRESETS;
}

const char *infx_preset_name(int i) {
    return i >= 0 && i < NPRESETS ? PRESETS[i].name : "";
}

int infx_preset_kind(int i) {
    return i >= 0 && i < NPRESETS ? PRESETS[i].kind : 0;
}

static int first_of_kind(int kind) {
    for (int i = 0; i < NPRESETS; i++)
        if (PRESETS[i].kind == kind) return i;
    return 0;
}

// A chain has one reverb and one delay at most: a type is blocked from a slot while another
// slot has one of its kind. Track/Mixer's nine sends are side by side, so there it is free.
static int blocked(const struct os *s, int slot, int type) {
    int c = fx_cat_of(type);
    if (type == FX_OFF || (c != FXC_REVERB && c != FXC_DELAY) || s->infx_mode == FXM_TRACK) return 0;
    for (int i = 0; i < FX_SLOTS; i++) {
        int t = atomic_load((atomic_int *)&s->fx.type[i]);
        if (i != slot && t != FX_OFF && fx_cat_of(t) == c) return 1;
    }
    return 0;
}

static void slot_set(struct os *s, int i, int type, const int *p) {
    for (int k = 0; k < 3; k++) atomic_store(&s->fx.p[i][k], p ? p[k] : fx_param_default(type, k));
    atomic_store(&s->fx.type[i], type);
}

static int64_t now_ms(const struct ui *u) {
    if (u->now_ms) return u->now_ms;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void changed(struct os *s, const struct ui *u) {
    s->fx_save_at = now_ms(u) + SAVE_DELAY_MS;
}

static int chain_empty(const struct os *s) {
    for (int i = 0; i < FX_SLOTS; i++)
        if (atomic_load((atomic_int *)&s->fx.type[i]) != FX_OFF) return 0;
    return 1;
}

// A preset; one with effects in it turns the effects on.
void infx_load_preset(struct os *s, int i) {
    if (i < 0 || i >= NPRESETS) return;
    for (int k = 0; k < FX_SLOTS; k++) slot_set(s, k, PRESETS[i].slot[k].type, PRESETS[i].slot[k].p);
    s->infx_preset = i;
    s->infx_kind = PRESETS[i].kind;
    s->infx_edited = 0;
    if (!chain_empty(s)) atomic_store(&s->fx.on, 1);
    knobs_free(s);
}

// The chain in use kept in its mode's place, and another mode's put in use.
static void keep_config(struct os *s) {
    struct fx_config *c = &s->fx_cfg[s->infx_mode];
    for (int i = 0; i < FX_SLOTS; i++) {
        c->type[i] = atomic_load(&s->fx.type[i]);
        for (int k = 0; k < 3; k++) c->p[i][k] = atomic_load(&s->fx.p[i][k]);
    }
    c->kind = s->infx_kind, c->preset = s->infx_preset, c->edited = s->infx_edited;
}

static void use_config(struct os *s, int mode) {
    const struct fx_config *c = &s->fx_cfg[mode];
    for (int i = 0; i < FX_SLOTS; i++) slot_set(s, i, c->type[i], c->p[i]);
    s->infx_kind = c->kind, s->infx_preset = c->preset, s->infx_edited = c->edited;
    s->infx_mode = mode;
    atomic_store(&s->fx.mode, mode);
}

static void set_mode(struct os *s, int mode) {
    if (mode == s->infx_mode) return;
    keep_config(s);
    use_config(s, mode);
    if (mode == FXM_PAD) s->infx_pad = s->selected % OS_PADS; // the pad selected now
    atomic_store(&s->fx.on, 1);
    knobs_free(s);
}

void fx_insert_hook(void *ctx, struct engine *e, int track, float *buf, int frames) {
    (void)e;
    fx_inserts_process(ctx, track, buf, frames);
}

void fx_engine_hook(void *ctx, struct engine *e, float *out, int frames) {
    float *buses[FX_SLOTS + 1];
    for (int i = 0; i <= FX_SLOTS; i++) buses[i] = e->fxbus[i];
    fx_chain_buses(ctx, buses, out, frames);
}

void fx_pages_init(struct os *s) {
    fx_chain_init(&s->fx);
    s->rec.fx = &s->fx;
    print_set_type(s, FX_ASR_HALL);
    s->infx_slot = G_AMP;
    for (int m = 0; m < FXM_COUNT; m++) { // each mode starts empty, on its own presets
        s->fx_cfg[m].kind = m == FXM_TRACK ? T_MIX : m == FXM_PAD ? T_KEYS : T_GUITAR;
        s->fx_cfg[m].preset = first_of_kind(s->fx_cfg[m].kind);
    }
    s->infx_kind = s->fx_cfg[FXM_INPUT].kind, s->infx_preset = s->fx_cfg[FXM_INPUT].preset;
    fx_inserts_init(&s->ins);
    if (s->engine) {
        s->engine->fx_hook = fx_engine_hook;
        s->engine->fx_ctx = &s->fx;
        s->engine->insert_hook = fx_insert_hook;
        s->engine->insert_ctx = &s->ins;
    }
}

// Power-on: Input Effects off and as new, whatever was used last or came with the autoload
// project: every Type's chain empty on its blank first preset, Type INPUT.
void infx_power_on(struct os *s) {
    for (int m = 0; m < FXM_COUNT; m++) {
        struct fx_config *c = &s->fx_cfg[m];
        memset(c->type, 0, sizeof c->type);
        memset(c->p, 0, sizeof c->p);
        c->kind = m == FXM_TRACK ? T_MIX : m == FXM_PAD ? T_KEYS : T_GUITAR;
        c->preset = first_of_kind(c->kind), c->edited = 0;
    }
    s->infx_mode = -1; // so INPUT is taken up afresh
    use_config(s, FXM_INPUT);
    atomic_store(&s->fx.on, 0);
}

enum { I_TYPE, I_ON, I_INPUT, I_GAIN, I_BRIGHT, I_CAT, I_PRESET, I_FX, I_1, I_2, I_3, I_FIELDS };

void infx_cursor_effect(struct os *s) {
    s->cursor_infx = I_FX;
}

void infx_open(struct os *s) {
    if (s->screen != OS_INFX && s->screen != OS_PRINT) s->infx_return = s->screen;
    s->screen = OS_INFX;
    s->cursor_infx = I_FX;
    knobs_free(s);
}

// The Input Effects input for the hardware's input: the mic, USB, or the line as chosen here.
static int infx_input_of(const struct os *s) {
    return s->input_src == INPUT_MIC ? FXIN_MIC : s->input_src == INPUT_USB ? FXIN_USB : s->input_src == INPUT_INST ? FXIN_INST
                                                                                                                    : s->infx_line;
}

// The Input select follows a change of LINE/MIC/USB made on the record pages. Also, each frame:
// the engine told whether the sends work and which pad the chain is on.
void infx_follow_input(struct os *s) {
    int in = atomic_load(&s->fx.input);
    int want = infx_input_of(s);
    if (in != want) atomic_store(&s->fx.input, want);
    atomic_store(&s->fx.inst, s->input_src == INPUT_INST); // the instrument stage, with its settings
    atomic_store(&s->fx.inst_gain, s->inst_gain);
    atomic_store(&s->fx.inst_bright, s->inst_bright);
    int on = atomic_load(&s->fx.on);
    if (s->engine) {
        atomic_store_explicit(&s->engine->sends_on, on && s->infx_mode == FXM_TRACK, memory_order_relaxed);
        atomic_store_explicit(&s->engine->fx_pad, s->infx_mode == FXM_PAD ? os_pad_slot(s, s->infx_pad) : -1,
                              memory_order_relaxed);
    }
}

static int read_ints(const char *p, int *v, int max) {
    int n = 0;
    while (n < max && *p) {
        char *end;
        long x = strtol(p, &end, 10);
        if (end == p) break;
        v[n++] = (int)x;
        if (*end != ',') break;
        p = end + 1;
    }
    return n;
}

#define CFG_INTS (3 + FX_SLOTS * 4)

void infx_save_line(const struct os *s, char *b, size_t n) {
    struct os *w = (struct os *)s;
    keep_config(w); // what is in use, into its place first
    int at = snprintf(b, n, "2,%d,%d,%d,%d", atomic_load(&w->fx.on), s->infx_mode, s->infx_line, s->infx_pad);
    for (int m = 0; m < FXM_COUNT; m++) {
        const struct fx_config *c = &s->fx_cfg[m];
        if (at > 0 && (size_t)at < n) at += snprintf(b + at, n - at, ",%d,%d,%d", c->kind, c->preset, c->edited);
        for (int i = 0; i < FX_SLOTS && at > 0 && (size_t)at < n; i++)
            at += snprintf(b + at, n - at, ",%d,%d,%d,%d", c->type[i], c->p[i][0], c->p[i][1], c->p[i][2]);
    }
}

static void read_config(struct fx_config *c, const int *v) {
    c->kind = clamp(v[0], 0, T_COUNT - 1);
    c->preset = clamp(v[1], 0, NPRESETS - 1);
    if (PRESETS[c->preset].kind != c->kind) c->preset = first_of_kind(c->kind);
    c->edited = v[2] != 0;
    for (int i = 0; i < FX_SLOTS; i++) {
        const int *q = v + 3 + i * 4;
        c->type[i] = q[0] > FX_OFF && q[0] < FX_COUNT ? q[0] : FX_OFF;
        for (int k = 0; k < 3; k++) c->p[i][k] = clamp(q[1 + k], 0, 127);
    }
}

void infx_load_line(struct os *s, const char *line) {
    int v[5 + FXM_COUNT * CFG_INTS], n = read_ints(line, v, (int)(sizeof v / sizeof v[0]));
    if (n == 5 + FX_SLOTS * 4) { // saved before the modes: one chain, the input's
        int old[CFG_INTS] = {v[1], v[2], v[3]};
        memcpy(old + 3, v + 5, sizeof(int) * FX_SLOTS * 4);
        read_config(&s->fx_cfg[FXM_INPUT], old);
        s->infx_line = clamp(v[4], FXIN_MONO_L, FXIN_STEREO);
        s->infx_mode = -1; // nothing in use yet: nothing to keep
        use_config(s, FXM_INPUT);
        atomic_store(&s->fx.on, v[0] != 0);
    } else if (n == 5 + FXM_COUNT * CFG_INTS && v[0] == 2) {
        for (int m = 0; m < FXM_COUNT; m++) read_config(&s->fx_cfg[m], v + 5 + m * CFG_INTS);
        s->infx_line = clamp(v[3], FXIN_MONO_L, FXIN_STEREO);
        s->infx_pad = clamp(v[4], 0, OS_PADS - 1);
        s->infx_mode = -1;
        use_config(s, clamp(v[2], 0, FXM_COUNT - 1));
        atomic_store(&s->fx.on, v[1] != 0);
    } else {
        return;
    }
    for (int i = 0; i < FX_SLOTS; i++) // a second reverb or delay where the rule holds
        if (blocked(s, i, atomic_load(&s->fx.type[i]))) slot_set(s, i, FX_OFF, 0);
    atomic_store(&s->fx.input, infx_input_of(s));
}

// The wah's pedal, from the mod wheel (any page) or the slider (on this page, with the cursor
// on the Pedal).
void infx_wah_pedal(struct os *s, int value) {
    for (int i = 0; i < FX_SLOTS; i++)
        if (atomic_load(&s->fx.type[i]) == FX_WAH) atomic_store(&s->fx.p[i][0], clamp(value, 0, 127));
    if (s->print_type == FX_WAH) s->print_p[0] = clamp(value, 0, 127);
}

// A pad pressed on this page in PAD: the chain moves to it.
void infx_pad_pressed(struct os *s, int slot) {
    if (s->screen == OS_INFX && s->infx_mode == FXM_PAD) s->infx_pad = slot % OS_PADS;
}

static const char *const INPUTS[FXIN_COUNT] = {"MONO L", "MONO R", "STEREO", "MIC", "USB", "INST L"};
// The order the Input field steps through them in (INST was added after the others were saved).
static const int INPUT_ORDER[FXIN_COUNT] = {FXIN_MONO_L, FXIN_MONO_R, FXIN_STEREO, FXIN_INST, FXIN_MIC, FXIN_USB};
static const char *const MODES[FXM_COUNT] = {"INPUT", "TRACK/MIXER", "PAD"};
static const char *const TITLES[FXM_COUNT] = {"Input Effects", "Track Effects", "Pad Effects"};

void infx_frame(struct os *s, struct ui *u, const struct input *in) {
    struct fx_chain *c = &s->fx;
    int sl = s->infx_slot = clamp(s->infx_slot, 0, FX_SLOTS - 1), edit = 0;
    if (input_down(in, IN_B1)) { // <Select>: the next effect (Shift: the one before)
        sl = s->infx_slot = (sl + (in->shift ? FX_SLOTS - 1 : 1)) % FX_SLOTS;
        if (s->cursor_infx < I_FX) s->cursor_infx = I_FX;
        knobs_free(s);
    }
    if (input_down(in, IN_B2)) { // <Clear>: this one
        slot_set(s, sl, FX_OFF, 0);
        edit = 1;
    }
    if (input_down(in, IN_B3)) { // <Bypass>: the whole chain
        atomic_store(&c->on, !atomic_load(&c->on));
        changed(s, u);
    }
    int type = atomic_load(&c->type[sl]), p[3];
    if (in->encoder_press) {
        for (int guard = 0; guard < I_FIELDS; guard++) {
            s->cursor_infx = (s->cursor_infx + (in->shift ? I_FIELDS - 1 : 1)) % I_FIELDS;
            if (s->cursor_infx == I_INPUT && s->infx_mode == FXM_TRACK) continue; // no input in Track/Mixer
            if ((s->cursor_infx == I_GAIN || s->cursor_infx == I_BRIGHT) && (s->infx_mode != FXM_INPUT || s->input_src != INPUT_INST))
                continue; // Gain and Bright: only for INST
            if (type == FX_OFF && s->cursor_infx > I_FX) continue; // no controls shown
            break;
        }
    }
    for (int k = 0; k < 3; k++) p[k] = atomic_load(&c->p[sl][k]);
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d) switch (s->cursor_infx) {
        case I_TYPE:
            set_mode(s, clamp(s->infx_mode + d, 0, FXM_COUNT - 1));
            changed(s, u);
            break;
        case I_ON: atomic_store(&c->on, d > 0); changed(s, u); break;
        case I_GAIN: s->inst_gain = clamp(s->inst_gain + d, 0, 30), s->fx_save_at = (s->frame_ms ? s->frame_ms : 1) + 1500; break;
        case I_BRIGHT:
            s->inst_bright = clamp(s->inst_bright + d * 5, 0, 100), s->fx_save_at = (s->frame_ms ? s->frame_ms : 1) + 1500;
            break;
        case I_INPUT:
            if (s->infx_mode == FXM_PAD) { // which pad
                s->infx_pad = clamp(s->infx_pad + d, 0, OS_PADS - 1);
            } else {
                int now = infx_input_of(s), at = 0;
                while (at < FXIN_COUNT - 1 && INPUT_ORDER[at] != now) at++;
                int next = INPUT_ORDER[clamp(at + d, 0, FXIN_COUNT - 1)];
                if (next == now) break;
                if (next < FXIN_MIC) s->infx_line = next;
                int src = next == FXIN_MIC ? INPUT_MIC : next == FXIN_USB ? INPUT_USB : next == FXIN_INST ? INPUT_INST : INPUT_LINE;
                if (src != s->input_src) { // the hardware's input, shared with the record pages
                    s->input_src = src;
                    if (s->set_input && s->set_input(src) != 0 && src == INPUT_USB) s->usb_missing = 1;
                }
                atomic_store(&c->input, next);
                atomic_store(&c->inst, s->input_src == INPUT_INST); // the instrument stage now, not a frame later
            }
            changed(s, u);
            break;
        case I_CAT: {
            int k = clamp(s->infx_kind + d, 0, T_COUNT - 1);
            if (k != s->infx_kind) infx_load_preset(s, first_of_kind(k)); // its EMPTY
            changed(s, u);
            break;
        }
        case I_PRESET: { // within the Category
            int i = s->infx_preset, step = d > 0 ? 1 : -1;
            for (int n = d > 0 ? d : -d; n > 0; n--) { // the next of the Category, wherever it is in the list
                int j = i + step;
                while (j >= 0 && j < NPRESETS && PRESETS[j].kind != s->infx_kind) j += step;
                if (j >= 0 && j < NPRESETS) i = j;
            }
            if (i != s->infx_preset || s->infx_edited) infx_load_preset(s, i);
            changed(s, u);
            break;
        }
        case I_FX: {
            // Steps over the reverbs while another slot has one, and the same for delays.
            int t = type, skipped = 0;
            for (int n = d > 0 ? d : -d; n > 0; n--) {
                int next = fx_step(t, d > 0 ? 1 : -1, -1);
                while (next != t && blocked(s, sl, next)) {
                    int after = fx_step(next, d > 0 ? 1 : -1, -1);
                    skipped = 1;
                    if (after == next) {
                        next = t;
                        break;
                    }
                    next = after;
                }
                t = next;
            }
            if (skipped) os_toast(s, u, "1 reverb, 1 delay max");
            if (t != type) {
                slot_set(s, sl, t, 0), knobs_free(s), edit = 1;
                if (t != FX_OFF) atomic_store(&c->on, 1);
            }
            break;
        }
        default: {
            int *v = &p[s->cursor_infx - I_1];
            *v = clamp(*v + d, 0, 127);
            atomic_store(&c->p[sl][s->cursor_infx - I_1], *v);
            edit = 1;
        }
        }
    if (type != FX_OFF) {
        int k = knob_params(s, in, p);
        if (k >= 0) {
            for (int j = 0; j < 3; j++) atomic_store(&c->p[sl][j], p[j]);
            s->cursor_infx = I_1 + k;
            edit = 1;
        }
    }
    // The slider is the wah's pedal while the cursor is on it.
    if (in->fader_moved && type == FX_WAH && s->cursor_infx == I_1) infx_wah_pedal(s, in->fader);
    if (edit) {
        s->infx_edited = 1;
        changed(s, u);
    }

    type = atomic_load(&c->type[sl]);
    for (int k = 0; k < 3; k++) p[k] = atomic_load(&c->p[sl][k]);
    char v[40];
    lcd_text(u, 0, KEY_ROW, "<Select>");
    lcd_text(u, 9, KEY_ROW, "<Clear>");
    lcd_text(u, 18, KEY_ROW, "<Bypass>");
    lcd_rule(u, TITLE_ROW, TITLES[s->infx_mode]);
    lcd_text(u, 0, 3, "Type:");
    draw_field(u, 5, 3, MODES[s->infx_mode], s->cursor_infx == I_TYPE);
    lcd_text(u, 18, 3, "FX:");
    draw_field(u, 21, 3, atomic_load(&c->on) ? "ON" : "OFF", s->cursor_infx == I_ON);
    if (s->infx_mode == FXM_PAD) {
        const struct os_pad *pad = &s->pads[s->infx_pad];
        lcd_text(u, 0, 4, "Pad:");
        snprintf(v, sizeof v, "%c%02d %.6s", 'A' + s->infx_pad / 16, s->infx_pad % 16 + 1, pad->s ? pad->name : "--");
        draw_field(u, 4, 4, v, s->cursor_infx == I_INPUT);
    } else if (s->infx_mode == FXM_TRACK) {
        // nothing: the send levels are on the FX Sends page (the mixer's Shift+B3), as help says
    } else {
        lcd_text(u, 0, 4, "Input:");
        draw_field(u, 6, 4, INPUTS[infx_input_of(s)], s->cursor_infx == I_INPUT);
    }
    float load = atomic_load_explicit(&c->load, memory_order_relaxed);
    snprintf(v, sizeof v, "CPU%3d%%", atomic_load(&c->on) ? (int)(load * 100 + 0.5f) : 0);
    if (s->infx_mode == FXM_INPUT && s->input_src == INPUT_INST) { // INST: its gain and Bright in place of the CPU
        snprintf(v, sizeof v, "+%ddB", s->inst_gain);
        draw_field(u, 13, 4, v, s->cursor_infx == I_GAIN);
        snprintf(v, sizeof v, "Brt%d", s->inst_bright);
        draw_field(u, 20, 4, v, s->cursor_infx == I_BRIGHT);
    } else if (s->infx_mode != FXM_TRACK) {
        lcd_text(u, 19, 4, v);
    }
    lcd_text(u, 0, 5, "Category:");
    draw_field(u, 9, 5, TYPE_NAMES[s->infx_kind], s->cursor_infx == I_CAT);
    lcd_text(u, 0, 6, "Preset:");
    snprintf(v, sizeof v, "%s%s", PRESETS[s->infx_preset].name, s->infx_edited ? "*" : "");
    draw_field(u, 7, 6, v, s->cursor_infx == I_PRESET);
    for (int i = 0; i < FX_SLOTS; i++) { // three columns of three; an empty place is just its number
        int t = atomic_load(&c->type[i]), col = i / 3 * 9, row = 7 + i % 3;
        if (t) snprintf(v, sizeof v, "%d %s", i + 1, fx_short(t));
        else snprintf(v, sizeof v, "%d", i + 1);
        if (i == sl) lcd_inverse(u, col, row, v);
        else lcd_text(u, col, row, v);
    }
    snprintf(v, sizeof v, "Effect %d:", sl + 1);
    lcd_text(u, 0, 10, v);
    draw_field(u, 9, 10, fx_name(type), s->cursor_infx == I_FX);
    for (int i = 0; i < 3; i++) {
        if (type == FX_OFF) break;
        if (type == FX_WAH && i == 0) snprintf(v, sizeof v, "K1/Slider Pedal:");
        else snprintf(v, sizeof v, "K%d %s:", i + 1, fx_param_name(type, i));
        lcd_text(u, 0, 11 + i, v);
        fx_param_text(type, i, p[i], tempo(s), v, sizeof v);
        draw_field(u, type == FX_WAH && i == 0 ? 16 : 15, 11 + i, v, s->cursor_infx == I_1 + i);
    }
    if (type == FX_OFF && s->cursor_infx > I_FX) s->cursor_infx = I_FX;
    if (!s->input_thru && s->infx_mode == FXM_INPUT) lcd_text(u, 0, BOTTOM_ROW, "Input Thru OFF: not heard");
    else lcd_rule(u, BOTTOM_ROW, 0);
    ui_wake(u, 250); // the CPU readout
}

// ---- The mixer's FX Sends page ----

void os_sends_push(struct os *s) {
    pthread_mutex_lock(&s->seq.lock);
    const struct seq_sequence *q = &s->seq.sequence[s->seq.current];
    for (int t = 0; t < SEQ_TRACKS; t++) engine_track_sends(s->engine, t + 1, q->track[t].send);
    pthread_mutex_unlock(&s->seq.lock);
}

void sends_open(struct os *s) {
    s->screen = OS_SENDS;
    s->fader_caught = 0;
}

void sends_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    if (input_down(in, IN_B3) && in->shift) { // SHIFT + B3 <Inserts>: the channel's insert effects
        inserts_open(s);
        ui_wake(u, 1);
        return;
    }
    if (input_down(in, IN_B3)) { // <Back>: the mixer
        s->screen = OS_MIXER;
        ui_wake(u, 1);
        return;
    }
    if (input_down(in, IN_B1)) s->sends_fx = (s->sends_fx + FX_SLOTS - 1) % FX_SLOTS, s->fader_caught = 0;
    if (input_down(in, IN_B2)) s->sends_fx = (s->sends_fx + 1) % FX_SLOTS, s->fader_caught = 0;
    pthread_mutex_lock(&sq->lock);
    if (s->mixer_track / 16 != s->bank) { // PAD BANK: the same place in the next sixteen
        s->mixer_track = s->bank * 16 + s->mixer_track % 16;
        if (s->mixer_track >= SEQ_TRACKS) s->mixer_track = SEQ_TRACKS - 1;
        s->fader_caught = 0;
    }
    int t = s->mixer_track, fx = s->sends_fx, changed_send = 0;
    struct seq_sequence *q = &sq->sequence[sq->current];
    unsigned char *send = q->track[t].send;
    if (in->fader_moved) { // the slider is the send, once it reaches it
        int last = s->fader_last, now = in->fader, at = send[fx] * 127 / 100;
        s->fader_last = now;
        if (!s->fader_caught) {
            int crossed = last >= 0 && ((last <= at && now >= at) || (last >= at && now <= at));
            if (crossed || abs(now - at) <= 2) s->fader_caught = 1;
        }
        if (s->fader_caught) send[fx] = (unsigned char)(now * 100 / 127), changed_send = 1;
    }
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d) send[fx] = (unsigned char)clamp(send[fx] + d, 0, 100), changed_send = 1;
    unsigned char mine[FX_SLOTS];
    memcpy(mine, send, sizeof mine);
    char line[LCD_COLS + 8], name[24];
    int type = atomic_load(&s->fx.type[fx]);
    lcd_text(u, 0, KEY_ROW, "<FX->");
    lcd_text(u, 9, KEY_ROW, "<FX+>");
    lcd_text(u, in->shift ? 17 : 20, KEY_ROW, in->shift ? "<Inserts>" : "<Back>");
    snprintf(line, sizeof line, "Trk:%c%02d %-.14s", 'A' + t / 16, t + 1, q->track[t].name);
    lcd_text(u, 0, 1, line);
    snprintf(name, sizeof name, "Send %d: %s", fx + 1, type ? fx_name(type) : "(empty)");
    lcd_rule(u, TITLE_ROW, name);
    for (int i = 0; i < 16; i++) { // the bank's sixteen sends to this effect, as bars
        int tr = s->bank * 16 + i;
        if (tr >= SEQ_TRACKS) break;
        int lvl = q->track[tr].send[fx], h = lvl * 120 / 100, x = i * 20 + 3;
        if (tr == t) {
            lcd_fill(u, x - 2, 48, 18, 1, LCD_FG), lcd_fill(u, x - 2, 172, 18, 1, LCD_FG);
            lcd_fill(u, x - 2, 48, 1, 125, LCD_FG), lcd_fill(u, x + 15, 48, 1, 125, LCD_FG);
        }
        if (h > 0) lcd_fill(u, x + 2, 170 - h, 10, h, LCD_FG);
        else lcd_fill(u, x + 2, 169, 10, 1, LCD_FG);
        char n[4];
        snprintf(n, sizeof n, "%02d", tr + 1);
        lcd_small_text(u, x + 1, 176, n, LCD_FG);
    }
    pthread_mutex_unlock(&sq->lock);
    if (changed_send) engine_track_sends(s->engine, t + 1, mine);
    snprintf(line, sizeof line, "Send:%3d (Slider, Wheel)", mine[fx]);
    lcd_text(u, 0, 12, line);
    char all[80];
    int pos = 0; // this track's nine sends, small
    for (int i = 0; i < FX_SLOTS && pos < (int)sizeof all - 8; i++) pos += snprintf(all + pos, sizeof all - pos, "%d:%d ", i + 1, mine[i]);
    lcd_small_text(u, 2, 13 * CELL_H + 4, all, LCD_FG);
    if (s->infx_mode != FXM_TRACK || !atomic_load(&s->fx.on))
        lcd_inverse(u, 0, BOTTOM_ROW, " Off: KNOB FX Type TRACK  ");
    else
        lcd_rule(u, BOTTOM_ROW, 0);
}

// ---- Chains on the card (Save/Load, FX CHAIN) ----

// The chain a file goes to or from: the Track/Mixer one, or the input one (PAD's while in use).
static int chain_mode(const struct os *s, int track) {
    if (track) return FXM_TRACK;
    return s->infx_mode == FXM_PAD ? FXM_PAD : FXM_INPUT;
}

int infx_chain_save(struct os *s, const char *path, int track) {
    keep_config(s);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    const struct fx_config *c = &s->fx_cfg[chain_mode(s, track)];
    fprintf(f, "mpc600s fx chain 1\n%d\n", c->kind);
    for (int i = 0; i < FX_SLOTS; i++) fprintf(f, "%d %d %d %d\n", c->type[i], c->p[i][0], c->p[i][1], c->p[i][2]);
    return fclose(f) == 0 ? 0 : -1;
}

// Into its chain, marked edited (it is no preset any more): the one in use straight away, or the
// other kept for when its mode is chosen.
int infx_chain_load(struct os *s, const char *path, int track) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char head[64];
    int kind, ok = fgets(head, sizeof head, f) && strncmp(head, "mpc600s fx chain", 16) == 0 && fscanf(f, "%d", &kind) == 1;
    int type[FX_SLOTS], p[FX_SLOTS][3];
    for (int i = 0; ok && i < FX_SLOTS; i++) ok = fscanf(f, "%d %d %d %d", &type[i], &p[i][0], &p[i][1], &p[i][2]) == 4;
    fclose(f);
    if (!ok) return -1;
    int mode = chain_mode(s, track);
    if (mode != s->infx_mode) { // not the one in use: into its keeping
        struct fx_config *c = &s->fx_cfg[mode];
        int reverb = 0, delay = 0;
        for (int i = 0; i < FX_SLOTS; i++) {
            int ty = type[i] > FX_OFF && type[i] < FX_COUNT ? type[i] : FX_OFF, cat = ty ? fx_cat_of(ty) : -1;
            if (mode != FXM_TRACK && ((cat == FXC_REVERB && reverb++) || (cat == FXC_DELAY && delay++))) ty = FX_OFF;
            c->type[i] = ty;
            for (int k = 0; k < 3; k++) c->p[i][k] = clamp(p[i][k], 0, 127);
        }
        c->kind = clamp(kind, 0, T_COUNT - 1), c->preset = first_of_kind(c->kind), c->edited = 1;
        return 0;
    }
    for (int i = 0; i < FX_SLOTS; i++) {
        int pp[3] = {clamp(p[i][0], 0, 127), clamp(p[i][1], 0, 127), clamp(p[i][2], 0, 127)};
        slot_set(s, i, type[i] > FX_OFF && type[i] < FX_COUNT ? type[i] : FX_OFF, pp);
    }
    for (int i = 0; i < FX_SLOTS; i++) // the rule where it holds
        if (blocked(s, i, atomic_load(&s->fx.type[i]))) slot_set(s, i, FX_OFF, 0);
    s->infx_kind = clamp(kind, 0, T_COUNT - 1);
    s->infx_preset = first_of_kind(s->infx_kind);
    s->infx_edited = 1;
    atomic_store(&s->fx.on, 1);
    keep_config(s);
    return 0;
}

// ---- Insert effects ----
//
// The selected channel's inserts (SHIFT + B3 on FX Sends): its sound through them in series,
// before its fader, pan and sends. Eight insert slots shared by every track: each effect uses as
// many as it costs (1 light, 2 medium, 4 a reverb), and one that will not fit is not taken.

enum { INS_SEL, INS_CAT, INS_FX, INS_P1, INS_P2, INS_P3, INS_FIELDS };

static int clampi_ins(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

int inserts_used(const struct os *s) {
    int n = 0;
    for (int i = 0; i < FX_INSERTS; i++)
        if (atomic_load(&((struct os *)s)->ins.track[i])) n += fx_insert_weight(atomic_load(&((struct os *)s)->ins.type[i]));
    return n;
}

// The track's inserts in chain order: their slots in the pool, how many.
static int chain_of(const struct os *s, int track, int *out) {
    int n = 0;
    struct fx_inserts *ins = (struct fx_inserts *)&s->ins;
    for (int pos = 0; pos < FX_INSERTS; pos++)
        for (int i = 0; i < FX_INSERTS; i++)
            if (atomic_load(&ins->track[i]) == track + 1 && atomic_load(&ins->pos[i]) == pos) out[n++] = i;
    return n;
}

void inserts_push(struct os *s) {
    int tracks[ENGINE_INSERTS] = {0}, n = 0;
    for (int i = 0; i < FX_INSERTS; i++) {
        int t = atomic_load(&s->ins.track[i]), seen = 0;
        for (int k = 0; k < n; k++) seen |= tracks[k] == t;
        if (t && !seen && n < ENGINE_INSERTS) tracks[n++] = t;
    }
    engine_set_inserts(s->engine, tracks);
}

void inserts_open(struct os *s) {
    s->screen = OS_INSERT;
    s->ins_cursor = INS_FX, s->ins_sel = 0;
}

static void set_type(struct os *s, int slot, int type) {
    atomic_store(&s->ins.type[slot], type);
    for (int k = 0; k < 3; k++) atomic_store(&s->ins.p[slot][k], fx_param_default(type, k));
}

// An effect for this insert, from `from` on in direction d (within cat, or all): the first that
// fits in the slots left. Its message if none does.
static int fitting(struct os *s, int slot, int from, int d, int cat, struct ui *u) {
    int free_slots = FX_INSERT_SLOTS - inserts_used(s) + fx_insert_weight(atomic_load(&s->ins.type[slot]));
    int t = from, tried = 0;
    while (tried++ < FX_COUNT) {
        if (t > FX_OFF && fx_insert_weight(t) <= free_slots) return t;
        int next = fx_step(t, d, cat);
        if (next == t) break;
        t = next;
    }
    char msg[48];
    snprintf(msg, sizeof msg, "%s needs %d slots: %d left", fx_name(from), fx_insert_weight(from), free_slots);
    os_toast(s, u, msg);
    return -1;
}

static void ins_add(struct os *s, struct ui *u, int track) {
    int chain[FX_INSERTS], n = chain_of(s, track, chain), slot = -1;
    for (int i = 0; i < FX_INSERTS && slot < 0; i++)
        if (!atomic_load(&s->ins.track[i])) slot = i;
    if (slot < 0 || inserts_used(s) >= FX_INSERT_SLOTS) {
        os_toast(s, u, "All 8 insert slots in use");
        return;
    }
    // A light one to start (an EQ): one slot, changed with the wheel.
    atomic_store(&s->ins.type[slot], FX_OFF);
    int t = fitting(s, slot, fx_cat_first(FXC_EQ), 1, -1, u);
    if (t < 0) return;
    set_type(s, slot, t);
    atomic_store(&s->ins.pos[slot], n);
    atomic_store(&s->ins.track[slot], track + 1);
    s->ins_sel = n, s->ins_cursor = INS_FX;
    inserts_push(s);
}

static void ins_remove(struct os *s, int track) {
    int chain[FX_INSERTS], n = chain_of(s, track, chain);
    if (!n) return;
    int at = clampi_ins(s->ins_sel, 0, n - 1);
    atomic_store(&s->ins.track[chain[at]], 0);
    atomic_store(&s->ins.type[chain[at]], FX_OFF);
    for (int k = at + 1; k < n; k++) atomic_store(&s->ins.pos[chain[k]], k - 1); // the rest close up
    if (s->ins_sel >= n - 1) s->ins_sel = n - 2 < 0 ? 0 : n - 2;
    inserts_push(s);
}

void inserts_frame(struct os *s, struct ui *u, const struct input *in) {
    if (input_down(in, IN_B1)) { // <Back>: FX Sends
        s->screen = OS_SENDS;
        ui_wake(u, 1);
        return;
    }
    int t = s->mixer_track;
    if (t / 16 != s->bank) t = s->mixer_track = s->bank * 16 + t % 16 < SEQ_TRACKS ? s->bank * 16 + t % 16 : SEQ_TRACKS - 1;
    int chain[FX_INSERTS], n = chain_of(s, t, chain);
    if (input_down(in, IN_B3)) ins_add(s, u, t); // <Add>: at the end of this channel's chain
    if (input_down(in, IN_B2) && n) ins_remove(s, t); // <Remove> the one shown
    n = chain_of(s, t, chain);
    if (s->ins_sel >= n) s->ins_sel = n ? n - 1 : 0;
    int slot = n ? chain[s->ins_sel] : -1, type = slot >= 0 ? atomic_load(&s->ins.type[slot]) : FX_OFF;
    if (in->encoder_press) s->ins_cursor = (s->ins_cursor + (in->shift ? INS_FIELDS - 1 : 1)) % INS_FIELDS;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d && slot >= 0) {
        switch (s->ins_cursor) {
        case INS_SEL: s->ins_sel = clampi_ins(s->ins_sel + d, 0, n - 1); break;
        case INS_CAT: {
            int cat = clampi_ins(fx_cat_of(type) + d, 0, FX_CATS - 1);
            if (cat != fx_cat_of(type)) {
                int to = fitting(s, slot, fx_cat_first(cat), 1, cat, u);
                if (to > 0) set_type(s, slot, to);
            }
            break;
        }
        case INS_FX: {
            int next = fx_step(type, d, -1);
            if (next > FX_OFF && next != type) {
                int to = fitting(s, slot, next, d > 0 ? 1 : -1, -1, u);
                if (to > 0) set_type(s, slot, to);
            }
            break;
        }
        default: {
            int k = s->ins_cursor - INS_P1;
            atomic_store(&s->ins.p[slot][k], clampi_ins(atomic_load(&s->ins.p[slot][k]) + d, 0, 127));
            break;
        }
        }
    }
    for (int k = 0; k < 3 && slot >= 0; k++) // K1-K3: the three controls
        if (in->knob_moved[k]) atomic_store(&s->ins.p[slot][k], in->knob[k]), s->ins_cursor = INS_P1 + k;
    type = slot >= 0 ? atomic_load(&s->ins.type[slot]) : FX_OFF;

    char line[LCD_COLS + 16], v[32];
    lcd_text(u, 0, KEY_ROW, "<Back>");
    if (n) lcd_text(u, 8, KEY_ROW, "<Remove>");
    lcd_text(u, 21, KEY_ROW, "<Add>");
    snprintf(line, sizeof line, "Trk:%c%02d %-.14s", 'A' + t / 16, t + 1, s->seq.sequence[s->seq.current].track[t].name);
    lcd_text(u, 0, 1, line);
    lcd_rule(u, TITLE_ROW, "Insert Effects");
    int used = inserts_used(s), cpu = (int)(atomic_load(&s->engine->load) * 100.0f + 0.5f);
    snprintf(line, sizeof line, "Slots: %d/%d used  CPU %d%%", used, FX_INSERT_SLOTS, cpu);
    lcd_text(u, 0, 3, line);
    int pos = 0; // the chain: 1 PLATE > 2 EQ3
    line[0] = 0;
    for (int i = 0; i < n && pos < LCD_COLS; i++)
        pos += snprintf(line + pos, sizeof line - (size_t)pos, "%s%d %s", i ? ">" : "", i + 1, fx_short(atomic_load(&s->ins.type[chain[i]])));
    lcd_text(u, 0, 4, n ? line : "No inserts: B3 <Add>");
    lcd_rule(u, 5, 0);
    if (slot >= 0) {
        const char *labels[INS_FIELDS] = {"Insert:", "Category:", "Effect:", "", "", ""};
        for (int f = 0; f < INS_FIELDS; f++) {
            int row = 6 + f + (f >= INS_P1);
            char lab[24];
            if (f >= INS_P1) snprintf(lab, sizeof lab, "K%d %s:", f - INS_P1 + 1, fx_param_name(type, f - INS_P1));
            else snprintf(lab, sizeof lab, "%s", labels[f]);
            if (f == INS_SEL) snprintf(v, sizeof v, " %d of %d ", s->ins_sel + 1, n);
            else if (f == INS_CAT) snprintf(v, sizeof v, " %s ", fx_cat_name(fx_cat_of(type)));
            else if (f == INS_FX) snprintf(v, sizeof v, " %s (%d) ", fx_name(type), fx_insert_weight(type));
            else {
                char pv[16];
                fx_param_text(type, f - INS_P1, atomic_load(&s->ins.p[slot][f - INS_P1]), atomic_load(&s->ins.bpm10) / 10.0, pv, sizeof pv);
                snprintf(v, sizeof v, " %s ", pv);
            }
            int col = f >= INS_P1 ? 14 : (int)strlen(lab);
            if (col < (int)strlen(lab)) col = (int)strlen(lab);
            lcd_text(u, 0, row, lab);
            if (col + (int)strlen(v) > LCD_COLS) v[LCD_COLS - col] = 0;
            if (f == s->ins_cursor) lcd_inverse(u, col, row, v);
            else lcd_text(u, col, row, v);
        }
        lcd_rule(u, 9, 0);
        int w = fx_insert_weight(type); // what this one costs, and what is left
        snprintf(line, sizeof line, "Uses %d slot%s: %d left", w, w == 1 ? "" : "s", FX_INSERT_SLOTS - used);
        lcd_text(u, 0, 13, line);
    } else {
        lcd_text(u, 0, 7, "8 insert slots, shared by");
        lcd_text(u, 0, 8, "every track. Each effect");
        lcd_text(u, 0, 9, "uses 1 (light), 2 (medium)");
        lcd_text(u, 0, 10, "or 4 (reverb) of them.");
        lcd_text(u, 0, 12, "Tip: use reverb as a send");
    }
    lcd_rule(u, BOTTOM_ROW, 0);
    ui_wake(u, 250); // the CPU figure moves
}

// Saved with the project: track,pos,type,p1,p2,p3 for each insert, ';' between.
void inserts_save_line(const struct os *s, char *b, size_t n) {
    size_t pos = 0;
    b[0] = 0;
    struct fx_inserts *ins = (struct fx_inserts *)&s->ins;
    for (int i = 0; i < FX_INSERTS && pos < n; i++) {
        int t = atomic_load(&ins->track[i]);
        if (!t) continue;
        pos += (size_t)snprintf(b + pos, n - pos, "%d,%d,%d,%d,%d,%d;", t, atomic_load(&ins->pos[i]), atomic_load(&ins->type[i]),
                                atomic_load(&ins->p[i][0]), atomic_load(&ins->p[i][1]), atomic_load(&ins->p[i][2]));
    }
}

void inserts_load_line(struct os *s, const char *line) {
    for (int i = 0; i < FX_INSERTS; i++) atomic_store(&s->ins.track[i], 0), atomic_store(&s->ins.type[i], FX_OFF);
    int i = 0, used = 0;
    const char *p = line;
    while (p && *p && i < FX_INSERTS) {
        int t, pos, type, a, b2, c;
        if (sscanf(p, "%d,%d,%d,%d,%d,%d", &t, &pos, &type, &a, &b2, &c) == 6 && t > 0 && t <= SEQ_TRACKS && type > FX_OFF &&
            type < FX_COUNT && used + fx_insert_weight(type) <= FX_INSERT_SLOTS) {
            atomic_store(&s->ins.track[i], t), atomic_store(&s->ins.pos[i], clampi_ins(pos, 0, FX_INSERTS - 1));
            atomic_store(&s->ins.type[i], type);
            atomic_store(&s->ins.p[i][0], clampi_ins(a, 0, 127)), atomic_store(&s->ins.p[i][1], clampi_ins(b2, 0, 127));
            atomic_store(&s->ins.p[i][2], clampi_ins(c, 0, 127));
            used += fx_insert_weight(type), i++;
        }
        p = strchr(p, ';');
        if (p) p++;
    }
    inserts_push(s);
}
