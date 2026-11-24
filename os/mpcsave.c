// Saving for an MPC 2500 (and the MPC 1000 and 500, which read the same files): a folder holding
// NAME.PRJ (the list of the project's files), ALL_SEQS&SONGS.ALL (the sequences), a .PGM for each
// program and a WAV for each sample, as an MPC 2500 writes them. The layouts are those seqimport.c
// and disk.c read; what the MPC 600 S does not set comes from the templates in mpc_templates.h.
//
// What fits an MPC goes: pads A01-D16 (64 a program), tracks 1-64, one sample a pad. A pad's
// sample is written at 44.1 kHz, 16 bits, with its start and end in the Akai "smpl" chunk.
#include "mpc_templates.h"
#include "os.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MPC_PADS 64
#define MPC_TRACKS 64
enum { TRK = 0x30 + 4000, EV = TRK + 65 * 0x30 + 16 };

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)(v >> 16), p[3] = (unsigned char)(v >> 24);
}

// A name an MPC takes, and a file name too: up to 16 letters, digits, space, - and _.
static void mpc_name(const char *in, char *out) {
    int j = 0;
    for (int i = 0; in[i] && j < 16; i++) {
        unsigned char c = (unsigned char)in[i];
        out[j++] = isalnum(c) || c == ' ' || c == '-' || c == '_' ? (char)c : '_';
    }
    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = 0;
    if (!j) snprintf(out, 17, "NONAME");
}

// A name not yet used: NAME, else NAME with a number, still 16 letters.
static void unique(char names[][17], int n, char *name) {
    char base[17];
    snprintf(base, sizeof base, "%s", name);
    for (int k = 2;; k++) {
        int taken = 0;
        for (int i = 0; i < n && !taken; i++) taken = !strcasecmp(names[i], name);
        if (!taken) return;
        char num[8];
        snprintf(num, sizeof num, "%d", k);
        snprintf(name, 17, "%.*s%s", (int)(16 - strlen(num)), base, num);
    }
}

// A sound as the MPC wants it, laid out as an MPC 2500 writes it: fmt, then smpl, then data, at
// 44.1 kHz and 16 bits. The smpl chunk is Akai's: the unity note stored the MPC's way round, and
// 20 bytes of sampler data holding the start (frames) at 10 and the last frame played at 14.
static int write_wav(const char *path, const struct sample *x, int32_t start, int32_t end) {
    double k = x->rate > 0 && x->rate != 44100 ? 44100.0 / x->rate : 1;
    int ch = x->channels == 2 ? 2 : 1;
    int32_t frames = (int32_t)(x->frames * k);
    if (frames < 1) return -1;
    uint32_t data = (uint32_t)frames * (uint32_t)ch * 2;
    unsigned char h[12 + 24 + 8 + 56 + 8] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16};
    put32(h + 4, (uint32_t)sizeof h - 8 + data);
    unsigned char *fm = h + 20;
    fm[0] = 1, fm[2] = (unsigned char)ch;
    put32(fm + 4, 44100), put32(fm + 8, 44100u * (uint32_t)ch * 2);
    fm[12] = (unsigned char)(ch * 2), fm[14] = 16;
    unsigned char *sm = h + 36;
    memcpy(sm, "smpl", 4);
    put32(sm + 4, 56);
    unsigned char *d = sm + 8;
    d[0] = 0x47, d[3] = 1, d[4] = 0x5e;  // made by Akai
    put32(d + 8, 22675);                 // the sample period, ns: 44.1 kHz
    d[15] = 60;                          // the unity note, as the MPC stores it
    put32(d + 32, 20);                   // 20 bytes of sampler data
    unsigned char *x20 = d + 36;
    x20[0] = 1, x20[2] = 60;
    int32_t s0 = clampi((int32_t)(start * k), 0, frames - 1), s1 = clampi((int32_t)(end * k), s0 + 1, frames);
    put32(x20 + 10, (uint32_t)s0), put32(x20 + 14, (uint32_t)(s1 - 1));
    unsigned char *dh = sm + 8 + 56;
    memcpy(dh, "data", 4);
    put32(dh + 4, data);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(h, 1, sizeof h, f) == sizeof h;
    int16_t buf[1024];
    int nb = 0;
    for (int32_t i = 0; i < frames && ok; i++) {
        double at = i / k;
        int32_t a = (int32_t)at;
        float t = (float)(at - a);
        int32_t b = a + 1 < x->frames ? a + 1 : a;
        for (int c = 0; c < ch; c++) {
            int sc = c < x->channels ? c : 0;
            float v = x->data[a * x->channels + sc] * (1 - t) + x->data[b * x->channels + sc] * t;
            long q = lrintf(v * 32768.0f); // the inverse of reading: 16-bit sounds come back exact
            buf[nb++] = (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
            if (nb == 1024) ok &= fwrite(buf, 2, (size_t)nb, f) == (size_t)nb, nb = 0;
        }
    }
    if (nb) ok &= fwrite(buf, 2, (size_t)nb, f) == (size_t)nb;
    ok &= fclose(f) == 0;
    return ok ? 0 : -1;
}

