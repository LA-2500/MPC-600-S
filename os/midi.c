// MIDI: the settings page (Shift + Pad 8), the Mixer CC Map (B2 there) and Pad Notes (B3).
#include "lcd.h"
#include "midi_out.h"
#include "os.h"
#include "seq.h"
#include <stdio.h>
#include <string.h>

static const int CC_DEFAULT[MCC_COUNT] = {-1, -1, -1, -1, -1, -1, -1}; // all OFF: nothing answers until set or learned
static const char *const CC_NAMES[MCC_COUNT] = {"Level:", "Pan:", "EQ Low:", "EQ Mid:", "EQ High:", "Mute:", "Solo:"};

void midi_cc_defaults(struct midi_settings *m) {
    m->cc_target = CCT_BANK;
    memcpy(m->cc, CC_DEFAULT, sizeof m->cc);
}

void midi_defaults(struct midi_settings *m) {
    memset(m, 0, sizeof *m);
    m->ext = 1;
    m->usb = 1;
    m->base = 1;
    m->local = 1;
    m->foot_chn = 0, m->foot_cc = 64, m->foot_pol = FPOL_AUTO; // any channel, the sustain pedal's CC
    midi_cc_defaults(m);
}

void midi_open(struct os *s) {
    if (s->screen != OS_MIDI && s->screen != OS_CCMAP && s->screen != OS_PADNOTES) s->midi_return = s->screen;
    s->screen = OS_MIDI;
    s->cursor_midi = 0;
}

static void back(struct os *s, struct ui *u, int to) {
    s->cc_learn = s->note_learn = s->foot_learn = 0;
    s->screen = to;
    os_save_settings(s);
    ui_wake(u, 1);
}

// ---- MIDI Settings ----

enum { M_EXT, M_USB, M_BASE, M_RECV, M_SYNC, M_SYNCIN, M_LOCAL, M_THRU, M_SUS, M_FOOT, M_FCHN, M_FCC, M_FPOL,
       M_FIELDS }; // Mixer CC: on the CC page
static const char *const M_LABELS[M_FIELDS] = {"Ext MIDI:", "USB MIDI:", "Base Chn:", "Receive Chn:", "Sync:", "  Sync In:",
                                               "Local Mode:", "Soft Thru:", "Sustain:", "  Footswitch:", "  Foot Chn:",
                                               "  Foot CC:", "  Polarity:"};
#define M_ROWS 10 // rows 3-12 for the fields; row 13 shows what the pedal last sent

