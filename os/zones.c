// Zone Assign (CHOP): a sound divided into zones, each then put on a pad of its own, all
// sharing the one sound. Two ways to divide it:
//   PAD: <Do It>, then a pad starts the sound playing through; each pad hit after that marks
//        where the next zone starts, snapped to the nearest transient unless that is off.
//   REGIONS: 1 to 32 equal zones, whose starts can then be moved.
// Then the zones go, in order, to a new empty program or into empty pad banks, one-shot,
// with a mute group: by default the first one the destination does not use.
#include "lcd.h"
#include "onset.h"
#include "os.h"
#include "seq.h"
#include <stdio.h>
#include <string.h>

#define ZONE_PAD_VOICE (-4) // the engine pad the sound plays through on
#define WAVE_TOP 64
#define WAVE_H 88
#define SNAP_MS 80          // a hit snaps to a transient this close
#define MIN_ZONE_MS 20      // zones closer than this are one

enum { Z_MODE, Z_SENS, Z_REGIONS, Z_ZONE, Z_START, Z_DEST, Z_MGRP };

void zones_defaults(struct os *s) {
    s->zone_mode = ZONE_PAD;
    s->zone_regions = 8;
    s->zone_sens = 5;
}

static const struct sample *sound(const struct os *s) {
    return s->pads[s->selected].s;
}

static void even_regions(struct os *s) {
    const struct sample *sm = sound(s);
    s->zone_count = s->zone_regions;
    for (int i = 0; i < s->zone_count; i++) s->zone_start[i] = sm ? (int32_t)((int64_t)i * sm->frames / s->zone_count) : 0;
    if (s->zone_cur >= s->zone_count) s->zone_cur = 0;
}

static void find_onsets(struct os *s) {
    const struct sample *sm = sound(s);
    s->zone_nonsets = 0;
    if (!sm || s->zone_sens <= 0) return;
    // Sensitivity 10 needs the smallest rise to count, 1 the largest.
    float rise = 1.1f - 0.09f * (float)s->zone_sens; // 5: about the usual 0.5
    s->zone_nonsets = onset_detect_rise(sm, s->zone_onsets, 256, rise);
}

void zones_open(struct os *s) {
    if (s->screen != OS_ZONES) s->zone_return = s->screen;
    s->screen = OS_ZONES;
    s->cursor_zones = 0;
    s->zone_phase = ZP_SETUP;
    find_onsets(s);
    if (s->zone_mode == ZONE_REGIONS) even_regions(s);
    else s->zone_count = 0;
}

// The fields the cursor moves over, for the current mode and phase.
static int fields(const struct os *s, int *out) {
    int n = 0;
    if (s->zone_phase == ZP_ASSIGN) {
        out[n++] = Z_DEST;
        out[n++] = Z_MGRP;
        return n;
    }
    out[n++] = Z_MODE;
    if (s->zone_mode == ZONE_PAD) {
        out[n++] = Z_SENS;
    } else {
        out[n++] = Z_REGIONS;
        out[n++] = Z_ZONE;
        out[n++] = Z_START;
    }
    return n;
}

static int bank_empty(const struct os *s, int bank) {
    for (int i = 0; i < 16; i++)
        if (s->pads[bank * 16 + i].s) return 0;
    return 1;
}

// The first mute group the destination's pads do not use, or 0 (OFF) when all are.
static int free_mute_group(const struct os *s, int dest) {
    if (dest == 0) return 1; // a new, empty program
    int used[17] = {0};
    for (int i = 0; i < OS_PADS; i++)
        if (s->pads[i].s && s->pads[i].p.mute_group > 0 && s->pads[i].p.mute_group <= 16) used[s->pads[i].p.mute_group] = 1;
    for (int g = 1; g <= 16; g++)
        if (!used[g]) return g;
    return 0;
}

static void start_assign(struct os *s) {
    s->zone_phase = ZP_ASSIGN;
    s->zone_dest = 0;
    s->zone_mgrp = free_mute_group(s, 0);
    s->cursor_zones = 0;
    engine_note_off(s->engine, ZONE_PAD_VOICE);
}

