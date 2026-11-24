// USB individual-outs test for the MPC Sample. Run once from its own autorun package.
//
// 1. Writes the stock USB gadget's settings (UAC2 audio, MIDI) to usbaudiotest.txt next to it.
// 2. Puts a gadget of our own on the port in its place: UAC2 audio with 18 channels to the
//    computer (16 outputs, then the main mix on 17-18) and 2 from it, 44.1 kHz, 32-bit, plus
//    USB MIDI, so the computer shows "MPC 600 S" with 18 inputs.
// 3. Rounds of 2, 8, 16 and 18 channels, a minute each. Plays a tone on each channel: channel n a note n semitones above A3
//    (220 Hz), beeping n times then a pause, so each channel can be told apart by ear and by
//    count; 17-18 a C major chord, left and right.
// 4. Takes our gadget away and binds the stock one again, whatever happened. Every step and
//    its result go into the report. Nothing on the card or in the system is changed for good.
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

#define GADGETS "/sys/kernel/config/usb_gadget"
#define OURS GADGETS "/mpc600s_outs"
#define MAXCH 18
static int nch = 18; // this round's channel count
static int round_seconds = 75;
#define RATE 44100
#define TEST_SECONDS 75 // each round (the first, the stock stereo, a check: 30 s)

static FILE *rep;
static volatile sig_atomic_t stop_now;
static char stock[128], udc[128];

static void say(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    if (rep) vfprintf(rep, fmt, a), fputc('\n', rep), fflush(rep);
    va_end(a);
}

static int put(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return say("  write %s: open failed (%s)", path, strerror(errno)), -1;
    const char *t = text[0] ? text : "\n"; // configfs ignores an empty write
    ssize_t n = write(fd, t, strlen(t));
    int e = errno;
    close(fd);
    if (n < 0) return say("  write %s = '%s': %s", path, text, strerror(e)), -1;
    return 0;
}

static int get(const char *path, char *b, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return snprintf(b, n, "(cannot open: %s)", strerror(errno)), -1;
    ssize_t r = read(fd, b, n - 1);
    int e = errno;
    close(fd);
    if (r < 0) return snprintf(b, n, "(cannot read: %s)", strerror(e)), -1;
    b[r] = 0;
    while (r > 0 && (b[r - 1] == '\n' || b[r - 1] == ' ')) b[--r] = 0;
    return 0;
}

static void dump_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return say("  %s: cannot open (%s)", dir, strerror(errno));
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[512], v[256];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        get(p, v, sizeof v);
        say("  %s = %s", e->d_name, v);
    }
    closedir(d);
}

// The gadget bound to a port now, and the port.
static int find_stock(void) {
    DIR *d = opendir(GADGETS);
    if (!d) return say("no %s (%s)", GADGETS, strerror(errno)), -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strcmp(e->d_name, "mpc600s_outs") == 0 || strcmp(e->d_name, "mpc600s") == 0) continue;
        char p[512], v[128];
        snprintf(p, sizeof p, GADGETS "/%s/UDC", e->d_name);
        if (get(p, v, sizeof v) == 0 && v[0]) {
            snprintf(stock, sizeof stock, "%s", e->d_name), snprintf(udc, sizeof udc, "%s", v);
            break;
        }
    }
    closedir(d);
    return stock[0] ? 0 : -1;
}

static void report_stock(void) {
    say("\n== the stock gadget: %s on %s", stock, udc);
    char p[512];
    const char *parts[] = {"", "strings/0x409", "functions/uac2.audio", "functions/midi.midi"};
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        snprintf(p, sizeof p, GADGETS "/%s/%s", stock, parts[i]);
        say("-- %s", parts[i][0] ? parts[i] : "(top)");
        dump_dir(p);
    }
    snprintf(p, sizeof p, GADGETS "/%s/functions", stock);
    DIR *d = opendir(p);
    struct dirent *e;
    say("-- functions:");
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') say("  %s", e->d_name);
    if (d) closedir(d);
}

