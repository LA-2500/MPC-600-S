// Pattern Edit (B3 <Edit> on the Arpeggiator page, on a USER pattern): a step pattern made
// step by step. Each step is a note from C to B (on the lowest pad held),
// an octave down or up, NOTE, TIE (the note before goes on) or REST, with Accent (full
// velocity, the other notes at 70%) and Slide (the note glides into the next one).
//
// Pads 1-12 enter C to B into the selected step and move on; pads 13-16 set its octave down,
// octave up, accent and slide first. B2 <Rest> and B3 <Tie> enter those. Shift+B2 plays the
// pattern on the selected pad; Shift+B1 clears it.
//
// The knobs work from where they are, not catching up: K1 moves the selected step (a step for
// each few degrees), K2 turned right puts an accent on it and left takes it off, K3 the same
// for a slide.
#include "lcd.h"
#include "os.h"
#include <stdio.h>
#include <string.h>

enum { AE_STEP, AE_LEN, AE_NOTE, AE_OCT, AE_TIME, AE_ACCENT, AE_SLIDE, AE_FIELDS };
static const char *const NOTES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const TIMES[3] = {"NOTE", "TIE", "REST"};

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static struct seq_arp_steps *pattern(struct os *s) {
    return &s->seq.arp_user[clampi(s->arpedit_slot, 0, ARP_USER - 1)];
}

static void changed(struct os *s) {
    s->fx_save_at = (s->frame_ms ? s->frame_ms : 1) + 1500; // kept on the card with the settings
}

static void preview(struct os *s, int on) {
    struct seq *sq = &s->seq;
    if (on) {
        pthread_mutex_lock(&sq->lock);
        sq->arp = 1;
        sq->arp_pattern = seq_arp_user_pattern(s->arpedit_slot);
        pthread_mutex_unlock(&sq->lock);
        seq_set_repeat(sq, 1);
        seq_repeat_press(sq, 0, s->selected, 100, 0, 0); // as if the selected pad were held
    } else if (s->arpedit_play) {
        seq_set_repeat(sq, 0);
    }
    s->arpedit_play = on;
}

void arpedit_open(struct os *s, int slot) {
    s->screen = OS_ARPEDIT;
    s->arpedit_slot = clampi(slot, 0, ARP_USER - 1);
    s->arpedit_step = 0;
    s->arpedit_ask = 0;
    s->cursor_arpedit = AE_STEP;
    s->arpedit_play = 0;
    s->arpedit_turn = 0;
    for (int k = 0; k < 3; k++) s->knob_last[k] = -1, s->knob_caught[k] = 0; // read from where they are
}

void arpedit_leave(struct os *s) {
    preview(s, 0);
}

static void next_step(struct os *s) {
    s->arpedit_step = (s->arpedit_step + 1) % clampi(pattern(s)->len, 1, ARP_STEPS);
}

// A pad on the page. Returns the semitones to sound it at, or ARPEDIT_SILENT.
int arpedit_pad(struct os *s, int i) {
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    struct seq_arp_step *st = &pattern(s)->step[clampi(s->arpedit_step, 0, ARP_STEPS - 1)];
    int sound = ARPEDIT_SILENT;
    if (i < 12) { // the note, entered: the step plays it, and the next is selected
        st->note = (signed char)i, st->time = ARP_NOTE;
        sound = i + 12 * st->oct;
        next_step(s);
    } else if (i == 12) {
        st->oct = st->oct == -1 ? 0 : -1;
    } else if (i == 13) {
        st->oct = st->oct == 1 ? 0 : 1;
    } else if (i == 14) {
        st->accent = !st->accent;
    } else if (i == 15) {
        st->slide = !st->slide;
    }
    pthread_mutex_unlock(&sq->lock);
    changed(s);
    return s->arpedit_play ? ARPEDIT_SILENT : sound; // playing, the pattern is heard instead
}

#define KNOB_STEP 6 // K1: knob units for each step it moves
#define KNOB_FLIP 3 // K2, K3: how far a turn goes before it sets or clears