static int m_fields(const struct os *s, int *out) {
    int n = 0;
    for (int f = 0; f < M_FIELDS; f++)
        if ((f < M_FOOT || s->midi.sustain == SUS_FOOT) && (f != M_SYNCIN || s->midi.sync == SYNC_RECEIVE)) out[n++] = f;
    return n;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void m_change(struct midi_settings *m, int f, int d) {
    switch (f) {
    case M_EXT: m->ext = d > 0; break;
    case M_USB: m->usb = d > 0; break;
    case M_SYNCIN: m->sync_in = d > 0 ? MIDI_PORT_USB : MIDI_PORT_EXT; break;
    case M_BASE: m->base = clampi(m->base + d, 1, 16); break;
    case M_RECV: m->recv = clampi(m->recv + d, 0, 16); break;
    case M_SYNC: m->sync = clampi(m->sync + d, SYNC_OFF, SYNC_RECEIVE); break;
    case M_LOCAL: m->local = d > 0; break;
    case M_THRU: m->thru = d > 0; break;
    case M_SUS: m->sustain = clampi(m->sustain + d, SUS_NORMAL, SUS_FOOT); break;
    case M_FOOT: m->foot = clampi(m->foot + d, FOOT_PLAY, FOOT_MODES - 1); break;
    case M_FCHN: m->foot_chn = clampi(m->foot_chn + d, 0, 16); break;
    case M_FCC: m->foot_cc = clampi(m->foot_cc + d, 0, 127); break;
    case M_FPOL: m->foot_pol = clampi(m->foot_pol + d, 0, FPOL_COUNT - 1); break;
    }
}

static void m_value(const struct midi_settings *m, int f, char *b, size_t n) {
    static const char *const sync[] = {"OFF", "SEND", "RECEIVE"}, *const sus[] = {"NORMAL", "TO DURATION", "FOOTSWITCH"};
    switch (f) {
    case M_EXT: snprintf(b, n, " %s ", m->ext ? "ON" : "OFF"); break;
    case M_USB: snprintf(b, n, " %s ", m->usb ? "ON" : "OFF"); break;
    case M_SYNCIN: snprintf(b, n, " %s ", m->sync_in == MIDI_PORT_USB ? "USB" : "EXT"); break;
    case M_BASE: snprintf(b, n, " %d ", m->base); break;
    case M_RECV:
        if (m->recv) snprintf(b, n, " %d ", m->recv);
        else snprintf(b, n, " ALL ");
        break;
    case M_SYNC: snprintf(b, n, " %s ", sync[m->sync]); break;
    case M_LOCAL: snprintf(b, n, " %s ", m->local ? "ON" : "OFF"); break;
    case M_THRU: snprintf(b, n, " %s ", m->thru ? "ON" : "OFF"); break;
    case M_SUS: snprintf(b, n, " %s ", sus[m->sustain]); break;
    case M_FOOT: snprintf(b, n, " %s ", m->foot == FOOT_AUDIO ? "AUDIO REC" : m->foot == FOOT_REC ? "REC/OVERDUB" : "PLAY/STOP"); break;
    case M_FCHN:
        if (m->foot_chn) snprintf(b, n, " %d ", m->foot_chn);
        else snprintf(b, n, " ANY ");
        break;
    case M_FCC: snprintf(b, n, " %d ", m->foot_cc); break;
    case M_FPOL: snprintf(b, n, " %s ", m->foot_pol == FPOL_NORMAL ? "NORMAL" : m->foot_pol == FPOL_INVERTED ? "INVERTED" : "AUTO"); break;
    }
}

void midi_frame(struct os *s, struct ui *u, const struct input *in) {
    if (input_down(in, IN_B1)) { // <Settings>: Additional Settings
        s->foot_learn = 0;
        prefs_open(s);
        ui_wake(u, 1);
        return;
    }
    int list[M_FIELDS], n = m_fields(s, list);
    if (s->cursor_midi >= n) s->cursor_midi = 0;
    int on_foot = list[s->cursor_midi] >= M_FOOT; // B2 is <Learn> on the footswitch's fields
    if (input_down(in, IN_B2) && on_foot) {
        s->foot_learn = !s->foot_learn; // pressed again before the pedal: cancelled
        ui_wake(u, 1);
    } else if (input_down(in, IN_B2)) {
        s->screen = OS_CCMAP;
        s->cursor_ccmap = 0;
        ui_wake(u, 1);
        return;
    }
    if (input_down(in, IN_B3)) {
        s->foot_learn = 0;
        s->screen = OS_PADNOTES;
        s->pad_note_sel = s->bank * 16;
        ui_wake(u, 1);
        return;
    }
    if (in->encoder_press) s->cursor_midi = (s->cursor_midi + (in->shift ? n - 1 : 1)) % n;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d) m_change(&s->midi, list[s->cursor_midi], d);
    n = m_fields(s, list);
    if (s->cursor_midi >= n) s->cursor_midi = n - 1;

    on_foot = list[s->cursor_midi] >= M_FOOT;
    if (!on_foot) s->foot_learn = 0;

    lcd_text(u, 0, KEY_ROW, "<Settings>");
    if (on_foot) lcd_text(u, 11, KEY_ROW, s->foot_learn ? "<Stop>" : "<Learn>");
    else lcd_text(u, 11, KEY_ROW, "<CC>");
    lcd_text(u, 19, KEY_ROW, "<Notes>");
    lcd_rule(u, TITLE_ROW, "MIDI Settings");
    int top = s->cursor_midi - (M_ROWS - 1); // scrolled to keep the cursor in view
    top = top < 0 ? 0 : top;
    for (int i = top; i < n && i < top + M_ROWS; i++) {
        char v[24];
        int row = 3 + i - top;
        lcd_text(u, 0, row, M_LABELS[list[i]]);
        m_value(&s->midi, list[i], v, sizeof v);
        if (i == s->cursor_midi) lcd_inverse(u, 13, row, v);
        else lcd_text(u, 13, row, v);
    }
    if (s->midi.sustain == SUS_FOOT) { // what the pedal last sent, to see it is getting through
        char line[LCD_COLS + 8];
        if (s->foot_learn) snprintf(line, sizeof line, "Press the pedal to learn");
        else if (s->foot_seen) snprintf(line, sizeof line, "Pedal: Chn %d CC %d = %d", s->foot_last_ch, s->foot_last_cc, s->foot_last_val);
        else snprintf(line, sizeof line, "Pedal: nothing yet");
        lcd_text(u, 0, 13, line);
        ui_wake(u, 100);
    }
    lcd_rule(u, BOTTOM_ROW, 0);
}

