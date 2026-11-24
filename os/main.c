// Device entry point: start audio, then hand the screen and panel to the OS.
#include "alsa_out.h"
#include "os.h"
#include "midi_out.h"
#include "power_key.h"
#include "wav.h"
#include "usbmode.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static struct engine engine;
static struct os os;
static char audio_status[96];

// There is no shell on a plain card, so the result goes next to the program.
static void log_line(const char *text) {
    const char *dir = getenv("MPC_APP_DIR");
    char path[512];
    if (usbmode_card_away()) return; // USB DATA MODE: the card is the computer's
    snprintf(path, sizeof path, "%s/mpc-os.log", dir ? dir : "/tmp");
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", text);
    fflush(f);
    fsync(fileno(f)); // the MPC can lose power at any moment, so reach the card now
    fclose(f);
}

// Each frame: the speaker switch, and a line in the log whenever the audio drops out (an xrun),
// a guard silences a blown-up filter or mix, a pad bounce is ignored, or the speaker goes on or off, with the time since
// power-on, so a cut-out can be traced afterwards. At most a line a second.
static void audio_poll(void) {
    alsa_out_speaker_update();
    // USB individual outs, while on: every 5 minutes, what reached the computer and how loud each output was.
    static time_t outs_logged;
    if (os.usb_outs) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - outs_logged >= 300) {
            outs_logged = t.tv_sec;
            char st[200], line[240];
            alsa_out_outs_status(st, sizeof st);
            snprintf(line, sizeof line, "t=%lds %s (state %d, by %s)", (long)t.tv_sec, st, os.usb_outs_state,
                     os.outs_by_track ? "track" : "pad");
            log_line(line);
        }
    }
    char note[400];
    if (panel_take_note(note, sizeof note)) log_line(note);
    static unsigned xr, gt, pb, rj;
    static int spk = -2;
    static time_t logged;
    unsigned x = alsa_out_xruns(), g = engine_guard_trips(), b = panel_pad_bounces(), j = panel_rejected();
    int sp = alsa_out_speaker_on();
    if (x == xr && g == gt && sp == spk && b == pb && j == rj) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec == logged && sp == spk) return;
    logged = now.tv_sec;
    char line[192];
    snprintf(line, sizeof line, "t=%lds dropouts %u (+%u), guard %u (+%u), pad bounces %u, noise ignored %u, speaker %s",
             (long)now.tv_sec, x, x - xr, g, g - gt, b, j, sp > 0 ? "on" : sp == 0 ? "off" : "?");
    log_line(line);
    xr = x, gt = g, spk = sp, pb = b, rj = j;
}

// A recording of the real MPC3000 click, if one is on the card, replaces the built-in one:
// click.wav, and optionally click-accent.wav for the first beat of each bar. They are
// looked for next to the program and at the top of the card.
static struct sample *find_wav(const char *name) {
    const char *dir = getenv("MPC_APP_DIR");
    if (!dir) return 0;
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    struct sample *s = wav_load(path);
    if (s) return s;
    snprintf(path, sizeof path, "%s/../%s", dir, name);
    return wav_load(path);
}

static void load_clicks(void) {
    struct sample *click = find_wav("click.wav");
    struct sample *accent = find_wav("click-accent.wav");
    if (!click) {
        if (accent) sample_free(accent);
        log_line("click: built in");
        return;
    }
    engine_set_click(&engine, 0, click);
    engine_set_click(&engine, 1, accent ? accent : click);
    log_line(accent ? "click: click.wav and click-accent.wav" : "click: click.wav");
}

// MemAvailable from /proc/meminfo, in bytes: how long a sample recording may run.
static long mem_available(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128];
    long kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb * 1024;
}

