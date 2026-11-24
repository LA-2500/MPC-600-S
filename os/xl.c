// MPC 2000XL files: a program (.PGM), all programs (.APS), all sequences and songs (.ALL) and one
// sequence (.MID, a MIDI file with the 2000XL's own text events). Loaded and saved.
//
// The layouts, as the 2000XL writes them (all numbers little-endian):
//
// A program body (in a .PGM after its sound names, in an .APS after an index byte and 5 bytes):
//   name (16, spaces, then 0), the slider (9), program change (1), 5 bytes, then 64 notes (35..98)
//   of 25 bytes in a .PGM, 26 in an .APS (the sound number is 1 byte there, 2 here; 0xFF / 0xFFFF
//   none): sound, mode, velocity range low, also play 1, high, also play 2, voice overlap (0 POLY,
//   1 MONO, 2 NOTE OFF), mute assign 1 and 2 (notes), tune (16 bits signed, tenths of a semitone),
//   attack, decay (0..100), decay mode (0 END, 1 START), cutoff (0..100, 100 open), resonance
//   (0..15), then nine velocity and slider settings. Then 1 byte, 64 note mixers of 6 bytes (FX out,
//   level 0..100, pan 0..100 (50 the middle), individual level, output, FX send), 3 bytes, the 64
//   pads' notes (A01..D16), 200 bytes of effect settings.
// A .PGM: 07 04, the sound count (16 bits), each sound's name (17), 30 0, then the body.
// An .APS: 0A 05, the sound count, the names, 24 0, the set's name (17), 7 bytes of settings, 1, the
//   64 pads' notes, 7 bytes, four drums (DRUM 1..4: 64 note mixers, 2 bytes, receive program change
//   and volume, 1, the drum's program, 2 bytes) with 4 bytes between and 1 after; then each program:
//   its number (0..23) and its body; 0xFF ends the list.
// An .ALL: "MPC2KXL ALL 1.00", 2048 bytes of settings, 99 sequences' names (16) and sizes (2), 20
//   songs (528 bytes each: name, 250 steps of sequence and repeats (0xFF unused), 2 bytes, used,
//   loop first and last step, loop on, 6 bytes), then each used sequence: 10240 bytes (name, tempo,
//   bars, the 64 tracks' names, MIDI devices, buses (0 MIDI, 1..4 DRUM 1..4), program changes,
//   velocity ratios, status (1 used, 2 on, 4 send program change); each bar's ticks per beat and end)
//   then its events of 8 bytes, in time order: a note's are the tick (20 bits), its track (6) and
//   duration (14, in three parts), the note, the velocity and the note variation. Eight 0xFF end it
//   (twice when the events take an even number of 8-byte segments).
// The sounds are .SND files, each named as the sound (8 characters, as the 2000XL's card has them).
#include "os.h"
#include "seq.h"
#include "seqimport.h"
#include "snd.h"
#include "wav.h"
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// The 2000XL's pads, A01 to D16, and the notes it gives them as it comes.
const unsigned char XL_PAD_NOTES[64] = {37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53, 54, 69, 81, 80, 65, 66,
                                        76, 77, 56, 62, 63, 64, 73, 74, 71, 39, 52, 57, 58, 59, 60, 61, 67, 68, 70, 72, 75, 78,
                                        79, 35, 41, 50, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98};

// The bytes it fills a program with that are not set note by note: its slider, the five after the
// program change, and the effect settings at the end. As the 2000XL writes them.
static const unsigned char XL_SLIDER[9] = {0, 136, 120, 12, 45, 0, 20, 206, 50};
static const unsigned char XL_AFTER_PC[5] = {35, 64, 0, 26, 0};
static const unsigned char XL_FX[200] = {
    2, 0, 72, 0, 208, 7, 0, 0, 99, 1, 20, 8, 29, 252, 50, 51, 2, 50, 60, 8, 5, 10, 20, 20, 50,
    0, 0, 2, 15, 25, 0, 5, 65, 20, 30, 1, 5, 0, 0, 5, 99, 0, 244, 255, 12, 0, 0, 0, 0, 0,
    0, 0, 2, 0, 79, 1, 79, 1, 0, 66, 79, 1, 0, 66, 79, 1, 0, 66, 50, 0, 99, 40, 0, 60, 0,
    0, 208, 7, 0, 0, 99, 1, 20, 8, 29, 252, 50, 51, 2, 50, 60, 8, 5, 10, 20, 20, 50, 0, 0, 2,
    15, 25, 0, 5, 65, 20, 30, 1, 5, 0, 0, 5, 99, 0, 244, 255, 12, 0, 0, 0, 0, 0, 0, 0, 2,
    0, 79, 1, 79, 1, 0, 66, 79, 1, 0, 66, 79, 1, 0, 66, 50, 0, 99, 40, 0, 60, 0, 0, 4, 0,
    12, 0, 0, 0, 50, 0, 35, 0, 62, 51, 90, 50, 20, 0, 0, 0, 50, 0, 35, 0, 62, 51, 90, 50, 20,
    0, 0, 0, 50, 0, 35, 0, 62, 51, 90, 50, 20, 0, 0, 0, 50, 0, 35, 0, 62, 51, 90, 50, 20, 0};

#define XL_NOTES 64
#define XL_PROGRAMS 24
#define XL_TRACKS 64
#define XL_NAME 8 // a file's or sound's name on the 2000XL's card

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static unsigned u16(const unsigned char *p) {
    return p[0] | p[1] << 8;
}

static void put16(unsigned char *p, unsigned v) {
    p[0] = v & 255, p[1] = v >> 8 & 255;
}

static unsigned char *read_all(const char *path, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = *n > 0 && *n < (64L << 20) ? malloc((size_t)*n) : 0;
    if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) free(b), b = 0;
    fclose(f);
    return b;
}

static void name_of(char *out, const unsigned char *in, int n) { // printable, trailing spaces gone
    int j = 0;
    for (int i = 0; i < n && in[i]; i++) out[j++] = in[i] >= 32 && in[i] < 127 ? (char)in[i] : ' ';
    while (j && out[j - 1] == ' ') j--;
    out[j] = 0;
}

static void pad_name(unsigned char *out, const char *in, int n) { // spaces after it
    int l = (int)strlen(in);
    for (int i = 0; i < n; i++) out[i] = i < l ? (unsigned char)in[i] : ' ';
}

// ---- Programs ----

struct xl_note {
    int sound; // -1 none
    int overlap, mute1, mute2, tune, attack, decay, decay_mode, cutoff, reso;
    int level, pan; // its mixer
};

struct xl_prog {
    char name[17];
    int number;
    struct xl_note note[XL_NOTES];
    unsigned char pads[64];
};

// A program body from its name on; wide: an .APS's 2-byte sound numbers. Returns its length.
static long prog_parse(const unsigned char *b, long n, int wide, struct xl_prog *g) {
    int nl = 25 + wide;
    long len = 17 + 9 + 1 + 5 + XL_NOTES * nl + 1 + 384 + 3 + 64 + 200;
    if (n < len) return -1;
    name_of(g->name, b, 16);
    const unsigned char *q = b + 32;
    for (int i = 0; i < XL_NOTES; i++, q += nl) {
        struct xl_note *x = &g->note[i];
        unsigned si = wide ? u16(q) : q[0];
        x->sound = si == (wide ? 0xFFFFu : 0xFFu) ? -1 : (int)si;
        const unsigned char *o = q + 1 + wide;
        x->overlap = o[5], x->mute1 = o[6], x->mute2 = o[7];
        x->tune = (int16_t)u16(o + 8);
        x->attack = o[10], x->decay = o[11], x->decay_mode = o[12], x->cutoff = o[13], x->reso = o[14];
    }
    const unsigned char *m = b + 32 + XL_NOTES * nl + 1;
    for (int i = 0; i < XL_NOTES; i++) g->note[i].level = m[i * 6 + 1], g->note[i].pan = m[i * 6 + 2];
    memcpy(g->pads, m + 384 + 3, 64);
    return len;
}