static void change(struct os *s, int f, int d) {
    const struct sample *sm = sound(s);
    switch (f) {
    case Z_MODE:
        s->zone_mode = d > 0 ? ZONE_REGIONS : ZONE_PAD;
        if (s->zone_mode == ZONE_REGIONS) even_regions(s);
        else s->zone_count = 0;
        break;
    case Z_SENS:
        s->zone_sens = s->zone_sens + d < 0 ? 0 : s->zone_sens + d > 10 ? 10 : s->zone_sens + d;
        find_onsets(s);
        break;
    case Z_REGIONS:
        s->zone_regions = s->zone_regions + d < 1 ? 1 : s->zone_regions + d > 32 ? 32 : s->zone_regions + d;
        even_regions(s);
        break;
    case Z_ZONE: s->zone_cur = (s->zone_cur + d % s->zone_count + s->zone_count) % s->zone_count; break;
    case Z_START: {
        if (!sm || s->zone_cur == 0) break; // the first zone always starts at the start
        int step = sm->frames / 304 > 0 ? sm->frames / 304 : 1;
        int32_t v = s->zone_start[s->zone_cur] + d * step;
        int32_t lo = s->zone_start[s->zone_cur - 1] + 1;
        int32_t hi = s->zone_cur + 1 < s->zone_count ? s->zone_start[s->zone_cur + 1] - 1 : sm->frames - 1;
        s->zone_start[s->zone_cur] = v < lo ? lo : v > hi ? hi : v;
        break;
    }
    case Z_DEST: { // a new program, or one of the banks with nothing on it
        int dest = s->zone_dest;
        for (int tries = 0; tries < OS_BANKS + 1; tries++) {
            dest = (dest + (d > 0 ? 1 : OS_BANKS)) % (OS_BANKS + 1);
            if (dest == 0 || bank_empty(s, dest - 1)) break;
        }
        s->zone_dest = dest;
        s->zone_mgrp = free_mute_group(s, dest);
        break;
    }
    case Z_MGRP: s->zone_mgrp = s->zone_mgrp + d < 0 ? 0 : s->zone_mgrp + d > 16 ? 16 : s->zone_mgrp + d; break;
    }
}

// The zones, in order, onto the destination: each a pad of its own on the one sound.
static void assign(struct os *s, struct ui *u) {
    struct sample *sm = s->pads[s->selected].s;
    if (!sm || s->zone_count < 1) return;
    char base[40], name[40], msg[40];
    snprintf(base, sizeof base, "%s", s->pads[s->selected].name);
    os_undo_save(s, "Zone Assign");
    int first, room;
    if (s->zone_dest == 0) {
        int to = -1;
        for (int p = 0; p < OS_PROGRAMS && to < 0; p++)
            if (!os_program_sounds(s, p)) to = p;
        if (to < 0) {
            os_toast(s, u, "No empty program");
            return;
        }
        sm->refs++; // held while the program changes under it
        pthread_mutex_lock(&s->seq.lock);
        s->seq.sequence[s->seq.current].track[s->seq.track].kit = to;
        pthread_mutex_unlock(&s->seq.lock);
        os_set_program(s, to);
        first = 0, room = OS_PADS;
        snprintf(msg, sizeof msg, "%d zones: Program %02d", s->zone_count, to + 1);
    } else {
        sm->refs++;
        first = (s->zone_dest - 1) * 16, room = 0;
        for (int b = s->zone_dest - 1; b < OS_BANKS && bank_empty(s, b); b++) room += 16; // on into empty banks
        snprintf(msg, sizeof msg, "%d zones: Bank %c", s->zone_count < room ? s->zone_count : room, 'A' + s->zone_dest - 1);
    }
    int n = s->zone_count < room ? s->zone_count : room;
    for (int i = 0; i < n; i++) {
        int slot = first + i;
        snprintf(name, sizeof name, "%.30s-%02d", base, i + 1);
        os_pad_load(s, slot, sm, name);
        struct pad_params *p = &s->pads[slot].p;
        p->start = s->zone_start[i];
        p->end = i + 1 < s->zone_count ? s->zone_start[i + 1] : sm->frames;
        p->loop = p->start;
        p->gate = 0; // Note On off: a zone plays through, however short the hit
        p->mute_group = s->zone_mgrp;
        os_pad_push(s, slot);
    }
    if (--sm->refs <= 0) engine_retire(s->engine, sm); // the hold above
    if (n < s->zone_count) snprintf(msg, sizeof msg, "Only %d of %d zones fit", n, s->zone_count);
    os_toast(s, u, msg);
    s->selected = first;
    s->zone_phase = ZP_SETUP;
    s->zone_count = 0;
    if (s->zone_mode == ZONE_REGIONS) even_regions(s);
}

// Where the sound playing through is now, in its frames: the time since it started, less
// the delay before it is heard, so a hit lands where the sound was when it was heard.
static int32_t play_position(const struct os *s) {
    const struct sample *sm = sound(s);
    long long heard = atomic_load(&s->engine->clock) - s->zone_clock0 - (s->rec.latency > 0 ? s->rec.latency : 424);
    if (heard < 0) heard = 0;
    return (int32_t)(heard * sm->rate / ENGINE_RATE);
}