// USB individual outs: the outs' USB device put on the port or taken off, in a thread (it takes
// a couple of seconds), as many times as the setting changes while it works; then the engine's
// outputs fed to it or not. os.usb_outs_state says how it went: 1 on, 0 off, -1 failed.
static atomic_int outs_busy;
static void *outs_worker(void *arg) {
    (void)arg;
    for (;;) {
        int want = os.usb_outs;
        if (usbmode_card_away()) break; // USB DATA MODE has the port: left as it is
        int rc = usbmode_outs(want, &os.input_src);
        alsa_out_set_usb_outs(want && rc == 0);
        os.usb_outs_state = !want ? 0 : rc == 0 ? 1 : -1;
        log_line(!want ? "USB Indiv Out: off" : rc == 0 ? "USB Indiv Out: on" : "USB Indiv Out: could not start");
        if (want == os.usb_outs) break; // changed again meanwhile: once more
    }
    atomic_store(&outs_busy, 0);
    return 0;
}
static void set_usb_outs(int on) {
    (void)on; // read from os.usb_outs by the worker, which may already be running
    if (atomic_exchange(&outs_busy, 1)) return;
    pthread_t t;
    if (pthread_create(&t, 0, outs_worker, 0) == 0) pthread_detach(t);
    else atomic_store(&outs_busy, 0);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--codec-volume") == 0) {
        int ok = alsa_out_codec_volume(atoi(argv[2])) == 0;
        if (ok && atoi(argv[2]) == 0) usleep(150 * 1000); // the codec's own glide down (4080 samples) is over
        return ok ? 0 : 1;
    }
    mlockall(MCL_CURRENT | MCL_FUTURE); // a page fault would be an audible dropout
    engine_init(&engine);
    os_init(&os, &engine);
    load_clicks();
    // Settings live in MPC600S at the top of the card, outside autorun, so an update keeps them.
    static char settings_path[600];
    const char *app_dir = getenv("MPC_APP_DIR");
    if (app_dir) {
        snprintf(settings_path, sizeof settings_path, "%s/../MPC600S", app_dir);
        mkdir(settings_path, 0755);
        snprintf(settings_path, sizeof settings_path, "%s/../MPC600S/settings.txt", app_dir);
        os.settings_path = settings_path;
        os_load_settings(&os);
        prefs_startup(&os); // Start BPM, TC and Swing, before a project loaded at power-on
        static char data_dir[600]; // Save/Load's folders, and the project loaded at power-on
        snprintf(data_dir, sizeof data_dir, "%s/../MPC600S", app_dir);
        os.data_dir = data_dir;
        disk_install_waves(&os); // the S600 waveforms, on the card for Save/Load's S600 OSC
        disk_autoload(&os);
        infx_power_on(&os); // Input Effects start off and as new, not as last used or as autoloaded
        static char stems_dir[600]; // the Stems models, copied to the card once
        snprintf(stems_dir, sizeof stems_dir, "%s/../MPC600S/stems", app_dir);
        os.stems_dir = stems_dir;
    }
    prefs_apply(&os); // the lights and screen as set (after the LED style hook below too)
    alsa_out_set_master(&os.master); // Master Effects, the last stage
    int failed = alsa_out_start(&engine, &os.main_gain, &os.rec, audio_status, sizeof audio_status);
    log_line(failed ? "audio failed:" : "audio started:");
    log_line(audio_status);
    os.audio_status = audio_status;
    os.mem_available = mem_available;
    os.set_input = alsa_out_set_input;
    alsa_out_set_input(os.input_src); // LINE unless saved otherwise: not what the stock app left
    if (!failed) os.xruns = alsa_out_xruns;
    if (!failed) os.audio_poll = audio_poll;
    // MIDI: the IN/OUT jacks, and USB MIDI (there when the device's MIDI gadget is).
    int ext = midi_port_open(MIDI_PORT_EXT) == 0, usb = midi_port_open(MIDI_PORT_USB) == 0;
    log_line(ext ? "MIDI jacks: open" : "MIDI jacks: not found");
    log_line(usb ? "USB MIDI: open" : "USB MIDI: not found");
    if (ext || usb) {
        os.midi_in_read = midi_in_read;
        os.seq.midi_send = midi_out_send;
        os.midi_ports = midi_ports_enable;
    }
    os.set_led_style = panel_set_bicolour_style;
    os.usbmode = usbmode_hw(&os.input_src); // USB DATA MODE
    os.set_pad_threshold = panel_set_pad_threshold;
    panel_set_pad_threshold(os.pad_threshold);
    if (!failed) os.set_usb_out = alsa_out_set_usb_out, alsa_out_set_usb_out(os.usb_out); // the mix to a computer
    log_line(os.usb_out ? "USB Audio Out: on" : "USB Audio Out: off");
    if (!failed) {
        os.set_usb_outs = set_usb_outs;
        os.set_outs_mix = alsa_out_set_outs_mix;
        if (os.usb_outs) set_usb_outs(1); // the outs' USB device, as last set
    }
    panel_set_bicolour_style(os.led_style);
    static char power_status[32];
    int power_keys = power_key_open();
    snprintf(power_status, sizeof power_status, "Power key: %s", power_keys > 0 ? "found" : "not found");
    log_line(power_status);
    os.power_status = power_status;
    os.power_off = power_off;
    if (power_keys > 0) os.power_pressed = power_key_pressed;
    int rc = panel_run(os_frame, &os);
    // The stock gadget as the stock app knows it: the outs' device off before it starts again.
    for (int i = 0; i < 50 && atomic_load(&outs_busy); i++) usleep(100 * 1000);
    if (!failed && os.usb_outs_state == 1 && !usbmode_card_away()) usbmode_outs(0, &os.input_src);
    if (!failed) {
        char line[64];
        snprintf(line, sizeof line, "exit, dropouts: %u", alsa_out_xruns());
        log_line(line);
    }
    alsa_out_stop();
    return rc;
}
