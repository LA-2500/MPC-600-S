#include "fx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SR 44100.0f
#define PI_F 3.14159265f
#define DLY_MAX 88200 // two seconds a channel: the longest delay

// ---- The table: names, controls, defaults ----

enum { V_NUM, V_TIME, V_SLAP, V_JUNO, V_VIBE, V_BIPOLAR, V_DB, V_GATE, V_CAB, V_HZ, V_HZ_CUT, V_Q, V_RAW, V_MFC };

static const struct {
    const char *name, *shortname;
    const char *param[3];
    int def[3], fmt[3];
} INFO[FX_COUNT] = {
    [FX_OFF] = {"---", "", {"", "", ""}, {0, 0, 0}, {V_NUM, V_NUM, V_NUM}},
    [FX_ASR_HALL] = {"ASR HALL", "Hall", {"Decay", "Tone", "Mix"}, {80, 70, 30}, {V_NUM, V_NUM, V_NUM}},
    [FX_ROOM] = {"ROOM", "Room", {"Decay", "Tone", "Mix"}, {60, 80, 35}, {V_NUM, V_NUM, V_NUM}},
    [FX_PLATE] = {"PLATE", "Plate", {"Decay", "Tone", "Mix"}, {70, 90, 35}, {V_NUM, V_NUM, V_NUM}},
    [FX_SPRING] = {"SPRING", "Spring", {"Decay", "Tone", "Mix"}, {70, 70, 40}, {V_NUM, V_NUM, V_NUM}},
    [FX_SNARE] = {"SNARE", "SnRvb", {"Decay", "Tone", "Mix"}, {60, 100, 45}, {V_NUM, V_NUM, V_NUM}},
    [FX_GATED] = {"GATED", "Gated", {"Decay", "Tone", "Mix"}, {60, 80, 50}, {V_NUM, V_NUM, V_NUM}},
    [FX_DUB] = {"DUB DELAY", "Dub", {"Time", "Feedback", "Mix"}, {103, 85, 50}, {V_TIME, V_NUM, V_NUM}},
    [FX_DDL] = {"DDL", "DDL", {"Time", "Feedback", "Mix"}, {93, 50, 40}, {V_TIME, V_NUM, V_NUM}},
    [FX_TAPE] = {"TAPE ECHO", "Tape", {"Time", "Feedback", "Wow"}, {103, 60, 40}, {V_TIME, V_NUM, V_NUM}},
    [FX_ANALOG] = {"ANALOG DLY", "Analog", {"Time", "Feedback", "Mix"}, {93, 60, 45}, {V_TIME, V_NUM, V_NUM}},
    [FX_PINGPONG] = {"PING-PONG", "PngPng", {"Time", "Feedback", "Mix"}, {93, 55, 45}, {V_TIME, V_NUM, V_NUM}},
    [FX_SLAP] = {"SLAPBACK", "Slap", {"Time", "Tone", "Mix"}, {50, 80, 45}, {V_SLAP, V_NUM, V_NUM}},
    [FX_JUNO] = {"JUNO CHORUS", "Juno", {"Mode", "Tone", "Mix"}, {0, 90, 64}, {V_JUNO, V_NUM, V_NUM}},
    [FX_UNIVIBE] = {"UNIVIBE", "Vibe", {"Speed", "Intensity", "Mode"}, {40, 90, 0}, {V_NUM, V_NUM, V_VIBE}},
    [FX_DP4] = {"DP4 PHASER", "DP4Ph", {"Rate", "Depth", "Feedback"}, {30, 90, 70}, {V_NUM, V_NUM, V_NUM}},
    [FX_MXR_PH] = {"MXR PHASER", "Phase", {"Speed", "Depth", "Mix"}, {40, 100, 64}, {V_NUM, V_NUM, V_NUM}},
    [FX_SMALLSTONE] = {"SMALL STONE", "Stone", {"Rate", "Color", "Mix"}, {30, 90, 64}, {V_NUM, V_NUM, V_NUM}},
    [FX_FLANGER] = {"FLANGER", "Flange", {"Rate", "Depth", "Feedback"}, {20, 90, 95}, {V_NUM, V_NUM, V_BIPOLAR}},
    [FX_RHODES] = {"RHODES TREM", "Trem", {"Rate", "Depth", "Stereo"}, {60, 70, 127}, {V_NUM, V_NUM, V_NUM}},
    [FX_AUTOWAH] = {"AUTO-WAH", "AWah", {"Sensitivity", "Range", "Resonance"}, {80, 90, 70}, {V_NUM, V_NUM, V_NUM}},
    [FX_KLON] = {"KLON", "Klon", {"Gain", "Treble", "Output"}, {40, 64, 70}, {V_NUM, V_NUM, V_NUM}},
    [FX_OD1] = {"OD-1", "OD1", {"Drive", "Level", "Mix"}, {50, 70, 127}, {V_NUM, V_NUM, V_NUM}},
    [FX_DS1] = {"DS-1", "DS1", {"Dist", "Tone", "Level"}, {70, 64, 60}, {V_NUM, V_NUM, V_NUM}},
    [FX_DYNACOMP] = {"DYNACOMP", "Dyna", {"Sensitivity", "Output", "Attack"}, {70, 64, 20}, {V_NUM, V_NUM, V_NUM}},
    [FX_CS3] = {"CS-3 COMP", "CS3", {"Sustain", "Attack", "Tone"}, {64, 40, 64}, {V_NUM, V_NUM, V_BIPOLAR}},
    [FX_GATE] = {"NOISE GATE", "Gate", {"Threshold", "Release", "Range"}, {40, 40, 127}, {V_GATE, V_NUM, V_NUM}},
    [FX_BASSMAN] = {"BASSMAN", "Bassmn", {"Gain", "Tone", "Presence"}, {40, 64, 50}, {V_NUM, V_NUM, V_NUM}},
    [FX_AC30] = {"AC30", "AC30", {"Gain", "Cut", "Volume"}, {45, 40, 80}, {V_NUM, V_NUM, V_NUM}},
    [FX_FENDER_EQ] = {"FENDER EQ", "FndrEQ", {"Bass", "Mid", "Treble"}, {64, 64, 64}, {V_NUM, V_NUM, V_NUM}},
    [FX_EQ3] = {"3-BAND EQ", "EQ", {"Low", "Mid", "High"}, {64, 64, 64}, {V_DB, V_DB, V_DB}},
    [FX_WAH] = {"WAH PEDAL", "Wah", {"Pedal", "Resonance", "Level"}, {64, 64, 64}, {V_NUM, V_NUM, V_DB}},
    [FX_SPEAKER] = {"SPEAKER SIM", "Spkr", {"Cab", "Mic", "Level"}, {0, 64, 64}, {V_CAB, V_NUM, V_DB}},
    // EQ for drums: three bands each, set where that drum needs them, starting from a typical
    // curve (64 is flat on every gain).
    [FX_PARAM_EQ] = {"PARAMETRIC EQ", "PEQ", {"Freq", "Gain", "Q"}, {64, 64, 40}, {V_HZ, V_DB, V_Q}},
    [FX_EQ_KICK] = {"KICK EQ", "KickEQ", {"Sub", "Box", "Click"}, {85, 38, 80}, {V_DB, V_DB, V_DB}},
    [FX_EQ_SNARE] = {"SNARE EQ", "SnrEQ", {"Body", "Ring", "Snap"}, {80, 48, 85}, {V_DB, V_DB, V_DB}},
    [FX_EQ_HAT] = {"HI-HAT EQ", "HatEQ", {"Low Cut", "Harsh", "Air"}, {75, 53, 80}, {V_HZ_CUT, V_DB, V_DB}},
    [FX_EQ_CLAP] = {"CLAP EQ", "ClapEQ", {"Mud", "Body", "Crisp"}, {48, 75, 80}, {V_DB, V_DB, V_DB}},
    [FX_EQ_TOM] = {"TOM EQ", "TomEQ", {"Body", "Box", "Attack"}, {80, 43, 80}, {V_DB, V_DB, V_DB}},
    [FX_EQ_808] = {"808 EQ", "808EQ", {"Sub", "Mud", "Knock"}, {80, 48, 80}, {V_DB, V_DB, V_DB}},
    [FX_VINYL] = {"VINYL SIM", "Vinyl", {"Comp", "Noise", "Wow Flut"}, {40, 0, 10}, {V_RAW, V_RAW, V_RAW}},
    [FX_JTM45] = {"JTM45", "JTM45", {"Gain", "Tone", "Presence"}, {50, 60, 50}, {V_NUM, V_NUM, V_NUM}},
    [FX_FUZZ] = {"FUZZ FACE", "Fuzz", {"Fuzz", "Volume", "Bias"}, {80, 64, 64}, {V_NUM, V_NUM, V_BIPOLAR}},
    [FX_JC] = {"JC-120", "JC120", {"Bright", "Chorus", "Volume"}, {40, 64, 64}, {V_NUM, V_NUM, V_NUM}},
    [FX_DC30] = {"DC30", "DC30", {"Gain", "Tone", "Cut"}, {45, 64, 40}, {V_NUM, V_NUM, V_NUM}},
    [FX_MFC42] = {"MFC42 FILTER", "MFC42", {"Cutoff", "Reson", "Mod"}, {70, 50, 96}, {V_NUM, V_NUM, V_MFC}},
    [FX_B15] = {"B-15", "B15", {"Bass", "Treble", "Volume"}, {64, 64, 50}, {V_DB, V_DB, V_NUM}},
};

