#include "seq_screen.h"
#include "lcd.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAP_RESET_MS 2000

enum {
    F_SEQ, F_BPM, F_SIG_NUM, F_SIG_DEN, F_BARS, F_LOOP,
    F_TRK, F_TYPE, F_ON, F_CHN, F_VEL, F_PGM,
    F_TC_NOTE, F_TC_SWING, F_TC_SHIFT, F_TC_AMOUNT, F_TC_TRACK,
    F_FROM_BAR, F_FROM_BEAT, F_FROM_TICK, F_TO_BAR, F_TO_BEAT, F_TO_TICK, F_TC_NOTES,
    F_CLICK, F_INPLAY, F_CVOL, F_RATE, F_CAUDIO, F_CLOOP, F_CIN, F_CTYPE, F_CBARS, F_KIT, F_PSND, F_PTYPE, F_PINST,
    F_REP_TYPE, F_REP_RATE, F_ARP_PAT, F_REP_KEYS, F_ARP_CAT, F_ARP_OCT, F_ARP_GATE, F_ARP_LATCH, F_ARP_TRANS,
    F_E_SEQ, F_E_TRK, F_E_FROM_BAR, F_E_FROM_BEAT, F_E_FROM_TICK, F_E_TO_BAR, F_E_TO_BEAT, F_E_TO_TICK, F_E_NOTES,
};

struct field {
    int id, row, col;
};

struct label {
    int row, col;
    const char *text;
};

// Play/Record and Track Data share one page, in reading order. SEQ jumps the cursor
// between the first field of each section.
static const struct field MAIN_FIELDS[] = {
    {F_SEQ, 3, 4}, {F_BPM, 4, 4}, {F_BARS, 4, 21}, {F_SIG_NUM, 5, 4}, {F_SIG_DEN, 5, 7}, {F_LOOP, 5, 15},
    {F_TRK, 7, 4}, {F_TYPE, 8, 5}, {F_ON, 8, 15}, {F_CHN, 9, 4}, {F_VEL, 9, 13}, {F_PGM, 9, 21},
};
#define MAIN_TRACK_FIELD 6
#define MAIN_PGM_FIELD 11
static const struct label MAIN_LABELS[] = {
    {3, 0, "Seq:"}, {4, 0, "BPM:"}, {4, 10, "(SEQ)"}, {4, 16, "Bars:"}, {5, 0, "Sig:"}, {5, 6, "/"},
    {5, 10, "Loop:"}, {7, 0, "Trk:"}, {8, 0, "Type:"}, {8, 12, "On:"}, {9, 0, "Chn:"}, {9, 8, "Vel%:"},
    {9, 17, "Pgm:"},
};
static const struct field TC_FIELDS[] = {
    {F_TC_NOTE, 3, 11}, {F_TC_SWING, 4, 7}, {F_TC_SHIFT, 5, 13}, {F_TC_AMOUNT, 6, 13}, {F_TC_TRACK, 8, 6},
    {F_FROM_BAR, 9, 6}, {F_FROM_BEAT, 9, 10}, {F_FROM_TICK, 9, 13},
    {F_TO_BAR, 9, 16}, {F_TO_BEAT, 9, 20}, {F_TO_TICK, 9, 23}, {F_TC_NOTES, 10, 6},
};
static const struct label TC_LABELS[] = {
    {3, 0, "Note value:"}, {5, 0, "Shift timing:"}, {6, 0, "Shift amount:"}, {8, 0, "Track:"},
    {9, 0, "Ticks:"}, {9, 9, "."}, {9, 12, "."}, {9, 15, "-"}, {9, 19, "."}, {9, 22, "."},
    {10, 0, "Notes:"},
};

