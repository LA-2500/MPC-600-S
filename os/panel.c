// The MPC Sample's front panel: see panel.h.
//
// The display is a 320 x 240 SPI panel behind the kernel's DRM driver (/dev/dri/card0). We
// draw into our own buffer; rows that changed are copied into the driver's buffer and sent with
// a DIRTYFB. Sending ties up the SPI bus (a full screen takes about 50 ms), so it happens on a
// thread of its own and the OS never waits for it. The copy into the driver's buffer has to be
// made on that same thread: the cache maintenance in the ioctl only covers the calling CPU's
// writes, and pixels written from another thread can reach the panel as black.
//
// The panel's controls and lights are a MIDI port (the serial device the kernel names
// platform-fe670000.serial). It sends:
//   pads      channel 10 note on/off, and polyphonic pressure while held
//   buttons   channel 1 note on (velocity 127) and note off
//   controls  channel 1 CC: K1-K3 16-18, main volume 19, slider 20, the wheel 100
//             (1 a step clockwise, 127 a step back)
// and takes:
//   pads      SysEx F0 47 7F 5C 65 00 04 pad r g b F7, each colour 0..127
//   buttons   channel 1 CC, the button's note number: 0 off, 1 dim, 2 bright; the two-colour
//             ones take 3 x (first colour) + (second colour)
//   meters    channel 1 CC 83 (left) and 84 (right), five lights as bits
//   tracks    channel 1 CC 57-64, the lights A-H, 127 on
//   and channel 16 CC 0 and 1 at 127 power the meter and track lights, 0 turns them off.
#include "panel.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

// ---- What the controls send ----

const unsigned char INPUT_PAD_NOTE[16] = {37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53};

static const unsigned char BUTTON_NOTE[IN_N] = {
    [IN_PLAY] = 82,  [IN_STOP] = 81,         [IN_SEQ_REC] = 80,     [IN_SEQ] = 7,       [IN_SAMPLE] = 6,
    [IN_SAMPLE_REC] = 73, [IN_SAMPLE_SELECT] = 36, [IN_SHIFT] = 49, [IN_ERASE] = 9,     [IN_NOTE_REPEAT] = 11,
    [IN_TAP_TEMPO] = 53,  [IN_LEVELS] = 40,  [IN_LOOP] = 74,        [IN_CHOP] = 75,     [IN_MUTE] = 43,
    [IN_PAD_FX] = 1, [IN_KNOB_FX] = 0,       [IN_PAD_BANK] = 35,    [IN_B1] = 12,       [IN_B2] = 13,
    [IN_B3] = 14,    [IN_MINUS] = 55,        [IN_PLUS] = 54,        [IN_ENCODER] = 111,
};

// The buttons with a two-colour light.
static int two_colour(int button) {
    switch (button) {
    case IN_KNOB_FX: case IN_PAD_FX: case IN_SAMPLE: case IN_SEQ: case IN_ERASE: case IN_SAMPLE_SELECT:
    case IN_LEVELS: case IN_MUTE: case IN_TAP_TEMPO: case IN_LOOP: case IN_CHOP:
        return 1;
    default:
        return 0;
    }
}

static long long clock_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// ---- Input ----

int input_down(const struct input *in, int button) {
    return button >= 0 && button < IN_N && (in->down >> button) & 1;
}

void input_begin(struct input *in, const struct input_dev *dev) {
    memset(in, 0, sizeof *in);
    for (int p = 0; p < 16; p++) in->pad_hit[p] = in->pad_pressure[p] = -1;
    if (!dev) return;
    in->held = dev->held;
    in->shift = (in->held >> IN_SHIFT) & 1;
    memcpy(in->pad_down, dev->pad_down, sizeof in->pad_down);
}

static int pad_threshold = 30; // presses lighter than this are noise (Pad Threshold in Settings)

void panel_set_pad_threshold(int velocity) {
    pad_threshold = velocity < 1 ? 1 : velocity > 60 ? 60 : velocity;
}