// The order the lists scroll in, a category at a time.
static const int ORDER[FX_COUNT - 1] = {
    FX_ASR_HALL, FX_ROOM, FX_PLATE, FX_SPRING, FX_SNARE, FX_GATED,
    FX_EQ3, FX_PARAM_EQ, FX_EQ_KICK, FX_EQ_SNARE, FX_EQ_HAT, FX_EQ_CLAP, FX_EQ_TOM, FX_EQ_808,
    FX_DUB, FX_DDL, FX_TAPE, FX_ANALOG, FX_PINGPONG, FX_SLAP,
    FX_JUNO, FX_UNIVIBE, FX_DP4, FX_MXR_PH, FX_SMALLSTONE, FX_FLANGER, FX_RHODES, FX_WAH, FX_AUTOWAH, FX_MFC42,
    FX_KLON, FX_OD1, FX_DS1, FX_FUZZ, FX_DYNACOMP, FX_CS3, FX_VINYL, FX_GATE,
    FX_BASSMAN, FX_AC30, FX_JTM45, FX_DC30, FX_JC, FX_B15, FX_SPEAKER, FX_FENDER_EQ,
};
static const int CAT_START[FX_CATS + 1] = {0, 6, 14, 20, 30, 38, FX_COUNT - 1};

static const char *const CABS[4] = {"1x12 OPEN", "2x12 BLUE", "4x10 TWD", "4x12 GB"};
static int cab_of(int v) {
    return (v < 0 ? 0 : v > 127 ? 127 : v) * 4 / 128;
}

static const char *const CAT_NAMES[FX_CATS] = {"REVERB", "EQ", "DELAY", "MODULATION", "DRIVE/DYN", "AMP"};

const char *fx_cat_name(int c) {
    return c >= 0 && c < FX_CATS ? CAT_NAMES[c] : "";
}

static int position(int type) { // in ORDER, or -1 for FX_OFF
    for (int i = 0; i < FX_COUNT - 1; i++)
        if (ORDER[i] == type) return i;
    return -1;
}

int fx_cat_of(int type) {
    int at = position(type);
    for (int c = 0; c < FX_CATS; c++)
        if (at >= CAT_START[c] && at < CAT_START[c + 1]) return c;
    return 0;
}

int fx_cat_first(int c) {
    return c >= 0 && c < FX_CATS ? ORDER[CAT_START[c]] : FX_OFF;
}

int fx_step(int type, int d, int cat) {
    int lo = cat >= 0 ? CAT_START[cat] : -1, hi = cat >= 0 ? CAT_START[cat + 1] - 1 : FX_COUNT - 2;
    int at = position(type) + d;
    at = at < lo ? lo : at > hi ? hi : at;
    return at < 0 ? FX_OFF : ORDER[at];
}

const char *fx_name(int type) {
    return type >= 0 && type < FX_COUNT ? INFO[type].name : "---";
}

const char *fx_short(int type) {
    return type >= 0 && type < FX_COUNT ? INFO[type].shortname : "";
}

const char *fx_param_name(int type, int i) {
    return type > 0 && type < FX_COUNT && i >= 0 && i < 3 ? INFO[type].param[i] : "";
}

int fx_param_default(int type, int i) {
    return type > 0 && type < FX_COUNT && i >= 0 && i < 3 ? INFO[type].def[i] : 0;
}

static float k01(int v) { // a control, 0..1
    return (v < 0 ? 0 : v > 127 ? 127 : v) / 127.0f;
}

// Delay times: the lower half of the knob is 20 ms to 1 s, the upper half note values.
static const struct {
    const char *name;
    double beats;
} NOTES[12] = {{"1/32", 0.125}, {"1/16T", 1 / 6.0}, {"1/16", 0.25}, {"1/8T", 1 / 3.0}, {"1/16D", 0.375}, {"1/8", 0.5},
               {"1/4T", 2 / 3.0}, {"1/8D", 0.75},   {"1/4", 1},     {"1/2T", 4 / 3.0}, {"1/4D", 1.5},   {"1/2", 2}};

static float delay_samples(int v, double bpm) {
    if (v < 64) return 20.0f * powf(50.0f, v / 63.0f) * SR / 1000.0f;
    int i = (v - 64) * 12 / 64;
    float s = (float)(NOTES[i].beats * 60.0 / (bpm > 20 ? bpm : 120) * SR);
    return s > DLY_MAX - 4 ? DLY_MAX - 4 : s;
}

void fx_param_text(int type, int i, int v, double bpm, char *b, size_t n) {
    (void)bpm;
    if (type <= 0 || type >= FX_COUNT || i < 0 || i > 2) {
        snprintf(b, n, "--");
        return;
    }
    switch (INFO[type].fmt[i]) {
    case V_TIME:
        if (v < 64) snprintf(b, n, "%dms", (int)(20.0f * powf(50.0f, v / 63.0f)));
        else snprintf(b, n, "%s", NOTES[(v - 64) * 12 / 64].name);
        break;
    case V_SLAP: snprintf(b, n, "%dms", 60 + v * 140 / 127); break;
    case V_JUNO: snprintf(b, n, "%s", v < 43 ? "I" : v < 86 ? "II" : "I+II"); break;
    case V_VIBE: snprintf(b, n, "%s", v < 64 ? "CHORUS" : "VIBRATO"); break;
    case V_BIPOLAR: snprintf(b, n, "%+d", (v - 64) * 100 / 63); break;
    case V_DB: snprintf(b, n, "%+ddB", (int)lroundf((v - 64) * 12 / 63.0f)); break;
    case V_GATE: snprintf(b, n, "%ddB", -80 + v * 60 / 127); break;
    case V_CAB: snprintf(b, n, "%s", CABS[cab_of(v)]); break;
    case V_HZ: case V_HZ_CUT: {
        float hz = INFO[type].fmt[i] == V_HZ ? 20 * powf(1000, k01(v)) : 20 * powf(100, k01(v));
        if (hz < 1000) snprintf(b, n, "%dHz", (int)(hz + 0.5f));
        else snprintf(b, n, "%.1fkHz", hz / 1000);
        break;
    }
    case V_Q: snprintf(b, n, "%.1f", 0.3f * powf(25, k01(v))); break;
    case V_RAW: snprintf(b, n, "%d", v); break;
    case V_MFC: // MFC42's Mod: the envelope below the middle, the LFO above, nothing at the middle
        if (v < 60) snprintf(b, n, "ENV %d", (60 - v) * 100 / 60);
        else if (v > 68) snprintf(b, n, "LFO %d", (v - 68) * 100 / 59);
        else snprintf(b, n, "--");
        break;
    default: snprintf(b, n, "%d", v * 100 / 127); break;
    }
}

// ---- Building blocks ----

struct dl { // a delay line read at any distance, linearly between samples
    float *b;
    int size, w;
};

static inline void dl_write(struct dl *d, float x) {
    d->b[d->w] = x;
    if (++d->w >= d->size) d->w = 0;
}

static inline float dl_read(const struct dl *d, float dist) { // dist >= 1: samples back
    float p = (float)d->w - dist;
    while (p < 0) p += d->size;
    int i = (int)p;
    float f = p - i;
    int j = i + 1 >= d->size ? 0 : i + 1;
    return d->b[i] + f * (d->b[j] - d->b[i]);
}

static inline float dl_tap(const struct dl *d, int dist) {
    int i = d->w - dist;
    while (i < 0) i += d->size;
    return d->b[i];
}

static inline float onepole(float *z, float x, float a) { // low-pass: a near 1 is darker
    *z = x + a * (*z - x);
    return *z;
}

static float lp_coef(float hz) {
    return expf(-2.0f * PI_F * hz / SR);
}

struct biquad {
    float b0, b1, b2, a1, a2, z1[2], z2[2];
};

static void bq_set(struct biquad *q, int kind, float hz, float qv, float db) { // RBJ: 0 LP, 1 HP, 2 BP, 3 peak, 4 low shelf, 5 high shelf
    float A = powf(10.0f, db / 40.0f), w = 2 * PI_F * hz / SR, cw = cosf(w), sw = sinf(w), al = sw / (2 * qv);
    float b0, b1, b2, a0, a1, a2, sA = sqrtf(A);
    switch (kind) {
    case 0: b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = b0, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al; break;
    case 1: b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = b0, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al; break;
    case 2: b0 = al, b1 = 0, b2 = -al, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al; break;
    case 3: b0 = 1 + al * A, b1 = -2 * cw, b2 = 1 - al * A, a0 = 1 + al / A, a1 = -2 * cw, a2 = 1 - al / A; break;
    case 4:
        b0 = A * ((A + 1) - (A - 1) * cw + 2 * sA * al), b1 = 2 * A * ((A - 1) - (A + 1) * cw),
        b2 = A * ((A + 1) - (A - 1) * cw - 2 * sA * al), a0 = (A + 1) + (A - 1) * cw + 2 * sA * al,
        a1 = -2 * ((A - 1) + (A + 1) * cw), a2 = (A + 1) + (A - 1) * cw - 2 * sA * al;
        break;
    default:
        b0 = A * ((A + 1) + (A - 1) * cw + 2 * sA * al), b1 = -2 * A * ((A - 1) + (A + 1) * cw),
        b2 = A * ((A + 1) + (A - 1) * cw - 2 * sA * al), a0 = (A + 1) - (A - 1) * cw + 2 * sA * al,
        a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - 2 * sA * al;
        break;
    }
    q->b0 = b0 / a0, q->b1 = b1 / a0, q->b2 = b2 / a0, q->a1 = a1 / a0, q->a2 = a2 / a0;
}

static inline float bq(struct biquad *q, int ch, float x) {
    float y = q->b0 * x + q->z1[ch];
    q->z1[ch] = q->b1 * x - q->a1 * y + q->z2[ch];
    q->z2[ch] = q->b2 * x - q->a2 * y;
    return y;
}

static inline float allpass1(float *z, float x, float a) { // first order: the phasers' stage
    float y = a * x + *z;
    *z = x - a * y;
    return y;
}

static inline float ap_coef(float hz) {
    float t = tanf(PI_F * (hz < 20 ? 20 : hz > 18000 ? 18000 : hz) / SR);
    return (t - 1) / (t + 1);
}

static inline float follower(float *env, float x, float att, float rel) {
    float a = fabsf(x);
    *env = a > *env ? a + att * (*env - a) : a + rel * (*env - a);
    return *env;
}

// ---- State: big enough for any one effect ----