static void remove_ours(void) {
    put(OURS "/UDC", "");
    unlink(OURS "/configs/c.1/uac2.0");
    unlink(OURS "/configs/c.1/midi.0");
    rmdir(OURS "/configs/c.1/strings/0x409");
    rmdir(OURS "/configs/c.1");
    rmdir(OURS "/functions/uac2.0");
    rmdir(OURS "/functions/midi.0");
    rmdir(OURS "/strings/0x409");
    rmdir(OURS);
}

static int make_ours(void) {
    remove_ours();
    if (mkdir(OURS, 0755) != 0) return say("mkdir gadget: %s", strerror(errno)), -1;
    put(OURS "/idVendor", "0x1d6b"), put(OURS "/idProduct", "0x0104"), put(OURS "/bcdDevice", "0x0100");
    put(OURS "/bcdUSB", "0x0200"), put(OURS "/max_speed", "high-speed");
    mkdir(OURS "/strings/0x409", 0755);
    put(OURS "/strings/0x409/manufacturer", "MPC 600 S");
    put(OURS "/strings/0x409/product", "MPC 600 S");
    put(OURS "/strings/0x409/serialnumber", "MPC600S0002");
    if (mkdir(OURS "/functions/uac2.0", 0755) != 0) return say("mkdir uac2: %s", strerror(errno)), -1;
    char v[32];
    snprintf(v, sizeof v, "0x%x", (1u << nch) - 1);
    int bad = 0;
    bad |= put(OURS "/functions/uac2.0/p_chmask", v);
    bad |= put(OURS "/functions/uac2.0/p_srate", "44100");
    bad |= put(OURS "/functions/uac2.0/p_ssize", "4"); // 32-bit, as the stock gadget
    bad |= put(OURS "/functions/uac2.0/c_chmask", "0x3");
    bad |= put(OURS "/functions/uac2.0/c_srate", "44100");
    bad |= put(OURS "/functions/uac2.0/c_ssize", "4");
    put(OURS "/functions/uac2.0/req_number", "8"); // as the stock gadget: requests queued, fewer dropouts
    put(OURS "/functions/uac2.0/p_hs_bint", "1"), put(OURS "/functions/uac2.0/c_hs_bint", "1");
    put(OURS "/functions/uac2.0/function_name", "MPC 600 S"); // newer kernels only
    put(OURS "/functions/uac2.0/shared_clock", "1"); // one clock both ways, as the stock gadget
    say("uac2 settings %s", bad ? "NOT all taken (above)" : "taken");
    say("-- uac2.0 as set:");
    dump_dir(OURS "/functions/uac2.0");
    int midi = mkdir(OURS "/functions/midi.0", 0755) == 0;
    if (midi) put(OURS "/functions/midi.0/in_ports", "1"), put(OURS "/functions/midi.0/out_ports", "1");
    say("midi function: %s", midi ? "made" : strerror(errno));
    mkdir(OURS "/configs/c.1", 0755);
    put(OURS "/configs/c.1/MaxPower", "100");
    mkdir(OURS "/configs/c.1/strings/0x409", 0755);
    put(OURS "/configs/c.1/strings/0x409/configuration", "MPC 600 S outs");
    if (symlink(OURS "/functions/uac2.0", OURS "/configs/c.1/uac2.0") != 0) say("link uac2: %s", strerror(errno));
    if (midi && symlink(OURS "/functions/midi.0", OURS "/configs/c.1/midi.0") != 0) say("link midi: %s", strerror(errno));
    if (put(OURS "/UDC", udc) != 0) return say("bind ours to %s: FAILED", udc), -1;
    say("ours bound to %s", udc);
    return 0;
}

// The UAC2 gadget cards (ids UAC2Gadget, UAC2Gadget_1, ...), up to n of them. How many.
static int gadget_cards(int *out, int n) {
    int k = 0;
    for (int c = 0; c < 16 && k < n; c++) {
        char path[64], id[64];
        snprintf(path, sizeof path, "/proc/asound/card%d/id", c);
        if (get(path, id, sizeof id) == 0 && strncmp(id, "UAC2Gadget", 10) == 0) out[k++] = c;
    }
    return k;
}

