// Records the panel's raw MIDI bytes with times, for studying stray pad, button and wheel
// messages. Sends the stock app's start-up request (F0 47 7F 5C 42 00 00 F7) first, lights the
// pads white while recording, and dark when done. usage: panel-capture OUTFILE SECONDS
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long long now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void pads(int fd, int v) {
    for (int p = 0; p < 16; p++) {
        unsigned char m[12] = {0xF0, 0x47, 0x7F, 0x5C, 0x65, 0x00, 0x04, (unsigned char)p, (unsigned char)v, (unsigned char)v, (unsigned char)v, 0xF7};
        if (write(fd, m, 12) < 0) return;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    char link[128];
    ssize_t n = readlink("/dev/snd/by-path/platform-fe670000.serial", link, sizeof link - 1);
    if (n <= 0) return 1;
    link[n] = 0;
    const char *c = strstr(link, "controlC");
    if (!c) return 1;
    char path[40];
    snprintf(path, sizeof path, "/dev/snd/midiC%dD0", atoi(c + 8));
    int fd = open(path, O_RDWR | O_NONBLOCK);
    FILE *out = fopen(argv[1], "w");
    if (fd < 0 || !out) return 1;
    int secs = atoi(argv[2]);
    static const unsigned char init[8] = {0xF0, 0x47, 0x7F, 0x5C, 0x42, 0x00, 0x00, 0xF7};
    long long t0 = now_us();
    if (write(fd, init, 8) < 0) fprintf(out, "init write failed\n");
    pads(fd, 100);
    fprintf(out, "# time_ms bytes (each read as it arrived)\n");
    while (now_us() - t0 < (long long)secs * 1000000) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 200) <= 0) continue;
        unsigned char b[256];
        while ((n = read(fd, b, sizeof b)) > 0) {
            fprintf(out, "%.3f", (now_us() - t0) / 1000.0);
            for (ssize_t i = 0; i < n; i++) fprintf(out, " %02x", b[i]);
            fputc('\n', out);
        }
        fflush(out);
    }
    pads(fd, 0);
    fclose(out);
    close(fd);
    return 0;
}
