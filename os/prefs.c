// Additional Settings (B1 on MIDI Settings): the button and pad lights, the sequencer's
// resolution and swing, the screen's colours, how the knobs take over a value, and the tempo, Timing Correct and swing the
// MPC 600 S starts with. Kept on the card with the settings.
#include "lcd.h"
#include "os.h"
#include "seq_screen.h"
#include <stdio.h>
#include <string.h>

static const char *const BUTTONS[BUTTON_LIGHTS] = {"RED", "WHITE", "PINK", "OFF"};
static const char *const PADS[PAD_LIGHTS] = {"BLUE", "RED", "WHITE", "OFF"};
static const char *const RES[SEQ_RES_COUNT] = {"MPC 3000", "MPC 2000", "SP12", "MPC60"};
static const int PPQN[SEQ_RES_COUNT] = {96, 96, 24, 96};
// The order the wheel shows them in (the MPC60 was added after the others were saved as numbers).
static const char *const LOOP_CONV[LOOP_CONVS] = {"IMMEDIATE", "AFTER 1ST LOOP"};
static const int RES_ORDER[SEQ_RES_COUNT] = {SEQ_RES_MPC3000, SEQ_RES_MPC60, SEQ_RES_MPC2000, SEQ_RES_SP1200};
// Page 1: lights, pads, screen, timing and the Start sequencer values. Page 2: Live Loop
// Conversion and the Start recording values.
enum { PF_BUTTONS, PF_PADS, PF_THRESH, PF_SCREEN, PF_LAYOUT, PF_RES, PF_USBOUT, PF_BPM, PF_BARS, PF_TC, PF_SWING, // page 1
       PF_LOOPCONV, PF_AUTOROOT, PF_RATE, PF_BITS, PF_AA, PF_NORM, PF_THRU, PF_MODE, PF_KNOB, PF_MUTE, PF_FIELDS }; // page 2
#define PF_PAGE2 PF_LOOPCONV
static const char *const MODES[REC_MODES] = {"STEREO", "MONO LFT", "MONO RGT", "MONO LR"};
static const int BITS[] = {4, 8, 12, 16};
static const char *const KNOB_NAMES[KNOB_MODES] = {"CATCH", "JUMP", "SCALE"};

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void prefs_open(struct os *s) {
    s->screen = OS_PREFS;
    s->cursor_prefs = 0;
    s->rate_edit = 0;
}

// The two-colour buttons: which of their colours light (red alone, white alone, both: pink).
void prefs_apply(struct os *s) {
    static const int STYLE[BUTTON_LIGHTS] = {1, 2, 0, 2};
    s->led_style = STYLE[clampi(s->button_lights, 0, BUTTON_LIGHTS - 1)];
    if (s->set_pad_threshold) s->set_pad_threshold(s->pad_threshold);
    if (s->set_usb_out) s->set_usb_out(s->usb_out);
    if (s->set_led_style) s->set_led_style(s->led_style);
    lcd_set_scheme(s->screen_scheme);
    lcd_layout = s->layout;
}

static int is_classic(int hz) {
    int c[16], n = record_classic(c);
    for (int i = 0; i < n; i++)
        if (c[i] == hz) return 1;
    return 0;
}

// Start Sample Rate: the wheel steps the slider's rates, then SET. On SET, pushing the wheel in
// changes the number itself, to any rate the recording pages can take; pushed again, done.
static void change_rate(struct os *s, int d) {
    if (s->rate_edit) {
        int list[96], n = record_rates(list), i = 0;
        while (i < n - 1 && list[i] < s->start_rate) i++;
        s->start_rate = list[clampi(i + d, 0, n - 1)];
        return;
    }
    int c[16], n = record_classic(c), at = n; // n: SET
    if (!s->start_rate_set)
        for (int i = 0; i < n; i++)
            if (c[i] == s->start_rate) at = i;
    at = clampi(at + d, 0, n);
    if (at == n) {
        s->start_rate_set = 1; // the number stays as it was, to change from
    } else {
        s->start_rate_set = 0;
        s->start_rate = c[at];
    }
}