#define CB 4096 // comb and short delay buffers
struct fx_state {
    float mem[2 * DLY_MAX + 16 * CB + 8 * 8192]; // carved up by each effect
    struct dl d[24];
    float z[64]; // filter and allpass states
    struct biquad q[8];
    double ph, ph2;
    float env, env2, gain, smooth;
    int hold, inited;
    unsigned rnd;
};

struct fx *fx_new(void) {
    struct fx *f = calloc(1, sizeof *f);
    if (!f) return 0;
    f->st = calloc(1, sizeof *f->st);
    if (!f->st) {
        free(f);
        return 0;
    }
    f->bpm = 120;
    fx_reset(f); // touches the buffers now, so the audio thread never waits for memory
    return f;
}

void fx_free(struct fx *f) {
    if (!f) return;
    free(f->st);
    free(f);
}

void fx_reset(struct fx *f) {
    struct fx_state *s = f->st;
    memset(s->z, 0, sizeof s->z);
    memset(s->q, 0, sizeof s->q);
    s->ph = s->ph2 = 0, s->env = s->env2 = 0, s->gain = 1, s->smooth = 0, s->hold = 0, s->inited = 0, s->rnd = 12345;
    memset(s->mem, 0, sizeof s->mem);
}

void fx_set(struct fx *f, int type) {
    f->type = type > 0 && type < FX_COUNT ? type : FX_OFF;
    for (int i = 0; i < 3; i++) f->p[i] = INFO[f->type].def[i];
    fx_reset(f);
}

// Lays the delay lines out over mem. sizes: n lines.
static void lay(struct fx_state *s, const int *sizes, int n) {
    float *m = s->mem;
    for (int i = 0; i < n; i++) {
        s->d[i].b = m, s->d[i].size = sizes[i], s->d[i].w = 0;
        m += sizes[i];
    }
}

// ---- Reverbs ----

// Freeverb's tunings: eight combs and four allpasses a channel, the right one spread wider.
static const int COMB[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int APL[4] = {556, 441, 341, 225};

// Each reverb's Decay: the time its tail takes to fall 60 dB, from the shortest to the longest,
// spread evenly in ratio across the knob.
static float rt60(int type, int decay) {
    float lo, hi;
    switch (type) {
    case FX_ASR_HALL: lo = 1.2f, hi = 6.0f; break;
    case FX_ROOM: lo = 0.3f, hi = 1.8f; break;
    case FX_PLATE: lo = 0.8f, hi = 5.0f; break;
    case FX_SPRING: lo = 1.0f, hi = 4.0f; break;
    case FX_SNARE: lo = 0.4f, hi = 2.0f; break;
    default: lo = 1.0f, hi = 1.0f; break; // GATED: the gate ends it
    }
    return lo * powf(hi / lo, k01(decay));
}

// ASR HALL, ROOM, GATED: a Freeverb tank, scaled. The ASR's is long, dark and a little
// grainy (its output rounded to 12 bits, as the ASR-10's own effects were coarse; outside the
// loop, so the tail still dies away). Each comb's feedback is set from its length so the whole
// tank falls 60 dB in the Decay time.
static void freeverb(struct fx *f, float *buf, int frames, float scale, float predelay_ms, int grain, int gated) {
    struct fx_state *s = f->st;
    if (!s->inited) {
        int sizes[24], n = 0;
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 8; i++) sizes[n++] = (int)((COMB[i] + c * 23) * scale) + 1;
            for (int i = 0; i < 4; i++) sizes[n++] = (int)((APL[i] + c * 23) * scale) + 1;
        }
        lay(s, sizes, 24);
        s->inited = 1;
    }
    struct dl pre = {s->mem + 2 * DLY_MAX, 8820, 0}; // the pre-delay, after the tank's memory
    pre.w = s->hold % 8820;
    float decay = k01(f->p[0]), tone = k01(f->p[1]), mix = k01(f->p[2]);
    float rt = gated ? 1.0f : rt60(f->type, f->p[0]), damp = 0.05f + 0.6f * (1 - tone), fb[8];
    for (int k = 0; k < 8; k++) fb[k] = powf(10.0f, -3.0f * s->d[k].size / (rt * SR));
    int pd = (int)(predelay_ms * SR / 1000) + 1;
    int gate_len = (int)((0.1f + 0.5f * decay) * SR); // GATED: how long it stays open
    for (int i = 0; i < frames; i++) {
        float l = buf[i * 2], r = buf[i * 2 + 1], in = (l + r) * 0.5f;
        dl_write(&pre, in);
        float x = dl_tap(&pre, pd) * 0.03f;
        float out[2];
        for (int c = 0; c < 2; c++) {
            float acc = 0;
            for (int k = 0; k < 8; k++) {
                struct dl *d = &s->d[c * 12 + k];
                float y = dl_tap(d, d->size);
                float *st = &s->z[c * 8 + k];
                *st = y * (1 - damp) + *st * damp;
                dl_write(d, x + *st * fb[k]);
                acc += y;
            }
            for (int k = 0; k < 4; k++) {
                struct dl *d = &s->d[c * 12 + 8 + k];
                float bo = dl_tap(d, d->size);
                dl_write(d, acc + bo * 0.5f);
                acc = bo - acc;
            }
            out[c] = grain ? roundf(acc * 3 * 2048.0f) / (3 * 2048.0f) : acc;
        }
        if (gated) { // open while the input keeps coming, then a hard close
            if (follower(&s->env, in, 0.9f, 0.9995f) > 0.02f) s->ph = gate_len;
            float target = s->ph > 0 ? 1 : 0;
            s->ph -= 1;
            s->gain += (target - s->gain) * (target > s->gain ? 0.05f : 0.01f);
            out[0] *= s->gain, out[1] *= s->gain;
        }
        buf[i * 2] = l * (1 - mix * 0.5f) + out[0] * mix * 3;
        buf[i * 2 + 1] = r * (1 - mix * 0.5f) + out[1] * mix * 3;
    }
    s->hold = pre.w;
}

// PLATE, SNARE: Dattorro's plate (1997), its delays scaled from 29761 Hz.
static void plate(struct fx *f, float *buf, int frames, int snare) {
    struct fx_state *s = f->st;
    const float k = SR / 29761.0f;
    if (!s->inited) {
        int sizes[] = {(int)(142 * k), (int)(107 * k), (int)(379 * k), (int)(277 * k),            // input diffusers
                       (int)(672 * k) + 32, (int)(4453 * k), (int)(1800 * k), (int)(3720 * k), // left half
                       (int)(908 * k) + 32, (int)(4217 * k), (int)(2656 * k), (int)(3163 * k), // right half
                       8820};                                                               // pre-delay
        lay(s, sizes, 13);
        s->inited = 1;
    }
    // Decay per pass: half the tank (about 0.36 s) multiplies by it twice.
    float decay = powf(10.0f, -3.0f * 0.358f / (2 * rt60(f->type, f->p[0])));
    float damp = 0.6f * (1 - k01(f->p[1])), mix = k01(f->p[2]);
    int pd = snare ? 1 : (int)(0.01f * SR);
    float *bw = &s->z[0], *d1 = &s->z[1], *d2 = &s->z[2], *hp = &s->z[3];
    for (int i = 0; i < frames; i++) {
        float l = buf[i * 2], r = buf[i * 2 + 1], x = (l + r) * 0.5f;
        if (snare) x = x - onepole(hp, x, lp_coef(250)); // the snare's crack, not its body
        dl_write(&s->d[12], x);
        x = dl_tap(&s->d[12], pd);
        x = onepole(bw, x, 0.0005f);
        static const float diff[4] = {0.75f, 0.75f, 0.625f, 0.625f};
        for (int j = 0; j < 4; j++) { // input diffusion
            struct dl *d = &s->d[j];
            float v = dl_tap(d, d->size), u = x - diff[j] * v;
            dl_write(d, u);
            x = v + diff[j] * u;
        }
        s->ph += 1.0 / SR;
        float mod = 8.0f * k * sinf(2 * PI_F * (float)s->ph);
        // Each half: a modulated allpass, a delay, damping, decay, an allpass, a delay.
        float tl = x + dl_tap(&s->d[11], s->d[11].size) * decay;
        float tr = x + dl_tap(&s->d[7], s->d[7].size) * decay;
        for (int h = 0; h < 2; h++) {
            int b = h ? 8 : 4;
            float in = h ? tr : tl, *damper = h ? d2 : d1;
            struct dl *ap = &s->d[b];
            float v = dl_read(ap, ap->size - 32 + (h ? -mod : mod)), u = in + 0.7f * v;
            dl_write(ap, u);
            in = v - 0.7f * u;
            dl_write(&s->d[b + 1], in);
            float y = dl_tap(&s->d[b + 1], s->d[b + 1].size);
            y = onepole(damper, y, damp) * decay;
            struct dl *ap2 = &s->d[b + 2];
            float v2 = dl_tap(ap2, ap2->size), u2 = y - 0.5f * v2;
            dl_write(ap2, u2);
            dl_write(&s->d[b + 3], v2 + 0.5f * u2);
        }
#define T(i, n) dl_tap(&s->d[i], (int)((n) * k))
        float ol = T(9, 266) + T(9, 2974) - T(10, 1913) + T(11, 1996) - T(5, 1990) - T(6, 187) - T(7, 1066);
        float orr = T(5, 353) + T(5, 3627) - T(6, 1228) + T(7, 2673) - T(9, 2111) - T(10, 335) - T(11, 121);
#undef T
        buf[i * 2] = l * (1 - mix * 0.5f) + ol * 0.6f * mix;
        buf[i * 2 + 1] = r * (1 - mix * 0.5f) + orr * 0.6f * mix;
    }
}