// A pad hit shakes the panel, and the pads around it send light note-ons of their own. A note-on
// within CROSS_MS of a hit on a neighbouring pad (side or corner) and under 60% of its velocity is
// that; from a pad further off, only one under a third (and light) is, so two hands still play.
#define CROSS_MS 40
#define CTL_GUARD_MS 400 // after a knob, slider or wheel movement
#define CTL_GUARD_VEL 80 // pad hits lighter than this are taken as the panel shaking
// A pad hit shakes the panel too: a lone wheel step within WHEEL_SHAKE_MS after one, or a press
// of the wheel, B1-B3 or -/+ within BUTTON_SHAKE_MS, is that, not a hand (they moved the cursor
// or changed the page by themselves). A turn under way, steps close together, is let through.
#define WHEEL_SHAKE_MS 150
#define BUTTON_SHAKE_MS 80

static int crosstalk(const struct input_dev *dev, int p, int value, long long now) {
    for (int q = 0; q < 16; q++) {
        if (q == p || dev->pad_on_vel[q] <= 0 || now - dev->pad_on_ms[q] >= CROSS_MS) continue;
        int dr = p / 4 - q / 4, dc = p % 4 - q % 4, near = dr >= -1 && dr <= 1 && dc >= -1 && dc <= 1;
        if (near ? value * 10 < dev->pad_on_vel[q] * 6 : value * 3 < dev->pad_on_vel[q] && value < 40) return 1;
    }
    return 0;
}

static void pad_message(struct input_dev *dev, struct input *in, int kind, int note, int value) {
    int p = 0;
    while (p < 16 && INPUT_PAD_NOTE[p] != note) p++;
    if (p == 16) return;
    if (kind == 0xA0) { // pressure on a held pad
        in->pad_pressure[p] = value;
        return;
    }
    if (kind == 0x90 && value > 0 && value < pad_threshold) { // too light: noise, or a brush
        if (dev) dev->rejected++;
        return;
    }
    // PAD BANK held: pads 1-8 jump to banks A-H, and play nothing; 9-16 do nothing.
    if (dev && kind == 0x90 && value > 0 && (dev->confirm_pending >> IN_PAD_BANK & 1)) {
        if (p < 8) in->bank_jump = p + 1, dev->bank_jumped = 1;
        dev->pad_eaten[p] = 1;
        return;
    }
    if (dev && dev->pad_eaten[p] && kind != 0x90) return; // its pressure
    if (dev && dev->pad_eaten[p] && kind == 0x90 && value == 0) {
        dev->pad_eaten[p] = 0;
        return;
    }
    if (kind == 0x90 && value > 0) { // pressed
        long long now = clock_ms();
        if (dev && dev->pad_down[p] && now - dev->pad_on_ms[p] < 100) { // sent again, still held
            dev->pad_bounces++;
            return;
        }
        // Turning a knob, moving the slider, or turning or pushing the wheel presses on the panel too:
        // light pad hits meanwhile are that, not a press.
        if (dev && value < CTL_GUARD_VEL && (now - dev->ctl_ms < CTL_GUARD_MS || (dev->held >> IN_ENCODER & 1))) {
            dev->rejected++;
            return;
        }
        if (dev && crosstalk(dev, p, value, now)) { // another pad's hit shaking this one
            dev->rejected++;
            return;
        }
        if (dev) dev->pad_on_vel[p] = value;
        if (dev) dev->pad_down[p] = 1, dev->pad_on_ms[p] = now, dev->pad_hit_ms = now;
        in->pad_hit[p] = value, in->pad_down[p] = 1;
    } else if (kind == 0x80 || (kind == 0x90 && value == 0)) {
        if (dev && dev->pad_eaten[p]) { // the release of a pad pressed for a bank jump
            dev->pad_eaten[p] = 0;
            return;
        }
        if (dev) dev->pad_down[p] = 0;
        in->pad_release[p] = 1, in->pad_down[p] = 0;
    }
}

// The panel's last raw bytes, and a note of them when KNOB FX is pressed: a tester saw Input
// Effects open by itself, so the log shows what arrived just before (panel_take_note).
static unsigned char raw_ring[48];
static unsigned raw_count;
static char raw_note[400];
static long long raw_start;

static void raw_keep(unsigned char c) {
    raw_ring[raw_count++ % sizeof raw_ring] = c;
}

static void raw_snapshot(const char *what) {
    if (!raw_start) raw_start = clock_ms();
    int at = snprintf(raw_note, sizeof raw_note, "t=%llds %s; last bytes:", (clock_ms() - raw_start) / 1000, what);
    unsigned n = raw_count < sizeof raw_ring ? raw_count : (unsigned)sizeof raw_ring;
    for (unsigned i = raw_count - n; i != raw_count && at < (int)sizeof raw_note - 4; i++)
        at += snprintf(raw_note + at, sizeof raw_note - at, " %02x", raw_ring[i % sizeof raw_ring]);
}

