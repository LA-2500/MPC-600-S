// MUTE: the Track Mute page, after the MPC2000's. The 16 pads are laid out as a 4 x 4 grid of
// what they control: tracks (TRACK MUTE, TRACK SOLO, a bank of sixteen at a time), the active
// track's program's pads (PAD MUTE, PAD SOLO), or sequences (SEQUENCE: a pad queues the next).
// Next Seq: is on every type, set with the wheel.
#include "lcd.h"
#include "os.h"
#include "seq_screen.h"
#include <stdio.h>
#include <string.h>

static const char *const TYPES[MUTE_TYPES] = {"TRACK MUTE", "TRACK SOLO", "PAD MUTE", "PAD SOLO", "SEQUENCE"};
enum { M_TYPE, M_NEXT, M_FIELDS };

#define GRID_Y 72 // where the grid starts, in screen pixels: under Type: and Next Seq:
#define CELL_PX_W 80
#define CELL_PX_H 42

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void mute_open(struct os *s) {
    if (s->screen == OS_MUTE) {
        s->cursor_mute = 0; // pressed again: the first field
        return;
    }
    s->mute_return = s->screen;
    s->screen = OS_MUTE;
    s->cursor_mute = 0;
}

// The sequence that plays next: the queued one, or the current one when none is queued.
// Stopped, choosing one makes it the current one. Caller holds the lock.
static void queue_sequence(struct os *s, int target) {
    struct seq *sq = &s->seq;
    target = clampi(target, 0, SEQ_COUNT - 1);
    if (sq->mode == SEQ_STOPPED) sq->current = target, sq->pos = 0, sq->next = -1;
    else sq->next = target == sq->current ? -1 : target;
}

// A pad on this page: what it controls changes, and it does not play.
void mute_pad(struct os *s, struct ui *u, int i) {
    struct seq *sq = &s->seq;
    char msg[32];
    pthread_mutex_lock(&sq->lock);
    struct seq_sequence *q = &sq->sequence[sq->current];
    int n = s->bank * 16 + i;
    switch (s->mute_type) {
    case MUTE_TRACK:
        if (n < SEQ_TRACKS) q->track[n].mute = !q->track[n].mute;
        break;
    case MUTE_TRACK_SOLO:
        if (n < SEQ_TRACKS) q->track[n].solo = !q->track[n].solo;
        break;
    case MUTE_PAD: {
        int kit = q->track[sq->track].kit;
        sq->pad_mute[kit][n] = !sq->pad_mute[kit][n];
        break;
    }
    case MUTE_PAD_SOLO: {
        int kit = q->track[sq->track].kit;
        sq->pad_solo[kit][n] = !sq->pad_solo[kit][n];
        break;
    }
    case MUTE_SEQUENCE:
        if (n < SEQ_COUNT) {
            queue_sequence(s, n);
            snprintf(msg, sizeof msg, sq->mode == SEQ_STOPPED ? "Sequence: %d" : "Next: %d", n + 1);
            pthread_mutex_unlock(&sq->lock);
            os_toast(s, u, msg);
            return;
        }
        break;
    }
    if ((s->mute_type == MUTE_TRACK || s->mute_type == MUTE_TRACK_SOLO) && !sq->mute_next) seq_tracks_heard(s); // at once
    pthread_mutex_unlock(&sq->lock);
}

// Every mute (or solo, or pad mute ...) of the current type off. Caller holds the lock.
static void clear_type(struct os *s) {
    struct seq *sq = &s->seq;
    struct seq_sequence *q = &sq->sequence[sq->current];
    int kit = q->track[sq->track].kit;
    for (int t = 0; t < SEQ_TRACKS; t++) {
        if (s->mute_type == MUTE_TRACK) q->track[t].mute = 0;
        if (s->mute_type == MUTE_TRACK_SOLO) q->track[t].solo = 0;
    }
    if (s->mute_type == MUTE_PAD) memset(sq->pad_mute[kit], 0, 128);
    if (s->mute_type == MUTE_PAD_SOLO) memset(sq->pad_solo[kit], 0, 128);
    if (!sq->mute_next) seq_tracks_heard(s);
}

// SHIFT + MUTE (UNMUTE ALL), on any page: every track of the sequence playing unmuted.
void mute_unmute_all(struct os *s, struct ui *u) {
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    struct seq_sequence *q = &sq->sequence[sq->current];
    for (int t = 0; t < SEQ_TRACKS; t++) q->track[t].mute = 0;
    if (!sq->mute_next) seq_tracks_heard(s);
    pthread_mutex_unlock(&sq->lock);
    os_toast(s, u, "All tracks unmuted");
}

