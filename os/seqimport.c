// Sequences from other machines. See seqimport.h.
//
// A Standard MIDI File: format 0 or 1, any resolution, brought to 96 PPQN. Each track with notes
// is a part; a format 0 file's channels are each a part. The first tempo and time signature are
// the sequence's. A part on channel 10, or named like drums, is a drum part.
//
// An MPC 1000 / 2500 / 500 .ALL file ("MPC1000 ALL 1.00"), as its sequences are laid out (a .SEQ,
// "MPC1000 SEQ 1.00", is one of them on its own: the sequence from 0, the tag where its name goes):
//   0x00 the file's size; 0x2c where the sequences start. Each sequence:
//   +0x00 its size  +0x04 its name (16)  +0x1c bars  +0x20 tempo x 10
//   +0x30 the bars: 1000 x 4 bytes, each a bar's start tick (24 bits) and a byte
//   +0x30 + 4000: 65 tracks x 0x30: the name (16); +0x11 the program's name (16); +0x24 1 for a
//                 MIDI track, +0x25 its channel (1-16 port A, 17-32 port B). Track n is entry n.
//   then 16 bytes, then the events, 16 bytes each, in time order: tick (4), track (1), type (1:
//   0x90 a note), note, velocity, length (4), 4 more. 0xffffff7f ends them.
// The MPC runs at 96 PPQN, as the MPC 600 S does. A drum track's notes are its program's pad notes:
// the program file (NAME.PGM, "MPC1000 PGM") beside the .ALL turns them back into pads, from its
// note -> pad table at 0x2958; without one, the MPC 600 S's own Pad Notes do.
#include "seqimport.h"
#include "os.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static unsigned char *read_file(const char *path, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = len > 0 && len < (64L << 20) ? malloc((size_t)len) : 0;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) free(b), b = 0;
    fclose(f);
    *n = b ? len : 0;
    return b;
}

static uint32_t le32(const unsigned char *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void add_note(struct imp_track *t, uint32_t tick, int dur, int note, int vel) {
    if (t->count == t->cap) {
        int cap = t->cap ? t->cap * 2 : 64;
        if (cap > 1 << 20) return;
        struct imp_note *n = realloc(t->notes, (size_t)cap * sizeof *n);
        if (!n) return;
        t->notes = n, t->cap = cap;
    }
    t->notes[t->count++] = (struct imp_note){tick, (uint16_t)clampi(dur, 1, 65535), (uint8_t)note, (uint8_t)clampi(vel, 1, 127)};
}

static int by_src(const void *a, const void *b) {
    const struct imp_track *x = a, *y = b;
    return x->src - y->src;
}

static int by_tick(const void *a, const void *b) {
    const struct imp_note *x = a, *y = b;
    return x->tick < y->tick ? -1 : x->tick > y->tick;
}

static void copy_name(char *dst, const unsigned char *src, int n) { // printable, trailing spaces gone
    int j = 0;
    for (int i = 0; i < n && src[i] && j < 16; i++) dst[j++] = isprint(src[i]) ? (char)src[i] : '_';
    while (j > 0 && dst[j - 1] == ' ') j--;
    dst[j] = 0;
}

static int drum_name(const char *n) {
    static const char *const W[] = {"drum", "kick", "kck", "snare", "hat", "perc", "kit", "cymbal", "clap", "tom", "beat"};
    char low[17];
    int i = 0;
    for (; n[i] && i < 16; i++) low[i] = (char)tolower((unsigned char)n[i]);
    low[i] = 0;
    for (size_t k = 0; k < sizeof W / sizeof *W; k++)
        if (strstr(low, W[k])) return 1;
    return 0;
}

// The sequence's length in whole bars, enough for every note.
static void fit_bars(struct imp_seq *q) {
    int bar = SEQ_PPQ * 4 * q->sig_num / q->sig_den;
    uint32_t end = 0;
    for (int t = 0; t < q->ntracks; t++)
        for (int i = 0; i < q->track[t].count; i++) {
            uint32_t e = q->track[t].notes[i].tick + 1;
            if (e > end) end = e;
        }
    int need = bar > 0 ? (int)((end + (uint32_t)bar - 1) / (uint32_t)bar) : 1;
    if (q->bars < need) q->bars = need;
    q->bars = clampi(q->bars, 1, SEQ_MAX_BARS);
}

void imp_free(struct seq_import *im) {
    for (int q = 0; q < im->nseq; q++)
        for (int t = 0; t < im->seq[q].ntracks; t++) free(im->seq[q].track[t].notes);
    free(im->seq);
    memset(im, 0, sizeof *im);
}

void imp_select_all(struct seq_import *im, int on) {
    memset(im->sel, 0, sizeof im->sel);
    if (im->pick < 0 || im->pick >= im->nseq) return;
    for (int t = 0; t < im->seq[im->pick].ntracks; t++) im->sel[t] = (unsigned char)on;
}

// ---- Standard MIDI Files ----

static uint32_t varlen(const unsigned char *b, long n, long *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && *p < n; i++) {
        unsigned char c = b[(*p)++];
        v = v << 7 | (c & 0x7f);
        if (!(c & 0x80)) break;
    }
    return v;
}