// SPRING: a delay fed back through a chain of allpasses, which smear each echo into the
// spring's chirp; darkened, and boinging on transients.
static void spring(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    if (!s->inited) {
        int sizes[2] = {2048, 2048};
        lay(s, sizes, 2);
        s->inited = 1;
    }
    float fb = 0.55f + 0.37f * k01(f->p[0]), lp = lp_coef(1500 + 4000 * k01(f->p[1])), mix = k01(f->p[2]);
    for (int i = 0; i < frames; i++) {
        float l = buf[i * 2], r = buf[i * 2 + 1];
        for (int c = 0; c < 2; c++) {
            float in = c ? r : l;
            float y = dl_tap(&s->d[c], c ? 1601 : 1499); // ~35 ms
            for (int j = 0; j < 12; j++) y = allpass1(&s->z[c * 16 + j], y, 0.62f);
            y = onepole(&s->z[c * 16 + 12], y, lp);
            y -= onepole(&s->z[c * 16 + 13], y, lp_coef(120)); // no low rumble
            dl_write(&s->d[c], tanhf(in * 0.5f + y * fb));
            buf[i * 2 + c] = in * (1 - mix * 0.5f) + y * mix * 1.6f;
        }
    }
}

// ---- Delays ----

static void delay(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    if (!s->inited) {
        int sizes[2] = {DLY_MAX, DLY_MAX};
        lay(s, sizes, 2);
        s->smooth = delay_samples(f->p[0], f->bpm);
        s->inited = 1;
    }
    int t = f->type;
    float target = t == FX_SLAP ? (60 + f->p[0] * 140 / 127) * SR / 1000 : delay_samples(f->p[0], f->bpm);
    float fbk = k01(f->p[1]), mix = k01(f->p[2]);
    float fb = t == FX_DUB ? 1.15f * fbk : t == FX_SLAP ? 0 : 0.95f * fbk;
    float lp = t == FX_DUB ? lp_coef(2200) : t == FX_TAPE ? lp_coef(5500) : t == FX_ANALOG ? lp_coef(2800)
             : t == FX_SLAP ? lp_coef(1500 + 6000 * k01(f->p[1])) : 0;
    float wet = t == FX_TAPE ? 0.45f : mix;
    float wow = t == FX_TAPE ? k01(f->p[2]) : t == FX_ANALOG ? 0.15f : 0;
    for (int i = 0; i < frames; i++) {
        s->smooth += (target - s->smooth) * 0.0005f; // a moved time glides, like the tape slowing
        s->ph += 0.5 / SR, s->ph2 += 6.0 / SR;
        float dt = s->smooth * (1 + wow * (0.004f * sinf(2 * PI_F * (float)s->ph) + 0.0012f * sinf(2 * PI_F * (float)s->ph2)));
        if (dt < 2) dt = 2;
        float l = buf[i * 2], r = buf[i * 2 + 1], y[2];
        for (int c = 0; c < 2; c++) y[c] = dl_read(&s->d[c], dt);
        for (int c = 0; c < 2; c++) {
            float in = c ? r : l, back = t == FX_PINGPONG ? y[1 - c] : y[c];
            if (t == FX_PINGPONG && c == 1) in = 0; // into the left, then bouncing
            float w = in + back * fb;
            if (lp > 0) w = onepole(&s->z[c], w, lp);
            if (t == FX_DUB) w -= onepole(&s->z[2 + c], w, lp_coef(150)), w = tanhf(w * 1.2f) / 1.2f;
            if (t == FX_TAPE || t == FX_ANALOG) w = tanhf(w);
            dl_write(&s->d[c], w);
        }
        buf[i * 2] = l + y[0] * wet;
        buf[i * 2 + 1] = r + y[1] * wet;
    }
}

// ---- Modulation ----

static float tri(double ph) {
    float p = (float)(ph - floor(ph));
    return p < 0.5f ? 4 * p - 1 : 3 - 4 * p;
}

// JUNO CHORUS: the Juno-60's bucket-brigade chorus. I and II are slow and wide, I+II fast and
// shallow; the left and right taps move against each other.
static void juno(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    if (!s->inited) {
        int sizes[2] = {CB, CB};
        lay(s, sizes, 2);
        s->inited = 1;
    }
    int mode = f->p[0] < 43 ? 0 : f->p[0] < 86 ? 1 : 2;
    static const float rate[3] = {0.513f, 0.863f, 9.75f}, depth[3] = {1.85f, 1.85f, 0.3f};
    float lp = lp_coef(3000 + 9000 * k01(f->p[1])), mix = k01(f->p[2]);
    for (int i = 0; i < frames; i++) {
        s->ph += rate[mode] / SR;
        float m = tri(s->ph) * depth[mode] * SR / 1000, base = 3.4f * SR / 1000;
        float l = buf[i * 2], r = buf[i * 2 + 1], in = (l + r) * 0.5f;
        dl_write(&s->d[0], in);
        float wl = onepole(&s->z[0], dl_read(&s->d[0], base + m), lp);
        float wr = onepole(&s->z[1], dl_read(&s->d[0], base - m), lp);
        buf[i * 2] = l * (1 - mix * 0.5f) + wl * mix;
        buf[i * 2 + 1] = r * (1 - mix * 0.5f) + wr * mix;
    }
}

// The phasers: chains of first-order allpasses swept by an LFO, mixed with the dry sound.
static void phaser(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    int t = f->type, stages = t == FX_DP4 ? 8 : 4;
    float rate = 0.05f + 7.95f * k01(f->p[0]) * k01(f->p[0]), depth = k01(f->p[1]);
    float fb = t == FX_DP4 ? 0.85f * k01(f->p[2]) : t == FX_SMALLSTONE ? 0.75f * k01(f->p[1]) : 0.25f;
    float mix = t == FX_DP4 ? 0.5f : k01(f->p[2]);
    float lo = t == FX_SMALLSTONE ? 100 : 250, hi = t == FX_DP4 ? 4000 : t == FX_SMALLSTONE ? 3500 : 2500;
    if (t == FX_SMALLSTONE) depth = 1;
    for (int i = 0; i < frames; i++) {
        s->ph += rate / SR;
        for (int c = 0; c < 2; c++) {
            // The DP4 spreads its two sides a quarter turn apart; the pedals are mono.
            double ph = s->ph + (t == FX_DP4 && c ? 0.25 : 0);
            float lfo = 0.5f + 0.5f * sinf(2 * PI_F * (float)ph);
            float hz = lo * powf(hi / lo, 0.5f - 0.5f * depth + depth * lfo);
            float a = ap_coef(hz), x = buf[i * 2 + c], y = x + s->z[40 + c] * fb;
            for (int j = 0; j < stages; j++) y = allpass1(&s->z[c * 16 + j], y, a);
            s->z[40 + c] = y;
            buf[i * 2 + c] = x * (1 - mix) + y * mix;
        }
    }
}

// UNIVIBE: four allpass stages at staggered frequencies, all swept by a lamp: an LFO that
// glows up slowly and fades, so the throb is uneven. CHORUS mixes in the dry, VIBRATO not.
static void univibe(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float rate = 0.5f + 9.5f * k01(f->p[0]) * k01(f->p[0]), depth = k01(f->p[1]);
    int vibrato = f->p[2] >= 64;
    static const float stagger[4] = {1.0f, 2.2f, 6.8f, 15.0f};
    for (int i = 0; i < frames; i++) {
        s->ph += rate / SR;
        float lamp = 0.5f + 0.5f * sinf(2 * PI_F * (float)s->ph);
        lamp = lamp * lamp; // the bulb: quick to brighten, slow to fade
        for (int c = 0; c < 2; c++) {
            float x = buf[i * 2 + c], y = x;
            for (int j = 0; j < 4; j++) y = allpass1(&s->z[c * 8 + j], y, ap_coef(80 * stagger[j] * (1 + 4 * depth * lamp)));
            buf[i * 2 + c] = vibrato ? y : 0.7f * (x + y);
        }
    }
}

static void flanger(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    if (!s->inited) {
        int sizes[2] = {CB, CB};
        lay(s, sizes, 2);
        s->inited = 1;
    }
    float rate = 0.05f + 4.95f * k01(f->p[0]) * k01(f->p[0]), depth = k01(f->p[1]);
    float fb = 0.9f * (f->p[2] - 64) / 63.0f;
    for (int i = 0; i < frames; i++) {
        s->ph += rate / SR;
        for (int c = 0; c < 2; c++) {
            float lfo = 0.5f + 0.5f * tri(s->ph + (c ? 0.25 : 0));
            float dt = (0.3f + 6.5f * depth * lfo) * SR / 1000;
            float x = buf[i * 2 + c], y = dl_read(&s->d[c], dt + 1);
            dl_write(&s->d[c], x + y * fb);
            buf[i * 2 + c] = 0.6f * (x + y);
        }
    }
}

// RHODES TREM: the Suitcase's stereo vibrato. Stereo at 0 is plain tremolo; turned up, the
// two sides move against each other until it pans across.
static void rhodes(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float rate = 0.5f + 8.5f * k01(f->p[0]), depth = k01(f->p[1]), stereo = k01(f->p[2]);
    for (int i = 0; i < frames; i++) {
        s->ph += rate / SR;
        float a = sinf(2 * PI_F * (float)s->ph), b = sinf(2 * PI_F * (float)s->ph + PI_F * stereo);
        buf[i * 2] *= 1 - depth * (0.5f + 0.5f * a);
        buf[i * 2 + 1] *= 1 - depth * (0.5f + 0.5f * b);
    }
}

// AUTO-WAH: the playing's level opens a resonant filter.
static void autowah(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float sens = 1 + 20 * k01(f->p[0]), range = 1 + 15 * k01(f->p[1]), q = 1 + 9 * k01(f->p[2]);
    for (int i = 0; i < frames; i++) {
        float in = 0.5f * (buf[i * 2] + buf[i * 2 + 1]);
        float e = follower(&s->env, in, 0.99f, 0.9995f) * sens;
        float hz = 250 * powf(range, e > 1 ? 1 : e);
        float g = tanf(PI_F * hz / SR), k = 1 / q, h = 1 / (1 + g * (g + k));
        for (int c = 0; c < 2; c++) { // a state-variable band-pass, a little low-pass blended in
            float *s1 = &s->z[c * 2], *s2 = &s->z[c * 2 + 1], x = buf[i * 2 + c];
            float hp = h * (x - (g + k) * *s1 - *s2), bp = g * hp + *s1, lp = g * bp + *s2;
            *s1 = g * hp + bp, *s2 = g * bp + lp;
            buf[i * 2 + c] = bp * k * 8.0f + lp * 0.3f;
        }
    }
}

