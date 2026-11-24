// USB DATA MODE on the MPC Sample (see usbmode.h): the SD card as a USB drive.
//
// Connect: the card unmounted (refused while anything has it open), USB audio and MIDI closed,
// the stock USB gadget unbound, and a gadget of our own bound in its place with one thing in
// it: a mass storage drive backed by the card's partition. Exit: the drive closed (even if the
// computer has not ejected it), our gadget removed, the stock gadget bound again, USB audio and
// MIDI opened again, the card checked (fsck.exfat -n) and mounted where it was, read-only if
// the check found errors. Every step runs in a thread of its own and none of them waits
// without a limit, so the screen keeps going and Exit always comes back.
#include "usbmode.h"
#include "alsa_out.h"
#include "midi_out.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MS_NOSYMFOLLOW
#define MS_NOSYMFOLLOW 256
#endif

#define GADGETS "/sys/kernel/config/usb_gadget"
#define OURS GADGETS "/mpc600s"
#define LUN OURS "/functions/mass_storage.0/lun.0"
#define CHECK_LIMIT_S 120 // the card check; past this it is stopped and the card used as it is

static struct {
    pthread_mutex_t lock;
    pthread_t thread;
    int running, joinable;
    int state, op;
    char msg[96];
    struct timespec started;
    // The card, as it was mounted when USB mode began.
    char dev[64], devname[32], mnt[256], opts[256];
    // The stock gadget and the port it was on.
    char stock[64], udc[64];
    const int *input_src; // the OS's Input setting, for when the port comes back
    char last_port[32], history[1024]; // the port's states while connected, for the log
} m = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void set_state(int state, const char *msg) {
    pthread_mutex_lock(&m.lock);
    m.state = state;
    snprintf(m.msg, sizeof m.msg, "%s", msg ? msg : "");
    pthread_mutex_unlock(&m.lock);
}

static int put(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    // configfs ignores a zero-length write, so "" (unbind a port, close a drive) goes as "\n",
    // as `echo "" >` does; the kernel takes the newline off.
    if (!text[0]) text = "\n";
    ssize_t n = write(fd, text, strlen(text));
    int err = errno;
    close(fd);
    errno = err;
    return n < 0 ? -1 : 0;
}

// The first step of make_ours that failed, and why: shown on the screen if USB mode can't start.
static char why[48];
static int step(const char *name, int rc) {
    if (rc != 0 && !why[0]) snprintf(why, sizeof why, "%s: %s", name, strerror(errno));
    return rc != 0;
}

static int get(const char *path, char *out, size_t n) {
    out[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) r = 0;
    out[r] = 0;
    while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ')) out[--r] = 0;
    return 0;
}

// /proc/mounts writes a space as \040.
static void unescape(char *s) {
    char *o = s;
    for (char *i = s; *i; i++) {
        if (i[0] == '\\' && i[1] >= '0' && i[1] <= '7' && i[2] && i[3]) {
            *o++ = (char)((i[1] - '0') * 64 + (i[2] - '0') * 8 + (i[3] - '0'));
            i += 3;
        } else {
            *o++ = *i;
        }
    }
    *o = 0;
}

// The mount that holds the app's folder: the card. 0 when found.
static int find_card(void) {
    const char *app = getenv("MPC_APP_DIR");
    if (!app) return -1;
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return -1;
    char line[1024];
    size_t best = 0;
    while (fgets(line, sizeof line, f)) {
        char dev[256], mnt[256], type[64], opts[512];
        if (sscanf(line, "%255s %255s %63s %511s", dev, mnt, type, opts) != 4) continue;
        unescape(mnt);
        size_t len = strlen(mnt);
        if (strncmp(dev, "/dev/", 5) != 0 || strncmp(app, mnt, len) != 0 || (app[len] != '/' && app[len])) continue;
        if (len <= best) continue;
        best = len;
        snprintf(m.dev, sizeof m.dev, "%s", dev);
        snprintf(m.mnt, sizeof m.mnt, "%s", mnt);
        // The options the system mounted it with, less the ones mount(2) takes as flags.
        m.opts[0] = 0;
        for (char *o = strtok(opts, ","); o; o = strtok(0, ",")) {
            if (!strcmp(o, "rw") || !strcmp(o, "ro") || !strcmp(o, "nosuid") || !strcmp(o, "nodev") || !strcmp(o, "noexec") ||
                !strcmp(o, "relatime") || !strcmp(o, "nosymfollow") || !strcmp(o, "noatime"))
                continue;
            size_t at = strlen(m.opts);
            snprintf(m.opts + at, sizeof m.opts - at, "%s%s", at ? "," : "", o);
        }
    }
    fclose(f);
    if (!best) return -1;
    const char *slash = strrchr(m.dev, '/');
    snprintf(m.devname, sizeof m.devname, "%s", slash ? slash + 1 : m.dev);
    return 0;
}

