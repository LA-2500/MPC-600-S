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
// TRANSIENT (Zone Assign): every drum hit in a break, for a zone each. Three bands (kick, snare
// body, hats and cymbals) looked at every 2.9 ms; a threshold that follows the loudness around
// each point, so ghost notes count beside loud snares; hits as close as 40 ms. sens 1..10: how
// soft a hit still counts (5 to start). The first start is the first hit (or 0); each start sits
// a millisecond before its hit, on a zero crossing. strength[] (may be NULL) gets each hit's
// strength; with more than max, the strongest are kept.
int onset_detect_transient(const struct sample *s, int32_t *out, float *strength, int max, int sens);