static void cell(struct ui *u, int i, const char *top, const char *bottom, int lit) {
    int x = (i % 4) * CELL_PX_W, y = GRID_Y + (3 - i / 4) * CELL_PX_H; // pad 1 bottom left, as on the pads
    uint32_t ink = lit ? LCD_BG : LCD_FG;
    if (lit) lcd_fill(u, x + 2, y + 2, CELL_PX_W - 4, CELL_PX_H - 4, LCD_FG);
    else {
        lcd_fill(u, x + 2, y + 2, CELL_PX_W - 4, 1, LCD_FG);
        lcd_fill(u, x + 2, y + CELL_PX_H - 3, CELL_PX_W - 4, 1, LCD_FG);
        lcd_fill(u, x + 2, y + 2, 1, CELL_PX_H - 4, LCD_FG);
        lcd_fill(u, x + CELL_PX_W - 3, y + 2, 1, CELL_PX_H - 4, LCD_FG);
    }
    lcd_small_text(u, x + 5, y + 7, top, ink);
    lcd_small_text(u, x + 5, y + 21, bottom, ink);
}

// The MPC2000XL layout: its Track Mute page. Each pad's name in a 4 x 4 grid, pad 1 bottom left,
// on a bar while it plays (TRACK MUTE: the tracks not muted) and plain when not; a track with
// nothing on it is (Unused), as on the XL. Caller holds the lock.
static void mute_xl(struct os *s, struct ui *u) {
    struct seq *sq = &s->seq;
    struct seq_sequence *q = &sq->sequence[sq->current];
    int kit = q->track[sq->track].kit;
    char b[40];
    lcd_text(u, 0, KEY_ROW, "<Type>");
    lcd_text(u, 8, KEY_ROW, "<All Off>");
    lcd_text(u, 20, KEY_ROW, "<Back>");
    int next = sq->next >= 0 ? sq->next : sq->current;
    lcd_text(u, 0, 2, "Sq:");
    snprintf(b, sizeof b, "%02d-%.16s", next + 1, seq_used(&sq->sequence[next]) ? sq->sequence[next].name : "(unused)");
    if (s->cursor_mute == M_NEXT) lcd_inverse(u, 3, 2, b);
    else lcd_text(u, 3, 2, b);
    long at = sq->pos > 0 ? (long)sq->pos : 0;
    int bt = seq_bar_ticks(q), qt = seq_beat_ticks(q);
    snprintf(b, sizeof b, "Now:%03ld.%02ld.%02ld", at / bt + 1, at % bt / qt + 1, at % qt);
    lcd_text(u, 0, 3, b);
    if (sq->mode != SEQ_STOPPED && sq->next >= 0) {
        snprintf(b, sizeof b, "Plays %d", sq->current + 1);
        lcd_text(u, 18, 3, b);
    }
    // The box: what the grid holds, then the grid.
    lcd_box(u, 0, 4 * CELL_H + 6, UI_W, 9 * CELL_H + 4);
    if (s->mute_type <= MUTE_TRACK_SOLO) snprintf(b, sizeof b, "Tr:%02d-%02d BANK %c", s->bank * 16 + 1, s->bank * 16 + 16, 'A' + s->bank);
    else if (s->mute_type <= MUTE_PAD_SOLO) snprintf(b, sizeof b, "Pgm:%02d BANK %c", kit + 1, 'A' + s->bank);
    else snprintf(b, sizeof b, "Sq:%02d-%02d BANK %c", s->bank * 16 + 1, s->bank * 16 + 16, 'A' + s->bank);
    lcd_text(u, 1, 5, b);
    int top = 6 * CELL_H + 8, row_h = 28, col_w = (UI_W - 8) / 4;
    for (int i = 0; i < 16; i++) {
        int n = s->bank * 16 + i, on = 0;
        char name[16] = "";
        switch (s->mute_type) {
        case MUTE_TRACK: case MUTE_TRACK_SOLO:
            if (n >= SEQ_TRACKS) break;
            on = s->mute_type == MUTE_TRACK ? !q->track[n].mute : q->track[n].solo;
            snprintf(name, sizeof name, "%.12s", seq_track_used(&q->track[n]) ? q->track[n].name : "(Unused)");
            break;
        case MUTE_PAD: case MUTE_PAD_SOLO: {
            const struct os_pad *p = &s->programs[kit].pads[n];
            on = s->mute_type == MUTE_PAD ? !sq->pad_mute[kit][n] : sq->pad_solo[kit][n];
            snprintf(name, sizeof name, "%.12s", p->s ? p->name : "(Unused)");
            break;
        }
        case MUTE_SEQUENCE:
            if (n >= SEQ_COUNT) break;
            on = n == next;
            snprintf(name, sizeof name, "%.12s", seq_used(&sq->sequence[n]) ? sq->sequence[n].name : "(Unused)");
            break;
        }
        int x = 4 + (i % 4) * col_w, y = top + (3 - i / 4) * row_h;
        if (on) {
            lcd_fill(u, x, y, col_w - 4, 13, LCD_FG);
            lcd_small_text(u, x + 3, y + 3, name, LCD_BG);
        } else {
            lcd_small_text(u, x + 3, y + 3, name, LCD_FG);
        }
    }
    lcd_text(u, 0, 14, "Type:");
    snprintf(b, sizeof b, "%s", TYPES[s->mute_type]);
    if (s->cursor_mute == M_TYPE) lcd_inverse(u, 5, 14, b);
    else lcd_text(u, 5, 14, b);
}

