// Akai .SND sound files (as the MPC 2000 / 2000XL write them): a 42-byte header, then 16-bit PCM, a stereo sound's
// left channel in full and then its right.
#pragma once
#include "engine.h"
#include <stddef.h>

struct snd_info {
    char name[17];  // as kept in the file, trailing spaces dropped
    int level;      // 0..200, 100 as recorded
    int tune;       // -120..120, tenths of a semitone
    int32_t start, end, loop_len; // frames; the loop runs from end - loop_len to end
    int loop_on;
    int beats;      // the beats in the loop, 1..16
    int mpc3000;    // the MPC 3000's file: version 2, a 38-byte header (no rate: 44.1 kHz)
};

// Parses a whole file held in memory. NULL for anything it cannot read. info may be NULL.
struct sample *snd_parse(const unsigned char *buf, size_t len, struct snd_info *info);
struct sample *snd_load(const char *path, struct snd_info *info);

// Writes the sound as 16-bit with info's name, trim, loop and tune (info NULL: the whole
// sound, no loop), as an MPC 2000XL writes it (a rate other than 44.1 kHz kept in the header),
// or with info->mpc3000 as an MPC 3000 does (38-byte header, no loop or rate). 0, or -1.
int snd_save(const char *path, const struct sample *s, const struct snd_info *info);