int imp_midi_load(struct seq_import *im, const char *path) {
    memset(im, 0, sizeof *im);
    long n;
    unsigned char *b = read_file(path, &n);
    if (!b) return -1;
    if (n < 14 || memcmp(b, "MThd", 4) != 0) return free(b), -1;
    int format = b[8] << 8 | b[9], ntrk = b[10] << 8 | b[11], div = b[12] << 8 | b[13];
    if (div <= 0 || (div & 0x8000)) return free(b), -1; // SMPTE time: not taken
    struct imp_seq *q = calloc(1, sizeof *q);
    if (!q) return free(b), -1;
    const char *base = strrchr(path, '/'); // the file's name, without .mid
    base = base ? base + 1 : path;
    int bl = (int)strlen(base);
    if (bl > 4 && strcasecmp(base + bl - 4, ".mid") == 0) bl -= 4;
    copy_name(q->name, (const unsigned char *)base, bl < 16 ? bl : 16);
    q->tempo10 = 1200, q->sig_num = 4, q->sig_den = 4, q->bars = 1;
    int got_tempo = 0, got_sig = 0;
    long p = 8 + (long)(b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7]);
    for (int tr = 0; tr < ntrk && p + 8 <= n; tr++) {
        if (memcmp(b + p, "MTrk", 4) != 0) break;
        long len = (long)((uint32_t)b[p + 4] << 24 | (uint32_t)b[p + 5] << 16 | (uint32_t)b[p + 6] << 8 | b[p + 7]);
        long e = p + 8, end = p + 8 + len < n ? p + 8 + len : n;
        p = end;
        char tname[17] = "";
        // Format 1: this track is one part. Format 0: a part for each channel.
        int part_of[16];
        for (int c = 0; c < 16; c++) part_of[c] = -1;
        int base = q->ntracks;
        // Notes held: their start and velocity, by channel and note.
        static uint32_t on_at[16][128];
        static unsigned char on_vel[16][128];
        memset(on_vel, 0, sizeof on_vel);
        uint64_t tick = 0;
        int status = 0;
        while (e < end) {
            tick += varlen(b, end, &e);
            if (e >= end) break;
            int c = b[e];
            if (c & 0x80) {
                e++;
                if (c < 0xf0) status = c;
            } else if (!status) {
                break;
            } else {
                c = status; // running status
            }
            if (c == 0xff) { // meta
                if (e >= end) break;
                int type = b[e++];
                uint32_t l = varlen(b, end, &e);
                if (e + (long)l > end) break;
                if (type == 0x03 && !tname[0]) copy_name(tname, b + e, (int)l);
                if (type == 0x51 && l >= 3 && !got_tempo) {
                    uint32_t us = (uint32_t)b[e] << 16 | b[e + 1] << 8 | b[e + 2];
                    if (us) q->tempo10 = clampi((int)(600000000.0 / us + 0.5), 300, 3000), got_tempo = 1;
                }
                if (type == 0x58 && l >= 2 && !got_sig) {
                    q->sig_num = clampi(b[e], 1, 16), q->sig_den = 1 << clampi(b[e + 1], 0, 4), got_sig = 1;
                    if (q->sig_den != 4 && q->sig_den != 8 && q->sig_den != 16) q->sig_num = 4, q->sig_den = 4;
                }
                e += l;
                continue;
            }
            if (c == 0xf0 || c == 0xf7) { // sysex
                uint32_t l = varlen(b, end, &e);
                e += l;
                continue;
            }
            int kind = c & 0xf0, ch = c & 0x0f, d1 = e < end ? b[e] : 0;
            int two = kind != 0xc0 && kind != 0xd0;
            int d2 = two && e + 1 < end ? b[e + 1] : 0;
            e += two ? 2 : 1;
            uint32_t t96 = (uint32_t)((tick * SEQ_PPQ + (uint64_t)div / 2) / (uint64_t)div);
            if (kind == 0x90 && d2 > 0) {
                if (on_vel[ch][d1 & 127]) continue; // already sounding: the first one stands
                on_at[ch][d1 & 127] = t96, on_vel[ch][d1 & 127] = (unsigned char)d2;
            } else if (kind == 0x80 || (kind == 0x90 && d2 == 0)) {
                int k = d1 & 127;
                if (!on_vel[ch][k]) continue;
                int part = format == 0 ? part_of[ch] : (base < q->ntracks ? base : -1);
                if (part < 0) {
                    if (q->ntracks >= IMP_TRACKS) continue;
                    part = q->ntracks++;
                    struct imp_track *t = &q->track[part];
                    t->channel = ch + 1;
                    if (format == 0) part_of[ch] = part;
                }
                add_note(&q->track[part], on_at[ch][k], (int)(t96 - on_at[ch][k]), k, on_vel[ch][k]);
                on_vel[ch][k] = 0;
            }
        }
        for (int t = base; t < q->ntracks; t++) { // names, and drum or melodic
            struct imp_track *k = &q->track[t];
            if (format == 0 || !tname[0]) snprintf(k->name, sizeof k->name, "%s%sCH %d", tname, tname[0] ? " " : "", k->channel);
            else snprintf(k->name, sizeof k->name, "%s", tname);
            k->midi = !(k->channel == 10 || drum_name(k->name));
            qsort(k->notes, (size_t)k->count, sizeof *k->notes, by_tick);
        }
    }
    free(b);
    if (!q->ntracks) return free(q), -1;
    fit_bars(q);
    im->kind = IMP_MIDI, im->seq = q, im->nseq = 1, im->pick = 0;
    imp_select_all(im, 1);
    return 0;
}

