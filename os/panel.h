// The MPC Sample's front panel, as the MPC 600 S sees it: the 320 x 240 display, the pads,
// buttons, knobs, slider and wheel, and their lights.
//
// A frame works like this: panel_run reads whatever the panel has sent, turns it into one
// struct input, calls the OS's frame function with a struct ui to draw into, then sends the
// display and the lights whatever changed. Nothing here knows about the OS's screens.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define UI_W 320
#define UI_H 240

// The buttons, each a bit in struct input's held, down and up. The order is ours; what each
// sends is in panel.c.
enum {
    IN_PLAY, IN_STOP, IN_SEQ_REC, IN_SEQ, IN_SAMPLE, IN_SAMPLE_REC, IN_SAMPLE_SELECT, IN_SHIFT,
    IN_ERASE, IN_NOTE_REPEAT, IN_TAP_TEMPO, IN_LEVELS, IN_LOOP, IN_CHOP, IN_MUTE, IN_PAD_FX,
    IN_KNOB_FX, IN_PAD_BANK, IN_B1, IN_B2, IN_B3, IN_MINUS, IN_PLUS, IN_ENCODER, IN_N
};

// The note each pad sends, pad 1 (bottom left) to pad 16.
extern const unsigned char INPUT_PAD_NOTE[16];

// One frame of input: what happened since the last frame, and what is held now.
struct input {
    unsigned held;     // buttons down now: bit (1u << IN_*)
    unsigned down, up; // pressed, let go, this frame
    int shift;         // SHIFT held
    int encoder;       // the wheel, in steps (+ clockwise)
    int encoder_press; // the wheel pushed
    int knob_moved[3], knob[3]; // K1-K3: moved this frame, and where to (0..127)
    int fader_moved, fader;     // the slider
    int main_moved, main;       // the main volume knob
    int pad_hit[16];      // a pad pressed this frame: its velocity, else -1
    int pad_pressure[16]; // pressure on a held pad this frame, else -1
    int pad_release[16];  // let go this frame
    int pad_down[16];     // held now
};

// What the panel holds between frames.
struct input_dev {
    unsigned held;
    int pad_down[16];
    long long pad_on_ms[16]; // when each pad last went down, to tell a bounce from a press
    unsigned pad_bounces;    // repeated note-ons for a pad still held, ignored
    unsigned late_up;        // buttons let go in the frame they were pressed: their release, next frame
    // The knobs, the slider and the main volume (K1-K3, main, slider): the last value taken, and
    // one far from it held back until the next reading confirms it (-1 none). A lone reading far
    // off is noise from the panel, not a turn.
    int ctl_last[5], ctl_held[5], ctl_known[5];
    int ctl_confirms[5];   // readings so far that agree with the one held back
    long long ctl_ms;      // the last knob, slider or wheel movement (or wheel push): pads close
                           // to them shake while they are worked, see CTL_GUARD
    int pad_on_vel[16];    // the velocity of each pad's last press: a light hit just after it on
                           // a pad near it is that pad shaking, not a press
    unsigned rejected;     // readings and pad hits thrown away as noise
    // The page buttons (CONFIRM_BUTTONS in panel.c) count only as a whole press: their note-on
    // waits here until the release comes, at least CONFIRM_MIN_MS later. A stray note-on alone,
    // or a note-off alone, is not a press.
    unsigned confirm_pending, confirm_shift;
    long long confirm_ms[32];
    unsigned ghosts;       // page-button note-ons never confirmed, for the log
};

// A new frame: nothing happened yet, held things as the panel holds them (dev NULL: nothing).
void input_begin(struct input *in, const struct input_dev *dev);
// The releases held over from the last frame (input_dev's late_up), into this one. 1 if any.
int input_late_releases(struct input_dev *dev, struct input *in);
// One message from the panel (three bytes, status first) into the frame and the panel state.
void input_push(struct input_dev *dev, struct input *in, const unsigned char m[3]);
int input_down(const struct input *in, int button);

// A frame to draw. px is the whole display, one 0xRRGGBB word a pixel, row after row.
struct ui {
    uint32_t *px;
    struct {
        unsigned char index, r, g, b; // 0..127 each
    } led[16];
    int nled;                       // pads whose light to set this frame
    int meter_left, meter_right;    // the output meters: five lights each, a bit a light
    int track_led;                  // the track lights A-H: 0..7, or -1 for none
    unsigned button_mask;           // buttons whose light to set: bit (1u << IN_*)
    unsigned char button_level[IN_N]; // 0 off, 1 dim, 2 bright
    int64_t now_ms;                 // the time; tests set it, 0 reads the clock
    int wake_ms;                    // another frame in this long, even with no input; 0 none
    int quit;                       // stop after this frame
};

void ui_wake(struct ui *u, int ms); // another frame within ms (the soonest asked wins)
void ui_quit(struct ui *u);
void ui_clear(struct ui *u, uint32_t color);
void ui_led(struct ui *u, int pad, int r, int g, int b);
void ui_button_led(struct ui *u, int button, int level);
void ui_meter(struct ui *u, int left, int right);

// The frame function: 1 to stop.
typedef int (*ui_frame)(struct ui *u, const struct input *in, void *state);

// Opens the display and the panel, and runs frames until one says stop. 0, or 1 when the
// display or panel could not be opened.
int panel_run(ui_frame frame, void *state);
// The two-colour buttons' lights: 0 both colours, 1 the first only, 2 the second only.
void panel_set_bicolour_style(int style);
unsigned panel_pad_bounces(void); // pad bounces ignored so far, for the log
unsigned panel_rejected(void);    // knob jumps and ghost pad hits ignored so far, for the log
int panel_take_note(char *out, size_t n); // a KNOB FX press and the raw bytes before it, once, for the log
// The lightest pad hit that counts (velocity 1..60; default 12).
void panel_set_pad_threshold(int velocity);