static const struct field METRO_FIELDS[] = {
    {F_CLICK, 3, 7}, {F_INPLAY, 4, 9}, {F_CVOL, 5, 8}, {F_RATE, 6, 6}, {F_CAUDIO, 7, 11},
    {F_CLOOP, 8, 12}, {F_CIN, 10, 10}, {F_CTYPE, 11, 6}, {F_CBARS, 12, 6},
};
static const struct label METRO_LABELS[] = {
    {3, 0, "Click:"}, {4, 0, "In Play:"}, {5, 0, "Volume:"}, {6, 0, "Rate:"}, {7, 0, "Audio Rec:"},
    {8, 0, "After Loop:"}, {10, 0, "Count-in:"}, {11, 0, "Type:"}, {12, 0, "Bars:"},
};
static const char *const RATE_NAMES[CLICK_RATES] = {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const char *const REP_NAMES[REP_RATES] = {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"};
static const struct field REPEAT_FIELDS[] = {{F_REP_TYPE, 3, 6}, {F_REP_RATE, 4, 6}, {F_REP_KEYS, 5, 8}};
static const struct label REPEAT_LABELS[] = {{3, 0, "Type:"}, {4, 0, "Rate:"}, {5, 0, "B Keys:"}};
// The arpeggiator's page: its pattern by category, and how it plays.
static const struct field ARP_FIELDS[] = {
    {F_REP_TYPE, 3, 6}, {F_REP_RATE, 4, 6}, {F_ARP_CAT, 5, 10}, {F_ARP_PAT, 6, 9},
    {F_ARP_OCT, 7, 9}, {F_ARP_GATE, 7, 18}, {F_ARP_LATCH, 8, 7}, {F_ARP_TRANS, 8, 19}, {F_REP_KEYS, 9, 8},
};
static const struct label ARP_LABELS[] = {
    {3, 0, "Type:"}, {4, 0, "Rate:"}, {5, 0, "Category:"}, {6, 0, "Pattern:"}, {7, 0, "Octaves:"}, {7, 13, "Gate:"},
    {8, 0, "Latch:"}, {8, 13, "Trans:"}, {9, 0, "B Keys:"},
};

static const char *const TC_NAMES[TC_COUNT];
const char *seq_tc_name(int tc) {
    return tc >= 0 && tc < TC_COUNT ? TC_NAMES[tc] : "";
}
static const char *const TC_NAMES[TC_COUNT] = {"1/4 NOTE", "1/4 TRPLT", "1/8 NOTE", "1/8 TRPLT", "1/16 NOTE", "1/16 TRPLT",
                                               "1/32 NOTE", "1/32 TRPLT", "1/64 NOTE", "1/64 TRPLT", "OFF"};

#define COUNT(a) ((int)(sizeof a / sizeof a[0]))

static int64_t now_ms(const struct ui *u) {
    if (u->now_ms) return u->now_ms;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static struct seq_sequence *current(struct os *s) {
    return &s->seq.sequence[s->seq.current];
}

// Splits a tick count into bar, beat and tick, all counted the MPC way (bar and beat from 1).
static void split(const struct seq_sequence *q, long ticks, int *bar, int *beat, int *tick) {
    if (ticks < 0) ticks = 0;
    int b = seq_bar_ticks(q), t = seq_beat_ticks(q);
    *bar = (int)(ticks / b) + 1;
    *beat = (int)(ticks % b / t) + 1;
    *tick = (int)(ticks % t);
}

static int swing_shown(const struct os *s) {
    return s->seq.tc == TC_8 || s->seq.tc == TC_16 || s->seq.tc == TC_32;
}

static int field_shown(const struct os *s, int id) {
    (void)s;
    return id != F_TC_SWING || swing_shown(s);
}

static void track_label(const struct seq_track *k, int index, char *b, size_t n) {
    snprintf(b, n, "%2d-%.16s", index + 1, seq_track_used(k) ? k->name : "(unused)");
}

// Picked notes as the first two, then how many more: A01 A03 +2. None picked is ALL.
static void notes_label(const unsigned char *notes, int count, int midi, char *b, size_t n) {
    if (!count) {
        snprintf(b, n, "ALL");
        return;
    }
    int shown = 0;
    size_t used = 0;
    b[0] = 0;
    for (int note = 0; note < 128 && shown < 2; note++) {
        if (!notes[note]) continue;
        char one[8];
        if (midi) snprintf(one, sizeof one, "%d", note);
        else snprintf(one, sizeof one, "%c%02d", 'A' + note / 16, note % 16 + 1);
        used += (size_t)snprintf(b + used, n - used, "%s%s", shown ? " " : "", one);
        shown++;
    }
    if (count > shown) snprintf(b + used, n - used, " +%d", count - shown);
}

static void tick_part(const struct seq_sequence *q, uint32_t at, int part, char *b, size_t n) {
    int bar, beat, tick;
    split(q, (long)at, &bar, &beat, &tick);
    if (part == 0) snprintf(b, n, "%03d", bar);
    else snprintf(b, n, "%02d", part == 1 ? beat : tick);
}

// Caller holds the sequencer lock.
static void value(struct os *s, int id, char *b, size_t n) {
    struct seq_sequence *q = current(s);
    struct seq_track *k = &q->track[s->seq.track];
    int bar, beat, tick;
    switch (id) {
    case F_SEQ:
        snprintf(b, n, "%2d-%.16s", s->seq.current + 1, seq_used(q) ? q->name : "(unused)");
        break;
    case F_BPM: snprintf(b, n, "%3d.%d", q->tempo10 / 10, q->tempo10 % 10); break;
    case F_SIG_NUM: snprintf(b, n, "%2d", q->sig_num); break;
    case F_SIG_DEN: snprintf(b, n, "%2d", q->sig_den); break;
    case F_BARS: snprintf(b, n, "%3d", q->bars); break;
    case F_LOOP:
        if (q->loop_bar) snprintf(b, n, "TO BAR %3d", q->loop_bar);
        else snprintf(b, n, "OFF");
        break;
    case F_TRK: track_label(k, s->seq.track, b, n); break;
    case F_TYPE: snprintf(b, n, "%s", k->type == TRACK_MIDI ? "MIDI" : "DRUM"); break;
    case F_ON: snprintf(b, n, "%s", k->on ? "YES" : "NO"); break;
    case F_CHN:
        if (k->channel) snprintf(b, n, "%2d", k->channel);
        else snprintf(b, n, "OFF");
        break;
    case F_VEL: snprintf(b, n, "%3d", k->vel_pct); break;
    case F_PGM: // a MIDI track sends a program change; the others play one of the pad programs
        if (k->type != TRACK_MIDI) snprintf(b, n, "%d", k->kit + 1);
        else if (k->program) snprintf(b, n, "%3d", k->program);
        else snprintf(b, n, "OFF");
        break;
    case F_KIT: snprintf(b, n, "%2d-%.16s", k->kit + 1, s->programs[k->kit].name); break;
    case F_PTYPE: {
        static const char *const TYPES[PROG_TYPES] = {"MPC", "S600", "S600 MULTI"};
        snprintf(b, n, "%s", TYPES[s->programs[k->kit].s600 % PROG_TYPES]);
        break;
    }
    case F_PINST: // the pad MIDI keys play: S600 and S600 MULTI
        if (!s->programs[k->kit].s600) snprintf(b, n, "--");
        else if (s->programs[k->kit].inst < 0) snprintf(b, n, "LAST PAD");
        else snprintf(b, n, "%c%02d", 'A' + s->programs[k->kit].inst / 16, s->programs[k->kit].inst % 16 + 1);
        break;
    case F_PSND: {
        const struct os_pad *p = &s->pads[s->selected];
        snprintf(b, n, "%.15s", p->s ? p->name : "(empty)");
        break;
    }
    case F_TC_NOTE: snprintf(b, n, "%s", TC_NAMES[s->seq.tc]); break;
    case F_TC_SWING: snprintf(b, n, "%d", s->seq.swing); break;
    case F_TC_SHIFT: snprintf(b, n, "%s", s->seq.shift_later ? "LATER" : "EARLIER"); break;
    case F_TC_AMOUNT: snprintf(b, n, "%2d", s->seq.shift_ticks); break;
    case F_TC_TRACK: track_label(&q->track[s->tc_track], s->tc_track, b, n); break;
    case F_FROM_BAR: case F_FROM_BEAT: case F_FROM_TICK:
    case F_TO_BAR: case F_TO_BEAT: case F_TO_TICK: {
        int from = id <= F_FROM_TICK;
        split(q, from ? (long)s->tc_from : (long)s->tc_to, &bar, &beat, &tick);
        int part = from ? id - F_FROM_BAR : id - F_TO_BAR;
        if (part == 0) snprintf(b, n, "%03d", bar);
        else snprintf(b, n, "%02d", part == 1 ? beat : tick);
        break;
    }
    case F_CLICK: snprintf(b, n, "%s", s->seq.metronome != METRO_OFF ? "ON" : "OFF"); break;
    case F_INPLAY:
        snprintf(b, n, "%s", (s->seq.metronome == METRO_OFF ? s->metro_on_mode : s->seq.metronome) == METRO_ON ? "YES" : "NO");
        break;
    case F_CVOL: snprintf(b, n, "%3d", s->seq.click_volume); break;
    case F_CAUDIO: snprintf(b, n, "%s", s->seq.click_audio ? "YES" : "NO"); break;
    case F_CLOOP: snprintf(b, n, "%s", s->seq.click_loop ? "CLICK" : "STOP"); break;
    case F_RATE: snprintf(b, n, "%s", RATE_NAMES[s->seq.click_rate]); break;
    case F_REP_RATE: snprintf(b, n, "%s", REP_NAMES[s->seq.rep_rate]); break;
    case F_REP_KEYS: snprintf(b, n, "%s", s->rep_toggle ? "TOGGLE" : "HOLD"); break;
    case F_REP_TYPE: snprintf(b, n, "%s", s->seq.arp ? "ARPEGGIATOR" : "NOTE REPEAT"); break;
    case F_ARP_PAT: snprintf(b, n, "%s", seq_arp_name(s->seq.arp_pattern)); break;
    case F_ARP_CAT: snprintf(b, n, "%s", seq_arp_group_name(seq_arp_group(s->seq.arp_pattern))); break;
    case F_ARP_OCT: snprintf(b, n, "%d", s->seq.arp_oct); break;
    case F_ARP_GATE: snprintf(b, n, "%d%%", s->seq.arp_gate); break;
    case F_ARP_LATCH: snprintf(b, n, "%s", s->seq.arp_latch ? "ON" : "OFF"); break;
    case F_ARP_TRANS: snprintf(b, n, "%+d", s->seq.arp_trans); break;
    case F_CIN: snprintf(b, n, "%s", s->seq.count_in ? "ON" : "OFF"); break;
    case F_CTYPE: snprintf(b, n, "%s", s->seq.count_wait ? "WAIT KEY" : "COUNT IN"); break;
    case F_CBARS: snprintf(b, n, "%d", s->seq.count_bars); break;
    case F_TC_NOTES:
        notes_label(s->tc_notes, s->tc_count, q->track[s->tc_track].type == TRACK_MIDI, b, n);
        break;
    case F_E_SEQ: {
        const struct seq_sequence *eq = &s->seq.sequence[s->erase_seq];
        snprintf(b, n, "%2d-%.16s", s->erase_seq + 1, seq_used(eq) ? eq->name : "(unused)");
        break;
    }
    case F_E_TRK:
        if (!s->erase_track) snprintf(b, n, " 0-ALL TRACKS");
        else track_label(&s->seq.sequence[s->erase_seq].track[s->erase_track - 1], s->erase_track - 1, b, n);
        break;
    case F_E_FROM_BAR: case F_E_FROM_BEAT: case F_E_FROM_TICK:
        tick_part(&s->seq.sequence[s->erase_seq], s->erase_from, id - F_E_FROM_BAR, b, n);
        break;
    case F_E_TO_BAR: case F_E_TO_BEAT: case F_E_TO_TICK:
        tick_part(&s->seq.sequence[s->erase_seq], s->erase_to, id - F_E_TO_BAR, b, n);
        break;
    case F_E_NOTES:
        notes_label(s->erase_notes, s->erase_count, k->type == TRACK_MIDI, b, n);
        break;
    default: b[0] = 0; break;
    }
}

static uint32_t move_ticks(const struct seq_sequence *q, uint32_t at, int part, int d) {
    long step = part == 0 ? seq_bar_ticks(q) : part == 1 ? seq_beat_ticks(q) : 1;
    long v = (long)at + d * step;
    long length = seq_length(q);
    return (uint32_t)(v < 0 ? 0 : v > length ? length : v);
}

// Caller holds the sequencer lock.
static void change(struct os *s, int id, int d, int fine) {
    struct seq *sq = &s->seq;
    struct seq_sequence *q = current(s);
    struct seq_track *k = &q->track[sq->track];
    switch (id) {
    case F_SEQ: {
        int target = clampi((sq->next >= 0 ? sq->next : sq->current) + d, 0, SEQ_COUNT - 1);
        // While playing, the next sequence waits for the end of this one, as on the MPC3000.
        if (sq->mode == SEQ_STOPPED) sq->current = target, sq->pos = 0;
        else sq->next = target == sq->current ? -1 : target;
        break;
    }
    case F_BPM: q->tempo10 = clampi(q->tempo10 + d * (fine ? 1 : 10), 300, 3000); break;
    case F_SIG_NUM: q->sig_num = clampi(q->sig_num + d, 1, 16); break;
    case F_SIG_DEN: {
        static const int dens[] = {4, 8, 16};
        int i = q->sig_den == 16 ? 2 : q->sig_den == 8 ? 1 : 0;
        q->sig_den = dens[clampi(i + d, 0, 2)];
        break;
    }
    case F_BARS: { // with notes to copy or to lose, it asks first
        int to = clampi(q->bars + d, 1, SEQ_MAX_BARS);
        uint32_t bar = (uint32_t)seq_bar_ticks(q);
        if ((to > q->bars && seq_used(q)) || (to < q->bars && seq_events_from(q, (uint32_t)to * bar))) {
            s->seq_ask = SEQ_ASK_BARS;
            s->ask_bars = to;
        } else {
            q->bars = to;
            if (q->loop_bar > q->bars) q->loop_bar = q->bars;
        }
        break;
    }
    case F_LOOP: q->loop_bar = clampi(q->loop_bar + d, 0, q->bars); break;
    case F_TRK: sq->track = clampi(sq->track + d, 0, SEQ_TRACKS - 1); break;
    case F_TYPE: k->type = k->type == TRACK_MIDI ? TRACK_DRUM : TRACK_MIDI; break;
    case F_ON: k->on = !k->on; break;
    case F_CHN: k->channel = clampi(k->channel + d, 0, 16); break;
    case F_VEL: k->vel_pct = clampi(k->vel_pct + d, 1, 200); break;
    case F_PGM:
        if (k->type != TRACK_MIDI) k->kit = clampi(k->kit + d, 0, OS_PROGRAMS - 1);
        else k->program = clampi(k->program + d, 0, 128);
        break;
    case F_KIT: k->kit = clampi(k->kit + d, 0, OS_PROGRAMS - 1); break;
    case F_PTYPE: { // the lock is held here: set directly, the key maps after (they take no lock)
        int t = clampi(s->programs[k->kit].s600 + d, 0, PROG_TYPES - 1);
        s->programs[k->kit].s600 = t;
        sq->kit_s600[k->kit] = (unsigned char)(t != 0);
        if (t == PROG_MULTI && s->programs[k->kit].inst < 0) s->programs[k->kit].inst = 0; // MULTI: a pad, not LAST PAD
        s->prog_refresh = k->kit + 1;
        break;
    }
    case F_PINST:
        if (s->programs[k->kit].s600) {
            int lo = s->programs[k->kit].s600 == PROG_MULTI ? 0 : -1;
            s->prog_inst = clampi(s->programs[k->kit].inst + d, lo, OS_PADS - 1) + 2; // set once the lock is let go
            s->prog_refresh = k->kit + 1;
        }
        break;
    case F_PSND: // the lock is held here, and assigning takes none of it
        os_pad_step_sound(s, s->selected, d);
        break;
    case F_TC_NOTE: sq->tc = clampi(sq->tc + d, 0, TC_COUNT - 1); break;
    case F_TC_SWING: sq->swing = seq_swing_step(sq, sq->swing, d); break;
    case F_TC_SHIFT: sq->shift_later = !sq->shift_later; break;
    case F_TC_AMOUNT: sq->shift_ticks = clampi(sq->shift_ticks + d, 0, SEQ_PPQ / 2 - 1); break;
    case F_TC_TRACK: s->tc_track = clampi(s->tc_track + d, 0, SEQ_TRACKS - 1); break;
    case F_FROM_BAR: case F_FROM_BEAT: case F_FROM_TICK:
        s->tc_from = move_ticks(q, s->tc_from, id - F_FROM_BAR, d);
        break;
    case F_TO_BAR: case F_TO_BEAT: case F_TO_TICK:
        s->tc_to = move_ticks(q, s->tc_to, id - F_TO_BAR, d);
        break;
    case F_TC_NOTES: // turning the wheel here clears the pads back to ALL
        memset(s->tc_notes, 0, sizeof s->tc_notes);
        s->tc_count = 0;
        break;
    case F_E_SEQ: // a new sequence: the range covers all of it again
        s->erase_seq = clampi(s->erase_seq + d, 0, SEQ_COUNT - 1);
        s->erase_from = 0;
        s->erase_to = (uint32_t)seq_length(&sq->sequence[s->erase_seq]);
        break;
    case F_E_TRK: s->erase_track = clampi(s->erase_track + d, 0, SEQ_TRACKS); break;
    case F_E_FROM_BAR: case F_E_FROM_BEAT: case F_E_FROM_TICK:
        s->erase_from = move_ticks(&sq->sequence[s->erase_seq], s->erase_from, id - F_E_FROM_BAR, d);
        break;
    case F_E_TO_BAR: case F_E_TO_BEAT: case F_E_TO_TICK:
        s->erase_to = move_ticks(&sq->sequence[s->erase_seq], s->erase_to, id - F_E_TO_BAR, d);
        break;
    case F_E_NOTES:
        memset(s->erase_notes, 0, sizeof s->erase_notes);
        s->erase_count = 0;
        break;
    case F_CLICK:
        if (sq->metronome == METRO_OFF) {
            sq->metronome = s->metro_on_mode;
        } else {
            s->metro_on_mode = sq->metronome;
            sq->metronome = METRO_OFF;
        }
        break;
    case F_CAUDIO: sq->click_audio = d > 0; break;
    case F_CLOOP: sq->click_loop = d > 0; break;
    case F_INPLAY:
        s->metro_on_mode = (sq->metronome == METRO_OFF ? s->metro_on_mode : sq->metronome) == METRO_ON ? METRO_REC : METRO_ON;
        if (sq->metronome != METRO_OFF) sq->metronome = s->metro_on_mode;
        break;
    case F_CVOL: sq->click_volume = clampi(sq->click_volume + d, 0, 100); break;
    case F_RATE: sq->click_rate = clampi(sq->click_rate + d, 0, CLICK_RATES - 1); break;
    case F_REP_RATE: sq->rep_rate = clampi(sq->rep_rate + d, 0, REP_RATES - 1); break;
    case F_REP_KEYS: s->rep_toggle = d > 0; if (!s->rep_toggle) sq->rep_alt = -1; break;
    case F_REP_TYPE: sq->arp = d > 0, s->cursor_repeat = 0; break;
    case F_ARP_PAT: case F_ARP_CAT: { // a pattern comes with its own rate and gate, changed after if wanted
        int to = id == F_ARP_PAT ? seq_arp_step(sq->arp_pattern, d)
                                 : seq_arp_first(clampi(seq_arp_group(sq->arp_pattern) + d, 0, ARP_GROUPS - 1));
        if (to != sq->arp_pattern) {
            sq->arp_pattern = to, sq->rep_rate = seq_arp_rate(to), sq->rep_alt = -1;
            sq->arp_gate = seq_arp_is_riff(to) ? 80 : 50;
        }
        break;
    }
    case F_ARP_OCT: sq->arp_oct = clampi(sq->arp_oct + d, 1, 4); break;
    case F_ARP_GATE: sq->arp_gate = clampi(sq->arp_gate + d * 5, 10, 100); break;
    case F_ARP_LATCH: // set under the lock it already holds: the notes let go when it goes off
        sq->arp_latch = d > 0;
        if (!sq->arp_latch)
            for (int i = 0; i < 16; i++)
                if (!sq->rep[i].held) sq->rep[i].on = 0;
        break;
    case F_ARP_TRANS: sq->arp_trans = clampi(sq->arp_trans + d, -12, 12); break;
    case F_CIN: sq->count_in = !sq->count_in; break;
    case F_CTYPE: sq->count_wait = !sq->count_wait; break;
    case F_CBARS: sq->count_bars = clampi(sq->count_bars + d, 1, SEQ_COUNT_BARS_MAX); break;
    }
    if (sq->tc != TC_OFF) s->tc_last = sq->tc;
}

static void draw_fields(struct os *s, struct ui *u, const struct field *f, int n, int cursor) {
    for (int i = 0; i < n; i++) {
        if (!field_shown(s, f[i].id)) continue;
        char b[LCD_COLS + 1];
        value(s, f[i].id, b, sizeof b);
        if (i == cursor) lcd_inverse(u, f[i].col, f[i].row, b);
        else lcd_text(u, f[i].col, f[i].row, b);
    }
}

static void draw_labels(struct ui *u, const struct label *l, int n) {
    for (int i = 0; i < n; i++) lcd_text(u, l[i].col, l[i].row, l[i].text);
}

static void move_cursor(struct os *s, const struct field *f, int n, int *cursor, int dir) {
    for (int i = 0; i < n; i++) {
        *cursor = (*cursor + dir + n) % n;
        if (field_shown(s, f[*cursor].id)) return;
    }
}

// The wheel, its push and -/+ on a page of fields. Caller holds the lock.
static void edit_fields(struct os *s, const struct input *in, const struct field *f, int n, int *cursor) {
    if (!field_shown(s, f[*cursor].id)) move_cursor(s, f, n, cursor, 1);
    if (in->encoder_press) move_cursor(s, f, n, cursor, in->shift ? -1 : 1);
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d) change(s, f[*cursor].id, d, in->shift);
}

static const char *status_text(const struct seq *sq) {
    switch (sq->mode) {
    case SEQ_PLAYING: return "PLAYING";
    case SEQ_COUNT_IN: return "COUNT IN";
    case SEQ_WAITING: return "WAITING FOR KEY";
    case SEQ_RECORDING: return "RECORDING";
    case SEQ_OVERDUB: return "OVERDUB";
    default:
        return sq->rec_arm == REC_ARM_REC ? "REC READY" : sq->rec_arm == REC_ARM_OVERDUB ? "OVERDUB READY" : "STOPPED";
    }
}

// ===== Now:001.01.00 ===== and the running time under it. Caller holds the lock.
static void draw_now(struct os *s, struct ui *u) {
    const struct seq_sequence *q = current(s);
    int bar, beat, tick;
    split(q, (long)s->seq.pos, &bar, &beat, &tick);
    char line[32];
    snprintf(line, sizeof line, "Now:%03d.%02d.%02d", bar, beat, tick);
    lcd_rule(u, 10, line); // right under the track data
    double seconds = s->seq.pos > 0 ? s->seq.pos / SEQ_PPQ * 600.0 / q->tempo10 : 0;
    long frames = (long)(seconds * 30.0); // 30 frames per second, as on the MPC3000's clock
    snprintf(line, sizeof line, "(%02ld:%02ld:%02ld:%02ld)", frames / 108000 % 100, frames / 1800 % 60, frames / 30 % 60,
             frames % 30);
    if (s->seq.next >= 0) snprintf(line, sizeof line, "Next: %d", s->seq.next + 1); // a queued sequence instead
    lcd_center(u, 11, line);
    lcd_rule(u, 12, 0);
    // What the knobs do on this page, under them.
    lcd_text(u, 0, 13, "<Track>");
    lcd_text(u, 9, 13, "<BPM>");
    lcd_text(u, 18, 13, "<Pgm>");
}

// Where a knob sits for a value, and the value for where it sits.
static int knob_at(int v, int lo, int hi) {
    return hi > lo ? (v - lo) * 127 / (hi - lo) : 0;
}

static int knob_value(int k, int lo, int hi) {
    return lo + (hi - lo) * k / 127;
}

// A knob over a value from lo to hi, taking over once it reaches the value (or crosses it).
// The value it now sets, or v while it is still catching up.
static int knob_follow(struct os *s, const struct input *in, int k, int v, int lo, int hi) {
    if (!in->knob_moved[k]) return v;
    int at = knob_at(v, lo, hi), last = s->knob_last[k], now = in->knob[k];
    s->knob_last[k] = now;
    int pos = os_knob_take(s, &s->knob_caught[k], last, now, at);
    return pos >= 0 ? knob_value(pos, lo, hi) : v;
}

// K1 the track, K2 the BPM (whole), K3 the program; each takes over once it reaches the value.
// Caller holds the lock.
static void main_knobs(struct os *s, const struct input *in) {
    struct seq *sq = &s->seq;
    for (int k = 0; k < 3; k++) {
        if (!in->knob_moved[k]) continue;
        struct seq_sequence *q = current(s);
        struct seq_track *t = &q->track[sq->track];
        int midi = t->type == TRACK_MIDI, at;
        if (k == 0) at = knob_at(sq->track, 0, SEQ_TRACKS - 1);
        else if (k == 1) at = knob_at(q->tempo10, 300, 3000);
        else at = midi ? knob_at(t->program, 0, 128) : knob_at(t->kit, 0, OS_PROGRAMS - 1);
        int last = s->knob_last[k], now = in->knob[k];
        s->knob_last[k] = now;
        int pos = os_knob_take(s, &s->knob_caught[k], last, now, at);
        if (pos < 0) continue;
        if (k == 0) {
            sq->track = knob_value(pos, 0, SEQ_TRACKS - 1);
            s->knob_caught[2] = 0; // another track's program: K3 catches up to it again
            s->cursor_play = MAIN_TRACK_FIELD;
        } else if (k == 1) {
            q->tempo10 = knob_value(pos, 300, 3000) / 10 * 10;
            s->cursor_play = 1;
        } else {
            if (midi) t->program = knob_value(pos, 0, 128);
            else t->kit = knob_value(pos, 0, OS_PROGRAMS - 1);
            s->cursor_play = MAIN_PGM_FIELD;
        }
    }
}

static void ask_line(struct ui *u, int row, const char *text) {
    char b[LCD_COLS + 1];
    snprintf(b, sizeof b, " %-24.24s ", text);
    lcd_inverse(u, 0, row, b);
}

// The question about the whole sequence: its keys and the wheel, then drawn over the page's
// foot. 1 while it is up. Called without the lock.
static int seq_question(struct os *s, struct ui *u, const struct input *in) {
    if (s->seq_ask == SEQ_ASK_NONE) return 0;
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    struct seq_sequence *q = current(s);
    int bars = q->bars, seqn = sq->current;
    pthread_mutex_unlock(&sq->lock);
    char msg[48];
    if (s->seq_ask == SEQ_ASK_BARS) {
        int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
        if (d) s->ask_bars = clampi(s->ask_bars + d, 1, SEQ_MAX_BARS); // the wheel still sets how many
        if (s->ask_bars == bars) {
            s->seq_ask = SEQ_ASK_NONE;
            return 0;
        }
        int grow = s->ask_bars > bars;
        if (input_down(in, IN_B1)) s->seq_ask = SEQ_ASK_NONE; // <Cancel>, or <No> to deleting
        else if (input_down(in, IN_B2) && grow) { // <No>: new bars, empty
            os_undo_save(s, "Bars");
            seq_set_bars(sq, seqn, s->ask_bars, 0);
            s->seq_ask = SEQ_ASK_NONE;
        } else if (input_down(in, IN_B3)) { // <Yes>
            os_undo_save(s, "Bars");
            if (seq_set_bars(sq, seqn, s->ask_bars, grow) != 0) os_toast(s, u, "Out of memory");
            s->seq_ask = SEQ_ASK_NONE;
        }
        if (s->seq_ask == SEQ_ASK_NONE) {
            ui_wake(u, 1);
            return 1;
        }
        lcd_fill(u, 0, KEY_ROW * CELL_H, UI_W, CELL_H, LCD_BG);
        if (grow) {
            lcd_text(u, 0, KEY_ROW, "<Cancel>");
            lcd_text(u, 11, KEY_ROW, "<No>");
            lcd_text(u, 20, KEY_ROW, "<Yes>");
            snprintf(msg, sizeof msg, "Bars %d to %d:", bars, s->ask_bars);
            ask_line(u, 11, msg);
            ask_line(u, 12, "Duplicate sequence data?");
            ask_line(u, 13, "");
        } else {
            lcd_text(u, 0, KEY_ROW, "<No>");
            lcd_text(u, 20, KEY_ROW, "<Yes>");
            if (s->ask_bars + 1 == bars) snprintf(msg, sizeof msg, "Delete bar %d?", bars);
            else snprintf(msg, sizeof msg, "Delete bars %d-%d?", s->ask_bars + 1, bars);
            ask_line(u, 11, msg);
            ask_line(u, 12, "");
            ask_line(u, 13, "");
        }
        return 1;
    }
    int twice = s->seq_ask == SEQ_ASK_BPM_DOUBLE;
    if (input_down(in, IN_B1)) s->seq_ask = SEQ_ASK_NONE; // <Cancel>
    else if (input_down(in, IN_B2) || input_down(in, IN_B3)) { // <No>: the BPM only. <Yes>: and the notes to match
        int comp = input_down(in, IN_B3);
        os_undo_save(s, twice ? "Double BPM" : "Half BPM");
        if (seq_tempo_scale(sq, seqn, twice, comp) != 0) os_toast(s, u, twice ? "Too fast or too long" : "BPM too slow");
        s->seq_ask = SEQ_ASK_NONE;
    }
    if (s->seq_ask == SEQ_ASK_NONE) {
        ui_wake(u, 1);
        return 1;
    }
    lcd_fill(u, 0, KEY_ROW * CELL_H, UI_W, CELL_H, LCD_BG);
    lcd_text(u, 0, KEY_ROW, "<Cancel>");
    lcd_text(u, 11, KEY_ROW, "<No>");
    lcd_text(u, 20, KEY_ROW, "<Yes>");
    ask_line(u, 11, twice ? "BPM will be doubled." : "BPM will be halved.");
    ask_line(u, 12, "Apply note-value");
    ask_line(u, 13, "compensation?");
    return 1;
}

void seq_go_main(struct os *s) {
    if (s->screen == OS_MAIN) { // Seq:, then Trk:, then Pgm:, then round again
        s->cursor_play = s->cursor_play < MAIN_TRACK_FIELD ? MAIN_TRACK_FIELD
                       : s->cursor_play < MAIN_PGM_FIELD ? MAIN_PGM_FIELD : 0;
    } else {
        s->screen = OS_MAIN;
        s->cursor_play = 0;
        for (int i = 0; i < 3; i++) s->knob_caught[i] = 0; // K1-K3 are the page's own now
    }
}

// ---- Renaming (Shift + B2 on the main page, the cursor on Seq, Trk or Pgm) ----

static const char NAME_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Shift + B2: the name of what the cursor is on, ready to change. 0 when there is nothing to name.
static int rename_start(struct os *s, struct ui *u) {
    struct seq *sq = &s->seq;
    const struct seq_sequence *q = &sq->sequence[sq->current];
    const struct seq_track *k = &q->track[sq->track];
    const char *name;
    int f = s->cursor_play;
    if (f == 0) s->rename_what = RENAME_SEQ, name = q->name;
    else if (f == MAIN_TRACK_FIELD) s->rename_what = RENAME_TRACK, name = k->name[0] ? k->name : "";
    else if (f == MAIN_PGM_FIELD && k->type != TRACK_MIDI) s->rename_what = RENAME_PROGRAM, name = s->programs[k->kit].name;
    else {
        os_toast(s, u, f == MAIN_PGM_FIELD ? "A MIDI program is a number" : "Cursor on Seq, Trk or Pgm");
        return 0;
    }
    snprintf(s->rename_buf, sizeof s->rename_buf, "%-16.16s", name);
    s->rename_pos = 0;
    return 1;
}

static void rename_finish(struct os *s, struct ui *u) {
    struct seq *sq = &s->seq;
    char name[17];
    snprintf(name, sizeof name, "%s", s->rename_buf);
    for (int l = (int)strlen(name); l > 0 && name[l - 1] == ' '; l--) name[l - 1] = 0;
    if (!name[0]) {
        os_toast(s, u, "Name it first");
        return;
    }
    struct seq_sequence *q = &sq->sequence[sq->current];
    struct seq_track *k = &q->track[sq->track];
    if (s->rename_what == RENAME_SEQ) snprintf(q->name, sizeof q->name, "%s", name);
    else if (s->rename_what == RENAME_TRACK) snprintf(k->name, sizeof k->name, "%s", name);
    else if (s->rename_what == RENAME_PROGRAM) snprintf(s->programs[k->kit].name, sizeof s->programs[k->kit].name, "%s", name);
    s->rename_what = RENAME_NONE;
    os_toast(s, u, "Renamed");
}

// While renaming: the wheel sets the letter, - and + move along the name, B1 cancels, B3 keeps it.
static void rename_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    if (input_down(in, IN_B1)) {
        s->rename_what = RENAME_NONE;
        return;
    }
    if (input_down(in, IN_B3)) {
        pthread_mutex_lock(&sq->lock);
        rename_finish(s, u);
        pthread_mutex_unlock(&sq->lock);
        if (s->rename_what == RENAME_NONE) return;
    }
    s->rename_pos = clampi(s->rename_pos + input_down(in, IN_PLUS) - input_down(in, IN_MINUS), 0, 15);
    if (in->encoder) {
        const char *at = strchr(NAME_CHARS, s->rename_buf[s->rename_pos]);
        int i = at ? (int)(at - NAME_CHARS) : 0, n = (int)strlen(NAME_CHARS);
        s->rename_buf[s->rename_pos] = NAME_CHARS[((i + in->encoder) % n + n) % n];
    }
    static const char *const WHAT[] = {"", "Sequence", "Track", "Program"};
    char title[32];
    pthread_mutex_lock(&sq->lock);
    int n = s->rename_what == RENAME_SEQ ? sq->current + 1 : s->rename_what == RENAME_TRACK ? sq->track + 1
          : sq->sequence[sq->current].track[sq->track].kit + 1;
    pthread_mutex_unlock(&sq->lock);
    snprintf(title, sizeof title, "Rename %s %02d", WHAT[s->rename_what], n);
    lcd_fill(u, LCD_X, 5 * CELL_H, UI_W - 2 * LCD_X, 6 * CELL_H, LCD_BG);
    lcd_rule(u, 5, title);
    lcd_text(u, 0, 7, "Name:");
    lcd_text(u, 6, 7, s->rename_buf);
    char one[2] = {s->rename_buf[s->rename_pos], 0};
    lcd_inverse(u, 6 + s->rename_pos, 7, one);
    lcd_text(u, 0, 9, "Wheel: letter  -/+: move");
    lcd_rule(u, 10, 0);
    lcd_fill(u, LCD_X, KEY_ROW * CELL_H, UI_W - 2 * LCD_X, CELL_H, LCD_BG);
    lcd_text(u, 0, KEY_ROW, "<Cancel>");
    lcd_text(u, 22, KEY_ROW, "<OK>");
}

void seq_screen_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    // A question up takes the keys and the wheel; the page shows under it.
    struct input quiet;
    const struct input *real = in;
    int asking = s->seq_ask != SEQ_ASK_NONE;
    if (asking) {
        quiet = *in;
        quiet.down = 0, quiet.encoder = 0, quiet.encoder_press = 0;
        in = &quiet;
    }
    pthread_mutex_lock(&sq->lock);
    struct seq_track *k = &current(s)->track[sq->track];
    if (input_down(in, IN_B2) && in->shift && !asking && !s->rename_what) rename_start(s, u); // Shift + B2: rename
    if (s->rename_what || (input_down(in, IN_B2) && in->shift)) { // renaming: the keys, wheel and knobs are the name's
        quiet = *in;
        quiet.down = 0, quiet.encoder = 0, quiet.encoder_press = 0, quiet.fader_moved = 0;
        for (int i = 0; i < 3; i++) quiet.knob_moved[i] = 0;
        in = &quiet;
    }
    if (input_down(in, IN_B3) && in->shift) { // Shift + B3: Copy Sequence
        pthread_mutex_unlock(&sq->lock);
        seq_copy_open(s);
        ui_wake(u, 1);
        return;
    }
    if (input_down(in, IN_B1)) k->on = !k->on;
    if (input_down(in, IN_B2)) sq->track = clampi(sq->track - 1, 0, SEQ_TRACKS - 1);
    if (input_down(in, IN_B3)) sq->track = clampi(sq->track + 1, 0, SEQ_TRACKS - 1);
    edit_fields(s, in, MAIN_FIELDS, COUNT(MAIN_FIELDS), &s->cursor_play);
    main_knobs(s, in);
    if (in->fader_moved) { // the fader sets BPM, Chn or Pgm when the cursor is on it, else the track
        struct seq_track *t = &current(s)->track[sq->track];
        int f = in->fader;
        switch (MAIN_FIELDS[s->cursor_play].id) {
        case F_BPM: current(s)->tempo10 = (300 + 2700 * f / 127) / 10 * 10; break; // whole BPM
        default: sq->track = f * (SEQ_TRACKS - 1) / 127; break;
        case F_CHN: t->channel = f * 16 / 127; break;
        case F_PGM:
            if (t->type != TRACK_MIDI) t->kit = f * (OS_PROGRAMS - 1) / 127;
            else t->program = f * 128 / 127;
            break;
        }
    }

    lcd_text(u, 0, KEY_ROW, "<Tk on/off>");
    lcd_text(u, 12, KEY_ROW, "<Trk->");
    lcd_text(u, 20, KEY_ROW, in->shift && !asking ? "<Copy>" : "<Trk+>");
    lcd_rule(u, TITLE_ROW, "Play/Record");
    lcd_rule(u, 6, "Track Data");
    draw_labels(u, MAIN_LABELS, COUNT(MAIN_LABELS));
    draw_fields(s, u, MAIN_FIELDS, COUNT(MAIN_FIELDS), s->cursor_play);
    draw_now(s, u);
    if (sq->mode != SEQ_STOPPED) ui_wake(u, 50);
    pthread_mutex_unlock(&sq->lock);
    // Asked before this frame: it takes this frame's keys. Asked just now (Bars:): drawn only.
    struct input none;
    input_begin(&none, 0);
    if (asking) seq_question(s, u, real);
    else if (s->seq_ask != SEQ_ASK_NONE) seq_question(s, u, &none);
    if (s->rename_what) rename_frame(s, u, real); // over the page
}