// ---- Mixer CC Map ----

// The CC page: Mixer CC on or off, its Target, then each control's CC.
enum { C_ON, C_TARGET, C_FIRST };

void ccmap_frame(struct os *s, struct ui *u, const struct input *in) {
    struct midi_settings *m = &s->midi;
    int n = C_FIRST + MCC_COUNT;
    if (input_down(in, IN_B1)) return back(s, u, OS_MIDI);
    if (input_down(in, IN_B2)) { // <Learn>: 1 + the control; from Mixer CC or Target, the first control
        if (s->cursor_ccmap < C_FIRST) s->cursor_ccmap = C_FIRST;
        s->cc_learn = s->cc_learn ? 0 : s->cursor_ccmap - C_FIRST + 1;
    }
    if (input_down(in, IN_B3)) { // <Default>
        midi_cc_defaults(m);
        s->cc_learn = 0;
        os_toast(s, u, "CC Map: default");
    }
    if (in->encoder_press) s->cursor_ccmap = (s->cursor_ccmap + (in->shift ? n - 1 : 1)) % n, s->cc_learn = 0;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d && s->cursor_ccmap == C_ON) m->mixer_cc = d > 0, os_save_settings(s);
    else if (d && s->cursor_ccmap == C_TARGET) m->cc_target = d > 0 ? CCT_SELECTED : CCT_BANK;
    else if (d) m->cc[s->cursor_ccmap - C_FIRST] = clampi(m->cc[s->cursor_ccmap - C_FIRST] + d, -1, 127); // below 0 is OFF

    lcd_text(u, 0, KEY_ROW, "<Back>");
    if (s->cc_learn) lcd_inverse(u, 9, KEY_ROW, "<Learn>");
    else lcd_text(u, 9, KEY_ROW, "<Learn>");
    lcd_text(u, 17, KEY_ROW, "<Default>");
    lcd_rule(u, TITLE_ROW, "Mixer CC Map");
    char v[24];
    lcd_text(u, 0, 3, "Mixer CC:");
    snprintf(v, sizeof v, " %s ", m->mixer_cc ? "ON" : "OFF");
    if (s->cursor_ccmap == C_ON) lcd_inverse(u, 9, 3, v);
    else lcd_text(u, 9, 3, v);
    lcd_text(u, 0, 4, "Target:");
    snprintf(v, sizeof v, " %s ", m->cc_target == CCT_BANK ? "BANK" : "SELECTED");
    if (s->cursor_ccmap == C_TARGET) lcd_inverse(u, 9, 4, v);
    else lcd_text(u, 9, 4, v);
    for (int i = 0; i < MCC_COUNT; i++) {
        lcd_text(u, 0, 5 + i, CC_NAMES[i]);
        if (m->cc[i] < 0) snprintf(v, sizeof v, " OFF ");
        else snprintf(v, sizeof v, " CC %3d ", m->cc[i]);
        if (s->cursor_ccmap == C_FIRST + i) lcd_inverse(u, 9, 5 + i, v);
        else lcd_text(u, 9, 5 + i, v);
    }
    if (s->cc_learn) lcd_text(u, 0, 12, "Move a control to assign");
    else if (m->cc_target == CCT_BANK) lcd_text(u, 0, 12, "Chn 1-16: bank's strips");
    else lcd_text(u, 0, 12, "Base Chn: selected track");
    lcd_rule(u, BOTTOM_ROW, 0);
}