static int ms_of(int v) { // the 2000XL's 0..100 attack and decay, as milliseconds
    v = clampi(v, 0, 100);
    return v * v * 3;
}

static int sqrt_ms(int ms) { // and back
    return clampi((int)(sqrt(ms / 3.0) + 0.5), 0, 100);
}

// Its sounds, read once each (a sound on several pads is one sound).
struct xl_sounds {
    int n;
    char (*name)[17];
    struct sample **s;
    struct snd_info *info;
    unsigned char *tried;
};

static struct sample *sound_get(struct os *s, struct xl_sounds *l, int i, const char *dir) {
    if (i < 0 || i >= l->n) return 0;
    if (!l->tried[i]) {
        l->tried[i] = 1;
        char file[900];
        memset(&l->info[i], 0, sizeof l->info[i]);
        if (disk_find_sound(dir, l->name[i], file, sizeof file)) {
            size_t fl = strlen(file);
            if (fl > 4 && strcasecmp(file + fl - 4, ".snd") == 0) l->s[i] = snd_load(file, &l->info[i]);
            else l->s[i] = wav_load(file), l->info[i].end = l->s[i] ? l->s[i]->frames : 0;
        }
        if (!l->s[i]) s->mpc_missing++;
    }
    return l->s[i];
}

static void sounds_done(struct xl_sounds *l) {
    for (int i = 0; i < l->n; i++)
        if (l->s[i] && l->s[i]->refs == 0) sample_free(l->s[i]); // read, but on no pad
    free(l->name), free(l->s), free(l->info), free(l->tried);
}

static int sounds_init(struct xl_sounds *l, const unsigned char *names, int n) {
    l->n = n;
    l->name = calloc((size_t)(n ? n : 1), sizeof *l->name);
    l->s = calloc((size_t)(n ? n : 1), sizeof *l->s);
    l->info = calloc((size_t)(n ? n : 1), sizeof *l->info);
    l->tried = calloc((size_t)(n ? n : 1), 1);
    if (!l->name || !l->s || !l->info || !l->tried) return -1;
    for (int i = 0; i < n; i++) name_of(l->name[i], names + i * 17, 16);
    return 0;
}

// Notes that cut each other (mute assign) as the MPC 600 S's mute groups: notes joined in a group.
static void mute_groups(const struct xl_prog *g, int group[XL_NOTES]) {
    int root[XL_NOTES];
    for (int i = 0; i < XL_NOTES; i++) root[i] = i;
    for (int i = 0; i < XL_NOTES; i++)
        for (int k = 0; k < 2; k++) {
            int m = (k ? g->note[i].mute2 : g->note[i].mute1) - 35;
            if (m < 0 || m >= XL_NOTES || m == i) continue;
            int a = i, b = m;
            while (root[a] != a) a = root[a];
            while (root[b] != b) b = root[b];
            if (a != b) root[b] = a;
        }
    int next = 0, of[XL_NOTES];
    for (int i = 0; i < XL_NOTES; i++) of[i] = 0, group[i] = 0;
    for (int i = 0; i < XL_NOTES; i++) {
        int a = i, size = 0;
        while (root[a] != a) a = root[a];
        for (int j = 0; j < XL_NOTES; j++) {
            int b = j;
            while (root[b] != b) b = root[b];
            size += b == a;
        }
        if (size < 2) continue;
        if (!of[a]) of[a] = ++next;
        group[i] = of[a] <= 32 ? of[a] : 0;
    }
}

// A 2000XL program onto program `to`: pad A01..D16 plays the sound its note has, as it is set there.
static int prog_apply(struct os *s, int to, const struct xl_prog *g, struct xl_sounds *l, const char *dir) {
    int keep = s->program, loaded = 0, group[XL_NOTES];
    mute_groups(g, group);
    os_program_clear(s, to);
    os_set_program(s, to);
    snprintf(s->programs[to].name, sizeof s->programs[to].name, "%.16s", g->name);
    for (int pad = 0; pad < 64 && pad < OS_PADS; pad++) {
        int nt = g->pads[pad] - 35;
        if (nt < 0 || nt >= XL_NOTES) continue;
        const struct xl_note *x = &g->note[nt];
        struct sample *sm = sound_get(s, l, x->sound, dir);
        if (!sm) continue;
        const struct snd_info *in = &l->info[x->sound];
        os_pad_load(s, pad, sm, l->name[x->sound]);
        struct pad_params *p = &s->pads[pad].p;
        if (in->end > in->start && in->end <= sm->frames) p->start = in->start, p->end = in->end;
        if (in->loop_on && in->loop_len > 0 && in->loop_len <= p->end) p->loop = p->end - in->loop_len, p->loop_on = 1;
        int t = clampi(in->tune + x->tune, -360, 360); // the sound's tune and the note's, in tenths
        p->semi = t / 10, p->fine = t % 10 * 10;
        p->volume = clampi(x->level, 0, 100);
        p->pan = clampi(x->pan - 50, -50, 50);
        p->attack_ms = ms_of(x->attack);
        p->decay_end = x->decay_mode == 0;
        p->decay_ms = x->decay_mode == 0 && x->decay <= 5 ? 0 : ms_of(x->decay); // END, as it comes: to the end
        p->gate = x->overlap == 2;                                                // NOTE OFF: stops with the note
        p->cutoff = clampi(x->cutoff, 0, 100), p->reso = clampi(x->reso * 100 / 15, 0, 100);
        p->filter = p->cutoff < 100 ? FILTER_LP2 : FILTER_OFF;
        p->mute_group = group[nt];
        os_pad_push(s, pad);
        loaded++;
    }
    os_set_program(s, keep);
    return loaded;
}

