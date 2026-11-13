// Additional Settings (B1 on MIDI Settings): the button and pad lights, the sequencer's
// resolution and swing, the screen's colours, and the tempo, Timing Correct and swing the
// MPC 600 S starts with. Kept on the card with the settings.
#include "lcd.h"
#include "os.h"
#include <stdio.h>
#include <string.h>

static const char *const BUTTONS[BUTTON_LIGHTS] = {"RED", "WHITE", "PINK", "OFF"};
static const char *const PADS[PAD_LIGHTS] = {"BLUE", "RED", "WHITE", "OFF"};
static const char *const RES[SEQ_RES_COUNT] = {"MPC 3000", "MPC 2000", "SP12", "MPC60"};
static const int PPQN[SEQ_RES_COUNT] = {96, 96, 24, 96};
// The order the wheel shows them in (the MPC60 was added after the others were saved as numbers).
static const int RES_ORDER[SEQ_RES_COUNT] = {SEQ_RES_MPC3000, SEQ_RES_MPC60, SEQ_RES_MPC2000, SEQ_RES_SP1200};
enum { PF_BUTTONS, PF_PADS, PF_THRESH, PF_RES, PF_SCREEN, PF_BPM, PF_BARS, PF_TC, PF_SWING, PF_FIELDS };

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void prefs_open(struct os *s) {
    s->screen = OS_PREFS;
    s->cursor_prefs = 0;
}

// The two-colour buttons: which of their colours light (red alone, white alone, both: pink).
void prefs_apply(struct os *s) {
    static const int STYLE[BUTTON_LIGHTS] = {1, 2, 0, 2};
    s->led_style = STYLE[clampi(s->button_lights, 0, BUTTON_LIGHTS - 1)];
    if (s->set_pad_threshold) s->set_pad_threshold(s->pad_threshold);
    if (s->set_led_style) s->set_led_style(s->led_style);
    lcd_set_scheme(s->screen_scheme);
}

void prefs_frame(struct os *s, struct ui *u, const struct input *in) {
    if (input_down(in, IN_B1)) { // <MIDI>: back to MIDI Settings
        s->screen = OS_MIDI;
        ui_wake(u, 1);
        return;
    }
    if (in->encoder_press) s->cursor_prefs = (s->cursor_prefs + (in->shift ? PF_FIELDS - 1 : 1)) % PF_FIELDS;
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (d) {
        switch (s->cursor_prefs) {
        case PF_BUTTONS: s->button_lights = clampi(s->button_lights + d, 0, BUTTON_LIGHTS - 1); break;
        case PF_PADS: s->pad_lights = clampi(s->pad_lights + d, 0, PAD_LIGHTS - 1); break;
        case PF_THRESH: s->pad_threshold = clampi(s->pad_threshold + d, 1, 60); break;
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
        case PF_BPM: s->start_bpm10 = clampi(s->start_bpm10 + d * (in->shift ? 1 : 10), 300, 3000); break;
        case PF_BARS: s->start_bars = clampi(s->start_bars + d, 1, SEQ_MAX_BARS); break;
        case PF_TC: s->start_tc = clampi(s->start_tc + d, 0, TC_COUNT - 1); break;
        case PF_SWING: s->start_swing = seq_swing_step(&s->seq, s->start_swing, d); break;
        }
        prefs_apply(s);
        s->fx_save_at = (s->frame_ms ? s->frame_ms : 1) + 1500; // with the other settings, in a moment
        ui_wake(u, 1);
    }
    char v[32];
    lcd_text(u, 0, KEY_ROW, "<MIDI>");
    lcd_rule(u, TITLE_ROW, "Additional Settings");
    char bpm[12], sw[8], bars[8], thresh[8];
    snprintf(thresh, sizeof thresh, "%d", s->pad_threshold);
    snprintf(bars, sizeof bars, "%d", s->start_bars);
    snprintf(bpm, sizeof bpm, "%d.%d", s->start_bpm10 / 10, s->start_bpm10 % 10);
    snprintf(sw, sizeof sw, "%d%%", seq_swing_step(&s->seq, s->start_swing, 0));
    const char *labels[PF_FIELDS] = {"Button Lights:", "Pad Lights:", "Pad Thresh:", "", "Screen:", "Start BPM:", "Start Bars:",
                                     "Start TC:", "Start Swing:"};
    const char *values[PF_FIELDS] = {BUTTONS[s->button_lights], PADS[s->pad_lights], thresh, RES[s->seq.res],
                                     lcd_scheme_name(s->screen_scheme), bpm, bars, seq_tc_name(s->start_tc), sw};
    const int rows[PF_FIELDS] = {3, 4, 5, 7, 9, 10, 11, 12, 13}, cols[PF_FIELDS] = {14, 14, 14, 14, 7, 12, 12, 12, 12};
    for (int i = 0; i < PF_FIELDS; i++) {
        lcd_text(u, 0, rows[i], labels[i]);
        snprintf(v, sizeof v, " %s ", values[i]);
        if (s->cursor_prefs == i) lcd_inverse(u, cols[i], rows[i], v);
        else lcd_text(u, cols[i], rows[i], v);
    }
    // The resolution, and under it the machine whose timing and swing it follows (the setting).
    snprintf(v, sizeof v, "Sequencer Res: %dPPQN", PPQN[s->seq.res]);
    lcd_text(u, 0, 6, v);
    lcd_rule(u, BOTTOM_ROW, 0);
}

// Each frame: the pads in their colour, dimmed while pressed (harder, dimmer); off once.
void prefs_pad_lights(struct os *s, struct ui *u) {
    static const int RGB[PAD_LIGHTS][3] = {{40, 90, 127}, {127, 0, 0}, {127, 127, 127}, {0, 0, 0}}; // light blue, red, white
    if (s->pad_lights == PAD_LIGHT_OFF) {
        if (s->pads_lit) // just turned off: dark once
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
    snprintf(b, n, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", s->button_lights, s->pad_lights, s->seq.res, s->screen_scheme, s->start_bpm10,
             s->start_tc, s->start_swing, s->start_bars, s->inst_gain, s->inst_bright, s->pad_threshold);
}

void prefs_load_line(struct os *s, const char *line) {
    int v[11];
    int got = sscanf(line, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
                     &v[10]);
    if (got >= 10) s->inst_gain = clampi(v[8], 0, 30), s->inst_bright = clampi(v[9], 0, 100);
    if (got >= 11) s->pad_threshold = clampi(v[10], 1, 60);
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