void seq_tc_open(struct os *s) {
    pthread_mutex_lock(&s->seq.lock);
    s->screen = OS_TC;
    s->tc_track = s->seq.track;
    s->tc_from = 0;
    s->tc_to = (uint32_t)seq_length(current(s));
    memset(s->tc_notes, 0, sizeof s->tc_notes);
    s->tc_count = 0;
    for (int i = 0; i < 3; i++) s->knob_caught[i] = 0; // K1-K3 are this page's now
    pthread_mutex_unlock(&s->seq.lock);
}

// K1 Swing (an SP12's six settings in SP12 mode), K2 Shift amount, K3 the Move Existing track;
// each takes the cursor to its field. Caller holds the lock.
static void tc_knobs(struct os *s, const struct input *in) {
    struct seq *sq = &s->seq;
    if (in->knob_moved[0]) {
        int v = knob_follow(s, in, 0, sq->swing, 50, 75);
        if (v != sq->swing) sq->swing = sq->res == SEQ_RES_SP1200 ? seq_swing_step(sq, v, 0) : v;
        if (swing_shown(s)) s->cursor_tc = 1;
    }
    if (in->knob_moved[1]) sq->shift_ticks = knob_follow(s, in, 1, sq->shift_ticks, 0, SEQ_PPQ / 2 - 1), s->cursor_tc = 3;
    if (in->knob_moved[2]) s->tc_track = knob_follow(s, in, 2, s->tc_track, 0, SEQ_TRACKS - 1), s->cursor_tc = 4;
}