static void dir_of(const char *path, char *dir, size_t n) {
    snprintf(dir, n, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else snprintf(dir, n, ".");
}

int disk_xl_program_load(struct os *s, int to, const char *path) {
    s->mpc_missing = 0;
    long n;
    unsigned char *b = read_all(path, &n);
    if (!b) return -1;
    if (n < 8 || b[0] != 7 || b[1] != 4) return free(b), -1;
    int ns = (int)u16(b + 2);
    long body = 4 + (long)ns * 17 + 2;
    struct xl_prog *g = calloc(1, sizeof *g);
    struct xl_sounds l = {0};
    int rc = -1;
    if (g && body < n && prog_parse(b + body, n - body, 0, g) > 0 && sounds_init(&l, b + 4, ns) == 0) {
        char dir[700];
        dir_of(path, dir, sizeof dir);
        os_undo_save(s, "Load Program");
        rc = prog_apply(s, to, g, &l, dir);
    }
    sounds_done(&l);
    free(g), free(b);
    return rc;
}

// An .APS: its sound names, and each program (up to 24) with its number. Returns how many.
static int aps_parse(const unsigned char *b, long n, struct xl_prog *out, int *ns, int drum_prog[4]) {
    if (n < 8 || b[0] != 0x0A || b[1] != 5) return -1;
    *ns = (int)u16(b + 2);
    long p = 4 + (long)*ns * 17 + 2 + 17 + 7 + 1 + 64 + 7;
    for (int d = 0; d < 4; d++) {
        if (p + 392 > n) return -1;
        drum_prog[d] = b[p + 384 + 5];
        p += 392 + (d < 3 ? 4 : 1);
    }
    int got = 0;
    while (p < n && got < XL_PROGRAMS) {
        int index = b[p++];
        if (index >= XL_PROGRAMS) break;
        if (p + 5 > n) break;
        out[got].number = index;
        long len = prog_parse(b + p + 5, n - p - 5, 1, &out[got]);
        if (len < 0) break;
        p += 5 + len, got++;
    }
    return got;
}

int disk_xl_aps_load(struct os *s, const char *path, int *programs) {
    *programs = 0, s->mpc_missing = 0;
    long n;
    unsigned char *b = read_all(path, &n);
    if (!b) return -1;
    struct xl_prog *g = calloc(XL_PROGRAMS, sizeof *g);
    int ns = 0, drums[4], np = g ? aps_parse(b, n, g, &ns, drums) : -1, pads = 0;
    struct xl_sounds l = {0};
    if (np > 0 && sounds_init(&l, b + 4, ns) == 0) {
        char dir[700];
        dir_of(path, dir, sizeof dir);
        os_undo_save(s, "Load Programs");
        for (int i = 0; i < np; i++) { // into the empty programs, in order
            int to = -1;
            for (int k = 0; k < OS_PROGRAMS && to < 0; k++)
                if (!os_program_sounds(s, k)) to = k;
            if (to < 0) break;
            int got = prog_apply(s, to, &g[i], &l, dir);
            if (got > 0) pads += got, (*programs)++;
            else os_program_clear(s, to), snprintf(s->programs[to].name, sizeof s->programs[to].name, "Program%02d", to + 1);
        }
    }
    sounds_done(&l);
    free(g), free(b);
    return np < 0 ? -1 : pads;
}

// ---- Saving programs ----

// The sounds a save writes: each sound with its trim and loop once, as an .SND of its own name.
struct xl_out {
    int n;
    struct {
        const struct sample *s;
        int32_t start, end, loop, loop_on;
        int whole; // one trim for the sound: saved whole, the trim in the .SND (still trimmable there)
        char name[XL_NAME + 1];
    } x[XL_PROGRAMS * 64];
};

static void clean8(const char *in, char *out) { // a name the 2000XL's card takes: A-Z 0-9 _ -, 8 long
    int j = 0;
    for (int i = 0; in[i] && j < XL_NAME; i++) {
        int c = toupper((unsigned char)in[i]);
        out[j++] = (char)(isalnum(c) || c == '-' || c == '_' ? c : '_');
    }
    out[j] = 0;
    if (!j) snprintf(out, XL_NAME + 1, "SOUND");
}

// Another pad (any program, A01-D16) plays this sound with another trim: it is chopped.
static int chopped(const struct os *s, const struct sample *x, int32_t from, int32_t to) {
    for (int g = 0; g < OS_PROGRAMS; g++)
        for (int i = 0; i < 64 && i < OS_PADS; i++) {
            const struct os_pad *pd = &s->programs[g].pads[i];
            if (pd->s != x) continue;
            int32_t t = pd->p.end > pd->p.start && pd->p.end <= x->frames ? pd->p.end : x->frames, f = pd->p.start < t ? pd->p.start : 0;
            if (f != from || t != to) return 1;
        }
    return 0;
}

static int out_sound(const struct os *s, struct xl_out *o, const struct os_pad *pad) {
    const struct pad_params *p = &pad->p;
    int32_t end = p->end > p->start && p->end <= pad->s->frames ? p->end : pad->s->frames, start = p->start < end ? p->start : 0;
    for (int i = 0; i < o->n; i++)
        if (o->x[i].s == pad->s && o->x[i].start == start && o->x[i].end == end && o->x[i].loop_on == p->loop_on &&
            (!p->loop_on || o->x[i].loop == p->loop))
            return i;
    if (o->n >= 256) return -1; // the 2000XL holds 256 sounds
    int i = o->n++;
    o->x[i].s = pad->s, o->x[i].start = start, o->x[i].end = end, o->x[i].loop = p->loop, o->x[i].loop_on = p->loop_on;
    o->x[i].whole = !chopped(s, pad->s, start, end);
    char base[XL_NAME + 1];
    clean8(pad->name, base);
    snprintf(o->x[i].name, sizeof o->x[i].name, "%s", base);
    for (int k = 2;; k++) { // each its own name: KICK, KICK2, KICK3 (or KIC10)
        int clash = 0;
        for (int j = 0; j < i && !clash; j++) clash = !strcmp(o->x[j].name, o->x[i].name);
        if (!clash) break;
        char num[8];
        int nl = snprintf(num, sizeof num, "%d", k), bl = (int)strlen(base);
        snprintf(o->x[i].name, sizeof o->x[i].name, "%.*s%s", bl + nl > XL_NAME ? XL_NAME - nl : bl, base, num);
    }
    return i;
}

// A sound at 44.1 kHz, as the 2000XL plays it: another rate is converted (as the MPC 2500 save
// does), so it sounds at the same pitch there. NULL: it is 44.1 already (or no memory).
static struct sample *at_44k(const struct sample *x) {
    if (x->rate <= 0 || x->rate == 44100) return 0;
    double k = 44100.0 / x->rate;
    int32_t frames = (int32_t)(x->frames * k);
    struct sample *n = frames > 0 ? sample_new(frames, x->channels, 44100) : 0;
    for (int32_t i = 0; n && i < frames; i++) {
        double at = i / k;
        int32_t a = (int32_t)at, b = a + 1 < x->frames ? a + 1 : a;
        float t = (float)(at - a);
        for (int c = 0; c < x->channels; c++)
            n->data[(size_t)i * (size_t)x->channels + (size_t)c] =
                x->data[(size_t)a * (size_t)x->channels + (size_t)c] * (1 - t) + x->data[(size_t)b * (size_t)x->channels + (size_t)c] * t;
    }
    return n;
}

static int write_sounds(const struct xl_out *o, const char *dir) {
    for (int i = 0; i < o->n; i++) {
        struct sample v = *o->x[i].s;
        int32_t ts = o->x[i].start, te = o->x[i].end; // the trim, in the sound written
        if (!o->x[i].whole) { // a zone of a sound pads chop: only the zone
            v.data += (size_t)ts * (size_t)v.channels;
            v.frames = te - ts;
            te -= ts, ts = 0;
        }
        int32_t loop_len = o->x[i].loop_on && o->x[i].loop >= o->x[i].start && o->x[i].loop < o->x[i].end ? o->x[i].end - o->x[i].loop : 0;
        struct sample *c = at_44k(&v);
        if (c) {
            double k = 44100.0 / v.rate;
            loop_len = (int32_t)(loop_len * k), ts = (int32_t)(ts * k), te = (int32_t)(te * k), v = *c;
        }
        if (te > v.frames || te <= ts) te = v.frames;
        if (loop_len > te) loop_len = te;
        struct snd_info in = {0};
        snprintf(in.name, sizeof in.name, "%s", o->x[i].name);
        in.level = 100, in.beats = 4, in.start = ts, in.end = te;
        in.loop_on = o->x[i].loop_on, in.loop_len = loop_len;
        char path[900];
        snprintf(path, sizeof path, "%s/%s.SND", dir, o->x[i].name);
        int rc = snd_save(path, &v, &in);
        sample_free(c);
        if (rc != 0) return -1;
    }
    return 0;
}

// A program's body (from its name), its sounds numbered in o. wide: the .APS's.
static long prog_write(unsigned char *b, const struct os *s, int prog, struct xl_out *o, int wide, int *dropped) {
    const struct os_program *g = &s->programs[prog];
    int nl = 25 + wide;
    memset(b, 0, 17 + 9 + 1 + 5 + XL_NOTES * nl + 1 + 384 + 3 + 64 + 200);
    pad_name(b, g->name, 16);
    memcpy(b + 17, XL_SLIDER, 9);
    memcpy(b + 27, XL_AFTER_PC, 5);
    unsigned char *q = b + 32, *m = b + 32 + XL_NOTES * nl + 1;
    for (int i = 0; i < XL_NOTES; i++) { // every note as the 2000XL leaves it, then the pads' own
        unsigned char *x = q + i * nl, *mx = m + i * 6;
        if (wide) put16(x, 0xFFFF);
        else x[0] = 0xFF;
        unsigned char *o2 = x + 1 + wide;
        o2[1] = 44, o2[3] = 88, o2[11] = 5, o2[13] = 100, o2[18] = 100;
        mx[1] = 100, mx[2] = 50, mx[3] = 100;
    }
    b[32 + XL_NOTES * nl] = 6;
    m[384 + 1] = 64;
    memcpy(m + 384 + 3, XL_PAD_NOTES, 64);
    memcpy(m + 384 + 3 + 64, XL_FX, sizeof XL_FX);
    int group_note[33] = {0}; // a mute group's first note: the others mute it, and it them
    for (int pad = 0; pad < OS_PADS; pad++) {
        const struct os_pad *pd = &g->pads[pad];
        if (!pd->s) continue;
        if (pad >= 64) {
            (*dropped)++; // banks E to H: the 2000XL has four
            continue;
        }
        int si = out_sound(s, o, pd);
        if (si < 0 || (!wide && si > 254)) {
            (*dropped)++;
            continue;
        }
        int nt = XL_PAD_NOTES[pad] - 35;
        const struct pad_params *p = &pd->p;
        unsigned char *x = q + nt * nl, *o2 = x + 1 + wide, *mx = m + nt * 6;
        if (wide) put16(x, (unsigned)si);
        else x[0] = (unsigned char)si;
        o2[5] = p->gate ? 2 : 0;
        int mg = clampi(p->mute_group, 0, 32);
        if (mg) {
            if (!group_note[mg]) group_note[mg] = nt + 35;
            else o2[6] = (unsigned char)group_note[mg]; // cut by the group's first note
        }
        int t = clampi(p->semi * 10 + p->fine / 10, -120, 120);
        put16(o2 + 8, (unsigned)(t & 0xFFFF));
        o2[10] = (unsigned char)sqrt_ms(p->attack_ms);
        o2[11] = p->decay_ms ? (unsigned char)sqrt_ms(p->decay_ms) : 5;
        o2[12] = p->decay_end ? 0 : 1;
        o2[13] = (unsigned char)(p->filter == FILTER_OFF ? 100 : clampi(p->cutoff, 0, 100));
        o2[14] = (unsigned char)(p->filter == FILTER_OFF ? 0 : clampi(p->reso * 15 / 100, 0, 15));
        mx[1] = (unsigned char)clampi(p->volume, 0, 100);
        mx[2] = (unsigned char)clampi(p->pan + 50, 0, 100);
    }
    // The group's first note mutes the others too (each note has two to name).
    for (int pad = 0; pad < 64 && pad < OS_PADS; pad++) {
        const struct os_pad *pd = &g->pads[pad];
        int mg = pd->s ? clampi(pd->p.mute_group, 0, 32) : 0;
        if (!mg || group_note[mg] != XL_PAD_NOTES[pad]) continue;
        int k = 0;
        for (int o3 = 0; o3 < 64 && o3 < OS_PADS && k < 2; o3++)
            if (o3 != pad && g->pads[o3].s && g->pads[o3].p.mute_group == mg) {
                unsigned char *o2 = q + (XL_PAD_NOTES[pad] - 35) * nl + 1 + wide;
                o2[6 + k++] = XL_PAD_NOTES[o3];
            }
    }
    return 17 + 9 + 1 + 5 + XL_NOTES * nl + 1 + 384 + 3 + 64 + 200;
}

static void names_write(unsigned char *b, const struct xl_out *o) {
    for (int i = 0; i < o->n; i++) {
        pad_name(b + i * 17, o->x[i].name, 16);
        b[i * 17 + 16] = 0;
    }
}

static int write_file(const char *path, const unsigned char *b, long n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(b, 1, (size_t)n, f) == (size_t)n;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

int disk_xl_pgm_save(struct os *s, int prog, const char *path, int *dropped) {
    *dropped = 0;
    struct xl_out *o = calloc(1, sizeof *o);
    unsigned char *body = calloc(1, 4096), *b = 0;
    int rc = -1;
    if (!o || !body) goto done;
    long bl = prog_write(body, s, prog, o, 0, dropped);
    long n = 4 + (long)o->n * 17 + 2 + bl;
    if (!(b = calloc(1, (size_t)n))) goto done;
    b[0] = 7, b[1] = 4;
    put16(b + 2, (unsigned)o->n);
    names_write(b + 4, o);
    b[4 + o->n * 17] = 30;
    memcpy(b + 4 + o->n * 17 + 2, body, (size_t)bl);
    char dir[700];
    dir_of(path, dir, sizeof dir);
    rc = write_sounds(o, dir) == 0 ? write_file(path, b, n) : -1;
done:
    free(o), free(body), free(b);
    return rc;
}

int disk_xl_aps_save(struct os *s, const char *path, const char *name, int *programs, int *dropped) {
    *programs = *dropped = 0;
    struct xl_out *o = calloc(1, sizeof *o);
    unsigned char *bodies = calloc(XL_PROGRAMS, 4096), *b = 0;
    long bl[XL_PROGRAMS] = {0};
    int which[XL_PROGRAMS], rc = -1;
    if (!o || !bodies) goto done;
    for (int g = 0; g < OS_PROGRAMS; g++) {
        if (!os_program_sounds(s, g)) continue;
        if (g >= XL_PROGRAMS || *programs >= XL_PROGRAMS) { // the 2000XL holds 24
            (*dropped)++;
            continue;
        }
        which[*programs] = g;
        bl[*programs] = prog_write(bodies + (size_t)*programs * 4096, s, g, o, 1, dropped);
        (*programs)++;
    }
    long n = 4 + (long)o->n * 17 + 2 + 17 + 7 + 1 + 64 + 7 + 4 * 392 + 3 * 4 + 1 + 2;
    for (int i = 0; i < *programs; i++) n += 1 + 5 + bl[i];
    if (!(b = calloc(1, (size_t)n))) goto done;
    unsigned char *p = b;
    p[0] = 0x0A, p[1] = 5;
    put16(p + 2, (unsigned)o->n);
    names_write(p + 4, o);
    p += 4 + o->n * 17;
    p[0] = 24, p += 2;
    pad_name(p, name, 16), p += 17;
    static const unsigned char SET[7] = {1, 48, 0, 3, 0, 0, 0}, AFTER[7] = {4, 0, 136, 1, 64, 0, 6};
    memcpy(p, SET, 7), p += 7;
    *p++ = 64;
    memcpy(p, XL_PAD_NOTES, 64), p += 64;
    memcpy(p, AFTER, 7), p += 7;
    for (int d = 0; d < 4; d++) { // DRUM 1..4 play programs 1..4 (or the first)
        for (int i = 0; i < XL_NOTES; i++) p[i * 6 + 1] = 100, p[i * 6 + 2] = 50, p[i * 6 + 3] = 100;
        p += 384;
        p[2] = 1, p[3] = 1, p[4] = 127, p[5] = (unsigned char)(d < *programs ? which[d] : 0), p[6] = 1, p[7] = 1;
        p += 8;
        if (d < 3) p[0] = 127, p[1] = 64, p[3] = 6, p += 4;
        else *p++ = 127;
    }
    for (int i = 0; i < *programs; i++) {
        *p++ = (unsigned char)which[i];
        static const unsigned char HEAD[5] = {0, 7, 4, 30, 0};
        memcpy(p, HEAD, 5), p += 5;
        memcpy(p, bodies + (size_t)i * 4096, (size_t)bl[i]), p += bl[i];
    }
    *p++ = 0xFF, *p++ = 0xFF;
    char dir[700];
    dir_of(path, dir, sizeof dir);
    rc = *programs == 0 ? -1 : write_sounds(o, dir) == 0 ? write_file(path, b, n) : -1;
done:
    free(o), free(bodies), free(b);
    return rc;
}

// ---- Sequences ----

#define ALL_HEAD 2064
#define ALL_SONG 528
#define SEQ_HEAD 10240

// A track's notes -> pads: the program it plays, from the .APS beside the sequence (its program
// change, else its DRUM's program), else the 2000XL's own pads.
struct xl_maps {
    int have, nprog, drum[4];
    struct xl_prog *prog;
};

static void maps_read(struct xl_maps *m, const char *dir) {
    memset(m, 0, sizeof *m);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && !m->have) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".aps") != 0) continue;
        char p[900];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        long n;
        unsigned char *b = read_all(p, &n);
        m->prog = b ? calloc(XL_PROGRAMS, sizeof *m->prog) : 0;
        int ns;
        if (m->prog && (m->nprog = aps_parse(b, n, m->prog, &ns, m->drum)) > 0) m->have = 1;
        else free(m->prog), m->prog = 0, m->nprog = 0;
        free(b);
    }
    if (d) closedir(d);
}

