#include "seq.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int TC_GRID[TC_COUNT] = {96, 64, 48, 32, 24, 16, 12, 8, 6, 4, 0};

int seq_tc_grid(int tc) {
    return tc >= 0 && tc < TC_COUNT ? TC_GRID[tc] : 0;
}

static unsigned char pad_note[128]; // the pad note map; set up by seq_init

int seq_pad_note(int pad) {
    return pad >= 0 && pad < 128 ? pad_note[pad] : 0;
}

int seq_note_pad(int note) {
    for (int p = 0; p < 128; p++)
        if (pad_note[p] == note) return p;
    return -1;
}

void seq_set_pad_note(int pad, int note) {
    if (pad >= 0 && pad < 128) pad_note[pad] = (unsigned char)(note < 0 ? 0 : note > 127 ? 127 : note);
}

void seq_default_pad_notes(void) {
    for (int p = 0; p < 128; p++) pad_note[p] = (unsigned char)(35 + p > 127 ? 127 : 35 + p); // the MPC3000's, from 35
}

int seq_beat_ticks(const struct seq_sequence *q) {
    return SEQ_PPQ * 4 / q->sig_den;
}

int seq_bar_ticks(const struct seq_sequence *q) {
    return q->sig_num * seq_beat_ticks(q);
}

int seq_length(const struct seq_sequence *q) {
    return q->bars * seq_bar_ticks(q);
}

int seq_track_used(const struct seq_track *t) {
    return t->count > 0;
}

int seq_used(const struct seq_sequence *q) {
    for (int t = 0; t < SEQ_TRACKS; t++)
        if (q->track[t].count > 0) return 1;
    return 0;
}

void seq_init(struct seq *s) {
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->lock, 0);
    s->next = -1;
    seq_default_pad_notes();
    s->tc = TC_16;
    s->rep_rate = REP_16;
    s->arp_oct = 1, s->arp_gate = 50;
    for (int u = 0; u < ARP_USER; u++) seq_arp_steps_clear(&s->arp_user[u]);
    s->rep_alt = -1;
    s->swing = 50;
    s->shift_later = 1;
    s->metronome = METRO_REC;
    s->count_in = 1;
    s->count_bars = 1;
    s->click_rate = CLICK_4;
    s->click_volume = 100;
    s->erase_note = -1;
    for (int i = 0; i < SEQ_COUNT; i++) seq_sequence_reset(&s->sequence[i], i);
}

void seq_sequence_reset(struct seq_sequence *q, int i) {
    for (int t = 0; t < SEQ_TRACKS; t++) free(q->track[t].notes), free(q->track[t].autos);
    memset(q, 0, sizeof *q);
    snprintf(q->name, sizeof q->name, "Sequence%02d", i + 1);
    q->bars = 2;
    q->sig_num = 4;
    q->sig_den = 4;
    q->tempo10 = 1200;
    q->loop_bar = 1;
    for (int t = 0; t < SEQ_TRACKS; t++) {
        struct seq_track *k = &q->track[t];
        snprintf(k->name, sizeof k->name, "Track%02d", t + 1);
        k->type = TRACK_DRUM;
        k->on = 1;
        k->vel_pct = 100;
        k->level = 100;
        k->eq = (struct track_eq){.type = EQ_1073, .on = 1, .lf = 110, .mf = 1600, .mq = 7, .hf = 12000};
    }
}

const int SEQ_SP_SWINGS[SP_SWINGS] = {50, 54, 58, 63, 67, 71};

// The MPC's swing: in each pair of grid steps the second one lands at `swing` percent of
// the pair, so 50 is straight and 66 is a triplet feel. 1/8, 1/16 and 1/32 swing.
// The SP-1200's six settings put a swung 16th 0, 2, 4 ... 10 ticks (96 PPQN) late, half
// steps of its 24 PPQN clock, as measured from its grooves; an 8th twice that.
// Swing, after the MPC500/2000 manual (Open500 supplement, page 13): where the swung step of a
// pair lands, in ticks from the pair's start.
// MPC3000: fixed steps. The swung 1/32 moves a whole number of ticks, 0 to 6 (50 to 75%,
// rounded), and the delay doubles for each larger grid: at 1/16, 66% is exactly 8 ticks, a
// true triplet.
// MPC2000 (and MPC500): (swing - 50) x grid / 50 ticks, the fraction thrown away: at 1/16, 66%
// is 7.68, so 7; most settings land up to a tick early.
// The MPC60's swing, measured from its grooves: the 1/16 delay in ticks for each setting 50..75.
// It moves a tick at a time (the MPC 3000 setting moves two), and 64 lands on 6, not 7.
static const unsigned char MPC60_16[26] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 6, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12};

int seq_swing_ticks(const struct seq *s, int grid) {
    if (s->res == SEQ_RES_MPC2000) return grid + (s->swing - 50) * grid / 50;
    if (s->res == SEQ_RES_MPC60) { // 1/8 twice the 1/16 delay, 1/32 half of it (rounded up): scaled, not measured
        int sw = s->swing < 50 ? 50 : s->swing > 75 ? 75 : s->swing;
        return grid + (MPC60_16[sw - 50] * grid + 12) / 24;
    }
    if (s->res == SEQ_RES_MPC3000) {
        int d32 = ((s->swing - 50) * 12 * 2 + 50) / 100; // (swing - 50) x 12 / 50, rounded
        return grid + d32 * grid / 12;
    }
    int k = 0; // the SP-1200
    while (k < SP_SWINGS - 1 && SEQ_SP_SWINGS[k] < s->swing) k++;
    return grid + 2 * k * grid / 24;
}

int seq_swing_step(const struct seq *s, int swing, int d) {
    if (s->res != SEQ_RES_SP1200) return swing + d < 50 ? 50 : swing + d > 75 ? 75 : swing + d;
    int k = 0;
    while (k < SP_SWINGS - 1 && SEQ_SP_SWINGS[k] < swing) k++;
    k += d;
    return SEQ_SP_SWINGS[k < 0 ? 0 : k >= SP_SWINGS ? SP_SWINGS - 1 : k];
}

uint32_t seq_correct(const struct seq *s, uint32_t tick, int length) {
    int grid = seq_tc_grid(s->tc);
    int64_t t = tick;
    if (grid > 0) {
        int64_t step = (t + grid / 2) / grid;
        t = step * grid;
        if ((s->tc == TC_8 || s->tc == TC_16 || s->tc == TC_32) && (step & 1)) t = (step - 1) * grid + seq_swing_ticks(s, grid);
    }
    if (s->res == SEQ_RES_SP1200 && !((s->tc == TC_8 || s->tc == TC_16 || s->tc == TC_32) && grid > 0))
        t = (t + 2) / 4 * 4; // the SP-1200 records on its 24 PPQN clock
    t += s->shift_later ? s->shift_ticks : -s->shift_ticks;
    if (length > 0) {
        t %= length;
        if (t < 0) t += length;
    }
    return (uint32_t)t;
}

