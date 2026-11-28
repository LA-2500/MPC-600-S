#include "rawmidi_plain.h"
#include "midi_out.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// The two MIDI ports: the MIDI IN/OUT jacks, and USB MIDI when a computer is plugged in
// (the device's MIDI gadget). Each is opened for reading and for writing.
static const char *const LINKS[MIDI_PORTS] = {
    "/dev/snd/by-path/platform-fe690000.serial", // the External MIDI jacks
    "/dev/snd/by-path/platform-fcc00000.usb",    // USB MIDI
};

static struct {
    int in, out, on;
    unsigned char status, data[2]; // the reader's running status
    int count;
    unsigned char buf[256];
    int len, at;
} port[MIDI_PORTS] = {{.in = -1, .out = -1, .on = 1}, {.in = -1, .out = -1, .on = 1}};

static int open_port(int which, int flags) {
    char link[64];
    ssize_t n = readlink(LINKS[which], link, sizeof link - 1);
    if (n <= 0) return -1;
    link[n] = 0;
    char *c = strstr(link, "controlC");
    if (!c) return -1;
    int card = 0;
    for (c += 8; *c >= '0' && *c <= '9'; c++) card = card * 10 + (*c - '0');
    char path[32];
    snprintf(path, sizeof path, "/dev/snd/midiC%dD0", card);
    return open(path, flags | O_NONBLOCK | O_CLOEXEC);
}

int midi_port_open(int which) {
    if (which < 0 || which >= MIDI_PORTS) return -1;
    port[which].out = open_port(which, O_WRONLY);
    port[which].in = open_port(which, O_RDONLY);
    rawmidi_plain(port[which].in); // see rawmidi_plain.h
    return port[which].out >= 0 || port[which].in >= 0 ? 0 : -1;
}

void midi_port_close(int which) {
    if (which < 0 || which >= MIDI_PORTS) return;
    if (port[which].out >= 0) close(port[which].out);
    if (port[which].in >= 0) close(port[which].in);
    port[which].out = port[which].in = -1;
    port[which].len = port[which].at = port[which].count = 0, port[which].status = 0;
}

int midi_in_fds(int *fds, int max) {
    int n = 0;
    for (int p = 0; p < MIDI_PORTS && n < max; p++)
        if (port[p].in >= 0) fds[n++] = port[p].in;
    return n;
}

void midi_ports_enable(int ext, int usb) {
    port[MIDI_PORT_EXT].on = ext;
    port[MIDI_PORT_USB].on = usb;
}

// Never blocks: a full port drops the message rather than stall the audio thread.
void midi_out_send(const unsigned char *m, int n) {
    for (int p = 0; p < MIDI_PORTS; p++)
        if (port[p].out >= 0 && port[p].on) (void)!write(port[p].out, m, (size_t)n);
}

void midi_out_send_usb(const unsigned char *m, int n) {
    if (port[MIDI_PORT_USB].out >= 0 && port[MIDI_PORT_USB].on) (void)!write(port[MIDI_PORT_USB].out, m, (size_t)n);
}

// The next message from one port, or 0.
static int read_port(int p, unsigned char m[3]) {
    for (;;) {
        if (port[p].at >= port[p].len) {
            ssize_t n = port[p].in >= 0 ? read(port[p].in, port[p].buf, sizeof port[p].buf) : -1;
            if (n <= 0) return 0;
            port[p].len = (int)n;
            port[p].at = 0;
        }
        unsigned char b = port[p].buf[port[p].at++];
        if (b == 0xf8 || b == 0xfa || b == 0xfb || b == 0xfc) { // clock, start, continue, stop: for Sync
            m[0] = b, m[1] = m[2] = 0;
            return 1;
        }
        if (b >= 0xf8) continue; // other real-time bytes
        if (b & 0x80) {
            port[p].status = b >= 0xf0 ? 0 : b; // system messages end running status
            port[p].count = 0;
            continue;
        }
        if (!port[p].status) continue;
        port[p].data[port[p].count++] = b;
        int need = (port[p].status & 0xe0) == 0xc0 ? 1 : 2;
        if (port[p].count < need) continue;
        port[p].count = 0; // running status: the next data bytes reuse this status
        m[0] = port[p].status;
        m[1] = port[p].data[0];
        m[2] = need == 2 ? port[p].data[1] : 0;
        return 1;
    }
}

int midi_in_read(unsigned char m[3], int *from) {
    for (int p = 0; p < MIDI_PORTS; p++) {
        // A switched-off port is still read, so what it held does not arrive later.
        while (read_port(p, m)) {
            if (!port[p].on) continue;
            if (from) *from = p;
            return 1;
        }
    }
    return 0;
}