static int card_present(void) {
    char p[96];
    snprintf(p, sizeof p, "/sys/class/block/%s", m.devname);
    return access(p, F_OK) == 0;
}

// The gadget bound to a port now (the stock one), and that port. 0 when found.
static int find_stock(void) {
    m.stock[0] = m.udc[0] = 0;
    DIR *d = opendir(GADGETS);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "mpc600s")) continue;
        char p[320], udc[64];
        snprintf(p, sizeof p, GADGETS "/%s/UDC", e->d_name);
        if (get(p, udc, sizeof udc) == 0 && udc[0]) {
            snprintf(m.stock, sizeof m.stock, "%s", e->d_name);
            snprintf(m.udc, sizeof m.udc, "%s", udc);
        }
    }
    if (d) closedir(d);
    if (!m.udc[0]) { // nothing bound: the port by its own name
        d = opendir("/sys/class/udc");
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.') snprintf(m.udc, sizeof m.udc, "%s", e->d_name);
        if (d) closedir(d);
    }
    return m.udc[0] ? 0 : -1;
}

// ---- USB individual outs: the stock UAC2 function with 8 channels to the computer ----
//
// Found on the device (MPC Sample, kernel 6.12) with Windows: the stock function with more than 8
// channels to the computer stalls after about two seconds; a second UAC2 function beside it is
// streamed but Windows plays it as silence. The stock function with 8 plays. So with the outs on,
// its playback channel mask is 8 (the main mix on 1-2 or not, as the OS's Layout says); off, back
// to the stock's own. A function's settings are locked while it is linked into a configuration, so
// every link is taken out (order noted), the setting changed, and the links put back in that order.
#define OLD_OUTS_FN "uac2.outs" // a second function, from a build that tried one: taken away
static char outs_bcd[16], outs_chmask[16], outs_serial[64]; // the stock gadget's own, as found the first time