// WAH PEDAL: a Cry Baby's sweep, about 350 Hz heel down to 2.2 kHz toe, with its vocal peak.
// The pedal is K1, the slider on the Input Effects page, or the mod wheel; moves are smoothed so
// the sweep never steps.
static void wah(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float target = 350 * powf(2200 / 350.0f, k01(f->p[0])), q = 2.5f + 7.5f * k01(f->p[1]);
    float level = powf(10, (f->p[2] - 64) * 12 / 63.0f / 20);
    if (!s->inited) s->smooth = target, s->inited = 1;
    float k = 1 / q;
    for (int i = 0; i < frames; i++) {
        s->smooth += (target - s->smooth) * 0.003f;
        float g = tanf(PI_F * s->smooth / SR), h = 1 / (1 + g * (g + k));
        for (int c = 0; c < 2; c++) {
            float *s1 = &s->z[c * 2], *s2 = &s->z[c * 2 + 1], x = buf[i * 2 + c];
            float hp = h * (x - (g + k) * *s1 - *s2), bp = g * hp + *s1, lp = g * bp + *s2;
            *s1 = g * hp + bp, *s2 = g * bp + lp;
            // The peak, a little of the lows the pedal lets through, made up to about the
            // level it came in at.
            buf[i * 2 + c] = (bp * k * 7.0f + lp * 0.25f) * level;
        }
    }
}

// ---- Drive and dynamics ----

static void klon(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float gain = 1 + 40 * k01(f->p[0]) * k01(f->p[0]), out = 0.55f * k01(f->p[2]);
    bq_set(&s->q[0], 3, 1000, 0.7f, 6 * k01(f->p[0])); // its push in the mids, more with gain
    bq_set(&s->q[1], 5, 2500, 0.7f, 12 * (k01(f->p[1]) - 0.5f));
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float x = buf[i * 2 + c], m = bq(&s->q[0], c, x);
            float clip = tanhf(m * gain) / (1 + 0.15f * gain); // germanium diodes: soft
            float y = x * (1 - 0.6f * k01(f->p[0])) + clip * 1.2f; // the clean always under it
            buf[i * 2 + c] = bq(&s->q[1], c, y) * out;
        }
}

static void od1(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float gain = 1 + 60 * k01(f->p[0]), level = 1.2f * k01(f->p[1]), mix = k01(f->p[2]);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float x = buf[i * 2 + c];
            float hp = x - onepole(&s->z[c], x, lp_coef(500)); // the bass kept out of the clipping
            float v = (hp + x * 0.15f) * gain;
            float y = v > 0 ? tanhf(v) : tanhf(v * 0.7f) / 0.7f; // the OD-1's asymmetric clip
            y = onepole(&s->z[2 + c], y, lp_coef(5000));
            buf[i * 2 + c] = x * (1 - mix) + y * level * mix * 0.32f;
        }
}

static void ds1(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float gain = 3 + 300 * k01(f->p[0]) * k01(f->p[0]), tone = k01(f->p[1]), level = 0.8f * k01(f->p[2]);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float x = buf[i * 2 + c];
            x -= onepole(&s->z[c], x, lp_coef(100));
            float v = x * gain, y = v / (1 + fabsf(v)); // hard: transistor boost into diodes
            y = y > 0.8f ? 0.8f : y < -0.8f ? -0.8f : y;
            // The DS-1's tone: a low-pass and a high-pass blended, scooping the middle.
            float lo = onepole(&s->z[2 + c], y, lp_coef(234)), hi = y - onepole(&s->z[4 + c], y, lp_coef(1200));
            buf[i * 2 + c] = (lo * (1 - tone) + hi * tone * 1.5f) * level;
        }
}

// DYNACOMP and CS-3: compressors, fast and squashy (an OTA), or smoother with a tone control.
static void comp(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    int cs3 = f->type == FX_CS3;
    float sens = k01(f->p[0]);
    float att_ms = cs3 ? 1 + 49 * k01(f->p[1]) : 0.5f + 19.5f * k01(f->p[2]);
    float att = expf(-1 / (att_ms * SR / 1000)), rel = expf(-1 / (0.25f * SR));
    float thresh = powf(10, (-10 - 30 * sens) / 20), ratio = cs3 ? 3 + 9 * sens : 8 + 12 * sens;
    float makeup = cs3 ? 1.3f * (1 + 1.5f * sens) : 1.4f * (0.4f + 1.2f * k01(f->p[1])) * (1 + 1.2f * sens);
    if (cs3) bq_set(&s->q[0], 5, 3000, 0.7f, 12 * ((f->p[2] - 64) / 63.0f));
    for (int i = 0; i < frames; i++) {
        float l = buf[i * 2], r = buf[i * 2 + 1];
        float e = follower(&s->env, fabsf(l) > fabsf(r) ? l : r, att, rel);
        float g = e > thresh ? powf(thresh / e, 1 - 1 / ratio) : 1;
        for (int c = 0; c < 2; c++) {
            float y = buf[i * 2 + c] * g * makeup;
            y = cs3 ? bq(&s->q[0], c, y) : y;
            buf[i * 2 + c] = y / (1 + 0.25f * fabsf(y)); // the attack's first instant rounded off, not clipped
        }
    }
}

static void gate(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float thresh = powf(10, (-80 + 60 * k01(f->p[0])) / 20), rel = expf(-1 / ((10 + 490 * k01(f->p[1])) * SR / 1000));
    float floor = powf(10, -80 * k01(f->p[2]) / 20);
    for (int i = 0; i < frames; i++) {
        float e = follower(&s->env, fabsf(buf[i * 2]) > fabsf(buf[i * 2 + 1]) ? buf[i * 2] : buf[i * 2 + 1], 0.99f, 0.9995f);
        float target = e > thresh ? 1 : floor;
        s->gain = target > s->gain ? s->gain + (target - s->gain) * 0.2f : target + rel * (s->gain - target);
        buf[i * 2] *= s->gain, buf[i * 2 + 1] *= s->gain;
    }
}

// ---- Amps and EQ ----

// The Fender tone stack: Yeh's discretization of the '59 Bassman's treble/mid/bass network.
struct stack {
    float b[4], a[4];
};

static void tonestack(struct stack *st, float t, float m, float l) {
    const double C1 = 250e-12, C2 = 20e-9, C3 = 20e-9, R1 = 250e3, R2 = 1e6, R3 = 25e3, R4 = 56e3;
    l = expf((l - 1) * 3.4f); // the bass pot is log taper
    double b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
    double b2 = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) +
                (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
    double b3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) + t * C1 * C2 * C3 * R1 * R3 * R4 -
                t * m * C1 * C2 * C3 * R1 * R3 * R4 + t * l * C1 * C2 * C3 * R1 * R2 * R4;
    double a0 = 1;
    double a1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
    double a2 = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
                (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
    double a3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4) +
                l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;
    double c = 2 * SR, c2 = c * c, c3 = c2 * c;
    double B0 = -b1 * c - b2 * c2 - b3 * c3, B1 = -b1 * c + b2 * c2 + 3 * b3 * c3, B2 = b1 * c + b2 * c2 - 3 * b3 * c3,
           B3 = b1 * c - b2 * c2 + b3 * c3;
    double A0 = -a0 - a1 * c - a2 * c2 - a3 * c3, A1 = -3 * a0 - a1 * c + a2 * c2 + 3 * a3 * c3,
           A2 = -3 * a0 + a1 * c + a2 * c2 - 3 * a3 * c3, A3 = -a0 + a1 * c - a2 * c2 + a3 * c3;
    st->b[0] = (float)(B0 / A0), st->b[1] = (float)(B1 / A0), st->b[2] = (float)(B2 / A0), st->b[3] = (float)(B3 / A0);
    st->a[0] = 1, st->a[1] = (float)(A1 / A0), st->a[2] = (float)(A2 / A0), st->a[3] = (float)(A3 / A0);
}

static inline float stack_run(const struct stack *st, float *z, float x) { // transposed direct form II
    float y = st->b[0] * x + z[0];
    z[0] = st->b[1] * x - st->a[1] * y + z[1];
    z[1] = st->b[2] * x - st->a[2] * y + z[2];
    z[2] = st->b[3] * x - st->a[3] * y;
    return y;
}

static void fender_eq(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    struct stack st;
    tonestack(&st, k01(f->p[2]), k01(f->p[1]), k01(f->p[0]));
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) buf[i * 2 + c] = stack_run(&st, &s->z[c * 4], buf[i * 2 + c]) * 2.0f; // its loss made up
}

// BASSMAN and AC30: a preamp stage, the amp's tone shaping and a power stage, ending at the
// output transformer. No speaker: SPEAKER SIM after it is the cabinet.
static void amp(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    int ac30 = f->type == FX_AC30;
    float gain = 1 + (ac30 ? 30 : 25) * k01(f->p[0]) * k01(f->p[0]);
    struct stack st;
    if (!ac30) tonestack(&st, k01(f->p[1]), 0.5f, 0.6f);
    if (ac30) { // top boost: bright, then Cut darkens the power stage
        bq_set(&s->q[0], 5, 2000, 0.7f, 6);
        bq_set(&s->q[1], 0, 12000 - 9000 * k01(f->p[1]), 0.7f, 0);
        bq_set(&s->q[2], 3, 800, 1.0f, 1.5f); // the EL84s' forward mids
        bq_set(&s->q[3], 0, 10000, 0.7f, 0);
        bq_set(&s->q[4], 1, 70, 0.7f, 0);
    } else { // presence, then the transformer
        bq_set(&s->q[0], 5, 3000, 0.7f, 9 * k01(f->p[2]));
        bq_set(&s->q[2], 3, 100, 1.0f, 1);
        bq_set(&s->q[3], 0, 9000, 0.7f, 0);
        bq_set(&s->q[4], 1, 50, 0.7f, 0);
    }
    float vol = ac30 ? 0.4f + 1.4f * k01(f->p[2]) : 1.0f;
    float trim = (ac30 ? 0.45f : 0.3f) / (1 + 0.25f * (gain - 1)); // more gain drives harder, not much louder
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float x = buf[i * 2 + c] * gain;
            float pre = tanhf(x + 0.2f) - tanhf(0.2f); // a triode: a little asymmetric
            float y;
            if (ac30) {
                y = bq(&s->q[0], c, pre);
                y = tanhf(y * vol * 1.5f) / 1.2f; // EL84s: chime, then a soft squash
                y = bq(&s->q[1], c, y);
            } else {
                y = stack_run(&st, &s->z[c * 4], pre) * 4.0f;
                y = tanhf(y * 1.3f); // 6L6s
                y = bq(&s->q[0], c, y);
            }
            y = bq(&s->q[4], c, bq(&s->q[3], c, bq(&s->q[2], c, y)));
            buf[i * 2 + c] = y * trim;
        }
}