static void map_track(const struct xl_maps *m, struct imp_track *t, int bus, int pc) {
    const unsigned char *pads = XL_PAD_NOTES;
    if (m->have) {
        int want = pc > 0 ? pc - 1 : bus >= 1 && bus <= 4 ? m->drum[bus - 1] : 0;
        const struct xl_prog *g = 0;
        for (int i = 0; i < m->nprog && !g; i++)
            if (m->prog[i].number == want) g = &m->prog[i];
        if (!g) g = &m->prog[0];
        pads = g->pads;
        snprintf(t->prog, sizeof t->prog, "%s", g->name);
    }
    memset(t->map, 255, sizeof t->map);
    for (int pad = 63; pad >= 0; pad--)
        if (pads[pad] < 128) t->map[pads[pad]] = (unsigned char)pad;
    t->has_map = 1;
}

static void add(struct imp_track *t, uint32_t tick, int dur, int note, int vel) {
    if (t->count == t->cap) {
        int cap = t->cap ? t->cap * 2 : 64;
        struct imp_note *n = realloc(t->notes, (size_t)cap * sizeof *n);
        if (!n) return;
        t->notes = n, t->cap = cap;
    }
    t->notes[t->count++] = (struct imp_note){tick, (uint16_t)clampi(dur, 1, 65535), (uint8_t)note, (uint8_t)vel};
}

