// SONG (Shift + Pad 12): sequences chained into a song. Pads add steps (bank A pad 1 is
// sequence 1, bank B pad 1 sequence 17 ...), each at the selected step, which then moves on, so
// a song is typed in with the pads. The wheel and - + set a step's sequence and Reps. The song
// ends after its last step. PLAY plays it from the selected step; <Convert> makes it one sequence.
#include "lcd.h"
#include "os.h"
#include "seq_screen.h"
#include <stdio.h>
#include <string.h>

enum { G_SONG, G_STEP, G_SEQ, G_REPS, G_FIELDS };
#define LIST_ROW 5
#define LIST_ROWS 7

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void song_open(struct os *s) {
    if (s->screen == OS_SONG) {
        s->cursor_song = G_SEQ; // pressed again: the selected step's sequence
        return;
    }
    s->song_return = s->screen;
    s->screen = OS_SONG;
    s->cursor_song = G_SEQ;
}

// Caller holds the lock. A step for sequence `seq` put in at `at`; the steps after move down.
static int insert_step(struct seq_song *g, int at, int seq) {
    if (g->nsteps >= SONG_STEPS) return -1;
    at = clampi(at, 0, g->nsteps);
    memmove(&g->step[at + 1], &g->step[at], (size_t)(g->nsteps - at) * sizeof g->step[0]);
    g->step[at].seq = (unsigned char)seq;
    g->step[at].reps = 1;
    g->nsteps++;
    return 0;
}

static void delete_step(struct seq_song *g, int at) {
    if (at < 0 || at >= g->nsteps) return;
    memmove(&g->step[at], &g->step[at + 1], (size_t)(g->nsteps - at - 1) * sizeof g->step[0]);
    g->nsteps--;
}

// A pad on this page: its sequence goes in at the selected step, and the selection moves past it.
void song_pad(struct os *s, struct ui *u, int i) {
    struct seq *sq = &s->seq;
    int seq = s->bank * 16 + i;
    if (seq >= SEQ_COUNT) return;
    os_undo_save(s, "Song Step");
    pthread_mutex_lock(&sq->lock);
    struct seq_song *g = &sq->song[s->song_sel];
    int ok = insert_step(g, s->song_step_sel, seq) == 0;
    if (ok) s->song_step_sel++;
    pthread_mutex_unlock(&sq->lock);
    char msg[32];
    snprintf(msg, sizeof msg, ok ? "Step: Seq %d" : "Song is full", seq + 1);
    os_toast(s, u, msg);
}

// The first sequence with nothing in it and not used by the song, or -1.
static int empty_sequence(const struct seq *sq, const struct seq_song *g) {
    for (int n = 0; n < SEQ_COUNT; n++) {
        int used = seq_used(&sq->sequence[n]);
        for (int i = 0; i < g->nsteps && !used; i++) used = g->step[i].seq == n;
        if (!used) return n;
    }
    return -1;
}