static int to_level(int ms) { // milliseconds to the MPC's 0..100 (100: three seconds, as loaded)
    return clampi((int)lround(sqrt(ms / 3.0)), 0, 100);
}

// Another pad (of any program, A01-D16) plays this sound with another trim: it is chopped.
static int chopped(const struct os *s, const struct sample *x, int32_t from, int32_t to) {
    for (int g = 0; g < OS_PROGRAMS; g++)
        for (int pad = 0; pad < MPC_PADS && pad < OS_PADS; pad++) {
            const struct os_pad *pd = &s->programs[g].pads[pad];
            if (pd->s != x) continue;
            int32_t t = pd->p.end > 0 && pd->p.end <= x->frames ? pd->p.end : x->frames, f = pd->p.start >= 0 && pd->p.start < t ? pd->p.start : 0;
            if (f != from || t != to) return 1;
        }
    return 0;
}

int disk_mpc_project_save(struct os *s, const char *dir, const char *name, int *programs, int *sequences, int *dropped) {
    *programs = *sequences = *dropped = 0;
    if (mkdir(dir, 0755) != 0) {
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    }
    char path[900], lines[1 + OS_PROGRAMS + OS_PROGRAMS * MPC_PADS][24];
    int nlines = 0;
    snprintf(lines[nlines++], sizeof lines[0], "ALL_SEQS&SONGS.ALL");

    // The programs with sounds, each a .PGM; their samples, each a WAV once.
    static char pnames[OS_PROGRAMS][17], snames[OS_PROGRAMS * MPC_PADS][17];
    int pnum[OS_PROGRAMS], nprog = 0, nsamp = 0;
    const struct sample *sdone[OS_PROGRAMS * MPC_PADS];
    int32_t sfrom[OS_PROGRAMS * MPC_PADS], sto[OS_PROGRAMS * MPC_PADS]; // each sound with its trim: a chop is its own WAV
    for (int g = 0; g < OS_PROGRAMS; g++) {
        pnum[g] = -1;
        if (!os_program_sounds(s, g)) continue;
        const struct os_program *pg = &s->programs[g];
        unsigned char *b = malloc(sizeof MPC_PGM_EMPTY);
        if (!b) return -1;
        memcpy(b, MPC_PGM_EMPTY, sizeof MPC_PGM_EMPTY);
        for (int pad = 0; pad < OS_PADS; pad++) {
            const struct os_pad *pd = &pg->pads[pad];
            if (!pd->s) continue;
            if (pad >= MPC_PADS) { // banks E-H: an MPC has four
                (*dropped)++;
                continue;
            }
            int32_t to = pd->p.end > 0 && pd->p.end <= pd->s->frames ? pd->p.end : pd->s->frames;
            int32_t from = pd->p.start >= 0 && pd->p.start < to ? pd->p.start : 0;
            int si = -1;
            for (int i = 0; i < nsamp && si < 0; i++)
                if (sdone[i] == pd->s && sfrom[i] == from && sto[i] == to) si = i;
            if (si < 0) { // only its part: pads playing zones of one long sound each keep their own
                char nm[17];
                mpc_name(pd->name[0] ? pd->name : "SAMPLE", nm);
                unique(snames, nsamp, nm);
                snprintf(path, sizeof path, "%s/%s.WAV", dir, nm);
                struct sample v = *pd->s;
                int32_t ws = from, we = to; // a sound with one trim: whole, its trim in the WAV (still trimmable there)
                if (chopped(s, pd->s, from, to)) { // pads playing zones of it: only this part
                    v.data += (size_t)from * (size_t)v.channels;
                    v.frames = to - from;
                    ws = 0, we = v.frames;
                }
                if (write_wav(path, &v, ws, we) != 0) {
                    free(b);
                    return -1;
                }
                si = nsamp++;
                sdone[si] = pd->s, sfrom[si] = from, sto[si] = to;
                snprintf(snames[si], 17, "%s", nm);
            }
            const struct pad_params *p = &pd->p;
            unsigned char *o = b + 0x18 + pad * 0xa4;
            memset(o, 0, 16);
            memcpy(o, snames[si], strlen(snames[si]));
            o[0x11] = 100;
            o[0x62] = 1; // as the MPC sets it on a pad with a sample
            int cents = clampi(p->semi * 100 + p->fine, -3600, 3600);
            o[0x14] = (unsigned char)(cents & 0xff), o[0x15] = (unsigned char)((cents >> 8) & 0xff);
            o[0x16] = p->gate ? 1 : 0;
            o[0x63] = (unsigned char)clampi(p->mute_group, 0, 32);
            o[0x66] = (unsigned char)to_level(p->attack_ms);
            o[0x67] = (unsigned char)(p->decay_ms ? to_level(p->decay_ms) : 5);
            o[0x68] = p->decay_end || !p->decay_ms ? 0 : 1;
            int ft = p->filter == FILTER_OFF ? 0 : p->filter == FILTER_BP2 || p->filter == FILTER_BP4 ? 2
                   : p->filter == FILTER_HP2 || p->filter == FILTER_HP4 ? 3 : 1;
            o[0x71] = (unsigned char)ft, o[0x72] = (unsigned char)clampi(ft ? p->cutoff : 100, 0, 100);
            o[0x73] = (unsigned char)clampi(ft ? p->reso : 0, 0, 100);
            o[0x8f] = (unsigned char)clampi(p->volume, 0, 100);
            o[0x90] = (unsigned char)clampi(p->pan + 50, 0, 100);
        }
        mpc_name(pg->name[0] ? pg->name : "PROGRAM", pnames[nprog]);
        unique(pnames, nprog, pnames[nprog]);
        snprintf(path, sizeof path, "%s/%s.PGM", dir, pnames[nprog]);
        FILE *f = fopen(path, "wb");
        int ok = f && fwrite(b, 1, sizeof MPC_PGM_EMPTY, f) == sizeof MPC_PGM_EMPTY;
        if (f && fclose(f) != 0) ok = 0;
        free(b);
        if (!ok) return -1;
        pnum[g] = nprog++;
    }
    *programs = nprog;

    // The sequences: the index, then each sequence as the MPC lays it out.
    const unsigned char *padnote = MPC_PGM_EMPTY + 0x2918; // a pad's note, as the programs above map them
    struct seq *sq = &s->seq;
    size_t cap = sizeof MPC_ALL_HEAD + 1024, n = sizeof MPC_ALL_HEAD;
    unsigned char *all = malloc(cap);
    if (!all) return -1;
    memcpy(all, MPC_ALL_HEAD, sizeof MPC_ALL_HEAD);
    pthread_mutex_lock(&sq->lock);
    for (int qn = 0; qn < SEQ_COUNT; qn++) {
        const struct seq_sequence *q = &sq->sequence[qn];
        if (!seq_used(q)) continue;
        int nev = 0, ntr = 0;
        for (int t = 0; t < SEQ_TRACKS; t++) {
            if (!q->track[t].count) continue;
            if (ntr >= MPC_TRACKS) {
                (*dropped) += q->track[t].count;
                continue;
            }
            ntr++, nev += q->track[t].count;
        }
        size_t L = (size_t)EV + 16u * (size_t)nev + 16;
        if (n + L > cap) {
            cap = (n + L) * 2;
            unsigned char *m = realloc(all, cap);
            if (!m) {
                pthread_mutex_unlock(&sq->lock);
                free(all);
                return -1;
            }
            all = m;
        }
        unsigned char *c = all + n;
        memset(c, 0, L);
        put32(c, (uint32_t)L);
        char nm[17];
        mpc_name(q->name[0] ? q->name : "SEQUENCE", nm);
        memcpy(c + 4, nm, strlen(nm));
        memcpy(c + 0x14, MPC_SEQ_HDR, sizeof MPC_SEQ_HDR);
        c[0x15] = (unsigned char)(qn + 1);
        put32(c + 0x1c, (uint32_t)clampi(q->bars, 1, SEQ_MAX_BARS));
        put32(c + 0x20, (uint32_t)clampi(q->tempo10, 300, 3000));
        memset(c + 0x24, 0, 12);
        int den = q->sig_den == 8 || q->sig_den == 16 ? q->sig_den : 4;
        uint32_t bar = (uint32_t)(SEQ_PPQ * 4 * q->sig_num / den), beat = (uint32_t)(SEQ_PPQ * 4 / den);
        for (uint32_t i = 0; i < 1000; i++) put32(c + 0x30 + 4 * i, (i * bar & 0xffffff) | beat << 24);
        // The tracks: in their order, onto MPC tracks 1-64.
        int to_mpc[SEQ_TRACKS];
        for (int i = 0; i <= MPC_TRACKS; i++) memcpy(c + TRK + i * 0x30, MPC_TRK_UNUSED, 0x30);
        for (int t = 0, k = 0; t < SEQ_TRACKS; t++) {
            to_mpc[t] = -1;
            const struct seq_track *tr = &q->track[t];
            if (!tr->count || k >= MPC_TRACKS) continue;
            to_mpc[t] = ++k;
            unsigned char *te = c + TRK + k * 0x30;
            memcpy(te, MPC_TRK_USED, 0x30);
            char tn[17];
            mpc_name(tr->name[0] ? tr->name : "TRACK", tn);
            memcpy(te, tn, strlen(tn));
            int pg = tr->kit >= 0 && tr->kit < OS_PROGRAMS ? pnum[tr->kit] : -1;
            if (tr->type == TRACK_MIDI) {
                te[0x24] = 1, te[0x25] = (unsigned char)clampi(tr->channel, 1, 16);
            } else if (pg >= 0) {
                memcpy(te + 0x11, pnames[pg], strlen(pnames[pg]));
                te[0x22] = (unsigned char)(pg + 1);
            }
            te[0x2a] = (unsigned char)clampi(tr->vel_pct, 1, 200);
        }
        memcpy(c + TRK + 65 * 0x30, MPC_EV_MARK, 16);
        // The events, every track's together, in time order.
        int t_at[SEQ_TRACKS] = {0}, e = 0;
        for (;;) {
            int best = -1;
            for (int t = 0; t < SEQ_TRACKS; t++)
                if (to_mpc[t] > 0 && t_at[t] < q->track[t].count &&
                    (best < 0 || q->track[t].notes[t_at[t]].tick < q->track[best].notes[t_at[best]].tick))
                    best = t;
            if (best < 0) break;
            const struct seq_track *tr = &q->track[best];
            const struct seq_note *x = &tr->notes[t_at[best]++];
            int note = x->note;
            if (tr->type != TRACK_MIDI) { // a pad: the note the MPC's program has on it
                if (note >= MPC_PADS) {
                    (*dropped)++;
                    continue;
                }
                note = padnote[note];
            }
            unsigned char *r = c + EV + 16 * e++;
            put32(r, x->tick);
            r[4] = (unsigned char)to_mpc[best], r[5] = 0x90, r[6] = (unsigned char)(note & 127), r[7] = (unsigned char)clampi(x->vel, 1, 127);
            put32(r + 8, x->dur ? x->dur : 1);
        }
        memcpy(c + EV + 16 * e, MPC_EV_END, 16);
        L = (size_t)EV + 16u * (size_t)e + 16; // notes left out: shorter
        put32(c, (uint32_t)L);
        // Into the index.
        unsigned char *ix = all + 0x18 + qn * 24;
        memset(ix, 0, 24);
        memcpy(ix, nm, strlen(nm));
        put32(ix + 20, (uint32_t)n);
        n += L;
        (*sequences)++;
    }
    pthread_mutex_unlock(&sq->lock);
    put32(all, (uint32_t)n);
    snprintf(path, sizeof path, "%s/ALL_SEQS&SONGS.ALL", dir);
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(all, 1, n, f) == n;
    if (f && fclose(f) != 0) ok = 0;
    free(all);
    if (!ok) return -1;

    // The project: its files, a line each, as the MPC lists them: the .ALL, the programs, the samples.
    for (int i = 0; i < nprog; i++) snprintf(lines[nlines++], sizeof lines[0], "%s.PGM", pnames[i]);
    for (int i = 0; i < nsamp; i++) snprintf(lines[nlines++], sizeof lines[0], "%s.WAV", snames[i]);
    snprintf(path, sizeof path, "%s/%s.PRJ", dir, name);
    f = fopen(path, "wb");
    if (!f) return -1;
    for (int i = 0; i < nlines; i++) fprintf(f, "%s\r\n", lines[i]);
    return fclose(f) == 0 ? 0 : -1;
}