// Each pad hit adds that pad to the notes Move Existing works on, or takes it off again.
void seq_tc_pad(struct os *s, int note) {
    if (note < 0 || note >= 128) return;
    s->tc_notes[note] = !s->tc_notes[note];
    s->tc_count += s->tc_notes[note] ? 1 : -1;
}

void seq_tc_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    edit_fields(s, in, TC_FIELDS, COUNT(TC_FIELDS), &s->cursor_tc);
    tc_knobs(s, in);
    int move = input_down(in, IN_B3);
    uint32_t from = s->tc_from < s->tc_to ? s->tc_from : s->tc_to;
    uint32_t to = s->tc_from < s->tc_to ? s->tc_to : s->tc_from;
    pthread_mutex_unlock(&sq->lock);
    if (move) {
        os_undo_save(s, "Timing Correct");
        int moved = seq_move_existing(sq, s->tc_track, from, to, s->tc_count ? s->tc_notes : NULL);
        char msg[32];
        snprintf(msg, sizeof msg, "Moved %d notes", moved);
        os_toast(s, u, msg);
    }
    pthread_mutex_lock(&sq->lock);
    lcd_text(u, 11, KEY_ROW, "<Move Existing>");
    lcd_rule(u, TITLE_ROW, "Timing Correct");
    draw_labels(u, TC_LABELS, COUNT(TC_LABELS));
    if (!s->tc_count) lcd_text(u, 14, 10, "(Press pads)"); // the hint, while no notes are picked
    if (swing_shown(s)) lcd_text(u, 0, 4, "Swing%:");
    lcd_rule(u, 7, "Move Existing");
    draw_fields(s, u, TC_FIELDS, COUNT(TC_FIELDS), s->cursor_tc);
    lcd_text(u, 0, 13, "K1 Swing K2 Shift K3 Track");
    lcd_rule(u, BOTTOM_ROW, 0);
    pthread_mutex_unlock(&sq->lock);
}