// JTM45: a hotter triode preamp, the Marshall's tone stack (mids up, unlike the Fender's), and
// KT66s that break up later and rounder than EL34s. Tone is the treble; presence after.
static void jtm45(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float gain = 1 + 40 * k01(f->p[0]) * k01(f->p[0]);
    struct stack st;
    tonestack(&st, k01(f->p[1]), 0.8f, 0.45f);
    bq_set(&s->q[0], 5, 3000, 0.7f, 4 * (1 - k01(f->p[0]))); // the bright cap: less as it is turned up
    bq_set(&s->q[1], 5, 3500, 0.7f, 8 * k01(f->p[2]));       // presence
    bq_set(&s->q[2], 3, 700, 0.9f, 2.5f);                    // the midrange push
    bq_set(&s->q[3], 0, 8000, 0.7f, 0);
    bq_set(&s->q[4], 1, 60, 0.7f, 0);
    float trim = 0.34f / (1 + 0.22f * (gain - 1));
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float x = bq(&s->q[0], c, buf[i * 2 + c]) * gain;
            float pre = tanhf(x + 0.25f) - tanhf(0.25f);
            pre = tanhf(pre * 1.6f + 0.1f) - tanhf(0.1f); // a second stage, as the JTM45 cascades
            float y = stack_run(&st, &s->z[c * 4], pre) * 4.0f;
            y = tanhf(y * 1.1f) * 0.9f; // KT66s
            y = bq(&s->q[1], c, y);
            y = bq(&s->q[4], c, bq(&s->q[3], c, bq(&s->q[2], c, y)));
            buf[i * 2 + c] = y * trim * (1 + gain * 0.02f);
        }
}

// FUZZ FACE: two germanium transistors with a lot of gain, clipping lopsided; the bass thins
// as it is fed by the guitar's pickup. Bias moves the operating point: off the middle it spits
// and gates. Rolling the guitar down (a quieter input) cleans it up, as on the pedal.
static void fuzz(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float gain = 2 + 220 * k01(f->p[0]) * k01(f->p[0]), level = 0.9f * k01(f->p[1]);
    float bias = (f->p[2] - 64) / 63.0f * 0.6f;
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float x = buf[i * 2 + c];
            x -= onepole(&s->z[c], x, lp_coef(120)); // the low end the pedal loses
            float v = x * gain + bias;
            float y = v > 0 ? tanhf(v) : tanhf(v * 0.55f) / 0.55f * 0.8f; // lopsided
            y -= tanhf(bias) * (bias > 0 ? 1.0f : 1.0f / 0.55f * 0.8f * 0.55f);
            y = onepole(&s->z[2 + c], y, lp_coef(4500)); // germanium's soft top
            buf[i * 2 + c] = y * level * 0.2f;
        }
}

// JC-120: a clean solid-state amp, flat and loud, clipping hard only at the very top, with its
// stereo chorus: the left side dry, the right through a moving delay. Bright lifts the top.
static void jc(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    if (!s->inited) {
        int sizes[2] = {CB, CB};
        lay(s, sizes, 2);
        s->inited = 1;
    }
    bq_set(&s->q[0], 5, 3000, 0.7f, 8 * k01(f->p[0]));
    bq_set(&s->q[1], 3, 500, 0.8f, -2); // its slight mid scoop
    float depth = k01(f->p[1]), vol = 0.3f + 1.4f * k01(f->p[2]);
    for (int i = 0; i < frames; i++) {
        s->ph += 0.7 / SR;
        float l = bq(&s->q[1], 0, bq(&s->q[0], 0, buf[i * 2])) * vol;
        float r = bq(&s->q[1], 1, bq(&s->q[0], 1, buf[i * 2 + 1])) * vol;
        float m = (l + r) * 0.5f;
        dl_write(&s->d[0], m);
        float dt = (4.0f + 2.5f * depth * (0.5f + 0.5f * sinf(2 * PI_F * (float)s->ph))) * SR / 1000;
        float wet = dl_read(&s->d[0], dt);
        float yl = depth > 0.01f ? l : l, yr = depth > 0.01f ? r * (1 - depth) + wet * depth : r;
        yl = yl > 1.2f ? 1.2f : yl < -1.2f ? -1.2f : yl; // transistors: clean, then a wall
        yr = yr > 1.2f ? 1.2f : yr < -1.2f ? -1.2f : yr;
        buf[i * 2] = yl * 0.95f, buf[i * 2 + 1] = yr * 0.95f;
    }
}

// DC30: EL84s in class A like the AC30, but a fuller, thicker voice and more gain on tap; Tone
// tilts from dark to bright, Cut takes the top off the power stage.
static void dc30(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float gain = 1 + 34 * k01(f->p[0]) * k01(f->p[0]), tone = k01(f->p[1]) - 0.5f;
    bq_set(&s->q[0], 5, 1800, 0.7f, 10 * tone);
    bq_set(&s->q[1], 4, 250, 0.7f, -6 * tone);
    bq_set(&s->q[2], 3, 1100, 0.9f, 2.0f); // its thicker mids
    bq_set(&s->q[3], 0, 11000 - 8000 * k01(f->p[2]), 0.7f, 0);
    bq_set(&s->q[4], 1, 70, 0.7f, 0);
    float trim = 0.42f / (1 + 0.24f * (gain - 1));
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float x = buf[i * 2 + c] * gain;
            float pre = tanhf(x + 0.2f) - tanhf(0.2f);
            float y = bq(&s->q[1], c, bq(&s->q[0], c, pre));
            y = tanhf(y * 1.6f) / 1.2f; // EL84s
            y = bq(&s->q[3], c, bq(&s->q[2], c, y));
            buf[i * 2 + c] = bq(&s->q[4], c, y) * trim * (1 + gain * 0.02f);
        }
}

// MFC42: a resonant 4-pole low-pass, the cutoff swept by an envelope (Mod below the middle:
// the playing opens it) or a slow LFO (above the middle).
static void mfc42(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float base = k01(f->p[0]), res = k01(f->p[1]), envd = 0, lfod = 0;
    if (f->p[2] < 60) envd = (60 - f->p[2]) / 60.0f;
    else if (f->p[2] > 68) lfod = (f->p[2] - 68) / 59.0f;
    float k = 2.0f - 1.9f * res; // each section's damping
    for (int i = 0; i < frames; i++) {
        float in = 0.5f * (fabsf(buf[i * 2]) + fabsf(buf[i * 2 + 1]));
        s->env = in > s->env ? s->env + (in - s->env) * 0.01f : s->env * 0.9995f;
        s->ph += 0.35 / SR;
        float sweep = envd * (s->env * 4 > 1 ? 1 : s->env * 4) * 0.5f + lfod * 0.4f * sinf(2 * PI_F * (float)s->ph);
        float pos = base + sweep;
        pos = pos < 0 ? 0 : pos > 1 ? 1 : pos;
        float hz = 40 * powf(400.0f, pos);
        if (hz > 0.45f * SR) hz = 0.45f * SR;
        float g = tanf(PI_F * hz / SR), h = 1 / (1 + g * (g + k));
        for (int c = 0; c < 2; c++) {
            float x = buf[i * 2 + c] / (1 + res); // the level held as resonance rises
            for (int sec = 0; sec < 2; sec++) {
                float *s1 = &s->z[c * 4 + sec * 2], *s2 = s1 + 1;
                float hp = h * (x - (g + k) * *s1 - *s2), bp = g * hp + *s1, lp = g * bp + *s2;
                *s1 = g * hp + bp, *s2 = g * bp + lp;
                x = lp;
            }
            buf[i * 2 + c] = tanhf(x * (1 + res));
        }
    }
}

// B-15: the Portaflex, a tube bass amp with its own 15" speaker: round lows, a soft top, and a
// gentle squash as Volume pushes it. Bass and Treble are its two tone controls.
static void b15(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    float drive = 1 + 5 * k01(f->p[2]);
    bq_set(&s->q[0], 4, 100, 0.7f, (f->p[0] - 64) * 12 / 63.0f);
    bq_set(&s->q[1], 5, 2500, 0.7f, (f->p[1] - 64) * 12 / 63.0f);
    bq_set(&s->q[2], 3, 180, 0.8f, 3); // the cabinet's low-mid weight
    bq_set(&s->q[3], 0, 3200, 0.7f, 0); // the 15" speaker: little above 3 kHz
    bq_set(&s->q[4], 1, 40, 0.7f, 0);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float y = bq(&s->q[1], c, bq(&s->q[0], c, buf[i * 2 + c]));
            y = tanhf(y * drive * 0.8f) / (0.8f * (1 + 0.15f * (drive - 1))); // 6L6s, soft
            y = bq(&s->q[4], c, bq(&s->q[3], c, bq(&s->q[2], c, y)));
            buf[i * 2 + c] = y * 0.42f;
        }
}

