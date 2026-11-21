// Save/Load (Shift + Pad 16; Shift + SAMPLE SELECT opens it on SAVE SAMPLE). Everything lives
// in MPC600S on the card:
//   projects/NAME/   project.bin and samples/NNN.wav: everything (programs, sounds, memory,
//                    sequences, songs, effects, master, settings that go with a project)
//   programs/NAME/   program.bin and samples/: one program and its sounds
//   samples/NAME.wav one sound (24-bit, for other gear too)
//   settings/NAME.txt  the MPC's settings and preferences
//   fx/NAME.fxc      an Input Effects chain (INPUT, or PAD while in use)
//   trackfx/NAME.tfx the Track/Mixer chain: the nine send effects
//   s600/NAME.wav    the S600 waveforms (put there at power-on), and sounds saved as S600 WAVE
//   arp/NAME.arp     a USER step pattern of the arpeggiator
//   autoload/        a project loaded at power-on
//   exports/NAME/    a sequence or song as WAVs: each track, and the stereo mix (with Master FX)
//
// Each type opens in its own folder, but SAVE, LOAD and EXPORT browse anywhere on the card
// (.. goes up as far as the card itself), and SAVE and EXPORT's <New Dir> makes a folder.
//
// project.bin and program.bin: a tag, then numbers and strings, each list with its length,
// so a file saved now still loads after more settings are added (what it lacks stays default).
#include "lcd.h"
#include "os.h"
#include "s600_waves.h"
#include "wav.h"
#include <dirent.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "seqimport.h"

#define NAME_MAX_LEN 16
static const char CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char *const MODES[DISK_MODES] = {"SAVE", "LOAD", "EXPORT", "USB DATA"};
static const char *const TYPES[DISK_TYPES] = {"PROJECT", "PROGRAM", "SAMPLE", "SETTINGS", "AUTOLOAD", "FX CHAIN", "ARP PATTERN",
                                              "TRACK FX", "S600 WAVE", "SEQUENCE", "MIDI FILE", "MPC 2500 SEQUENCE", "MPC 2500 PROGRAM", "MPC 2500 PROJECT", "SEQUENCE", "SONG",
                                              "MIXDOWN SEQ", "MIXDOWN SONG"};
static const char *const DIRS[DISK_TYPES] = {"projects", "programs", "samples", "settings", "autoload", "fx", "arp", "trackfx",
                                             "s600", "sequences", "sequences", "sequences", "programs", "projects", "exports", "exports", "exports", "exports"};
static const char *const EXT[DISK_TYPES] = {"", "", ".wav", ".txt", "", ".fxc", ".arp", ".tfx", ".wav", ".sq6", ".mid", "",
                                            ".pgm", ".prj", "", "", ".wav", ".wav"};
// Inside a saved project or program's folder, the file that makes it one (not a folder of yours).
static const char *const MARK[DISK_TYPES] = {"project.bin", "program.bin", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""};

static int is_sound(int type) { // a sound on the selected pad, as a WAV
    return type == DISK_SAMPLE || type == DISK_WAVE;
}

static int is_prog(int type) { // into or from a program
    return type == DISK_PROGRAM || type == DISK_MPCPGM;
}

static int is_seqtype(int type) { // a sequence on its own, a MIDI file or an MPC .ALL: into or from a sequence
    return type == DISK_SEQFILE || type == DISK_MIDI || type == DISK_MPC;
}

static int is_song(int type) {
    return type == DISK_SONG || type == DISK_MIX_SONG;
}

static int is_mixdown(int type) {
    return type == DISK_MIX_SEQ || type == DISK_MIX_SONG;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// ---- Writing and reading the binary files ----

struct bin {
    FILE *f;
    int ok;
};

static void w32(struct bin *b, int32_t v) {
    if (b->ok && fwrite(&v, 4, 1, b->f) != 1) b->ok = 0;
}

static void wstr(struct bin *b, const char *s) {
    int32_t n = (int32_t)strlen(s);
    w32(b, n);
    if (b->ok && n && fwrite(s, 1, (size_t)n, b->f) != (size_t)n) b->ok = 0;
}

static void wints(struct bin *b, const void *p, int n) { // a list of int32s, its length first
    w32(b, n);
    if (b->ok && n && fwrite(p, 4, (size_t)n, b->f) != (size_t)n) b->ok = 0;
}

static int32_t r32(struct bin *b) {
    int32_t v = 0;
    if (b->ok && fread(&v, 4, 1, b->f) != 1) b->ok = 0;
    return v;
}

static void rstr(struct bin *b, char *dst, size_t cap) {
    int32_t n = r32(b);
    if (n < 0 || n > 4096) b->ok = 0;
    if (!b->ok) {
        if (cap) dst[0] = 0;
        return;
    }
    char tmp[4097];
    if (n && fread(tmp, 1, (size_t)n, b->f) != (size_t)n) b->ok = 0;
    tmp[b->ok ? n : 0] = 0;
    snprintf(dst, cap, "%s", tmp);
}

static void rints(struct bin *b, void *dst, int cap) { // what fits; the rest of the list skipped
    int32_t n = r32(b);
    if (n < 0 || n > 1 << 20) b->ok = 0;
    if (!b->ok) return;
    int take = n < cap ? n : cap;
    if (take && fread(dst, 4, (size_t)take, b->f) != (size_t)take) b->ok = 0;
    if (n > take) fseek(b->f, (long)(n - take) * 4, SEEK_CUR);
}

// ---- Paths ----

static int make_dirs(const char *path) { // like mkdir -p
    char p[600];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir(p, 0755);
            *c = '/';
        }
    return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int is_mpc(int type) { // another MPC's files: they are wherever they were copied to
    return type == DISK_MPC || type == DISK_MPCPGM || type == DISK_MPCPRJ;
}

static void type_dir(const struct os *s, int type, char *b, size_t n) {
    if (s->disk_at[0] && type != DISK_AUTOLOAD) snprintf(b, n, "%s", s->disk_at);
    else if (is_mpc(type)) snprintf(b, n, "%s/..", s->data_dir); // the top of the card, where an MPC's folders are copied
    else snprintf(b, n, "%s/%s", s->data_dir, DIRS[type]);
}

// The top of the card: the folder MPC600S is in. As far up as .. goes.
static void card_root(const struct os *s, char *b, size_t n) {
    char up[600];
    snprintf(up, sizeof up, "%s/..", s->data_dir);
    char *real = realpath(up, 0);
    snprintf(b, n, "%s", real ? real : up);
    free(real);
}

// A folder as a clean path, for comparing and showing.
static void clean_path(const char *in, char *b, size_t n) {
    char *real = realpath(in, 0);
    snprintf(b, n, "%s", real ? real : in);
    free(real);
}

static void item_path(const struct os *s, int type, const char *name, char *b, size_t n) {
    if (type == DISK_AUTOLOAD) {
        snprintf(b, n, "%s/autoload", s->data_dir);
        return;
    }
    char dir[600];
    type_dir(s, type, dir, sizeof dir);
    snprintf(b, n, "%s/%s%s", dir, name, EXT[type]);
    if (!EXT[type][0] || access(b, F_OK) == 0) return;
    // Not there as named: the same name in any case (an MPC's .PGM and .ALL, a MIDI file's .MID).
    DIR *d = opendir(dir);
    if (!d) return;
    char want[200];
    snprintf(want, sizeof want, "%s%s", name, EXT[type]);
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcasecmp(e->d_name, want) == 0) {
            snprintf(b, n, "%s/%s", dir, e->d_name);
            break;
        }
    closedir(d);
}

// A USER step pattern on its own: "M6A1", then the line the settings keep it as.
static int arp_save(const struct os *s, int slot, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    char line[80];
    seq_arp_steps_save(&s->seq.arp_user[slot], line, sizeof line);
    fprintf(f, "M6A1\n%s\n", line);
    fflush(f);
    fsync(fileno(f));
    return fclose(f) == 0 ? 0 : -1;
}

static int arp_load(struct os *s, int slot, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char tag[16], line[128];
    int ok = fgets(tag, sizeof tag, f) && strncmp(tag, "M6A1", 4) == 0 && fgets(line, sizeof line, f);
    fclose(f);
    if (!ok) return -1;
    line[strcspn(line, "\r\n")] = 0;
    struct seq_arp_steps p;
    if (seq_arp_steps_load(&p, line) != 0) return -1;
    pthread_mutex_lock(&s->seq.lock);
    s->seq.arp_user[slot] = p;
    pthread_mutex_unlock(&s->seq.lock);
    return os_save_settings(s) == 0 || !s->settings_path ? 0 : -1; // kept with the settings, as one edited
}

static int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[700];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            remove_tree(sub);
        }
        if (d) closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

// ---- Sounds, programs, sequences ----

struct sound_list {
    struct sample **s;
    int n, cap;
};

static int sound_index(struct sound_list *l, struct sample *x) {
    for (int i = 0; i < l->n; i++)
        if (l->s[i] == x) return i;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        struct sample **g = realloc(l->s, (size_t)cap * sizeof *g);
        if (!g) return -1;
        l->s = g, l->cap = cap;
    }
    l->s[l->n] = x;
    return l->n++;
}

static int write_sounds(const struct sound_list *l, const char *dir) {
    char p[700];
    snprintf(p, sizeof p, "%s/samples", dir);
    make_dirs(p);
    for (int i = 0; i < l->n; i++) {
        snprintf(p, sizeof p, "%s/samples/%03d.wav", dir, i);
        if (wav_save(p, l->s[i], 32) != 0) return -1;
    }
    return 0;
}

static struct sample **read_sounds(const char *dir, int n) {
    struct sample **out = calloc((size_t)(n > 0 ? n : 1), sizeof *out);
    char p[700];
    for (int i = 0; out && i < n; i++) {
        snprintf(p, sizeof p, "%s/samples/%03d.wav", dir, i);
        out[i] = wav_load(p); // a missing sound leaves its pads empty
    }
    return out;
}

#define KGS_MAGIC 0x4b475253 // "KGRS": a pad's keygroups follow

static void write_program(struct bin *b, struct sound_list *l, const struct os_program *g) {
    wstr(b, g->name);
    // The Type, and from S600 on the Instrument (+ 2: 1 LAST PAD, 2 A01 ...) above it. An older
    // OS reads any of them as an S600 program.
    w32(b, g->s600 ? g->s600 | (g->inst + 2) << 8 : 0);
    w32(b, OS_PADS);
    for (int i = 0; i < OS_PADS; i++) {
        const struct os_pad *p = &g->pads[i];
        w32(b, p->s ? sound_index(l, p->s) : -1);
        wstr(b, p->name);
        wints(b, &p->p, (int)(sizeof p->p / 4));
        if (p->sp.multi || p->kgs.n > 0) { // the keygroups ride after the S600 settings, marked
            static int32_t all[sizeof(struct s600_params) / 4 + 2 + sizeof(struct s600_kgs) / 4];
            int sp_n = (int)(sizeof p->sp / 4);
            memcpy(all, &p->sp, sizeof p->sp);
            all[sp_n] = KGS_MAGIC, all[sp_n + 1] = sp_n;
            memcpy(all + sp_n + 2, &p->kgs, sizeof p->kgs);
            wints(b, all, (int)(sizeof all / 4));
        } else {
            wints(b, &p->sp, (int)(sizeof p->sp / 4));
        }
    }
}

// A pad's S600 settings, and after them (MULTI) its keygroups: KGS_MAGIC, then how many ints
// the settings were, then the keygroups. Older files have no keygroups; an older OS reads the
// settings and skips the rest.
static void read_s600(struct bin *b, struct s600_params *sp, struct s600_kgs *kgs) {
    static int32_t buf[4096];
    int32_t n = r32(b);
    if (n < 0 || n > 1 << 20) b->ok = 0;
    if (!b->ok) return;
    int take = n < 4096 ? n : 4096;
    if (take && fread(buf, 4, (size_t)take, b->f) != (size_t)take) b->ok = 0;
    if (n > take) fseek(b->f, (long)(n - take) * 4, SEEK_CUR);
    if (!b->ok) return;
    int sp_n = take, at = -1;
    for (int i = 0; i + 1 < take && at < 0; i++)
        if (buf[i] == KGS_MAGIC && buf[i + 1] == i) at = i, sp_n = i;
    int fit = sp_n < (int)(sizeof *sp / 4) ? sp_n : (int)(sizeof *sp / 4);
    memcpy(sp, buf, (size_t)fit * 4);
    memset(kgs, 0, sizeof *kgs);
    if (at >= 0) {
        int k = take - at - 2, cap = (int)(sizeof *kgs / 4);
        memcpy(kgs, buf + at + 2, (size_t)(k < cap ? k : cap) * 4);
        if (kgs->n < 0 || kgs->n > S600_KGS) kgs->n = 0;
    }
}