static const struct field PROGRAM_FIELDS[] = {{F_KIT, 2, 5}, {F_PTYPE, 3, 6}, {F_PINST, 4, 6}, {F_PSND, 13, 5}};

// The Program Config screen (Shift + SAMPLE): picks the active track's program, copies and empties programs, and
// shows the bank's pads. Hitting a pad picks it, as everywhere.
static void pad_name(int slot, char *b, size_t n) {
    snprintf(b, n, "%c%02d", 'A' + slot / 16, slot % 16 + 1);
}

int seq_program_pad(struct os *s, struct ui *u, int slot) {
    if (s->screen == OS_PROGRAM && s->pgm_mode == PGM_NONE && PROGRAM_FIELDS[s->cursor_program].id == F_PINST &&
        s->programs[s->program].s600) { // Inst: the pad pressed is the Instrument (and it plays as usual)
        os_set_instrument(s, s->program, slot);
        return 0;
    }
    if (s->screen != OS_PROGRAM || (s->pgm_mode != PGM_COPY && s->pgm_mode != PGM_SWAP)) return 0;
    char a[8], b[8], msg[32];
    pad_name(s->selected, a, sizeof a);
    pad_name(slot, b, sizeof b);
    if (slot == s->selected) {
        os_toast(s, u, "Pick another pad");
        return 1;
    }
    os_undo_save(s, s->pgm_mode == PGM_COPY ? "Copy Pad" : "Swap Pads");
    if (s->pgm_mode == PGM_COPY) {
        os_pad_copy(s, s->selected, slot);
        snprintf(msg, sizeof msg, "Copied %s to %s", a, b);
    } else {
        os_pad_swap(s, s->selected, slot);
        snprintf(msg, sizeof msg, "Swapped %s and %s", a, b);
    }
    s->selected = slot;
    s->pgm_mode = PGM_NONE;
    os_toast(s, u, msg);
    return 1;
}

// The program's pads. B1 <Copy>, B2 <Swap> and B3 <Delete> act on the selected pad (Copy and
// Swap then take the next pad hit); Shift + B1 and B3 copy or empty the whole program. Snd:
// gives the selected pad any sound in memory.
void seq_program_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    char name[8], line[LCD_COLS + 8];
    pad_name(s->selected, name, sizeof name);
    int has = s->pads[s->selected].s != 0;
    if (s->pgm_ask) { // emptying the whole program
        if (input_down(in, IN_B1)) s->pgm_ask = 0;
        if (input_down(in, IN_B3)) {
            os_undo_save(s, "Empty Pgm");
            os_program_clear(s, s->program);
            os_toast(s, u, "Program emptied");
            s->pgm_ask = 0;
        }
    } else if (s->pgm_mode == PGM_DELETE) {
        if (input_down(in, IN_B1)) s->pgm_mode = PGM_NONE;
        if (input_down(in, IN_B3)) {
            os_undo_save(s, "Delete Pad");
            os_pad_clear(s, s->selected);
            snprintf(line, sizeof line, "Pad %s deleted", name);
            os_toast(s, u, line);
            s->pgm_mode = PGM_NONE;
        }
    } else if (s->pgm_mode != PGM_NONE) { // Copy or Swap, waiting for the other pad
        if (input_down(in, IN_B1)) s->pgm_mode = PGM_NONE;
    } else if (in->shift && input_down(in, IN_B3)) {
        if (os_program_sounds(s, s->program)) s->pgm_ask = 1;
    } else if (in->shift && input_down(in, IN_B1)) { // the whole program into the first empty one
        int to = -1;
        for (int p = 0; p < OS_PROGRAMS && to < 0; p++)
            if (p != s->program && !os_program_sounds(s, p)) to = p;
        if (to < 0) {
            os_toast(s, u, "No empty program");
        } else {
            os_undo_save(s, "Copy Pgm");
            os_program_copy(s, s->program, to);
            pthread_mutex_lock(&sq->lock);
            sq->sequence[sq->current].track[sq->track].kit = to;
            pthread_mutex_unlock(&sq->lock);
            os_set_program(s, to);
            snprintf(line, sizeof line, "Copied to Program %02d", to + 1);
            os_toast(s, u, line);
        }
    } else if (input_down(in, IN_B1) || input_down(in, IN_B2)) {
        if (has) s->pgm_mode = input_down(in, IN_B1) ? PGM_COPY : PGM_SWAP;
        else if (input_down(in, IN_B2)) s->pgm_mode = PGM_SWAP; // an empty pad can swap with a full one
        else os_toast(s, u, "Pad is empty");
    } else if (input_down(in, IN_B3)) {
        if (has) s->pgm_mode = PGM_DELETE;
        else os_toast(s, u, "Pad is empty");
    }
    pthread_mutex_lock(&sq->lock);
    int asking = s->pgm_ask || s->pgm_mode != PGM_NONE;
    if (!asking) edit_fields(s, in, PROGRAM_FIELDS, COUNT(PROGRAM_FIELDS), &s->cursor_program);
    int kit = sq->sequence[sq->current].track[sq->track].kit;
    if (s->prog_refresh) { // Type or Inst changed: the instrument's keygroups (the lock let go: they take none)
        int g = s->prog_refresh - 1, inst = s->prog_inst - 2;
        s->prog_refresh = s->prog_inst = 0;
        pthread_mutex_unlock(&sq->lock);
        if (inst >= -1) os_set_instrument(s, g, inst);
        else os_multi_refresh(s, g);
        pthread_mutex_lock(&sq->lock);
    }
    if (s->pgm_ask || s->pgm_mode == PGM_DELETE) {
        lcd_text(u, 0, KEY_ROW, "<No>");
        lcd_text(u, 21, KEY_ROW, "<Yes>");
    } else if (asking) {
        lcd_text(u, 0, KEY_ROW, "<Cancel>");
    } else if (in->shift) {
        lcd_text(u, 0, KEY_ROW, "<Copy Pgm>");
        lcd_text(u, 16, KEY_ROW, "<Clear Pgm>");
    } else {
        lcd_text(u, 0, KEY_ROW, "<Copy>");
        lcd_text(u, 9, KEY_ROW, "<Swap>");
        lcd_text(u, 18, KEY_ROW, "<Delete>");
    }
    // No title: Pgm: and Type: on the title's row and the next, then a row of space.
    lcd_text(u, 0, 2, "Pgm:");
    lcd_text(u, 0, 3, "Type:");
    lcd_text(u, 0, 4, "Inst:");
    snprintf(line, sizeof line, "Bank %c", 'A' + s->bank);
    lcd_text(u, 20, 3, line);
    // The bank's 16 pads in two columns: 1 to 8 on the left, 9 to 16 on the right.
    for (int i = 0; i < 16; i++) {
        int slot = s->bank * 16 + i;
        const struct os_pad *pad = &s->programs[kit].pads[slot];
        snprintf(line, sizeof line, "%02d %-9.9s", i + 1, pad->s ? pad->name : "--");
        int col = i < 8 ? 0 : 13, row = 5 + i % 8;
        if (slot == s->selected) lcd_inverse(u, col, row, line);
        else lcd_text(u, col, row, line);
    }
    snprintf(line, sizeof line, "%s:", name);
    lcd_text(u, 0, 13, line);
    draw_fields(s, u, PROGRAM_FIELDS, COUNT(PROGRAM_FIELDS), asking ? -1 : s->cursor_program);
    lcd_rule(u, BOTTOM_ROW, 0);
    if (asking) { // the question sits over the pad list
        const char *q1 = " Empty every pad in", *q2 = 0;
        char l2[LCD_COLS + 8];
        if (s->pgm_ask) {
            snprintf(l2, sizeof l2, " Program %02d?", kit + 1);
        } else if (s->pgm_mode == PGM_DELETE) {
            q1 = " Delete the sound on";
            snprintf(l2, sizeof l2, " pad %s?", name);
        } else {
            q1 = s->pgm_mode == PGM_COPY ? " Press the pad to copy" : " Press the pad to swap";
            snprintf(l2, sizeof l2, " %s %s", name, s->pgm_mode == PGM_COPY ? "to" : "with");
        }
        q2 = l2;
        snprintf(line, sizeof line, "%-*s", LCD_COLS, q1);
        lcd_inverse(u, 0, 6, line);
        snprintf(line, sizeof line, "%-*s", LCD_COLS, q2);
        lcd_inverse(u, 0, 7, line);
    }
    pthread_mutex_unlock(&sq->lock);
}

static const struct field ERASE_FIELDS[] = {
    {F_E_SEQ, 3, 10}, {F_E_TRK, 4, 7},
    {F_E_FROM_BAR, 5, 6}, {F_E_FROM_BEAT, 5, 10}, {F_E_FROM_TICK, 5, 13},
    {F_E_TO_BAR, 5, 16}, {F_E_TO_BEAT, 5, 20}, {F_E_TO_TICK, 5, 23}, {F_E_NOTES, 6, 6},
};
static const struct label ERASE_LABELS[] = {
    {3, 0, "Sequence:"}, {4, 0, "Track:"}, {5, 0, "Ticks:"}, {5, 9, "."}, {5, 12, "."}, {5, 15, "-"},
    {5, 19, "."}, {5, 22, "."}, {6, 0, "Notes:"},
    {8, 0, "Track 0 is every track"},
    {13, 0, "K1 Sequence  K2 Track"},
};

// ---- The track mixer, after the MPC3000's stereo mix screen: 16 channels at a time ----

#define MIXER_BANKS ((SEQ_TRACKS + 15) / 16) // A to G: tracks 1 to 99

void seq_mixer_open(struct os *s) {
    if (s->bank >= MIXER_BANKS) s->bank = MIXER_BANKS - 1;
    s->screen = OS_MIXER;
    s->fader_caught = 0;
    s->mixer_track = s->bank * 16 + s->mixer_track % 16;
    if (s->mixer_track >= SEQ_TRACKS) s->mixer_track = SEQ_TRACKS - 1;
}

void seq_mixer_pad(struct os *s, int pad) {
    if (pad >= 0 && pad < SEQ_TRACKS && pad != s->mixer_track) {
        s->mixer_track = pad;
        s->fader_caught = 0; // the slider waits to reach this channel's level
    }
}

// A track is heard unless muted, or while others are soloed and it is not.
static int audible(const struct seq_sequence *q, int t) {
    int solo = 0;
    for (int i = 0; i < SEQ_TRACKS && !solo; i++) solo = q->track[i].solo;
    return !q->track[t].mute && (!solo || q->track[t].solo);
}