static int by_src(const void *a, const void *b) {
    return ((const struct imp_track *)a)->src - ((const struct imp_track *)b)->src;
}

int imp_xl_all_load(struct seq_import *im, const char *path) {
    memset(im, 0, sizeof *im);
    long n;
    unsigned char *b = read_all(path, &n);
    if (!b) return -1;
    if (n < ALL_HEAD + 99 * 18 + 20 * ALL_SONG || memcmp(b, "MPC2KXL ALL 1.00", 16) != 0) return free(b), -1;
    char dir[700];
    dir_of(path, dir, sizeof dir);
    struct xl_maps maps;
    maps_read(&maps, dir);
    im->seq = calloc(SEQ_COUNT, sizeof *im->seq);
    long p = ALL_HEAD + 99 * 18 + 20 * ALL_SONG;
    while (im->seq && p + SEQ_HEAD <= n && im->nseq < SEQ_COUNT) {
        if (b[p] == 0xFF) { // an end marker between sequences
            p += 8;
            continue;
        }
        const unsigned char *h = b + p;
        struct imp_seq *q = &im->seq[im->nseq];
        name_of(q->name, h, 16);
        q->tempo10 = clampi((int)u16(h + 22), 300, 3000);
        q->bars = clampi((int)u16(h + 26), 1, SEQ_MAX_BARS);
        const unsigned char *bar = h + 5379, *tr = h + 384;
        int tpb = bar[0] ? bar[0] : 96, len0 = bar[1] | bar[2] << 8 | bar[3] << 16;
        q->sig_den = clampi(SEQ_PPQ * 4 / tpb, 1, 32), q->sig_num = clampi(len0 / tpb, 1, 32);
        if (q->sig_den != 4 && q->sig_den != 8 && q->sig_den != 16) q->sig_num = 4, q->sig_den = 4;
        int slot[XL_TRACKS];
        for (int t = 0; t < XL_TRACKS; t++) slot[t] = -1;
        long e = p + SEQ_HEAD;
        while (e + 8 <= n) {
            const unsigned char *x = b + e;
            uint32_t v = x[0] | x[1] << 8 | x[2] << 16 | (uint32_t)x[3] << 24, tick = v & 0xFFFFF;
            if (tick == 0xFFFFF) {
                e += 8;
                break;
            }
            int track = (int)(v >> 24 & 0x3F), id = x[4];
            if (id <= 127) {
                int dur = (int)((v >> 20 & 0xF) << 10 | (v >> 30 & 3) << 8 | x[5]), vel = x[6] & 0x7F;
                if (slot[track] < 0 && q->ntracks < IMP_TRACKS) {
                    struct imp_track *it = &q->track[slot[track] = q->ntracks++];
                    name_of(it->name, tr + track * 16, 16);
                    it->src = track;
                    int bus = tr[1088 + track], dev = tr[1024 + track];
                    it->midi = bus == 0;
                    it->channel = it->midi && dev ? (dev - 1) % 16 + 1 : 0;
                    if (!it->midi) map_track(&maps, it, bus, tr[1152 + track]);
                }
                if (slot[track] >= 0 && vel) add(&q->track[slot[track]], tick, dur, id, vel);
                e += 8;
            } else if (id == 0xF0) { // system exclusive: its bytes in 8-byte pieces, and one more
                long l = x[5] | x[6] << 8 | x[7] << 16;
                e += 8 + (l + 7) / 8 * 8 + 8;
            } else {
                e += 8;
            }
        }
        p = e;
        qsort(q->track, (size_t)q->ntracks, sizeof q->track[0], by_src); // in track order
        if (q->ntracks) im->nseq++;
        else memset(q, 0, sizeof *q);
    }
    free(maps.prog), free(b);
    if (!im->nseq) return free(im->seq), im->seq = 0, -1;
    im->kind = IMP_XL, im->pick = 0;
    imp_select_all(im, 1);
    return 0;
}

