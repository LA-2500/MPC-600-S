// STEP EDIT (Shift + SEQ), after the MPC3000's: the active track's notes at one position (Now),
// edited one at a time. - and + step Now back and on by the Step note value (Timing Correct's),
// playing what is there; B2 and B3 jump to the previous and next notes. A pad pressed puts a
// note in at Now, a Step long. B1 deletes the selected note.
//
// GRID (Shift + SEQ again, and again back to the list), its look taken from the Atari drum edit
// screens: the 16 pads of the bank as rows, their names on the left, 16 Steps across with a
// diamond for each note. - and + step Now (the cursor column), the wheel picks the row, B1 puts a
// note in or takes it out where they cross, and B2 / B3 go a page (16 Steps) back or on.
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

static int track_is_midi(const struct os *s);
static void grid_midi_open(struct os *s);

void step_open(struct os *s) {
    if (s->screen == OS_STEP) { // pressed again: the Grid view, or back to the list
        s->step_grid = !s->step_grid;
        s->cursor_step = P_NOW;
        if (s->step_grid && track_is_midi(s)) grid_midi_open(s);
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

// What the Grid's rows stand for: the bank's pads; with 16 Levels on, the levels of its pad
// (Tune: a tuning a row, Velocity: a velocity); on a MIDI track, notes 0-127, 16 at a time.
enum { G_PADS, G_TUNE, G_VEL, G_MIDI };

static int track_is_midi(const struct os *s) {
    const struct seq *sq = &s->seq;
    return sq->sequence[sq->current].track[sq->track].type == TRACK_MIDI;
}

static int grid_mode(const struct os *s) {
    if (track_is_midi(s)) return G_MIDI;
    if (s->levels_on) return s->levels_type == LEVELS_TUNE ? G_TUNE : G_VEL;
    return G_PADS;
}

// The note, tuning and velocity (0: the default) a row puts in.
static void row_key(const struct os *s, int mode, int r, int *note, int *tune, int *vel) {
    int slot = s->bank * 16 + r;
    *note = slot, *tune = 0, *vel = 0;
    if (mode == G_MIDI) *note = s->step_mtop + r;
    if (mode == G_TUNE || mode == G_VEL) *note = s->levels_pad;
    if (mode == G_TUNE) *tune = slot - s->levels_root < -64 ? -64 : slot - s->levels_root > 63 ? 63 : slot - s->levels_root;
    if (mode == G_VEL) *vel = (r + 1) * 8 - 1;
}

// The row a note is on, or -1 (not on this bank, scrolled off, another pad than 16 Levels').
static int note_row(const struct os *s, int mode, int note, int tune, int vel) {
    int r = -1;
    if (mode == G_PADS) r = note - s->bank * 16;
    if (mode == G_MIDI) r = note - s->step_mtop;
    if (mode == G_TUNE && note == s->levels_pad) r = s->levels_root + tune - s->bank * 16;
    if (mode == G_VEL && note == s->levels_pad) r = vel <= 7 ? 0 : vel >= 127 ? 15 : (vel + 1) / 8 - 1;
    return r >= 0 && r < 16 ? r : -1;
}

// The MIDI Grid's selected note, kept on screen.
static void midi_select(struct os *s, int note) {
    s->step_mnote = clampi(note, 0, 127);
    if (s->step_mnote < s->step_mtop) s->step_mtop = s->step_mnote;
    if (s->step_mnote > s->step_mtop + 15) s->step_mtop = s->step_mnote - 15;
    s->step_mtop = clampi(s->step_mtop, 0, 112);
}

// The Grid opening on a MIDI track: the lowest note the track plays, else the bank's first pad's.
static void grid_midi_open(struct os *s) {
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    const struct seq_track *k = &sq->sequence[sq->current].track[sq->track];
    int lo = 128;
    for (int i = 0; i < k->count; i++)
        if (k->notes[i].note < lo) lo = k->notes[i].note;
    pthread_mutex_unlock(&sq->lock);
    if (lo > 127) lo = seq_pad_note(s->bank * 16);
    s->step_mtop = clampi(lo, 0, 112);
    midi_select(s, lo);
}

// A pad on this page: a note at Now, a Step long (or its velocity, if that pad is already there).
void step_pad(struct os *s, int note, int tune, int vel) {
    if (s->screen != OS_STEP) return;
    struct seq *sq = &s->seq;
    int mode = grid_mode(s), r = note_row(s, mode, note, tune, vel);
    if (mode == G_MIDI) midi_select(s, note);
    else if (r >= 0) s->step_row = r;
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
        for (int i = n - 1; i >= 0; i--) // the one just put in: the last on this tick (not a +0 already there)
            if (k->notes[idx[i]].note == note && k->notes[idx[i]].vel == vel && k->notes[idx[i]].tune == 0) {
                k->notes[idx[i]].tune = (int8_t)tune;
                break;
            }
        pthread_mutex_unlock(&sq->lock);
    }
}

static void grid_frame(struct os *s, struct ui *u, const struct input *in);

void step_frame(struct os *s, struct ui *u, const struct input *in) {
    if (s->step_grid) {
        grid_frame(s, u, in);
        return;
    }
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

// ---- Grid ----
#define G_ROWS 16
#define G_COLS 16
#define G_X 64    // where the steps start; the names to the left
#define G_W 16    // a Step, in pixels
#define G_Y 56    // the first row
#define G_H 11    // a row

static void hline(struct ui *u, int x, int y, int w, int dotted) {
    for (int i = 0; i < w; i++)
        if (!dotted || !(i & 1)) lcd_fill(u, x + i, y, 1, 1, LCD_FG);
}

static void vline(struct ui *u, int x, int y, int h, int dotted) {
    for (int i = 0; i < h; i++)
        if (!dotted || !(i & 1)) lcd_fill(u, x, y + i, 1, 1, LCD_FG);
}

// A note: a diamond from its start, filled (hollow when soft, under 64).
static void diamond(struct ui *u, int x, int y, int soft) {
    for (int i = 0; i <= 4; i++) {
        int top = y + i, bottom = y + 8 - i;
        for (int dx = -i; dx <= i; dx++) {
            if (soft && dx != -i && dx != i && i > 0) continue;
            lcd_fill(u, x + 4 + dx, top, 1, 1, LCD_FG);
            lcd_fill(u, x + 4 + dx, bottom, 1, 1, LCD_FG);
        }
    }
}

// Notes together on one row and step (a chord, 16 Levels' tunings at one velocity): a music note.
static void quaver(struct ui *u, int x, int y) {
    static const char *const Q[9] = {"...##..", "...#.#.", "...#..#", "...#..#", "...#...",
                                     ".###...", "####...", "####...", ".##...."};
    for (int r = 0; r < 9; r++)
        for (int i = 0; Q[r][i]; i++)
            if (Q[r][i] == '#') lcd_fill(u, x + i + 1, y + r, 1, 1, LCD_FG);
}

// The window's title bar, as the Atari draws one: stripes, a close box, the name in a clear gap.
static void title_bar(struct ui *u, int y, const char *name) {
    lcd_fill(u, 0, y, UI_W, 13, LCD_BG);
    hline(u, 0, y, UI_W, 0), hline(u, 0, y + 12, UI_W, 0);
    vline(u, 0, y, 13, 0), vline(u, UI_W - 1, y, 13, 0);
    for (int r = y + 2; r <= y + 10; r += 2) hline(u, 13, r, UI_W - 26, 0);
    vline(u, 12, y, 13, 0), vline(u, UI_W - 13, y, 13, 0);
    lcd_fill(u, 4, y + 4, 5, 5, LCD_FG), lcd_fill(u, 5, y + 5, 3, 3, LCD_BG); // close
    lcd_fill(u, UI_W - 9, y + 3, 6, 7, LCD_FG), lcd_fill(u, UI_W - 8, y + 4, 4, 5, LCD_BG); // full
    int w = (int)strlen(name) * 6 + 8, x = (UI_W - w) / 2;
    lcd_fill(u, x, y + 1, w, 11, LCD_BG);
    lcd_small_text(u, x + 4, y + 3, name, LCD_FG);
}

static void note_name(int n, char *b, size_t size) { // C3 = 60, as the Atari sequencers name it
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(b, size, "%s%d", N[n % 12], n / 12 - 2);
}

static void grid_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    int moved = 0, mode = grid_mode(s);
    if (mode == G_MIDI) midi_select(s, s->step_mnote);
    pthread_mutex_lock(&sq->lock);
    struct seq_sequence *q = &sq->sequence[sq->current];
    struct seq_track *k = &q->track[sq->track];
    int len = seq_length(q), step = step_ticks(sq), page = step * G_COLS;
    int d_step = in->shift ? 0 : input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d_step) {
        long t = (long)s->step_tick + (long)d_step * step;
        if (d_step < 0 && s->step_tick % (uint32_t)step) t = (long)(s->step_tick / (uint32_t)step * (uint32_t)step);
        s->step_tick = (uint32_t)clampi((int)t, 0, len - 1);
        moved = 1;
    }
    if (input_down(in, IN_B2) || input_down(in, IN_B3)) { // a page back or on, the same column
        long t = (long)s->step_tick + (input_down(in, IN_B3) ? page : -page);
        if (t >= 0 && t < len) s->step_tick = (uint32_t)t, moved = 1;
    }
    pthread_mutex_unlock(&sq->lock);
    if (in->encoder) {
        if (mode == G_MIDI) midi_select(s, s->step_mnote + in->encoder);
        else s->step_row = clampi(s->step_row + in->encoder, 0, G_ROWS - 1);
    }
    int row = mode == G_MIDI ? s->step_mnote - s->step_mtop : s->step_row;
    if (input_down(in, IN_B1)) { // <Dot>: the selected row's note at Now, in or out (all of them, if several)
        int note, tune, vel, found = 0;
        row_key(s, mode, row, &note, &tune, &vel);
        pthread_mutex_lock(&sq->lock);
        for (int i = 0; i < k->count; i++)
            found += k->notes[i].tick == s->step_tick && note_row(s, mode, k->notes[i].note, k->notes[i].tune, k->notes[i].vel) == row;
        pthread_mutex_unlock(&sq->lock);
        os_undo_save(s, found ? "Grid Remove" : "Grid Note");
        pthread_mutex_lock(&sq->lock);
        if (found) {
            int j = 0;
            for (int i = 0; i < k->count; i++)
                if (!(k->notes[i].tick == s->step_tick && note_row(s, mode, k->notes[i].note, k->notes[i].tune, k->notes[i].vel) == row))
                    k->notes[j++] = k->notes[i];
            k->count = j;
        }
        pthread_mutex_unlock(&sq->lock);
        if (!found) {
            if (!vel) vel = s->full_level ? 127 : 100;
            seq_add_note(sq, sq->current, sq->track, s->step_tick, note, vel, step * 9 / 10 > 0 ? step * 9 / 10 : 1);
            pthread_mutex_lock(&sq->lock); // its tuning (16 Levels Tune) goes with it
            for (int i = k->count - 1; tune && i >= 0; i--) // the one just put in: the last on this tick
                if (k->notes[i].tick == s->step_tick && k->notes[i].note == note && k->notes[i].vel == vel && k->notes[i].tune == 0) {
                    k->notes[i].tune = (int8_t)tune;
                    break;
                }
            pthread_mutex_unlock(&sq->lock);
            os_audition(s, note, tune, vel);
        }
    }
    if (moved) audition(s);

    // ---- Drawing ----
    lcd_text(u, 0, KEY_ROW, "<Dot>");
    lcd_text(u, 9, KEY_ROW, "<<Page");
    lcd_text(u, 18, KEY_ROW, "Page>>");
    pthread_mutex_lock(&sq->lock);
    int bar = seq_bar_ticks(q), beat = seq_beat_ticks(q);
    // Playing: the page the play position is on, turning as it goes, and a line where it is.
    int playing = sq->mode != SEQ_STOPPED && sq->mode != SEQ_COUNT_IN && sq->mode != SEQ_WAITING && sq->pos >= 0;
    uint32_t play = playing ? (uint32_t)sq->pos : 0;
    uint32_t start = (playing ? play : s->step_tick) / (uint32_t)page * (uint32_t)page;
    int now_here = s->step_tick >= start && s->step_tick < start + (uint32_t)page;
    char line[64], tl[40];
    static const char *const WHAT[4] = {"", "16 LEVELS TUNE", "16 LEVELS VEL", "MIDI"};
    if (mode == G_PADS) snprintf(tl, sizeof tl, "GRID: TRACK %02d", sq->track + 1);
    else snprintf(tl, sizeof tl, "GRID: TRK %02d %s", sq->track + 1, WHAT[mode]);
    title_bar(u, 18, tl);
    // The info line: Now, the Step, and what is under the cursor.
    static const char *const STEPS[TC_COUNT] = {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64", "1/64T", "1/16"};
    int here = 0, vel = 0;
    for (int i = 0; i < k->count; i++)
        if (k->notes[i].tick == s->step_tick && note_row(s, mode, k->notes[i].note, k->notes[i].tune, k->notes[i].vel) == row)
            here++, vel = k->notes[i].vel;
    uint32_t shown = playing ? play : s->step_tick;
    snprintf(line, sizeof line, "%s %03d.%02d.%02d  Step %-5s", playing ? "Play" : "Now ", (int)(shown / (uint32_t)bar) + 1,
             (int)(shown % (uint32_t)bar / (uint32_t)beat) + 1, (int)(shown % (uint32_t)beat), STEPS[sq->tc]);
    lcd_small_text(u, 4, 34, line, LCD_FG);
    if (!now_here) here = 0; // Now is on another page: nothing under it shown here
    if (here > 1) snprintf(line, sizeof line, "%2d notes", here);
    else if (here) snprintf(line, sizeof line, " Vel %3d", vel);
    else snprintf(line, sizeof line, " Vel ---");
    lcd_small_text(u, UI_W - 4 - 8 * 6, 34, line, LCD_FG);
    hline(u, 0, 44, UI_W, 0);
    // The ruler: the bar's number where one starts, the beat's where a beat does; Now's step dark.
    lcd_small_text(u, 4, 47, mode == G_MIDI ? "Note" : mode == G_PADS ? "Sound" : "Level", LCD_FG);
    int cx = G_X + (int)(s->step_tick - start) * G_W / step, ry = G_Y + row * G_H;
    uint32_t now_col = start + (s->step_tick - start) / (uint32_t)step * (uint32_t)step;
    if (now_here) lcd_fill(u, cx + 1, 45, G_W - 1, G_Y - 46, LCD_FG);
    for (int c = 0; c < G_COLS; c++) {
        uint32_t t = start + (uint32_t)(c * step);
        int x = G_X + c * G_W;
        if (t % (uint32_t)bar == 0) snprintf(line, sizeof line, "%d", (int)(t / (uint32_t)bar) + 1);
        else if (t % (uint32_t)beat == 0) snprintf(line, sizeof line, ".%d", (int)(t % (uint32_t)bar / (uint32_t)beat) + 1);
        else line[0] = 0;
        if (t >= (uint32_t)len) line[0] = 0;
        if (line[0]) lcd_small_text(u, x + 2, 47, line, now_here && t == now_col ? LCD_BG : LCD_FG);
    }
    hline(u, 0, G_Y - 1, UI_W, 0);
    vline(u, G_X - 1, 44, G_Y + G_ROWS * G_H - 44, 0);
    // The steps' lines: solid at a beat, dotted between; past the end, shaded.
    for (int c = 0; c <= G_COLS; c++) {
        uint32_t t = start + (uint32_t)(c * step);
        int x = G_X + c * G_W;
        if (x >= UI_W) x = UI_W - 1;
        if (c > 0) vline(u, x, G_Y, G_ROWS * G_H, t % (uint32_t)beat != 0);
        if (c < G_COLS && t >= (uint32_t)len)
            for (int y = G_Y; y < G_Y + G_ROWS * G_H; y += 2)
                for (int xx = x + 1 + (y / 2 & 1); xx < x + G_W; xx += 2) lcd_fill(u, xx, y, 1, 1, LCD_FG);
    }
    // Playing: the rows with a note sounding now, lit (a short note stays lit for a step).
    int active[G_ROWS] = {0};
    for (int i = 0; playing && i < k->count; i++) {
        const struct seq_note *nt = &k->notes[i];
        uint32_t held = nt->dur > (uint32_t)step ? nt->dur : (uint32_t)step;
        if (nt->tick > play || play >= nt->tick + held) continue;
        int r = note_row(s, mode, nt->note, nt->tune, nt->vel);
        if (r >= 0 && r < G_ROWS) active[r] = 1;
    }
    for (int r = 0; r < G_ROWS; r++) // across the steps: a solid line above and below the row
        if (active[r]) {
            int y = G_Y + r * G_H;
            hline(u, G_X, y, UI_W - G_X, 0), hline(u, G_X, y + 1, UI_W - G_X, 0);
            hline(u, G_X, y + G_H - 2, UI_W - G_X, 0), hline(u, G_X, y + G_H - 1, UI_W - G_X, 0);
        }
    // The rows' names: the pad and its sound; the level; the note.
    for (int r = 0; r < G_ROWS; r++) {
        int y = G_Y + r * G_H, slot = s->bank * 16 + r, sel = r == row, note, tune, v;
        row_key(s, mode, r, &note, &tune, &v);
        if (mode == G_MIDI) {
            char nn[8];
            note_name(note, nn, sizeof nn);
            snprintf(line, sizeof line, "%-4s %3d", nn, note);
        } else if (mode == G_TUNE) snprintf(line, sizeof line, "%c%02d %+d", 'A' + note / 16, note % 16 + 1, tune);
        else if (mode == G_VEL) snprintf(line, sizeof line, "%c%02d v%d", 'A' + note / 16, note % 16 + 1, v);
        else snprintf(line, sizeof line, "%c%02d %.6s", 'A' + slot / 16, slot % 16 + 1, s->pads[slot].s ? s->pads[slot].name : "");
        int lit = sel || active[r];
        if (lit) lcd_fill(u, 0, y, G_X - 1, G_H - 1, LCD_FG);
        if (sel && active[r]) lcd_fill(u, 1, y + 1, G_X - 3, G_H - 3, LCD_BG), lit = 0; // the selected row sounding: a box
        lcd_small_text(u, 2, y + 2, line, lit ? LCD_BG : LCD_FG);
        if (r < G_ROWS - 1) hline(u, 0, y + G_H - 1, G_X - 1, 1);
    }
    if (mode == G_MIDI) { // more above, more below: arrows by the names
        if (s->step_mtop > 0) lcd_small_text(u, G_X - 8, G_Y + 2, "^", s->step_mnote == s->step_mtop ? LCD_BG : LCD_FG);
        if (s->step_mtop < 112) lcd_small_text(u, G_X - 8, G_Y + 15 * G_H + 2, "v", s->step_mnote == s->step_mtop + 15 ? LCD_BG : LCD_FG);
    }
    // The notes: a diamond, or a music note where several share the row and the moment.
    for (int i = 0; i < k->count; i++) {
        const struct seq_note *nt = &k->notes[i];
        if (nt->tick < start || nt->tick >= start + (uint32_t)page) continue;
        int r = note_row(s, mode, nt->note, nt->tune, nt->vel), first = 1, many = 0;
        if (r < 0) continue;
        for (int j = 0; j < k->count; j++) {
            if (j == i || k->notes[j].tick != nt->tick || note_row(s, mode, k->notes[j].note, k->notes[j].tune, k->notes[j].vel) != r) continue;
            many = 1;
            if (j < i) first = 0;
        }
        if (!first) continue; // drawn once
        int x = G_X + (int)(nt->tick - start) * G_W / step + 3, y = G_Y + r * G_H + 1;
        if (many) quaver(u, x, y);
        else diamond(u, x, y, nt->vel < 64);
    }
    // The selected row's cell under Now: a box.
    if (now_here) {
        hline(u, cx + 1, ry, G_W - 1, 0), hline(u, cx + 1, ry + G_H - 1, G_W - 1, 0);
        for (int i = 0; i < 2; i++) vline(u, cx + 1 + i, ry, G_H, 0), vline(u, cx + G_W - 1 - i, ry, G_H, 0);
    }
    // The play position: a line down the steps, a marker on the ruler.
    if (playing) {
        int px = G_X + (int)((double)(play - start) * G_W / step);
        if (px >= UI_W - 1) px = UI_W - 2;
        vline(u, px, G_Y, G_ROWS * G_H, 0), vline(u, px + 1, G_Y, G_ROWS * G_H, 0);
        for (int i = 0; i < 4; i++) lcd_fill(u, px - 3 + i, 45 + i, 8 - 2 * i, 1, LCD_FG); // a small triangle, pointing down
    }
    pthread_mutex_unlock(&sq->lock);
    hline(u, 0, G_Y + G_ROWS * G_H, UI_W, 0);
    if (playing || sq->mode != SEQ_STOPPED) ui_wake(u, 40); // the play line, the page and the lit rows keep moving
}