// Sends a channel's level and pan on: to the notes it is playing (silent when muted), and out
// MIDI on a MIDI track. Caller holds the lock.
static void mixer_changed(struct os *s, int t) {
    const struct seq_track *k = &current(s)->track[t];
    engine_track_mix(s->engine, t + 1, audible(current(s), t) ? k->level : 0, k->pan);
    if (k->channel > 0) {
        seq_send_cc(&s->seq, k->channel, 7, k->level * 127 / 100);
        seq_send_cc(&s->seq, k->channel, 10, (k->pan + 50) * 127 / 100);
    }
}

// A mixer control from MIDI CC (0..127): level, pan, the EQ's three gains, mute, solo.
void seq_mixer_cc(struct os *s, int t, int control, int value) {
    pthread_mutex_lock(&s->seq.lock);
    struct seq_track *k = &current(s)->track[t];
    int eq = 0, all = 0, max = eq_gain_max(k->eq.type);
    switch (control) {
    case MCC_LEVEL: k->level = value * 100 / 127; break;
    case MCC_PAN: k->pan = clampi((value - 64) * 50 / 63, -50, 50); break;
    case MCC_LOW: k->eq.lg = (value * 2 * max / 127 - max) / 5 * 5, eq = 1; break;
    case MCC_MID: k->eq.mg = (value * 2 * max / 127 - max) / 5 * 5, eq = 1; break;
    case MCC_HIGH: k->eq.hg = (value * 2 * max / 127 - max) / 5 * 5, eq = 1; break;
    case MCC_MUTE: k->mute = value >= 64, all = 1; break;
    case MCC_SOLO: k->solo = value >= 64, all = 1; break;
    }
    if (all)
        for (int i = 0; i < SEQ_TRACKS; i++) mixer_changed(s, i);
    else if (!eq) mixer_changed(s, t);
    pthread_mutex_unlock(&s->seq.lock);
    if (eq) os_eq_push(s, t);
}

static void pan_text(int pan, char *b, size_t n) {
    if (pan == 0) snprintf(b, n, "C");
    else snprintf(b, n, "%c%d", pan < 0 ? 'L' : 'R', pan < 0 ? -pan : pan);
}

// A round knob of radius r dots, its pointer at `turn` (-1 to 1: up to 135 degrees either way).
static void draw_knob(struct ui *u, int cx, int cy, int r, double turn, int filled) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            int d2 = x * x + y * y;
            if ((filled && d2 <= r * r) || (d2 <= r * r + r && d2 >= r * r - r)) lcd_dot(u, (cx + x) * 2, (cy + y) * 2, LCD_FG);
        }
    double a = turn * 2.356;
    for (int i = 0; i < r; i++)
        lcd_dot(u, (cx + (int)lround(sin(a) * i)) * 2, (cy - (int)lround(cos(a) * i)) * 2, filled ? LCD_BG : LCD_FG);
}

// One channel strip, as on the MPC3000's stereo out mixer: the pan knob (filled when soloed),
// a triangle under it when selected, the name written downwards (A, 0, 1), its fader beside
// the name (dotted when muted), and a line between strips. In LCD dots, 10 wide.
#define STRIP 10
#define STRIP_TOP 50 // dots down: the track's settings and its EQ above
static void draw_strip(struct ui *u, int i, int track, const struct seq_track *k, int selected) {
    int x0 = (i * STRIP) * 2, y0 = STRIP_TOP * 2;
#define DOT(x, y) lcd_dot(u, x0 + (x) * 2, y0 + (y) * 2, LCD_FG)
    static const char *const knob[9] = {"..#####..", ".#.....#.", "#.......#", "#.......#", "#.......#",
                                        "#.......#", "#.......#", ".#.....#.", "..#####.."};
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 9; x++)
            if (knob[y][x] == '#' || (k->solo && y > 0 && y < 8 && x > 0 && x < 8 && knob[y][x] == '.' &&
                                       (y > 1 || (x > 1 && x < 7)) && (y < 7 || (x > 1 && x < 7))))
                DOT(x, y);
    double a = k->pan / 50.0 * 2.356; // the pointer: up at centre, 135 degrees at L50 and R50
    for (int r = 0; r <= 3; r++) {
        int px = 4 + (int)lround(sin(a) * r), py = 4 - (int)lround(cos(a) * r);
        lcd_dot(u, x0 + px * 2, y0 + py * 2, k->solo ? LCD_BG : LCD_FG);
    }
    if (selected) {
        for (int x = 2; x <= 6; x++) DOT(x, 10);
        for (int x = 3; x <= 5; x++) DOT(x, 11);
        DOT(4, 12);
    }
    char name[4];
    snprintf(name, sizeof name, "%c%02d", 'A' + track / 16, track + 1);
    for (int c = 0; c < 3; c++) lcd_glyph(u, x0, y0 + (15 + c * 10) * 2, name[c], LCD_FG);
    int top = 15, bottom = 119 - STRIP_TOP, h = k->level * (bottom - top) / 100; // the fader, up to the level
    for (int y = bottom - h; y <= bottom; y++)
        if (!k->mute || y % 3 == 0) DOT(6, y), DOT(7, y);
    for (int y = 0; y < 120 - STRIP_TOP; y++) DOT(STRIP - 1, y);
#undef DOT
}

void seq_mixer_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    if (s->mixer_ask) { // Shift + B1 <Reset EQ>: every track's EQ back to flat, once confirmed
        if (input_down(in, IN_B3)) {
            os_undo_save(s, "Reset EQ");
            pthread_mutex_lock(&sq->lock);
            for (int i = 0; i < SEQ_TRACKS; i++) {
                struct track_eq *q = &current(s)->track[i].eq;
                q->lg = q->mg = q->hg = 0;
            }
            pthread_mutex_unlock(&sq->lock);
            os_eq_push_all(s);
            os_toast(s, u, "EQ reset on every track");
            s->mixer_ask = 0;
        } else if (input_down(in, IN_B1)) {
            s->mixer_ask = 0;
        }
    } else if (input_down(in, IN_B3) && !in->shift) {
        eq_open(s);
        ui_wake(u, 1);
        return;
    } else if (input_down(in, IN_B1) && in->shift) {
        s->mixer_ask = 1;
    } else if (input_down(in, IN_B3) && in->shift) { // the FX Sends page (Track/Mixer effects)
        sends_open(s);
        ui_wake(u, 1);
        return;
    } else if (input_down(in, IN_B2) && in->shift) { // MIDI CC control of the mixer, on or off
        s->midi.mixer_cc = !s->midi.mixer_cc;
        os_toast(s, u, s->midi.mixer_cc ? "Mixer MIDI CC: On" : "Mixer MIDI CC: Off");
        os_save_settings(s);
    }
    pthread_mutex_lock(&sq->lock);
    if (s->mixer_track / 16 != s->bank) { // PAD BANK: the same place in the next sixteen
        s->mixer_track = s->bank * 16 + s->mixer_track % 16;
        if (s->mixer_track >= SEQ_TRACKS) s->mixer_track = SEQ_TRACKS - 1;
        s->fader_caught = 0;
        for (int i = 0; i < 3; i++) s->knob_caught[i] = 0;
    }
    int t = s->mixer_track;
    struct seq_track *k = &current(s)->track[t];
    int changed = 0, eq_changed = 0;
    if (!s->mixer_ask && !in->shift) {
        if (input_down(in, IN_B1)) { // <Mute>
            k->mute = !k->mute;
            for (int i = 0; i < SEQ_TRACKS; i++) mixer_changed(s, i);
        }
        if (input_down(in, IN_B2)) { // <Solo>: on its own, others silenced while any is soloed
            k->solo = !k->solo;
            for (int i = 0; i < SEQ_TRACKS; i++) mixer_changed(s, i);
        }
    }
    if (in->fader_moved) { // the slider is the channel's level, once it reaches it, so nothing jumps
        int last = s->fader_last, now = in->fader, at = k->level * 127 / 100;
        s->fader_last = now;
        int pos = os_knob_take(s, &s->fader_caught, last, now, at);
        if (pos >= 0) {
            k->level = pos * 100 / 127;
            changed = 1;
        }
    }
    int d = in->encoder + (in->shift ? 0 : input_down(in, IN_PLUS) - input_down(in, IN_MINUS));
    if (d) { // the wheel pans it
        k->pan = clampi(k->pan + d, -50, 50);
        changed = 1;
    }
    if (changed) mixer_changed(s, t);
    const char *band = 0;
    for (int b = 0; b < 3; b++) { // K1-K3: the EQ's low, mid and high gains, caught like the slider
        if (!in->knob_moved[b]) continue;
        int *g = b == 0 ? &k->eq.lg : b == 1 ? &k->eq.mg : &k->eq.hg, max = eq_gain_max(k->eq.type);
        int at = (*g + max) * 127 / (2 * max), last = s->knob_last[b], now = in->knob[b];
        s->knob_last[b] = now;
        int pos = os_knob_take(s, &s->knob_caught[b], last, now, at);
        if (pos >= 0) {
            *g = (pos * 2 * max / 127 - max) / 5 * 5;
            eq_changed = 1;
            band = b == 0 ? "Low" : b == 1 ? "Mid" : "High";
        }
    }
    struct track_eq q = k->eq;

    char line[LCD_COLS + 8], pan[8], f[12];
    if (s->mixer_ask) {
        lcd_text(u, 0, KEY_ROW, "<No>");
        lcd_text(u, 21, KEY_ROW, "<Yes>");
    } else if (in->shift) {
        lcd_text(u, 0, KEY_ROW, "<Reset EQ>");
        if (s->midi.mixer_cc) lcd_inverse(u, 11, KEY_ROW, "<MIDI CC>");
        else lcd_text(u, 11, KEY_ROW, "<MIDI CC>");
        lcd_text(u, 21, KEY_ROW, "<FX>");
    } else {
        if (k->mute) lcd_inverse(u, 0, KEY_ROW, "<Mute>");
        else lcd_text(u, 0, KEY_ROW, "<Mute>");
        if (k->solo) lcd_inverse(u, 9, KEY_ROW, "<Solo>");
        else lcd_text(u, 9, KEY_ROW, "<Solo>");
        lcd_text(u, 20, KEY_ROW, "<EQ>");
    }
    snprintf(line, sizeof line, "Trk:%c%02d %-.11s", 'A' + t / 16, t + 1, k->name);
    lcd_text(u, 0, 1, line);
    if (k->type == TRACK_MIDI) snprintf(line, sizeof line, "Pgm:%d", k->program);
    else snprintf(line, sizeof line, "Pgm:%02d", k->kit + 1);
    lcd_text(u, 19, 1, line);
    // The EQ: a knob for each band, its pointer at the gain, the frequency under it.
    int hz[3] = {q.lf, q.mf, q.type == EQ_1073 ? 12000 : q.hf}, gain[3] = {q.lg, q.mg, q.hg}, max = eq_gain_max(q.type);
    for (int b = 0; b < 3; b++) {
        int cx = 22 + b * 56;
        draw_knob(u, cx, 24, 5, q.on ? (double)gain[b] / max : 0, 0);
        eq_freq_text(hz[b], f, sizeof f);
        int len = (int)strlen(f), col = (cx * 2 - LCD_X) / CELL_W - len / 2;
        lcd_text(u, col < 0 ? 0 : col, 4, f);
    }
    if (!q.on) lcd_text(u, 22, 2, "OFF");
    pan_text(k->pan, pan, sizeof pan);
    char chn[8];
    if (k->channel > 0) snprintf(chn, sizeof chn, "%d", k->channel);
    else snprintf(chn, sizeof chn, "OFF");
    snprintf(line, sizeof line, "Level:%3d Pan:%-3s Chn:%s", k->level, pan, chn);
    lcd_text(u, 0, 5, line);
    for (int i = 0; i < 16; i++) {
        int track = s->bank * 16 + i;
        if (track >= SEQ_TRACKS) break;
        draw_strip(u, i, track, &current(s)->track[track], track == t);
    }
    if (s->mixer_ask) {
        lcd_inverse(u, 0, 2, " Reset the EQ on every    ");
        lcd_inverse(u, 0, 3, " track to flat?           ");
    }
    pthread_mutex_unlock(&sq->lock);
    if (eq_changed) {
        os_eq_push(s, t);
        char g[16];
        int v = band[0] == 'L' ? q.lg : band[0] == 'M' ? q.mg : q.hg;
        snprintf(g, sizeof g, "%s %+d.%ddB", band, v / 10, (v < 0 ? -v : v) % 10);
        os_toast(s, u, g);
    }
}