// A 2000XL .MID read as a MIDI file: its tracks' TRACK DATA says which are drums (DRUM 1..4), and
// the .APS beside it, which pads they play.
void imp_xl_mid_fix(struct seq_import *im, const char *path) {
    if (im->kind != IMP_MIDI || im->nseq != 1) return;
    struct imp_seq *q = &im->seq[0];
    int any = 0;
    for (int i = 0; i < q->ntracks; i++) any |= q->track[i].xl;
    if (!any) return;
    char dir[700];
    dir_of(path, dir, sizeof dir);
    struct xl_maps maps;
    maps_read(&maps, dir);
    for (int i = 0; i < q->ntracks; i++) {
        struct imp_track *t = &q->track[i];
        if (!t->xl) continue;
        t->midi = t->xl_bus == 0;
        if (!t->midi) map_track(&maps, t, t->xl_bus, t->xl_pc);
        else if (t->xl_dev) t->channel = (t->xl_dev - 1) % 16 + 1;
    }
    free(maps.prog);
    im->kind = IMP_XL;
}

// ---- Saving sequences ----

struct xl_ev {
    uint32_t tick;
    unsigned char b[8];
};

static int by_tick(const void *a, const void *b) {
    const struct xl_ev *x = a, *y = b;
    return x->tick < y->tick ? -1 : x->tick > y->tick;
}

static int seq_len(const struct seq_sequence *q) { // in ticks
    int bar = SEQ_PPQ * 4 * q->sig_num / (q->sig_den ? q->sig_den : 4);
    return bar * (q->bars > 0 ? q->bars : 1);
}

// A sequence's tracks as the 2000XL has them: tracks 1..64; a DRUM track on DRUM 1..4 (each
// program in the sequence its own, the fifth on sharing DRUM 4), the program as its program change.
struct xl_trk {
    int bus, pc, dev, used;
};

static void drum_kits(const struct os *s, int kits[4]) { // DRUM 1..4: the first four programs with sounds
    int n = 0;
    for (int g = 0; g < OS_PROGRAMS && g < XL_PROGRAMS && n < 4; g++)
        if (os_program_sounds((struct os *)s, g)) kits[n++] = g;
    while (n < 4) kits[n++] = -1;
}

static int xl_tracks(const struct os *s, const struct seq_sequence *q, struct xl_trk out[XL_TRACKS], int *dropped) {
    int kits[4], last = -1;
    drum_kits(s, kits);
    for (int t = 0; t < SEQ_TRACKS; t++) {
        const struct seq_track *k = &q->track[t];
        if (!k->count) continue;
        if (t >= XL_TRACKS) {
            *dropped += k->count;
            continue;
        }
        last = t;
        struct xl_trk *x = &out[t];
        x->used = 1;
        if (k->type == TRACK_MIDI) {
            x->bus = 0, x->dev = clampi(k->channel, 0, 16), x->pc = clampi(k->program, 0, 128);
            continue;
        }
        int d = 0; // its program's DRUM; another program: DRUM 1, changed to it by the program change
        for (int i = 0; i < 4; i++)
            if (kits[i] == k->kit) d = i;
        x->bus = d + 1, x->pc = k->kit < XL_PROGRAMS ? k->kit + 1 : 0;
    }
    return last;
}

// One sequence's block: its header and its events. Returns its length (0: nothing in it).
static long seq_block(const struct os *s, unsigned char *b, long cap, const struct seq_sequence *q, int number, int *dropped, int *segs) {
    struct xl_trk tk[XL_TRACKS];
    memset(tk, 0, sizeof tk);
    xl_tracks(s, q, tk, dropped);
    int len = seq_len(q), ne = 0;
    for (int t = 0; t < XL_TRACKS; t++) ne += q->track[t].count;
    struct xl_ev *ev = calloc((size_t)(ne ? ne : 1), sizeof *ev);
    if (!ev) return -1;
    int k = 0;
    for (int t = 0; t < XL_TRACKS; t++) {
        const struct seq_track *tr = &q->track[t];
        for (int i = 0; i < tr->count; i++) {
            const struct seq_note *x = &tr->notes[i];
            int note = x->note;
            if (tr->type != TRACK_MIDI) {
                if (note >= 64) {
                    (*dropped)++;
                    continue;
                }
                note = XL_PAD_NOTES[note];
            }
            if ((int)x->tick >= len) continue;
            unsigned dur = (unsigned)clampi(x->dur, 1, 16383);
            uint32_t v = (x->tick & 0xFFFFF) | (dur >> 10 & 0xF) << 20 | (uint32_t)t << 24 | (uint32_t)(dur >> 8 & 3) << 30;
            struct xl_ev *e = &ev[k++];
            e->tick = x->tick;
            e->b[0] = v & 255, e->b[1] = v >> 8 & 255, e->b[2] = v >> 16 & 255, e->b[3] = v >> 24 & 255;
            e->b[4] = (unsigned char)(note & 127), e->b[5] = dur & 255, e->b[6] = (unsigned char)clampi(x->vel, 1, 127), e->b[7] = 64;
        }
    }
    qsort(ev, (size_t)k, sizeof *ev, by_tick);
    *segs = k;
    long total = SEQ_HEAD + (long)k * 8 + (k % 2 == 0 ? 16 : 8);
    if (total > cap) return free(ev), -1;
    memset(b, 0, (size_t)total);
    pad_name(b, q->name, 16);
    put16(b + 16, (unsigned)(1 + k / 2 * 1));
    b[18] = (unsigned char)number;
    b[19] = 1, b[20] = 1;
    put16(b + 22, (unsigned)clampi(q->tempo10, 300, 3000));
    b[24] = 4, b[25] = 4;
    put16(b + 26, (unsigned)clampi(q->bars, 1, SEQ_MAX_BARS));
    b[28] = len & 255, b[29] = len >> 8 & 255, b[30] = (unsigned char)(len >> 16);
    b[64] = b[28], b[65] = b[29], b[66] = b[30];
    uint32_t u1 = 10000000, u2 = (uint32_t)(len * 5208.333333333333);
    for (int i = 0; i < 4; i++) {
        uint32_t u = i < 2 ? u1 : u2;
        b[32 + i * 4] = u & 255, b[33 + i * 4] = u >> 8 & 255, b[34 + i * 4] = u >> 16 & 255, b[35 + i * 4] = (unsigned char)(u >> 24);
    }
    int loop_last = 0xFFFF;
    put16(b + 48, (unsigned)(q->loop_bar > 0 ? q->loop_bar - 1 : 0));
    put16(b + 50, (unsigned)loop_last);
    b[52] = q->loop_bar > 0;
    static const unsigned char P4[5] = {40, 0, 128, 0, 0};
    memcpy(b + 59, P4, 5);
    for (int i = 0; i < 33; i++) {
        char dn[12] = "        ";
        if (i) snprintf(dn, sizeof dn, "Device%02d", i);
        memcpy(b + 120 + i * 8, dn, 8);
    }
    unsigned char *tr = b + 384;
    for (int t = 0; t < XL_TRACKS; t++) {
        const struct seq_track *k2 = &q->track[t];
        char nm[17];
        if (k2->name[0]) snprintf(nm, sizeof nm, "%s", k2->name);
        else snprintf(nm, sizeof nm, "Track-%02d", t + 1);
        pad_name(tr + t * 16, nm, 16);
        tr[1024 + t] = (unsigned char)tk[t].dev;
        tr[1088 + t] = (unsigned char)(tk[t].used ? tk[t].bus : 1);
        tr[1152 + t] = (unsigned char)tk[t].pc;
        tr[1216 + t] = (unsigned char)clampi(k2->vel_pct ? k2->vel_pct : 100, 1, 200);
        tr[1280 + t] = tk[t].used ? (unsigned char)(1 | (k2->on ? 2 : 0) | (tk[t].pc ? 4 : 0)) : 0;
    }
    static const unsigned char TP[16] = {232, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 232, 3};
    memcpy(tr + 1408, TP, 16);
    tr[1424] = (len % 65535) & 255, tr[1425] = (len % 65535) >> 8 & 255, tr[1426] = (unsigned char)(len / 65536);
    for (int i = 0; i < 4; i++) tr[1428 + i] = u1 >> (8 * i) & 255, tr[1432 + i] = u2 >> (8 * i) & 255;
    unsigned char *bar = b + 5379;
    int bl = len / (q->bars > 0 ? q->bars : 1), tpb = bl / (q->sig_num ? q->sig_num : 4), at = 0;
    for (int i = 0; i < q->bars && i < SEQ_MAX_BARS; i++) {
        at += bl;
        bar[i * 4] = (unsigned char)tpb, bar[i * 4 + 1] = at & 255, bar[i * 4 + 2] = at >> 8 & 255, bar[i * 4 + 3] = at >> 16 & 255;
    }
    bar[(q->bars < SEQ_MAX_BARS ? q->bars : SEQ_MAX_BARS - 1) * 4] = (unsigned char)tpb;
    for (int i = 0; i < k; i++) memcpy(b + SEQ_HEAD + i * 8, ev[i].b, 8);
    memset(b + SEQ_HEAD + (long)k * 8, 0xFF, (size_t)(total - SEQ_HEAD - (long)k * 8));
    free(ev);
    return total;
}