// Into program `to`, emptied first. Pads whose sound is missing stay empty.
static void read_program(struct os *s, struct bin *b, struct sample **sounds, int nsounds, int to) {
    char name[64];
    rstr(b, name, sizeof name);
    int type = r32(b), npads = r32(b), s600 = type & 0xff, inst = (type >> 8) - 2;
    if (!b->ok) return;
    int keep = s->program;
    os_program_clear(s, to);
    os_set_program(s, to);
    snprintf(s->programs[to].name, sizeof s->programs[to].name, "%s", name);
    for (int i = 0; i < npads && b->ok; i++) {
        int idx = r32(b);
        char pname[40];
        rstr(b, pname, sizeof pname);
        struct pad_params p;
        struct s600_params sp;
        static struct s600_kgs kgs;
        memset(&p, 0, sizeof p);
        s600_defaults(&sp);
        rints(b, &p, (int)(sizeof p / 4));
        read_s600(b, &sp, &kgs);
        if (i >= OS_PADS || idx < 0 || idx >= nsounds || !sounds[idx]) continue;
        os_pad_load(s, i, sounds[idx], pname);
        s->pads[i].p = p;
        s->pads[i].sp = sp;
        s->pads[i].kgs = kgs;
        os_pad_push(s, i);
    }
    s->programs[to].inst = inst >= -1 && inst < OS_PADS ? inst : 0; // none stored: A01
    os_program_set_s600(s, to, s600); // and every MULTI instrument's keygroups, with every pad they name loaded
    os_set_program(s, keep);
}

static void write_track(struct bin *b, const struct seq_track *k) {
    int32_t f[] = {k->type, k->on, k->channel, k->vel_pct, k->level, k->pan, k->mute, k->solo, k->program, k->kit};
    wints(b, f, (int)(sizeof f / 4));
    wints(b, &k->eq, (int)(sizeof k->eq / 4));
    int32_t send[9];
    for (int i = 0; i < 9; i++) send[i] = k->send[i];
    wints(b, send, 9);
    wstr(b, k->name);
    w32(b, k->count);
    for (int i = 0; i < k->count; i++) {
        const struct seq_note *n = &k->notes[i];
        int32_t v[5] = {(int32_t)n->tick, n->dur, n->note, n->vel, n->tune};
        wints(b, v, 5);
    }
    w32(b, k->nauto);
    for (int i = 0; i < k->nauto; i++) {
        const struct seq_auto *a = &k->autos[i];
        int32_t v[4] = {(int32_t)a->tick, a->pad, a->param, a->value};
        wints(b, v, 4);
    }
}

static void read_track(struct bin *b, struct seq_track *k) {
    int32_t f[10] = {TRACK_DRUM, 1, 0, 100, 100, 0, 0, 0, 0, 0};
    rints(b, f, 10);
    k->type = f[0], k->on = f[1], k->channel = f[2], k->vel_pct = f[3], k->level = f[4], k->pan = f[5];
    k->mute = f[6], k->solo = f[7], k->program = f[8], k->kit = clampi(f[9], 0, OS_PROGRAMS - 1);
    rints(b, &k->eq, (int)(sizeof k->eq / 4));
    int32_t send[9] = {0};
    rints(b, send, 9);
    for (int i = 0; i < 9; i++) k->send[i] = (unsigned char)clampi(send[i], 0, 100);
    rstr(b, k->name, sizeof k->name);
    int n = r32(b);
    if (!b->ok || n < 0 || n > 1 << 22) {
        b->ok = 0;
        return;
    }
    k->notes = n ? calloc((size_t)n, sizeof *k->notes) : 0;
    k->cap = k->notes ? n : 0;
    for (int i = 0; i < n && b->ok; i++) {
        int32_t v[5] = {0};
        rints(b, v, 5);
        if (k->notes)
            k->notes[k->count++] = (struct seq_note){.tick = (uint32_t)v[0], .dur = (uint16_t)v[1], .note = (uint8_t)v[2],
                                                     .vel = (uint8_t)v[3], .tune = (int8_t)v[4]};
    }
    n = r32(b);
    if (!b->ok || n < 0 || n > 1 << 22) {
        b->ok = 0;
        return;
    }
    k->autos = n ? calloc((size_t)n, sizeof *k->autos) : 0;
    k->capauto = k->autos ? n : 0;
    for (int i = 0; i < n && b->ok; i++) {
        int32_t v[4] = {0};
        rints(b, v, 4);
        if (k->autos)
            k->autos[k->nauto++] = (struct seq_auto){.tick = (uint32_t)v[0], .pad = (uint8_t)v[1], .param = (uint8_t)v[2],
                                                     .value = (uint8_t)v[3]};
    }
}

// ---- Projects ----

int disk_project_save(struct os *s, const char *dir) {
    if (make_dirs(dir) != 0) return -1;
    char p[700];
    snprintf(p, sizeof p, "%s/project.bin", dir);
    struct bin b = {fopen(p, "wb"), 1};
    if (!b.f) return -1;
    struct sound_list l = {0};
    // The sounds first: every one on a pad or in memory, listed once.
    for (int g = 0; g < OS_PROGRAMS; g++)
        for (int i = 0; i < OS_PADS; i++)
            if (s->programs[g].pads[i].s) sound_index(&l, s->programs[g].pads[i].s);
    for (int m = 0; m < s->nmemory; m++) sound_index(&l, s->memory[m].s);
    fwrite("M6P1", 1, 4, b.f);
    w32(&b, l.n);
    w32(&b, OS_PROGRAMS);
    for (int g = 0; g < OS_PROGRAMS; g++) write_program(&b, &l, &s->programs[g]);
    w32(&b, s->nmemory);
    for (int m = 0; m < s->nmemory; m++) w32(&b, sound_index(&l, s->memory[m].s)), wstr(&b, s->memory[m].name);
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    w32(&b, SEQ_COUNT);
    for (int n = 0; n < SEQ_COUNT; n++) {
        const struct seq_sequence *q = &sq->sequence[n];
        wstr(&b, q->name);
        int32_t f[] = {q->bars, q->sig_num, q->sig_den, q->tempo10, q->loop_bar};
        wints(&b, f, 5);
        w32(&b, SEQ_TRACKS);
        for (int t = 0; t < SEQ_TRACKS; t++) write_track(&b, &q->track[t]);
    }
    w32(&b, SEQ_SONGS);
    for (int g = 0; g < SEQ_SONGS; g++) {
        const struct seq_song *song = &sq->song[g];
        w32(&b, song->nsteps);
        for (int i = 0; i < song->nsteps; i++) w32(&b, song->step[i].seq), w32(&b, song->step[i].reps);
    }
    int32_t state[] = {sq->current, sq->track, sq->tc, sq->swing, sq->shift_later, sq->shift_ticks, sq->metronome,
                       sq->click_rate, sq->click_volume, sq->count_in, sq->count_bars, sq->count_wait, sq->rep_rate,
                       sq->arp, sq->arp_pattern, sq->res, sq->click_audio, sq->click_loop};
    wints(&b, state, (int)(sizeof state / 4));
    for (int g = 0; g < OS_PROGRAMS; g++) { // Pad Mute and Pad Solo, as bytes in ints
        int32_t m[128], so[128];
        for (int i = 0; i < 128; i++) m[i] = sq->pad_mute[g][i], so[i] = sq->pad_solo[g][i];
        wints(&b, m, 128);
        wints(&b, so, 128);
    }
    pthread_mutex_unlock(&sq->lock);
    char line[1600];
    infx_save_line(s, line, sizeof line);
    wstr(&b, line);
    master_save_line(&s->master, line, sizeof line);
    wstr(&b, line);
    inserts_save_line(s, line, sizeof line); // after them: an older OS stops before it
    wstr(&b, line);
    int ok = b.ok;
    if (fclose(b.f) != 0) ok = 0;
    if (ok && write_sounds(&l, dir) != 0) ok = 0;
    free(l.s);
    return ok ? 0 : -1;
}

int disk_project_load(struct os *s, const char *dir) {
    char p[700];
    snprintf(p, sizeof p, "%s/project.bin", dir);
    struct bin b = {fopen(p, "rb"), 1};
    if (!b.f) return -1;
    char tag[4];
    if (fread(tag, 1, 4, b.f) != 4 || memcmp(tag, "M6P1", 4) != 0) {
        fclose(b.f);
        return -1;
    }
    int nsounds = r32(&b);
    if (!b.ok || nsounds < 0 || nsounds > 100000) {
        fclose(b.f);
        return -1;
    }
    struct sample **sounds = read_sounds(dir, nsounds);
    // The old project goes: playing stopped, every pad and sound let go, undo forgotten.
    seq_stop(&s->seq, 1);
    engine_kill_all(s->engine);
    os_undo_clear(s);
    for (int m = 0; m < s->nmemory; m++) os_sample_release(s, s->memory[m].s);
    s->nmemory = 0;
    int nprog = r32(&b);
    for (int g = 0; g < OS_PROGRAMS; g++) os_program_clear(s, g);
    for (int g = 0; g < nprog && b.ok; g++) read_program(s, &b, sounds, nsounds, g < OS_PROGRAMS ? g : OS_PROGRAMS - 1);
    int nmem = r32(&b);
    for (int m = 0; m < nmem && b.ok; m++) {
        int idx = r32(&b);
        char name[40];
        rstr(&b, name, sizeof name);
        if (idx >= 0 && idx < nsounds && sounds[idx]) os_memory_add(s, sounds[idx], name);
    }
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    int nseq = r32(&b);
    for (int n = 0; n < SEQ_COUNT; n++) // the old sequences' notes go
        for (int t = 0; t < SEQ_TRACKS; t++) {
            struct seq_track *k = &sq->sequence[n].track[t];
            free(k->notes), free(k->autos);
            k->notes = 0, k->autos = 0, k->count = k->cap = k->nauto = k->capauto = 0;
        }
    for (int n = 0; n < nseq && b.ok && n < SEQ_COUNT; n++) {
        struct seq_sequence *q = &sq->sequence[n];
        rstr(&b, q->name, sizeof q->name);
        int32_t f[5] = {2, 4, 4, 900, 0};
        rints(&b, f, 5);
        q->bars = clampi(f[0], 1, SEQ_MAX_BARS), q->sig_num = clampi(f[1], 1, 16), q->sig_den = f[2] == 8 || f[2] == 16 ? f[2] : 4;
        q->tempo10 = clampi(f[3], 300, 3000), q->loop_bar = clampi(f[4], 0, q->bars);
        int ntr = r32(&b);
        for (int t = 0; t < ntr && b.ok; t++) {
            struct seq_track spare = {0};
            read_track(&b, t < SEQ_TRACKS ? &q->track[t] : &spare);
            free(spare.notes), free(spare.autos);
        }
    }
    int nsongs = r32(&b);
    for (int g = 0; g < nsongs && b.ok; g++) {
        struct seq_song tmp = {0}, *song = g < SEQ_SONGS ? &sq->song[g] : &tmp;
        int n = r32(&b);
        song->nsteps = 0;
        for (int i = 0; i < n && b.ok; i++) {
            int a = r32(&b), r = r32(&b);
            if (i < SONG_STEPS) song->step[i].seq = (unsigned char)clampi(a, 0, SEQ_COUNT - 1), song->step[i].reps = (unsigned char)clampi(r, 1, 99), song->nsteps = i + 1;
        }
    }
    int32_t st[18] = {0, 0, TC_16, 50, 0, 0, METRO_REC, CLICK_4, 80, 1, 1, 0, REP_16, 0, 0, SEQ_RES_MPC3000, 0, 0};
    rints(&b, st, 18);
    sq->current = clampi(st[0], 0, SEQ_COUNT - 1), sq->track = clampi(st[1], 0, SEQ_TRACKS - 1);
    sq->tc = clampi(st[2], 0, TC_COUNT - 1), sq->swing = clampi(st[3], 50, 75), sq->shift_later = st[4];
    sq->shift_ticks = clampi(st[5], 0, SEQ_PPQ / 2 - 1), sq->metronome = clampi(st[6], 0, 2), sq->click_rate = clampi(st[7], 0, CLICK_RATES - 1);
    sq->click_volume = clampi(st[8], 0, 100), sq->count_in = st[9] != 0, sq->count_bars = clampi(st[10], 1, SEQ_COUNT_BARS_MAX);
    sq->count_wait = st[11] != 0, sq->rep_rate = clampi(st[12], 0, REP_RATES - 1), sq->arp = st[13] != 0;
    sq->arp_pattern = clampi(st[14], 0, seq_arp_patterns() - 1), sq->res = clampi(st[15], 0, SEQ_RES_COUNT - 1);
    sq->click_audio = st[16] != 0, sq->click_loop = st[17] != 0;
    sq->pos = 0, sq->next = -1;
    for (int g = 0; g < OS_PROGRAMS && b.ok; g++) {
        int32_t m[128] = {0}, so[128] = {0};
        rints(&b, m, 128);
        rints(&b, so, 128);
        for (int i = 0; i < 128; i++) sq->pad_mute[g][i] = m[i] != 0, sq->pad_solo[g][i] = so[i] != 0;
    }
    pthread_mutex_unlock(&sq->lock);
    char line[1600];
    rstr(&b, line, sizeof line);
    if (b.ok) infx_load_line(s, line);
    rstr(&b, line, sizeof line);
    if (b.ok) master_load_line(&s->master, line);
    rstr(&b, line, sizeof line);
    inserts_load_line(s, b.ok ? line : ""); // none saved (an older project): none
    fclose(b.f);
    for (int i = 0; i < nsounds; i++) // a sound nothing took (a pad that no longer exists)
        if (sounds[i] && sounds[i]->refs == 0) sample_free(sounds[i]);
    free(sounds);
    s->eq_seq = -1; // every track's EQ and sends to the engine, next frame
    s->selected = 0;
    return 0;
}