int panel_take_note(char *out, size_t n) {
    if (!raw_note[0]) return 0;
    snprintf(out, n, "%s", raw_note);
    raw_note[0] = 0;
    return 1;
}

// Buttons that only open a page (none of them is held for anything): they act on a whole
// press, note-on then note-off, so a stray message from the panel cannot open a page by itself.
// The others act at once: SHIFT, SAMPLE, ERASE, NOTE REPEAT, B1-B3 and -/+ are held with other
// controls, and PLAY, STOP, REC and TAP TEMPO keep time.
#define CONFIRM_BUTTONS ((1u << IN_KNOB_FX) | (1u << IN_PAD_FX) | (1u << IN_MUTE) | (1u << IN_LEVELS) | \
                         (1u << IN_LOOP) | (1u << IN_CHOP) | (1u << IN_SAMPLE_SELECT) | (1u << IN_SEQ) | \
                         (1u << IN_SAMPLE_REC) | (1u << IN_PAD_BANK))
#define CONFIRM_MIN_MS 12   // a finger holds a button longer than this; a glitch does not
#define CONFIRM_MAX_MS 4000 // a release this long after is not that press's

static void button_message(struct input_dev *dev, struct input *in, int kind, int note, int value) {
    int b = 0;
    while (b < IN_N && BUTTON_NOTE[b] != note) b++;
    if (b == IN_N) return;
    unsigned bit = 1u << b;
    if (dev && b == IN_KNOB_FX && kind == 0x90) raw_snapshot(value == 127 ? "KNOB FX note-on" : "KNOB FX note");
    if (dev && (bit & CONFIRM_BUTTONS)) {
        long long now = clock_ms();
        if (kind == 0x90 && value == 127) { // a press, maybe: kept until its release
            if (dev->confirm_pending & bit) dev->ghosts++; // the last one never released: not a press
            dev->confirm_pending |= bit;
            dev->confirm_ms[b] = now;
            if (b == IN_PAD_BANK) dev->bank_jumped = 0;
            if (in->shift) dev->confirm_shift |= bit;
            else dev->confirm_shift &= ~bit;
        } else if (kind == 0x80 || (kind == 0x90 && value == 0)) {
            if (!(dev->confirm_pending & bit)) return; // a release with no press: nothing
            dev->confirm_pending &= ~bit;
            if (b == IN_PAD_BANK && dev->bank_jumped) { // it was held for a jump: no step on top of it
                dev->bank_jumped = 0;
                return;
            }
            long long held = now - dev->confirm_ms[b];
            if (held < CONFIRM_MIN_MS || held > CONFIRM_MAX_MS) {
                dev->ghosts++;
                if (b == IN_KNOB_FX) raw_snapshot("KNOB FX ignored (not a whole press)");
                return;
            }
            // The whole press, now: down this frame, let go the next, with SHIFT as it was when
            // the button went down.
            in->down |= bit, in->held |= bit;
            dev->late_up |= bit;
            if (dev->confirm_shift & bit) in->held |= 1u << IN_SHIFT;
            in->shift = (in->held >> IN_SHIFT) & 1;
            if (b == IN_KNOB_FX) raw_snapshot("KNOB FX pressed");
        }
        return;
    }
    if (kind == 0x90 && value == 127) { // a press (other velocities are not the panel's presses)
        // The cursor's and the page's buttons, just after a pad press: the panel shaking.
        unsigned shaken = (1u << IN_ENCODER) | (1u << IN_B1) | (1u << IN_B2) | (1u << IN_B3) | (1u << IN_MINUS) | (1u << IN_PLUS);
        if (dev && (bit & shaken) && !(dev->held & bit) && dev->pad_hit_ms && clock_ms() - dev->pad_hit_ms < BUTTON_SHAKE_MS) {
            dev->ghosts++;
            return; // and its release, with no press before it, is nothing either
        }
        if (!(in->held & bit)) {
            in->down |= bit;
            if (b == IN_ENCODER) in->encoder_press = 1;
        }
        if (b == IN_ENCODER && dev) dev->ctl_ms = clock_ms();
        in->held |= bit;
        if (dev) dev->held |= bit, dev->confirm_ms[b] = clock_ms();
    } else if (kind == 0x80 || (kind == 0x90 && value == 0)) {
        // A release with no press before it is a stray message, not a release: ERASE, for one,
        // acts on its release, and a lone one opened the Erase page.
        if (dev && !(dev->held & bit) && !(in->held & bit)) {
            dev->ghosts++;
            return;
        }
        // ERASE's tap acts on the release: shorter than a finger's press, it was not one.
        if (dev && b == IN_ERASE && clock_ms() - dev->confirm_ms[b] < CONFIRM_MIN_MS) {
            dev->ghosts++;
            dev->held &= ~bit, in->held &= ~bit, in->down &= ~bit;
            dev->late_up &= ~bit;
            in->shift = (in->held >> IN_SHIFT) & 1;
            return;
        }
        if ((in->down & bit) && dev) { // pressed this same frame: a quick tap, released next frame
            dev->late_up |= bit;
        } else {
            in->held &= ~bit;
            if (dev) dev->held &= ~bit;
            in->up |= bit;
        }
    }
    in->shift = (in->held >> IN_SHIFT) & 1;
}