static int gadget_card(void) {
    char link[64];
    ssize_t n = readlink("/proc/asound/UAC2Gadget", link, sizeof link - 1);
    if (n <= 0) return -1;
    link[n] = 0;
    char *c = strstr(link, "card");
    return c ? atoi(c + 4) : -1;
}

static void hw_any(struct snd_pcm_hw_params *hw) {
    memset(hw, 0, sizeof *hw);
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_MASK; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; i++)
        memset(&hw->masks[i - SNDRV_PCM_HW_PARAM_FIRST_MASK], 0xff, sizeof(struct snd_mask));
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; i++) {
        struct snd_interval *v = &hw->intervals[i - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        v->min = 0, v->max = ~0u;
    }
    hw->rmask = ~0u;
}
static void mask_only(struct snd_pcm_hw_params *hw, int p, unsigned bit) {
    struct snd_mask *m = &hw->masks[p - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof *m);
    m->bits[bit >> 5] = 1u << (bit & 31);
}
static void interval_only(struct snd_pcm_hw_params *hw, int p, unsigned v) {
    struct snd_interval *i = &hw->intervals[p - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = v, i->integer = 1;
}

static int period_frames = 256;

static unsigned imin(const struct snd_pcm_hw_params *hw, int p) { return hw->intervals[p - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min; }
static unsigned imax(const struct snd_pcm_hw_params *hw, int p) { return hw->intervals[p - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max; }

// The gadget (u_audio) takes a period of at most a page (4096 bytes), 4 to 16 of them, and only the
// sample size it was made with. Asked first, so the report shows what it allows; then the largest
// period that fits, up to 256 frames.
static int open_out(int card, int *bytes) {
    char path[40];
    snprintf(path, sizeof path, "/dev/snd/pcmC%dD0p", card);
    int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return say("open %s: %s", path, strerror(errno)), -1;
    static const struct { unsigned f; int b; const char *n; } F[] = {
        {SNDRV_PCM_FORMAT_S32_LE, 4, "S32_LE"}, {SNDRV_PCM_FORMAT_S24_3LE, 3, "S24_3LE"}, {SNDRV_PCM_FORMAT_S16_LE, 2, "S16_LE"}};
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
        struct snd_pcm_hw_params q;
        hw_any(&q);
        mask_only(&q, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
        mask_only(&q, SNDRV_PCM_HW_PARAM_FORMAT, F[i].f);
        mask_only(&q, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
        interval_only(&q, SNDRV_PCM_HW_PARAM_CHANNELS, nch);
        interval_only(&q, SNDRV_PCM_HW_PARAM_RATE, RATE);
        if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &q) != 0) {
            say("  %d channels %s at %d Hz: not offered (%s)", nch, F[i].n, RATE, strerror(errno));
            continue;
        }
        unsigned pmin = imin(&q, SNDRV_PCM_HW_PARAM_PERIOD_SIZE), pmax = imax(&q, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
        unsigned nmin = imin(&q, SNDRV_PCM_HW_PARAM_PERIODS), nmax = imax(&q, SNDRV_PCM_HW_PARAM_PERIODS);
        say("  %d channels %s: period %u-%u frames, %u-%u periods, buffer up to %u frames", nch, F[i].n, pmin, pmax, nmin,
            nmax, imax(&q, SNDRV_PCM_HW_PARAM_BUFFER_SIZE));
        int tries[4][2], nt = 0;
        unsigned top = pmax < 256 ? pmax : 256;
        tries[nt][0] = (int)top, tries[nt++][1] = (int)nmax; // the most slack the gadget allows
        tries[nt][0] = (int)top, tries[nt++][1] = (int)nmin;
        tries[nt][0] = (int)(top / 2 > pmin ? top / 2 : pmin), tries[nt++][1] = (int)(nmax < 16 ? nmax : 16);
        tries[nt][0] = (int)pmin, tries[nt++][1] = (int)nmin;
        for (int l = 0; l < nt; l++) {
        int period = tries[l][0], periods = tries[l][1];
        struct snd_pcm_hw_params hw;
        hw_any(&hw);
        mask_only(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
        mask_only(&hw, SNDRV_PCM_HW_PARAM_FORMAT, F[i].f);
        mask_only(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
        interval_only(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, nch);
        interval_only(&hw, SNDRV_PCM_HW_PARAM_RATE, RATE);
        interval_only(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, (unsigned)period);
        interval_only(&hw, SNDRV_PCM_HW_PARAM_PERIODS, (unsigned)periods);
        if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) != 0) {
            say("  %d channels %s, %d x %d frames (%d bytes): refused (%s)", nch, F[i].n, periods, period,
                nch * F[i].b * period * periods, strerror(errno));
            ioctl(fd, SNDRV_PCM_IOCTL_HW_FREE);
            continue;
        }
        struct snd_pcm_sw_params sw;
        memset(&sw, 0, sizeof sw);
        sw.period_step = 1, sw.avail_min = (snd_pcm_uframes_t)period, sw.xfer_align = 1;
        sw.start_threshold = (snd_pcm_uframes_t)period * 2, sw.stop_threshold = (snd_pcm_uframes_t)(period * periods);
        if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) != 0) say("  sw params: %s", strerror(errno));
        ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
        say("  %d channels %s at %d Hz, %d x %d frames: open", nch, F[i].n, RATE, periods, period);
        *bytes = F[i].b;
        period_frames = period;
        return fd;
        }
    }
    close(fd);
    return -1;
}

// Channel c: a note c semitones above A3, beeping c+1 times then a pause. The last two: a chord.
static float tone(int c, long frame) {
    double t = (double)frame / RATE;
    if (c >= nch - 2) {
        static const double chord[2][2] = {{261.63, 392.0}, {329.63, 523.25}};
        int k = c - (nch - 2);
        return (float)(0.12 * (sin(2 * M_PI * chord[k][0] * t) + sin(2 * M_PI * chord[k][1] * t)));
    }
    double cycle = (c + 1) * 0.3 + 1.0, at = fmod(t, cycle); // beeps of 0.15 s, 0.15 s apart, then 1 s quiet
    if (at >= (c + 1) * 0.3 || fmod(at, 0.3) >= 0.15) return 0;
    return (float)(0.3 * sin(2 * M_PI * 220.0 * pow(2.0, c / 12.0) * t));
}

static int play(int fd, int bytes) {
    unsigned char buf[256 * MAXCH * 4];
    long frame = 0, written = 0, waits = 0, xruns = 0;
    time_t start = time(0), last = start;
    while (!stop_now && time(0) - start < round_seconds) {
        int pf = period_frames;
        for (int i = 0; i < pf; i++)
            for (int c = 0; c < nch; c++) {
                int32_t v = (int32_t)(tone(c, frame + i) * 8388607.0f);
                unsigned char *o = buf + ((size_t)i * nch + (size_t)c) * (size_t)bytes;
                if (bytes == 2) v >>= 8, o[0] = (unsigned char)v, o[1] = (unsigned char)(v >> 8);
                else if (bytes == 3) o[0] = (unsigned char)v, o[1] = (unsigned char)(v >> 8), o[2] = (unsigned char)(v >> 16);
                else v <<= 8, o[0] = (unsigned char)v, o[1] = (unsigned char)(v >> 8), o[2] = (unsigned char)(v >> 16), o[3] = (unsigned char)(v >> 24);
            }
        struct snd_xferi x = {.buf = buf, .frames = (snd_pcm_uframes_t)pf};
        for (;;) {
            if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) {
                written += x.result;
                if (written > 4096 && written - (long)x.result <= 4096) say("  the computer is taking audio (%lds in)", (long)(time(0) - start));
                break;
            }
            if (errno == EAGAIN) { // the computer is not taking audio yet (no app recording): wait
                struct pollfd p = {.fd = fd, .events = POLLOUT};
                poll(&p, 1, 50);
                waits++;
                if (stop_now || time(0) - start >= round_seconds) break;
                continue;
            }
            if (errno == EPIPE) xruns++, ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
            else {
                say("write: %s", strerror(errno));
                return (int)(written / RATE);
            }
        }
        frame += pf;
        if (time(0) - last >= 5) last = time(0), say("  %lds: %ld frames taken by the computer, %ld waits, %ld underruns",
                                                   (long)(last - start), written, waits, xruns);
    }
    say("played: %ld frames taken (%.1f s of audio), %ld waits, %ld underruns", written, (double)written / RATE, waits, xruns);
    return (int)(written / RATE);
}

