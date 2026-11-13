// STEP EDIT (Shift + SEQ), after the MPC3000's: the active track's notes at one position (Now),
// edited one at a time. - and + step Now back and on by the Step note value (Timing Correct's),
// playing what is there; B2 and B3 jump to the previous and next notes. A pad pressed puts a
// note in at Now, a Step long. B1 deletes the selected note.
#include "lcd.h"
#include "os.h"
#include <stdio.h>
#include <string.h>

enum { P_NOW, P_STEP, P_EVENTS }; // then three fields a note: note, velocity, duration
#define EV_ROW 6
#define EV_ROWS 7

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int step_ticks(const struct seq *sq) {
    int g = seq_tc_grid(sq->tc);
    return g > 0 ? g : SEQ_PPQ / 4; // Timing Correct OFF: sixteenths
}

void step_open(struct os *s) {
    if (s->screen == OS_STEP) {
        s->cursor_step = P_NOW; // pressed again: Now
        return;
    }
    s->step_return = s->screen;
    s->screen = OS_STEP;
    s->cursor_step = P_NOW;
    pthread_mutex_lock(&s->seq.lock);
    double pos = s->seq.pos < 0 ? 0 : s->seq.pos;
    s->step_tick = (uint32_t)pos / (uint32_t)step_ticks(&s->seq) * (uint32_t)step_ticks(&s->seq); // on the grid
    pthread_mutex_unlock(&s->seq.lock);
}

// The notes at Now on the active track: their indexes, up to max. Caller holds the lock.
static int at_now(const struct os *s, const struct seq_track *k, int *idx, int max) {
    int n = 0;
    for (int i = 0; i < k->count && n < max; i++)
        if (k->notes[i].tick == s->step_tick) idx[n++] = i;
    return n;
}

// Plays the notes at Now, as the MPC3000 does on each step.
static void audition(struct os *s) {
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    const struct seq_track *k = &sq->sequence[sq->current].track[sq->track];
    int idx[16], n = at_now(s, k, idx, 16), notes[16][3];
    for (int i = 0; i < n; i++) notes[i][0] = k->notes[idx[i]].note, notes[i][1] = k->notes[idx[i]].tune, notes[i][2] = k->notes[idx[i]].vel;
    pthread_mutex_unlock(&sq->lock);
    for (int i = 0; i < n; i++) os_audition(s, notes[i][0], notes[i][1], notes[i][2]);
}

// A pad on this page: a note at Now, a Step long (or its velocity, if that pad is already there).
void step_pad(struct os *s, int note, int tune, int vel) {
    if (s->screen != OS_STEP) return;
    struct seq *sq = &s->seq;
    os_undo_save(s, "Step Edit");
    pthread_mutex_lock(&sq->lock);
    struct seq_track *k = &sq->sequence[sq->current].track[sq->track];
    int idx[16], n = at_now(s, k, idx, 16), found = -1, len = step_ticks(sq);
    for (int i = 0; i < n; i++)
        if (k->notes[idx[i]].note == note && k->notes[idx[i]].tune == tune) found = idx[i];
    pthread_mutex_unlock(&sq->lock);
    if (found >= 0) {
        pthread_mutex_lock(&sq->lock);
        k->notes[found].vel = (uint8_t)vel;
        pthread_mutex_unlock(&sq->lock);
    } else {
        seq_add_note(sq, sq->current, sq->track, s->step_tick, note, vel, len * 9 / 10 > 0 ? len * 9 / 10 : 1);
        pthread_mutex_lock(&sq->lock); // the tuning (16 Levels Tune, S600) goes with it
        n = at_now(s, k, idx, 16);
        for (int i = 0; i < n; i++)
            if (k->notes[idx[i]].note == note && k->notes[idx[i]].vel == vel && k->notes[idx[i]].tune == 0) k->notes[idx[i]].tune = (int8_t)tune;
        pthread_mutex_unlock(&sq->lock);
    }
}