static int xl_seq_used(const struct seq_sequence *q) {
    for (int t = 0; t < SEQ_TRACKS; t++)
        if (q->track[t].count) return 1;
    return 0;
}

// The settings before the sequences, as a 2000XL that has just been switched on has them.
static void all_head(unsigned char *b) {
    memset(b, 0, ALL_HEAD);
    memcpy(b, "MPC2KXL ALL 1.00", 16);
    unsigned char *d = b + 16; // the new sequence's defaults
    pad_name(d, "Sequence", 16);
    static const unsigned char U1[6] = {1, 0, 0, 1, 1, 0};
    memcpy(d + 16, U1, 6);
    put16(d + 22, 1200), d[24] = 4, d[25] = 4, put16(d + 26, 2);
    for (int i = 0; i < 4; i++) d[30 + i * 4] = 0x80, d[31 + i * 4] = 0x84, d[32 + i * 4] = 0x1e;
    d[48] = 0xFF, d[49] = 0xFF, d[50] = 1;
    memset(d + 46 + 66, ' ', 8);
    for (int i = 0; i < 33; i++) {
        char dn[12] = "        ";
        if (i) snprintf(dn, sizeof dn, "Device%02d", i);
        memcpy(d + 120 + i * 8 - 0, dn, 8);
    }
    for (int t = 0; t < 64; t++) {
        char nm[17];
        snprintf(nm, sizeof nm, "Track-%02d", t + 1);
        pad_name(d + 384 + t * 16, nm, 16);
        d[384 + 1024 + 64 + t] = 1;   // DRUM 1
        d[384 + 1024 + 192 + t] = 100; // velocity ratio
        d[384 + 1024 + 256 + t] = 6;
    }
    unsigned char *s = b + 16 + 1792; // the sequencer
    put16(s + 4, 1200), s[6] = 1, s[7] = 3;
    unsigned char *u = s + 11;
    u[1] = 12;
    unsigned char *c = u + 10; // count in
    c[0] = 1, c[2] = 100, c[4] = 1, c[5] = 1, c[11] = 127, c[12] = 64;
    unsigned char *mi = c + 13 + 1; // MIDI in
    mi[1] = 1;
    for (int i = 0; i < 34; i++) mi[5 + i] = (unsigned char)i;
    memset(mi + 39, 1, 6);
    memset(mi + 45, 0xFF, 16);
    unsigned char *sg = mi + 61 + 7; // the song settings
    pad_name(sg, "Song", 16);
    unsigned char *misc = sg + 17 + 5 + 36;
    misc[0] = 2;
    unsigned char *st = misc + 10 + 3;
    st[0] = 1, st[2] = 50;
}

int disk_xl_all_save(struct os *s, const char *path, int *sequences, int *dropped) {
    *sequences = *dropped = 0;
    long cap = ALL_HEAD + 99 * 18 + 20 * ALL_SONG;
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    for (int i = 0; i < SEQ_COUNT; i++)
        if (xl_seq_used(&sq->sequence[i])) {
            long nn = 0;
            for (int t = 0; t < SEQ_TRACKS; t++) nn += sq->sequence[i].track[t].count;
            cap += SEQ_HEAD + nn * 8 + 16;
        }
    unsigned char *b = calloc(1, (size_t)cap);
    if (!b) return pthread_mutex_unlock(&sq->lock), -1;
    all_head(b);
    long p = ALL_HEAD + 99 * 18 + 20 * ALL_SONG;
    for (int i = 0; i < SEQ_COUNT; i++) {
        unsigned char *meta = b + ALL_HEAD + i * 18;
        const struct seq_sequence *q = &sq->sequence[i];
        if (!xl_seq_used(q)) {
            pad_name(meta, "(Unused)", 16);
            continue;
        }
        int segs = 0;
        long l = seq_block(s, b + p, cap - p, q, i + 1, dropped, &segs);
        if (l <= 0) continue;
        pad_name(meta, q->name, 16);
        put16(meta + 16, (unsigned)(641 + segs / 2));
        p += l, (*sequences)++;
    }
    for (int g = 0; g < SEQ_SONGS; g++) { // the songs, each sequence and its repeats
        unsigned char *so = b + ALL_HEAD + 99 * 18 + g * ALL_SONG;
        const struct seq_song *x = &sq->song[g];
        char nm[17];
        if (x->nsteps && x->name[0]) snprintf(nm, sizeof nm, "%s", x->name);
        else if (x->nsteps) snprintf(nm, sizeof nm, "Song%02d", g + 1);
        else snprintf(nm, sizeof nm, "(Unused)");
        pad_name(so, nm, 16);
        for (int k = 0; k < 250; k++) {
            int on = k < x->nsteps;
            so[16 + k * 2] = on ? x->step[k].seq : 0xFF, so[17 + k * 2] = on ? (unsigned char)clampi(x->step[k].reps, 1, 99) : 0xFF;
        }
        so[516] = 0xFF, so[517] = 0xFF, so[518] = x->nsteps > 0;
    }
    pthread_mutex_unlock(&sq->lock);
    int rc = *sequences ? write_file(path, b, p) : -1;
    free(b);
    return rc;
}

// ---- One sequence as a 2000XL .MID ----

struct mbuf {
    unsigned char *b;
    long n, cap;
};

static void put(struct mbuf *m, const void *p, long n) {
    if (m->n + n > m->cap) {
        long cap = (m->cap + n) * 2;
        unsigned char *b = realloc(m->b, (size_t)cap);
        if (!b) return;
        m->b = b, m->cap = cap;
    }
    memcpy(m->b + m->n, p, (size_t)n), m->n += n;
}

static void vlq(struct mbuf *m, uint32_t v) {
    unsigned char t[5];
    int n = 0;
    t[n++] = v & 0x7F;
    while (v >>= 7) t[n++] = (unsigned char)((v & 0x7F) | 0x80);
    while (n) put(m, &t[--n], 1);
}

static void meta(struct mbuf *m, int type, const void *p, int n) {
    unsigned char h[2] = {0xFF, (unsigned char)type};
    vlq(m, 0), put(m, h, 2), vlq(m, (uint32_t)n), put(m, p, n);
}

static void chunk(struct mbuf *file, const struct mbuf *t) {
    unsigned char h[8] = {'M', 'T', 'r', 'k', (unsigned char)(t->n >> 24), (unsigned char)(t->n >> 16), (unsigned char)(t->n >> 8),
                          (unsigned char)t->n};
    put(file, h, 8), put(file, t->b, t->n);
}