// ---- MPC .ALL ----

// A program's note -> pad table, from NAME.PGM in the folder (any case); 0 if there is none.
static int pgm_map(const char *dir, const char *prog, unsigned char *map) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    char want[40];
    snprintf(want, sizeof want, "%s.PGM", prog);
    int ok = 0;
    while ((e = readdir(d)) && !ok) {
        if (strcasecmp(e->d_name, want) != 0) continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        long n;
        unsigned char *b = read_file(p, &n);
        if (b && n >= 0x2958 + 128 && memcmp(b + 4, "MPC1000 PGM", 11) == 0) {
            for (int i = 0; i < 128; i++) map[i] = b[0x2958 + i] < 64 ? b[0x2958 + i] : 255;
            ok = 1;
        }
        free(b);
    }
    closedir(d);
    return ok;
}

int imp_mpc_load(struct seq_import *im, const char *path) {
    memset(im, 0, sizeof *im);
    long n;
    unsigned char *b = read_file(path, &n);
    if (!b) return -1;
    int single = n >= 0x30 && memcmp(b + 4, "MPC1000 SEQ", 11) == 0; // one sequence, saved on its own
    if (n < 0x30 || (!single && memcmp(b + 4, "MPC1000 ALL", 11) != 0)) return free(b), -1;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else snprintf(dir, sizeof dir, ".");
    const long TRK = 0x30 + 4000, EV = TRK + 65 * 0x30 + 16;
    long p = single ? 0 : le32(b + 0x2c); // a .SEQ is one sequence as an .ALL holds it, from the start
    if (!single) // the first sequence the index points to (sequence 1 can be unused)
        for (int i = 0; i < 99 && 0x18 + i * 24 + 24 <= n; i++) {
            long at = le32(b + 0x18 + i * 24 + 20);
            if (at > 0 && (p <= 0 || at < p)) p = at;
        }
    int cap = 0;
    while ((p > 0 || single) && p + EV <= n) {
        long L = le32(b + p);
        if (L < EV || p + L > n) break;
        const unsigned char *c = b + p;
        struct imp_seq one = {0};
        copy_name(one.name, c + 4, 16);
        if (single) { // its name is the file's: the place holds "MPC1000 SEQ 1.00"
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            int bl = (int)strlen(base);
            if (bl > 4 && base[bl - 4] == '.') bl -= 4;
            copy_name(one.name, (const unsigned char *)base, bl < 16 ? bl : 16);
        }
        one.bars = clampi((int)le32(c + 0x1c), 1, SEQ_MAX_BARS);
        uint32_t t10 = le32(c + 0x20);
        one.tempo10 = clampi(t10 ? (int)t10 : 1200, 300, 3000);
        int bar = (int)((le32(c + 0x34) & 0xffffff) - (le32(c + 0x30) & 0xffffff)); // the first bar's length
        one.sig_num = 4, one.sig_den = 4;
        if (bar > 0 && bar % SEQ_PPQ == 0 && bar / SEQ_PPQ <= 16) one.sig_num = bar / SEQ_PPQ;
        else if (bar > 0 && bar % (SEQ_PPQ / 2) == 0 && bar / (SEQ_PPQ / 2) <= 16) one.sig_num = bar / (SEQ_PPQ / 2), one.sig_den = 8;
        // The tracks the events use, in their order: MPC track k is one part.
        int part[65];
        for (int k = 0; k < 65; k++) part[k] = -1;
        uint32_t last = 0;
        for (long e = EV; e + 16 <= L; e += 16) {
            const unsigned char *r = c + e;
            uint32_t tick = le32(r);
            if (tick == 0x7fffffffu || (r[0] == 0xff && r[1] == 0xff && r[2] == 0xff)) break;
            if (tick < last) break; // out of order: past the events
            last = tick;
            int trk = r[4], type = r[5], note = r[6], vel = r[7];
            if (type != 0x90 || trk < 1 || trk > 64 || note > 127 || vel < 1 || vel > 127) continue;
            if (part[trk] < 0) {
                if (one.ntracks >= IMP_TRACKS) continue;
                part[trk] = one.ntracks++;
                struct imp_track *t = &one.track[part[trk]];
                t->src = trk;
                const unsigned char *te = c + TRK + trk * 0x30;
                copy_name(t->name, te, 16);
                if (!t->name[0]) snprintf(t->name, sizeof t->name, "Track%02d", trk);
                t->midi = te[0x24] == 1;
                t->channel = t->midi ? (te[0x25] - 1) % 16 + 1 : 10;
                copy_name(t->prog, te + 0x11, 16);
                if (!t->midi) t->has_map = t->prog[0] && pgm_map(dir, t->prog, t->map);
            }
            add_note(&one.track[part[trk]], tick, (int)le32(r + 8), note, vel);
        }
        p += L;
        if (!one.ntracks) continue; // an empty sequence: not listed
        qsort(one.track, (size_t)one.ntracks, sizeof *one.track, by_src); // in the MPC's track order
        fit_bars(&one);
        if (im->nseq == cap) {
            cap = cap ? cap * 2 : 8;
            struct imp_seq *m = realloc(im->seq, (size_t)cap * sizeof *m);
            if (!m) {
                for (int t = 0; t < one.ntracks; t++) free(one.track[t].notes);
                break;
            }
            im->seq = m;
        }
        im->seq[im->nseq++] = one;
    }
    free(b);
    if (!im->nseq) return imp_free(im), -1;
    im->kind = IMP_MPC, im->pick = 0;
    imp_select_all(im, 1);
    return 0;
}