// Clear all: everything a project holds goes, as at power-on: playing stopped, every pad, sound,
// program, sequence and song emptied, undo forgotten. Settings stay.
void disk_clear_all(struct os *s) {
    seq_stop(&s->seq, 1);
    engine_kill_all(s->engine);
    os_undo_clear(s);
    for (int m = 0; m < s->nmemory; m++) os_sample_release(s, s->memory[m].s);
    s->nmemory = 0;
    for (int g = 0; g < OS_PROGRAMS; g++) {
        os_program_clear(s, g);
        snprintf(s->programs[g].name, sizeof s->programs[g].name, "Program%02d", g + 1);
    }
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    for (int n = 0; n < SEQ_COUNT; n++) seq_sequence_reset(&sq->sequence[n], n);
    for (int g = 0; g < SEQ_SONGS; g++) sq->song[g].nsteps = 0;
    sq->current = 0, sq->track = 0, sq->pos = 0, sq->next = -1;
    pthread_mutex_unlock(&sq->lock);
    s->eq_seq = -1; // every track's EQ and sends to the engine, next frame
    s->selected = 0;
}

int disk_program_save(struct os *s, int program, const char *dir) {
    if (make_dirs(dir) != 0) return -1;
    char p[700];
    snprintf(p, sizeof p, "%s/program.bin", dir);
    struct bin b = {fopen(p, "wb"), 1};
    if (!b.f) return -1;
    struct sound_list l = {0};
    for (int i = 0; i < OS_PADS; i++)
        if (s->programs[program].pads[i].s) sound_index(&l, s->programs[program].pads[i].s);
    fwrite("M6G1", 1, 4, b.f);
    w32(&b, l.n);
    write_program(&b, &l, &s->programs[program]);
    int ok = b.ok;
    if (fclose(b.f) != 0) ok = 0;
    if (ok && write_sounds(&l, dir) != 0) ok = 0;
    free(l.s);
    return ok ? 0 : -1;
}

int disk_program_load(struct os *s, int program, const char *dir) {
    char p[700];
    snprintf(p, sizeof p, "%s/program.bin", dir);
    struct bin b = {fopen(p, "rb"), 1};
    if (!b.f) return -1;
    char tag[4];
    int n = 0;
    if (fread(tag, 1, 4, b.f) != 4 || memcmp(tag, "M6G1", 4) != 0 || (n = r32(&b)) < 0 || n > 100000) {
        fclose(b.f);
        return -1;
    }
    struct sample **sounds = read_sounds(dir, n);
    os_undo_save(s, "Load Program");
    read_program(s, &b, sounds, n, program);
    fclose(b.f);
    for (int i = 0; i < n; i++)
        if (sounds[i] && sounds[i]->refs == 0) sample_free(sounds[i]);
    free(sounds);
    return b.ok ? 0 : -1;
}

// ---- An MPC 1000 / 2500 / 500 program ----
// "MPC1000 PGM": 64 pads of 0xa4 bytes from 0x18. Each: four layers of 0x18 (the sample's name
// (16), its level, velocity range, tuning in cents (16 bits), play mode: 1 Note On), then the
// pad's: +0x63 mute group, +0x66 attack, +0x67 decay, +0x68 decay mode (0 END), +0x71 filter type
// (0 OFF, 1 LOW, 2 BAND, 3 HIGH, 4 LOW BOOST), +0x72 cutoff, +0x73 resonance, +0x8f mixer level,
// +0x90 pan (50 the middle). The samples are WAVs of those names, in the same folder.

// A sample's WAV, any case: in the program's folder, else a folder inside it, else the one above.
static int find_wav(const char *dir, const char *name, char *out, size_t n) {
    char want[40];
    snprintf(want, sizeof want, "%s.WAV", name);
    char up[700], sub[900];
    snprintf(up, sizeof up, "%s/..", dir);
    const char *tries[2] = {dir, up};
    for (int pass = 0; pass < 3; pass++) {
        const char *d0 = pass == 2 ? tries[1] : tries[0];
        DIR *d = opendir(d0);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            if (pass != 1 && strcasecmp(e->d_name, want) == 0) {
                snprintf(out, n, "%s/%s", d0, e->d_name);
                closedir(d);
                return 1;
            }
            if (pass == 1) { // one folder down
                snprintf(sub, sizeof sub, "%s/%s", d0, e->d_name);
                struct stat st;
                if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                DIR *d2 = opendir(sub);
                struct dirent *e2;
                while (d2 && (e2 = readdir(d2)))
                    if (strcasecmp(e2->d_name, want) == 0) {
                        snprintf(out, n, "%s/%s", sub, e2->d_name);
                        closedir(d2), closedir(d);
                        return 1;
                    }
                if (d2) closedir(d2);
            }
        }
        closedir(d);
    }
    return 0;
}

static uint32_t le32(const unsigned char *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// The trim an MPC 1000 / 2500 / 500 keeps in its WAV: the "smpl" chunk, made by Akai (0x47), its 20
// bytes of sampler data holding the start (at 10) and the last frame played (at 14); a standard
// loop, if it has one. Returns 1 with them in frames of the file (end exclusive; loop -1 none).
static int mpc_wav_trim(const char *path, int32_t *start, int32_t *end, int32_t *loop, int32_t *frames) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[12], c[8];
    int ok = 0, channels = 0, bytes = 0;
    *frames = 0, *loop = -1;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return fclose(f), 0;
    unsigned char sm[36 + 24 + 64];
    int have_smpl = 0;
    while (fread(c, 1, 8, f) == 8) {
        uint32_t l = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        long next = ftell(f) + (long)l + (l & 1);
        if (!memcmp(c, "fmt ", 4) && l >= 16) {
            unsigned char fm[16];
            if (fread(fm, 1, 16, f) == 16) channels = fm[2] | fm[3] << 8, bytes = (fm[14] | fm[15] << 8) / 8;
        } else if (!memcmp(c, "data", 4) && channels > 0 && bytes > 0) {
            *frames = (int32_t)(l / (uint32_t)(channels * bytes));
        } else if (!memcmp(c, "smpl", 4) && l >= 36) {
            size_t want = l < sizeof sm ? l : sizeof sm;
            have_smpl = fread(sm, 1, want, f) == want && want >= 36;
        }
        if (fseek(f, next, SEEK_SET) != 0) break;
    }
    fclose(f);
    if (have_smpl && *frames > 0 && sm[0] == 0x47) { // Akai's
        uint32_t nl = le32(sm + 28), sd = le32(sm + 32);
        const unsigned char *x = sm + 36 + 24 * (nl > 1 ? 1 : nl);
        if (nl >= 1) *loop = (int32_t)le32(sm + 36 + 8);
        if (sd >= 18 && x + 18 <= sm + sizeof sm) {
            *start = (int32_t)le32(x + 10), *end = (int32_t)le32(x + 14) + 1;
            ok = *start >= 0 && *start < *end && *end <= *frames;
        }
    }
    return ok;
}

static int mpc_ms(int v) { // the MPC's 0..100 attack and decay, as milliseconds
    v = clampi(v, 0, 100);
    return v * v * 3; // 100: three seconds
}

int disk_mpc_program_load(struct os *s, int to, const char *path, int *layers) {
    *layers = 0, s->mpc_missing = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char *b = malloc(0x2a04);
    size_t n = b ? fread(b, 1, 0x2a04, f) : 0;
    fclose(f);
    if (n < 0x2918 || memcmp(b + 4, "MPC1000 PGM", 11) != 0) return free(b), -1;
    char dir[700], pname[40];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    snprintf(pname, sizeof pname, "%s", slash ? slash + 1 : path);
    if (slash) *slash = 0;
    char *dot = strrchr(pname, '.');
    if (dot) *dot = 0;
    os_undo_save(s, "Load Program");
    int keep = s->program, loaded = 0;
    os_program_clear(s, to);
    os_set_program(s, to);
    snprintf(s->programs[to].name, sizeof s->programs[to].name, "%.16s", pname);
    for (int pad = 0; pad < 64 && pad < OS_PADS; pad++) {
        const unsigned char *o = b + 0x18 + pad * 0xa4;
        char name[17] = {0};
        memcpy(name, o, 16);
        if (!name[0]) continue;
        if (o[0x18] || o[0x30] || o[0x48]) (*layers)++; // more layers: only the first is loaded
        char file[900] = "";
        struct sample *x = find_wav(dir, name, file, sizeof file) ? wav_load(file) : 0;
        if (!x) {
            s->mpc_missing++;
            continue;
        }
        os_pad_load(s, pad, x, name);
        struct pad_params *p = &s->pads[pad].p;
        int32_t ts, te, tl, tf;
        if (mpc_wav_trim(file, &ts, &te, &tl, &tf) && tf > 0) { // its start and end as on the MPC, in this sound's frames
            double k = (double)x->frames / tf;
            p->start = (int32_t)(ts * k), p->end = (int32_t)(te * k);
            if (p->end > x->frames) p->end = x->frames;
            if (tl >= 0 && tl < tf) p->loop = (int32_t)(tl * k), p->loop_on = 1;
        }
        int16_t cents = (int16_t)(o[0x14] | o[0x15] << 8);
        p->volume = clampi(o[0x11] * o[0x8f] / 100, 0, 100);
        p->pan = clampi(o[0x90] - 50, -50, 50);
        p->semi = clampi(cents / 100, -36, 36), p->fine = clampi(cents % 100, -100, 100);
        p->gate = o[0x16] == 1;
        p->mute_group = clampi(o[0x63], 0, 32);
        p->attack_ms = mpc_ms(o[0x66]);
        p->decay_end = o[0x68] == 0;
        p->decay_ms = o[0x68] == 0 && o[0x67] <= 5 ? 0 : mpc_ms(o[0x67]); // END with the default: to the end
        static const int FT[5] = {FILTER_OFF, FILTER_LP2, FILTER_BP2, FILTER_HP2, FILTER_LP2};
        p->filter = FT[o[0x71] < 5 ? o[0x71] : 0];
        p->cutoff = clampi(o[0x72], 0, 100), p->reso = clampi(o[0x73], 0, 100);
        if (p->filter == FILTER_LP2 && p->cutoff >= 100) p->filter = FILTER_OFF; // fully open: none
        os_pad_push(s, pad);
        loaded++;
    }
    free(b);
    os_set_program(s, keep);
    return loaded;
}

// ---- One sequence: its settings and all its tracks ----

static int seq_in_use(struct os *s, int which) { // any notes or moves in it
    pthread_mutex_lock(&s->seq.lock);
    int used = seq_used(&s->seq.sequence[which]);
    pthread_mutex_unlock(&s->seq.lock);
    return used;
}

int disk_seq_save(struct os *s, int which, const char *path) {
    struct bin b = {fopen(path, "wb"), 1};
    if (!b.f) return -1;
    fwrite("M6S1", 1, 4, b.f);
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    const struct seq_sequence *q = &sq->sequence[clampi(which, 0, SEQ_COUNT - 1)];
    wstr(&b, q->name);
    int32_t f[] = {q->bars, q->sig_num, q->sig_den, q->tempo10, q->loop_bar};
    wints(&b, f, 5);
    w32(&b, SEQ_TRACKS);
    for (int t = 0; t < SEQ_TRACKS; t++) write_track(&b, &q->track[t]);
    pthread_mutex_unlock(&sq->lock);
    int ok = b.ok;
    if (fclose(b.f) != 0) ok = 0;
    return ok ? 0 : -1;
}

int disk_seq_load(struct os *s, int which, const char *path) {
    struct bin b = {fopen(path, "rb"), 1};
    if (!b.f) return -1;
    char magic[4] = {0};
    if (fread(magic, 1, 4, b.f) != 4 || memcmp(magic, "M6S1", 4) != 0) return fclose(b.f), -1;
    struct seq_sequence *tmp = calloc(1, sizeof *tmp); // read whole first; then it goes in
    if (!tmp) return fclose(b.f), -1;
    rstr(&b, tmp->name, sizeof tmp->name);
    int32_t f[5] = {2, 4, 4, 900, 0};
    rints(&b, f, 5);
    int ntr = r32(&b);
    for (int t = 0; t < ntr && b.ok; t++) {
        struct seq_track spare = {0};
        read_track(&b, t < SEQ_TRACKS ? &tmp->track[t] : &spare);
        free(spare.notes), free(spare.autos);
    }
    fclose(b.f);
    if (!b.ok) {
        for (int t = 0; t < SEQ_TRACKS; t++) free(tmp->track[t].notes), free(tmp->track[t].autos);
        free(tmp);
        return -1;
    }
    os_undo_save(s, "Load Sequence");
    struct seq *sq = &s->seq;
    pthread_mutex_lock(&sq->lock);
    struct seq_sequence *q = &sq->sequence[clampi(which, 0, SEQ_COUNT - 1)];
    snprintf(q->name, sizeof q->name, "%s", tmp->name);
    q->bars = clampi(f[0], 1, SEQ_MAX_BARS), q->sig_num = clampi(f[1], 1, 16), q->sig_den = f[2] == 8 || f[2] == 16 ? f[2] : 4;
    q->tempo10 = clampi(f[3], 300, 3000), q->loop_bar = clampi(f[4], 0, q->bars);
    for (int t = 0; t < SEQ_TRACKS; t++) {
        free(q->track[t].notes), free(q->track[t].autos);
        q->track[t] = tmp->track[t];
        if (t >= ntr) q->track[t].on = 1, q->track[t].vel_pct = 100, q->track[t].level = 100; // not in the file: as new
    }
    pthread_mutex_unlock(&sq->lock);
    free(tmp);
    s->eq_seq = -1; // the tracks' EQ and sends to the engine, next frame
    return 0;
}