void step_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    int moved = 0;
    pthread_mutex_lock(&sq->lock);
    struct seq_sequence *q = &sq->sequence[sq->current];
    struct seq_track *k = &q->track[sq->track];
    int len = seq_length(q), step = step_ticks(sq);
    int idx[EV_ROWS], n = at_now(s, k, idx, EV_ROWS), fields = P_EVENTS + n * 3;
    if (s->cursor_step >= fields) s->cursor_step = fields - 1;
    // - and +: Now a Step back or on. B2, B3: the previous or next note anywhere on the track.
    int d_step = in->shift ? 0 : input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d_step) {
        long t = (long)s->step_tick + (long)d_step * step;
        if (d_step < 0 && s->step_tick % (uint32_t)step) t = (long)(s->step_tick / (uint32_t)step * (uint32_t)step);
        s->step_tick = (uint32_t)clampi((int)t, 0, len - 1);
        moved = 1;
    }
    if (input_down(in, IN_B2) || input_down(in, IN_B3)) {
        int fwd = input_down(in, IN_B3);
        long best = -1;
        for (int i = 0; i < k->count; i++) {
            long t = k->notes[i].tick;
            if (fwd && t > (long)s->step_tick && (best < 0 || t < best)) best = t;
            if (!fwd && t < (long)s->step_tick && t > best) best = t;
        }
        if (best >= 0) s->step_tick = (uint32_t)best, moved = 1;
    }
    pthread_mutex_unlock(&sq->lock);
    if (input_down(in, IN_B1) && s->cursor_step >= P_EVENTS) { // <Delete> the selected note
        os_undo_save(s, "Step Delete");
        pthread_mutex_lock(&sq->lock);
        int e = (s->cursor_step - P_EVENTS) / 3;
        n = at_now(s, k, idx, EV_ROWS);
        if (e < n) {
            memmove(&k->notes[idx[e]], &k->notes[idx[e] + 1], (size_t)(k->count - idx[e] - 1) * sizeof k->notes[0]);
            k->count--;
        }
        pthread_mutex_unlock(&sq->lock);
        s->cursor_step = P_EVENTS + (e > 0 ? (e - 1) * 3 : 0);
    }
    if (moved) {
        s->step_armed = 0;
        audition(s);
    }
    if (in->encoder_press) {
        s->cursor_step = (s->cursor_step + (in->shift ? fields - 1 : 1)) % (fields > 0 ? fields : 1);
        s->step_armed = 0;
    }
    int d = in->encoder;
    if (d && s->cursor_step >= P_EVENTS && !s->step_armed) os_undo_save(s, "Step Edit"), s->step_armed = 1;
    pthread_mutex_lock(&sq->lock);
    n = at_now(s, k, idx, EV_ROWS);
    fields = P_EVENTS + n * 3;
    if (s->cursor_step >= fields) s->cursor_step = fields - 1;
    if (d) {
        if (s->cursor_step == P_NOW) { // the wheel steps Now too; with Shift, a tick at a time
            long t = (long)s->step_tick + (long)d * (in->shift ? 1 : step);
            s->step_tick = (uint32_t)clampi((int)t, 0, len - 1);
        } else if (s->cursor_step == P_STEP) {
            sq->tc = clampi(sq->tc + d, 0, TC_COUNT - 1);
            if (sq->tc != TC_OFF) s->tc_last = sq->tc;
        } else {
            struct seq_note *nt = &k->notes[idx[(s->cursor_step - P_EVENTS) / 3]];
            switch ((s->cursor_step - P_EVENTS) % 3) {
            case 0: nt->note = (uint8_t)clampi(nt->note + d, 0, 127); break;
            case 1: nt->vel = (uint8_t)clampi(nt->vel + d, 1, 127); break;
            default: nt->dur = (uint16_t)clampi(nt->dur + d * (in->shift ? 1 : step / 4 > 0 ? step / 4 : 1), 1, 65535); break;
            }
        }
    }

    // ---- Drawing ----
    char line[48], v[24];
    int midi = k->type == TRACK_MIDI;
    lcd_text(u, 0, KEY_ROW, "<Delete>");
    lcd_text(u, 9, KEY_ROW, "<<Note");
    lcd_text(u, 18, KEY_ROW, "Note>>");
    lcd_rule(u, TITLE_ROW, "Step Edit");
    int bar = seq_bar_ticks(q), beat = seq_beat_ticks(q);
    lcd_text(u, 0, 3, "Now:");
    snprintf(v, sizeof v, " %03d.%02d.%02d ", (int)(s->step_tick / (uint32_t)bar) + 1, (int)(s->step_tick % (uint32_t)bar / (uint32_t)beat) + 1,
             (int)(s->step_tick % (uint32_t)beat));
    if (s->cursor_step == P_NOW) lcd_inverse(u, 4, 3, v);
    else lcd_text(u, 4, 3, v);
    snprintf(line, sizeof line, "Trk:%02d", sq->track + 1);
    lcd_text(u, 20, 3, line);
    static const char *const STEPS[TC_COUNT] = {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64", "1/64T", "OFF:1/16"};
    lcd_text(u, 0, 4, "Step:");
    snprintf(v, sizeof v, " %s ", STEPS[sq->tc]);
    if (s->cursor_step == P_STEP) lcd_inverse(u, 5, 4, v);
    else lcd_text(u, 5, 4, v);
    lcd_rule(u, 5, n ? "Notes here" : 0);
    for (int e = 0; e < n; e++) {
        const struct seq_note *nt = &k->notes[idx[e]];
        int row = EV_ROW + e, col = 0;
        char f[3][16];
        if (midi) snprintf(f[0], 16, " N:%3d ", nt->note);
        else if (nt->tune) snprintf(f[0], 16, " %c%02d%+d ", 'A' + nt->note / 16, nt->note % 16 + 1, nt->tune);
        else snprintf(f[0], 16, " %c%02d ", 'A' + nt->note / 16, nt->note % 16 + 1);
        snprintf(f[1], 16, " V:%3d ", nt->vel);
        snprintf(f[2], 16, " D:%4d ", nt->dur);
        for (int c = 0; c < 3; c++) {
            if (s->cursor_step == P_EVENTS + e * 3 + c) lcd_inverse(u, col, row, f[c]);
            else lcd_text(u, col, row, f[c]);
            col += (int)strlen(f[c]);
        }
    }
    if (!n) lcd_center(u, 8, "(no notes here)");
    lcd_text(u, 0, 13, "-/+ step, pads add notes");
    lcd_rule(u, BOTTOM_ROW, 0);
    pthread_mutex_unlock(&sq->lock);
}