// K1-K3, relative: how far each moved since the last frame.
static void knobs(struct os *s, struct seq_arp_steps *p, const struct input *in) {
    for (int k = 0; k < 3; k++) {
        if (!in->knob_moved[k]) continue;
        int now = in->knob[k], last = s->knob_last[k];
        if (last < 0) { // the first reading: only where it is
            s->knob_last[k] = now;
            continue;
        }
        int d = now - last;
        struct seq_arp_step *st = &p->step[s->arpedit_step];
        if (k == 0) {
            s->knob_last[k] = now;
            s->arpedit_turn += d;
            while (s->arpedit_turn >= KNOB_STEP) s->arpedit_turn -= KNOB_STEP, s->arpedit_step = (s->arpedit_step + 1) % p->len;
            while (s->arpedit_turn <= -KNOB_STEP) s->arpedit_turn += KNOB_STEP, s->arpedit_step = (s->arpedit_step + p->len - 1) % p->len;
            s->cursor_arpedit = AE_STEP;
        } else if (d >= KNOB_FLIP || d <= -KNOB_FLIP) {
            s->knob_last[k] = now;
            if (k == 1) st->accent = d > 0, s->cursor_arpedit = AE_ACCENT;
            else st->slide = d > 0, s->cursor_arpedit = AE_SLIDE;
            changed(s);
        }
    }
}

static void change(struct os *s, struct seq_arp_steps *p, int f, int d) {
    struct seq_arp_step *st = &p->step[s->arpedit_step];
    switch (f) {
    case AE_STEP: s->arpedit_step = clampi(s->arpedit_step + d, 0, p->len - 1); return;
    case AE_LEN:
        p->len = clampi(p->len + d, 1, ARP_STEPS);
        if (s->arpedit_step >= p->len) s->arpedit_step = p->len - 1;
        break;
    case AE_NOTE: st->note = (signed char)clampi(st->note + d, 0, 11), st->time = ARP_NOTE; break;
    case AE_OCT: st->oct = (signed char)clampi(st->oct + d, -1, 1); break;
    case AE_TIME: st->time = (unsigned char)clampi(st->time + d, ARP_NOTE, ARP_REST); break;
    case AE_ACCENT: st->accent = d > 0; break;
    case AE_SLIDE: st->slide = d > 0; break;
    }
    changed(s);
}