static void change(struct os *s, int f, int d, int shift) {
    switch (f) {
    case PF_BUTTONS: s->button_lights = clampi(s->button_lights + d, 0, BUTTON_LIGHTS - 1); break;
    case PF_PADS: s->pad_lights = clampi(s->pad_lights + d, 0, PAD_LIGHTS - 1); break;
    case PF_THRESH: s->pad_threshold = clampi(s->pad_threshold + d, 1, 60); break;
    case PF_USBOUT: s->usb_out = d > 0; return;
    case PF_RES:
        pthread_mutex_lock(&s->seq.lock);
        {
            int at = 0;
            while (at < SEQ_RES_COUNT - 1 && RES_ORDER[at] != s->seq.res) at++;
            s->seq.res = RES_ORDER[clampi(at + d, 0, SEQ_RES_COUNT - 1)];
        }
        s->seq.swing = seq_swing_step(&s->seq, s->seq.swing, 0); // onto one of its settings
        pthread_mutex_unlock(&s->seq.lock);
        break;
    case PF_SCREEN: s->screen_scheme = clampi(s->screen_scheme + d, 0, SCREEN_SCHEMES - 1); break;
    case PF_LAYOUT: s->layout = clampi(s->layout + d, 0, LAYOUTS - 1); break;
    case PF_BPM: s->start_bpm10 = clampi(s->start_bpm10 + d * (shift ? 1 : 10), 300, 3000); break;
    case PF_BARS: s->start_bars = clampi(s->start_bars + d, 1, SEQ_MAX_BARS); break;
    case PF_TC: s->start_tc = clampi(s->start_tc + d, 0, TC_COUNT - 1); break;
    case PF_SWING: s->start_swing = seq_swing_step(&s->seq, s->start_swing, d); break;
    case PF_LOOPCONV: s->loop_conv = clampi(s->loop_conv + d, 0, LOOP_CONVS - 1); break;
    case PF_AUTOROOT: s->auto_root = d > 0; break;
    case PF_RATE: change_rate(s, d); break;
    case PF_BITS: {
        int i = 0;
        while (i < 3 && BITS[i] != s->start_bits) i++;
        s->start_bits = BITS[clampi(i + d, 0, 3)];
        break;
    }
    case PF_AA: s->start_aa = d > 0; break;
    case PF_NORM: s->start_norm = d > 0; break;
    case PF_THRU: s->start_thru = d > 0; break;
    case PF_MODE: s->start_mode = clampi(s->start_mode + d, 0, REC_MODES - 1); break;
    case PF_KNOB: s->knob_mode = clampi(s->knob_mode + d, 0, KNOB_MODES - 1); return;
    case PF_MUTE: // AT ONCE (to start) or NEXT NOTE (as before 2.0F)
        pthread_mutex_lock(&s->seq.lock);
        s->seq.mute_next = d > 0;
        if (!s->seq.mute_next) seq_tracks_heard(s); // a muted track's sounds still playing go silent now
        pthread_mutex_unlock(&s->seq.lock);
        return;
    }
    if (f >= PF_RATE) record_apply_start(s); // every recording page, now
}