// ---- An MPC 1000 / 2500 / 500 project ----
// Project01.PRJ lists the project's files, a line each: its .ALL, its programs, its samples.

int disk_mpc_project_load(struct os *s, const char *path, int *programs, int *sequences) {
    *programs = *sequences = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char dir[700], lines[64][128];
    int nl = 0;
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    char line[256];
    while (fgets(line, sizeof line, f) && nl < 64) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
        if (l > 4 && l < sizeof lines[0]) snprintf(lines[nl++], sizeof lines[0], "%s", line);
    }
    fclose(f);
    char p[900];
    uint32_t fresh = 0;
    int missing = 0;
    // The programs first, so the sequences' tracks find theirs by name.
    for (int i = 0; i < nl; i++) {
        size_t l = strlen(lines[i]);
        if (strcasecmp(lines[i] + l - 4, ".pgm") != 0) continue;
        int to = -1;
        for (int g = 0; g < OS_PROGRAMS && to < 0; g++)
            if (!os_program_sounds(s, g)) to = g;
        if (to < 0) break; // no empty program left
        snprintf(p, sizeof p, "%s/%s", dir, lines[i]);
        int layers;
        char keep[17];
        snprintf(keep, sizeof keep, "%s", s->programs[to].name);
        int got = disk_mpc_program_load(s, to, p, &layers);
        missing += s->mpc_missing;
        if (got > 0) (*programs)++, fresh |= to < 32 ? 1u << to : 0;
        else os_program_clear(s, to), snprintf(s->programs[to].name, sizeof s->programs[to].name, "%s", keep); // its samples missing: left as it was
    }
    for (int i = 0; i < nl; i++) {
        size_t l = strlen(lines[i]);
        if (strcasecmp(lines[i] + l - 4, ".all") != 0) continue;
        snprintf(p, sizeof p, "%s/%s", dir, lines[i]);
        struct seq_import im;
        if (imp_mpc_load(&im, p) != 0) continue;
        im.fresh = fresh;
        int to = 0;
        for (int q = 0; q < im.nseq; q++) {
            while (to < SEQ_COUNT && seq_in_use(s, to)) to++;
            if (to >= SEQ_COUNT) break;
            im.pick = q;
            imp_select_all(&im, 1);
            int first, dropped;
            if (imp_apply(s, &im, to, &first, &dropped) > 0) (*sequences)++;
            to++;
        }
        imp_free(&im);
    }
    s->eq_seq = -1;
    s->mpc_missing = missing;
    return *programs || *sequences ? 0 : -1;
}

// ---- Exports: a sequence or song rendered apart from what is playing ----

// An export running in the background, and how far it has got, for the page to show.
struct disk_job {
    pthread_t thread;
    atomic_int done, file, files;
    atomic_long frames; // of the file being rendered
    char label[40];     // "Mix", "T03 Hats"
    struct os *s;
    int song, which, tracks, mix, mixdown, rc;
    char dir[700]; // an export's folder; a mixdown's file
};

// One render of the sequence (or song) into a WAV: one track alone (track >= 0, no Master FX),
// or the whole mix with the Track FX and the Master FX. Ends 2 s after the music.
static int render(struct os *s, int song, int which, int track, int mono, const char *path, struct disk_job *j) {
    struct engine *e = calloc(1, sizeof *e);
    struct seq *q = malloc(sizeof *q);
    struct master *m = calloc(1, sizeof *m);
    float *out = 0;
    long n = 0, cap = 0;
    int rc = -1;
    struct fx_chain c;
    memset(&c, 0, sizeof c);
    struct fx_inserts *ic = calloc(1, sizeof *ic); // the insert effects, fresh, as set now
    if (!e || !q || !m || !ic) goto done;
    engine_init(e);
    memcpy(e->pad, s->engine->pad, sizeof e->pad); // every pad as the engine has it now
    memcpy(e->bus, s->engine->bus, sizeof e->bus); // the track EQs
    memcpy(e->km, s->engine->km, sizeof e->km);       // S600 MULTI instruments: their key maps
    memcpy(e->km_of, s->engine->km_of, sizeof e->km_of);
    if (fx_inserts_init(ic) == 0) {
        for (int i = 0; i < FX_INSERTS; i++) {
            atomic_store(&ic->track[i], atomic_load(&s->ins.track[i])), atomic_store(&ic->pos[i], atomic_load(&s->ins.pos[i]));
            atomic_store(&ic->type[i], atomic_load(&s->ins.type[i]));
            for (int k = 0; k < 3; k++) atomic_store(&ic->p[i][k], atomic_load(&s->ins.p[i][k]));
        }
        atomic_store(&ic->bpm10, atomic_load(&s->ins.bpm10));
        memcpy(e->ins_track, s->engine->ins_track, sizeof e->ins_track);
        memcpy(e->trk_cut, s->engine->trk_cut, sizeof e->trk_cut), memcpy(e->trk_pan, s->engine->trk_pan, sizeof e->trk_pan);
        e->insert_hook = fx_insert_hook, e->insert_ctx = ic;
    }
    for (int i = 0; i < ENGINE_BUSES; i++) memset(e->bus[i].z, 0, sizeof e->bus[i].z);
    pthread_mutex_lock(&s->seq.lock);
    memcpy(q, &s->seq, sizeof *q);
    pthread_mutex_unlock(&s->seq.lock);
    pthread_mutex_init(&q->lock, 0);
    q->mode = SEQ_STOPPED, q->npending = 0, q->nrecq = 0, q->repeat = 0, q->rec_arm = REC_ARM_OFF, q->count_in = 0;
    q->metronome = METRO_OFF, q->sync_send = 0, q->midi_send = 0, q->midi_off = 1, q->next = -1, q->song_on = 0;
    q->stop_flush = 0, q->missed = 0, q->abs = 0, q->pos = 0, q->erase_note = -1;
    for (int i = 0; i < 16; i++) q->rep[i].on = 0;
    for (int n2 = 0; n2 < SEQ_COUNT; n2++) q->sequence[n2].loop_bar = 0; // once through, not round and round
    for (int n2 = 0; n2 < SEQ_COUNT; n2++) // only the track asked for, or all as mixed
        for (int t = 0; t < SEQ_TRACKS; t++)
            if (track >= 0) q->sequence[n2].track[t].mute = t != track, q->sequence[n2].track[t].solo = 0;
    e->hook = seq_audio_block, e->hook_ctx = q;
    if (track < 0) { // the mix: its sends and Track FX, then Master FX
        if (fx_chain_init(&c) != 0) goto done;
        for (int i = 0; i < FX_SLOTS; i++) {
            atomic_store(&c.type[i], atomic_load(&s->fx.type[i]));
            for (int k = 0; k < 3; k++) atomic_store(&c.p[i][k], atomic_load(&s->fx.p[i][k]));
        }
        atomic_store(&c.mode, atomic_load(&s->fx.mode));
        atomic_store(&c.on, atomic_load(&s->fx.on));
        atomic_store(&c.bpm10, atomic_load(&s->fx.bpm10));
        memcpy(e->send, s->engine->send, sizeof e->send);
        atomic_store(&e->sends_on, atomic_load(&s->engine->sends_on));
        atomic_store(&e->fx_pad, atomic_load(&s->engine->fx_pad));
        e->fx_hook = fx_engine_hook, e->fx_ctx = &c;
        master_init(m);
        char line[256];
        master_save_line(&s->master, line, sizeof line);
        master_load_line(m, line);
    }
    if (song >= 0) seq_song_play(q, song, 0);
    else q->current = which, seq_play(q, 1);
    long tail = 0, limit = 44100L * 60 * 20; // twenty minutes at most
    float block[2048];
    while (tail < 44100 * 2 && n < limit) {
        engine_render(e, block, 1024);
        if (track < 0 && m->vfx) master_process(m, block, 1024);
        if (n + 1024 > cap) {
            long nc = cap ? cap * 2 : 44100 * 30;
            float *g = realloc(out, (size_t)nc * 2 * sizeof(float));
            if (!g) goto done;
            out = g, cap = nc;
        }
        memcpy(out + n * 2, block, sizeof block);
        n += 1024;
        if (j) atomic_store_explicit(&j->frames, n, memory_order_relaxed);
        if (q->mode == SEQ_STOPPED) tail += 1024;
    }
    struct sample *w = sample_new((int32_t)n, mono ? 1 : 2, 44100);
    if (!w) goto done;
    for (long i = 0; i < n; i++) {
        if (mono) w->data[i] = 0.5f * (out[i * 2] + out[i * 2 + 1]);
        else w->data[i * 2] = out[i * 2], w->data[i * 2 + 1] = out[i * 2 + 1];
    }
    rc = wav_save(path, w, 24);
    sample_free(w);
done:
    if (c.slot[0])
        for (int i = 0; i < FX_SLOTS; i++) fx_free(c.slot[i]);
    if (ic) fx_inserts_free(ic), free(ic);
    if (m && m->vfx) fx_free(m->vfx);
    free(out), free(m), free(q), free(e);
    return rc;
}

// Tracks with notes in the sequence, or in any of the song's.
static void tracks_used(struct os *s, int song, int which, unsigned char *used) {
    memset(used, 0, SEQ_TRACKS);
    pthread_mutex_lock(&s->seq.lock);
    if (song >= 0) {
        const struct seq_song *g = &s->seq.song[song];
        for (int i = 0; i < g->nsteps; i++)
            for (int t = 0; t < SEQ_TRACKS; t++) used[t] |= s->seq.sequence[g->step[i].seq].track[t].count > 0;
    } else {
        for (int t = 0; t < SEQ_TRACKS; t++) used[t] = s->seq.sequence[which].track[t].count > 0;
    }
    pthread_mutex_unlock(&s->seq.lock);
}

static void safe_name(const char *in, char *out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) out[j++] = strchr("/\\:*?\"<>|", in[i]) ? '_' : in[i];
    out[j] = 0;
}

// tracks: 0 none, 1 mono, 2 stereo. mix: the stereo mix with Master FX. Returns files written.
static int export_files(struct os *s, int song, int which, int tracks, int mix, const char *dir, struct disk_job *j) {
    if (make_dirs(dir) != 0) return -1;
    int written = 0, n = mix != 0, at = 0;
    char p[700], nm[40];
    unsigned char used[SEQ_TRACKS];
    tracks_used(s, song, which, used);
    for (int t = 0; tracks && t < SEQ_TRACKS; t++) n += used[t];
    if (j) atomic_store(&j->files, n);
    if (mix) {
        snprintf(p, sizeof p, "%s/Mix.wav", dir);
        if (j) snprintf(j->label, sizeof j->label, "Mix (with Master FX)"), atomic_store(&j->file, ++at);
        if (render(s, song, which, -1, 0, p, j) == 0) written++;
    }
    for (int t = 0; tracks && t < SEQ_TRACKS; t++) {
        if (!used[t]) continue;
        const struct seq_track *k = &s->seq.sequence[song >= 0 ? s->seq.song[song].step[0].seq : which].track[t];
        safe_name(k->name[0] ? k->name : "Track", nm, sizeof nm);
        snprintf(p, sizeof p, "%s/T%02d %s.wav", dir, t + 1, nm);
        if (j) snprintf(j->label, sizeof j->label, "Track %02d %.14s", t + 1, nm), atomic_store(&j->file, ++at);
        if (render(s, song, which, t, tracks == 1, p, j) == 0) written++;
    }
    return written;
}

int disk_export(struct os *s, int song, int which, int tracks, int mix, const char *dir) {
    return export_files(s, song, which, tracks, mix, dir, 0);
}

static void *export_run(void *arg) {
    struct disk_job *j = arg;
    if (j->mixdown) { // the stereo mix alone, with the Track and Master FX
        atomic_store(&j->files, 1);
        snprintf(j->label, sizeof j->label, "Mixdown (with Master FX)");
        atomic_store(&j->file, 1);
        j->rc = render(j->s, j->song, j->which, -1, 0, j->dir, j) == 0 ? 1 : 0;
    } else {
        j->rc = export_files(j->s, j->song, j->which, j->tracks, j->mix, j->dir, j);
    }
    atomic_store(&j->done, 1);
    return 0;
}

// Tests: whether an export is still running.
int disk_busy(const struct os *s) {
    return s->disk_job != 0;
}

// ---- Power-on ----

// The S600 waveforms (built into the OS) into MPC600S/s600 as WAVs, each only if it is not
// there: deleted ones come back, but edited ones are kept.
void disk_install_waves(struct os *s) {
    if (!s->data_dir) return;
    char dir[600], p[700];
    snprintf(dir, sizeof dir, "%s/s600", s->data_dir);
    make_dirs(dir);
    for (int w = 0; w < S600_WAVES; w++) {
        snprintf(p, sizeof p, "%s/%s.wav", dir, S600_WAVE_NAMES[w]);
        if (access(p, F_OK) == 0) continue;
        struct sample *x = sample_new(S600_WAVE_POINTS, 1, 44100);
        if (!x) return;
        for (int i = 0; i < S600_WAVE_POINTS; i++) x->data[i] = S600_WAVE_DATA[w][i] / 32768.0f;
        wav_save(p, x, 16);
        sample_free(x);
    }
}