int zones_pad(struct os *s, struct ui *u, int slot) {
    (void)u, (void)slot; // which pad does not matter: zones go out in order
    if (s->screen != OS_ZONES) return 0;
    const struct sample *sm = sound(s);
    if (s->zone_phase == ZP_WAIT && sm) { // the first hit starts the sound, and the first zone
        struct pad_params p;
        memset(&p, 0, sizeof p);
        p.end = sm->frames;
        p.volume = 100;
        p.cutoff = 100;
        p.mono = 1;
        engine_note_on(s->engine, ZONE_PAD_VOICE, 127, sm, &p);
        s->zone_clock0 = atomic_load(&s->engine->clock);
        s->zone_start[0] = 0;
        s->zone_count = 1;
        s->zone_phase = ZP_PLAY;
        return 1;
    }
    if (s->zone_phase == ZP_PLAY && sm) { // each later hit: a new zone, snapped to a transient
        int32_t at = play_position(s), best = -1;
        int32_t snap = SNAP_MS * sm->rate / 1000;
        for (int i = 1; i < s->zone_nonsets; i++) {
            int32_t o = s->zone_onsets[i], dist = o > at ? o - at : at - o;
            if (dist <= snap && (best < 0 || dist < (best > at ? best - at : at - best))) best = o;
        }
        if (best >= 0) at = best;
        int32_t last = s->zone_start[s->zone_count - 1];
        if (at - last >= MIN_ZONE_MS * sm->rate / 1000 && at < sm->frames && s->zone_count < ZONES_MAX)
            s->zone_start[s->zone_count++] = at;
        return 1;
    }
    if (s->zone_phase == ZP_ASSIGN) return 1; // the prompt is up: pads do nothing
    return 0; // setting up: a pad picks the sound, as anywhere
}

static void value(const struct os *s, int f, char *b, size_t n) {
    const struct sample *sm = sound(s);
    switch (f) {
    case Z_MODE: snprintf(b, n, " %s ", s->zone_mode == ZONE_PAD ? "PAD" : "REGIONS"); break;
    case Z_SENS:
        if (s->zone_sens <= 0) snprintf(b, n, " OFF ");
        else snprintf(b, n, " %d ", s->zone_sens);
        break;
    case Z_REGIONS: snprintf(b, n, " %d ", s->zone_regions); break;
    case Z_ZONE: snprintf(b, n, " %d ", s->zone_cur + 1); break;
    case Z_START: {
        long units = sm && sm->rate > 0 ? (long)((int64_t)s->zone_start[s->zone_cur] * 100000 / sm->rate) : 0;
        snprintf(b, n, " %03ld.%03ld.%02ld ", units / 100000 % 1000, units / 100 % 1000, units % 100);
        break;
    }
    case Z_DEST:
        if (s->zone_dest == 0) snprintf(b, n, " NEW PROGRAM ");
        else snprintf(b, n, " BANK %c ", 'A' + s->zone_dest - 1);
        break;
    case Z_MGRP:
        if (s->zone_mgrp == 0) snprintf(b, n, " OFF ");
        else snprintf(b, n, " %d ", s->zone_mgrp);
        break;
    }
}

static const char *label(int f) {
    static const char *const l[] = {"Mode:", "Sensitivity:", "Regions:", "Zone:", "Start:", "Assign to:", "Mute Grp:"};
    return l[f];
}

static void draw_wave(struct ui *u, const struct os *s) {
    const struct os_pad *pad = &s->pads[s->selected];
    const struct sample *sm = pad->s;
    if (!sm) {
        lcd_center(u, 6, "Press pad to select sample");
        return;
    }
    int mid = WAVE_TOP + WAVE_H / 2, scale = 2; // the pad's overview is drawn at half height
    for (int x = 0; x < OS_WAVE_W; x += 2)
        for (int y = mid - pad->hi[x] / scale & ~1; y <= mid - pad->lo[x] / scale; y += 2) lcd_dot(u, 8 + x, y, LCD_FG);
#define XPOS(f) (8 + (int)((int64_t)(f) * OS_WAVE_W / sm->frames) & ~1)
    for (int i = 0; i < s->zone_count; i++) { // zone starts: dashed, the selected one solid
        int x = XPOS(s->zone_start[i]), solid = s->zone_mode == ZONE_REGIONS && i == s->zone_cur;
        for (int y = WAVE_TOP; y < WAVE_TOP + WAVE_H; y += 2)
            if (solid || (y / 2) % 3 != 2) lcd_dot(u, x, y, LCD_FG);
    }
    if (s->zone_phase == ZP_PLAY) { // the play position, as a bar above the waveform
        int x = XPOS(play_position(s));
        for (int y = WAVE_TOP - 6; y < WAVE_TOP; y += 2) lcd_dot(u, x, y, LCD_FG);
    }
    for (int i = 1; i < s->zone_nonsets; i++) // transients: a tick under the waveform
        lcd_dot(u, XPOS(s->zone_onsets[i]), WAVE_TOP + WAVE_H + 2, LCD_FG);
#undef XPOS
}