// A CC arrived while <Learn> waited: it goes to that control, Mixer CC goes on so it answers
// at once, and the cursor moves to the next control, ready for the next <Learn>.
void ccmap_learned(struct os *s, const struct ui *u, int cc) {
    int i = s->cc_learn - 1;
    if (i < 0 || i >= MCC_COUNT) return;
    s->midi.cc[i] = cc;
    s->midi.mixer_cc = 1;
    s->cc_learn = 0;
    if (i + 1 < MCC_COUNT) s->cursor_ccmap = C_FIRST + i + 1;
    char msg[32];
    snprintf(msg, sizeof msg, "%s CC %d", CC_NAMES[i], cc);
    os_toast(s, u, msg);
    os_save_settings(s);
}

// ---- Pad Notes ----

static void note_name(int note, char *b, size_t n) {
    static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(b, n, "%s%d", names[note % 12], note / 12 - 1); // middle C, note 60, is C4
}

// ---- Pad Notes templates ----

// The scales, as semitones from the root, up to the octave.
static const struct {
    const char *name;
    int n;
    signed char st[8];
} SCALES[] = {
    {"MAJOR", 7, {0, 2, 4, 5, 7, 9, 11}},          {"NATURAL MINOR", 7, {0, 2, 3, 5, 7, 8, 10}},
    {"HARMONIC MINOR", 7, {0, 2, 3, 5, 7, 8, 11}}, {"MELODIC MINOR", 7, {0, 2, 3, 5, 7, 9, 11}},
    {"DORIAN", 7, {0, 2, 3, 5, 7, 9, 10}},         {"PHRYGIAN", 7, {0, 1, 3, 5, 7, 8, 10}},
    {"LYDIAN", 7, {0, 2, 4, 6, 7, 9, 11}},         {"MIXOLYDIAN", 7, {0, 2, 4, 5, 7, 9, 10}},
    {"LOCRIAN", 7, {0, 1, 3, 5, 6, 8, 10}},        {"MAJ PENTATONIC", 5, {0, 2, 4, 7, 9}},
    {"MIN PENTATONIC", 5, {0, 3, 5, 7, 10}},       {"BLUES", 6, {0, 3, 5, 6, 7, 10}},
    {"MAJOR BLUES", 6, {0, 2, 3, 4, 7, 9}},        {"WHOLE TONE", 6, {0, 2, 4, 6, 8, 10}},
    {"DIM WHOLE-HALF", 8, {0, 2, 3, 5, 6, 8, 9, 11}}, {"DIM HALF-WHOLE", 8, {0, 1, 3, 4, 6, 7, 9, 10}},
    {"PHRYGIAN DOM", 7, {0, 1, 4, 5, 7, 8, 10}},   {"LYDIAN DOM", 7, {0, 2, 4, 6, 7, 9, 10}},
    {"ALTERED", 7, {0, 1, 3, 4, 6, 8, 10}},        {"HUNGARIAN MINOR", 7, {0, 2, 3, 6, 7, 8, 11}},
    {"DOUBLE HARMONIC", 7, {0, 1, 4, 5, 7, 8, 11}}, {"NEAPOLITAN MIN", 7, {0, 1, 3, 5, 7, 8, 11}},
    {"NEAPOLITAN MAJ", 7, {0, 1, 3, 5, 7, 9, 11}}, {"UKRAINIAN DOR", 7, {0, 2, 3, 6, 7, 9, 10}},
    {"PERSIAN", 7, {0, 1, 4, 5, 6, 8, 11}},        {"ENIGMATIC", 7, {0, 1, 4, 6, 8, 10, 11}},
    {"PROMETHEUS", 6, {0, 2, 4, 6, 9, 10}},        {"AUGMENTED", 6, {0, 3, 4, 7, 8, 11}},
    {"HIRAJOSHI", 5, {0, 2, 3, 7, 8}},             {"IN SEN", 5, {0, 1, 5, 7, 10}},
    {"IWATO", 5, {0, 1, 5, 6, 10}},                {"EGYPTIAN", 5, {0, 2, 5, 7, 10}},
    {"BEBOP DOMINANT", 8, {0, 2, 4, 5, 7, 9, 10, 11}}, {"BEBOP MAJOR", 8, {0, 2, 4, 5, 7, 8, 9, 11}},
    {"SPANISH 8-TONE", 8, {0, 1, 3, 4, 5, 6, 8, 10}},
};
#define NSCALES ((int)(sizeof SCALES / sizeof SCALES[0]))
static const char *const TPL_NAMES[PADTPL_KINDS] = {"DEFAULT MPC", "CHROMATIC", "SCALE"};
static const char *const ROOTS[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// The note a template gives pad p (0..127): DEFAULT the MPC map, CHROMATIC a semitone a pad from
// C in the octave, SCALE the scale's notes upward from the root in the octave.
static int template_note(const struct os *s, int p) {
    int base = 12 * (s->padtpl_oct + 1); // C in that octave: C4 is 60
    int note;
    if (s->padtpl_kind == PADTPL_CHROMATIC) note = base + p;
    else if (s->padtpl_kind == PADTPL_SCALE) {
        const int sc = s->padtpl_scale < 0 ? 0 : s->padtpl_scale >= NSCALES ? NSCALES - 1 : s->padtpl_scale;
        note = base + s->padtpl_root + 12 * (p / SCALES[sc].n) + SCALES[sc].st[p % SCALES[sc].n];
    } else {
        note = 35 + p;
    }
    return note < 0 ? 0 : note > 127 ? 127 : note;
}

int padnotes_scales(void) {
    return NSCALES;
}

void padnotes_apply(struct os *s) {
    for (int p = 0; p < 128; p++) seq_set_pad_note(p, template_note(s, p));
}

static void note_name(int note, char *b, size_t n);

// The template chooser, over the pad list: Template, then (by template) Octave, Root and Scale.
static void template_frame(struct os *s, struct ui *u, const struct input *in) {
    int fields[4] = {0}, nf = 0; // 0 Template, 1 Root, 2 Scale, 3 Octave
    fields[nf++] = 0;
    if (s->padtpl_kind == PADTPL_SCALE) fields[nf++] = 1, fields[nf++] = 2;
    if (s->padtpl_kind != PADTPL_DEFAULT) fields[nf++] = 3;
    if (s->cursor_padtpl >= nf) s->cursor_padtpl = 0;
    if (input_down(in, IN_B1)) { // <Cancel>
        s->padtpl_open = 0;
        return;
    }
    if (input_down(in, IN_B3)) { // <Apply>
        padnotes_apply(s);
        s->padtpl_open = 0;
        os_save_settings(s);
        os_toast(s, u, "Template applied");
        return;
    }
    if (in->encoder_press) s->cursor_padtpl = (s->cursor_padtpl + (in->shift ? nf - 1 : 1)) % nf;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d) switch (fields[s->cursor_padtpl]) {
        case 0: s->padtpl_kind = clampi(s->padtpl_kind + d, 0, PADTPL_KINDS - 1); break;
        case 1: s->padtpl_root = ((s->padtpl_root + d) % 12 + 12) % 12; break;
        case 2: s->padtpl_scale = clampi(s->padtpl_scale + d, 0, NSCALES - 1); break;
        case 3: s->padtpl_oct = clampi(s->padtpl_oct + d, -1, 8); break;
        }
    char v[32], a[8], b[8];
    lcd_text(u, 0, KEY_ROW, "<Cancel>");
    lcd_text(u, 19, KEY_ROW, "<Apply>");
    lcd_rule(u, TITLE_ROW, "Pad Note Template");
    static const char *const LABELS[4] = {"Template:", "Root:", "Scale:", "Octave:"};
    for (int i = 0; i < nf; i++) {
        int f = fields[i];
        if (f == 0) snprintf(v, sizeof v, " %s ", TPL_NAMES[s->padtpl_kind]);
        else if (f == 1) snprintf(v, sizeof v, " %s ", ROOTS[s->padtpl_root]);
        else if (f == 2) snprintf(v, sizeof v, " %s ", SCALES[s->padtpl_scale].name);
        else snprintf(v, sizeof v, " C%d ", s->padtpl_oct);
        lcd_text(u, 0, 4 + i, LABELS[f]);
        if (i == s->cursor_padtpl) lcd_inverse(u, 9, 4 + i, v);
        else lcd_text(u, 9, 4 + i, v);
    }
    note_name(template_note(s, 0), a, sizeof a), note_name(template_note(s, 15), b, sizeof b);
    snprintf(v, sizeof v, "A01 %s ... A16 %s", a, b);
    lcd_text(u, 0, 9, v);
    lcd_text(u, 0, 10, "All 128 pads, every bank");
    lcd_text(u, 0, 12, "Apply replaces the notes");
    lcd_rule(u, BOTTOM_ROW, 0);
}