void prefs_frame(struct os *s, struct ui *u, const struct input *in) {
    int page = s->cursor_prefs >= PF_PAGE2;
    if (input_down(in, IN_B1)) { // <MIDI>: back to MIDI Settings
        s->screen = OS_MIDI;
        s->rate_edit = 0;
        ui_wake(u, 1);
        return;
    }
    if (input_down(in, IN_B3)) { // <Page 2> / <Page 1>
        page = !page;
        s->cursor_prefs = page ? PF_PAGE2 : 0;
        s->rate_edit = 0;
        ui_wake(u, 1);
    }
    int first = page ? PF_PAGE2 : 0, count = page ? PF_FIELDS - PF_PAGE2 : PF_PAGE2;
    if (in->encoder_press) {
        if (s->cursor_prefs == PF_RATE && s->start_rate_set && !in->shift) {
            s->rate_edit = !s->rate_edit; // SET: the number, and back
        } else {
            s->rate_edit = 0;
            s->cursor_prefs = first + (s->cursor_prefs - first + (in->shift ? count - 1 : 1)) % count;
        }
    }
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d) {
        change(s, s->cursor_prefs, d, in->shift);
        prefs_apply(s);
        s->fx_save_at = (s->frame_ms ? s->frame_ms : 1) + 1500; // with the other settings, in a moment
        ui_wake(u, 1);
    }
    char v[32];
    lcd_text(u, 0, KEY_ROW, "<MIDI>");
    lcd_text(u, 18, KEY_ROW, page ? "<Page 1>" : "<Page 2>");
    lcd_rule(u, TITLE_ROW, page ? "Additional Settings 2" : "Additional Settings");
    const char *labels[PF_FIELDS] = {"Button Lights:", "Pad Lights:", "Pad Thresh:", "Screen:", "Layout:", "", "USB Audio Out:", "Start BPM:", "Start Bars:",
                                     "Start TC:", "Start Swing:", "Loop Conv:", "Root Auto-Detect:", "Sample Rate:", "Bit Rate:", "Anti-Aliasing:",
                                     "Normalize:", "Input Thru:", "Rec Mode:", "Knob Mode:", "Track Mute:"};
    char bpm[12], sw[8], bars[8], thresh[8], rate[16], bits[8];
    snprintf(thresh, sizeof thresh, "%d", s->pad_threshold);
    snprintf(bars, sizeof bars, "%d", s->start_bars);
    snprintf(bpm, sizeof bpm, "%d.%d", s->start_bpm10 / 10, s->start_bpm10 % 10);
    snprintf(sw, sizeof sw, "%d%%", seq_swing_step(&s->seq, s->start_swing, 0));
    snprintf(bits, sizeof bits, "%d", s->start_bits);
    int r = s->start_rate;
    if (s->rate_edit) snprintf(rate, sizeof rate, ">%d.%02d kHz", r / 1000, r % 1000 / 10); // being changed
    else if (s->start_rate_set) snprintf(rate, sizeof rate, "SET %d.%02dk", r / 1000, r % 1000 / 10);
    else snprintf(rate, sizeof rate, "%d.%02d kHz", r / 1000, r % 1000 / 10);
    const char *values[PF_FIELDS] = {BUTTONS[s->button_lights], PADS[s->pad_lights], thresh, lcd_scheme_name(s->screen_scheme),
                                     lcd_layout_name(s->layout), RES[s->seq.res], s->usb_out ? "ON" : "OFF", bpm, bars, seq_tc_name(s->start_tc), sw, LOOP_CONV[s->loop_conv], s->auto_root ? "ON" : "OFF", rate, bits,
                                     s->start_aa ? "ON" : "OFF", s->start_norm ? "ON" : "OFF",
                                     s->start_thru ? "ALWAYS ON" : "OFF", MODES[s->start_mode], KNOB_NAMES[s->knob_mode],
                                     s->seq.mute_next ? "NEXT NOTE" : "AT ONCE"};
    const int rows[PF_FIELDS] = {3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 3, 4, 6, 7, 8, 9, 10, 11, 13, 14};
    const int cols[PF_FIELDS] = {14, 14, 14, 7, 7, 14, 14, 12, 12, 12, 12, 10, 17, 14, 14, 14, 14, 14, 14, 11, 12};
    for (int i = first; i < first + count; i++) {
        lcd_text(u, 0, rows[i], labels[i]);
        snprintf(v, sizeof v, " %s ", values[i]);
        if (s->cursor_prefs == i) lcd_inverse(u, cols[i], rows[i], v);
        else lcd_text(u, cols[i], rows[i], v);
    }
    if (!page) { // the resolution, and under it the machine whose timing and swing it is taken from (the setting)
        snprintf(v, sizeof v, "Sequencer Res: %dPPQN", PPQN[s->seq.res]);
        lcd_text(u, 0, 8, v);
    } else {
        lcd_rule(u, 5, "Recording Starts With");
        lcd_rule(u, 12, "Knobs, Slider and Mutes");
    }
}

// Each frame: the pads in their colour, dimmed while pressed (harder, dimmer); off once.
void prefs_pad_lights(struct os *s, struct ui *u) {
    static const int RGB[PAD_LIGHTS][3] = {{40, 90, 127}, {127, 0, 0}, {127, 127, 127}, {0, 0, 0}}; // light blue, red, white
    if (s->pad_lights == PAD_LIGHT_OFF) {
        // Just turned off, or the first seconds after power-on (the stock app may leave a pad
        // lit as it stops, and a pad's light can arrive late): dark.
        if (s->pads_lit || s->frame_ms < s->start_ms + 3000)
            for (int i = 0; i < 16; i++) ui_led(u, i, 0, 0, 0);
        s->pads_lit = 0;
        return;
    }
    s->pads_lit = 1;
    const int *c = RGB[s->pad_lights];
    for (int i = 0; i < 16; i++) {
        float k = s->pad_level[i] > 0 ? 1.0f - 0.65f * s->pad_level[i] / 127.0f : 1.0f;
        ui_led(u, i, (int)(c[0] * k), (int)(c[1] * k), (int)(c[2] * k));
    }
}