void mute_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    if (input_down(in, IN_B1)) s->mute_type = (s->mute_type + (in->shift ? MUTE_TYPES - 1 : 1)) % MUTE_TYPES; // <Type>
    if (input_down(in, IN_B2)) clear_type(s); // <All Off>
    if (input_down(in, IN_B3)) { // <Back>
        s->screen = s->mute_return == OS_MUTE ? OS_MAIN : s->mute_return;
        pthread_mutex_unlock(&sq->lock);
        ui_wake(u, 1);
        return;
    }
    if (in->encoder_press) s->cursor_mute = (s->cursor_mute + 1) % M_FIELDS;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d && s->cursor_mute == M_TYPE) s->mute_type = clampi(s->mute_type + d, 0, MUTE_TYPES - 1);
    if (d && s->cursor_mute == M_NEXT) queue_sequence(s, (sq->next >= 0 ? sq->next : sq->current) + d);

    if (lcd_layout == LAYOUT_2000XL) {
        mute_xl(s, u);
        if (sq->mode != SEQ_STOPPED) ui_wake(u, 100);
        pthread_mutex_unlock(&sq->lock);
        return;
    }
    struct seq_sequence *q = &sq->sequence[sq->current];
    int kit = q->track[sq->track].kit;
    char b[40], top[16], bottom[16];
    lcd_text(u, 0, KEY_ROW, "<Type>");
    lcd_text(u, 8, KEY_ROW, "<All Off>");
    lcd_text(u, 20, KEY_ROW, "<Back>");
    lcd_text(u, 0, 2, "Type:");
    snprintf(b, sizeof b, " %s ", TYPES[s->mute_type]);
    if (s->cursor_mute == M_TYPE) lcd_inverse(u, 5, 2, b);
    else lcd_text(u, 5, 2, b);
    int next = sq->next >= 0 ? sq->next : sq->current;
    lcd_text(u, 0, 3, "Next Seq:");
    snprintf(b, sizeof b, " %d-%.12s ", next + 1, sq->sequence[next].name);
    if (s->cursor_mute == M_NEXT) lcd_inverse(u, 9, 3, b);
    else lcd_text(u, 9, 3, b);
    // What the grid shows, on its own line just above it.
    if (s->mute_type <= MUTE_TRACK_SOLO) snprintf(b, sizeof b, "Tracks %d-%d", s->bank * 16 + 1, s->bank * 16 + 16);
    else if (s->mute_type <= MUTE_PAD_SOLO) snprintf(b, sizeof b, "P%02d Bank %c", kit + 1, 'A' + s->bank);
    else snprintf(b, sizeof b, "Sequences %d-%d", s->bank * 16 + 1, s->bank * 16 + 16);
    lcd_small_text(u, 4, 64, b, LCD_FG);
    if (sq->mode != SEQ_STOPPED && sq->next >= 0) {
        snprintf(b, sizeof b, "Playing %d", sq->current + 1);
        lcd_small_text(u, 200, 64, b, LCD_FG);
    }
    for (int i = 0; i < 16; i++) {
        int n = s->bank * 16 + i, lit = 0;
        top[0] = bottom[0] = 0;
        switch (s->mute_type) {
        case MUTE_TRACK: case MUTE_TRACK_SOLO:
            if (n >= SEQ_TRACKS) break;
            lit = s->mute_type == MUTE_TRACK ? q->track[n].mute : q->track[n].solo;
            snprintf(top, sizeof top, "T%02d %s", n + 1, lit ? (s->mute_type == MUTE_TRACK ? "MUTE" : "SOLO") : "");
            snprintf(bottom, sizeof bottom, "%.12s", seq_track_used(&q->track[n]) ? q->track[n].name : "(unused)");
            break;
        case MUTE_PAD: case MUTE_PAD_SOLO: {
            const struct os_pad *p = &s->programs[kit].pads[n];
            lit = s->mute_type == MUTE_PAD ? sq->pad_mute[kit][n] : sq->pad_solo[kit][n];
            snprintf(top, sizeof top, "%c%02d %s", 'A' + n / 16, n % 16 + 1, lit ? (s->mute_type == MUTE_PAD ? "MUTE" : "SOLO") : "");
            snprintf(bottom, sizeof bottom, "%.12s", p->s ? p->name : "--");
            break;
        }
        case MUTE_SEQUENCE:
            if (n >= SEQ_COUNT) break;
            lit = n == next;
            snprintf(top, sizeof top, "S%02d %s", n + 1, n == sq->current ? "<" : "");
            snprintf(bottom, sizeof bottom, "%.12s", seq_used(&sq->sequence[n]) ? sq->sequence[n].name : "(unused)");
            break;
        }
        cell(u, i, top, bottom, lit);
    }
    if (sq->mode != SEQ_STOPPED) ui_wake(u, 100);
    pthread_mutex_unlock(&sq->lock);
}