int padnotes_pad(struct os *s, struct ui *u, int slot) {
    (void)u;
    if (s->screen != OS_PADNOTES) return 0;
    s->pad_note_sel = slot; // a pad picks the one to set; it does not play here
    return 1;
}

void padnotes_frame(struct os *s, struct ui *u, const struct input *in) {
    if (s->padtpl_open) {
        template_frame(s, u, in);
        if (s->padtpl_open) return;
        ui_wake(u, 1);
        return;
    }
    if (input_down(in, IN_B1)) return back(s, u, OS_MIDI);
    if (input_down(in, IN_B2)) s->note_learn = !s->note_learn; // <Learn>: the next note in
    if (input_down(in, IN_B3)) { // <Template>: default, chromatic or a scale, for every pad
        s->padtpl_open = 1, s->cursor_padtpl = 0;
        s->note_learn = 0;
        ui_wake(u, 1);
        return;
    }
    if (s->pad_note_sel / 16 != s->bank) s->pad_note_sel = s->bank * 16 + s->pad_note_sel % 16;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d) seq_set_pad_note(s->pad_note_sel, seq_pad_note(s->pad_note_sel) + d);

    char line[LCD_COLS + 8], name[8];
    lcd_text(u, 0, KEY_ROW, "<Back>");
    if (s->note_learn) lcd_inverse(u, 9, KEY_ROW, "<Learn>");
    else lcd_text(u, 9, KEY_ROW, "<Learn>");
    lcd_text(u, 16, KEY_ROW, "<Template>");
    snprintf(line, sizeof line, "Pad Notes  Bank %c", 'A' + s->bank);
    lcd_rule(u, TITLE_ROW, line);
    for (int i = 0; i < 16; i++) { // two columns, as on the Program screen
        int pad = s->bank * 16 + i, note = seq_pad_note(pad);
        note_name(note, name, sizeof name);
        snprintf(line, sizeof line, "%02d %3d %-4s", i + 1, note, name);
        int col = i < 8 ? 0 : 13, row = 3 + i % 8;
        if (pad == s->pad_note_sel) lcd_inverse(u, col, row, line);
        else lcd_text(u, col, row, line);
    }
    if (s->note_learn) lcd_text(u, 0, 12, "Play a note to assign");
    else lcd_text(u, 0, 12, "Press pad, turn the wheel");
    lcd_rule(u, BOTTOM_ROW, 0);
}