int usbmode_outs(int on, const int *input_src) {
    if (find_stock() != 0 || !m.stock[0]) return -1;
    char g[160], p[400], cfg[300], chm[400];
    snprintf(g, sizeof g, GADGETS "/%s", m.stock);
    snprintf(cfg, sizeof cfg, "%s/configs/c.1", g);
    snprintf(chm, sizeof chm, "%s/functions/uac2.audio/p_chmask", g);
    if (access(chm, F_OK) != 0) return -1;
    char now[16];
    get(chm, now, sizeof now);
    if (!outs_chmask[0]) {
        snprintf(outs_chmask, sizeof outs_chmask, "%s", strcmp(now, "255") && strcmp(now, "0xff") ? now : "3");
        snprintf(p, sizeof p, "%s/bcdDevice", g);
        get(p, outs_bcd, sizeof outs_bcd);
        snprintf(p, sizeof p, "%s/strings/0x409/serialnumber", g);
        get(p, outs_serial, sizeof outs_serial);
        size_t sl = strlen(outs_serial);
        if (sl > 2 && !strcmp(outs_serial + sl - 2, "-8")) outs_serial[sl - 2] = 0; // left on by a crash: the stock's
        if (!outs_bcd[0] || !strcmp(outs_bcd, "0x0190") || !strcmp(outs_bcd, "0x0191") || !strcmp(outs_bcd, "0x0192"))
            snprintf(outs_bcd, sizeof outs_bcd, "0x0101");
    }
    snprintf(p, sizeof p, "%s/functions/" OLD_OUTS_FN, g);
    int old = access(p, F_OK) == 0, is_on = !strcmp(now, "255") || !strcmp(now, "0xff");
    if (is_on == on && !old) return 0; // already so
    midi_port_close(MIDI_PORT_USB);
    alsa_out_usb_release();
    snprintf(p, sizeof p, "%s/UDC", g);
    put(p, "");
    usleep(500 * 1000);
    char name[16][64], target[16][300];
    int n = 0;
    DIR *d = opendir(cfg);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 16) {
        char lp[400];
        snprintf(lp, sizeof lp, "%s/%s", cfg, e->d_name);
        struct stat st;
        if (e->d_name[0] == '.' || lstat(lp, &st) != 0 || !S_ISLNK(st.st_mode)) continue;
        unlink(lp);
        if (!strcmp(e->d_name, OLD_OUTS_FN)) continue;
        snprintf(name[n], sizeof name[n], "%s", e->d_name);
        snprintf(target[n], sizeof target[n], "%s/functions/%s", g, e->d_name);
        n++;
    }
    if (d) closedir(d);
    snprintf(p, sizeof p, "%s/functions/" OLD_OUTS_FN, g);
    rmdir(p);
    int rc = put(chm, on ? "0xff" : outs_chmask);
    // configfs lists a configuration's links newest first: made again oldest first (the list read
    // backwards), so every function keeps its place and its interface numbers. (Made in the order
    // listed, the order turned round each time, and Windows lost the MIDI port.)
    for (int i = n - 1; i >= 0; i--) {
        char lp[400];
        snprintf(lp, sizeof lp, "%s/%s", cfg, name[i]);
        symlink(target[i], lp);
    }
    // With the outs, its own serial number (the stock's with -8): Windows keeps a device's setup by
    // its serial, and fitting the 8-channel device onto the stereo one's lost the MIDI port (as did
    // a new bcdDevice, which made it new hardware). Each now has its own whole setup, MIDI and all.
    snprintf(p, sizeof p, "%s/bcdDevice", g);
    put(p, outs_bcd);
    if (outs_serial[0]) {
        char sn[80];
        snprintf(sn, sizeof sn, "%s%s", outs_serial, on && rc == 0 ? "-8" : "");
        snprintf(p, sizeof p, "%s/strings/0x409/serialnumber", g);
        put(p, sn);
    }
    snprintf(p, sizeof p, "%s/UDC", g);
    if (put(p, m.udc) != 0) rc = -1;
    for (int i = 0; i < 10; i++) {
        usleep(100 * 1000);
        if (midi_port_open(MIDI_PORT_USB) == 0) break;
    }
    alsa_out_usb_restore(input_src ? *input_src : 0);
    return rc;
}

// Our gadget away, whatever state it is in. Each step may fail when it is not there.
static void remove_ours(void) {
    if (access(OURS, F_OK) != 0) return;
    put(LUN "/forced_eject", "1"); // closes the drive even when the computer holds it
    put(OURS "/UDC", "");
    put(LUN "/file", "");
    unlink(OURS "/configs/c.1/mass_storage.0");
    rmdir(OURS "/configs/c.1/strings/0x409");
    rmdir(OURS "/configs/c.1");
    rmdir(OURS "/functions/mass_storage.0");
    rmdir(OURS "/strings/0x409");
    rmdir(OURS);
}

static int make_ours(void) {
    remove_ours(); // left from before, if the MPC was switched off in USB mode
    if (step("gadget", mkdir(OURS, 0755))) return -1;
    int bad = 0;
    bad |= step("ids", put(OURS "/idVendor", "0x1d6b")); // Linux Foundation
    bad |= step("ids", put(OURS "/idProduct", "0x0104")); // multifunction composite gadget
    bad |= step("ids", put(OURS "/bcdDevice", "0x0100"));
    bad |= step("ids", put(OURS "/bcdUSB", "0x0200"));
    put(OURS "/bMaxPacketSize0", "0x40");
    put(OURS "/max_speed", "high-speed"); // the port is USB 2 on the cable; no SuperSpeed try first
    bad |= step("names", mkdir(OURS "/strings/0x409", 0755));
    bad |= step("names", put(OURS "/strings/0x409/manufacturer", "MPC 600 S"));
    bad |= step("names", put(OURS "/strings/0x409/product", "SD Card"));
    bad |= step("names", put(OURS "/strings/0x409/serialnumber", "MPC600S0001"));
    bad |= step("drive", mkdir(OURS "/functions/mass_storage.0", 0755)); // makes lun.0 with it
    bad |= step("drive", put(LUN "/removable", "1")); // so the computer can eject it, and we see that
    bad |= step("drive", put(LUN "/ro", "0"));
    bad |= step("card", put(LUN "/file", m.dev));
    bad |= step("config", mkdir(OURS "/configs/c.1", 0755));
    bad |= step("config", put(OURS "/configs/c.1/MaxPower", "100"));
    bad |= step("config", mkdir(OURS "/configs/c.1/strings/0x409", 0755));
    bad |= step("config", put(OURS "/configs/c.1/strings/0x409/configuration", "SD Card"));
    bad |= step("link", symlink(OURS "/functions/mass_storage.0", OURS "/configs/c.1/mass_storage.0"));
    if (!bad) bad |= step("port", put(OURS "/UDC", m.udc));
    if (bad) remove_ours();
    return bad ? -1 : 0;
}