// A function's settings are locked while it is linked into a configuration (-EBUSY): every link in
// the stock gadget's configurations, noted, taken out, and put back in the same order afterwards.
struct link { char path[512], target[512]; };
static struct link links[16];
static int nlinks;

static void unlink_all(void) {
    char cdir[512];
    snprintf(cdir, sizeof cdir, GADGETS "/%s/configs", stock);
    DIR *d = opendir(cdir);
    struct dirent *c;
    nlinks = 0;
    while (d && (c = readdir(d))) {
        if (c->d_name[0] == '.') continue;
        char one[700];
        snprintf(one, sizeof one, "%s/%s", cdir, c->d_name);
        DIR *e = opendir(one);
        struct dirent *f;
        while (e && (f = readdir(e)) && nlinks < 16) {
            char lp[1000];
            snprintf(lp, sizeof lp, "%s/%s", one, f->d_name);
            struct stat st;
            if (f->d_name[0] == '.' || lstat(lp, &st) != 0 || !S_ISLNK(st.st_mode)) continue;
            struct link *l = &links[nlinks];
            ssize_t n = readlink(lp, l->target, sizeof l->target - 1);
            if (n <= 0) continue;
            l->target[n] = 0;
            snprintf(l->path, sizeof l->path, "%s", lp);
            nlinks++;
        }
        if (e) closedir(e);
    }
    if (d) closedir(d);
    for (int i = 0; i < nlinks; i++) say("  link %s -> %s: %s", links[i].path, links[i].target, unlink(links[i].path) == 0 ? "out" : strerror(errno));
}