static int lower_bound(const struct seq_track *k, double tick) {
    int lo = 0, hi = k->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (k->notes[mid].tick < tick) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Caller holds the lock. UI thread only, since it may allocate.
static struct seq_note *insert_note(struct seq_track *k, uint32_t tick) {
    if (k->count == k->cap) {
        int cap = k->cap ? k->cap * 2 : 64;
        struct seq_note *grown = realloc(k->notes, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        k->notes = grown;
        k->cap = cap;
    }
    int at = lower_bound(k, tick + 0.5); // after notes already on this tick
    memmove(&k->notes[at + 1], &k->notes[at], (size_t)(k->count - at) * sizeof k->notes[0]);
    k->count++;
    memset(&k->notes[at], 0, sizeof k->notes[at]);
    k->notes[at].tick = tick;
    return &k->notes[at];
}

int seq_add_note(struct seq *s, int sequence, int track, uint32_t tick, int note, int vel, int dur) {
    pthread_mutex_lock(&s->lock);
    struct seq_note *n = insert_note(&s->sequence[sequence].track[track], tick);
    if (n) {
        n->note = (uint8_t)note;
        n->vel = (uint8_t)vel;
        n->dur = (uint16_t)(dur < 1 ? 1 : dur > 65535 ? 65535 : dur);
    }
    pthread_mutex_unlock(&s->lock);
    return n ? 0 : -1;
}

void seq_send_raw(struct seq *s, const unsigned char *m, int n) {
    if (s->midi_send && !s->midi_off) s->midi_send(m, n);
}

static void send1(struct seq *s, int byte) {
    unsigned char m[1] = {(unsigned char)byte};
    seq_send_raw(s, m, 1);
}

static void send3(struct seq *s, int status, int a, int b) {
    if (!s->midi_send || s->midi_off) return;
    unsigned char m[3] = {(unsigned char)status, (unsigned char)(a & 0x7f), (unsigned char)(b & 0x7f)};
    s->midi_send(m, 3);
}

void seq_send_cc(struct seq *s, int channel, int cc, int value) {
    if (channel < 1 || channel > 16) return;
    send3(s, 0xb0 | (channel - 1), cc, value);
}

void seq_live_midi(struct seq *s, int channel, int note, int vel) {
    if (channel < 1 || channel > 16) return;
    send3(s, (vel > 0 ? 0x90 : 0x80) | (channel - 1), note, vel);
}

static void release(struct seq *s, struct engine *e, const struct seq_pending *p) {
    if (p->midi) send3(s, 0x80 | (p->channel - 1), p->note, 0);
    else engine_release_pad_key(e, p->note, p->key);
}

int seq_plays_s600(const struct seq *s, const struct seq_track *k) {
    return k->type == TRACK_DRUM && k->kit >= 0 && k->kit < ENGINE_PROGRAMS && s->kit_s600[k->kit];
}

int seq_track_slot(const struct seq *s, const struct seq_track *k, int pad) {
    return (seq_plays_s600(s, k) ? ENGINE_S600 : 0) + k->kit * ENGINE_PROGRAM_PADS + pad;
}

static void flush_pending(struct seq *s, struct engine *e) {
    for (int i = 0; i < s->npending; i++) release(s, e, &s->pending[i]);
    s->npending = 0;
}

static void add_pending_key(struct seq *s, double at, int midi, int note, int channel, int key) {
    if (s->npending >= SEQ_PENDING) return;
    s->pending[s->npending++] = (struct seq_pending){.at = at, .midi = midi, .note = note, .channel = channel, .key = key};
}

static void add_pending(struct seq *s, double at, int midi, int note, int channel) {
    add_pending_key(s, at, midi, note, channel, midi ? 0 : ENGINE_KEY_ANY);
}

// A note starts on the first frame at or after its moment.
static int clamp_offset(double frames, int block) {
    int f = (int)ceil(frames - 1e-6);
    return f < 0 ? 0 : f >= block ? block - 1 : f;
}

struct block {
    struct engine *e;
    int frames;     // frames in this audio block
    int late;       // catching up after a skipped block: everything plays at once
    double tpf;     // ticks per frame
    double abs0;    // absolute tick at the start of the block
    double done;    // frames of this block already accounted for
};

static int offset_of(const struct block *b, double ticks_in) {
    return b->late ? 0 : clamp_offset(b->done + ticks_in / b->tpf, b->frames);
}

static void cancel_pending(struct seq *s, int midi, int note, int channel, int key);

static void fire(struct seq *s, const struct block *b, const struct seq_track *k, const struct seq_note *n,
                 double ticks_in) {
    if (k->type != TRACK_MIDI && k->kit >= 0 && k->kit < ENGINE_PROGRAMS && n->note < 128) { // Pad Mute, Pad Solo
        const unsigned char *solo = s->pad_solo[k->kit];
        if (s->pad_mute[k->kit][n->note]) return;
        if (!solo[n->note] && memchr(solo, 1, 128)) return;
    }
    int vel = n->vel * k->vel_pct / 100;
    vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
    double end = b->abs0 + b->done * b->tpf + ticks_in + n->dur;
    if (k->type != TRACK_MIDI) {
        int pad = seq_track_slot(s, k, n->note); // this track's program
        int t = (int)(k - s->sequence[s->current].track); // the mixer knows tracks by number
        engine_play_pad_mix(b->e, pad, vel, n->tune, offset_of(b, ticks_in), t + 1, k->level, k->pan);
        // This note takes over the pad's release. An earlier note's release still waiting (a note
        // longer than the loop: its end falls in the next pass) would otherwise cut this one off.
        cancel_pending(s, 0, pad, 0, n->tune);
        add_pending_key(s, end, 0, pad, 0, n->tune);
    }
    int midi_note = k->type == TRACK_MIDI ? n->note : seq_plays_s600(s, k) ? 60 + n->tune : seq_pad_note(n->note);
    midi_note = midi_note < 0 ? 0 : midi_note > 127 ? 127 : midi_note;
    if (k->channel > 0) {
        send3(s, 0x90 | (k->channel - 1), midi_note, vel);
        cancel_pending(s, 1, midi_note, k->channel, 0); // the same for a MIDI note: the new one's end counts
        add_pending(s, end, 1, midi_note, k->channel);
    }
}

// The MIDI control each recorded parameter goes out as, or -1 for none (tuning has no
// standard control).
static const int AUTO_CC[AUTO_PARAMS] = {7, 10, 74, 71, -1, -1};

static int auto_lower_bound(const struct seq_track *k, double tick) {
    int lo = 0, hi = k->nauto;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (k->autos[mid].tick < tick) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Recorded fader moves in [start, end): a DRUM track changes the pad's sound, and any track
// with a channel sends them out as MIDI controls.
static void play_autos(struct seq *s, const struct block *b, const struct seq_track *k, double start, double end) {
    for (int a = auto_lower_bound(k, start); a < k->nauto && k->autos[a].tick < end; a++) {
        const struct seq_auto *m = &k->autos[a];
        if (k->type != TRACK_MIDI) engine_apply_param(b->e, seq_track_slot(s, k, m->pad), m->param, m->value);
        if (k->channel > 0 && m->param < AUTO_PARAMS && AUTO_CC[m->param] >= 0)
            send3(s, 0xb0 | (k->channel - 1), AUTO_CC[m->param], m->value);
    }
}

// Plays and erases the notes in [start, end) of the current sequence.
static void play_range(struct seq *s, const struct block *b, double start, double end) {
    struct seq_sequence *q = &s->sequence[s->current];
    int solo = 0; // with any track soloed, only soloed tracks play
    for (int t = 0; t < SEQ_TRACKS && !solo; t++) solo = q->track[t].solo;
    for (int t = 0; t < SEQ_TRACKS; t++) {
        struct seq_track *k = &q->track[t];
        if (!k->count && !k->nauto) continue;
        int active = t == s->track;
        int i = lower_bound(k, start);
        while (i < k->count && k->notes[i].tick < end) {
            struct seq_note *n = &k->notes[i];
            int replace = active && s->mode == SEQ_RECORDING && !n->fresh;
            int erase = active && s->erase_note >= 0 && n->note == s->erase_note;
            if (replace || erase) {
                memmove(n, n + 1, (size_t)(k->count - i - 1) * sizeof *n);
                k->count--;
                continue;
            }
            if (n->fresh) n->fresh = 0;
            else if (k->on && !k->mute && (!solo || k->solo)) fire(s, b, k, n, n->tick - start);
            i++;
        }
        if (k->on) play_autos(s, b, k, start, end);
    }
}

static int clicking(const struct seq *s) {
    if (s->mode == SEQ_COUNT_IN) return 1;
    if (s->audio_take) return s->click_audio && s->metronome != METRO_OFF; // kept out of an audio take unless asked
    if (s->loop_after) return s->click_loop && s->metronome != METRO_OFF;  // after a Live Looper take: After Loop
    if (s->metronome == METRO_ON) return 1;
    return s->metronome == METRO_REC && (s->mode == SEQ_RECORDING || s->mode == SEQ_OVERDUB);
}

int seq_click_ticks(int rate) {
    static const int ticks[CLICK_RATES] = {96, 64, 48, 32, 24, 16, 12};
    return rate >= 0 && rate < CLICK_RATES ? ticks[rate] : SEQ_PPQ;
}

static void clicks(struct seq *s, const struct block *b, double start, double end) {
    if (!clicking(s)) return;
    const struct seq_sequence *q = &s->sequence[s->current];
    int step = seq_click_ticks(s->click_rate), bar = seq_bar_ticks(q);
    for (double t = ceil(start / step) * step; t < end; t += step) {
        long long whole = (long long)t;
        int accent = ((whole % bar) + bar) % bar == 0;
        engine_click(b->e, accent, offset_of(b, t - start), s->click_volume);
    }
}

static int repeating(const struct seq *s) {
    if (!s->repeat) return 0;
    for (int i = 0; i < 16; i++)
        if (s->rep[i].on) return 1;
    return 0;
}

// ---- The arpeggiator ----

// Chord patterns work on the pads held, lowest first (by pad, then by tuning). Riffs are
// melodies, in semitones from the lowest pad held, which is played retuned for each note;
// REST is a silent step. Riffs are simplified to one note a step at the pattern's rate.
enum { A_UP, A_DOWN, A_UPDOWN, A_DOWNUP, A_UPDOWN_INCL, A_PLAYED, A_RANDOM, A_CHORD,
       A_UP2, A_UPDOWN2, A_THUMB, A_PINKY, A_CONVERGE, A_UP_OCT, A_STUTTER, A_THREE, A_RIFF,
       A_CHORD_RHYTHM, // the held chord, on the steps the pattern marks (0), resting on the others
       A_STEPS, A_USER }; // step patterns: a STEP preset, or USER n (made on the Pattern Edit page)
#define R (-128)
static const signed char RIFF_FEEL[] = {0, 12, 7, 10};
// ---- More riffs, all original (styles, not songs), and public-domain classical tunes ----
// Synth sequences.
static const signed char RIFF_ITALO[] = {0, 12, 0, 12, 0, 12, 0, 12, -2, 10, -2, 10, -4, 8, -4, 8};
static const signed char RIFF_BERLIN[] = {0, 7, 12, 7, 3, 7, 12, 15, 0, 7, 12, 7, 3, 7, 10, 7};
static const signed char RIFF_SYNTHWAVE[] = {0, 7, 12, 7, 0, 7, 12, 15, -4, 3, 8, 3, -4, 3, 8, 12};
static const signed char RIFF_TRANCE[] = {0, 12, 7, 0, 12, 7, 0, 12, 3, 15, 10, 3, 15, 10, 3, 15};
static const signed char RIFF_ACID[] = {0, 0, 12, 0, 3, 0, 10, 12, 0, R, 7, 0, 15, 12, 0, 10};
static const signed char RIFF_EUROBEAT[] = {0, R, 12, 0, R, 12, 0, 12, -2, R, 10, -2, R, 10, -2, 10};
static const signed char RIFF_KOSMISCHE[] = {0, 7, 10, 12, 15, 12, 10, 7, 0, 7, 10, 12, 17, 15, 12, 10};
static const signed char RIFF_MINIMAL[] = {0, R, 0, 12, R, 0, R, 7, 0, R, 0, 12, R, 10, R, 7};
// Basslines.
static const signed char RIFF_FUNK[] = {0, R, 12, 0, R, 10, R, 7, 0, R, 0, 12, R, 3, 5, R};
static const signed char RIFF_WALKING[] = {0, 4, 7, 9, 10, 9, 7, 4};
static const signed char RIFF_REGGAE[] = {0, R, R, 0, 7, R, 5, R, 0, R, R, 0, 3, R, 5, 7};
static const signed char RIFF_ELECTRO[] = {0, R, 0, 12, R, 0, 10, R, 0, R, 0, 12, R, 7, 10, R};
static const signed char RIFF_ROLLER[] = {0, R, R, 0, R, R, 0, R, R, R, 3, R, 5, R, R, R};
static const signed char RIFF_ROCK8[] = {0, 0, 0, 0, 0, 0, 0, 0, 5, 5, 5, 5, 7, 7, 7, 7};
static const signed char RIFF_BOOGIE[] = {0, 4, 7, 9, 10, 9, 7, 4, 5, 9, 12, 14, 15, 14, 12, 9};
static const signed char RIFF_HOUSE_BASS[] = {R, R, 0, R, R, R, 0, R, R, R, 0, R, R, 12, 0, R};
// Chord rhythms: 0 the chord held, R a rest.
static const signed char RHY_OFFBEAT[] = {R, 0};
static const signed char RHY_CHARLESTON[] = {0, R, R, 0, R, R, R, R};
static const signed char RHY_TRESILLO[] = {0, R, R, 0, R, R, 0, R};
static const signed char RHY_CLAVE[] = {0, R, R, 0, R, R, 0, R, R, R, 0, R, 0, R, R, R};
static const signed char RHY_HOUSE[] = {R, R, 0, R, R, 0, R, R, 0, R, R, R, 0, R, R, R};
static const signed char RHY_SKANK[] = {R, R, 0, R, R, R, 0, R};
static const signed char RHY_PUSH[] = {0, R, R, R, R, R, R, 0, R, R, 0, R, R, R, R, R};
static const signed char RHY_GATED[] = {0, 0, R, 0, 0, R, 0, R, 0, 0, R, 0, R, 0, 0, R};
// Game: chiptune arpeggios and figures.
static const signed char RIFF_CHIP_MAJ[] = {0, 4, 7, 12};
static const signed char RIFF_CHIP_MIN[] = {0, 3, 7, 12};
static const signed char RIFF_CHIP_FANFARE[] = {0, 4, 7, 12, R, 7, 12, R, 16, R, R, R, R, R, R, R};
static const signed char RIFF_CHIP_BASS[] = {0, 12, 0, 12, 5, 17, 5, 17, 7, 19, 7, 19, 5, 17, 5, 17};
static const signed char RIFF_POWER_UP[] = {0, 4, 7, 12, 16, 19, 24, R};
static const signed char RIFF_DUNGEON[] = {0, 3, 7, 3, 1, 3, 7, 3, 0, 3, 7, 3, -1, 3, 6, 3};
// Classical, public domain.
static const signed char RIFF_ELISE[] = {7, 6, 7, 6, 7, 2, 5, 3, 0, R, R, R};
static const signed char RIFF_MOUNTAIN[] = {0, 2, 3, 5, 7, 3, 7, R, 6, 2, 6, R, 5, 1, 5, R,
                                            0, 2, 3, 5, 7, 3, 7, 12, 10, 7, 3, 7, 10, R, R, R};
static const signed char RIFF_ODE[] = {4, 4, 5, 7, 7, 5, 4, 2, 0, 0, 2, 4, 4, 2, 2, R};
static const signed char RIFF_FIFTH[] = {R, 0, 0, 0, -4, -4, -4, -4, R, -2, -2, -2, -5, -5, -5, -5};
static const signed char RIFF_TURCA[] = {2, 0, -1, 0, 3, R, 5, 3, 2, 3, 7, R, 8, 7, 6, 7,
                                         14, 12, 11, 12, 14, 12, 11, 12, 15, R, R, R};
static const signed char RIFF_INVENTION[] = {R, 0, 2, 4, 5, 2, 4, 0, 7, R, 12, R, 11, R, 12, R};
static const signed char RIFF_GREENSLEEVES[] = {0, 3, 5, 7, 8, 7, 5, 2, -2, 0, 2, 3, 0, 0, -1, 0, 2, -1, -5, R};

// In the style of Moroder's early-80s soundtrack sequences (not any one of them): 16ths pumping
// root and octave under the fifth, over a minor i, VI, VII.
static const signed char RIFF_MOROD[] = {0, 12, 7, 12, 0, 12, 7, 12, 0, 12, 7, 12, 0, 12, 10, 12,
                                         -4, 8, 3, 8, -4, 8, 3, 8, -2, 10, 5, 10, -2, 10, 7, 10};
static const signed char RIFF_STRANGER[] = {0, 4, 7, 11, 12, 11, 7, 4};
static const signed char RIFF_HALLOWEEN[] = {5, 0, 0, 5, 0, 0, 5, 0, 1, 0};
static const signed char RIFF_TUBULAR[] = {0, 5, 0, 7, 0, 3, 5, 0, 8, 0, 10, 0, 7, 8, 7};
static const signed char RIFF_POPCORN[] = {0, -2, 0, -5, -9, -5, -12, R};
static const signed char RIFF_CANON[] = {0, -5, -3, -8, -7, -12, -7, -5};
static const signed char RIFF_AIR[] = {12, 0, 11, -1, 9, -3, 7, -5, 5, -7, 4, -8, 2, -10, 0, -12};
static const signed char RIFF_TOCCATA[] = {7, 5, 7, R, R, R, 5, 3, 2, 0, -1, R, 0, R, R, R};
static const signed char RIFF_MOONLIGHT[] = {-5, 0, 3, -5, 0, 3, -5, 0, 3, -5, 0, 3, -4, 0, 3, -4, 0, 3, -4, 1, 5, -4, 1, 5};
static const signed char RIFF_BOLERO[] = {0, R, R, 0, 0, 0, 0, R, R, 0, 0, 0, 0, R, R, 0, R, R,
                                          0, R, R, 0, 0, 0, 0, R, R, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const signed char RIFF_PRELUDE[] = {0, 4, 7, 12, 16, 7, 12, 16, 0, 4, 7, 12, 16, 7, 12, 16,
                                           0, 2, 9, 14, 17, 9, 14, 17, 0, 2, 9, 14, 17, 9, 14, 17};
static const signed char RIFF_FANTAISIE[] = {0, 7, 12, 15, 19, 15, 12, 7};
static const signed char RIFF_NACHTMUSIK[] = {0, R, -5, 0, R, -5, 0, -5, 0, 4, 7, R};
static const signed char RIFF_SUMMER[] = {0, 0, 0, 0, 0, 0, 0, 0, 15, 14, 12, 10, 9, 7, 6, 3};
// Famous riffs, from the notes as recorded (one note a step, rests where the riff rests).
static const signed char RIFF_SWEET_CHILD[] = {0, 12, 7, 5, 17, 7, 16, 7, 0, 12, 7, 5, 17, 7, 16, 7,
                                               2, 12, 7, 5, 17, 7, 16, 7, 2, 12, 7, 5, 17, 7, 16, 7,
                                               5, 12, 7, 5, 17, 7, 16, 7, 5, 12, 7, 5, 17, 7, 16, 7,
                                               0, 12, 7, 5, 17, 7, 16, 7, 0, 12, 7, 5, 17, 7, 16, 7};
static const signed char RIFF_BILLIE[] = {0, -5, -2, 0, -2, -5, -7, -5};
static const signed char RIFF_CRAZY_TRAIN[] = {0, 0, 7, 0, 8, 0, 7, 0, 5, 3, 2, 3, 5, 3, 2, -2};
static const signed char RIFF_SMOKE[] = {0, R, 3, R, 5, R, R, 0, R, 3, R, 6, 5, R, R, R,
                                         0, R, 3, R, 5, R, R, 3, R, 0, R, R, R, R, R, R};
static const signed char RIFF_SEVEN_NATION[] = {0, R, R, R, R, R, 0, R, 3, R, R, 0, R, R, -2, R,
                                                -4, R, R, R, R, R, R, R, -5, R, R, R, R, R, R, R};
static const signed char RIFF_MISSION[] = {0, R, R, 0, R, R, 3, R, 5, R, 0, R, R, 0, R, R, -2, R, -1, R};
static const signed char RIFF_FUNKYTOWN[] = {0, 0, -2, 0, R, -5, R, -5, 0, 5, 4, 0, R, R, R, R};
// More public-domain classical and traditional tunes.
static const signed char RIFF_KOROBEINIKI[] = {7, R, 2, 3, 5, R, 3, 2, 0, R, 0, 3, 7, R, 5, 3,
                                               2, R, R, 3, 5, R, 7, R, 3, R, 0, R, 0, R, R, R};
static const signed char RIFF_SPRING[] = {4, R, 4, R, 4, R, 2, 0, 7, R, R, R, R, R, 7, 5,
                                          4, R, 4, R, 4, R, 2, 0, 7, R, R, R, R, R, 7, 5,
                                          4, R, 5, R, 7, R, 5, R, 4, R, 2, R, R, R, R, R};
static const signed char RIFF_MORNING[] = {7, 4, 2, 0, 2, 4, 7, 4, 2, 0, 2, 4, 2, 4, 7, 4, 7, 9, 4, 9, 7, 4, 2, 0};
static const signed char RIFF_LULLABY[] = {4, 4, 7, R, R, R, 4, 4, 7, R, R, R, 4, 7, 12, R, 11, R, 9, R, 9, 7, R, R,
                                           2, 4, 5, R, 2, R, 2, 4, 5, R, R, R, 2, 5, 11, 9, 7, 11, 12, R, R, R, R, R};
static const signed char RIFF_BUMBLEBEE[] = {4, 3, 2, 1, 0, 5, 4, 3, 4, 3, 2, 1, 0, 1, 2, 3};
static const signed char RIFF_JESU[] = {0, 2, 4, 7, 5, 5, 9, 7, 7, 12, 11, 12, 7, 4, 0,
                                        2, 4, 5, 7, 9, 7, 5, 4, 2, 4, 0, -1, 0, R, R};
static const signed char RIFF_GYMNOPEDIE[] = {12, 15, 13, 12, 7, 5, 7, 8, 3, 0, R, R};
#undef R
#define RIFF(r) r, (int)sizeof r, 0

// ---- Step patterns: the STEP presets, all original ----
// N: a note (0..11 from C), its octave, accent, slide. TIE carries the note before on; REST.
#define N(n, o, a, sl) {n, o, ARP_NOTE, a, sl}
#define TIE {0, 0, ARP_TIE, 0, 0}
#define REST {0, 0, ARP_REST, 0, 0}
static const struct seq_arp_steps STEP_SQUELCH = {16, {N(0, 0, 1, 0), N(0, 0, 0, 0), N(0, 1, 0, 1), N(0, 0, 0, 0),
    REST, N(3, 0, 1, 0), N(0, 0, 0, 1), N(10, -1, 0, 0), N(0, 0, 1, 0), REST, N(7, 0, 0, 1), N(5, 0, 0, 0),
    N(3, 0, 1, 0), N(0, 1, 0, 1), N(0, 0, 0, 0), REST}};
static const struct seq_arp_steps STEP_RUBBER = {16, {N(0, 0, 1, 0), N(0, 1, 0, 1), N(0, 0, 0, 0), N(0, 1, 1, 0),
    N(0, 0, 0, 1), N(0, 1, 0, 0), N(3, 0, 0, 0), N(3, 1, 1, 1), N(3, 0, 0, 0), N(5, 0, 0, 1), N(5, 1, 0, 0), REST,
    N(0, 0, 1, 0), N(0, 1, 0, 1), N(10, 0, 0, 0), N(7, 0, 0, 0)}};
static const struct seq_arp_steps STEP_JACKIN = {16, {N(0, 0, 1, 0), REST, N(0, 0, 0, 0), N(0, 0, 0, 0),
    N(0, 1, 1, 0), REST, N(0, 0, 0, 1), N(3, 0, 0, 0), N(0, 0, 1, 0), REST, N(0, 0, 0, 0), N(7, 0, 0, 1),
    N(10, -1, 0, 0), TIE, N(0, 0, 1, 0), REST}};
static const struct seq_arp_steps STEP_DEEP = {16, {N(0, 0, 1, 1), TIE, N(3, 0, 0, 0), REST,
    N(5, 0, 0, 1), TIE, N(3, 0, 0, 0), N(0, 0, 0, 0), N(10, -1, 1, 1), TIE, N(0, 0, 0, 0), REST,
    N(7, -1, 0, 1), N(10, -1, 0, 0), N(0, 0, 1, 0), REST}};
static const struct seq_arp_steps STEP_PHRYGIAN = {16, {N(0, 0, 1, 0), N(1, 0, 0, 1), N(0, 0, 0, 0), REST,
    N(0, 1, 1, 0), N(0, 0, 0, 0), N(1, 0, 0, 1), N(0, 0, 0, 0), N(7, 0, 1, 0), REST, N(6, 0, 0, 1), N(5, 0, 0, 0),
    N(1, 0, 1, 0), N(0, 0, 0, 1), N(0, -1, 0, 0), REST}};
static const struct seq_arp_steps STEP_ROLLER = {16, {N(0, 0, 1, 0), N(0, 0, 0, 0), N(0, 1, 0, 0), N(0, 0, 0, 0),
    N(3, 0, 1, 0), N(0, 0, 0, 0), N(0, 1, 0, 1), N(3, 1, 0, 0), N(0, 0, 1, 0), N(0, 0, 0, 0), N(0, 1, 0, 0),
    N(0, 0, 0, 0), N(5, 0, 1, 0), N(3, 0, 0, 1), N(0, 1, 0, 0), N(10, 0, 0, 0)}};
static const struct seq_arp_steps STEP_THREE = {12, {N(0, 0, 1, 0), N(0, 1, 0, 1), N(3, 0, 0, 0), N(0, 0, 1, 0),
    N(7, 0, 0, 1), N(5, 0, 0, 0), N(0, 0, 1, 0), N(0, 1, 0, 1), N(10, 0, 0, 0), N(0, 0, 1, 0), N(3, 1, 0, 1),
    N(0, 1, 0, 0)}};
static const struct seq_arp_steps STEP_SUNNY = {16, {N(0, 0, 1, 0), N(4, 0, 0, 1), N(7, 0, 0, 0), N(0, 1, 0, 0),
    REST, N(7, 0, 1, 0), N(4, 0, 0, 1), N(2, 0, 0, 0), N(0, 0, 1, 0), REST, N(0, 1, 0, 1), N(11, 0, 0, 0),
    N(9, 0, 1, 0), N(7, 0, 0, 1), N(4, 0, 0, 0), REST}};
#undef N
#undef TIE
#undef REST
#define STEPS(p) 0, 0, &p
#define ARP_PRESET_DATA
#include "arp_presets.h" // the jazz lines, basslines, more step patterns and game cues
#undef ARP_PRESET_DATA

static const struct {
    const char *name;
    int group, kind, rate;
    const signed char *riff;
    int len;
    const struct seq_arp_steps *steps; // A_STEPS
} ARPS[] = {
    {"UP", ARP_GROUP_STANDARD, A_UP, REP_16, 0, 0, 0},
    {"DOWN", ARP_GROUP_STANDARD, A_DOWN, REP_16, 0, 0, 0},
    {"UP/DOWN", ARP_GROUP_STANDARD, A_UPDOWN, REP_16, 0, 0, 0},
    {"DOWN/UP", ARP_GROUP_STANDARD, A_DOWNUP, REP_16, 0, 0, 0},
    {"UP/DOWN INCL", ARP_GROUP_STANDARD, A_UPDOWN_INCL, REP_16, 0, 0, 0},
    {"AS PLAYED", ARP_GROUP_STANDARD, A_PLAYED, REP_16, 0, 0, 0},
    {"RANDOM", ARP_GROUP_STANDARD, A_RANDOM, REP_16, 0, 0, 0},
    {"CHORD", ARP_GROUP_STANDARD, A_CHORD, REP_8, 0, 0, 0},
    {"UP 2 OCT", ARP_GROUP_VARIATION, A_UP2, REP_16, 0, 0, 0},
    {"UP/DOWN 2 OCT", ARP_GROUP_VARIATION, A_UPDOWN2, REP_16, 0, 0, 0},
    {"THUMB", ARP_GROUP_VARIATION, A_THUMB, REP_16, 0, 0, 0},
    {"PINKY", ARP_GROUP_VARIATION, A_PINKY, REP_16, 0, 0, 0},
    {"CONVERGE", ARP_GROUP_VARIATION, A_CONVERGE, REP_16, 0, 0, 0},
    {"UP + OCTAVE", ARP_GROUP_VARIATION, A_UP_OCT, REP_16, 0, 0, 0},
    {"STUTTER", ARP_GROUP_VARIATION, A_STUTTER, REP_16, 0, 0, 0},
    {"3-STEP", ARP_GROUP_VARIATION, A_THREE, REP_16T, 0, 0, 0},
    {"MOROD 83", ARP_GROUP_SYNTH, A_RIFF, REP_16, RIFF(RIFF_MOROD)},
    {"I FEEL LOVE", ARP_GROUP_FAMOUS, A_RIFF, REP_16, RIFF(RIFF_FEEL)},
    {"STRANGER THINGS", ARP_GROUP_FAMOUS, A_RIFF, REP_8, RIFF(RIFF_STRANGER)},
    {"HALLOWEEN", ARP_GROUP_FAMOUS, A_RIFF, REP_8, RIFF(RIFF_HALLOWEEN)},
    {"TUBULAR BELLS", ARP_GROUP_FAMOUS, A_RIFF, REP_8, RIFF(RIFF_TUBULAR)},
    {"POPCORN", ARP_GROUP_FAMOUS, A_RIFF, REP_8, RIFF(RIFF_POPCORN)},
    {"CANON IN D", ARP_GROUP_CLASSICAL, A_RIFF, REP_4, RIFF(RIFF_CANON)},
    {"AIR ON G STRING", ARP_GROUP_CLASSICAL, A_RIFF, REP_8, RIFF(RIFF_AIR)},
    {"TOCCATA D MINOR", ARP_GROUP_CLASSICAL, A_RIFF, REP_16, RIFF(RIFF_TOCCATA)},
    {"MOONLIGHT", ARP_GROUP_CLASSICAL, A_RIFF, REP_8T, RIFF(RIFF_MOONLIGHT)},
    {"BOLERO", ARP_GROUP_CLASSICAL, A_RIFF, REP_16T, RIFF(RIFF_BOLERO)},
    {"PRELUDE IN C", ARP_GROUP_CLASSICAL, A_RIFF, REP_16, RIFF(RIFF_PRELUDE)},
    {"FANTAISIE-IMP", ARP_GROUP_CLASSICAL, A_RIFF, REP_16, RIFF(RIFF_FANTAISIE)},
    {"NACHTMUSIK", ARP_GROUP_CLASSICAL, A_RIFF, REP_8, RIFF(RIFF_NACHTMUSIK)},
    {"SUMMER: PRESTO", ARP_GROUP_CLASSICAL, A_RIFF, REP_16, RIFF(RIFF_SUMMER)},
    // Added later, at the end so saved patterns keep their numbers; each shows in its category.
    {"ITALO OCTAVES", ARP_GROUP_SYNTH, A_RIFF, REP_16, RIFF(RIFF_ITALO)},
    {"BERLIN SCHOOL", ARP_GROUP_SYNTH, A_RIFF, REP_16, RIFF(RIFF_BERLIN)},
    {"SYNTHWAVE", ARP_GROUP_SYNTH, A_RIFF, REP_16, RIFF(RIFF_SYNTHWAVE)},
    {"TRANCE GATE", ARP_GROUP_SYNTH, A_RIFF, REP_16, RIFF(RIFF_TRANCE)},
    {"ACID LINE", ARP_GROUP_SYNTH, A_RIFF, REP_16, RIFF(RIFF_ACID)},
    {"EUROBEAT", ARP_GROUP_SYNTH, A_RIFF, REP_16, RIFF(RIFF_EUROBEAT)},
    {"KOSMISCHE", ARP_GROUP_SYNTH, A_RIFF, REP_16, RIFF(RIFF_KOSMISCHE)},
    {"MINIMAL", ARP_GROUP_SYNTH, A_RIFF, REP_16, RIFF(RIFF_MINIMAL)},
    {"FUNK BASS", ARP_GROUP_BASS, A_RIFF, REP_16, RIFF(RIFF_FUNK)},
    {"WALKING BASS", ARP_GROUP_BASS, A_RIFF, REP_8, RIFF(RIFF_WALKING)},
    {"REGGAE BASS", ARP_GROUP_BASS, A_RIFF, REP_16, RIFF(RIFF_REGGAE)},
    {"ELECTRO BASS", ARP_GROUP_BASS, A_RIFF, REP_16, RIFF(RIFF_ELECTRO)},
    {"DNB ROLLER", ARP_GROUP_BASS, A_RIFF, REP_16, RIFF(RIFF_ROLLER)},
    {"ROCK 8THS", ARP_GROUP_BASS, A_RIFF, REP_8, RIFF(RIFF_ROCK8)},
    {"BOOGIE", ARP_GROUP_BASS, A_RIFF, REP_8, RIFF(RIFF_BOOGIE)},
    {"HOUSE BASS", ARP_GROUP_BASS, A_RIFF, REP_16, RIFF(RIFF_HOUSE_BASS)},
    {"OFFBEAT", ARP_GROUP_CHORD, A_CHORD_RHYTHM, REP_8, RIFF(RHY_OFFBEAT)},
    {"CHARLESTON", ARP_GROUP_CHORD, A_CHORD_RHYTHM, REP_8, RIFF(RHY_CHARLESTON)},
    {"TRESILLO", ARP_GROUP_CHORD, A_CHORD_RHYTHM, REP_8, RIFF(RHY_TRESILLO)},
    {"CLAVE 3-2", ARP_GROUP_CHORD, A_CHORD_RHYTHM, REP_16, RIFF(RHY_CLAVE)},
    {"HOUSE STAB", ARP_GROUP_CHORD, A_CHORD_RHYTHM, REP_16, RIFF(RHY_HOUSE)},
    {"SKANK", ARP_GROUP_CHORD, A_CHORD_RHYTHM, REP_16, RIFF(RHY_SKANK)},
    {"PUSH", ARP_GROUP_CHORD, A_CHORD_RHYTHM, REP_16, RIFF(RHY_PUSH)},
    {"GATED PAD", ARP_GROUP_CHORD, A_CHORD_RHYTHM, REP_16, RIFF(RHY_GATED)},
    {"CHIP MAJOR", ARP_GROUP_GAME, A_RIFF, REP_32, RIFF(RIFF_CHIP_MAJ)},
    {"CHIP MINOR", ARP_GROUP_GAME, A_RIFF, REP_32, RIFF(RIFF_CHIP_MIN)},
    {"FANFARE", ARP_GROUP_GAME, A_RIFF, REP_16, RIFF(RIFF_CHIP_FANFARE)},
    {"CHIP BASS", ARP_GROUP_GAME, A_RIFF, REP_16, RIFF(RIFF_CHIP_BASS)},
    {"POWER UP", ARP_GROUP_GAME, A_RIFF, REP_32, RIFF(RIFF_POWER_UP)},
    {"DUNGEON", ARP_GROUP_GAME, A_RIFF, REP_16, RIFF(RIFF_DUNGEON)},
    {"FUR ELISE", ARP_GROUP_CLASSICAL, A_RIFF, REP_16, RIFF(RIFF_ELISE)},
    {"MOUNTAIN KING", ARP_GROUP_CLASSICAL, A_RIFF, REP_8, RIFF(RIFF_MOUNTAIN)},
    {"ODE TO JOY", ARP_GROUP_CLASSICAL, A_RIFF, REP_4, RIFF(RIFF_ODE)},
    {"5TH SYMPHONY", ARP_GROUP_CLASSICAL, A_RIFF, REP_8, RIFF(RIFF_FIFTH)},
    {"RONDO TURCA", ARP_GROUP_CLASSICAL, A_RIFF, REP_16, RIFF(RIFF_TURCA)},
    {"INVENTION 1", ARP_GROUP_CLASSICAL, A_RIFF, REP_16, RIFF(RIFF_INVENTION)},
    {"GREENSLEEVES", ARP_GROUP_CLASSICAL, A_RIFF, REP_8, RIFF(RIFF_GREENSLEEVES)},
    {"SWEET CHILD", ARP_GROUP_FAMOUS, A_RIFF, REP_8, RIFF(RIFF_SWEET_CHILD)},
    {"BILLIE JEAN", ARP_GROUP_FAMOUS, A_RIFF, REP_8, RIFF(RIFF_BILLIE)},
    {"CRAZY TRAIN", ARP_GROUP_FAMOUS, A_RIFF, REP_8, RIFF(RIFF_CRAZY_TRAIN)},
    {"SMOKE ON WATER", ARP_GROUP_FAMOUS, A_RIFF, REP_8, RIFF(RIFF_SMOKE)},
    {"SEVEN NATION", ARP_GROUP_FAMOUS, A_RIFF, REP_16, RIFF(RIFF_SEVEN_NATION)},
    {"MARIO THEME", ARP_GROUP_FAMOUS, A_RIFF, REP_16, RIFF(RIFF_MARIO_MAIN)},
    {"MISSION: IMP", ARP_GROUP_FAMOUS, A_RIFF, REP_8, RIFF(RIFF_MISSION)},
    {"FUNKYTOWN", ARP_GROUP_FAMOUS, A_RIFF, REP_16, RIFF(RIFF_FUNKYTOWN)},
    {"KOROBEINIKI", ARP_GROUP_CLASSICAL, A_RIFF, REP_8, RIFF(RIFF_KOROBEINIKI)},
    {"SPRING", ARP_GROUP_CLASSICAL, A_RIFF, REP_16, RIFF(RIFF_SPRING)},
    {"MORNING MOOD", ARP_GROUP_CLASSICAL, A_RIFF, REP_8, RIFF(RIFF_MORNING)},
    {"BRAHMS LULLABY", ARP_GROUP_CLASSICAL, A_RIFF, REP_8, RIFF(RIFF_LULLABY)},
    {"BUMBLEBEE", ARP_GROUP_CLASSICAL, A_RIFF, REP_16, RIFF(RIFF_BUMBLEBEE)},
    {"JESU JOY", ARP_GROUP_CLASSICAL, A_RIFF, REP_8T, RIFF(RIFF_JESU)},
    {"GYMNOPEDIE 1", ARP_GROUP_CLASSICAL, A_RIFF, REP_4, RIFF(RIFF_GYMNOPEDIE)},
    {"SQUELCH", ARP_GROUP_STEP, A_STEPS, REP_16, STEPS(STEP_SQUELCH)},
    {"RUBBER BOUNCE", ARP_GROUP_STEP, A_STEPS, REP_16, STEPS(STEP_RUBBER)},
    {"JACKIN", ARP_GROUP_STEP, A_STEPS, REP_16, STEPS(STEP_JACKIN)},
    {"DEEP TIES", ARP_GROUP_STEP, A_STEPS, REP_16, STEPS(STEP_DEEP)},
    {"PHRYGIAN", ARP_GROUP_STEP, A_STEPS, REP_16, STEPS(STEP_PHRYGIAN)},
    {"ROLLER", ARP_GROUP_STEP, A_STEPS, REP_16, STEPS(STEP_ROLLER)},
    {"3 OVER 4", ARP_GROUP_STEP, A_STEPS, REP_16, STEPS(STEP_THREE)},
    {"SUNNY", ARP_GROUP_STEP, A_STEPS, REP_16, STEPS(STEP_SUNNY)},
    {"USER 1", ARP_GROUP_USER, A_USER, REP_16, 0, 0, 0},
    {"USER 2", ARP_GROUP_USER, A_USER, REP_16, 0, 0, 0},
    {"USER 3", ARP_GROUP_USER, A_USER, REP_16, 0, 0, 0},
    {"USER 4", ARP_GROUP_USER, A_USER, REP_16, 0, 0, 0},
    {"USER 5", ARP_GROUP_USER, A_USER, REP_16, 0, 0, 0},
    {"USER 6", ARP_GROUP_USER, A_USER, REP_16, 0, 0, 0},
    {"USER 7", ARP_GROUP_USER, A_USER, REP_16, 0, 0, 0},
    {"USER 8", ARP_GROUP_USER, A_USER, REP_16, 0, 0, 0},
#define ARP_PRESET_LIST
#include "arp_presets.h"
#undef ARP_PRESET_LIST
};
#undef STEPS
#undef RIFF
#define NARPS ((int)(sizeof ARPS / sizeof ARPS[0]))

int seq_arp_patterns(void) {
    return NARPS;
}

const char *seq_arp_name(int i) {
    return i >= 0 && i < NARPS ? ARPS[i].name : "";
}

int seq_arp_group(int i) {
    return i >= 0 && i < NARPS ? ARPS[i].group : 0;
}

int seq_arp_is_riff(int i) {
    return i >= 0 && i < NARPS && ARPS[i].kind == A_RIFF;
}

const char *seq_arp_group_name(int g) {
    static const char *const NAMES[ARP_GROUPS] = {"STANDARD", "VARIATION", "SYNTH", "BASSLINES", "CHORD", "GAME", "FAMOUS", "CLASSICAL",
                                                  "JAZZ", "STEP", "USER"};
    return g >= 0 && g < ARP_GROUPS ? NAMES[g] : "";
}

int seq_arp_first(int group) {
    for (int i = 0; i < NARPS; i++)
        if (ARPS[i].group == group) return i;
    return 0;
}

int seq_arp_step(int pattern, int d) {
    if (pattern < 0 || pattern >= NARPS) return 0;
    int g = ARPS[pattern].group, step = d > 0 ? 1 : -1;
    for (int n = d > 0 ? d : -d; n > 0; n--) {
        int i = pattern + step;
        while (i >= 0 && i < NARPS && ARPS[i].group != g) i += step;
        if (i < 0 || i >= NARPS) break;
        pattern = i;
    }
    return pattern;
}

int seq_arp_user_slot(int pattern) {
    if (pattern < 0 || pattern >= NARPS || ARPS[pattern].kind != A_USER) return -1;
    int slot = 0;
    for (int i = 0; i < pattern; i++) slot += ARPS[i].kind == A_USER;
    return slot < ARP_USER ? slot : -1;
}

int seq_arp_user_pattern(int slot) {
    for (int i = 0; i < NARPS; i++)
        if (ARPS[i].kind == A_USER && slot-- == 0) return i;
    return 0;
}

const struct seq_arp_steps *seq_arp_steps_of(const struct seq *s, int pattern) {
    if (pattern < 0 || pattern >= NARPS) return 0;
    if (ARPS[pattern].kind == A_STEPS) return ARPS[pattern].steps;
    int slot = seq_arp_user_slot(pattern);
    return slot >= 0 ? &s->arp_user[slot] : 0;
}

void seq_arp_steps_clear(struct seq_arp_steps *p) {
    memset(p, 0, sizeof *p);
    p->len = ARP_STEPS;
}

// Each step as three hex digits: the note, (octave + 1) * 3 + time, accent * 2 + slide.
void seq_arp_steps_save(const struct seq_arp_steps *p, char *b, size_t n) {
    int at = snprintf(b, n, "%d,", p->len);
    for (int i = 0; i < ARP_STEPS && at > 0 && (size_t)at + 4 < n; i++) {
        const struct seq_arp_step *st = &p->step[i];
        at += snprintf(b + at, n - (size_t)at, "%x%x%x", st->note & 15, (st->oct + 1) * 3 + st->time, st->accent * 2 + st->slide);
    }
}

int seq_arp_steps_load(struct seq_arp_steps *p, const char *line) {
    int len = atoi(line);
    const char *h = strchr(line, ',');
    if (!h || len < 1 || len > ARP_STEPS || strlen(h + 1) < 3 * ARP_STEPS) return -1;
    h++;
    struct seq_arp_steps q = {.len = len};
    for (int i = 0; i < ARP_STEPS; i++) {
        int d[3];
        for (int k = 0; k < 3; k++) {
            char c = h[i * 3 + k];
            d[k] = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d[k] < 0) return -1;
        }
        if (d[0] > 11 || d[1] > 8 || d[2] > 3) return -1;
        q.step[i] = (struct seq_arp_step){(signed char)d[0], (signed char)(d[1] / 3 - 1), (unsigned char)(d[1] % 3),
                                          (unsigned char)(d[2] >> 1), (unsigned char)(d[2] & 1)};
    }
    *p = q;
    return 0;
}

int seq_arp_rate(int i) {
    return i >= 0 && i < NARPS ? ARPS[i].rate : REP_16;
}

// Which held entry (index into the sorted list, -1 none) and octave a chord pattern plays at
// step `pos` of n notes.
static int arp_pick(int kind, int pos, int n, unsigned *rnd, int *oct) {
    *oct = 0;
    if (n <= 0) return -1;
    switch (kind) {
    case A_UP: return pos % n;
    case A_DOWN: return n - 1 - pos % n;
    case A_UPDOWN: case A_DOWNUP: {
        if (n == 1) return 0;
        int cyc = 2 * n - 2, p = pos % cyc, up = p < n ? p : cyc - p;
        return kind == A_UPDOWN ? up : n - 1 - up;
    }
    case A_UPDOWN_INCL: { // the top and bottom notes twice
        int p = pos % (2 * n);
        return p < n ? p : 2 * n - 1 - p;
    }
    case A_RANDOM:
        *rnd = *rnd * 1664525u + 1013904223u;
        return (int)((*rnd >> 16) % (unsigned)n);
    case A_UP2: *oct = pos % (2 * n) / n; return pos % n;
    case A_UPDOWN2: {
        int m = 2 * n, cyc = m > 1 ? 2 * m - 2 : 1, p = pos % cyc, up = p < m ? p : cyc - p;
        *oct = up / n;
        return up % n;
    }
    case A_THUMB: // the lowest between each of the others
        if (n == 1) return 0;
        return pos % 2 == 0 ? 0 : 1 + pos / 2 % (n - 1);
    case A_PINKY: // the highest between each of the others
        if (n == 1) return 0;
        return pos % 2 == 1 ? n - 1 : pos / 2 % (n - 1);
    case A_CONVERGE: { // outside in: lowest, highest, next lowest, next highest
        int p = pos % n;
        return p % 2 == 0 ? p / 2 : n - 1 - p / 2;
    }
    case A_UP_OCT: *oct = pos % 2; return pos / 2 % n;
    case A_STUTTER: return pos / 2 % n;
    case A_THREE: return (pos / 3 + pos % 3) % n;
    default: return pos % n;
    }
}

// A note's release taken back: a slide to the same note carries it on instead.
static void cancel_pending(struct seq *s, int midi, int note, int channel, int key) {
    for (int i = 0; i < s->npending;) {
        const struct seq_pending *p = &s->pending[i];
        if (p->midi == midi && p->note == note && p->channel == channel && p->key == key) s->pending[i] = s->pending[--s->npending];
        else i++;
    }
}

// One held pad sounded, `add` semitones from how it is held, for `len` ticks. vel 0: as held.
// glide_ms: a slide from the note before (step patterns), the voice carried on to this pitch.
static void fire_note(struct seq *s, const struct block *b, double ticks_in, double seq_tick, int i, int add, int len,
                      int vel, int glide_ms) {
    int recording = seq_tick >= 0 && (s->mode == SEQ_RECORDING || s->mode == SEQ_OVERDUB);
    double end = b->abs0 + b->done * b->tpf + ticks_in + len;
    int note = s->rep[i].note, tune = s->rep[i].tune + add;
    if (vel <= 0) vel = s->rep[i].vel;
    tune = tune < -64 ? -64 : tune > 63 ? 63 : tune;
    if (s->rep[i].channel > 0) { // MIDI: the note itself moves; a slide overlaps the notes (legato)
        int out = note + add < 0 ? 0 : note + add > 127 ? 127 : note + add, ch = s->rep[i].channel;
        int same = glide_ms > 0 && add == s->arp_last;
        if (same) cancel_pending(s, 1, out, ch, 0); // the same note slid to: it holds on
        else send3(s, 0x90 | (ch - 1), out, vel);
        add_pending(s, end, 1, out, ch);
    } else {
        const struct seq_track *k = &s->sequence[s->current].track[s->track];
        int pad = seq_track_slot(s, k, note);
        if (glide_ms > 0) {
            if (add == s->arp_last) cancel_pending(s, 0, pad, 0, tune);
            engine_play_pad_glide(b->e, pad, vel, tune, offset_of(b, ticks_in), s->track + 1, k->level, k->pan, glide_ms);
        } else {
            engine_play_pad_mix(b->e, pad, vel, tune, offset_of(b, ticks_in), s->track + 1, k->level, k->pan);
        }
        add_pending_key(s, end, 0, pad, 0, tune);
    }
    if (recording && s->rep[i].record && s->nrecq < (int)(sizeof s->recq / sizeof s->recq[0])) {
        s->recq[s->nrecq].tick = (uint32_t)seq_tick;
        s->recq[s->nrecq].seq = s->current;
        s->recq[s->nrecq].track = s->track;
        s->recq[s->nrecq].note = s->rep[i].channel > 0 ? note + add : note;
        s->recq[s->nrecq].vel = vel;
        s->recq[s->nrecq].dur = len;
        s->recq[s->nrecq].tune = s->rep[i].channel > 0 ? s->rep[i].tune : tune;
        s->nrecq++;
    }
}

static void fire_one(struct seq *s, const struct block *b, double ticks_in, double seq_tick, int i, int add, int len) {
    fire_note(s, b, ticks_in, seq_tick, i, add, len, 0, 0);
}

#define ARP_GLIDE_MS 60 // a slide's time

static void fire_repeats(struct seq *s, const struct block *b, double ticks_in, double seq_tick, int grid) {
    if (!s->arp) { // Note Repeat: every held pad, every step
        for (int i = 0; i < 16; i++)
            if (s->rep[i].on) fire_one(s, b, ticks_in, seq_tick, i, 0, grid / 2);
        return;
    }
    // The held pads, lowest first (AS PLAYED: in the order pressed).
    int held[16], n = 0;
    for (int i = 0; i < 16; i++)
        if (s->rep[i].on) held[n++] = i;
    int a = s->arp_pattern >= 0 && s->arp_pattern < NARPS ? s->arp_pattern : 0, kind = ARPS[a].kind;
    for (int x = 1; x < n; x++)
        for (int y = x; y > 0; y--) {
            const int *p = &held[y - 1], *q = &held[y];
            long kp = kind == A_PLAYED ? s->rep[*p].order : s->rep[*p].note * 256L + s->rep[*p].tune;
            long kq = kind == A_PLAYED ? s->rep[*q].order : s->rep[*q].note * 256L + s->rep[*q].tune;
            if (kp <= kq) break;
            int tmp = held[y - 1];
            held[y - 1] = held[y], held[y] = tmp;
        }
    int pos = s->arp_pos++;
    if (n == 0) return;
    int len = grid * (s->arp_gate > 0 ? s->arp_gate : 50) / 100, tr = s->arp_trans;
    if (len < 1) len = 1;
    if (kind == A_STEPS || kind == A_USER) { // a step pattern on the lowest pad held
        const struct seq_arp_steps *pt = seq_arp_steps_of(s, a);
        int steps = pt ? (pt->len < 1 ? 1 : pt->len > ARP_STEPS ? ARP_STEPS : pt->len) : 0;
        if (!steps) return;
        const struct seq_arp_step *st = &pt->step[pos % steps];
        if (st->time == ARP_REST) {
            s->arp_slide = 0;
            return;
        }
        if (st->time == ARP_TIE) return; // the note before goes on
        int ties = 1; // this step and the ties after it
        while (ties < steps && pt->step[(pos + ties) % steps].time == ARP_TIE) ties++;
        // A slide holds into the next note, which takes the voice over; otherwise the gate.
        int nlen = st->slide ? ties * grid + grid / 4 : (ties - 1) * grid + len;
        int add = st->note + 12 * st->oct + tr, vel = s->rep[held[0]].vel;
        if (!st->accent) vel = vel * 7 / 10 > 0 ? vel * 7 / 10 : 1; // an accent is the full velocity
        fire_note(s, b, ticks_in, seq_tick, held[0], add, nlen, vel, s->arp_slide ? ARP_GLIDE_MS : 0);
        s->arp_slide = st->slide, s->arp_last = add;
    } else if (kind == A_RIFF) { // a melody on the lowest pad held
        int step = ARPS[a].riff[pos % ARPS[a].len];
        if (step != -128) fire_one(s, b, ticks_in, seq_tick, held[0], step + tr, len);
    } else if (kind == A_CHORD_RHYTHM) { // the chord on the marked steps
        if (ARPS[a].riff[pos % ARPS[a].len] != -128)
            for (int i = 0; i < n; i++) fire_one(s, b, ticks_in, seq_tick, held[i], tr, len);
    } else if (kind == A_CHORD) {
        for (int i = 0; i < n; i++) fire_one(s, b, ticks_in, seq_tick, held[i], tr, len);
    } else { // across the held pads, and up through Octaves
        int octs = s->arp_oct > 0 ? s->arp_oct : 1;
        if (kind == A_UP2) kind = A_UP, octs = octs < 2 ? 2 : octs;
        if (kind == A_UPDOWN2) kind = A_UPDOWN, octs = octs < 2 ? 2 : octs;
        int oct, at = arp_pick(kind, pos, n * octs, &s->arp_rand, &oct);
        if (at >= 0) fire_one(s, b, ticks_in, seq_tick, held[at % n], 12 * (oct + at / n) + tr, len);
    }
}

// Repeats falling in [start, end) of a timeline whose grid starts at `origin`. In the
// sequence, seq_origin turns a time into its sequence tick; otherwise it is negative.
int seq_repeat_ticks(int rate) {
    static const int ticks[REP_RATES] = {96, 64, 48, 32, 24, 16, 12, 8};
    return rate >= 0 && rate < REP_RATES ? ticks[rate] : SEQ_PPQ / 4;
}

const char *seq_repeat_name(int rate) {
    static const char *const names[REP_RATES] = {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"};
    return rate >= 0 && rate < REP_RATES ? names[rate] : "--";
}

// Rates alternate straight, triplet: REP_4, REP_4T, REP_8 ... so odd ones are triplets.
int seq_repeat_alt(int rate, int b) {
    int triplet = rate & 1, to;
    if (b == 0) to = triplet ? rate - 2 : rate - 1;      // 1/16: 1/8T.  1/16T: 1/8T
    else if (b == 1) to = triplet ? rate + 2 : rate + 1; // 1/16: 1/16T. 1/16T: 1/32T
    else to = triplet ? rate + 1 : rate + 2;             // 1/16: 1/32.  1/16T: 1/32
    return to >= 0 && to < REP_RATES ? to : -1;
}

static void repeat_range(struct seq *s, const struct block *b, double start, double end, double origin, int in_seq) {
    int rate = s->rep_alt >= 0 ? s->rep_alt : s->rep_rate;
    int grid = seq_repeat_ticks(rate);
    int swung = rate == REP_8 || rate == REP_16 || rate == REP_32;
    long k = (long)floor((start - origin) / grid) - 1;
    for (;; k++) {
        double t = origin + (double)k * grid;
        if ((k & 1) && swung) t = origin + (double)(k - 1) * grid + seq_swing_ticks(s, grid);
        if (t >= end) break;
        if (t < start) continue;
        fire_repeats(s, b, t - start, in_seq ? t : -1, grid);
    }
}

void seq_audio_block(void *ctx, struct engine *e, int frames) {
    struct seq *s = ctx;
    if (pthread_mutex_trylock(&s->lock) != 0) {
        s->missed += frames;
        return;
    }
    if (s->stop_flush) {
        flush_pending(s, e);
        s->stop_flush = 0;
    }
    struct seq_sequence *q = &s->sequence[s->current];
    struct block b = {.e = e, .frames = frames, .late = s->missed > 0, .abs0 = s->abs};
    b.tpf = q->tempo10 / 10.0 / 60.0 * SEQ_PPQ / ENGINE_RATE;
    double span = (frames + s->missed) * b.tpf;
    s->missed = 0;

    // Note lengths run out whether or not the sequence is still playing.
    for (int i = 0; i < s->npending;) {
        if (s->pending[i].at < s->abs + span) {
            release(s, e, &s->pending[i]);
            s->pending[i] = s->pending[--s->npending];
        } else {
            i++;
        }
    }
    // Stopped or counting in, Note Repeat runs on its own grid from the first pad.
    if (repeating(s) && (s->mode == SEQ_STOPPED || s->mode == SEQ_COUNT_IN || s->mode == SEQ_WAITING)) {
        if (s->rep_restart) s->rep_origin = s->abs;
        repeat_range(s, &b, s->abs, s->abs + span, s->rep_origin, 0);
    }
    s->rep_restart = 0;
    s->abs += span;
    if (s->mode == SEQ_STOPPED || s->mode == SEQ_WAITING) {
        pthread_mutex_unlock(&s->lock);
        return;
    }

    double remain = span;
    long long clock = atomic_load_explicit(&e->clock, memory_order_relaxed);
    while (remain > 1e-9 && s->mode != SEQ_STOPPED) {
        q = &s->sequence[s->current];
        if (s->mode == SEQ_COUNT_IN) {
            double end = s->pos + remain < 0 ? s->pos + remain : 0;
            clicks(s, &b, s->pos, end);
            remain -= end - s->pos;
            b.done += (end - s->pos) / b.tpf;
            s->pos = end;
            if (s->pos >= 0) {
                s->mode = s->count_target;
                if (s->sync_send) send1(s, 0xfa); // Start, on the first tick
                s->zero_clock = clock + (long long)llround(b.done); // where the sequence's first tick sounds
                s->zero_seen = 1;
            }
            continue;
        }
        double length = seq_length(q);
        double end = s->pos + remain < length ? s->pos + remain : length;
        if (s->sync_send) // MIDI clock: 24 a beat, every 4 of our ticks
            for (double t = ceil(s->pos / 4) * 4; t < end; t += 4) send1(s, 0xf8);
        play_range(s, &b, s->pos, end);
        clicks(s, &b, s->pos, end);
        if (repeating(s)) repeat_range(s, &b, s->pos, end, 0, 1); // on the sequence's own grid
        remain -= end - s->pos;
        b.done += (end - s->pos) / b.tpf;
        s->pos = end;
        if (s->pos < length) continue;
        if (s->mode == SEQ_RECORDING) s->mode = SEQ_OVERDUB; // as on the MPC3000: REC overdubs from the second pass
        if (s->song_on) { // a song: the step again for its Reps, then the next step, then the end
            const struct seq_song *g = &s->song[s->song_cur];
            if (s->song_step < g->nsteps && ++s->song_rep >= g->step[s->song_step].reps) s->song_rep = 0, s->song_step++;
            if (s->song_step >= g->nsteps) {
                s->song_on = 0;
                s->mode = SEQ_STOPPED;
                s->pos = 0;
                flush_pending(s, e);
            } else {
                s->current = g->step[s->song_step].seq;
                s->pos = 0;
            }
            continue;
        }
        if (s->next >= 0) {
            s->current = s->next;
            s->next = -1;
            s->pos = 0;
        } else if (q->loop_bar <= 0) {
            s->mode = SEQ_STOPPED;
            s->pos = 0;
            flush_pending(s, e);
        } else {
            double loop = (double)(q->loop_bar - 1) * seq_bar_ticks(q);
            s->pos = loop < length ? loop : 0;
            s->wrap_clock = clock + (long long)llround(b.done); // where the next pass begins (Live Looper)
            s->wrap_count++;
        }
    }
    pthread_mutex_unlock(&s->lock);
}

static void send_programs(struct seq *s) {
    const struct seq_sequence *q = &s->sequence[s->current];
    for (int t = 0; t < SEQ_TRACKS; t++) {
        const struct seq_track *k = &q->track[t];
        if (k->type == TRACK_MIDI && k->channel > 0 && k->program > 0) {
            unsigned char m[2] = {(unsigned char)(0xc0 | (k->channel - 1)), (unsigned char)(k->program - 1)};
            seq_send_raw(s, m, 2);
        }
    }
}

void seq_play(struct seq *s, int from_start) {
    pthread_mutex_lock(&s->lock);
    if (s->mode == SEQ_STOPPED) {
        const struct seq_sequence *q = &s->sequence[s->current];
        if (from_start || s->pos < 0 || s->pos >= seq_length(q)) s->pos = 0;
        int target = s->rec_arm == REC_ARM_REC ? SEQ_RECORDING
                   : s->rec_arm == REC_ARM_OVERDUB ? SEQ_OVERDUB : SEQ_PLAYING;
        if (target != SEQ_PLAYING && s->count_in && s->count_wait) {
            s->count_target = target; // the first pad or MIDI note starts it
            s->mode = SEQ_WAITING;
        } else if (target != SEQ_PLAYING && s->count_in) {
            s->count_target = target;
            s->mode = SEQ_COUNT_IN;
            s->pos -= (double)seq_bar_ticks(q) * (s->count_bars < 1 ? 1 : s->count_bars);
        } else {
            s->mode = target;
            if (s->sync_send) send1(s, s->pos > 0 ? 0xfb : 0xfa); // Continue, or Start
        }
        send_programs(s);
    }
    pthread_mutex_unlock(&s->lock);
}

void seq_play_count_in(struct seq *s, int bars) {
    pthread_mutex_lock(&s->lock);
    const struct seq_sequence *q = &s->sequence[s->current];
    s->rec_arm = REC_ARM_OFF;
    s->count_target = SEQ_PLAYING;
    s->mode = SEQ_COUNT_IN;
    // No bars: a count-in too short to click, so the start is still marked to the frame.
    s->pos = bars < 1 ? -0.001 : -(double)seq_bar_ticks(q) * bars;
    s->zero_seen = 0;
    send_programs(s);
    pthread_mutex_unlock(&s->lock);
}

void seq_stop(struct seq *s, int to_start) {
    pthread_mutex_lock(&s->lock);
    if (s->sync_send && s->mode != SEQ_STOPPED) send1(s, 0xfc);
    s->mode = SEQ_STOPPED;
    s->stop_flush = 1;
    s->song_on = 0;
    s->rec_arm = REC_ARM_OFF;
    s->next = -1;
    s->erase_note = -1;
    if (to_start || s->pos < 0) s->pos = 0;
    for (int i = 0; i < 256; i++) s->rec[i].open = 0;
    for (int i = 0; i < AUTO_PARAMS; i++) s->auto_live[i] = 0;
    pthread_mutex_unlock(&s->lock);
}

void seq_rec_button(struct seq *s) {
    pthread_mutex_lock(&s->lock);
    switch (s->mode) {
    case SEQ_STOPPED: // OVERDUB first, since it never erases anything; then REC; then off
        s->rec_arm = s->rec_arm == REC_ARM_OFF ? REC_ARM_OVERDUB : s->rec_arm == REC_ARM_OVERDUB ? REC_ARM_REC : REC_ARM_OFF;
        break;
    case SEQ_PLAYING:
        s->mode = SEQ_OVERDUB; // punch in, without erasing anything
        break;
    case SEQ_RECORDING:
    case SEQ_OVERDUB:
        s->mode = SEQ_PLAYING; // punch out, the sequence keeps playing
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&s->lock);
}

int seq_wait_start(struct seq *s) {
    pthread_mutex_lock(&s->lock);
    int started = s->mode == SEQ_WAITING;
    if (started) s->mode = s->count_target;
    if (started && s->sync_send) send1(s, 0xfa);
    pthread_mutex_unlock(&s->lock);
    return started;
}

// rec[] holds plain pad hits by note, and tuned ones (several keys of one pad) by key.
static int rec_index(int note, int tune) {
    return tune ? 128 + ((tune + 64) & 127) : note & 127;
}

int seq_record_hit(struct seq *s, int note, int vel) {
    return seq_record_hit_tuned(s, note, 0, vel);
}

void seq_record_release(struct seq *s, int note) {
    seq_record_release_tuned(s, note, 0);
}

int seq_record_hit_tuned(struct seq *s, int note, int tune, int vel) {
    int recorded = -1;
    tune = tune < -64 ? -64 : tune > 63 ? 63 : tune;
    pthread_mutex_lock(&s->lock);
    // A hit just before the downbeat, at the end of the count-in, belongs on beat one:
    // within half a Timing Correct step, or a 32nd with Timing Correct off.
    int grid = seq_tc_grid(s->tc);
    double early = grid > 0 ? grid / 2.0 : SEQ_PPQ / 8.0;
    int counting = s->mode == SEQ_COUNT_IN && s->pos >= -early;
    int recording = (s->mode == SEQ_RECORDING || s->mode == SEQ_OVERDUB) && s->pos >= 0;
    if ((recording || counting) && note >= 0 && note < 128) {
        struct seq_sequence *q = &s->sequence[s->current];
        int length = seq_length(q);
        uint32_t tick = seq_correct(s, counting ? 0 : (uint32_t)s->pos, length);
        struct seq_note *n = insert_note(&q->track[s->track], tick);
        if (n) {
            n->note = (uint8_t)note;
            n->tune = (int8_t)tune;
            n->vel = (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel);
            n->dur = SEQ_PPQ / 4; // until the pad is released
            // Corrected ahead of the play position, or around the loop: the hit already
            // sounded live, so this pass must not play it again.
            n->fresh = counting || tick >= s->pos || tick + (uint32_t)SEQ_PPQ < s->pos;
            int r = rec_index(note, tune);
            s->rec[r].open = 1;
            s->rec[r].seq = s->current;
            s->rec[r].track = s->track;
            s->rec[r].note = note;
            s->rec[r].tune = tune;
            s->rec[r].tick = tick;
            s->rec[r].abs = s->abs;
            recorded = 0;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return recorded;
}

void seq_record_release_tuned(struct seq *s, int note, int tune) {
    if (note < 0 || note >= 128) return;
    tune = tune < -64 ? -64 : tune > 63 ? 63 : tune;
    int r = rec_index(note, tune);
    pthread_mutex_lock(&s->lock);
    if (s->rec[r].open && s->rec[r].note == note && s->rec[r].tune == tune) {
        s->rec[r].open = 0;
        double held = s->abs - s->rec[r].abs;
        int dur = held < 1 ? 1 : held > 65535 ? 65535 : (int)held;
        struct seq_track *k = &s->sequence[s->rec[r].seq].track[s->rec[r].track];
        for (int i = lower_bound(k, s->rec[r].tick); i < k->count && k->notes[i].tick == s->rec[r].tick; i++) {
            if (k->notes[i].note == note && k->notes[i].tune == tune) {
                k->notes[i].dur = (uint16_t)dur;
                break;
            }
        }
    }
    pthread_mutex_unlock(&s->lock);
}

void seq_set_erase(struct seq *s, int note) {
    pthread_mutex_lock(&s->lock);
    s->erase_note = s->mode == SEQ_STOPPED ? -1 : note;
    pthread_mutex_unlock(&s->lock);
}

static int by_tick(const void *a, const void *b) {
    const struct seq_note *x = a, *y = b;
    return x->tick < y->tick ? -1 : x->tick > y->tick;
}

int seq_move_existing(struct seq *s, int track, uint32_t from, uint32_t to, const unsigned char *notes) {
    int moved = 0;
    pthread_mutex_lock(&s->lock);
    struct seq_sequence *q = &s->sequence[s->current];
    struct seq_track *k = &q->track[track];
    int length = seq_length(q);
    for (int i = 0; i < k->count; i++) {
        struct seq_note *n = &k->notes[i];
        if (n->tick < from || n->tick >= to || (notes && !notes[n->note])) continue;
        n->tick = seq_correct(s, n->tick, length);
        moved++;
    }
    if (moved) qsort(k->notes, (size_t)k->count, sizeof k->notes[0], by_tick);
    pthread_mutex_unlock(&s->lock);
    return moved;
}

void seq_set_repeat(struct seq *s, int on) {
    pthread_mutex_lock(&s->lock);
    s->repeat = on;
    if (!on)
        for (int i = 0; i < 16; i++) s->rep[i].on = 0, s->rep[i].held = 0;
    pthread_mutex_unlock(&s->lock);
}

void seq_repeat_press(struct seq *s, int pad, int note, int vel, int channel, int record) {
    seq_repeat_press_tuned(s, pad, note, 0, vel, channel, record);
}

void seq_repeat_press_tuned(struct seq *s, int pad, int note, int tune, int vel, int channel, int record) {
    if (pad < 0 || pad >= 16) return;
    pthread_mutex_lock(&s->lock);
    int first = 1;
    for (int i = 0; i < 16; i++)
        if (s->rep[i].on) first = 0;
    if (s->arp && s->arp_latch) { // Latch: a pad down with none held starts a new chord
        int any = 0;
        for (int i = 0; i < 16; i++) any |= s->rep[i].held;
        if (!any)
            for (int i = 0; i < 16; i++) s->rep[i].on = 0;
        first = 1;
        for (int i = 0; i < 16; i++)
            if (s->rep[i].on) first = 0;
    }
    if (first) s->rep_restart = 1, s->arp_pos = 0, s->arp_slide = 0; // the arpeggio starts from its first step
    if (!s->rep[pad].on) s->rep[pad].order = ++s->arp_order;
    s->rep[pad].on = 1;
    s->rep[pad].held = 1;
    s->rep[pad].note = note;
    s->rep[pad].tune = tune < -64 ? -64 : tune > 63 ? 63 : tune;
    s->rep[pad].vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
    s->rep[pad].channel = channel;
    s->rep[pad].record = record;
    pthread_mutex_unlock(&s->lock);
}

void seq_repeat_pressure(struct seq *s, int pad, int vel) {
    if (pad < 0 || pad >= 16) return;
    pthread_mutex_lock(&s->lock);
    if (s->rep[pad].on) s->rep[pad].vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
    pthread_mutex_unlock(&s->lock);
}

void seq_repeat_release(struct seq *s, int pad) {
    if (pad < 0 || pad >= 16) return;
    pthread_mutex_lock(&s->lock);
    s->rep[pad].held = 0;
    if (!(s->arp && s->arp_latch)) s->rep[pad].on = 0; // latched, it plays on
    pthread_mutex_unlock(&s->lock);
}

void seq_arp_set_latch(struct seq *s, int on) {
    pthread_mutex_lock(&s->lock);
    s->arp_latch = on;
    if (!on)
        for (int i = 0; i < 16; i++)
            if (!s->rep[i].held) s->rep[i].on = 0;
    pthread_mutex_unlock(&s->lock);
}

void seq_drain(struct seq *s) {
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->nrecq; i++) {
        struct seq_note *n = insert_note(&s->sequence[s->recq[i].seq].track[s->recq[i].track], s->recq[i].tick);
        if (!n) break;
        n->note = (uint8_t)s->recq[i].note;
        n->tune = (int8_t)s->recq[i].tune;
        n->vel = (uint8_t)s->recq[i].vel;
        n->dur = (uint16_t)(s->recq[i].dur < 1 ? 1 : s->recq[i].dur);
    }
    s->nrecq = 0;
    pthread_mutex_unlock(&s->lock);
}

// Removes one pad and parameter's moves with ticks in [from, to]. Caller holds the lock.
static void drop_autos(struct seq_track *k, int pad, int param, uint32_t from, uint32_t to) {
    int w = 0;
    for (int i = 0; i < k->nauto; i++) {
        const struct seq_auto *m = &k->autos[i];
        if (m->pad == pad && m->param == param && m->tick >= from && m->tick <= to) continue;
        k->autos[w++] = *m;
    }
    k->nauto = w;
}

int seq_record_auto(struct seq *s, int pad, int param, int value) {
    if (param < 0 || param >= AUTO_PARAMS || pad < 0 || pad >= 128) return -1;
    int recorded = -1;
    pthread_mutex_lock(&s->lock);
    if ((s->mode == SEQ_RECORDING || s->mode == SEQ_OVERDUB) && s->pos >= 0) {
        struct seq_sequence *q = &s->sequence[s->current];
        struct seq_track *k = &q->track[s->track];
        uint32_t t = (uint32_t)s->pos, last = s->auto_last[param], length = (uint32_t)seq_length(q);
        // Moving the fader again over earlier moves replaces them as it goes.
        if (s->auto_live[param]) {
            if (t >= last) {
                drop_autos(k, pad, param, last + 1, t);
            } else { // the sequence looped since the last move
                drop_autos(k, pad, param, last + 1, length);
                drop_autos(k, pad, param, 0, t);
            }
        }
        if (k->nauto == k->capauto) {
            int cap = k->capauto ? k->capauto * 2 : 64;
            struct seq_auto *grown = realloc(k->autos, (size_t)cap * sizeof *grown);
            if (grown) k->autos = grown, k->capauto = cap;
        }
        if (k->nauto < k->capauto) {
            int at = auto_lower_bound(k, t + 0.5);
            memmove(&k->autos[at + 1], &k->autos[at], (size_t)(k->nauto - at) * sizeof k->autos[0]);
            k->autos[at] = (struct seq_auto){.tick = t, .pad = (uint8_t)pad, .param = (uint8_t)param,
                                             .value = (uint8_t)(value < 0 ? 0 : value > 127 ? 127 : value)};
            k->nauto++;
            s->auto_last[param] = t;
            s->auto_live[param] = 1;
            recorded = 0;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return recorded;
}

int seq_auto_count(struct seq *s, int pad, int param) {
    pthread_mutex_lock(&s->lock);
    const struct seq_track *k = &s->sequence[s->current].track[s->track];
    int n = 0;
    for (int i = 0; i < k->nauto; i++)
        if (k->autos[i].pad == pad && (param < 0 || k->autos[i].param == param)) n++;
    pthread_mutex_unlock(&s->lock);
    return n;
}

int seq_erase_autos(struct seq *s, int pad, int param) {
    pthread_mutex_lock(&s->lock);
    struct seq_track *k = &s->sequence[s->current].track[s->track];
    int before = k->nauto;
    if (param >= 0) {
        drop_autos(k, pad, param, 0, UINT32_MAX);
    } else {
        for (int p = 0; p < AUTO_PARAMS; p++) drop_autos(k, pad, p, 0, UINT32_MAX);
    }
    int erased = before - k->nauto;
    pthread_mutex_unlock(&s->lock);
    return erased;
}

int seq_erase_range(struct seq *s, int sequence, int track, uint32_t from, uint32_t to, const unsigned char *notes) {
    if (sequence < 0 || sequence >= SEQ_COUNT) return 0;
    int erased = 0;
    pthread_mutex_lock(&s->lock);
    for (int t = 0; t < SEQ_TRACKS; t++) {
        if (track >= 0 && t != track) continue;
        struct seq_track *k = &s->sequence[sequence].track[t];
        int w = 0;
        for (int i = 0; i < k->count; i++) {
            const struct seq_note *n = &k->notes[i];
            if (n->tick >= from && n->tick < to && (!notes || notes[n->note])) erased++;
            else k->notes[w++] = *n;
        }
        k->count = w;
        w = 0;
        for (int i = 0; i < k->nauto; i++) {
            const struct seq_auto *m = &k->autos[i];
            if (m->tick >= from && m->tick < to && (!notes || notes[m->pad])) erased++;
            else k->autos[w++] = *m;
        }
        k->nauto = w;
    }
    pthread_mutex_unlock(&s->lock);
    return erased;
}

void seq_sync_tempo_tap(struct seq *s, int tempo10) {
    pthread_mutex_lock(&s->lock);
    s->sequence[s->current].tempo10 = tempo10 < 300 ? 300 : tempo10 > 3000 ? 3000 : tempo10;
    pthread_mutex_unlock(&s->lock);
}

// ---- Bars and tempo on a whole sequence ----

static int auto_by_tick(const void *a, const void *b) {
    uint32_t x = ((const struct seq_auto *)a)->tick, y = ((const struct seq_auto *)b)->tick;
    return x < y ? -1 : x > y;
}

// Copies the track's events in [from, to) to start at `dest`, keeping everything in tick order.
// -1 when memory runs out (what was copied stays).
static int copy_span(struct seq_track *k, uint32_t from, uint32_t to, uint32_t dest) {
    int n = 0, na = 0;
    for (int i = 0; i < k->count; i++) n += k->notes[i].tick >= from && k->notes[i].tick < to;
    for (int i = 0; i < k->nauto; i++) na += k->autos[i].tick >= from && k->autos[i].tick < to;
    if (k->count + n > k->cap) {
        int cap = k->count + n + 16;
        struct seq_note *g = realloc(k->notes, (size_t)cap * sizeof *g);
        if (!g) return -1;
        k->notes = g, k->cap = cap;
    }
    if (k->nauto + na > k->capauto) {
        int cap = k->nauto + na + 16;
        struct seq_auto *g = realloc(k->autos, (size_t)cap * sizeof *g);
        if (!g) return -1;
        k->autos = g, k->capauto = cap;
    }
    int end = k->count;
    for (int i = 0; i < end; i++)
        if (k->notes[i].tick >= from && k->notes[i].tick < to) {
            struct seq_note c = k->notes[i];
            c.tick = c.tick - from + dest;
            c.fresh = 0;
            k->notes[k->count++] = c;
        }
    end = k->nauto;
    for (int i = 0; i < end; i++)
        if (k->autos[i].tick >= from && k->autos[i].tick < to) {
            struct seq_auto c = k->autos[i];
            c.tick = c.tick - from + dest;
            k->autos[k->nauto++] = c;
        }
    qsort(k->notes, (size_t)k->count, sizeof k->notes[0], by_tick);
    qsort(k->autos, (size_t)k->nauto, sizeof k->autos[0], auto_by_tick);
    return 0;
}

static void cut_from(struct seq_track *k, uint32_t from) {
    while (k->count > 0 && k->notes[k->count - 1].tick >= from) k->count--;
    while (k->nauto > 0 && k->autos[k->nauto - 1].tick >= from) k->nauto--;
}

int seq_events_from(const struct seq_sequence *q, uint32_t from) {
    int n = 0;
    for (int t = 0; t < SEQ_TRACKS; t++) {
        const struct seq_track *k = &q->track[t];
        for (int i = 0; i < k->count; i++) n += k->notes[i].tick >= from;
        for (int i = 0; i < k->nauto; i++) n += k->autos[i].tick >= from;
    }
    return n;
}

int seq_set_bars(struct seq *s, int sequence, int bars, int dup) {
    if (sequence < 0 || sequence >= SEQ_COUNT) return -1;
    bars = bars < 1 ? 1 : bars > SEQ_MAX_BARS ? SEQ_MAX_BARS : bars;
    pthread_mutex_lock(&s->lock);
    struct seq_sequence *q = &s->sequence[sequence];
    uint32_t bar = (uint32_t)seq_bar_ticks(q), old = (uint32_t)q->bars * bar, len = (uint32_t)bars * bar;
    int rc = 0;
    for (int t = 0; t < SEQ_TRACKS; t++) {
        struct seq_track *k = &q->track[t];
        if (len < old || !dup) cut_from(k, len < old ? len : old); // shorter: past the end goes
        if (len > old && dup) {
            cut_from(k, old); // anything left past the old end gives way to the copies
            // New bar n (counting from the old end) repeats old bar n, round and round.
            for (uint32_t at = old; at < len && rc == 0; at += old)
                rc = copy_span(k, 0, len - at < old ? len - at : old, at);
        }
    }
    q->bars = bars;
    if (q->loop_bar > bars) q->loop_bar = bars;
    if (s->pos >= (double)len) s->pos = 0;
    pthread_mutex_unlock(&s->lock);
    return rc;
}

int seq_tempo_scale(struct seq *s, int sequence, int twice, int comp) {
    if (sequence < 0 || sequence >= SEQ_COUNT) return -1;
    pthread_mutex_lock(&s->lock);
    struct seq_sequence *q = &s->sequence[sequence];
    int tempo = twice ? q->tempo10 * 2 : q->tempo10 / 2;
    if (tempo < 300 || tempo > 3000 || (twice && comp && q->bars * 2 > SEQ_MAX_BARS)) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    q->tempo10 = tempo;
    int rc = 0;
    if (comp && twice) { // every event twice as many ticks along: the same speed, twice the bars
        for (int t = 0; t < SEQ_TRACKS; t++) {
            struct seq_track *k = &q->track[t];
            for (int i = 0; i < k->count; i++) {
                k->notes[i].tick *= 2;
                k->notes[i].dur = (uint16_t)(k->notes[i].dur > 32767 ? 65535 : k->notes[i].dur * 2);
            }
            for (int i = 0; i < k->nauto; i++) k->autos[i].tick *= 2;
        }
        q->bars *= 2;
        if (q->loop_bar > 0) q->loop_bar = (q->loop_bar - 1) * 2 + 1;
        s->pos *= 2;
    } else if (comp) { // half as many ticks: the same speed in the first half, repeated in the second
        uint32_t len = (uint32_t)seq_length(q), half = len / 2;
        for (int t = 0; t < SEQ_TRACKS && rc == 0; t++) {
            struct seq_track *k = &q->track[t];
            for (int i = 0; i < k->count; i++) {
                k->notes[i].tick /= 2;
                k->notes[i].dur = (uint16_t)(k->notes[i].dur > 1 ? k->notes[i].dur / 2 : 1);
            }
            for (int i = 0; i < k->nauto; i++) k->autos[i].tick /= 2;
            cut_from(k, half);
            rc = copy_span(k, 0, half, half);
        }
        s->pos /= 2;
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}

void seq_locate_bar(struct seq *s, int dir) {
    pthread_mutex_lock(&s->lock);
    const struct seq_sequence *q = &s->sequence[s->current];
    double bar = seq_bar_ticks(q), len = seq_length(q), pos = s->pos < 0 ? 0 : s->pos;
    double at = floor(pos / bar) * bar;
    if (dir < 0) at = pos - at > 1 ? at : at - bar; // back to this bar's start, or from it the one before
    else at += bar;
    s->pos = at < 0 ? 0 : at >= len ? len - bar : at;
    pthread_mutex_unlock(&s->lock);
}

void seq_song_play(struct seq *s, int song, int step) {
    if (song < 0 || song >= SEQ_SONGS) return;
    pthread_mutex_lock(&s->lock);
    const struct seq_song *g = &s->song[song];
    if (g->nsteps > 0 && s->mode == SEQ_STOPPED) {
        step = step < 0 ? 0 : step >= g->nsteps ? 0 : step;
        s->song_on = 1, s->song_cur = song, s->song_step = step, s->song_rep = 0;
        s->current = g->step[step].seq;
        s->next = -1;
        s->pos = 0;
        s->mode = SEQ_PLAYING;
        if (s->sync_send) send1(s, 0xfa);
        send_programs(s);
    }
    pthread_mutex_unlock(&s->lock);
}

int seq_song_export(struct seq *s, int song, int to) {
    if (song < 0 || song >= SEQ_SONGS || to < 0 || to >= SEQ_COUNT) return -1;
    pthread_mutex_lock(&s->lock);
    const struct seq_song *g = &s->song[song];
    if (g->nsteps <= 0) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    // Its length: every pass end to end, in the first step's bars.
    const struct seq_sequence *first = &s->sequence[g->step[0].seq];
    long total = 0;
    for (int i = 0; i < g->nsteps; i++) total += (long)seq_length(&s->sequence[g->step[i].seq]) * (g->step[i].reps ? g->step[i].reps : 1);
    long bar = seq_bar_ticks(first), bars = (total + bar - 1) / bar;
    if (bars > SEQ_MAX_BARS || to == g->step[0].seq) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    struct seq_sequence *q = &s->sequence[to];
    for (int t = 0; t < SEQ_TRACKS; t++) q->track[t].count = q->track[t].nauto = 0;
    q->bars = (int)bars, q->sig_num = first->sig_num, q->sig_den = first->sig_den;
    q->tempo10 = first->tempo10, q->loop_bar = 0;
    snprintf(q->name, sizeof q->name, "Song%02d", song + 1);
    int rc = 0, named[SEQ_TRACKS] = {0};
    uint32_t at = 0;
    for (int i = 0; i < g->nsteps && rc == 0; i++) {
        const struct seq_sequence *src = &s->sequence[g->step[i].seq];
        for (int r = 0; r < (g->step[i].reps ? g->step[i].reps : 1) && rc == 0; r++) {
            for (int t = 0; t < SEQ_TRACKS && rc == 0; t++) {
                const struct seq_track *from = &src->track[t];
                struct seq_track *k = &q->track[t];
                if (!from->count && !from->nauto) continue;
                if (!named[t]) { // the track's settings from the first step that uses it
                    struct seq_note *notes = k->notes;
                    struct seq_auto *autos = k->autos;
                    int cap = k->cap, capauto = k->capauto;
                    *k = *from;
                    k->notes = notes, k->autos = autos, k->cap = cap, k->capauto = capauto;
                    k->count = k->nauto = 0;
                    named[t] = 1;
                }
                // Append this pass's events, shifted to where it starts.
                if (k->count + from->count > k->cap) {
                    int cap = k->count + from->count + 64;
                    struct seq_note *n = realloc(k->notes, (size_t)cap * sizeof *n);
                    if (!n) { rc = -1; break; }
                    k->notes = n, k->cap = cap;
                }
                if (k->nauto + from->nauto > k->capauto) {
                    int cap = k->nauto + from->nauto + 64;
                    struct seq_auto *n = realloc(k->autos, (size_t)cap * sizeof *n);
                    if (!n) { rc = -1; break; }
                    k->autos = n, k->capauto = cap;
                }
                for (int j = 0; j < from->count; j++) {
                    struct seq_note c = from->notes[j];
                    if (c.tick >= (uint32_t)seq_length(src)) continue; // past its own end: not heard
                    c.tick += at, c.fresh = 0;
                    k->notes[k->count++] = c;
                }
                for (int j = 0; j < from->nauto; j++) {
                    struct seq_auto c = from->autos[j];
                    if (c.tick >= (uint32_t)seq_length(src)) continue;
                    c.tick += at;
                    k->autos[k->nauto++] = c;
                }
            }
            at += (uint32_t)seq_length(src);
        }
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}

// Copy Sequence: the whole of one sequence (its tracks, notes, fader moves, bars, tempo and
// name) over another. 0, or -1 when out of memory (the copy then has what fitted).
int seq_copy(struct seq *s, int from, int to) {
    if (from < 0 || from >= SEQ_COUNT || to < 0 || to >= SEQ_COUNT || from == to) return -1;
    pthread_mutex_lock(&s->lock);
    const struct seq_sequence *src = &s->sequence[from];
    struct seq_sequence *q = &s->sequence[to];
    int rc = 0;
    memcpy(q->name, src->name, sizeof q->name);
    q->bars = src->bars, q->sig_num = src->sig_num, q->sig_den = src->sig_den;
    q->tempo10 = src->tempo10, q->loop_bar = src->loop_bar;
    for (int t = 0; t < SEQ_TRACKS; t++) {
        const struct seq_track *a = &src->track[t];
        struct seq_track *k = &q->track[t];
        struct seq_note *notes = k->notes;
        struct seq_auto *autos = k->autos;
        int cap = k->cap, capauto = k->capauto;
        if (a->count > cap) {
            struct seq_note *n = realloc(notes, (size_t)a->count * sizeof *n);
            if (n) notes = n, cap = a->count;
        }
        if (a->nauto > capauto) {
            struct seq_auto *n = realloc(autos, (size_t)a->nauto * sizeof *n);
            if (n) autos = n, capauto = a->nauto;
        }
        *k = *a; // the track's settings
        k->notes = notes, k->autos = autos, k->cap = cap, k->capauto = capauto;
        k->count = a->count <= cap ? a->count : cap;
        k->nauto = a->nauto <= capauto ? a->nauto : capauto;
        if (k->count < a->count || k->nauto < a->nauto) rc = -1;
        if (k->count) memcpy(k->notes, a->notes, (size_t)k->count * sizeof *k->notes);
        if (k->nauto) memcpy(k->autos, a->autos, (size_t)k->nauto * sizeof *k->autos);
        for (int j = 0; j < k->count; j++) k->notes[j].fresh = 0;
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}