// SPEAKER SIM: a miked cabinet. Each cab is its low resonance, its midrange shape, the cone's
// presence peak and the steep fall above it; Mic moves from off the cone's axis (dark) to
// right on it (bright).
static void speaker(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    static const struct {
        float hp, low, low_db, low_q, mid, mid_db, pres, pres_db, lp;
    } CAB[4] = {
        {70, 120, 2.5f, 1.0f, 500, -2.0f, 2500, 3.0f, 5500},  // 1x12 open back: airy, a soft low end
        {80, 110, 2.0f, 1.2f, 1200, 1.5f, 3000, 5.0f, 6000},  // 2x12 Alnico Blues: chime, bright upper mids
        {60, 100, 3.0f, 1.0f, 450, -1.0f, 2000, 3.0f, 4800},  // 4x10 tweed: round lows, warm top
        {75, 100, 5.0f, 1.4f, 750, -2.5f, 2200, 4.0f, 4500},  // 4x12 closed, Greenbacks: thump, a mid scoop
    };
    int c0 = cab_of(f->p[0]);
    float mic = k01(f->p[1]) * 2 - 1; // -1 off axis .. 1 on axis
    float level = powf(10, (f->p[2] - 64) * 12 / 63.0f / 20);
    bq_set(&s->q[0], 1, CAB[c0].hp, 0.8f, 0);
    bq_set(&s->q[1], 3, CAB[c0].low, CAB[c0].low_q, CAB[c0].low_db);
    bq_set(&s->q[2], 3, CAB[c0].mid, 0.8f, CAB[c0].mid_db);
    bq_set(&s->q[3], 3, CAB[c0].pres, 1.5f, CAB[c0].pres_db + 3 * mic);
    float lp = CAB[c0].lp * powf(1.4f, mic);
    bq_set(&s->q[4], 0, lp, 0.6f, 0);
    bq_set(&s->q[5], 0, lp * 1.1f, 1.0f, 0);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float y = buf[i * 2 + c];
            for (int k = 0; k < 6; k++) y = bq(&s->q[k], c, y);
            buf[i * 2 + c] = y * level * 0.9f;
        }
}

// The drum EQs: each band a shelf, a peak or (HI-HAT's Low Cut) a high-pass at the control's
// frequency. kind as bq_set's; hz 0 means the control sets the frequency.
static const struct {
    int kind;
    float hz, q;
} BANDS[7][3] = {
    [FX_EQ_KICK - FX_PARAM_EQ] = {{4, 60, 0.7f}, {3, 400, 1.2f}, {3, 4000, 1.0f}},
    [FX_EQ_SNARE - FX_PARAM_EQ] = {{3, 200, 1.0f}, {3, 900, 2.0f}, {5, 5000, 0.7f}},
    [FX_EQ_HAT - FX_PARAM_EQ] = {{1, 0, 0.7f}, {3, 3500, 1.5f}, {5, 10000, 0.7f}},
    [FX_EQ_CLAP - FX_PARAM_EQ] = {{3, 300, 1.0f}, {3, 1200, 1.0f}, {5, 8000, 0.7f}},
    [FX_EQ_TOM - FX_PARAM_EQ] = {{3, 100, 1.0f}, {3, 500, 1.2f}, {3, 5000, 1.0f}},
    [FX_EQ_808 - FX_PARAM_EQ] = {{4, 50, 0.7f}, {3, 250, 1.0f}, {3, 1500, 1.0f}},
};

static void drum_eq(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    int n = 3;
    if (f->type == FX_PARAM_EQ) {
        bq_set(&s->q[0], 3, 20 * powf(1000, k01(f->p[0])), 0.3f * powf(25, k01(f->p[2])), (f->p[1] - 64) * 12 / 63.0f);
        n = 1;
    } else {
        for (int b = 0; b < 3; b++) {
            int k = f->type - FX_PARAM_EQ;
            if (BANDS[k][b].kind == 1) bq_set(&s->q[b], 1, 20 * powf(100, k01(f->p[b])), BANDS[k][b].q, 0);
            else bq_set(&s->q[b], BANDS[k][b].kind, BANDS[k][b].hz, BANDS[k][b].q, (f->p[b] - 64) * 12 / 63.0f);
        }
    }
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            float y = buf[i * 2 + c];
            for (int b = 0; b < n; b++) y = bq(&s->q[b], c, y);
            buf[i * 2 + c] = y;
        }
}

static void eq3(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    bq_set(&s->q[0], 4, 120, 0.7f, (f->p[0] - 64) * 12 / 63.0f);
    bq_set(&s->q[1], 3, 1000, 0.7f, (f->p[1] - 64) * 12 / 63.0f);
    bq_set(&s->q[2], 5, 6000, 0.7f, (f->p[2] - 64) * 12 / 63.0f);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) buf[i * 2 + c] = bq(&s->q[2], c, bq(&s->q[1], c, bq(&s->q[0], c, buf[i * 2 + c])));
}

// VINYL SIM: after the SP-303's. A record's band (no deep lows, a soft top, a little warmth in
// the low mids), a compressor that pumps as Comp goes up, crackle and hiss on Noise, and the
// turntable's slow wow and quick flutter as a moving delay on Wow Flut.
static void vinyl(struct fx *f, float *buf, int frames) {
    struct fx_state *s = f->st;
    if (!s->inited) {
        int sizes[2] = {CB, CB};
        lay(s, sizes, 2);
        s->gain = 1, s->rnd = 0x1234567u, s->inited = 1;
    }
    float comp = k01(f->p[0]), noise = k01(f->p[1]), wow = k01(f->p[2]);
    bq_set(&s->q[0], 1, 45, 0.7f, 0);
    bq_set(&s->q[1], 3, 180, 0.8f, 2.0f);
    bq_set(&s->q[2], 0, 11000 - 4000 * comp, 0.7f, 0);
    float thresh = powf(10, -comp * 30 / 20), ratio = 1 + 5 * comp; // up to -30 dB at 6:1
    float makeup = powf(10, comp * 30 * (1 - 1 / ratio) * 0.7f / 20);
    float att = expf(-1 / (0.003f * SR)), rel = expf(-1 / (0.12f * SR));
    for (int i = 0; i < frames; i++) {
        s->ph += 0.55 / SR, s->ph2 += 6.5 / SR;
        float dt = 40 + wow * (90 * sinf(2 * PI_F * (float)s->ph) + 18 * sinf(2 * PI_F * (float)s->ph2) + 110);
        float l = buf[i * 2], r = buf[i * 2 + 1];
        float e = follower(&s->env, fabsf(l) > fabsf(r) ? l : r, att, rel);
        float g = e > thresh ? powf(thresh / e, 1 - 1 / ratio) : 1;
        // The needle: hiss always, a crackle now and then, more of both with Noise.
        s->rnd = s->rnd * 1664525u + 1013904223u;
        float white = (float)(s->rnd >> 8) / (float)(1u << 24) * 2 - 1, hiss = onepole(&s->z[20], white, lp_coef(5000));
        float crackle = 0;
        if (noise > 0 && (s->rnd >> 9) % 44100 < (unsigned)(noise * 30)) s->smooth = (white > 0 ? 1 : -1) * (0.2f + 0.5f * fabsf(white));
        crackle = s->smooth, s->smooth *= 0.6f;
        // The noise rides on the music (a slow follower): with nothing playing, nothing at all,
        // so a send of 0 is silent and the master is quiet between songs.
        float there = follower(&s->env2, fabsf(l) + fabsf(r), 0.999f, 0.99995f) * 30;
        float nz = noise * (hiss * 0.004f + crackle * 0.08f) * (there > 1 ? 1 : there);
        float in[2] = {l, r};
        for (int c = 0; c < 2; c++) {
            float x = bq(&s->q[2], c, bq(&s->q[1], c, bq(&s->q[0], c, in[c])));
            dl_write(&s->d[c], x);
            float y = wow > 0 ? dl_read(&s->d[c], dt) : x;
            y = y * g * makeup + nz;
            buf[i * 2 + c] = tanhf(y * 1.1f) / 1.1f; // the 303's output, rounding off what pokes out
        }
    }
}

// How much of the input an effect passes straight through at these controls, so a send can
// take it out again: reverbs, delays and modulation add to the dry; the rest replace it.
static float dry_gain(const struct fx *f) {
    float mix = k01(f->p[2]);
    switch (f->type) {
    case FX_ASR_HALL: case FX_ROOM: case FX_PLATE: case FX_SPRING: case FX_SNARE: case FX_GATED: case FX_JUNO:
        return 1 - mix * 0.5f;
    case FX_DUB: case FX_DDL: case FX_TAPE: case FX_ANALOG: case FX_PINGPONG: case FX_SLAP: return 1;
    case FX_DP4: return 0.5f;
    case FX_MXR_PH: case FX_SMALLSTONE: return 1 - mix;
    case FX_FLANGER: return 0.6f;
    case FX_UNIVIBE: return f->p[2] >= 64 ? 0 : 0.7f;
    case FX_OD1: return 1 - mix;
    default: return 0; // EQ, drive, dynamics, amps: the whole processed sound comes back (parallel)
    }
}

void fx_process_send(struct fx *f, float *buf, int frames) {
    float dry[2048];
    for (int at = 0; at < frames; at += 1024) {
        int n = frames - at < 1024 ? frames - at : 1024;
        float *b = buf + at * 2, g;
        memcpy(dry, b, sizeof(float) * 2 * (size_t)n);
        fx_process(f, b, n);
        g = dry_gain(f);
        if (g != 0)
            for (int i = 0; i < n * 2; i++) b[i] -= dry[i] * g;
    }
}