void zones_frame(struct os *s, struct ui *u, const struct input *in) {
    const struct sample *sm = sound(s);
    int list[8], n;
    if (s->zone_phase == ZP_PLAY) {
        long long played = atomic_load(&s->engine->clock) - s->zone_clock0;
        if (!sm || played * sm->rate / ENGINE_RATE >= sm->frames + (s->rec.latency > 0 ? s->rec.latency : 424) ||
            input_down(in, IN_B1))
            start_assign(s); // played through, or <Stop>
        ui_wake(u, 30);
    } else if (s->zone_phase == ZP_WAIT) {
        if (input_down(in, IN_B1)) s->zone_phase = ZP_SETUP;
    } else if (s->zone_phase == ZP_ASSIGN) {
        if (input_down(in, IN_B1)) { // <Cancel>
            s->zone_phase = ZP_SETUP;
            if (s->zone_mode == ZONE_PAD) s->zone_count = 0;
        } else if (input_down(in, IN_B3)) {
            assign(s, u);
        }
    } else { // setting up
        static const struct sample *seen;
        if (sm != seen) { // another sound picked with a pad
            seen = sm;
            find_onsets(s);
            if (s->zone_mode == ZONE_REGIONS) even_regions(s);
        }
        if (input_down(in, IN_B3) && sm) {
            if (s->zone_mode == ZONE_PAD) s->zone_phase = ZP_WAIT;
            else start_assign(s);
        }
    }
    n = fields(s, list);
    if (s->cursor_zones >= n) s->cursor_zones = 0;
    int editing = s->zone_phase == ZP_SETUP || s->zone_phase == ZP_ASSIGN;
    if (editing) {
        if (in->encoder_press) s->cursor_zones = (s->cursor_zones + (in->shift ? n - 1 : 1)) % n;
        int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
        if (d && (sm || s->zone_phase == ZP_ASSIGN)) change(s, list[s->cursor_zones], d);
    }

    char line[LCD_COLS + 8];
    if (s->zone_phase == ZP_ASSIGN) {
        lcd_text(u, 0, KEY_ROW, "<Cancel>");
        lcd_text(u, 19, KEY_ROW, "<Do It>");
    } else if (s->zone_phase == ZP_PLAY) {
        lcd_text(u, 0, KEY_ROW, "<Stop>");
    } else if (s->zone_phase == ZP_WAIT) {
        lcd_text(u, 0, KEY_ROW, "<Cancel>");
    } else {
        lcd_text(u, 19, KEY_ROW, "<Do It>");
    }
    lcd_rule(u, TITLE_ROW, "Zone Assign");
    snprintf(line, sizeof line, "Pad:%c%02d %.17s", 'A' + s->selected / 16, s->selected % 16 + 1,
             sm ? s->pads[s->selected].name : "(empty)");
    lcd_text(u, 0, 3, line);
    draw_wave(u, s);
    snprintf(line, sizeof line, "%d zones", s->zone_count);
    lcd_rule(u, 10, s->zone_count ? line : 0);
    for (int i = 0; i < n; i++) { // one a row, but Zone: and Start: share the third
        char b[LCD_COLS + 1];
        int row = 11 + (list[i] == Z_START ? i - 1 : i), x = list[i] == Z_START ? 8 : 0;
        int col = x + (int)strlen(label(list[i]));
        lcd_text(u, x, row, label(list[i]));
        value(s, list[i], b, sizeof b);
        if (editing && i == s->cursor_zones) lcd_inverse(u, col, row, b);
        else lcd_text(u, col, row, b);
    }
    if (s->zone_phase == ZP_WAIT) {
        lcd_inverse(u, 0, 5, " Press a pad to start the ");
        lcd_inverse(u, 0, 6, " sound; pads mark zones   ");
    }
    if (s->zone_phase == ZP_PLAY) ui_wake(u, 30);
    lcd_rule(u, BOTTOM_ROW, 0);
}