static void stock_back(void) {
    if (!m.stock[0]) return;
    char p[320];
    snprintf(p, sizeof p, GADGETS "/%s/UDC", m.stock);
    put(p, m.udc);
}

static void usb_back(void) {
    stock_back();
    for (int i = 0; i < 10; i++) { // the audio and MIDI devices come back as the gadget binds
        usleep(100 * 1000);
        if (midi_port_open(MIDI_PORT_USB) == 0) break;
    }
    alsa_out_usb_restore(m.input_src ? *m.input_src : 0);
}

static int mount_card(int read_only) {
    mkdir(m.mnt, 0755); // the folder may have gone with the unmount
    unsigned long flags = MS_NOSUID | MS_NODEV | MS_NOSYMFOLLOW | (read_only ? MS_RDONLY : 0);
    if (mount(m.dev, m.mnt, "exfat", flags, m.opts) == 0) return 0;
    if (mount(m.dev, m.mnt, "exfat", flags & ~(unsigned long)MS_NOSYMFOLLOW, m.opts) == 0) return 0;
    return mount(m.dev, m.mnt, "vfat", flags, m.opts);
}

// fsck.exfat on the card, with a time limit. 0 clean, 1 errors, 2 could not tell (no checker,
// or it ran out of time).
static int check_card(int repair) {
    static const char *const where[] = {"/usr/sbin/fsck.exfat", "/sbin/fsck.exfat", "/usr/bin/fsck.exfat"};
    const char *tool = 0;
    for (size_t i = 0; i < sizeof where / sizeof where[0] && !tool; i++)
        if (access(where[i], X_OK) == 0) tool = where[i];
    if (!tool) return 2;
    pid_t pid = fork();
    if (pid < 0) return 2;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) dup2(null, 0), dup2(null, 1), dup2(null, 2);
        execl(tool, tool, repair ? "-y" : "-n", m.dev, (char *)0);
        _exit(127);
    }
    int status = 0;
    for (int t = 0; t < CHECK_LIMIT_S * 10; t++) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (!WIFEXITED(status)) return 2;
            int code = WEXITSTATUS(status);
            if (code == 0) return 0;
            if (repair && code == 1) return 0; // errors found and all put right
            return code == 127 || code >= 8 ? 2 : 1;
        }
        usleep(100 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return 2;
}

static void do_connect(void) {
    if (find_card() != 0) { set_state(USBM_FAILED, "No SD card found"); return; }
    if (find_stock() != 0) { set_state(USBM_FAILED, "No USB port found"); return; }
    sync();
    if (umount2(m.mnt, 0) != 0)
        { set_state(USBM_FAILED, errno == EBUSY ? "The card is in use" : "Could not release the card"); return; }
    midi_port_close(MIDI_PORT_USB);
    alsa_out_usb_release();
    why[0] = 0;
    pthread_mutex_lock(&m.lock);
    m.history[0] = m.last_port[0] = 0;
    pthread_mutex_unlock(&m.lock);
    if (m.stock[0]) {
        char p[320];
        snprintf(p, sizeof p, GADGETS "/%s/UDC", m.stock);
        step("stock off", put(p, ""));
    }
    if (make_ours() != 0) { // back as it was
        usb_back();
        mount_card(0);
        char msg[96];
        snprintf(msg, sizeof msg, "Could not start USB mode\n%s", why);
        set_state(USBM_FAILED, msg);
        return;
    }
    set_state(USBM_CONNECTED, 0);
}