void disk_autoload(struct os *s) {
    if (!s->data_dir) return;
    char p[700];
    snprintf(p, sizeof p, "%s/autoload/project.bin", s->data_dir);
    if (access(p, F_OK) != 0) return;
    snprintf(p, sizeof p, "%s/autoload", s->data_dir);
    disk_project_load(s, p);
}

// ---- The page ----

enum { DF_MODE, DF_TYPE, DF_WHICH, DF_CLEAR, DF_TRACKS, DF_MIX, DF_NAME, DF_FILE, DF_FIELDS };

static int clear_ok(int t) { // a load that adds to what is in memory: Clear all can come first
    return t == DISK_PROJECT || is_prog(t) || is_sound(t) || is_seqtype(t) || t == DISK_MPCPRJ;
}

static int field_shown(const struct os *s, int f) {
    int t = s->disk_type, m = s->disk_mode;
    if (m == DISK_USB) return f == DF_MODE; // USB DATA MODE: nothing else to set
    switch (f) {
    case DF_WHICH: return is_prog(t) || is_sound(t) || t == DISK_ARP || m == DISK_EXPORT || is_seqtype(t);
    case DF_CLEAR: return m == DISK_LOAD && clear_ok(t) && !s->disk_renaming;
    case DF_TRACKS: case DF_MIX: return m == DISK_EXPORT && !is_mixdown(t);
    case DF_NAME: return (m == DISK_SAVE && t != DISK_AUTOLOAD) || m == DISK_EXPORT || s->disk_renaming;
    case DF_FILE: return (m != DISK_SAVE || t != DISK_AUTOLOAD) && !s->disk_renaming && !s->disk_newdir && m != DISK_LOAD;
    default: return 1;
    }
}

// What is in the folder: ".." when inside one of your folders, your folders, then the saved
// items of the type (folders holding a project or program, or files by extension), A to Z.
static void list_files(struct os *s) {
    s->disk_nfiles = 0;
    if (!s->data_dir) return;
    if (s->disk_type == DISK_AUTOLOAD) {
        char p[700];
        snprintf(p, sizeof p, "%s/autoload/project.bin", s->data_dir);
        if (access(p, F_OK) == 0) snprintf(s->disk_files[s->disk_nfiles], sizeof s->disk_files[0], "AUTOLOAD"),
                                  s->disk_isdir[s->disk_nfiles++] = 0;
        return;
    }
    char dir[700], here[700], root[700];
    type_dir(s, s->disk_type, dir, sizeof dir);
    clean_path(dir, here, sizeof here);
    card_root(s, root, sizeof root);
    if (strcmp(here, root) != 0) snprintf(s->disk_files[0], sizeof s->disk_files[0], ".."), s->disk_isdir[0] = 2, s->disk_nfiles = 1;
    int top = s->disk_nfiles;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    const char *ext = EXT[s->disk_type], *mark = MARK[s->disk_type];
    while ((e = readdir(d)) && s->disk_nfiles < DISK_FILES_MAX) {
        if (e->d_name[0] == '.') continue;
        char sub[800];
        struct stat st;
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        if (stat(sub, &st) != 0) continue;
        size_t len = strlen(e->d_name), el = strlen(ext);
        int folder = S_ISDIR(st.st_mode), item;
        if (folder) { // a saved project or program, or a folder of yours
            char m[900];
            snprintf(m, sizeof m, "%s/%s", sub, mark);
            item = mark[0] && access(m, F_OK) == 0;
            if (is_mpc(s->disk_type)) { // the MPC 600 S's own saved projects and programs are not an MPC's
                snprintf(m, sizeof m, "%s/project.bin", sub);
                int own = access(m, F_OK) == 0;
                snprintf(m, sizeof m, "%s/program.bin", sub);
                if (own || access(m, F_OK) == 0) continue;
            }
            if (!mark[0] && !el && s->disk_type != DISK_MPC) item = 1; // the exports: each is a folder of WAVs
            el = 0;
        } else {
            if (s->disk_type == DISK_MPC) { // an .ALL or a .SEQ, named in full so the two show apart
                if (len <= 4 || (strcasecmp(e->d_name + len - 4, ".all") != 0 && strcasecmp(e->d_name + len - 4, ".seq") != 0)) continue;
            } else if (!el || len <= el || strcasecmp(e->d_name + len - el, ext) != 0) {
                continue;
            }
            item = 1;
        }
        snprintf(s->disk_files[s->disk_nfiles], sizeof s->disk_files[0], "%.*s", (int)(len - el), e->d_name);
        s->disk_isdir[s->disk_nfiles++] = item ? 0 : 1;
    }
    closedir(d);
    for (int i = top + 1; i < s->disk_nfiles; i++) // folders first, then items; each A to Z
        for (int j = i; j > top; j--) {
            int a = s->disk_isdir[j - 1], b = s->disk_isdir[j];
            if (a > b || (a == b && strcmp(s->disk_files[j - 1], s->disk_files[j]) <= 0)) break;
            char tmp[sizeof s->disk_files[0]];
            memcpy(tmp, s->disk_files[j], sizeof tmp);
            memcpy(s->disk_files[j], s->disk_files[j - 1], sizeof tmp);
            memcpy(s->disk_files[j - 1], tmp, sizeof tmp);
            s->disk_isdir[j - 1] = (unsigned char)b, s->disk_isdir[j] = (unsigned char)a;
        }
    s->disk_file = clampi(s->disk_file, 0, s->disk_nfiles > 0 ? s->disk_nfiles - 1 : 0);
    if (s->disk_file == 0 && top == 1 && s->disk_nfiles > 1) s->disk_file = 1; // the first thing here, not ..
}

// Each folder keeps its place in the list for the session, as Load Files does.
#define DISK_KEEP 32
static struct { char path[700]; int file; } disk_kept[DISK_KEEP];
static int disk_keep_next;

static void disk_keep(const char *here, int file) {
    int slot = -1;
    for (int i = 0; i < DISK_KEEP && slot < 0; i++)
        if (strcmp(disk_kept[i].path, here) == 0) slot = i;
    if (slot < 0) slot = disk_keep_next, disk_keep_next = (disk_keep_next + 1) % DISK_KEEP;
    snprintf(disk_kept[slot].path, sizeof disk_kept[0].path, "%s", here);
    disk_kept[slot].file = file;
}

static int disk_kept_file(const char *here) {
    for (int i = 0; i < DISK_KEEP; i++)
        if (disk_kept[i].path[0] && strcmp(disk_kept[i].path, here) == 0) return disk_kept[i].file;
    return -1;
}

// The bar's place in this folder, kept as it moves (not on ..).
static void disk_keep_bar(const struct os *s) {
    if (s->disk_file >= s->disk_nfiles || s->disk_isdir[s->disk_file] == 2) return;
    char dir[700], here[700];
    type_dir(s, s->disk_type, dir, sizeof dir);
    clean_path(dir, here, sizeof here);
    disk_keep(here, s->disk_file);
}

// Into a folder of the list, or ".." back up.
static void open_folder(struct os *s, int i) {
    if (i < 0 || i >= s->disk_nfiles || !s->disk_isdir[i]) return;
    char dir[700], here[700], root[700];
    type_dir(s, s->disk_type, dir, sizeof dir);
    clean_path(dir, here, sizeof here);
    card_root(s, root, sizeof root);
    char left[sizeof s->disk_files[0]] = "";
    disk_keep_bar(s);
    if (s->disk_isdir[i] == 2) { // up, but not past the card
        if (strcmp(here, root) == 0) return;
        char *slash = strrchr(here, '/');
        if (slash) snprintf(left, sizeof left, "%s", slash + 1);
        if (slash && slash != here) *slash = 0;
        snprintf(s->disk_at, sizeof s->disk_at, "%s", here);
    } else {
        snprintf(s->disk_at, sizeof s->disk_at, "%.400s/%s", here, s->disk_files[i]);
    }
    s->disk_file = 0;
    snprintf(s->disk_last[s->disk_type], sizeof s->disk_last[0], "%s", s->disk_at); // kept for next time
    list_files(s);
    char now[700];
    type_dir(s, s->disk_type, dir, sizeof dir);
    clean_path(dir, now, sizeof now);
    int kept = disk_kept_file(now);
    if (kept >= 0 && kept < s->disk_nfiles) s->disk_file = kept; // where the list was
    for (int j = 0; left[0] && j < s->disk_nfiles; j++) // back up: on the folder just left
        if (s->disk_isdir[j] == 1 && strcmp(s->disk_files[j], left) == 0) s->disk_file = j;
}

