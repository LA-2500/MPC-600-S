// WAV reader. PCM 8, 16, 24, 32 bit and float 32. Mono or stereo; extra channels are dropped.
#pragma once
#include "engine.h"
#include <stddef.h>

// Parses a whole file held in memory. Returns NULL for anything it cannot read.
struct sample *wav_parse(const unsigned char *buf, size_t len);

struct sample *wav_load(const char *path);

// Loads only the start of a file, at most max_bytes of it, for previewing.
struct sample *wav_load_start(const char *path, size_t max_bytes);

// Writes a WAV: bits 32 float, 24 or 16 PCM. 0, or -1.
int wav_save(const char *path, const struct sample *s, int bits);