void fx_process(struct fx *f, float *buf, int frames) {
    switch (f->type) {
    case FX_ASR_HALL: freeverb(f, buf, frames, 1.6f, 30, 1, 0); break;
    case FX_ROOM: freeverb(f, buf, frames, 0.55f, 8, 0, 0); break;
    case FX_GATED: freeverb(f, buf, frames, 1.0f, 10, 0, 1); break;
    case FX_PLATE: plate(f, buf, frames, 0); break;
    case FX_SNARE: plate(f, buf, frames, 1); break;
    case FX_SPRING: spring(f, buf, frames); break;
    case FX_DUB: case FX_DDL: case FX_TAPE: case FX_ANALOG: case FX_PINGPONG: case FX_SLAP: delay(f, buf, frames); break;
    case FX_JUNO: juno(f, buf, frames); break;
    case FX_UNIVIBE: univibe(f, buf, frames); break;
    case FX_DP4: case FX_MXR_PH: case FX_SMALLSTONE: phaser(f, buf, frames); break;
    case FX_FLANGER: flanger(f, buf, frames); break;
    case FX_RHODES: rhodes(f, buf, frames); break;
    case FX_AUTOWAH: autowah(f, buf, frames); break;
    case FX_WAH: wah(f, buf, frames); break;
    case FX_SPEAKER: speaker(f, buf, frames); break;
    case FX_VINYL: vinyl(f, buf, frames); break;
    case FX_PARAM_EQ: case FX_EQ_KICK: case FX_EQ_SNARE: case FX_EQ_HAT: case FX_EQ_CLAP: case FX_EQ_TOM: case FX_EQ_808:
        drum_eq(f, buf, frames);
        break;
    case FX_KLON: klon(f, buf, frames); break;
    case FX_OD1: od1(f, buf, frames); break;
    case FX_DS1: ds1(f, buf, frames); break;
    case FX_DYNACOMP: case FX_CS3: comp(f, buf, frames); break;
    case FX_GATE: gate(f, buf, frames); break;
    case FX_BASSMAN: case FX_AC30: amp(f, buf, frames); break;
    case FX_FENDER_EQ: fender_eq(f, buf, frames); break;
    case FX_EQ3: eq3(f, buf, frames); break;
    case FX_JTM45: jtm45(f, buf, frames); break;
    case FX_FUZZ: fuzz(f, buf, frames); break;
    case FX_JC: jc(f, buf, frames); break;
    case FX_DC30: dc30(f, buf, frames); break;
    case FX_MFC42: mfc42(f, buf, frames); break;
    case FX_B15: b15(f, buf, frames); break;
    default: break;
    }
    int lost = 0;
    for (int i = 0; i < frames * 2; i++) // never let anything blow up the outputs
        if (!(buf[i] > -4.0f && buf[i] < 4.0f)) {
            if (buf[i] != buf[i] || buf[i] > 1e6f || buf[i] < -1e6f) lost = 1; // NaN, or gone far beyond
            buf[i] = buf[i] != buf[i] ? 0 : buf[i] > 0 ? 4.0f : -4.0f;
        }
    if (lost) { // its state is ruined: start it again rather than let it sit silent or howling
        memset(buf, 0, (size_t)frames * 2 * sizeof *buf);
        fx_reset(f);
    }
}

double fx_tail_seconds(const struct fx *f) {
    float d = k01(f->p[0]), fb = k01(f->p[1]);
    switch (f->type) {
    case FX_ASR_HALL: case FX_ROOM: case FX_PLATE: case FX_SNARE: return rt60(f->type, f->p[0]) + 0.1;
    case FX_SPRING: return 1 + 3 * d;
    case FX_GATED: return 0.2 + 0.6 * d;
    case FX_DUB: case FX_DDL: case FX_TAPE: case FX_ANALOG: case FX_PINGPONG: {
        double t = delay_samples(f->p[0], f->bpm) / SR, g = f->type == FX_DUB ? 1.15 * fb : 0.95 * fb;
        double repeats = g < 0.05 ? 1 : g >= 0.999 ? 40 : log(0.001) / log(g); // until -60 dB
        double s = t * (repeats + 1);
        return s > 10 ? 10 : s;
    }
    case FX_SLAP: return 0.25;
    default: return 0.05;
    }
}

// ---- The chain ----

int fx_chain_init(struct fx_chain *c) {
    atomic_store(&c->on, 0);
    atomic_store(&c->input, FXIN_MONO_L);
    atomic_store(&c->mode, FXM_INPUT);
    atomic_store(&c->bpm10, 1200);
    for (int i = 0; i < FX_SLOTS; i++) {
        atomic_store(&c->type[i], FX_OFF);
        for (int k = 0; k < 3; k++) atomic_store(&c->p[i][k], 0);
        if (!(c->slot[i] = fx_new())) return -1;
    }
    return 0;
}

// A slot's effect brought up to what the UI set: a new effect starts from silence.
static struct fx *sync_slot(struct fx_chain *c, int i, double bpm) {
    struct fx *f = c->slot[i];
    if (!f) return 0;
    int type = atomic_load_explicit(&c->type[i], memory_order_relaxed);
    if (type != f->type) {
        f->type = type;
        fx_reset(f);
    }
    if (type == FX_OFF) return 0;
    for (int k = 0; k < 3; k++) f->p[k] = atomic_load_explicit(&c->p[i][k], memory_order_relaxed);
    f->bpm = bpm;
    return f;
}

static void measure(struct fx_chain *c, const struct timespec *t0, int frames) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double took = (t1.tv_sec - t0->tv_sec) + (t1.tv_nsec - t0->tv_nsec) * 1e-9, block = frames / (double)SR;
    float load = (float)(took / block), was = atomic_load_explicit(&c->load, memory_order_relaxed);
    atomic_store_explicit(&c->load, load > was ? load : was * 0.98f + load * 0.02f, memory_order_relaxed);
}

void fx_chain_process(struct fx_chain *c, float *buf, int frames) {
    if (atomic_load_explicit(&c->mode, memory_order_relaxed) != FXM_INPUT) return; // working elsewhere
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double bpm = atomic_load_explicit(&c->bpm10, memory_order_relaxed) / 10.0;
    int input = atomic_load_explicit(&c->input, memory_order_relaxed);
    if (input == FXIN_MIC) { // the mic is on one side: whichever carries it, to both
        float el = 0, er = 0;
        for (int i = 0; i < frames; i++) el += buf[i * 2] * buf[i * 2], er += buf[i * 2 + 1] * buf[i * 2 + 1];
        input = el >= er ? FXIN_MONO_L : FXIN_MONO_R;
    }
    if (input == FXIN_MONO_L)
        for (int i = 0; i < frames; i++) buf[i * 2 + 1] = buf[i * 2];
    else if (input == FXIN_MONO_R)
        for (int i = 0; i < frames; i++) buf[i * 2] = buf[i * 2 + 1];
    for (int i = 0; i < FX_SLOTS; i++) {
        struct fx *f = sync_slot(c, i, bpm);
        if (f) fx_process(f, buf, frames);
    }
    measure(c, &t0, frames);
}

void fx_chain_buses(struct fx_chain *c, float *const *buses, float *out, int frames) {
    int mode = atomic_load_explicit(&c->mode, memory_order_relaxed), on = atomic_load_explicit(&c->on, memory_order_relaxed);
    if (mode == FXM_INPUT) return;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double bpm = atomic_load_explicit(&c->bpm10, memory_order_relaxed) / 10.0;
    if (mode == FXM_PAD) { // the pad's sound through the chain, or straight on with the chain off
        float *b = buses[FX_SLOTS];
        for (int i = 0; on && i < FX_SLOTS; i++) {
            struct fx *f = sync_slot(c, i, bpm);
            if (f) fx_process(f, b, frames);
        }
        for (int i = 0; i < frames * 2; i++) out[i] += b[i];
    } else if (on) { // each send through its own effect; tails ring on after the sends stop
        for (int s = 0; s < FX_SLOTS; s++) {
            struct fx *f = sync_slot(c, s, bpm);
            if (!f) continue;
            fx_process_send(f, buses[s], frames);
            for (int i = 0; i < frames * 2; i++) out[i] += buses[s][i];
        }
    }
    measure(c, &t0, frames);
}

// ---- The instrument stage ----

static void rbj(float c[5], int kind, float hz, float q, float db) { // 0 high-pass, 1 peak
    float w = 2 * PI_F * hz / SR, cw = cosf(w), al = sinf(w) / (2 * q), A = powf(10, db / 40), b0, b1, b2, a0, a1, a2;
    if (kind == 0) b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
    else b0 = 1 + al * A, b1 = -2 * cw, b2 = 1 - al * A, a0 = 1 + al / A, a1 = -2 * cw, a2 = 1 - al / A;
    c[0] = b0 / a0, c[1] = b1 / a0, c[2] = b2 / a0, c[3] = a1 / a0, c[4] = a2 / a0;
}

static inline float run(const float c[5], float *z, float x) { // transposed direct form II
    float y = c[0] * x + z[0];
    z[0] = c[1] * x - c[3] * y + z[1];
    z[1] = c[2] * x - c[4] * y;
    return y;
}

void fx_instrument(struct fx_chain *c, float *buf, int frames) {
    int gain = atomic_load_explicit(&c->inst_gain, memory_order_relaxed);
    int bright = atomic_load_explicit(&c->inst_bright, memory_order_relaxed);
    if (gain != c->inst_set_gain || bright != c->inst_set_bright || !c->inst_c[0][0]) {
        rbj(c->inst_c[0], 0, 40, 0.707f, 0);                               // under a bass's low B
        rbj(c->inst_c[1], 1, 3500, 1.2f, 9.0f * (float)bright / 100.0f); // the pickup's lost resonance
        c->inst_set_gain = gain, c->inst_set_bright = bright;
    }
    float g = powf(10, (float)gain / 20);
    for (int i = 0; i < frames; i++) {
        float x = buf[i * 2]; // the tip: a mono cable's signal
        x = run(c->inst_c[0], &c->inst_z[0], x);
        x = run(c->inst_c[1], &c->inst_z[2], x) * g;
        float a = fabsf(x); // clean to 0.8 of full scale, then rounded off, never past 0.98
        if (a > 0.8f) x = (x > 0 ? 1 : -1) * (0.8f + 0.18f * tanhf((a - 0.8f) / 0.18f));
        if (!(x == x)) x = 0, memset(c->inst_z, 0, sizeof c->inst_z);
        buf[i * 2] = buf[i * 2 + 1] = x;
    }
}