// ---- Into a sequence ----

int imp_apply(struct os *s, struct seq_import *im, int to, int *first, int *dropped) {
    *first = -1, *dropped = 0;
    if (im->pick < 0 || im->pick >= im->nseq || to < 0 || to >= SEQ_COUNT) return 0;
    struct imp_seq *src = &im->seq[im->pick];
    struct seq *sq = &s->seq;
    // The OS's own Pad Notes, note -> pad, for drum parts with no program file.
    unsigned char own[128];
    memset(own, 255, sizeof own);
    for (int pad = OS_PADS - 1; pad >= 0; pad--) {
        int nt = seq_pad_note(pad);
        if (nt >= 0 && nt < 128) own[nt] = (unsigned char)pad;
    }
    pthread_mutex_lock(&sq->lock);
    struct seq_sequence *q = &sq->sequence[to];
    int empty = 1;
    for (int t = 0; t < SEQ_TRACKS; t++) empty &= q->track[t].count == 0 && q->track[t].nauto == 0;
    if (empty) { // a new sequence: the file's tempo, time signature, length and name
        q->tempo10 = src->tempo10, q->sig_num = src->sig_num, q->sig_den = src->sig_den, q->bars = src->bars;
        q->loop_bar = 1; // looping, to bar 1, as a new sequence does
        snprintf(q->name, sizeof q->name, "%s", src->name);
    } else if (q->bars < src->bars) {
        q->bars = src->bars; // long enough for it
    }
    int kit = s->program, done = 0, next = 0;
    const struct os_program *g = &s->programs[kit];
    int s600 = g->s600 != PROG_MPC && g->inst >= 0;
    for (int i = 0; i < src->ntracks; i++) {
        struct imp_track *it = &src->track[i];
        if (!im->sel[i] || !it->count) continue;
        while (next < SEQ_TRACKS && (q->track[next].count || q->track[next].nauto)) next++;
        if (next >= SEQ_TRACKS) break;
        struct seq_track *k = &q->track[next];
        struct seq_note *notes = calloc((size_t)it->count, sizeof *notes);
        if (!notes) break;
        int n = 0;
        for (int j = 0; j < it->count; j++) {
            const struct imp_note *x = &it->notes[j];
            struct seq_note y = {.tick = x->tick, .dur = x->dur, .vel = x->vel};
            if (!it->midi) { // a drum part: onto the pad its note is on
                int pad = it->has_map ? it->map[x->note] : own[x->note];
                if (pad == 255) {
                    (*dropped)++;
                    continue;
                }
                y.note = (uint8_t)pad;
            } else if (s600) { // a melodic part on an S600 program: its Instrument, retuned
                y.note = (uint8_t)g->inst, y.tune = (int8_t)clampi(x->note - 60, -64, 63);
            } else {
                y.note = x->note;
            }
            notes[n++] = y;
        }
        if (!n) {
            free(notes);
            continue;
        }
        free(k->notes);
        k->notes = notes, k->count = n, k->cap = it->count;
        k->type = it->midi && !s600 ? TRACK_MIDI : TRACK_DRUM;
        k->kit = kit, k->on = 1;
        for (int pass = 0, found = 0; pass < 2 && !found && it->prog[0] && !it->midi; pass++) // its own program, loaded:
            for (int pg = 0; pg < OS_PROGRAMS && !found; pg++)                             // one just loaded first
                if ((pass || (pg < 32 && (im->fresh >> pg & 1))) && !strcasecmp(s->programs[pg].name, it->prog) &&
                    os_program_sounds(s, pg))
                    k->kit = pg, found = 1;
        if (k->type == TRACK_MIDI) k->channel = it->channel > 0 ? it->channel : 1;
        snprintf(k->name, sizeof k->name, "%s", it->name);
        if (*first < 0) *first = next;
        done++, next++;
    }
    pthread_mutex_unlock(&sq->lock);
    return done;
}