void prefs_save_line(const struct os *s, char *b, size_t n) {
    snprintf(b, n, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", s->button_lights, s->pad_lights, s->seq.res,
             s->screen_scheme, s->start_bpm10, s->start_tc, s->start_swing, s->start_bars, s->inst_gain, s->inst_bright,
             s->pad_threshold, s->loop_conv, s->start_rate, s->start_bits, s->start_aa, s->start_norm, s->start_thru, s->start_mode,
             s->auto_root, s->knob_mode, s->usb_out, s->layout, s->usb_outs, s->outs_by_track, s->outs_keep, s->outs_layout, s->seq.mute_next);
}

void prefs_load_line(struct os *s, const char *line) {
    int v[27];
    int got = sscanf(line, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3],
                     &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16], &v[17], &v[18],
                     &v[19], &v[20], &v[21], &v[22], &v[23], &v[24], &v[25], &v[26]);
    if (got >= 27) s->seq.mute_next = v[26] != 0;
    if (got >= 26) s->outs_layout = v[25] != 0;
    if (got >= 25) s->usb_outs = v[22] != 0, s->outs_by_track = v[23] != 0, s->outs_keep = v[24] != 0;
    if (got >= 22) s->layout = clampi(v[21], 0, LAYOUTS - 1);
    if (got >= 21) s->usb_out = v[20] != 0;
    if (got >= 19) s->auto_root = v[18] != 0;
    if (got >= 20) s->knob_mode = clampi(v[19], 0, KNOB_MODES - 1);
    if (got >= 10) s->inst_gain = clampi(v[8], 0, 30), s->inst_bright = clampi(v[9], 0, 100);
    if (got >= 11) s->pad_threshold = clampi(v[10], 1, 60);
    if (got >= 11 && v[10] == 12) s->pad_threshold = 30; // the old default: now 30, against stray presses
    if (got >= 12) s->loop_conv = clampi(v[11], 0, LOOP_CONVS - 1);
    if (got >= 18) { // the Start recording values: onto every recording page
        int list[96], n = record_rates(list), ok = 0;
        for (int i = 0; i < n; i++) ok |= list[i] == v[12];
        if (ok) s->start_rate = v[12];
        s->start_rate_set = !is_classic(s->start_rate);
        if (v[13] == 4 || v[13] == 8 || v[13] == 12 || v[13] == 16) s->start_bits = v[13];
        s->start_aa = v[14] != 0, s->start_norm = v[15] != 0, s->start_thru = v[16] != 0;
        s->start_mode = clampi(v[17], 0, REC_MODES - 1);
        record_apply_start(s);
    }
    if (got != 4 && got < 7) return; // 4: saved before the Start settings, which keep their defaults
    if (got >= 8) s->start_bars = clampi(v[7], 1, SEQ_MAX_BARS);
    if (got >= 7) {
        s->start_bpm10 = clampi(v[4], 300, 3000);
        s->start_tc = clampi(v[5], 0, TC_COUNT - 1);
        s->start_swing = clampi(v[6], 50, 75);
    }
    s->button_lights = clampi(v[0], 0, BUTTON_LIGHTS - 1);
    s->pad_lights = clampi(v[1], 0, PAD_LIGHTS - 1);
    s->seq.res = clampi(v[2], 0, SEQ_RES_COUNT - 1);
    s->seq.swing = seq_swing_step(&s->seq, s->seq.swing, 0);
    s->screen_scheme = clampi(v[3], 0, SCREEN_SCHEMES - 1);
    prefs_apply(s);
}

// At power-on: every sequence at Start BPM and Start Bars (all empty then), and Timing Correct
// and swing as set.
void prefs_startup(struct os *s) {
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    for (int i = 0; i < SEQ_COUNT; i++) sq->sequence[i].tempo10 = s->start_bpm10, sq->sequence[i].bars = s->start_bars;
    sq->tc = s->start_tc;
    sq->swing = seq_swing_step(sq, s->start_swing, 0);
    pthread_mutex_unlock(&sq->lock);
}