int input_late_releases(struct input_dev *dev, struct input *in) {
    unsigned bits = dev->late_up;
    if (!bits) return 0;
    dev->late_up = 0;
    dev->held &= ~bits;
    in->held &= ~bits;
    in->up |= bits;
    in->shift = (in->held >> IN_SHIFT) & 1;
    return 1;
}

// A knob, the slider or the main volume: taken if near the last value, or once readings confirm one
// held back. A reading far from the last is held back: alone, it was noise. The main volume needs two
// confirming readings, not one: a dropout to 0 sent twice in a row would otherwise silence the output
// until the knob is touched again. A real turn passes through the values in between.
#define CTL_JUMP 16
// The main volume, more so: a jump far from where it is must hold for MAIN_HOLD_MS, over at least
// three more readings, before it is taken. A real turn, however quick, comes in small steps (each
// taken at once); a stray reading repeated (0, 0, 0) silenced the MPC until the knob was touched.
#define MAIN_HOLD_MS 100
static int control_value(struct input_dev *dev, int c, int value) {
    if (!dev) return value;
    int need = c == 3 ? 3 : 1;
    long long now = clock_ms();
    if (!dev->ctl_known[c] || abs(value - dev->ctl_last[c]) <= CTL_JUMP) {
        dev->ctl_known[c] = 1, dev->ctl_last[c] = value, dev->ctl_held[c] = -1, dev->ctl_confirms[c] = 0;
        return value;
    }
    if (dev->ctl_held[c] >= 0 && abs(value - dev->ctl_held[c]) <= 6) {
        if (++dev->ctl_confirms[c] >= need && (c != 3 || now - dev->ctl_held_ms[c] >= MAIN_HOLD_MS)) {
            if (c == 3) { // for the log: a jump of the main volume taken, and from where
                if (!raw_start) raw_start = now;
                snprintf(raw_note, sizeof raw_note, "t=%llds MAIN volume jump %d -> %d taken (held %lld ms, %d readings)",
                         (now - raw_start) / 1000, dev->ctl_last[c], value, now - dev->ctl_held_ms[c], dev->ctl_confirms[c] + 1);
            }
            dev->ctl_last[c] = value, dev->ctl_held[c] = -1, dev->ctl_confirms[c] = 0;
            return value;
        }
        dev->ctl_held[c] = value;
        return -1;
    }
    if (dev->ctl_held[c] >= 0) dev->rejected++; // the one held back was not confirmed
    dev->ctl_held[c] = value, dev->ctl_confirms[c] = 0, dev->ctl_held_ms[c] = now;
    return -1;
}