static void relink_all(void) {
    for (int i = 0; i < nlinks; i++) {
        char t[600];
        // configfs gives the target relative (../../functions/x): make it whole from the link's folder
        if (links[i].target[0] == '/') snprintf(t, sizeof t, "%s", links[i].target);
        else {
            char dir[512];
            snprintf(dir, sizeof dir, "%s", links[i].path);
            char *slash = strrchr(dir, '/');
            if (slash) *slash = 0;
            snprintf(t, sizeof t, "%s/%s", dir, links[i].target);
        }
        if (strstr(links[i].target, "uac2.outs")) continue; // the extra outputs: not put back
        if (symlink(t, links[i].path) != 0) say("  relink %s: %s", links[i].path, strerror(errno));
    }
}

static void on_signal(int sig) { (void)sig, stop_now = 1; }

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) return 0; // the start script's --codec-volume calls: nothing to do here
    signal(SIGTERM, on_signal), signal(SIGINT, on_signal);
    const char *dir = getenv("MPC_APP_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/usbaudiotest.txt", dir ? dir : ".");
    rep = fopen(path, "w");
    time_t now = time(0);
    say("MPC 600 S USB outs test, %s", ctime(&now));
    if (find_stock() != 0) {
        say("no gadget bound to a port: nothing to test");
        return 1;
    }
    report_stock();
    // Rounds on the stock gadget itself, so only the channel count differs from what works with
    // the stock app: 1, untouched (stereo); 2, its playback channels set to 8; 3, to 18. Each round
    // its bcdDevice changes too, so the computer reads it afresh. Put back as it was at the end
    // (and the stock system makes it anew at every power-on anyway).
    char p[512], chm[512], bcd[512], old_chm[32], old_bcd[32];
    snprintf(p, sizeof p, GADGETS "/%s/UDC", stock);
    snprintf(chm, sizeof chm, GADGETS "/%s/functions/uac2.audio/p_chmask", stock);
    snprintf(bcd, sizeof bcd, GADGETS "/%s/bcdDevice", stock);
    get(chm, old_chm, sizeof old_chm), get(bcd, old_bcd, sizeof old_bcd);
    // Round 1: the stock audio function with 12 channels; 2: with 16; 3: the stock stereo function
    // as it is, and a second UAC2 function beside it with 8 channels (outputs only), so Windows
    // sees two audio devices. Each round its bcdDevice changes, so the computer reads it afresh.
    static const int ROUNDS[] = {12, 16, 8};
    char extra[512], cfg[512];
    snprintf(extra, sizeof extra, GADGETS "/%s/functions/uac2.outs", stock);
    snprintf(cfg, sizeof cfg, GADGETS "/%s/configs/c.1/uac2.outs", stock);
    int took[3] = {0};
    for (int r = 0; r < 3 && !stop_now; r++) {
        nch = ROUNDS[r];
        if (r < 2) say("\n== round %d: the stock gadget's audio with %d channels to the computer", r + 1, nch);
        else say("\n== round 3: the stock stereo audio, and a second audio device with 8 channels");
        if (put(p, "") != 0) say("unbind: FAILED");
        usleep(1500 * 1000); // the computer sees it go
        unlink_all();
        char v[32];
        snprintf(v, sizeof v, "0x%x", r < 2 ? (1u << nch) - 1 : 3u);
        say("p_chmask %s: %s", v, put(chm, v) == 0 ? "taken" : "REFUSED");
        relink_all();
        if (r == 2) {
            if (mkdir(extra, 0755) != 0) say("mkdir uac2.outs: %s", strerror(errno));
            char e[600];
            snprintf(e, sizeof e, "%s/p_chmask", extra), put(e, "0xff");
            snprintf(e, sizeof e, "%s/p_srate", extra), put(e, "44100");
            snprintf(e, sizeof e, "%s/p_ssize", extra), put(e, "4");
            snprintf(e, sizeof e, "%s/c_chmask", extra);
            if (put(e, "0") != 0) say("c_chmask 0 refused: the second device has 2 inputs too");
            snprintf(e, sizeof e, "%s/c_srate", extra), put(e, "44100");
            snprintf(e, sizeof e, "%s/c_ssize", extra), put(e, "4");
            snprintf(e, sizeof e, "%s/req_number", extra), put(e, "8");
            snprintf(e, sizeof e, "%s/p_hs_bint", extra), put(e, "1");
            snprintf(e, sizeof e, "%s/c_hs_bint", extra), put(e, "1");
            snprintf(e, sizeof e, "%s/function_name", extra), put(e, "MPC 600 S Outs 1-8");
            say("-- uac2.outs as set:");
            dump_dir(extra);
            if (symlink(extra, cfg) != 0) say("link uac2.outs: %s", strerror(errno));
        }
        snprintf(v, sizeof v, "0x01%02x", 0x90 + r);
        put(bcd, v);
        if (put(p, udc) != 0) {
            say("bind: FAILED");
            continue;
        }
        say("bound again to %s", udc);
        int cards[4], n = 0;
        for (int i = 0; i < 100 && n < (r == 2 ? 2 : 1); i++) usleep(100 * 1000), n = gadget_cards(cards, 4);
        say("audio cards: %d", n);
        int bytes = 0, fd = -1;
        for (int i = 0; i < n && fd < 0; i++) { // round 3: the one that takes 8 channels
            say("card %d:", cards[i]);
            fd = open_out(cards[i], &bytes);
        }
        if (fd >= 0) {
            round_seconds = TEST_SECONDS;
            say("playing for %d s", round_seconds);
            took[r] = play(fd, bytes);
            ioctl(fd, SNDRV_PCM_IOCTL_DROP);
            close(fd);
        }
    }
    say("\n== summary: seconds of audio the computer took");
    say("  round 1, 12 channels: %d s", took[0]);
    say("  round 2, 16 channels: %d s", took[1]);
    say("  round 3, a second device with 8 channels: %d s", took[2]);
    say("\n== putting the stock gadget back as it was");
    put(p, "");
    usleep(500 * 1000);
    unlink_all();
    if (rmdir(extra) != 0 && errno != ENOENT) say("rmdir uac2.outs: %s", strerror(errno));
    put(chm, old_chm), put(bcd, old_bcd);
    relink_all();
    say(put(p, udc) == 0 ? "stock gadget bound again to %s (p_chmask %s, bcdDevice %s)" : "stock gadget NOT bound again to %s", udc,
        old_chm, old_bcd);
    say("done");
    if (rep) fclose(rep);
    return 0;
}