struct mev {
    uint32_t tick;
    int order;
    unsigned char b[3];
};

static int by_mev(const void *a, const void *b) {
    const struct mev *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    return x->order - y->order;
}

int disk_xl_mid_save(struct os *s, int which, const char *path, int *dropped) {
    *dropped = 0;
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    const struct seq_sequence *q = &sq->sequence[which];
    struct xl_trk tk[XL_TRACKS];
    memset(tk, 0, sizeof tk);
    int last = xl_tracks(s, q, tk, dropped), len = seq_len(q);
    struct mbuf f = {0}, t = {0};
    unsigned char hd[14] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 0, 0, SEQ_PPQ};
    int ntr = 1;
    for (int i = 0; i <= last; i++) ntr += tk[i].used;
    hd[10] = (unsigned char)(ntr >> 8), hd[11] = (unsigned char)ntr;
    put(&f, hd, 14);
    char txt[64];
    snprintf(txt, sizeof txt, "LOOP=%s START=%03d END=END TEMPO=ON  ", q->loop_bar ? "ON " : "OFF", q->loop_bar > 0 ? q->loop_bar - 1 : 0);
    meta(&t, 0x01, txt, (int)strlen(txt));
    unsigned char nm[32];
    memcpy(nm, "MPC2000XL 1.00  ", 16), pad_name(nm + 16, q->name, 16);
    meta(&t, 0x03, nm, 32);
    uint32_t us = (uint32_t)(600000000.0 / clampi(q->tempo10, 300, 3000));
    unsigned char tp[3] = {(unsigned char)(us >> 16), (unsigned char)(us >> 8), (unsigned char)us};
    meta(&t, 0x51, tp, 3);
    unsigned char smpte[5] = {0}, ts[4] = {(unsigned char)q->sig_num, 2, 24, 8};
    for (int d = q->sig_den, p2 = 0; d > 1; d >>= 1) ts[1] = (unsigned char)++p2;
    meta(&t, 0x54, smpte, 5), meta(&t, 0x58, ts, 4);
    unsigned char eot[3] = {0xFF, 0x2F, 0};
    vlq(&t, (uint32_t)len), put(&t, eot, 3);
    chunk(&f, &t), t.n = 0;
    for (int i = 0; i <= last; i++) {
        if (!tk[i].used) continue;
        const struct seq_track *k = &q->track[i];
        meta(&t, 0x04, "        ", 8);
        int flags = (k->on ? 0x80 : 0) | (tk[i].bus ? 0x40 : 0) | (tk[i].dev ? 0x20 | (tk[i].dev - 1) : 0);
        int status = 1 | (k->on ? 2 : 0) | (tk[i].pc ? 4 : 0);
        snprintf(txt, sizeof txt, "TRACK DATA:%02X%02X006403  %02X%02X%02X   ", i, flags, tk[i].dev, tk[i].bus, status);
        meta(&t, 0x01, txt, (int)strlen(txt));
        unsigned char tn[16];
        char nmx[17];
        if (k->name[0]) snprintf(nmx, sizeof nmx, "%s", k->name);
        else snprintf(nmx, sizeof nmx, "Track-%02d", i + 1);
        pad_name(tn, nmx, 16);
        meta(&t, 0x03, tn, 16);
        struct mev *ev = calloc((size_t)(k->count * 2 + 1), sizeof *ev);
        int n = 0;
        for (int j = 0; ev && j < k->count; j++) {
            const struct seq_note *x = &k->notes[j];
            int note = x->note;
            if (k->type != TRACK_MIDI) {
                if (note >= 64) {
                    (*dropped)++;
                    continue;
                }
                note = XL_PAD_NOTES[note];
            }
            if ((int)x->tick >= len) continue;
            ev[n++] = (struct mev){x->tick, 2, {0x90, (unsigned char)(note & 127), (unsigned char)clampi(x->vel, 1, 127)}};
            uint32_t off = x->tick + (x->dur ? x->dur : 1);
            if ((int)off < len) ev[n++] = (struct mev){off, 0, {0x90, (unsigned char)(note & 127), 0}};
        }
        if (ev) qsort(ev, (size_t)n, sizeof *ev, by_mev);
        uint32_t at = 0;
        for (int j = 0; j < n; j++) vlq(&t, ev[j].tick - at), put(&t, ev[j].b, 3), at = ev[j].tick;
        free(ev);
        vlq(&t, (uint32_t)(len > (int)at ? len - (int)at : 0)), put(&t, eot, 3);
        chunk(&f, &t), t.n = 0;
    }
    pthread_mutex_unlock(&sq->lock);
    int rc = f.b && t.b ? write_file(path, f.b, f.n) : -1;
    free(f.b), free(t.b);
    return rc;
}

// ---- A 2000XL project: a folder of ALL_PGMS.APS, ALL_SEQS.ALL and the sounds ----

int disk_xl_project_save(struct os *s, const char *dir, int *programs, int *sequences, int *dropped) {
    *programs = *sequences = *dropped = 0;
    mkdir(dir, 0755);
    char p[900];
    int d1 = 0, d2 = 0, any = 0;
    for (int g = 0; g < OS_PROGRAMS; g++) any |= os_program_sounds(s, g);
    snprintf(p, sizeof p, "%s/ALL_PGMS.APS", dir);
    if (any && disk_xl_aps_save(s, p, "ALL_PGMS", programs, &d1) != 0) return -1;
    snprintf(p, sizeof p, "%s/ALL_SEQS.ALL", dir);
    if (disk_xl_all_save(s, p, sequences, &d2) != 0 && *sequences) return -1;
    *dropped = d1 + d2;
    return *programs || *sequences ? 0 : -1;
}

// Its .APS's programs into the empty programs, then every sequence of its .ALL into the empty
// sequences, each track on its own program (by name, those just loaded first).
int disk_xl_project_load(struct os *s, const char *dir, int *programs, int *sequences) {
    *programs = *sequences = 0;
    char aps[900] = "", all[900] = "";
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5) continue;
        if (!aps[0] && !strcasecmp(e->d_name + l - 4, ".aps")) snprintf(aps, sizeof aps, "%s/%s", dir, e->d_name);
        if (!all[0] && !strcasecmp(e->d_name + l - 4, ".all")) snprintf(all, sizeof all, "%s/%s", dir, e->d_name);
    }
    if (d) closedir(d);
    uint32_t had = 0, fresh = 0;
    for (int g = 0; g < OS_PROGRAMS && g < 32; g++) had |= (uint32_t)(os_program_sounds(s, g) != 0) << g;
    int missing = 0;
    if (aps[0] && disk_xl_aps_load(s, aps, programs) >= 0) missing = s->mpc_missing;
    for (int g = 0; g < OS_PROGRAMS && g < 32; g++)
        if (!(had >> g & 1) && os_program_sounds(s, g)) fresh |= 1u << g;
    struct seq_import im;
    if (all[0] && imp_xl_all_load(&im, all) == 0) {
        im.fresh = fresh;
        int to = 0;
        for (int q = 0; q < im.nseq; q++) {
            while (to < SEQ_COUNT && os_seq_in_use(s, to)) to++;
            if (to >= SEQ_COUNT) break;
            im.pick = q;
            imp_select_all(&im, 1);
            int first, dr;
            if (imp_apply(s, &im, to, &first, &dr) > 0) (*sequences)++;
            to++;
        }
        imp_free(&im);
    }
    s->mpc_missing = missing;
    s->eq_seq = -1;
    return *programs || *sequences ? 0 : -1;
}