static void control_message(struct input_dev *dev, struct input *in, int cc, int value) {
    if (dev && ((cc >= 16 && cc <= 20) || cc == 100)) dev->ctl_ms = clock_ms(); // a control being worked
    if (cc == 100) {
        int step = value < 64 ? value : value - 128; // two's complement steps
        if (dev) {
            long long now = clock_ms();
            int lone = step >= -1 && step <= 1 && now - dev->wheel_ms > WHEEL_SHAKE_MS;
            if (lone && dev->pad_hit_ms && now - dev->pad_hit_ms < WHEEL_SHAKE_MS) { // a pad's shake, not a turn
                dev->rejected++;
                return;
            }
            dev->wheel_ms = now;
        }
        in->encoder += step;
        return;
    }
    int c = cc >= 16 && cc <= 18 ? cc - 16 : cc == 19 ? 3 : cc == 20 ? 4 : -1;
    if (c < 0 || (value = control_value(dev, c, value)) < 0) return;
    if (c < 3) in->knob_moved[c] = 1, in->knob[c] = value;
    else if (c == 3) in->main_moved = 1, in->main = value;
    else in->fader_moved = 1, in->fader = value;
}

void input_push(struct input_dev *dev, struct input *in, const unsigned char m[3]) {
    int kind = m[0] & 0xF0, channel = m[0] & 0x0F, d1 = m[1] & 0x7F, d2 = m[2] & 0x7F;
    if (channel == 9 && (kind == 0x90 || kind == 0x80 || kind == 0xA0)) pad_message(dev, in, kind, d1, d2);
    else if (channel == 0 && (kind == 0x90 || kind == 0x80)) button_message(dev, in, kind, d1, d2);
    else if (channel == 0 && kind == 0xB0) control_message(dev, in, d1, d2);
}

// MIDI bytes into whole messages: running status kept, SysEx and real-time bytes passed over.
struct reader {
    unsigned char status, data[2];
    int have, sysex, running; // running: the status is carried over from the last message
};

static int reader_byte(struct reader *r, unsigned char c, unsigned char out[3]) {
    if (c >= 0xF8) return 0; // real time: passes through anything
    if (c == 0xF0) {
        r->sysex = 1, r->status = 0;
        return 0;
    }
    if (c >= 0xF1) { // the end of SysEx, or a system common message: neither is ours
        r->sysex = 0, r->status = 0;
        return 0;
    }
    if (c & 0x80) {
        r->sysex = 0, r->status = c, r->have = 0, r->running = 0;
        return 0;
    }
    if (r->sysex || !r->status) return 0;
    r->data[r->have++] = c;
    int need = (r->status & 0xE0) == 0xC0 ? 1 : 2; // program change and channel pressure: one byte
    if (r->have < need) return 0;
    int was_running = r->running;
    r->have = 0, r->running = 1; // running status: the next data bytes are another of the same
    // The panel pads its packets with zeros after each message, and gives every real message its
    // own status byte. Zeros under running status are that padding: after a note, they read as
    // note 0 (KNOB FX) at velocity 0, a release a moment after every press, so KNOB FX never
    // made a whole press (and opened Input Effects by itself before page buttons waited for one).
    if (was_running && !r->data[0] && (need == 1 || !r->data[1])) return 0;
    out[0] = r->status, out[1] = r->data[0], out[2] = need == 2 ? r->data[1] : 0;
    return 1;
}

// For the tests: bytes as they come from the panel, into messages (a reader kept between calls).
int panel_parse(const unsigned char *b, int n, unsigned char out[][3], int max) {
    static struct reader r;
    int k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (reader_byte(&r, b[i], out[k])) k++;
    return k;
}

// ---- Drawing ----

void ui_wake(struct ui *u, int ms) {
    if (ms < 1) ms = 1;
    if (!u->wake_ms || ms < u->wake_ms) u->wake_ms = ms;
}

void ui_quit(struct ui *u) {
    u->quit = 1;
}

void ui_clear(struct ui *u, uint32_t color) {
    for (int i = 0; i < UI_W * UI_H; i++) u->px[i] = color;
}

void ui_led(struct ui *u, int pad, int r, int g, int b) {
    if (pad < 0 || pad >= 16 || u->nled >= 16) return;
    u->led[u->nled].index = (unsigned char)pad;
    u->led[u->nled].r = (unsigned char)(r < 0 ? 0 : r > 127 ? 127 : r);
    u->led[u->nled].g = (unsigned char)(g < 0 ? 0 : g > 127 ? 127 : g);
    u->led[u->nled].b = (unsigned char)(b < 0 ? 0 : b > 127 ? 127 : b);
    u->nled++;
}