void song_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    int export_to = -1, confirmed = -1;
    // <Convert> asks first; while it asks, the keys and the wheel answer it and nothing else.
    struct input quiet, none;
    input_begin(&none, 0);
    const struct input *real = in;
    // SHIFT + B2 <Rename>: the song's name; the box takes the keys and the wheel while it is open.
    int opened = 0;
    if (!s->song_ask && s->rename_what == RENAME_NONE && input_down(in, IN_B2) && in->shift) seq_rename_song(s), opened = 1;
    if (s->rename_what == RENAME_SONG) {
        quiet = *in;
        quiet.down = 0, quiet.encoder = 0, quiet.encoder_press = 0;
        in = &quiet;
    }
    if (s->song_ask) {
        if (input_down(in, IN_B3)) confirmed = s->song_ask - 1;
        if (input_down(in, IN_B1) || input_down(in, IN_B3)) s->song_ask = 0;
        quiet = *in;
        quiet.down = 0, quiet.encoder = 0, quiet.encoder_press = 0;
        in = &quiet;
    }
    if (input_down(in, IN_B1) || input_down(in, IN_B2)) os_undo_save(s, "Song Step");
    pthread_mutex_lock(&sq->lock);
    s->song_sel = clampi(s->song_sel, 0, SEQ_SONGS - 1);
    struct seq_song *g = &sq->song[s->song_sel];
    s->song_step_sel = clampi(s->song_step_sel, 0, g->nsteps);
    int at = s->song_step_sel;
    if (input_down(in, IN_B1)) delete_step(g, at); // <Delete>
    if (input_down(in, IN_B2)) { // <Insert>: another of the selected step (or the current sequence)
        int seq = at < g->nsteps ? g->step[at].seq : sq->current;
        if (insert_step(g, at, seq) == 0 && at < g->nsteps - 1) g->step[at].reps = g->step[at + 1].reps;
    }
    if (input_down(in, IN_B3)) export_to = empty_sequence(sq, g); // <Convert>
    if (in->encoder_press) s->cursor_song = (s->cursor_song + (in->shift ? G_FIELDS - 1 : 1)) % G_FIELDS;
    int d = in->encoder + (in->shift ? 0 : input_down(in, IN_PLUS) - input_down(in, IN_MINUS));
    if (d) {
        switch (s->cursor_song) {
        case G_SONG: s->song_sel = clampi(s->song_sel + d, 0, SEQ_SONGS - 1), s->song_step_sel = 0; break;
        case G_STEP: s->song_step_sel = clampi(at + d, 0, g->nsteps); break;
        case G_SEQ:
            if (at >= g->nsteps) { // on (end): a new step, from the last step's sequence on
                int base = g->nsteps ? g->step[g->nsteps - 1].seq : sq->current;
                insert_step(g, at, clampi(base + d, 0, SEQ_COUNT - 1));
            } else {
                g->step[at].seq = (unsigned char)clampi(g->step[at].seq + d, 0, SEQ_COUNT - 1);
            }
            break;
        case G_REPS:
            if (at < g->nsteps) g->step[at].reps = (unsigned char)clampi(g->step[at].reps + d, 1, 99);
            break;
        }
        g = &sq->song[s->song_sel];
        at = s->song_step_sel = clampi(s->song_step_sel, 0, g->nsteps);
    }
    int playing = sq->song_on && sq->song_cur == s->song_sel ? sq->song_step : -1;

    if (export_to >= 0) s->song_ask = export_to + 1; // asked first, answered next frame
    char line[48];
    if (s->song_ask) {
        lcd_text(u, 0, KEY_ROW, "<Cancel>");
        lcd_text(u, 17, KEY_ROW, "<Convert>");
    } else {
        lcd_text(u, 0, KEY_ROW, "<Delete>");
        lcd_text(u, 9, KEY_ROW, real->held & (1u << IN_SHIFT) ? "<Rename>" : "<Insert>");
        lcd_text(u, 17, KEY_ROW, "<Convert>");
    }
    char title[32];
    if (g->name[0]) snprintf(title, sizeof title, "Song: %s", g->name);
    else snprintf(title, sizeof title, "Song: Song%02d", s->song_sel + 1);
    lcd_rule(u, TITLE_ROW, title);
    lcd_text(u, 0, 3, "Song:");
    snprintf(line, sizeof line, " %02d ", s->song_sel + 1);
    if (s->cursor_song == G_SONG) lcd_inverse(u, 5, 3, line);
    else lcd_text(u, 5, 3, line);
    snprintf(line, sizeof line, "Steps:%3d", g->nsteps);
    lcd_text(u, 17, 3, line);
    lcd_text(u, 0, 4, "Stp Sequence         Reps");
    int first = at - LIST_ROWS / 2;
    if (first > g->nsteps + 1 - LIST_ROWS) first = g->nsteps + 1 - LIST_ROWS;
    if (first < 0) first = 0;
    for (int r = 0; r < LIST_ROWS; r++) {
        int i = first + r, row = LIST_ROW + r;
        if (i > g->nsteps) break;
        char mark = i == playing ? '>' : ' ';
        if (i == g->nsteps) {
            snprintf(line, sizeof line, "%c%02d (end)", mark, i + 1);
            if (i == at) lcd_inverse(u, 0, row, line);
            else lcd_text(u, 0, row, line);
            continue;
        }
        int n = g->step[i].seq;
        char seq[24], reps[8];
        snprintf(line, sizeof line, "%c%02d", mark, i + 1);
        snprintf(seq, sizeof seq, " %02d-%-13.13s", n + 1, sq->sequence[n].name);
        snprintf(reps, sizeof reps, " %2d ", g->step[i].reps);
        int sel = i == at;
        if (sel && s->cursor_song == G_STEP) lcd_inverse(u, 0, row, line);
        else lcd_text(u, 0, row, line);
        if (sel && s->cursor_song == G_SEQ) lcd_inverse(u, 3, row, seq);
        else lcd_text(u, 3, row, seq);
        if (sel && s->cursor_song == G_REPS) lcd_inverse(u, 21, row, reps);
        else lcd_text(u, 21, row, reps);
    }
    if (s->song_ask) {
        snprintf(line, sizeof line, "Convert to Sequence %02d?", s->song_ask);
        lcd_inverse(u, 0, 13, line);
    } else {
        lcd_text(u, 0, 13, "Pads: A01=Seq1 B01=Seq17");
    }
    lcd_rule(u, BOTTOM_ROW, 0);
    if (sq->mode != SEQ_STOPPED) ui_wake(u, 100);
    pthread_mutex_unlock(&sq->lock);

    if (s->rename_what == RENAME_SONG) seq_rename_overlay(s, u, opened ? &none : real);
    if (input_down(in, IN_B3) && export_to < 0) os_toast(s, u, "No empty sequence");
    if (confirmed >= 0) {
        char msg[32];
        os_undo_save(s, "Convert Song");
        if (seq_song_export(sq, s->song_sel, confirmed) == 0) snprintf(msg, sizeof msg, "Converted: Seq %d", confirmed + 1);
        else snprintf(msg, sizeof msg, "Song empty or too long");
        os_toast(s, u, msg);
    }
}
