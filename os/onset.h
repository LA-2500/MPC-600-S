// Transient detection for picking sample start points.
#pragma once
#include "engine.h"

#define ONSETS_MAX 256

// Fills out[] with start points in frames, rising, and returns how many. The first is
// always 0. Each later one sits a few milliseconds before a detected hit (a bit of air),
// on a zero crossing so it starts without a click. It finds the first sound after
// leading silence, and the kicks and snares in a loop: the low band catches the kicks,
// the bright band the snares and hats.
int onset_detect(const struct sample *s, int32_t *out, int max);
// The same, needing a rise of `rise` (log10 energy, 0.5 is the usual 5 dB) to count as a hit.
int onset_detect_rise(const struct sample *s, int32_t *out, int max, float rise);
