// Stem separation test for the MPC Sample. Reads stemtest.wav (the first 15 s) from the app's
// folder, runs each Open-Unmix stem on its own (drums, bass, vocals, other) and Music (the mix
// less the vocals), and writes each as stem-NAME.wav with the time each took and the memory
// used to stemtest.txt. Run on the host with a folder as its argument to check against PyTorch.
#include "surface.h"
#include "umx.h"
#include "wav.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SECONDS 15

// Progress on the pads, as the screen is not used: pads 1-5 are the five stems, blue waiting,
// yellow working, green done, red failed. All 16 green: finished, safe to power off.
static struct surface pads;
static int have_pads;

static void pad(int i, int r, int g, int b) {
    if (have_pads) surface_pad(&pads, i, r, g, b);
}

static void all_pads(int r, int g, int b) {
    for (int i = 0; i < 16; i++) pad(i, r, g, b);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static long peak_kb(void) { // VmHWM: the most memory the program has held
    FILE *f = fopen("/proc/self/status", "r");
    char line[128];
    long kb = -1;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "VmHWM: %ld kB", &kb) == 1) break;
    if (f) fclose(f);
    return kb;
}

static int write_wav(const char *path, const float *x, long n) { // 16-bit stereo, 44.1 kHz
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned data = (unsigned)(n * 4), riff = 36 + data, sr = 44100, br = sr * 4, fmt = 16;
    unsigned short pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f), fwrite(&ch, 2, 1, f), fwrite(&sr, 4, 1, f), fwrite(&br, 4, 1, f);
    fwrite(&align, 2, 1, f), fwrite(&bits, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f);
    for (long i = 0; i < n * 2; i++) {
        float v = x[i] * 32767.0f;
        short s = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v));
        fwrite(&s, 2, 1, f);
    }
    return fclose(f);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : getenv("MPC_APP_DIR");
    if (!dir) dir = ".";
    char path[1024];
    snprintf(path, sizeof path, "%s/stemtest.txt", dir);
    FILE *log = fopen(path, "w");
    if (!log) log = stdout;
    have_pads = surface_open(&pads) == 0;
    all_pads(0, 0, 0);
    for (int i = 0; i < 5; i++) pad(i, 0, 0, 60);
    int threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (threads < 1) threads = 1;
    fprintf(log, "Stem separation test: Open-Unmix UMX-HQ, one stem at a time, %d threads\n", threads);

    snprintf(path, sizeof path, "%s/../MPC600S/stemtest.wav", dir); // yours, beside the settings, first
    struct sample *s = wav_load(path);
    if (!s) { // else the test clip that comes with it
        snprintf(path, sizeof path, "%s/stemtest.wav", dir);
        s = wav_load(path);
    }
    if (s) fprintf(log, "File: %s\n", path);
    if (!s) {
        fprintf(log, "No stemtest.wav found (put one in the app folder or MPC600S)\n");
        fclose(log);
        all_pads(127, 0, 0);
        return 1;
    }
    if (s->rate != 44100) fprintf(log, "Note: the file is %d Hz; the model expects 44100\n", s->rate);
    long n = s->frames < SECONDS * 44100L ? s->frames : SECONDS * 44100L;
    float *in = malloc(sizeof(float) * 2 * (size_t)n), *out = malloc(sizeof(float) * 2 * (size_t)n);
    for (long i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) in[i * 2 + c] = s->data[i * s->channels + (s->channels == 2 ? c : 0)];
    fprintf(log, "Clip: %.2f s, %s\n\n", n / 44100.0, s->channels == 2 ? "stereo" : "mono (doubled)");
    fflush(log);

    static const char *const names[] = {"drums", "bass", "vocals", "other"};
    double total = 0;
    for (int k = 0; k < 5; k++) {
        const char *model = k < 4 ? names[k] : "vocals"; // Music: the vocals model, removed
        const char *name = k < 4 ? names[k] : "music";
        snprintf(path, sizeof path, "%s/umxhq-%s.bin", dir, model);
        pad(k, 127, 90, 0);
        double t0 = now_s();
        struct umx_model *m = umx_load(path);
        double load = now_s() - t0;
        if (!m) {
            fprintf(log, "%-7s model missing: %s\n", name, path);
            pad(k, 127, 0, 0);
            continue;
        }
        struct umx_times tm;
        t0 = now_s();
        int rc = umx_separate(m, in, n, out, k == 4, threads, &tm);
        double took = now_s() - t0;
        umx_free(m);
        if (rc) {
            fprintf(log, "%-7s out of memory\n", name);
            pad(k, 127, 0, 0);
            continue;
        }
        total += took;
        snprintf(path, sizeof path, "%s/stem-%s.wav", dir, name);
        write_wav(path, out, n);
        fprintf(log, "%-7s %6.2f s (load %.2f s; spectrum %.2f, fc1 %.2f, lstm %.2f, fc2/3 %.2f, back %.2f)\n", name,
                took, load, tm.stft, tm.fc1, tm.lstm, tm.fc23, tm.istft);
        fflush(log);
        fsync(fileno(log));
        pad(k, 0, 127, 0);
    }
    fprintf(log, "\nAll five: %.2f s. Peak memory: %ld MB\n", total, peak_kb() / 1024);
    if (log != stdout) fclose(log);
    sync(); // everything on the card before the pads say so
    all_pads(0, 127, 0); // left lit: closing the surface would turn them off
    return 0;
}