static void finish_card(int repair) {
    if (!card_present()) { set_state(USBM_NO_CARD, 0); return; }
    int check = check_card(repair);
    if (check == 1) {
        mount_card(1);
        { set_state(USBM_ERRORS, "The card has errors"); return; }
    }
    if (mount_card(0) != 0) { set_state(USBM_ERRORS, "Could not mount the card"); return; }
    set_state(USBM_DONE, check == 2 ? "The card was not checked" : 0);
}

static void do_exit(void) {
    remove_ours();
    sync(); // what the computer wrote, all on the card before it is checked
    usb_back();
    finish_card(0);
}

static void do_repair(void) {
    sync();
    umount2(m.mnt, 0); // mounted read-only after the check
    finish_card(1);
}

static void *worker(void *arg) {
    (void)arg;
    if (m.op == USBM_CONNECT) do_connect();
    else if (m.op == USBM_EXIT) do_exit();
    else do_repair();
    pthread_mutex_lock(&m.lock);
    m.running = 0;
    pthread_mutex_unlock(&m.lock);
    return 0;
}

static int hw_begin(int op) {
    pthread_mutex_lock(&m.lock);
    int busy = m.running;
    pthread_mutex_unlock(&m.lock);
    if (busy) return -1;
    if (m.joinable) pthread_join(m.thread, 0), m.joinable = 0;
    m.op = op;
    m.running = 1;
    m.state = USBM_WORKING;
    m.msg[0] = 0;
    clock_gettime(CLOCK_MONOTONIC, &m.started);
    if (pthread_create(&m.thread, 0, worker, 0) != 0) {
        m.running = 0;
        set_state(op == USBM_CONNECT ? USBM_FAILED : USBM_ERRORS, "Could not start");
        return -1;
    }
    m.joinable = 1;
    return 0;
}

static int hw_elapsed(void);

static int hw_state(int *flags, char *msg, size_t n) {
    pthread_mutex_lock(&m.lock);
    int st = m.state;
    if (msg && n) snprintf(msg, n, "%s", m.msg);
    pthread_mutex_unlock(&m.lock);
    if (flags) {
        *flags = 0;
        if (st == USBM_CONNECTED) {
            char p[160], v[64];
            snprintf(p, sizeof p, "/sys/class/udc/%s/state", m.udc);
            get(p, v, sizeof v);
            if (v[0] && strcmp(v, "not attached") != 0) *flags |= USBM_CABLE;
            if (!strcmp(v, "configured")) *flags |= USBM_HOST;
            pthread_mutex_lock(&m.lock);
            if (strcmp(v, m.last_port) != 0) { // each change of the port's state
                snprintf(m.last_port, sizeof m.last_port, "%s", v);
                size_t at = strlen(m.history);
                snprintf(m.history + at, sizeof m.history - at, "%d ms: %s\n", hw_elapsed(), v[0] ? v : "(none)");
            }
            pthread_mutex_unlock(&m.lock);
            if (msg && n) { // what the port says, shown until a computer has the drive
                char role[32] = "";
                DIR *d = opendir("/sys/class/usb_role");
                struct dirent *e;
                while (d && (e = readdir(d)) && !role[0])
                    if (e->d_name[0] != '.') {
                        char rp[300];
                        snprintf(rp, sizeof rp, "/sys/class/usb_role/%s/role", e->d_name);
                        get(rp, role, sizeof role);
                    }
                if (d) closedir(d);
                snprintf(msg, n, "%s%s%s", v[0] ? v : "no state", role[0] ? ", role " : "", role);
            }
            get(LUN "/file", v, sizeof v);
            if (!v[0]) *flags |= USBM_EJECTED;
            if (!card_present()) *flags |= USBM_CARD_OUT;
        }
    }
    return st;
}

static int hw_elapsed(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int)((now.tv_sec - m.started.tv_sec) * 1000 + (now.tv_nsec - m.started.tv_nsec) / 1000000);
}

// While the card is away, nothing may write to where it was mounted (the log, for one).
int usbmode_card_away(void) {
    pthread_mutex_lock(&m.lock);
    int away = m.state == USBM_WORKING || m.state == USBM_CONNECTED;
    pthread_mutex_unlock(&m.lock);
    return away;
}

const struct usbmode_hooks *usbmode_hw(const int *input_src) {
    static const struct usbmode_hooks hooks = {hw_begin, hw_state, hw_elapsed};
    m.input_src = input_src;
    return &hooks;
}