// The type's folder as last used (if it is still there), and the item last loaded or saved in it.
static void restore_folder(struct os *s) {
    const char *at = s->disk_last[s->disk_type];
    struct stat st;
    if (at[0] && stat(at, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(s->disk_at, sizeof s->disk_at, "%s", at);
    else s->disk_at[0] = 0;
    s->disk_file = 0;
    list_files(s);
    for (int i = 0; i < s->disk_nfiles; i++)
        if (!s->disk_isdir[i] && !strcmp(s->disk_files[i], s->disk_last_item[s->disk_type])) s->disk_file = i;
}

static void default_name(struct os *s) {
    const char *base = s->disk_mode == DISK_EXPORT ? (is_mixdown(s->disk_type) ? "MIX" : is_song(s->disk_type) ? "SONG" : "SEQ")
                                                   : TYPES[s->disk_type];
    if (is_sound(s->disk_type) && s->pads[s->selected].s) base = s->pads[s->selected].name;
    if (s->disk_type == DISK_PROGRAM) base = s->programs[s->disk_which].name;
    if (s->disk_type == DISK_ARP) base = "PATTERN";
    if (s->disk_type == DISK_SEQFILE) base = s->seq.sequence[s->disk_which].name;
    if (s->disk_type == DISK_MPCPRJ) base = "PROJECT";
    char clean[NAME_MAX_LEN + 1];
    int j = 0;
    for (int i = 0; base[i] && j < NAME_MAX_LEN; i++) clean[j++] = strchr(CHARS, base[i]) && base[i] != ' ' ? base[i] : '_';
    clean[j] = 0;
    if (s->disk_type == DISK_FX || (s->disk_mode == DISK_EXPORT) || s->disk_type == DISK_PROJECT || s->disk_type == DISK_SETTINGS ||
        s->disk_type == DISK_ARP)
        snprintf(s->disk_name, sizeof s->disk_name, "%.*s%02d", NAME_MAX_LEN - 2, clean[0] == 'F' && clean[1] == 'X' ? "CHAIN" : clean,
                 (s->disk_mode == DISK_EXPORT ? s->disk_which : 0) + 1);
    else
        snprintf(s->disk_name, sizeof s->disk_name, "%s", clean);
    s->disk_pos = 0;
}

void disk_open(struct os *s, int mode, int type) {
    if (s->screen != OS_DISK) s->disk_return = s->screen;
    s->screen = OS_DISK;
    s->disk_mode = mode, s->disk_type = type;
    s->disk_cursor = s->disk_mode == DISK_SAVE ? DF_NAME : DF_MODE;
    s->disk_ask = 0, s->disk_renaming = 0, s->disk_pending = 0, s->disk_newdir = 0;
    s->disk_used = 1, s->disk_page = 0; // Clear all stays as it was set, for the next load too
    s->disk_which = is_prog(type) ? s->program : is_seqtype(type) ? s->seq.current : 0;
    default_name(s);
    restore_folder(s); // where this type was last used; its own folder to start with
}

static void trimmed(const char *in, char *out, size_t n) { // the name without trailing spaces
    snprintf(out, n, "%s", in);
    for (size_t l = strlen(out); l > 0 && out[l - 1] == ' '; l--) out[l - 1] = 0;
}

// The action itself, a frame after its key, so "Working..." shows first.
static void perform(struct os *s, struct ui *u, int action) {
    char name[64], path[700], msg[48];
    trimmed(s->disk_name, name, sizeof name);
    int rc = -1, t = s->disk_type;
    if (action == 'S') { // save
        if (t != DISK_AUTOLOAD) {
            char dir[700];
            type_dir(s, t, dir, sizeof dir);
            make_dirs(dir);
        }
        item_path(s, t, name, path, sizeof path);
        if (!is_sound(t) && t != DISK_SETTINGS && t != DISK_FX) remove_tree(path); // replaced whole
        switch (t) {
        case DISK_PROJECT: case DISK_AUTOLOAD: rc = disk_project_save(s, path); break;
        case DISK_PROGRAM: rc = disk_program_save(s, s->disk_which, path); break;
        case DISK_SAMPLE: case DISK_WAVE: rc = s->pads[s->selected].s ? wav_save(path, s->pads[s->selected].s, 24) : -1; break;
        case DISK_SETTINGS: {
            const char *keep = s->settings_path;
            s->settings_path = path;
            rc = os_save_settings(s);
            s->settings_path = keep;
            break;
        }
        case DISK_FX: rc = infx_chain_save(s, path, 0); break;
        case DISK_TRACKFX: rc = infx_chain_save(s, path, 1); break;
        case DISK_ARP: rc = arp_save(s, clampi(s->disk_which, 0, ARP_USER - 1), path); break;
        case DISK_SEQFILE: rc = disk_seq_save(s, s->disk_which, path); break;
        case DISK_MPCPRJ: { // a folder of MPC files, named as the project
            char dir[700], folder[800];
            type_dir(s, t, dir, sizeof dir);
            snprintf(folder, sizeof folder, "%s/%s", dir, name);
            int pg, sq, dropped;
            rc = disk_mpc_project_save(s, folder, name, &pg, &sq, &dropped);
            if (rc == 0) {
                if (dropped) snprintf(msg, sizeof msg, "Saved; %d left out (no room)", dropped);
                else snprintf(msg, sizeof msg, "Saved %d pgm, %d seq", pg, sq);
                os_toast(s, u, msg);
                list_files(s);
                return;
            }
            break;
        }
        }
        snprintf(msg, sizeof msg, rc == 0 ? "Saved: %.16s" : "Could not save %.10s", t == DISK_AUTOLOAD ? "AUTOLOAD" : name);
    } else if (action == 'L') { // load
        const char *file = s->disk_files[s->disk_file];
        snprintf(s->disk_last_item[t], sizeof s->disk_last_item[0], "%s", file);
        item_path(s, t, file, path, sizeof path);
        switch (t) {
        case DISK_PROJECT: case DISK_AUTOLOAD: rc = disk_project_load(s, path); break;
        case DISK_PROGRAM: rc = disk_program_load(s, s->disk_which, path); break;
        case DISK_SAMPLE: case DISK_WAVE: { // onto the selected pad (an S600 waveform: turn its loop on)
            struct sample *x = wav_load(path);
            if (x) {
                os_undo_save(s, "Load Sample");
                os_pad_assign(s, s->selected, x, file);
                if (s->disk_type == DISK_SAMPLE) os_auto_root(s, s->selected);
                rc = 0;
            }
            break;
        }
        case DISK_SETTINGS: {
            const char *keep = s->settings_path;
            s->settings_path = path;
            os_load_settings(s);
            s->settings_path = keep;
            rc = os_save_settings(s) == 0 || !keep ? 0 : -1; // and kept as the settings now
            break;
        }
        case DISK_FX: rc = infx_chain_load(s, path, 0); break;
        case DISK_TRACKFX: rc = infx_chain_load(s, path, 1); break;
        case DISK_ARP: rc = arp_load(s, clampi(s->disk_which, 0, ARP_USER - 1), path); break;
        case DISK_SEQFILE: rc = disk_seq_load(s, s->disk_which, path); break;
        case DISK_MPCPRJ: {
            int pg = 0, sq = 0;
            if (disk_mpc_project_load(s, path, &pg, &sq) != 0) snprintf(msg, sizeof msg, "Nothing to load in %.10s", file);
            else if (s->mpc_missing) snprintf(msg, sizeof msg, "%d pgm %d seq; %d WAVs missing", pg, sq, s->mpc_missing);
            else snprintf(msg, sizeof msg, "%d programs, %d sequences", pg, sq);
            os_toast(s, u, msg);
            list_files(s);
            return;
        }
        case DISK_MPCPGM: {
            int layers = 0, pads = disk_mpc_program_load(s, s->disk_which, path, &layers);
            if (pads < 0) snprintf(msg, sizeof msg, "Not an MPC program");
            else if (s->mpc_missing) snprintf(msg, sizeof msg, "%d pads; %d WAVs not found", pads, s->mpc_missing);
            else if (layers) snprintf(msg, sizeof msg, "%d pads; %d layered, 1st only", pads, layers);
            else snprintf(msg, sizeof msg, "Loaded %d pads", pads);
            os_toast(s, u, msg);
            list_files(s);
            return;
        }
        case DISK_MIDI: case DISK_MPC: { // read; then its tracks are chosen
            struct seq_import *im = calloc(1, sizeof *im);
            if (im && (t == DISK_MIDI ? imp_midi_load(im, path) : imp_mpc_load(im, path)) == 0) {
                snprintf(im->file, sizeof im->file, "%.39s", file);
                s->imp = im;
                s->disk_ask = t == DISK_MPC && im->nseq > 1 ? 'Q' : 'T';
                list_files(s);
                return;
            }
            free(im);
            snprintf(msg, sizeof msg, t == DISK_MIDI ? "No notes in %.12s" : "No sequences in %.10s", file);
            os_toast(s, u, msg);
            list_files(s);
            return;
        }
        }
        snprintf(msg, sizeof msg, rc == 0 ? "Loaded: %.16s" : "Could not load %.10s", file);
    } else if (action == 'E') { // export
        char dir[700], here[600];
        type_dir(s, t, here, sizeof here);
        make_dirs(here);
        // A mixdown is one WAV here; an export, a folder here of the tracks and the mix.
        if (is_mixdown(t)) snprintf(dir, sizeof dir, "%s/%s.wav", here, name);
        else snprintf(dir, sizeof dir, "%s/%s", here, name);
        // In the background: the page shows how far it has got until it is done.
        struct disk_job *j = calloc(1, sizeof *j);
        if (j) {
            j->s = s, j->song = is_song(t) ? s->disk_which : -1, j->which = s->disk_which;
            j->tracks = s->disk_tracks, j->mix = s->disk_mix, j->mixdown = is_mixdown(t);
            snprintf(j->dir, sizeof j->dir, "%s", dir);
            snprintf(j->label, sizeof j->label, "Starting");
            if (pthread_create(&j->thread, 0, export_run, j) == 0) {
                s->disk_job = j;
                return;
            }
            free(j);
        }
        snprintf(msg, sizeof msg, "Could not export");
    } else if (action == 'N') { // a new folder, here, and into it
        char dir[700];
        type_dir(s, t, dir, sizeof dir);
        snprintf(path, sizeof path, "%s/%s", dir, name);
        rc = !name[0] || access(path, F_OK) == 0 ? -1 : make_dirs(path);
        snprintf(msg, sizeof msg, rc == 0 ? "Folder: %.16s" : "Name taken or no card", name);
        if (rc == 0) snprintf(s->disk_at, sizeof s->disk_at, "%s", path), s->disk_file = 0;
        snprintf(s->disk_name, sizeof s->disk_name, "%s", s->disk_keep); // the item's name back
    } else if (action == 'D') { // delete, once confirmed
        item_path(s, t, s->disk_files[s->disk_file], path, sizeof path);
        rc = remove_tree(path);
        snprintf(msg, sizeof msg, rc == 0 ? "Deleted" : "Could not delete");
    } else if (action == 'R') { // rename
        char to[700];
        item_path(s, t, s->disk_files[s->disk_file], path, sizeof path);
        item_path(s, t, name, to, sizeof to);
        rc = access(to, F_OK) == 0 ? -1 : rename(path, to);
        snprintf(msg, sizeof msg, rc == 0 ? "Renamed: %.16s" : "Name taken or no card", name);
    }
    os_toast(s, u, msg);
    list_files(s);
}

// The export's progress, over the whole page, until it is done.
static int export_progress(struct os *s, struct ui *u) {
    struct disk_job *j = s->disk_job;
    if (!j) return 0;
    if (atomic_load(&j->done)) {
        pthread_join(j->thread, 0);
        char msg[48];
        snprintf(msg, sizeof msg, j->rc > 0 ? "Exported %d files" : "Nothing to export", j->rc);
        free(j);
        s->disk_job = 0;
        os_toast(s, u, msg);
        list_files(s);
        ui_wake(u, 1);
        return 0;
    }
    char line[48];
    int file = atomic_load(&j->file), files = atomic_load(&j->files);
    long secs = atomic_load(&j->frames) / 44100;
    lcd_rule(u, TITLE_ROW, "Exporting");
    snprintf(line, sizeof line, "File %d of %d", file, files);
    lcd_center(u, 4, line);
    lcd_center(u, 6, j->label);
    snprintf(line, sizeof line, "%ld:%02ld rendered", secs / 60, secs % 60);
    lcd_center(u, 8, line);
    int w = files > 0 ? (UI_W - 40) * (file > 0 ? file - 1 : 0) / files : 0; // a bar for the files done
    lcd_fill(u, 20, 10 * CELL_H + 2, UI_W - 40, 1, LCD_FG), lcd_fill(u, 20, 10 * CELL_H + 13, UI_W - 40, 1, LCD_FG);
    lcd_fill(u, 20, 10 * CELL_H + 2, 1, 12, LCD_FG), lcd_fill(u, UI_W - 21, 10 * CELL_H + 2, 1, 12, LCD_FG);
    if (w > 0) lcd_fill(u, 22, 10 * CELL_H + 4, w, 8, LCD_FG);
    lcd_center(u, 12, "Please wait...");
    lcd_rule(u, BOTTOM_ROW, 0);
    ui_wake(u, 100);
    return 1;
}

// Choosing from a MIDI file or an MPC .ALL: the sequence (an .ALL holds several), then its tracks,
// which go onto the first empty tracks of the sequence on Into.
static void import_frame(struct os *s, struct ui *u, const struct input *in) {
    struct seq_import *im = s->imp;
    char line[48];
    int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (s->disk_ask == 'Q') {
        im->pick = clampi(im->pick + d, 0, im->nseq - 1);
        if (input_down(in, IN_B1)) { // <Cancel>
            imp_free(im), free(im), s->imp = 0, s->disk_ask = 0;
            ui_wake(u, 1);
            return;
        }
        if (input_down(in, IN_B3) || in->encoder_press) imp_select_all(im, 1), im->cursor = 0, s->disk_ask = 'T'; // <Tracks>
        lcd_rule(u, TITLE_ROW, "Load MPC 2500 Seq");
        snprintf(line, sizeof line, "%.24s", im->file);
        lcd_text(u, 0, 3, line);
        lcd_text(u, 0, 4, "Which sequence:");
        int rows = 8, first = clampi(im->pick - rows / 2, 0, im->nseq > rows ? im->nseq - rows : 0);
        for (int r = 0; r < rows && first + r < im->nseq; r++) {
            const struct imp_seq *q = &im->seq[first + r];
            snprintf(line, sizeof line, " %02d %-10.10s%4db %3d.%d ", first + r + 1, q->name, q->bars, q->tempo10 / 10, q->tempo10 % 10);
            if (first + r == im->pick) lcd_inverse(u, 0, 5 + r, line);
            else lcd_text(u, 0, 5 + r, line);
        }
        lcd_text(u, 0, KEY_ROW, "<Cancel>");
        lcd_text(u, 18, KEY_ROW, "<Tracks>");
        lcd_rule(u, BOTTOM_ROW, 0);
        ui_wake(u, 1);
        return;
    }
    const struct imp_seq *q = &im->seq[im->pick];
    im->cursor = clampi(im->cursor + d, 0, q->ntracks - 1);
    if (in->encoder_press && in->shift) { // SHIFT and push: a drum part or a melodic one
        struct imp_track *t = &im->seq[im->pick].track[im->cursor];
        t->midi = !t->midi;
    } else if (in->encoder_press) {
        im->sel[im->cursor] = !im->sel[im->cursor]; // pushed: in or out
    }
    if (input_down(in, IN_B1)) { // <Back> to the sequences, or <Cancel>
        if (im->kind == IMP_MPC && im->nseq > 1) s->disk_ask = 'Q';
        else imp_free(im), free(im), s->imp = 0, s->disk_ask = 0;
        ui_wake(u, 1);
        return;
    }
    if (input_down(in, IN_B2)) { // <All>, or <None> when all are in
        int all = 1;
        for (int t = 0; t < q->ntracks; t++) all &= im->sel[t];
        imp_select_all(im, !all);
    }
    if (input_down(in, IN_B3)) { // <Load>
        int any = 0;
        for (int t = 0; t < q->ntracks; t++) any |= im->sel[t];
        if (!any) {
            os_toast(s, u, "Choose a track first");
        } else {
            os_undo_save(s, "Load Tracks");
            int first, dropped, n = imp_apply(s, im, s->disk_which, &first, &dropped);
            if (n <= 0) snprintf(line, sizeof line, "No empty tracks in Seq %02d", s->disk_which + 1);
            else if (dropped) snprintf(line, sizeof line, "Trk %d-%d; %d notes off pads", first + 1, first + n, dropped);
            else if (n == 1) snprintf(line, sizeof line, "Loaded to track %d", first + 1);
            else snprintf(line, sizeof line, "Loaded to tracks %d-%d", first + 1, first + n);
            os_toast(s, u, line);
            if (n > 0) s->eq_seq = -1;
            imp_free(im), free(im), s->imp = 0, s->disk_ask = 0;
            ui_wake(u, 1);
            return;
        }
    }
    lcd_rule(u, TITLE_ROW, im->kind == IMP_MPC ? "Load MPC 2500 Seq" : "Load MIDI File");
    snprintf(line, sizeof line, "%.12s: %db %d.%d BPM", q->name, q->bars, q->tempo10 / 10, q->tempo10 % 10);
    lcd_text(u, 0, 3, line);
    snprintf(line, sizeof line, "Into Seq %02d, empty tracks", s->disk_which + 1);
    lcd_text(u, 0, 4, line);
    int rows = 8, first = clampi(im->cursor - rows / 2, 0, q->ntracks > rows ? q->ntracks - rows : 0);
    for (int r = 0; r < rows && first + r < q->ntracks; r++) {
        const struct imp_track *t = &q->track[first + r];
        snprintf(line, sizeof line, "%c%-12.12s %-4s%5d ", im->sel[first + r] ? '*' : ' ', t->name, t->midi ? "KEYS" : "DRUM", t->count);
        if (first + r == im->cursor) lcd_inverse(u, 0, 5 + r, line);
        else lcd_text(u, 0, 5 + r, line);
    }
    lcd_text(u, 0, 13, "Push: in/out  SHIFT: type");
    int all = 1;
    for (int t = 0; t < q->ntracks; t++) all &= im->sel[t];
    lcd_text(u, 0, KEY_ROW, im->kind == IMP_MPC && im->nseq > 1 ? "<Back>" : "<Cancel>");
    lcd_text(u, 9, KEY_ROW, all ? "<None>" : "<All>");
    lcd_text(u, 20, KEY_ROW, "<Load>");
    lcd_rule(u, BOTTOM_ROW, 0);
    ui_wake(u, 1);
}

// LOAD's second page: the folder's items, worked as Load Files is. The wheel moves the bar, pushing
// it (or B3) opens a folder or loads the item, B1 goes up a folder (at the top: back to the settings),
// B2 renames, SHIFT + B2 deletes (asked first).
static void load_item(struct os *s); // the load, through its questions

static void load_item(struct os *s) {
    if (s->disk_clear && clear_ok(s->disk_type)) s->disk_ask = 'C';          // Clear all first?
    else if (is_prog(s->disk_type)) s->disk_ask = 'P';                       // into which program
    else if (s->disk_type == DISK_SEQFILE && seq_in_use(s, s->disk_which)) s->disk_ask = 'O'; // replace it?
    else s->disk_pending = 'L';
}

static void browse_frame(struct os *s, struct ui *u, const struct input *in) {
    int n = s->disk_nfiles;
    int was = s->disk_file;
    s->disk_file = clampi(s->disk_file + in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS), 0, n > 0 ? n - 1 : 0);
    if (s->disk_file != was) disk_keep_bar(s);
    int on_item = n && !s->disk_isdir[s->disk_file], on_folder = n && s->disk_isdir[s->disk_file];
    if (input_down(in, IN_B1) && !in->shift) {                    // <Back>
        if (n && s->disk_isdir[0] == 2) open_folder(s, 0);
        else s->disk_page = 0;
    } else if (input_down(in, IN_B2) && in->shift && on_item) {   // SHIFT + B2: <Delete>, asked first
        s->disk_ask = 'D';
    } else if (input_down(in, IN_B2) && on_item) {               // <Rename>
        snprintf(s->disk_name, sizeof s->disk_name, "%s", s->disk_files[s->disk_file]);
        s->disk_renaming = 1, s->disk_cursor = DF_NAME, s->disk_pos = 0;
    } else if ((in->encoder_press || input_down(in, IN_B3)) && on_folder) {
        open_folder(s, s->disk_file);                             // <Open> (.. goes up)
    } else if ((in->encoder_press || input_down(in, IN_B3)) && on_item) {
        load_item(s);                                             // <Load>
    }
    if (s->disk_ask || s->disk_renaming || s->disk_page == 0) { // drawn as that next frame
        ui_wake(u, 1);
        return;
    }
    n = s->disk_nfiles;
    on_item = n && !s->disk_isdir[s->disk_file], on_folder = n && s->disk_isdir[s->disk_file];
    lcd_text(u, 0, KEY_ROW, "<Back>");
    if (on_item) lcd_text(u, 8, KEY_ROW, in->shift ? "<Delete>" : "<Rename>");
    if (on_folder) lcd_text(u, 20, KEY_ROW, "<Open>");
    else if (on_item) lcd_text(u, 20, KEY_ROW, "<Load>");
    char title[40];
    snprintf(title, sizeof title, "Load %s", TYPES[s->disk_type]);
    lcd_rule(u, TITLE_ROW, s->disk_pending ? "Working..." : title);
    char dir[700], here[700], root[700], v[48];
    type_dir(s, s->disk_type, dir, sizeof dir);
    clean_path(dir, here, sizeof here);
    card_root(s, root, sizeof root);
    size_t rl = strlen(root);
    const char *where = strncmp(here, root, rl) == 0 ? here + rl : here;
    if (!*where) where = "/";
    size_t len = strlen(where);
    lcd_text(u, 0, 3, len > LCD_COLS ? where + len - LCD_COLS : where);
    int first_row = 4, rows = s->disk_clear ? 9 : 10;
    int items = 0;
    for (int i = 0; i < n; i++) items += !s->disk_isdir[i] || s->disk_isdir[i] == 1;
    if (!n) lcd_center(u, 8, "(empty)");
    else if (!items) lcd_center(u, 9, "(nothing to load here)");
    int top = s->disk_file - rows / 2 < 0 ? 0 : s->disk_file - rows / 2;
    if (top > n - rows) top = n - rows < 0 ? 0 : n - rows;
    for (int r = 0; r < rows && top + r < n; r++) {
        int i = top + r;
        char nm[LCD_COLS + 1];
        if (s->disk_isdir[i] == 2) snprintf(nm, sizeof nm, "..");
        else snprintf(nm, sizeof nm, "%.24s%s", s->disk_files[i], s->disk_isdir[i] ? "/" : "");
        if (i == s->disk_file) {
            snprintf(v, sizeof v, "%-*s", LCD_COLS, nm);
            lcd_inverse(u, 0, first_row + r, v);
        } else {
            lcd_text(u, 0, first_row + r, nm);
        }
    }
    if (s->disk_clear) lcd_inverse(u, 0, 13, " Clear all: YES ");
    lcd_rule(u, BOTTOM_ROW, 0);
}

void disk_frame(struct os *s, struct ui *u, const struct input *in) {
    if (export_progress(s, u)) return;
    if (s->disk_pending) { // drawn "Working..." last frame: now it happens
        int a = s->disk_pending;
        s->disk_pending = 0;
        perform(s, u, a);
    }
    if (!s->data_dir) {
        lcd_rule(u, TITLE_ROW, "Save/Load");
        lcd_center(u, 6, "No card folder");
        lcd_rule(u, BOTTOM_ROW, 0);
        return;
    }
    if (s->imp && (s->disk_ask == 'Q' || s->disk_ask == 'T')) { // choosing from a MIDI or MPC file
        import_frame(s, u, in);
        return;
    }
    if (s->disk_ask) { // a question first: replacing or deleting
        if (input_down(in, IN_B1)) s->disk_ask = 0;
        if (s->disk_ask == 'P') { // the wheel picks the program it goes into
            int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
            s->disk_which = clampi(s->disk_which + d, 0, OS_PROGRAMS - 1);
            if (input_down(in, IN_B2)) { // <To Empty>: the first program with no sounds, nothing cleared
                int to = -1;
                for (int g = 0; g < OS_PROGRAMS && to < 0; g++)
                    if (!os_program_sounds(s, g)) to = g;
                if (to < 0) {
                    os_toast(s, u, "No empty program");
                } else {
                    s->disk_which = to;
                    s->disk_pending = 'L', s->disk_ask = 0;
                }
            }
        }
        if (input_down(in, IN_B3) && s->disk_ask == 'C') { // Clear all, then the load
            disk_clear_all(s);
            s->disk_ask = 0, s->disk_pending = 'L';
        } else if (input_down(in, IN_B3)) s->disk_pending = s->disk_ask == 'P' || s->disk_ask == 'O' ? 'L' : s->disk_ask, s->disk_ask = 0;
        lcd_text(u, 0, KEY_ROW, s->disk_ask == 'P' ? "<Cancel>" : "<No>");
        if (s->disk_ask == 'P') lcd_text(u, 9, KEY_ROW, "<To Empty>");
        lcd_text(u, 21, KEY_ROW, "<Yes>");
        lcd_rule(u, TITLE_ROW, "Save/Load");
        char q[48];
        if (s->disk_ask == 'D') snprintf(q, sizeof q, "Delete %.16s?", s->disk_files[s->disk_file]);
        else if (s->disk_ask == 'P') snprintf(q, sizeof q, "Load into Program %02d?", s->disk_which + 1);
        else if (s->disk_ask == 'O') snprintf(q, sizeof q, "Replace Seq %02d?", s->disk_which + 1);
        else if (s->disk_ask == 'C') snprintf(q, sizeof q, "Clear all, then load?");
        else snprintf(q, sizeof q, "Replace %.16s?", s->disk_name);
        lcd_center(u, 6, q);
        if (s->disk_ask == 'D') lcd_center(u, 8, "This cannot be undone");
        if (s->disk_ask == 'C') {
            lcd_center(u, 8, "Programs, sounds, sequences");
            lcd_center(u, 9, "and songs are cleared");
        }
        if (s->disk_ask == 'P') { // which program, and that what is there goes
            snprintf(q, sizeof q, "%02d-%.16s", s->disk_which + 1, s->programs[s->disk_which].name);
            lcd_center(u, 7, q);
            lcd_center(u, 9, "Data will be cleared");
        }
        lcd_rule(u, BOTTOM_ROW, 0);
        ui_wake(u, 1);
        return;
    }
    int m = s->disk_mode;
    if (m == DISK_LOAD && s->disk_page == 1 && !s->disk_renaming && !s->disk_newdir) {
        browse_frame(s, u, in);
        return;
    }
    // Keys.
    int on_folder = s->disk_nfiles && s->disk_isdir[s->disk_file];
    if (s->disk_newdir) { // naming a new folder
        if (input_down(in, IN_B1)) { // <Cancel>
            s->disk_newdir = 0, s->disk_cursor = DF_FILE;
            snprintf(s->disk_name, sizeof s->disk_name, "%s", s->disk_keep);
        }
        if (input_down(in, IN_B3)) s->disk_pending = 'N', s->disk_newdir = 0, s->disk_cursor = DF_FILE; // <OK>
    } else if (s->disk_renaming) {
        if (input_down(in, IN_B1)) s->disk_renaming = 0, s->disk_cursor = m == DISK_LOAD ? DF_MODE : DF_FILE; // <Cancel>
        if (input_down(in, IN_B3)) s->disk_pending = 'R', s->disk_renaming = 0, s->disk_cursor = m == DISK_LOAD ? DF_MODE : DF_FILE; // <OK>
    } else if (m == DISK_USB) {
        if (input_down(in, IN_B3)) { // <Start>: USB DATA MODE
            usbmode_open(s, u);
            return;
        }
    } else if (m == DISK_LOAD) {
        if (input_down(in, IN_B3)) s->disk_page = 1, list_files(s); // <Browse>: the files
    } else if (m != DISK_LOAD && s->disk_type != DISK_AUTOLOAD && input_down(in, IN_B1)) { // <New Dir>
        snprintf(s->disk_keep, sizeof s->disk_keep, "%s", s->disk_name);
        snprintf(s->disk_name, sizeof s->disk_name, "FOLDER");
        s->disk_newdir = 1, s->disk_cursor = DF_NAME, s->disk_pos = 0;
    } else if (m != DISK_LOAD && s->disk_type != DISK_AUTOLOAD && input_down(in, IN_B2) && s->disk_nfiles) { // <Open>
        if (on_folder) open_folder(s, s->disk_file);
        else snprintf(s->disk_name, sizeof s->disk_name, "%s", s->disk_files[s->disk_file]); // its name, to save over it
    } else if (input_down(in, IN_B3)) { // <Save>, <Export>
        char name[64], path[700];
        trimmed(s->disk_name, name, sizeof name);
        item_path(s, s->disk_type, name, path, sizeof path);
        if (m == DISK_EXPORT && !name[0]) os_toast(s, u, "Name it first");
        else if (m == DISK_EXPORT) s->disk_ask = access(path, F_OK) == 0 ? 'E' : 0, s->disk_pending = s->disk_ask ? 0 : 'E';
        else if (!name[0] && s->disk_type != DISK_AUTOLOAD) os_toast(s, u, "Name it first");
        else if (s->disk_type == DISK_MPCPRJ) { // a folder of that name already: replace?
            char dir[700], folder[800];
            type_dir(s, s->disk_type, dir, sizeof dir);
            snprintf(folder, sizeof folder, "%s/%s", dir, name);
            if (access(folder, F_OK) == 0) s->disk_ask = 'S';
            else s->disk_pending = 'S';
        } else if (access(path, F_OK) == 0) s->disk_ask = 'S'; // already there: replace it?
        else s->disk_pending = 'S';
    }
    // The cursor, over the fields shown.
    if (!field_shown(s, s->disk_cursor)) s->disk_cursor = DF_MODE;
    if (in->encoder_press)
        for (int g = 0; g < DF_FIELDS; g++) {
            s->disk_cursor = (s->disk_cursor + (in->shift ? DF_FIELDS - 1 : 1)) % DF_FIELDS;
            if (field_shown(s, s->disk_cursor)) break;
        }
    int d = in->encoder, pm = input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
    if (s->disk_cursor != DF_NAME) d += pm;
    if (d || pm) {
        switch (s->disk_cursor) {
        case DF_MODE:
            s->disk_mode = clampi(m + d, 0, DISK_MODES - 1);
            if (s->disk_mode == DISK_EXPORT && s->disk_type < DISK_SEQUENCE) s->disk_type = DISK_SEQUENCE;
            if (s->disk_mode != DISK_EXPORT && s->disk_type >= DISK_SEQUENCE) s->disk_type = DISK_PROJECT;
            if (s->disk_mode == DISK_SAVE && (s->disk_type == DISK_MIDI || s->disk_type == DISK_MPC)) s->disk_type = DISK_SEQFILE;
            if (s->disk_mode == DISK_SAVE && s->disk_type == DISK_MPCPGM) s->disk_type = DISK_PROGRAM;
            default_name(s), restore_folder(s); // the new type's folder: where it was last used
            break;
        case DF_TYPE:
            if (m == DISK_EXPORT) s->disk_type = clampi(s->disk_type + d, DISK_SEQUENCE, DISK_MIX_SONG);
            else if (m == DISK_SAVE) { // MIDI FILE, MPC 2500 SEQUENCE, MPC 2500 PROGRAM: load only
                int t2 = clampi(s->disk_type + d, 0, DISK_MPCPRJ);
                while (t2 > DISK_SEQFILE && t2 < DISK_MPCPRJ) t2 += d > 0 ? 1 : -1;
                s->disk_type = t2;
            } else s->disk_type = clampi(s->disk_type + d, 0, DISK_MPCPRJ);
            s->disk_which = is_prog(s->disk_type) ? s->program : is_seqtype(s->disk_type) ? s->seq.current : 0;
            default_name(s), restore_folder(s); // the new type's folder: where it was last used
            break;
        case DF_WHICH:
            if (is_sound(s->disk_type)) s->selected = clampi(s->selected + d, 0, OS_PADS - 1);
            else s->disk_which = clampi(s->disk_which + d, 0, (is_prog(s->disk_type) ? OS_PROGRAMS : is_song(s->disk_type) ? SEQ_SONGS
                                                              : s->disk_type == DISK_ARP ? ARP_USER : SEQ_COUNT) - 1);
            if (m != DISK_LOAD) default_name(s);
            break;
        case DF_CLEAR: s->disk_clear = d > 0; break;
        case DF_TRACKS: s->disk_tracks = clampi(s->disk_tracks + d, 0, 2); break;
        case DF_MIX: s->disk_mix = d > 0; break;
        case DF_FILE: s->disk_file = clampi(s->disk_file + d, 0, s->disk_nfiles > 0 ? s->disk_nfiles - 1 : 0), disk_keep_bar(s); break;
        case DF_NAME: { // the wheel the character, - and + the place in the name
            s->disk_pos = clampi(s->disk_pos + pm, 0, NAME_MAX_LEN - 1);
            int len = (int)strlen(s->disk_name);
            while (len <= s->disk_pos) s->disk_name[len++] = ' ', s->disk_name[len] = 0;
            if (in->encoder) {
                const char *at = strchr(CHARS, s->disk_name[s->disk_pos]);
                int i = at ? (int)(at - CHARS) : 0, n = (int)strlen(CHARS);
                s->disk_name[s->disk_pos] = CHARS[((i + in->encoder) % n + n) % n];
            }
            break;
        }
        }
    }

    // ---- Drawing ----
    char v[48];
    on_folder = s->disk_nfiles && s->disk_isdir[s->disk_file];
    if (s->disk_renaming || s->disk_newdir) {
        lcd_text(u, 0, KEY_ROW, "<Cancel>");
        lcd_text(u, 22, KEY_ROW, "<OK>");
    } else if (m == DISK_USB) {
        lcd_text(u, 19, KEY_ROW, "<Start>");
    } else if (m == DISK_LOAD) {
        lcd_text(u, 18, KEY_ROW, "<Browse>");
    } else if (s->disk_type != DISK_AUTOLOAD) {
        lcd_text(u, 0, KEY_ROW, "<New Dir>");
        if (s->disk_nfiles) lcd_text(u, 10, KEY_ROW, "<Open>");
        lcd_text(u, m == DISK_EXPORT ? 18 : 20, KEY_ROW, m == DISK_EXPORT ? "<Export>" : "<Save>");
    } else {
        lcd_text(u, 18, KEY_ROW, m == DISK_EXPORT ? "<Export>" : "<Save>");
    }
    if (lcd_layout == LAYOUT_2000XL && !s->disk_pending && (m == DISK_SAVE || m == DISK_LOAD)) {
        // The XL's picture in place of the title: LOAD, the card's arrow to the MPC; SAVE, the
        // MPC's arrow to the card.
        int load = m == DISK_LOAD;
        lcd_art(u, load ? 190 : 194, 30, load ? LCD_ART_CARD : LCD_ART_MPC, load ? 16 : 14);
        lcd_text_y(u, 19, 24, load ? "LOAD" : "SAVE"); // over the arrow
        lcd_art(u, 244, 40, LCD_ART_ARROW, 11);
        lcd_art(u, load ? 288 : 284, 30, load ? LCD_ART_MPC : LCD_ART_CARD, load ? 14 : 16);
    } else {
        lcd_rule(u, TITLE_ROW, s->disk_pending ? "Working..." : "Save/Load");
    }
    struct {
        int f, row, col;
        const char *label;
    } rows[] = {{DF_MODE, 3, 6, "Mode:"}, {DF_TYPE, 4, 6, "Type:"}};
    snprintf(v, sizeof v, " %s ", MODES[m]);
    lcd_text(u, 0, 3, rows[0].label);
    if (s->disk_cursor == DF_MODE) lcd_inverse(u, 6, 3, v);
    else lcd_text(u, 6, 3, v);
    if (m == DISK_USB) { // what it does, before it starts
        static const char *const ABOUT[] = {
            "The SD card becomes a USB",
            "drive on a computer, for",
            "copying samples to it.",
            "",
            "USB audio and MIDI stop,",
            "and the MPC is locked",
            "until you exit. Nothing in",
            "memory is lost.",
        };
        for (int i = 0; i < (int)(sizeof ABOUT / sizeof ABOUT[0]); i++) lcd_text(u, 0, 5 + i, ABOUT[i]);
        lcd_rule(u, BOTTOM_ROW, 0);
        return;
    }
    snprintf(v, sizeof v, " %s ", TYPES[s->disk_type]);
    lcd_text(u, 0, 4, rows[1].label);
    if (s->disk_cursor == DF_TYPE) lcd_inverse(u, 6, 4, v);
    else lcd_text(u, 6, 4, v);
    if (field_shown(s, DF_WHICH)) {
        const char *label = is_prog(s->disk_type) || is_seqtype(s->disk_type) ? (m == DISK_LOAD ? "Into:" : is_prog(s->disk_type) ? "Pgm:" : "Seq:")
                          : is_sound(s->disk_type) ? "Pad:"
                          : is_song(s->disk_type) ? "Song:" : s->disk_type == DISK_ARP ? "User:" : "Seq:";
        if (is_sound(s->disk_type))
            snprintf(v, sizeof v, " %c%02d %.12s ", 'A' + s->selected / 16, s->selected % 16 + 1,
                     s->pads[s->selected].s ? s->pads[s->selected].name : "(empty)");
        else if (is_prog(s->disk_type)) snprintf(v, sizeof v, " %02d-%.12s ", s->disk_which + 1, s->programs[s->disk_which].name);
        else if (is_song(s->disk_type)) snprintf(v, sizeof v, " %02d (%d steps) ", s->disk_which + 1, s->seq.song[s->disk_which].nsteps);
        else if (s->disk_type == DISK_ARP) snprintf(v, sizeof v, " USER %d ", s->disk_which + 1);
        else snprintf(v, sizeof v, " %02d-%.12s ", s->disk_which + 1, s->seq.sequence[s->disk_which].name);
        lcd_text(u, 0, 5, label);
        if (s->disk_cursor == DF_WHICH) lcd_inverse(u, 6, 5, v);
        else lcd_text(u, 6, 5, v);
    }
    if (field_shown(s, DF_CLEAR)) {
        lcd_text(u, 0, 6, "Clear all:");
        snprintf(v, sizeof v, " %s ", s->disk_clear ? "YES" : "NO");
        if (s->disk_cursor == DF_CLEAR) lcd_inverse(u, 11, 6, v);
        else lcd_text(u, 11, 6, v);
    }
    if (m == DISK_EXPORT && !is_mixdown(s->disk_type)) {
        static const char *const TR[3] = {"NONE", "MONO", "STEREO"};
        lcd_text(u, 0, 6, "Tracks:");
        snprintf(v, sizeof v, " %s ", TR[s->disk_tracks]);
        if (s->disk_cursor == DF_TRACKS) lcd_inverse(u, 7, 6, v);
        else lcd_text(u, 7, 6, v);
        lcd_text(u, 16, 6, "Mix:");
        snprintf(v, sizeof v, " %s ", s->disk_mix ? "ON" : "OFF");
        if (s->disk_cursor == DF_MIX) lcd_inverse(u, 20, 6, v);
        else lcd_text(u, 20, 6, v);
    }
    if (field_shown(s, DF_NAME)) { // the name, the character being set lit
        int row = s->disk_renaming ? 8 : m == DISK_EXPORT ? 7 : 6; // the folder's list under it
        lcd_text(u, 0, row, s->disk_newdir ? "Folder:" : s->disk_renaming ? "New name:" : "Name:");
        int col = s->disk_renaming ? 10 : s->disk_newdir ? 8 : 6;
        char nm[NAME_MAX_LEN + 1];
        snprintf(nm, sizeof nm, "%-16s", s->disk_name);
        lcd_text(u, col, row, nm);
        if (s->disk_cursor == DF_NAME) {
            char one[2] = {nm[s->disk_pos], 0};
            lcd_inverse(u, col + s->disk_pos, row, one);
            lcd_text(u, 0, row + 1, "Wheel: letter  -/+: move");
        }
    } else if (m == DISK_SAVE && s->disk_type == DISK_AUTOLOAD) {
        lcd_text(u, 0, 8, "Saves this project to");
        lcd_text(u, 0, 9, "load at power-on");
    }
    if (field_shown(s, DF_FILE)) { // the folder's contents, a window round the selected
        int top = m == DISK_SAVE ? 8 : m == DISK_EXPORT ? 9 : 7, rows = 13 - top;
        if (s->disk_cursor == DF_NAME && m != DISK_LOAD) top++, rows--; // under the name's hint
        if (!s->disk_nfiles) lcd_text(u, 0, top, "(empty)");
        int items = 0;
        for (int i = 0; i < s->disk_nfiles; i++) items += !s->disk_isdir[i] || s->disk_isdir[i] == 1;
        if (m == DISK_LOAD && s->disk_nfiles && !items) lcd_text(u, 0, top + 1, "(nothing to load here)");
        int first = s->disk_file - rows / 2 < 0 ? 0 : s->disk_file - rows / 2;
        for (int r = 0; r < rows && first + r < s->disk_nfiles; r++) {
            int i = first + r;
            char nm[30];
            if (s->disk_isdir[i] == 2) snprintf(nm, sizeof nm, ".. (up)");
            else if (s->disk_isdir[i]) snprintf(nm, sizeof nm, "/%.21s", s->disk_files[i]); // a folder
            else snprintf(nm, sizeof nm, "%.22s", s->disk_files[i]);
            snprintf(v, sizeof v, " %-22.22s ", nm);
            if (i == s->disk_file && s->disk_cursor == DF_FILE) lcd_inverse(u, 0, top + r, v);
            else if (i == s->disk_file) lcd_text(u, 0, top + r, ">"), lcd_text(u, 1, top + r, v + 1);
            else lcd_text(u, 0, top + r, v);
        }
    }
    { // where: the folder, from the top of the card
        char dir[700], here[700], root[700];
        type_dir(s, s->disk_type, dir, sizeof dir);
        clean_path(dir, here, sizeof here);
        card_root(s, root, sizeof root);
        size_t rl = strlen(root);
        const char *where = strncmp(here, root, rl) == 0 ? here + rl : here;
        if (!*where) where = "/";
        size_t len = strlen(where);
        if (len > 18) snprintf(v, sizeof v, "Folder: ..%s", where + len - 16); // the end of a long path
        else snprintf(v, sizeof v, "Folder: %s", where);
        lcd_text(u, 0, 13, v);
    }
    lcd_rule(u, BOTTOM_ROW, 0);
    if (s->disk_pending) { // what is about to happen, plainly, while it does
        static const char *const WHAT[DISK_TYPES] = {"project", "program", "sample", "settings", "autoload", "FX chain", "arp pattern",
                                                     "track FX", "S600 wave", "sequence", "MIDI file", "MPC file", "MPC program", "MPC project", "", "", "", ""};
        char a[40];
        int act = s->disk_pending;
        if (act == 'E') snprintf(a, sizeof a, "Starting export");
        else if (act == 'S') snprintf(a, sizeof a, "Saving %s", WHAT[s->disk_type]);
        else if (act == 'L') snprintf(a, sizeof a, "Loading %s", WHAT[s->disk_type]);
        else if (act == 'D') snprintf(a, sizeof a, "Deleting");
        else if (act == 'N') snprintf(a, sizeof a, "Making folder");
        else snprintf(a, sizeof a, "Renaming");
        lcd_fill(u, LCD_X, 5 * CELL_H - 4, UI_W - 2 * LCD_X, 3 * CELL_H + 8, LCD_FG);
        char b[48];
        snprintf(b, sizeof b, "%.24s", a);
        lcd_inverse(u, (LCD_COLS - (int)strlen(b)) / 2, 5, b);
        lcd_inverse(u, (LCD_COLS - 14) / 2, 7, "Please wait...");
        ui_wake(u, 1);
    }
}