// Opens on the current sequence and active track, over the whole sequence, all notes.
void seq_erase_open(struct os *s) {
    pthread_mutex_lock(&s->seq.lock);
    if (s->screen != OS_ERASE) s->erase_return = s->screen;
    s->screen = OS_ERASE;
    s->erase_seq = s->seq.current;
    s->erase_track = s->seq.track + 1;
    s->erase_from = 0;
    s->erase_to = (uint32_t)seq_length(current(s));
    memset(s->erase_notes, 0, sizeof s->erase_notes);
    s->erase_count = 0;
    s->cursor_erase = 0;
    for (int k = 0; k < 3; k++) s->knob_caught[k] = 0; // K1 and K2 catch up to the sequence and track
    pthread_mutex_unlock(&s->seq.lock);
}

void seq_erase_pad(struct os *s, int note) {
    if (note < 0 || note >= 128) return;
    s->erase_notes[note] = !s->erase_notes[note];
    s->erase_count += s->erase_notes[note] ? 1 : -1;
}

void seq_erase_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    if (input_down(in, IN_B1)) {
        s->screen = s->erase_return;
        ui_wake(u, 1);
        return;
    }
    if (input_down(in, IN_B3)) { // <Do It>
        os_undo_save(s, "Erase");
        uint32_t from = s->erase_from < s->erase_to ? s->erase_from : s->erase_to;
        uint32_t to = s->erase_from < s->erase_to ? s->erase_to : s->erase_from;
        int erased = seq_erase_range(sq, s->erase_seq, s->erase_track - 1, from, to,
                                     s->erase_count ? s->erase_notes : NULL);
        char msg[32];
        snprintf(msg, sizeof msg, "Erased %d events", erased);
        os_toast(s, u, msg);
    }
    pthread_mutex_lock(&sq->lock);
    edit_fields(s, in, ERASE_FIELDS, COUNT(ERASE_FIELDS), &s->cursor_erase);
    if (in->fader_moved) s->erase_track = 1 + in->fader * (SEQ_TRACKS - 1) / 127; // tracks 1 to 99, never 0
    // K1 the sequence, K2 the track (0, every track, at the left end).
    int seq = knob_follow(s, in, 0, s->erase_seq, 0, SEQ_COUNT - 1);
    if (seq != s->erase_seq) change(s, F_E_SEQ, seq - s->erase_seq, 0), s->cursor_erase = 0;
    int trk = knob_follow(s, in, 1, s->erase_track, 0, SEQ_TRACKS);
    if (trk != s->erase_track) s->erase_track = trk, s->cursor_erase = 1;
    lcd_text(u, 0, KEY_ROW, "<Back>");
    lcd_text(u, 19, KEY_ROW, "<Do It>");
    lcd_rule(u, TITLE_ROW, "Erase");
    draw_labels(u, ERASE_LABELS, COUNT(ERASE_LABELS));
    if (!s->erase_count) lcd_text(u, 14, 6, "(Press pads)"); // the hint, while no notes are picked
    draw_fields(s, u, ERASE_FIELDS, COUNT(ERASE_FIELDS), s->cursor_erase);
    lcd_rule(u, BOTTOM_ROW, 0);
    pthread_mutex_unlock(&sq->lock);
}

void seq_metro_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    if (input_down(in, IN_B1)) {
        s->screen = s->metro_return;
        ui_wake(u, 1); // draw the screen being returned to
        return;
    }
    pthread_mutex_lock(&sq->lock);
    edit_fields(s, in, METRO_FIELDS, COUNT(METRO_FIELDS), &s->cursor_metro);
    lcd_text(u, 0, KEY_ROW, "<Back>");
    lcd_rule(u, TITLE_ROW, "Metronome");
    lcd_rule(u, 9, "Count-in");
    draw_labels(u, METRO_LABELS, COUNT(METRO_LABELS));
    draw_fields(s, u, METRO_FIELDS, COUNT(METRO_FIELDS), s->cursor_metro);
    if (sq->count_wait) {
        lcd_text(u, 0, 13, "A pad or MIDI note starts");
    }
    lcd_rule(u, BOTTOM_ROW, 0);
    pthread_mutex_unlock(&sq->lock);
}

void seq_repeat_open(struct os *s) {
    if (s->screen != OS_REPEAT) s->repeat_return = s->screen;
    s->screen = OS_REPEAT;
    s->cursor_repeat = 0; // pressed again: the first field
    for (int k = 0; k < 3; k++) s->knob_last[k] = -1, s->knob_turn[k] = 0; // read from where they are
}

// The knobs as steps, from where they are: a step for each few degrees turned. 0 until then.
#define KNOB_DETENT 6
static int knob_detents(struct os *s, const struct input *in, int k) {
    if (!in->knob_moved[k]) return 0;
    int now = in->knob[k], last = s->knob_last[k];
    s->knob_last[k] = now;
    if (last < 0) return 0; // the first reading: only where it is
    s->knob_turn[k] += now - last;
    int steps = s->knob_turn[k] / KNOB_DETENT;
    s->knob_turn[k] -= steps * KNOB_DETENT;
    return steps;
}

// Note Repeat's rate. Pads repeat here too, so the rate can be heard while it is picked.
void seq_repeat_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    if (input_down(in, IN_B1)) {
        s->screen = s->repeat_return;
        ui_wake(u, 1);
        return;
    }
    pthread_mutex_lock(&sq->lock);
    int arp = sq->arp;
    const struct field *fields = arp ? ARP_FIELDS : REPEAT_FIELDS;
    int nf = arp ? COUNT(ARP_FIELDS) : COUNT(REPEAT_FIELDS);
    if (s->cursor_repeat >= nf) s->cursor_repeat = 0;
    if (input_down(in, IN_B2)) { // <Rate>: the next (Shift: the one before), round again
        sq->rep_rate = (sq->rep_rate + (in->shift ? REP_RATES - 1 : 1)) % REP_RATES;
        sq->rep_alt = -1;
        s->cursor_repeat = 1;
    }
    edit_fields(s, in, fields, nf, &s->cursor_repeat);
    if (in->fader_moved) sq->rep_rate = in->fader * REP_RATES / 128; // the slider runs through them too
    // K1 the rate; on the arpeggiator K2 the category and K3 the pattern.
    int k1 = knob_detents(s, in, 0), k2 = knob_detents(s, in, 1), k3 = knob_detents(s, in, 2);
    if (k1) change(s, F_REP_RATE, k1, 0), sq->rep_alt = -1, s->cursor_repeat = 1;
    if (k2 && sq->arp) change(s, F_ARP_CAT, k2, 0), s->cursor_repeat = 2;
    if (k3 && sq->arp) change(s, F_ARP_PAT, k3, 0), s->cursor_repeat = 3;
    // B3 <Edit>: Pattern Edit, from anywhere on the page. A USER pattern is edited; a STEP one is
    // copied into the first empty USER pattern first; anything else (or Note Repeat) switches to
    // the arpeggiator on the USER pattern edited last.
    if (input_down(in, IN_B3)) {
        int user = sq->arp ? seq_arp_user_slot(sq->arp_pattern) : -1;
        if (user < 0 && sq->arp && seq_arp_group(sq->arp_pattern) == ARP_GROUP_STEP) {
            struct seq_arp_steps empty;
            seq_arp_steps_clear(&empty);
            for (int i = 0; i < ARP_USER && user < 0; i++)
                if (!memcmp(&sq->arp_user[i], &empty, sizeof empty)) user = i;
            if (user < 0) {
                pthread_mutex_unlock(&sq->lock);
                os_toast(s, u, "No empty USER pattern");
                return;
            }
            sq->arp_user[user] = *seq_arp_steps_of(sq, sq->arp_pattern);
            s->fx_save_at = (s->frame_ms ? s->frame_ms : 1) + 1500;
        }
        if (user < 0) user = s->arpedit_slot >= 0 && s->arpedit_slot < ARP_USER ? s->arpedit_slot : 0;
        if (sq->arp_pattern != seq_arp_user_pattern(user) || !sq->arp) { // a step pattern's rate and gate
            sq->rep_rate = REP_16, sq->rep_alt = -1, sq->arp_gate = 50;
        }
        sq->arp = 1, sq->arp_pattern = seq_arp_user_pattern(user);
        pthread_mutex_unlock(&sq->lock);
        arpedit_open(s, user);
        ui_wake(u, 1);
        return;
    }
    arp = sq->arp; // Type may have changed
    fields = arp ? ARP_FIELDS : REPEAT_FIELDS;
    nf = arp ? COUNT(ARP_FIELDS) : COUNT(REPEAT_FIELDS);
    lcd_text(u, 0, KEY_ROW, "<Back>");
    lcd_text(u, 9, KEY_ROW, "<Rate>");
    lcd_text(u, 20, KEY_ROW, "<Edit>");
    lcd_rule(u, TITLE_ROW, arp ? "Arpeggiator" : "Note Repeat");
    if (arp) draw_labels(u, ARP_LABELS, COUNT(ARP_LABELS));
    else draw_labels(u, REPEAT_LABELS, COUNT(REPEAT_LABELS));
    draw_fields(s, u, fields, nf, s->cursor_repeat);
    // What B1-B3 switch to, held (or toggled) with NOTE REPEAT.
    char line[LCD_COLS + 1];
    int row = arp ? 11 : 7;
    lcd_text(u, 0, row, s->rep_toggle ? "B keys switch to:" : "B keys held play:");
    for (int b = 0; b < 3; b++) {
        int alt = seq_repeat_alt(sq->rep_rate, b);
        snprintf(line, sizeof line, "B%d:%s", b + 1, alt >= 0 ? REP_NAMES[alt] : "--");
        lcd_text(u, b * 9, row + 1, line);
    }
    lcd_text(u, 0, 13, arp ? "K1 Rate K2 Cat K3 Pattern" : "K1 Rate");
    lcd_rule(u, BOTTOM_ROW, 0);
    pthread_mutex_unlock(&sq->lock);
}

