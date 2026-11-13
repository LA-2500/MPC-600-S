// Stem separation with Open-Unmix (UMX-HQ, MIT), one stem at a time: the model for that stem
// estimates its spectrum, which is put back together with the mix's own phase, as
// Open-Unmix does with its EM post-filter off. Plain C, no libraries, all cores.
#pragma once

struct umx_model;

// The weights exported by export_umx.py: NULL when the file is missing or not one.
struct umx_model *umx_load(const char *path);
void umx_free(struct umx_model *m);

struct umx_times { // seconds spent in each stage
    double stft, fc1, lstm, fc23, istft;
};

// in: n interleaved stereo frames at 44.1 kHz. out: the same length. remove 0 keeps only the
// stem, 1 everything but the stem. 0, or -1 when memory runs out.
int umx_separate(const struct umx_model *m, const float *in, long n, float *out, int remove, int threads,
                 struct umx_times *times);