void ui_button_led(struct ui *u, int button, int level) {
    if (button < 0 || button >= IN_N) return;
    u->button_mask |= 1u << button;
    u->button_level[button] = (unsigned char)(level < 0 ? 0 : level > 2 ? 2 : level);
}

void ui_meter(struct ui *u, int left, int right) {
    u->meter_left = left & 0x1F, u->meter_right = right & 0x1F;
}

// ---- The display ----

static struct {
    int fd, ok;
    uint32_t *mem; // the driver's buffer
    int pitch;     // its row length, in pixels
    unsigned fb, size;
    // Shared with the sending thread, under lock: the newest frame, and the rows not yet sent.
    pthread_mutex_t lock;
    pthread_cond_t wake;
    uint32_t shown[UI_W * UI_H];
    int top, bottom; // rows top..bottom-1 to send; top >= bottom: none
    int stop, running;
    pthread_t thread;
} disp = {.fd = -1, .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER};

static int ioctl_retry(int fd, unsigned long request, void *arg) {
    int r;
    do r = ioctl(fd, request, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static void display_close(void) {
    if (disp.running) {
        pthread_mutex_lock(&disp.lock);
        disp.stop = 1;
        pthread_cond_signal(&disp.wake);
        pthread_mutex_unlock(&disp.lock);
        pthread_join(disp.thread, 0);
        disp.running = 0;
    }
    if (disp.mem) { // dark, so the stock app takes over a clean screen
        memset(disp.mem, 0, disp.size);
        struct drm_clip_rect all = {0, 0, UI_W, UI_H};
        struct drm_mode_fb_dirty_cmd dirty = {.fb_id = disp.fb, .num_clips = 1, .clips_ptr = (uintptr_t)&all};
        ioctl_retry(disp.fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
        munmap(disp.mem, disp.size);
        disp.mem = 0;
    }
    if (disp.fd >= 0) {
        ioctl(disp.fd, DRM_IOCTL_DROP_MASTER, 0);
        close(disp.fd);
        disp.fd = -1;
    }
    disp.ok = 0;
}

static void *display_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&disp.lock);
    for (;;) {
        while (!disp.stop && disp.top >= disp.bottom) pthread_cond_wait(&disp.wake, &disp.lock);
        if (disp.stop) break;
        int top = disp.top, bottom = disp.bottom;
        for (int y = top; y < bottom; y++) memcpy(disp.mem + y * disp.pitch, disp.shown + y * UI_W, UI_W * 4);
        disp.top = UI_H, disp.bottom = 0;
        pthread_mutex_unlock(&disp.lock); // the OS draws on while these rows go out
        struct drm_clip_rect band = {0, (unsigned short)top, UI_W, (unsigned short)bottom};
        struct drm_mode_fb_dirty_cmd dirty = {.fb_id = disp.fb, .num_clips = 1, .clips_ptr = (uintptr_t)&band};
        ioctl_retry(disp.fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
        pthread_mutex_lock(&disp.lock);
    }
    pthread_mutex_unlock(&disp.lock);
    return 0;
}

static int display_open(void) {
    disp.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (disp.fd < 0) return -1;
    // The stock app may still hold the display for a moment: wait for it, up to a second.
    int master = 0;
    for (int i = 0; i < 50 && !master; i++) {
        master = ioctl(disp.fd, DRM_IOCTL_SET_MASTER, 0) == 0;
        if (!master && errno != EBUSY) break;
        if (!master) usleep(20000);
    }
    uint32_t crtc[4], conn[4];
    struct drm_mode_card_res res = {.crtc_id_ptr = (uintptr_t)crtc, .connector_id_ptr = (uintptr_t)conn, .count_crtcs = 4,
                                    .count_connectors = 4};
    struct drm_mode_modeinfo modes[8];
    struct drm_mode_get_connector c = {0};
    if (!master || ioctl_retry(disp.fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || !res.count_crtcs || !res.count_connectors) goto fail;
    c.connector_id = conn[0];
    if (ioctl_retry(disp.fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0 || c.count_modes < 1 || c.count_modes > 8) goto fail;
    unsigned nmodes = c.count_modes;
    memset(&c, 0, sizeof c);
    c.connector_id = conn[0], c.count_modes = nmodes, c.modes_ptr = (uintptr_t)modes;
    if (ioctl_retry(disp.fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) goto fail;
    int m = -1;
    for (unsigned i = 0; i < nmodes && m < 0; i++)
        if (modes[i].hdisplay == UI_W && modes[i].vdisplay == UI_H) m = (int)i;
    if (m < 0) goto fail;
    struct drm_mode_create_dumb dumb = {.width = UI_W, .height = UI_H, .bpp = 32};
    if (ioctl_retry(disp.fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) goto fail;
    struct drm_mode_fb_cmd2 fb = {.width = UI_W, .height = UI_H, .pixel_format = DRM_FORMAT_XRGB8888, .handles = {dumb.handle},
                                  .pitches = {dumb.pitch}};
    if (ioctl_retry(disp.fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) goto fail;
    struct drm_mode_map_dumb map = {.handle = dumb.handle};
    if (ioctl_retry(disp.fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) goto fail;
    void *mem = mmap(0, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, disp.fd, (off_t)map.offset);
    if (mem == MAP_FAILED) goto fail;
    disp.mem = mem, disp.size = (unsigned)dumb.size, disp.pitch = (int)(dumb.pitch / 4), disp.fb = fb.fb_id;
    memset(disp.mem, 0, disp.size);
    struct drm_mode_crtc set = {.crtc_id = crtc[0], .fb_id = fb.fb_id, .set_connectors_ptr = (uintptr_t)&conn[0],
                                .count_connectors = 1, .mode = modes[m], .mode_valid = 1};
    if (ioctl_retry(disp.fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) goto fail;
    memset(disp.shown, 0, sizeof disp.shown);
    disp.top = UI_H, disp.bottom = 0, disp.stop = 0;
    disp.ok = 1;
    disp.running = pthread_create(&disp.thread, 0, display_thread, 0) == 0;
    return 0;
fail:
    display_close();
    return -1;
}

// The rows of the frame that differ from what is shown, handed to the sending thread.
static void display_frame(const uint32_t *frame) {
    if (!disp.ok) return;
    pthread_mutex_lock(&disp.lock);
    int top = UI_H, bottom = 0;
    for (int y = 0; y < UI_H; y++) {
        const uint32_t *row = frame + y * UI_W;
        uint32_t *was = disp.shown + y * UI_W;
        if (memcmp(row, was, UI_W * 4) == 0) continue;
        memcpy(was, row, UI_W * 4);
        if (y < top) top = y;
        bottom = y + 1;
    }
    if (top < bottom) {
        if (top < disp.top) disp.top = top;
        if (bottom > disp.bottom) disp.bottom = bottom;
        pthread_cond_signal(&disp.wake);
    }
    pthread_mutex_unlock(&disp.lock);
}

// ---- The panel's port and its lights ----

static struct {
    int fd;
    int style; // two-colour buttons: 0 both colours, 1 the first, 2 the second
    unsigned char pad[16][3];
    int pad_known[16];
    unsigned char button[IN_N];
    int button_known[IN_N];
    int left, right, track, powered;
} port = {.fd = -1};

void panel_set_bicolour_style(int style) {
    port.style = style;
    memset(port.button_known, 0, sizeof port.button_known); // sent again in the new style
}

static void port_send(const unsigned char *b, int n) {
    while (port.fd >= 0 && n > 0) {
        ssize_t w = write(port.fd, b, (size_t)n);
        if (w > 0) {
            b += w, n -= (int)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            usleep(200); // the port is full for a moment
        } else {
            return;
        }
    }
}

static void port_cc(int channel, int cc, int value) {
    unsigned char m[3] = {(unsigned char)(0xB0 | channel), (unsigned char)cc, (unsigned char)value};
    port_send(m, 3);
}

static int port_open(void) {
    char link[128];
    ssize_t n = readlink("/dev/snd/by-path/platform-fe670000.serial", link, sizeof link - 1);
    if (n <= 0) return -1;
    link[n] = 0;
    const char *c = strstr(link, "controlC");
    if (!c) return -1;
    char path[40];
    snprintf(path, sizeof path, "/dev/snd/midiC%dD0", atoi(c + 8));
    port.fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    return port.fd >= 0 ? 0 : -1;
}

static void lights_power(int on) {
    if (on == port.powered) return;
    port_cc(15, 0, on ? 127 : 0);
    port_cc(15, 1, on ? 127 : 0);
    port.powered = on;
}

static void lights(const struct ui *u) {
    for (int i = 0; i < u->nled; i++) {
        int p = u->led[i].index;
        unsigned char rgb[3] = {u->led[i].r, u->led[i].g, u->led[i].b};
        if (port.pad_known[p] && !memcmp(port.pad[p], rgb, 3)) continue;
        unsigned char m[12] = {0xF0, 0x47, 0x7F, 0x5C, 0x65, 0x00, 0x04, (unsigned char)p, rgb[0], rgb[1], rgb[2], 0xF7};
        port_send(m, 12);
        memcpy(port.pad[p], rgb, 3), port.pad_known[p] = 1;
    }
    for (int b = 0; b < IN_N; b++) {
        if (!(u->button_mask >> b & 1)) continue;
        int level = u->button_level[b], value = level;
        if (two_colour(b) && level) value = port.style == 1 ? 3 * level : port.style == 2 ? level : 4 * level;
        if (port.button_known[b] && port.button[b] == value) continue;
        port_cc(0, BUTTON_NOTE[b], value);
        port.button[b] = (unsigned char)value, port.button_known[b] = 1;
    }
    int track = u->track_led >= 0 && u->track_led < 8 ? u->track_led : -1;
    if (u->meter_left || u->meter_right || track >= 0) lights_power(1);
    if (u->meter_left != port.left) port_cc(0, 83, u->meter_left), port.left = u->meter_left;
    if (u->meter_right != port.right) port_cc(0, 84, u->meter_right), port.right = u->meter_right;
    if (track != port.track) {
        if (port.track >= 0) port_cc(0, 57 + port.track, 0);
        if (track >= 0) port_cc(0, 57 + track, 127);
        port.track = track;
    }
    if (!u->meter_left && !u->meter_right && track < 0) lights_power(0);
}

static void lights_off(void) {
    struct ui dark = {.track_led = -1, .nled = 16, .button_mask = (1u << IN_N) - 1};
    for (int p = 0; p < 16; p++) dark.led[p].index = (unsigned char)p;
    lights(&dark);
    lights_power(0);
}

// ---- The frame loop ----

static struct input_dev *running_dev;

unsigned panel_pad_bounces(void) {
    return running_dev ? running_dev->pad_bounces : 0;
}

unsigned panel_rejected(void) {
    return running_dev ? running_dev->rejected : 0;
}

int panel_run(ui_frame frame, void *state) {
    if (!frame || display_open() != 0) return 1;
    if (port_open() != 0) {
        display_close();
        return 1;
    }
    port.track = -1, port.powered = -1;
    static uint32_t px[UI_W * UI_H];
    struct input_dev dev = {0};
    struct reader rd = {0};
    struct input in;
    running_dev = &dev;
    lights_off();
    input_begin(&in, &dev);
    int stop = 0, wake = 0;
    long long drawn = clock_ms();
    for (;;) {
        // A frame now: the first, or after input, or when the last one asked to be woken.
        struct ui u = {.px = px, .track_led = -1};
        stop = frame(&u, &in, state) || u.quit;
        display_frame(px);
        lights(&u);
        wake = u.wake_ms, drawn = clock_ms();
        if (stop) break;
        input_begin(&in, &dev);
        int got = input_late_releases(&dev, &in); // a quick tap's release: a frame of its own, now
        while (!got) {
            int wait = 1000;
            if (wake > 0) {
                long long left = drawn + wake - clock_ms();
                wait = left < 0 ? 0 : (int)left;
            }
            struct pollfd pfd = {.fd = port.fd, .events = POLLIN};
            int r = poll(&pfd, 1, wait);
            if (r < 0 && errno != EINTR) {
                stop = 1;
                break;
            }
            if (r > 0) { // everything waiting, into this frame
                unsigned char buf[256], msg[3];
                ssize_t n;
                while ((n = read(port.fd, buf, sizeof buf)) > 0)
                    for (ssize_t i = 0; i < n; i++) {
                        raw_keep(buf[i]);
                        if (reader_byte(&rd, buf[i], msg)) input_push(&dev, &in, msg), got = 1;
                    }
            }
            if (wake > 0 && clock_ms() >= drawn + wake) got = 1; // asked for: a frame with no input
        }
        if (stop) break;
    }
    lights_off();
    running_dev = 0;
    close(port.fd);
    port.fd = -1;
    display_close();
    return 0;
}