static void tap_tempo(struct os *s, struct ui *u) {
    int64_t now = now_ms(u);
    if (s->ntaps > 0 && now - s->taps[s->ntaps - 1] > TAP_RESET_MS) s->ntaps = 0;
    if (s->ntaps == 4) {
        memmove(s->taps, s->taps + 1, 3 * sizeof s->taps[0]);
        s->ntaps = 3;
    }
    s->taps[s->ntaps++] = now;
    if (s->ntaps < 2) return;
    double interval = (double)(s->taps[s->ntaps - 1] - s->taps[0]) / (s->ntaps - 1);
    if (interval <= 0) return;
    int tempo10 = (int)(600000.0 / interval + 0.5);
    seq_sync_tempo_tap(&s->seq, tempo10);
    char msg[32];
    pthread_mutex_lock(&s->seq.lock);
    int t = current(s)->tempo10;
    pthread_mutex_unlock(&s->seq.lock);
    snprintf(msg, sizeof msg, "BPM: %d.%d", t / 10, t % 10);
    os_toast(s, u, msg);
}

void seq_transport(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    // Recording about to start (PLAY with REC armed, or REC punching in): a step to undo.
    pthread_mutex_lock(&sq->lock);
    int armed = sq->mode == SEQ_STOPPED && sq->rec_arm != REC_ARM_OFF, playing = sq->mode == SEQ_PLAYING;
    pthread_mutex_unlock(&sq->lock);
    if ((input_down(in, IN_PLAY) && armed) || (input_down(in, IN_SEQ_REC) && !in->shift && playing))
        os_undo_save(s, "Record");
    if (input_down(in, IN_PLAY) && s->screen == OS_SONG) // the Song page: the song, from the selected step
        seq_song_play(sq, s->song_sel, s->song_step_sel);
    else if (input_down(in, IN_PLAY)) seq_play(sq, !in->shift); // Shift + PLAY continues from where it stopped
    if (input_down(in, IN_STOP)) seq_stop(sq, in->shift);
    if (input_down(in, IN_SEQ_REC)) {
        if (in->shift) { // Recall: the main page's position on a bar; elsewhere the page's help, its second page
            if (s->screen == OS_MAIN) seq_locate_bar(sq, 1);
            else if (!help_open(s, 1)) os_toast(s, u, "No help for this page");
        } else {
            seq_rec_button(sq);
            pthread_mutex_lock(&sq->lock);
            const char *state = status_text(sq);
            pthread_mutex_unlock(&sq->lock);
            os_toast(s, u, state);
        }
    }
    if (input_down(in, IN_TAP_TEMPO)) {
        if (in->shift) { // the metronome page
            if (s->screen == OS_METRO) s->cursor_metro = 0; // pressed again: the first field
            else s->metro_return = s->screen;
            s->screen = OS_METRO;
        } else {
            tap_tempo(s, u);
        }
    }
}

// Shift + Pad 2 and 3: the sequence halved or doubled, asking first when notes would be lost or
// could be copied. Shift + Pad 6 and 7: the BPM halved or doubled, asking about compensation.
// Each shows the main page, where the question is.
static void whole_sequence(struct os *s, struct ui *u, int pad) {
    struct seq *sq = &s->seq;
    if (s->screen != OS_MAIN) seq_go_main(s);
    pthread_mutex_lock(&sq->lock);
    struct seq_sequence *q = current(s);
    int bars = q->bars, used = seq_used(q), tempo = q->tempo10;
    int half = bars / 2 < 1 ? 1 : bars / 2, twice = bars * 2 > SEQ_MAX_BARS ? SEQ_MAX_BARS : bars * 2;
    int lost = seq_events_from(q, (uint32_t)half * (uint32_t)seq_bar_ticks(q));
    pthread_mutex_unlock(&sq->lock);
    char msg[32];
    switch (pad) {
    case 1: // half the bars, keeping the first half
        if (bars == 1) {
            os_toast(s, u, "Sequence is 1 bar");
        } else if (lost) {
            s->seq_ask = SEQ_ASK_BARS, s->ask_bars = half;
        } else {
            os_undo_save(s, "Half Seq");
            seq_set_bars(sq, sq->current, half, 0);
            snprintf(msg, sizeof msg, "Bars: %d", half);
            os_toast(s, u, msg);
        }
        break;
    case 2: // twice the bars
        if (twice == bars) {
            os_toast(s, u, "Sequence at its longest");
        } else if (used) {
            s->seq_ask = SEQ_ASK_BARS, s->ask_bars = twice;
        } else {
            os_undo_save(s, "Double Seq");
            seq_set_bars(sq, sq->current, twice, 0);
            snprintf(msg, sizeof msg, "Bars: %d", twice);
            os_toast(s, u, msg);
        }
        break;
    case 5: // half speed
        if (tempo / 2 < 300) os_toast(s, u, "BPM would be below 30");
        else s->seq_ask = SEQ_ASK_BPM_HALF;
        break;
    case 6: // double speed
        if (tempo * 2 > 3000) os_toast(s, u, "BPM would be above 300");
        else s->seq_ask = SEQ_ASK_BPM_DOUBLE;
        break;
    }
    ui_wake(u, 1);
}

int seq_shift_pad(struct os *s, struct ui *u, int pad) {
    struct seq *sq = &s->seq;
    switch (pad) {
    case 1: case 2: case 5: case 6:
        whole_sequence(s, u, pad);
        return 1;
    case 3: {
        pthread_mutex_lock(&sq->lock);
        sq->count_in = !sq->count_in;
        int on = sq->count_in;
        pthread_mutex_unlock(&sq->lock);
        os_toast(s, u, on ? "Count-In: Enabled" : "Count-In: Disabled");
        return 1;
    }
    case 9: {
        pthread_mutex_lock(&sq->lock);
        if (sq->tc != TC_OFF) {
            s->tc_last = sq->tc;
            sq->tc = TC_OFF;
        } else {
            sq->tc = s->tc_last;
        }
        int off = sq->tc == TC_OFF;
        pthread_mutex_unlock(&sq->lock);
        os_toast(s, u, off ? "Timing Correct: Off" : "Timing Correct: On");
        return 1;
    }
    case 8: // the track mixer
        if (s->screen == OS_MIXER) seq_mixer_pad(s, s->bank * 16); // pressed again: the bank's first channel
        else seq_mixer_open(s);
        return 1;
    case 13:
        if (s->screen == OS_TC) s->cursor_tc = 0; // pressed again: the first field
        else seq_tc_open(s);
        return 1;
    default:
        return 0;
    }
}

void seq_lights(struct ui *u, struct os *s) {
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    int mode = sq->mode, armed = sq->rec_arm != REC_ARM_OFF;
    pthread_mutex_unlock(&sq->lock);
    int recording = mode == SEQ_RECORDING || mode == SEQ_OVERDUB || mode == SEQ_COUNT_IN;
    ui_button_led(u, IN_PLAY, mode != SEQ_STOPPED ? 2 : 1);
    ui_button_led(u, IN_SEQ, s->screen == OS_MAIN ? 2 : 1);
    if (recording) {
        ui_button_led(u, IN_SEQ_REC, 2);
    } else if (mode == SEQ_WAITING || (armed && mode == SEQ_STOPPED)) {
        ui_button_led(u, IN_SEQ_REC, now_ms(u) / 250 % 2 ? 2 : 0); // a ready record button flashes
        ui_wake(u, 125);
    } else {
        ui_button_led(u, IN_SEQ_REC, 1);
    }
    if (mode != SEQ_STOPPED) ui_wake(u, 50); // also keeps MIDI IN read while waiting for a key
}

// Copy Sequence, after the MPC3000's: one sequence, all of it, over another. Opens on the
// current sequence, copying to the first empty one after it.
void seq_copy_open(struct os *s) {
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    s->screen = OS_SEQCOPY;
    s->copy_from = sq->current;
    s->copy_to = (sq->current + 1) % SEQ_COUNT;
    for (int i = 1; i < SEQ_COUNT; i++) {
        int n = (sq->current + i) % SEQ_COUNT;
        if (!seq_used(&sq->sequence[n])) {
            s->copy_to = n;
            break;
        }
    }
    s->copy_ask = 0;
    s->cursor_copy = 1;
    pthread_mutex_unlock(&sq->lock);
}

void seq_copy_pad(struct os *s, int pad) {
    if (pad >= 0 && pad < SEQ_COUNT) s->copy_to = pad, s->copy_ask = 0, s->cursor_copy = 1;
}

static void copy_do(struct os *s, struct ui *u) {
    os_undo_save(s, "Copy Seq");
    char msg[32];
    if (seq_copy(&s->seq, s->copy_from, s->copy_to) == 0) snprintf(msg, sizeof msg, "Copied to Seq %d", s->copy_to + 1);
    else snprintf(msg, sizeof msg, "Not enough memory");
    os_toast(s, u, msg);
}

void seq_copy_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq *sq = &s->seq;
    if (s->copy_ask) { // the sequence copied to has notes: replace them?
        if (input_down(in, IN_B1)) s->copy_ask = 0;
        else if (input_down(in, IN_B3)) s->copy_ask = 0, copy_do(s, u);
    } else if (input_down(in, IN_B1)) { // <Back>
        s->screen = OS_MAIN;
        ui_wake(u, 1);
        return;
    } else if (input_down(in, IN_B3) && s->copy_from != s->copy_to) { // <Copy>
        pthread_mutex_lock(&sq->lock);
        int used = seq_used(&sq->sequence[s->copy_to]);
        pthread_mutex_unlock(&sq->lock);
        if (used) s->copy_ask = 1;
        else copy_do(s, u);
    } else if (input_down(in, IN_B3)) {
        os_toast(s, u, "Pick another sequence");
    }
    if (!s->copy_ask) {
        if (in->encoder_press) s->cursor_copy = !s->cursor_copy;
        int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
        int *v = s->cursor_copy ? &s->copy_to : &s->copy_from;
        if (d) *v = clampi(*v + d, 0, SEQ_COUNT - 1);
    }
    pthread_mutex_lock(&sq->lock);
    char line[40];
    lcd_rule(u, TITLE_ROW, "Copy Sequence");
    for (int i = 0; i < 2; i++) {
        int n = i ? s->copy_to : s->copy_from;
        const struct seq_sequence *q = &sq->sequence[n];
        lcd_text(u, 0, 4 + 2 * i, i ? "To:" : "From:");
        if (seq_used(q)) snprintf(line, sizeof line, " %02d-%-16.16s ", n + 1, q->name);
        else snprintf(line, sizeof line, " %02d-(empty)%9s ", n + 1, "");
        if (s->cursor_copy == i && !s->copy_ask) lcd_inverse(u, 5, 4 + 2 * i, line);
        else lcd_text(u, 5, 4 + 2 * i, line);
    }
    if (s->copy_ask) {
        snprintf(line, sizeof line, "Replace Seq %d?", s->copy_to + 1);
        lcd_text(u, 0, 9, line);
        lcd_text(u, 0, KEY_ROW, "<Cancel>");
        lcd_text(u, 17, KEY_ROW, "<Replace>");
    } else {
        lcd_text(u, 0, 9, "Copies tracks, notes, bars");
        lcd_text(u, 0, 10, "and tempo.");
        lcd_text(u, 0, KEY_ROW, "<Back>");
        lcd_text(u, 20, KEY_ROW, "<Copy>");
    }
    lcd_text(u, 0, 13, "Pads: A01=Seq1 B01=Seq17");
    lcd_rule(u, BOTTOM_ROW, 0);
    pthread_mutex_unlock(&sq->lock);
}