void arpedit_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    if (s->arpedit_ask) { // clearing the pattern
        if (input_down(in, IN_B1)) s->arpedit_ask = 0;
        else if (input_down(in, IN_B3)) {
            pthread_mutex_lock(&sq->lock);
            seq_arp_steps_clear(pattern(s));
            pthread_mutex_unlock(&sq->lock);
            s->arpedit_ask = 0, s->arpedit_step = 0;
            changed(s);
        }
    } else if (input_down(in, IN_B1) && in->shift) {
        s->arpedit_ask = 1;
    } else if (input_down(in, IN_B1)) { // <Back>: to the Arpeggiator page
        preview(s, 0);
        s->screen = OS_REPEAT;
        ui_wake(u, 1);
        return;
    } else if (input_down(in, IN_B2) && in->shift) { // <Play>/<Stop>
        preview(s, !s->arpedit_play);
    } else if (input_down(in, IN_B2) || input_down(in, IN_B3)) { // <Rest>, <Tie>: entered, and on
        pthread_mutex_lock(&sq->lock);
        pattern(s)->step[s->arpedit_step].time = input_down(in, IN_B2) ? ARP_REST : ARP_TIE;
        next_step(s);
        pthread_mutex_unlock(&sq->lock);
        changed(s);
    }
    pthread_mutex_lock(&sq->lock);
    struct seq_arp_steps *p = pattern(s);
    if (!s->arpedit_ask) {
        if (in->encoder_press) s->cursor_arpedit = (s->cursor_arpedit + (in->shift ? AE_FIELDS - 1 : 1)) % AE_FIELDS;
        int d = in->encoder + (in->shift ? 0 : input_down(in, IN_PLUS) - input_down(in, IN_MINUS));
        if (d) change(s, p, s->cursor_arpedit, d);
        knobs(s, p, in);
    }
    if (s->arpedit_step >= p->len) s->arpedit_step = p->len - 1;

    char line[LCD_COLS + 1], v[12];
    if (s->arpedit_ask) {
        lcd_text(u, 0, KEY_ROW, "<Cancel>");
        lcd_text(u, 19, KEY_ROW, "<Clear>");
    } else if (in->shift) {
        lcd_text(u, 0, KEY_ROW, "<Clear>");
        lcd_text(u, 9, KEY_ROW, s->arpedit_play ? "<Stop>" : "<Play>");
    } else {
        lcd_text(u, 0, KEY_ROW, "<Back>");
        lcd_text(u, 9, KEY_ROW, "<Rest>");
        lcd_text(u, 20, KEY_ROW, "<Tie>");
    }
    snprintf(line, sizeof line, "Pattern Edit: USER %d", s->arpedit_slot + 1);
    lcd_rule(u, TITLE_ROW, line);
    // The steps, eight to a row: the note, and under it octave, accent and slide.
    for (int half = 0; half < 2; half++)
        for (int k = 0; k < 8; k++) {
            int i = half * 8 + k, col = 1 + k * 3, row = 4 + half * 3;
            const struct seq_arp_step *st = &p->step[i];
            char top[4], low[4];
            if (i >= p->len) snprintf(top, sizeof top, " . "), snprintf(low, sizeof low, "   ");
            else {
                if (st->time == ARP_REST) snprintf(top, sizeof top, "-- ");
                else if (st->time == ARP_TIE) snprintf(top, sizeof top, "~~ ");
                else snprintf(top, sizeof top, "%-2s ", NOTES[st->note % 12]);
                snprintf(low, sizeof low, "%c%c%c", st->time != ARP_NOTE ? ' ' : st->oct > 0 ? '+' : st->oct < 0 ? '-' : ' ',
                         st->accent ? 'A' : ' ', st->slide ? 'S' : ' ');
            }
            if (i == s->arpedit_step) lcd_inverse(u, col, row, top);
            else lcd_text(u, col, row, top);
            lcd_text(u, col, row + 1, low);
        }
    // The selected step in full, and the page's own settings.
    const struct seq_arp_step *st = &p->step[s->arpedit_step];
    struct {
        int row, col;
        const char *label;
    } F[AE_FIELDS] = {{10, 0, "Step:"}, {10, 9, "Len:"}, {11, 0, "Note:"}, {11, 9, "Oct:"},
                      {11, 16, "Time:"}, {12, 0, "Accent:"}, {12, 12, "Slide:"}};
    for (int f = 0; f < AE_FIELDS; f++) {
        switch (f) {
        case AE_STEP: snprintf(v, sizeof v, "%02d", s->arpedit_step + 1); break;
        case AE_LEN: snprintf(v, sizeof v, "%02d", p->len); break;
        case AE_NOTE: snprintf(v, sizeof v, "%-2s", NOTES[st->note % 12]); break;
        case AE_OCT: snprintf(v, sizeof v, "%+d", st->oct); break;
        case AE_TIME: snprintf(v, sizeof v, "%s", TIMES[st->time % 3]); break;
        case AE_ACCENT: snprintf(v, sizeof v, "%s", st->accent ? "ON" : "OFF"); break;
        case AE_SLIDE: snprintf(v, sizeof v, "%s", st->slide ? "ON" : "OFF"); break;
        }
        int col = F[f].col + (int)strlen(F[f].label);
        lcd_text(u, F[f].col, F[f].row, F[f].label);
        if (f == s->cursor_arpedit && !s->arpedit_ask) lcd_inverse(u, col, F[f].row, v);
        else lcd_text(u, col, F[f].row, v);
    }
    if (s->arpedit_ask) {
        snprintf(line, sizeof line, "Clear USER %d?", s->arpedit_slot + 1);
        lcd_text(u, 0, 13, line);
    } else {
        lcd_text(u, 0, 9, "1-12:C-B 13- 14+ 15Ac 16Sl");
        lcd_text(u, 0, 13, "K1 Step K2 Accent K3 Slide");
    }
    lcd_rule(u, BOTTOM_ROW, 0);
    if (s->arpedit_play) ui_wake(u, 100);
    pthread_mutex_unlock(&sq->lock);
}
