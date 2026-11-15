#include "engine.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI 3.14159265358979f
#define SQRT2 1.41421356f
#define MIN_RELEASE_MS 5
#define FADE_FRAMES 132 // 3 ms, so a cut voice does not click
#define SILENT 0.001f

enum { V_FREE, V_ATTACK, V_HOLD, V_DECAY, V_RELEASE };

struct sample *sample_new(int32_t frames, int channels, int rate) {
    if (frames <= 0 || channels < 1 || channels > 2 || rate <= 0) return 0;
    struct sample *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->data = malloc((size_t)frames * (size_t)channels * sizeof(float));
    if (!s->data) {
        free(s);
        return 0;
    }
    s->frames = frames;
    s->channels = channels;
    s->rate = rate;
    return s;
}

void sample_free(struct sample *s) {
    if (!s) return;
    free(s->data);
    free(s);
}

#define CLICK_FRAMES 1300 // 30 ms

static float click_data[2][CLICK_FRAMES];
static struct sample click_sample[2];

// The MPC3000's click, rebuilt from its shape as measured from the emulated MPC3000: a
// straight ramp up over 1.08 ms that stops dead, smoothed by its output filter (a 2-pole
// low-pass near 5.5 kHz, which gives the small undershoot). The first beat of a bar is
// the same click about 4.1 times louder.
#define CLICK_RAMP_MS 1.083f
#define CLICK_FILTER_HZ 5500.0f
#define CLICK_FILTER_Q 0.8f
#define CLICK_ACCENT 1.0f // the accent peaks at full scale
#define CLICK_RATIO 4.1f

static void make_clicks(void) {
    static int made;
    if (made) return;
    made = 1;
    int ramp = (int)(CLICK_RAMP_MS * ENGINE_RATE / 1000.0f + 0.5f);
    float g = tanf(PI * CLICK_FILTER_HZ / ENGINE_RATE), k = 1.0f / CLICK_FILTER_Q;
    float shape[CLICK_FRAMES], s1 = 0, s2 = 0, top = 0;
    for (int i = 0; i < CLICK_FRAMES; i++) {
        float x = i < ramp ? (float)(i + 1) / (float)ramp : 0.0f;
        float h = 1.0f / (1.0f + g * (g + k));
        float hp = h * (x - (g + k) * s1 - s2);
        float bp = g * hp + s1;
        float lp = g * bp + s2;
        s1 = g * hp + bp;
        s2 = g * bp + lp;
        shape[i] = lp;
        if (fabsf(lp) > top) top = fabsf(lp);
    }
    for (int a = 0; a < 2; a++) {
        float level = a ? CLICK_ACCENT : CLICK_ACCENT / CLICK_RATIO;
        for (int i = 0; i < CLICK_FRAMES; i++) click_data[a][i] = shape[i] / top * level;
        click_sample[a] = (struct sample){.data = click_data[a], .frames = CLICK_FRAMES, .channels = 1, .rate = ENGINE_RATE};
    }
}

void engine_init(struct engine *e) {
    memset(e, 0, sizeof *e);
    atomic_store(&e->fx_pad, -1);
    make_clicks();
    e->click[0] = &click_sample[0];
    e->click[1] = &click_sample[1];
}

void engine_set_click(struct engine *e, int accent, const struct sample *s) {
    if (s) e->click[accent ? 1 : 0] = s;
}

int engine_set_pad(struct engine *e, int pad, const struct sample *s, const struct pad_params *p) {
    struct event ev = {.type = EV_SET_PAD, .pad = pad, .s = s};
    if (p) ev.p = *p;
    return engine_send(e, &ev);
}

int engine_send(struct engine *e, const struct event *ev) {
    unsigned head = atomic_load_explicit(&e->head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&e->tail, memory_order_acquire);
    if (head - tail >= ENGINE_RING) return -1;
    e->ring[head % ENGINE_RING] = *ev;
    atomic_store_explicit(&e->head, head + 1, memory_order_release);
    return 0;
}

int engine_note_on(struct engine *e, int pad, int vel, const struct sample *s, const struct pad_params *p) {
    struct event ev = {.type = EV_ON, .pad = pad, .vel = vel, .s = s, .p = *p};
    return engine_send(e, &ev);
}

int engine_note_off(struct engine *e, int pad) {
    return engine_note_off_key(e, pad, ENGINE_KEY_ANY);
}

int engine_note_on_key(struct engine *e, int pad, int vel, int key, const struct sample *s, const struct pad_params *p) {
    struct event ev = {.type = EV_ON, .pad = pad, .vel = vel, .key = key, .s = s, .p = *p};
    return engine_send(e, &ev);
}

static int level_cut(int level) {
    return 100 - (level < 0 ? 0 : level > 100 ? 100 : level);
}

int engine_note_on_layers(struct engine *e, int pad, int vel, int key, const struct sample *s, const struct sample *s2,
                          const struct pad_params *p, int track, int level, int pan) {
    struct event ev = {.type = EV_ON, .pad = pad, .vel = vel, .key = key, .s = s, .s2 = s2, .p = *p,
                       .track = track, .track_cut = level_cut(level), .track_pan = pan};
    return engine_send(e, &ev);
}

int engine_track_eq(struct engine *e, int track, int on, const float c[3][5]) {
    struct event ev = {.type = EV_EQ, .track = track, .track_cut = on};
    if (c) memcpy(ev.eq, c, sizeof ev.eq);
    return engine_send(e, &ev);
}

int engine_track_sends(struct engine *e, int track, const unsigned char *levels) {
    struct event ev = {.type = EV_SEND, .track = track};
    for (int i = 0; i < ENGINE_SENDS; i++) ev.eq[i / 5][i % 5] = levels[i] / 100.0f;
    return engine_send(e, &ev);
}

int engine_track_mix(struct engine *e, int track, int level, int pan) {
    struct event ev = {.type = EV_MIX, .track = track, .track_cut = level_cut(level), .track_pan = pan};
    return engine_send(e, &ev);
}

int engine_set_pad_layers(struct engine *e, int pad, const struct sample *s, const struct sample *s2,
                          const struct pad_params *p) {
    struct event ev = {.type = EV_SET_PAD, .pad = pad, .s = s, .s2 = s2};
    if (p) ev.p = *p;
    return engine_send(e, &ev);
}

// A note-off is never dropped, or its note would hold forever: with the ring full (a burst of
// pad updates, say), it waits for the audio thread to make room, up to a quarter second.
int engine_note_off_key(struct engine *e, int pad, int key) {
    struct event ev = {.type = EV_OFF, .pad = pad, .key = key};
    for (int tries = 0; tries < 250; tries++) {
        if (engine_send(e, &ev) == 0) return 0;
        struct timespec t = {0, 1000000};
        nanosleep(&t, 0);
    }
    return engine_send(e, &ev);
}

void engine_set_bend(struct engine *e, int value) {
    atomic_store_explicit(&e->bend, value < -8192 ? -8192 : value > 8191 ? 8191 : value, memory_order_relaxed);
}

void engine_set_mod(struct engine *e, int value) {
    atomic_store_explicit(&e->mod, value < 0 ? 0 : value > 127 ? 127 : value, memory_order_relaxed);
}

int engine_update(struct engine *e, int pad, const struct pad_params *p) {
    struct event ev = {.type = EV_UPDATE, .pad = pad, .p = *p};
    return engine_send(e, &ev);
}

int engine_kill_all(struct engine *e) {
    struct event ev = {.type = EV_KILL_ALL};
    return engine_send(e, &ev);
}

// The key map goes into a staging copy (one the audio thread has finished with), and the
// event points the audio thread at it. With every copy still in flight, it waits for one, up
// to a quarter second.
int engine_set_keymap(struct engine *e, int slot, int at, const struct keymap *km) {
    struct event ev = {.type = EV_KEYMAP, .pad = slot, .key = km ? at : -1};
    if (km) {
        int free_at = -1;
        for (int tries = 0; tries < 250 && free_at < 0; tries++) {
            uint32_t ack = atomic_load_explicit(&e->km_ack, memory_order_acquire);
            for (int i = 0; i < ENGINE_KM_STAGE && free_at < 0; i++)
                if (!e->km_stage_seq[i] || (int32_t)(ack - e->km_stage_seq[i]) >= 0) free_at = i;
            if (free_at < 0) {
                struct timespec t = {0, 1000000};
                nanosleep(&t, 0);
            }
        }
        if (free_at < 0) return -1;
        e->km_stage[free_at] = *km;
        ev.km = &e->km_stage[free_at];
        ev.seq = ++e->km_seq;
        e->km_stage_seq[free_at] = ev.seq;
    }
    return engine_send(e, &ev);
}

void engine_retire(struct engine *e, struct sample *s) {
    if (!s) return;
    int slot = -1;
    for (int i = 0; i < ENGINE_RETIRE_MAX && slot < 0; i++)
        if (!e->retired[i].s) slot = i;
    if (slot < 0) return;
    uint32_t seq = ++e->retire_seq;
    struct event ev = {.type = EV_RETIRE, .seq = seq, .s = s};
    if (engine_send(e, &ev) != 0) return;
    e->retired[slot].s = s;
    e->retired[slot].seq = seq;
}

void engine_reap(struct engine *e) {
    uint32_t ack = atomic_load_explicit(&e->retire_ack, memory_order_acquire);
    for (int i = 0; i < ENGINE_RETIRE_MAX; i++) {
        if (e->retired[i].s && (int32_t)(ack - e->retired[i].seq) >= 0) {
            sample_free(e->retired[i].s);
            e->retired[i].s = 0;
        }
    }
}

static float env_coef(int ms) {
    if (ms < 1) ms = 1;
    return (float)exp(log(SILENT) / ((double)ms * ENGINE_RATE / 1000.0));
}

static void start_fade(struct voice *v) {
    if (v->stage != V_FREE) v->fading = 1;
}

static void start_release(struct voice *v) {
    if (v->stage == V_FREE || v->stage == V_RELEASE) return;
    int r = v->p.synth ? v->p.release_ms : v->p.decay_ms;
    int ms = r > MIN_RELEASE_MS ? r : MIN_RELEASE_MS;
    v->release_coef = env_coef(ms);
    v->stage = V_RELEASE;
}

// What the second layer plays: the sound's settings with the layer's level, tuning and trim.
static void layer2_params(const struct pad_params *p, struct pad_params *o) {
    *o = *p;
    o->volume = p->l2_volume;
    o->semi = p->l2_semi;
    o->fine = p->l2_fine;
    o->start = p->l2_start;
    o->end = p->l2_end;
    o->loop = p->l2_loop;
    o->loop_on = p->l2_loop_on;
}

// layer: the second layer of a note already started, so it cuts nothing and leaves portamento be.
static struct voice *start_voice(struct engine *e, const struct event *ev, int offset, int layer) {
    if (!ev->s) return 0;
    const struct pad_params *p = &ev->p;
    // A slide: this pad's voice still sounding goes on to the new key, gliding there.
    if (ev->glide_ms > 0 && ev->pad >= 0)
        for (int i = 0; i < ENGINE_VOICES; i++) {
            struct voice *v = &e->voice[i];
            if (v->stage == V_FREE || v->stage == V_RELEASE || v->fading || v->pad != ev->pad || v->layer != layer) continue;
            v->glide += (float)(v->key - ev->key);
            v->glide_ms = ev->glide_ms;
            v->key = ev->key;
            if (!layer && ev->pad < ENGINE_SLOTS) e->last_key[ev->pad] = (float)ev->key, e->has_last[ev->pad] = 1;
            return v;
        }
    // A looping one-shot pressed on a pad is a toggle: the next hit stops it. Played by the
    // sequencer it starts over instead, every pass. (An S600 note holds until released.)
    if (p->loop_on && !p->gate && !p->synth && !layer) {
        int stopped = 0;
        for (int i = 0; i < ENGINE_VOICES; i++) {
            struct voice *v = &e->voice[i];
            if (v->stage != V_FREE && v->pad == ev->pad && !v->fading && v->stage != V_RELEASE) {
                if (ev->restart) start_fade(v); // out quickly, under the new start
                else start_release(v);
                stopped = 1;
            }
        }
        if (stopped && !ev->restart) return 0;
    }
    // Mono Legato: a note while this pad's voice still sounds (not yet released) takes that
    // voice to the new pitch, gliding with Portamento, its envelopes and LFO carrying on.
    if (p->synth && p->mono && p->legato && ev->pad >= 0)
        for (int i = 0; i < ENGINE_VOICES; i++) {
            struct voice *v = &e->voice[i];
            if (v->stage == V_FREE || v->stage == V_RELEASE || v->fading || v->pad != ev->pad || v->layer != layer) continue;
            if (!layer) {
                v->glide = p->porta_ms > 0 ? v->glide + (float)(v->key - ev->key) : 0;
                if (ev->pad < ENGINE_SLOTS) e->last_key[ev->pad] = (float)ev->key, e->has_last[ev->pad] = 1;
            }
            v->key = ev->key;
            return v;
        }
    for (int i = 0; i < ENGINE_VOICES && !layer; i++) {
        struct voice *v = &e->voice[i];
        if (v->stage == V_FREE) continue;
        if (p->mono && v->pad == ev->pad) start_fade(v);
        // Mute groups work within one program, so two tracks' kits do not cut each other.
        if (p->mute_group > 0 && v->pad != ev->pad && v->p.mute_group == p->mute_group && v->pad >= 0 && ev->pad >= 0 &&
            v->pad / ENGINE_PROGRAM_PADS == ev->pad / ENGINE_PROGRAM_PADS)
            start_fade(v);
    }
    struct voice *v = 0;
    for (int i = 0; i < ENGINE_VOICES && !v; i++)
        if (e->voice[i].stage == V_FREE) v = &e->voice[i];
    if (!v) {
        v = &e->voice[0];
        for (int i = 1; i < ENGINE_VOICES; i++)
            if ((int32_t)(e->voice[i].age - v->age) < 0) v = &e->voice[i];
    }
    memset(v, 0, sizeof *v);
    v->pad = ev->pad;
    v->key = ev->key;
    v->layer = layer;
    v->track = ev->track;
    v->track_cut = ev->track_cut;
    v->track_pan = ev->track_pan;
    v->lfo_rand = 0x9e3779b9u * (e->age + 1);
    v->age = e->age++;
    if (p->synth && !layer && ev->pad >= 0 && ev->pad < ENGINE_SLOTS) { // portamento slides from the last note
        if (p->porta_ms > 0 && e->has_last[ev->pad]) v->glide = e->last_key[ev->pad] - (float)ev->key;
        e->last_key[ev->pad] = (float)ev->key;
        e->has_last[ev->pad] = 1;
    }
    v->s = ev->s;
    v->p = *p;
    float hit = (float)(ev->vel < 1 ? 1 : ev->vel > 127 ? 127 : ev->vel);
    // Vel Mod Sft St: the softest hit starts the full time later, the hardest at Zone Start.
    double later = (double)p->soft_start_ms * ev->s->rate / 1000.0 * (127.0f - hit) / 126.0f;
    if (p->reverse) {
        v->pos = (double)p->end - 1.0 - later;
        if (v->pos < p->start) v->pos = p->start;
    } else {
        v->pos = (double)p->start + later;
        if (v->pos > p->end - 1) v->pos = p->end - 1;
    }
    float sens = (float)p->vel_sens / 100.0f;
    v->vel_gain = 1.0f - sens + sens * (hit / 127.0f);
    v->fade = 1.0f;
    v->delay = offset > 0 ? offset : 0;
    // Dcy Md End holds the level and starts the decay so that it finishes with the sample.
    int after_attack = (p->decay_ms > 0 && !p->gate && !p->decay_end) ? V_DECAY : V_HOLD;
    if (p->synth) after_attack = V_DECAY; // falls to the sustain level
    v->decay_coef = env_coef(p->decay_ms);
    // Vel Mod Attack: a hard hit shortens the attack, by all of it at 100.
    float attack = (float)p->attack_ms * (1.0f - (float)p->vel_attack / 100.0f * hit / 127.0f);
    if (attack >= 1.0f) {
        v->stage = V_ATTACK;
        v->attack_step = 1000.0f / (attack * ENGINE_RATE);
    } else {
        v->stage = after_attack;
        v->env = 1.0f;
    }
    if (p->flt_attack_ms <= 0) {
        v->fenv = 1.0f;
        v->fenv_falling = 1;
    }
    return v;
}

// A MULTI instrument's note: the zone its note and velocity fall in gives the sample, its trim
// and loop and tuning (from the zone's root note); key tracking moves pan, filter, level and
// envelope times with the note. 0 when no zone answers: the note is silent.
static int keymap_note(const struct engine *e, const struct event *ev, struct event *out) {
    const struct keymap *km = &e->km[e->km_of[ev->pad] - 1];
    int note = 60 + ev->key, vel = ev->vel < 1 ? 1 : ev->vel > 127 ? 127 : ev->vel;
    const struct km_zone *z = 0;
    for (int i = 0; i < km->n && !z; i++)
        if (km->z[i].s && note >= km->z[i].lo && note <= km->z[i].hi && vel >= km->z[i].vlo && vel <= km->z[i].vhi) z = &km->z[i];
    if (!z) return 0;
    *out = *ev;
    struct pad_params *p = &out->p;
    out->s = z->s;
    out->s2 = 0;
    p->layers = 1;
    p->start = z->start, p->end = z->end, p->loop = z->loop, p->loop_on = z->loop_on;
    p->semi += 60 - z->root + z->semi;
    p->fine += z->fine;
    double from = note - 60, oct = from / 12.0;
    int pan = p->pan + z->pan + (int)lround(km->kt_pan * from / 24.0);
    p->pan = pan < -50 ? -50 : pan > 50 ? 50 : pan;
    // The cutoff's scale is 20 Hz to 20 kHz over 0..100: an octave is 100 / log2(1000) of it.
    p->cutoff += (int)lround(km->kt_filter / 100.0 * from * 100.0 / (12.0 * 9.9658));
    double db = km->kt_level / 100.0 * 6.0 * oct; // the level is volume squared: a quarter of the dB in volume
    double vol = p->volume * (z->level / 100.0) * pow(10.0, db / 40.0);
    p->volume = (int32_t)lround(vol < 0 ? 0 : vol > 200 ? 200 : vol);
    if (km->kt_env) {
        double f = pow(2.0, -oct * km->kt_env / 100.0);
        p->decay_ms = (int32_t)lround(p->decay_ms * f), p->release_ms = (int32_t)lround(p->release_ms * f);
        p->flt_decay_ms = (int32_t)lround(p->flt_decay_ms * f), p->flt_release_ms = (int32_t)lround(p->flt_release_ms * f);
    }
    return 1;
}

// A pad, and the pad set as its Also Play, together. Only one step, so two pads that name
// each other cannot set each other off forever.
static void note_on(struct engine *e, const struct event *ev, int offset) {
    struct event multi;
    if (ev->pad >= ENGINE_S600 && ev->pad < ENGINE_SLOTS && e->km_of[ev->pad] && ev->p.synth) {
        if (!keymap_note(e, ev, &multi)) return;
        ev = &multi;
    }
    struct voice *first = start_voice(e, ev, offset, 0);
    if (first && ev->p.synth && ev->p.layers == 2 && ev->s2) { // the S600's second layer
        struct event two = *ev;
        two.s = ev->s2;
        layer2_params(&ev->p, &two.p);
        struct voice *second = start_voice(e, &two, offset, 1);
        if (second) second->glide = first->glide;
    }
    if (ev->pad < 0 || ev->p.also_pad <= 0 || ev->p.also_pad > ENGINE_PROGRAM_PADS) return;
    int also = ev->pad - ev->pad % ENGINE_PROGRAM_PADS + ev->p.also_pad - 1; // in the same program
    if (also == ev->pad || !e->pad[also].s) return;
    struct event other = {.type = EV_ON, .pad = also, .vel = ev->vel, .s = e->pad[also].s, .p = e->pad[also].p,
                          .track = ev->track, .track_cut = ev->track_cut, .track_pan = ev->track_pan};
    start_voice(e, &other, offset, 0);
}

static void process_events(struct engine *e) {
    unsigned tail = atomic_load_explicit(&e->tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&e->head, memory_order_acquire);
    while (tail != head) {
        const struct event *ev = &e->ring[tail % ENGINE_RING];
        switch (ev->type) {
        case EV_ON:
            note_on(e, ev, 0);
            break;
        case EV_SET_PAD:
            if (ev->pad >= 0 && ev->pad < ENGINE_SLOTS) {
                e->pad[ev->pad].s = ev->s;
                e->pad[ev->pad].s2 = ev->s2;
                e->pad[ev->pad].p = ev->p;
                if (!ev->s) e->km_of[ev->pad] = 0; // an emptied pad plays no key map either
            }
            break;
        case EV_KEYMAP:
            if (ev->pad >= 0 && ev->pad < ENGINE_SLOTS) {
                if (ev->km && ev->key >= 0 && ev->key < ENGINE_KEYMAPS) {
                    e->km[ev->key] = *ev->km;
                    e->km_of[ev->pad] = (unsigned char)(ev->key + 1);
                } else {
                    e->km_of[ev->pad] = 0;
                }
            }
            if (ev->km) atomic_store_explicit(&e->km_ack, ev->seq, memory_order_release);
            break;
        case EV_OFF:
            for (int i = 0; i < ENGINE_VOICES; i++) {
                struct voice *v = &e->voice[i];
                if (v->stage != V_FREE && v->pad == ev->pad && v->p.gate &&
                    (ev->key == ENGINE_KEY_ANY || ev->key == v->key))
                    start_release(v);
            }
            break;
        case EV_UPDATE:
            for (int i = 0; i < ENGINE_VOICES; i++) {
                struct voice *v = &e->voice[i];
                if (v->stage != V_FREE && v->pad == ev->pad) {
                    if (v->layer) layer2_params(&ev->p, &v->p);
                    else v->p = ev->p;
                }
            }
            if (ev->pad >= 0 && ev->pad < ENGINE_SLOTS) e->pad[ev->pad].p = ev->p;
            break;
        case EV_MIX:
            for (int i = 0; i < ENGINE_VOICES; i++) {
                struct voice *v = &e->voice[i];
                if (v->stage != V_FREE && ev->track > 0 && v->track == ev->track) {
                    v->track_cut = ev->track_cut;
                    v->track_pan = ev->track_pan;
                }
            }
            break;
        case EV_SEND:
            if (ev->track > 0 && ev->track < ENGINE_TRACKS)
                for (int i = 0; i < ENGINE_SENDS; i++) e->send[ev->track][i] = ev->eq[i / 5][i % 5];
            break;
        case EV_EQ: { // a track's EQ: its bus, kept or taken; off frees it
            int at = -1, free_bus = -1;
            for (int b = 0; b < ENGINE_BUSES; b++) {
                if (e->bus[b].track == ev->track) at = b;
                if (!e->bus[b].track && free_bus < 0) free_bus = b;
            }
            if (!ev->track_cut) {
                if (at >= 0) e->bus[at].track = 0;
            } else {
                if (at < 0 && free_bus >= 0) { // a fresh bus starts from silence
                    at = free_bus;
                    memset(e->bus[at].z, 0, sizeof e->bus[at].z);
                    e->bus[at].track = ev->track;
                }
                if (at >= 0) memcpy(e->bus[at].c, ev->eq, sizeof e->bus[at].c);
            }
            break;
        }
        case EV_KILL_ALL:
            for (int i = 0; i < ENGINE_VOICES; i++) start_fade(&e->voice[i]);
            break;
        case EV_RETIRE:
            for (int i = 0; i < ENGINE_VOICES; i++)
                if (e->voice[i].s == ev->s) e->voice[i].stage = V_FREE;
            for (int i = 0; i < ENGINE_SLOTS; i++) {
                if (e->pad[i].s == ev->s) e->pad[i].s = 0;
                if (e->pad[i].s2 == ev->s) e->pad[i].s2 = 0;
            }
            for (int k = 0; k < ENGINE_KEYMAPS; k++) // a key map's zone playing it falls silent
                for (int i = 0; i < e->km[k].n; i++)
                    if (e->km[k].z[i].s == ev->s) e->km[k].z[i].s = 0;
            atomic_store_explicit(&e->retire_ack, ev->seq, memory_order_release);
            break;
        }
        tail++;
    }
    atomic_store_explicit(&e->tail, tail, memory_order_release);
}

void engine_play_pad(struct engine *e, int pad, int vel, int offset) {
    engine_play_pad_key(e, pad, vel, 0, offset);
}

void engine_play_pad_key(struct engine *e, int pad, int vel, int key, int offset) {
    engine_play_pad_mix(e, pad, vel, key, offset, 0, 100, 0);
}

void engine_play_pad_mix(struct engine *e, int pad, int vel, int key, int offset, int track, int level, int pan) {
    if (pad < 0 || pad >= ENGINE_SLOTS || !e->pad[pad].s) return;
    struct event ev = {.type = EV_ON, .pad = pad, .vel = vel, .key = key, .s = e->pad[pad].s, .s2 = e->pad[pad].s2,
                       .p = e->pad[pad].p, .track = track, .track_cut = level_cut(level), .track_pan = pan, .restart = 1};
    note_on(e, &ev, offset);
}

void engine_play_pad_glide(struct engine *e, int pad, int vel, int key, int offset, int track, int level, int pan, int glide_ms) {
    if (pad < 0 || pad >= ENGINE_SLOTS || !e->pad[pad].s) return;
    struct event ev = {.type = EV_ON, .pad = pad, .vel = vel, .key = key, .s = e->pad[pad].s, .s2 = e->pad[pad].s2,
                       .p = e->pad[pad].p, .track = track, .track_cut = level_cut(level), .track_pan = pan, .restart = 1,
                       .glide_ms = glide_ms};
    note_on(e, &ev, offset);
}

void engine_apply_param(struct engine *e, int pad, int param, int value) {
    if (pad < 0 || pad >= ENGINE_SLOTS) return;
    // Same order and ranges as the Vol, Filter and Tune pages: volume, pan, cutoff, reson, semi, fine.
    static const struct {
        size_t off;
        int min, max;
    } map[] = {
        {offsetof(struct pad_params, volume), 0, 100},  {offsetof(struct pad_params, pan), -50, 50},
        {offsetof(struct pad_params, cutoff), 0, 100},  {offsetof(struct pad_params, reso), 0, 100},
        {offsetof(struct pad_params, semi), -36, 36},   {offsetof(struct pad_params, fine), -100, 100},
    };
    if (param < 0 || param >= (int)(sizeof map / sizeof map[0])) return;
    int32_t v = map[param].min + (int32_t)((int64_t)(map[param].max - map[param].min) * value / 127);
    *(int32_t *)(void *)((char *)&e->pad[pad].p + map[param].off) = v;
    for (int i = 0; i < ENGINE_VOICES; i++) {
        struct voice *vo = &e->voice[i];
        // A second layer keeps its own level and tuning.
        if (vo->layer && (param == 0 || param == 4 || param == 5)) continue;
        if (vo->stage != V_FREE && vo->pad == pad) *(int32_t *)(void *)((char *)&vo->p + map[param].off) = v;
    }
}

void engine_release_pad(struct engine *e, int pad) {
    engine_release_pad_key(e, pad, ENGINE_KEY_ANY);
}

void engine_release_pad_key(struct engine *e, int pad, int key) {
    for (int i = 0; i < ENGINE_VOICES; i++) {
        struct voice *v = &e->voice[i];
        if (v->stage != V_FREE && v->pad == pad && v->p.gate && (key == ENGINE_KEY_ANY || key == v->key))
            start_release(v);
    }
}

void engine_click(struct engine *e, int accent, int offset, int volume) {
    const struct sample *s = e->click[accent ? 1 : 0];
    struct event ev = {.type = EV_ON, .pad = ENGINE_CLICK_PAD, .vel = 127, .s = s};
    ev.p.end = s->frames;
    ev.p.volume = volume;
    ev.p.cutoff = 100;
    ev.p.mono = 1;
    note_on(e, &ev, offset);
}

// S600 LPF: the Minimoog Model D's transistor ladder, after Huovilainen's model as simplified by
// Välimäki and Huovilainen (2006). Four one-pole stages, each with a zero at -0.3 (which keeps
// the cutoff where it should be at the top), the last stage fed back four times over through
// a tanh, as the ladder's differential pairs saturate: loud input thickens and squashes, and
// resonance at the top rings on by itself. Half the input is fed into the feedback (bass
// compensation 0.5), so the low end stays fat as resonance comes up, as on the Model D.
// Run at twice the rate, which keeps the resonant peak in tune at high cutoffs.
struct ladder_k {
    float g, res; // the stage coefficient and the feedback, both for the doubled rate
};

static inline float soft_tanh(float x) { // within 0.2% of tanh, much cheaper
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static struct ladder_k ladder_coefs(float fc, float reso) {
    float wc = 2.0f * PI * fc / (2.0f * ENGINE_RATE);
    if (wc > 1.6f) wc = 1.6f;
    struct ladder_k k;
    k.g = 0.9892f * wc - 0.4342f * wc * wc + 0.1381f * wc * wc * wc - 0.0202f * wc * wc * wc * wc;
    // Resonance at 100 is a little past the edge, so the last stretch of the knob sings on its own.
    float r = 1.05f * reso * (1.0029f + 0.0526f * wc - 0.926f * wc * wc + 0.0218f * wc * wc * wc);
    k.res = 4.0f * r;
    return k;
}

static inline float ladder(float *y, float *xs, float in, struct ladder_k k) {
    float out = 0;
    for (int os = 0; os < 2; os++) {
        float d = 1.4f * in; // the input pushed into the ladder; the loop's own gain stays k.res
        float u = soft_tanh(d - k.res * (y[3] - 0.5f * d));
        float x = u;
        for (int st = 0; st < 4; st++) {
            float yn = k.g * (x + 0.3f * xs[st]) / 1.3f + (1.0f - k.g) * y[st];
            xs[st] = x;
            y[st] = yn;
            x = yn;
        }
        out = y[3];
    }
    return out * (1.0f + 0.5f * k.res / 4.0f) / 1.4f; // the level the input came in at
}

// The guards: a sample this big (or NaN) is a filter or effect gone wrong, never music. It is
// silenced and what held it starts again, so one bad moment cannot linger in a filter, an EQ or
// the master compressor. Each time is counted, for the log.
#define GUARD_MAX 64.0f // 36 dB over full scale
static atomic_uint guard_trips;

unsigned engine_guard_trips(void) {
    return atomic_load_explicit(&guard_trips, memory_order_relaxed);
}

// Keeps a buffer finite and within +-GUARD_MAX. 1 when it had to.
static int guard(float *buf, int n) {
    int bad = 0;
    for (int i = 0; i < n; i++)
        if (!(fabsf(buf[i]) < GUARD_MAX)) bad = 1;
    if (bad) {
        memset(buf, 0, (size_t)n * sizeof *buf);
        atomic_fetch_add_explicit(&guard_trips, 1u, memory_order_relaxed);
    }
    return bad;
}

// Zavalishin's trapezoidal state-variable filter, one section, one channel.
static inline float svf(struct filter_state *f, int ch, float in, float g, float k, int mode) {
    float h = 1.0f / (1.0f + g * (g + k));
    float hp = h * (in - (g + k) * f->s1[ch] - f->s2[ch]);
    float bp = g * hp + f->s1[ch];
    float lp = g * bp + f->s2[ch];
    f->s1[ch] = g * hp + bp;
    f->s2[ch] = g * bp + lp;
    return mode == 0 ? lp : mode == 1 ? hp : bp;
}

static void advance_filter_env(struct voice *v, int frames) {
    const struct pad_params *p = &v->p;
    float dt_ms = (float)frames * 1000.0f / ENGINE_RATE;
    if (p->synth) { // Env2: attack, decay to the sustain level, release once the note is let go
        float sus = (float)p->flt_sustain / 100.0f;
        if (v->stage == V_RELEASE) {
            int r = p->flt_release_ms > 1 ? p->flt_release_ms : 1;
            v->fenv *= (float)pow(SILENT, dt_ms / (float)r);
        } else if (!v->fenv_falling) {
            v->fenv += dt_ms / (float)(p->flt_attack_ms > 0 ? p->flt_attack_ms : 1);
            if (v->fenv >= 1.0f) {
                v->fenv = 1.0f;
                v->fenv_falling = 1;
            }
        } else {
            int d = p->flt_decay_ms > 1 ? p->flt_decay_ms : 1;
            v->fenv = sus + (v->fenv - sus) * (float)pow(SILENT, dt_ms / (float)d);
        }
        return;
    }
    if (!v->fenv_falling) {
        v->fenv += dt_ms / (float)(p->flt_attack_ms > 0 ? p->flt_attack_ms : 1);
        if (v->fenv >= 1.0f) {
            v->fenv = 1.0f;
            v->fenv_falling = 1;
        }
    } else if (p->flt_decay_ms > 0) {
        v->fenv *= (float)pow(SILENT, dt_ms / (float)p->flt_decay_ms);
    }
}

// A glide (portamento or a slide) this block: the semitones still to go, shrinking to 0.
static float glide_step(struct voice *v, int frames) {
    if (v->glide == 0.0f) return 0;
    int ms = v->glide_ms > 0 ? v->glide_ms : v->p.porta_ms > 1 ? v->p.porta_ms : 1;
    v->glide *= (float)pow(0.01, (double)frames * 1000.0 / ((double)ms * ENGINE_RATE));
    if (fabsf(v->glide) < 0.001f) v->glide = 0, v->glide_ms = 0;
    return v->glide;
}

// An S600 voice's pitch offset this block: portamento, the bend wheel and the LFO (whose
// mod wheel depth adds to its own). Also the LFO's filter (octaves) and level change.
static double synth_pitch(struct voice *v, int bend, int mod, int frames, float *octaves, float *gain) {
    const struct pad_params *p = &v->p;
    double semis = 0;
    semis += glide_step(v, frames);
    float b = (float)bend / 8192.0f;
    semis += b > 0 ? b * (float)p->bend_up : b * (float)p->bend_down;
    float hz = 0.05f * powf(400.0f, (float)p->lfo_rate / 100.0f);
    v->lfo_phase += (double)hz * frames / ENGINE_RATE;
    if (v->lfo_phase >= 1.0) {
        v->lfo_phase -= floor(v->lfo_phase);
        v->lfo_rand = v->lfo_rand * 1664525u + 1013904223u;
        v->lfo_held = (float)(v->lfo_rand >> 8) / (float)(1u << 24) * 2.0f - 1.0f;
    }
    float ph = (float)v->lfo_phase, w;
    switch (p->lfo_shape) {
    case LFO_SINE: w = sinf(2.0f * PI * ph); break;
    case LFO_SQUARE: w = ph < 0.5f ? 1.0f : -1.0f; break;
    case LFO_SAW_UP: w = 2.0f * ph - 1.0f; break;
    case LFO_SAW_DOWN: w = 1.0f - 2.0f * ph; break;
    case LFO_RANDOM: w = v->lfo_held; break;
    default: w = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; break;
    }
    v->lfo_ms += (float)frames * 1000.0f / ENGINE_RATE;
    if (p->lfo_delay_ms > 0 && v->lfo_ms < (float)p->lfo_delay_ms) w *= v->lfo_ms / (float)p->lfo_delay_ms;
    float wheel = (float)mod / 127.0f;
    float depth = (float)p->lfo_pitch + (float)p->mod_pitch * wheel;
    semis += w * depth / 100.0f * 2.0f; // 100 swings two semitones each way
    float fdepth = (float)p->lfo_filter + (p->mod_flt_open ? 0.0f : (float)p->mod_filter * wheel);
    *octaves = w * fdepth / 100.0f * 2.0f;
    if (p->mod_flt_open) *octaves += (float)p->mod_filter / 100.0f * wheel * 3.0f; // OPEN: up to three octaves
    float adepth = (float)p->lfo_amp + (float)p->mod_level * wheel;
    if (adepth > 100.0f) adepth = 100.0f;
    *gain = 1.0f - adepth / 100.0f * 0.5f * (1.0f - w);
    return semis;
}

static void render_voice(struct voice *v, float *out, int frames, int e_bend, int e_mod) {
    const struct sample *s = v->s;
    const struct pad_params *p = &v->p;
    int32_t end = p->end < s->frames ? p->end : s->frames;
    int32_t start = p->start < 0 ? 0 : p->start;
    if (end <= start) {
        v->stage = V_FREE;
        return;
    }
    int32_t loop = p->loop < start ? start : p->loop >= end ? end - 1 : p->loop;
    double semis = (double)p->semi + p->fine / 100.0 + v->key;
    float lfo_octaves = 0, lfo_gain = 1;
    if (p->synth) semis += synth_pitch(v, e_bend, e_mod, frames, &lfo_octaves, &lfo_gain);
    else semis += glide_step(v, frames); // a slide on a drum pad's sound
    double step = (double)s->rate / ENGINE_RATE * pow(2.0, semis / 12.0);
    float vol = (float)p->volume / 100.0f;
    float mix = (100.0f - (float)v->track_cut) / 100.0f; // the track mixer's level, on the same curve
    vol = vol * vol * v->vel_gain * lfo_gain * mix * mix;
    int pan = p->pan + v->track_pan;
    pan = pan < -50 ? -50 : pan > 50 ? 50 : pan;
    float theta = ((float)pan + 50.0f) / 100.0f * (PI / 2.0f);
    float gl = (float)cos(theta) * SQRT2, gr = (float)sin(theta) * SQRT2;

    int sections = 0, mode = 0, moog = 0;
    float g = 0, k = 2, k_first = 2, drive = 1;
    struct ladder_k lk = {0, 0};
    // LPF is on by default. Fully open with no envelope it changes nothing, so skip it.
    int open_lpf = p->filter == FILTER_LP2 && p->cutoff >= 100 && p->flt_depth <= 0 && (!p->synth || (!p->lfo_filter && !p->mod_filter));
    if (p->filter != FILTER_OFF && !open_lpf) {
        advance_filter_env(v, frames);
        sections = (p->filter == FILTER_LP4 || p->filter == FILTER_HP4 || p->filter == FILTER_BP4) ? 2 : 1;
        mode = (p->filter == FILTER_HP2 || p->filter == FILTER_HP4) ? 1
             : (p->filter == FILTER_BP2 || p->filter == FILTER_BP4) ? 2 : 0;
        float octaves = (float)p->flt_depth / 100.0f * v->fenv * 6.0f + lfo_octaves;
        float fc = 20.0f * (float)pow(1000.0, (double)p->cutoff / 100.0) * (float)pow(2.0, octaves);
        float top = 0.45f * ENGINE_RATE;
        fc = fc < 20.0f ? 20.0f : fc > top ? top : fc;
        g = (float)tan(PI * fc / ENGINE_RATE);
        k = 2.0f - 1.95f * ((float)p->reso / 100.0f);
        // LPF: a 2-pole low-pass like the MPC3000's, whose resonance stays musical and
        // never whistles. Its exact curve still needs measuring against a real MPC3000.
        if (p->filter == FILTER_LP2) k = 2.0f - 1.6f * ((float)p->reso / 100.0f);
        k_first = k;
        if (p->filter == FILTER_MOOG) { // S600 LPF: the ladder, in place of the sections
            moog = 1;
            sections = 0;
            lk = ladder_coefs(fc, (float)p->reso / 100.0f);
        }
    }

    int ch = s->channels;
    // A sound below 44.1 kHz plays as a vintage sampler's converter did: each sample held
    // until the next one's time, not blended into it.
    int hold = s->rate < ENGINE_RATE;
    int first = v->delay < frames ? v->delay : frames;
    v->delay -= first;
    for (int n = first; n < frames; n++) {
        if (!p->reverse) {
            if (v->pos >= end) {
                if (!p->loop_on) {
                    v->stage = V_FREE;
                    return;
                }
                v->pos = loop + fmod(v->pos - end, (double)(end - loop));
            }
        } else {
            int32_t low = p->loop_on ? loop : start;
            if (v->pos < low) {
                if (!p->loop_on) {
                    v->stage = V_FREE;
                    return;
                }
                v->pos = (end - 1) - fmod(low - v->pos, (double)(end - low));
                if (v->pos < low) v->pos = low;
            }
        }
        int32_t i0 = (int32_t)v->pos;
        if (i0 < 0) i0 = 0;
        if (i0 >= s->frames) i0 = s->frames - 1;
        int32_t i1 = i0 + 1 < s->frames ? i0 + 1 : i0;
        float t = hold ? 0.0f : (float)(v->pos - i0);
        float l = s->data[i0 * ch] + t * (s->data[i1 * ch] - s->data[i0 * ch]);
        float r = l;
        if (ch == 2) r = s->data[i0 * 2 + 1] + t * (s->data[i1 * 2 + 1] - s->data[i0 * 2 + 1]);

        for (int c = 0; c < sections; c++) {
            float kc = c + 1 < sections ? k_first : k;
            l = svf(&v->f[c], 0, c ? l : l * drive, g, kc, mode);
            r = svf(&v->f[c], 1, c ? r : r * drive, g, kc, mode);
        }
        if (moog) {
            l = ladder(v->lad_y[0], v->lad_x[0], l, lk);
            r = ch == 2 ? ladder(v->lad_y[1], v->lad_x[1], r, lk) : l;
        }
        if (!(fabsf(l) < GUARD_MAX) || !(fabsf(r) < GUARD_MAX)) { // blown up (or NaN): its filter starts again
            memset(v->f, 0, sizeof v->f);
            memset(v->lad_y, 0, sizeof v->lad_y), memset(v->lad_x, 0, sizeof v->lad_x);
            l = r = 0;
            atomic_fetch_add_explicit(&guard_trips, 1u, memory_order_relaxed);
        }

        switch (v->stage) {
        case V_ATTACK:
            v->env += v->attack_step;
            if (v->env >= 1.0f) {
                v->env = 1.0f;
                v->stage = p->synth || (p->decay_ms > 0 && !p->gate && !p->decay_end) ? V_DECAY : V_HOLD;
            }
            break;
        case V_HOLD:
            if (p->decay_end && p->decay_ms > 0 && !p->gate && !p->loop_on && !p->synth) {
                double left = p->reverse ? v->pos - start : end - v->pos; // sample frames to go
                if (left / step * 1000.0 / ENGINE_RATE <= p->decay_ms) v->stage = V_DECAY;
            }
            break;
        case V_DECAY:
            if (p->synth) { // Env1 falls to the sustain level and holds there
                float sus = (float)p->sustain / 100.0f;
                v->env = sus + (v->env - sus) * v->decay_coef;
                if (sus > 0 && v->env - sus < 0.0005f) {
                    v->env = sus;
                    v->stage = V_HOLD;
                    if (!p->gate) start_release(v); // one-shot: no holding, straight on to the release
                }
            } else {
                v->env *= v->decay_coef;
            }
            break;
        case V_RELEASE:
            v->env *= v->release_coef;
            break;
        default:
            break;
        }
        if ((v->stage == V_DECAY || v->stage == V_RELEASE) && v->env < SILENT) {
            v->stage = V_FREE;
            return;
        }
        float amp = v->env * vol;
        if (v->fading) {
            v->fade -= 1.0f / FADE_FRAMES;
            if (v->fade <= 0.0f) {
                v->stage = V_FREE;
                return;
            }
            amp *= v->fade;
        }
        out[n * 2] += l * amp * gl;
        out[n * 2 + 1] += r * amp * gr;
        v->pos += p->reverse ? -step : step;
    }
}

// One band of a bus: a biquad, transposed direct form II, both channels.
static void biquad(float *x, int frames, const float c[5], float z[2][2]) {
    for (int ch = 0; ch < 2; ch++) {
        float z1 = z[ch][0], z2 = z[ch][1];
        for (int i = 0; i < frames; i++) {
            float in = x[i * 2 + ch], y = c[0] * in + z1;
            z1 = c[1] * in - c[3] * y + z2;
            z2 = c[2] * in - c[4] * y;
            x[i * 2 + ch] = y;
        }
        z[ch][0] = z1, z[ch][1] = z2;
    }
}

void engine_render(struct engine *e, float *out, int frames) {
    if (frames > ENGINE_MAX_BLOCK) { // longer than a bus holds: in pieces
        for (int done = 0; done < frames; done += ENGINE_MAX_BLOCK)
            engine_render(e, out + done * 2, frames - done < ENGINE_MAX_BLOCK ? frames - done : ENGINE_MAX_BLOCK);
        return;
    }
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    process_events(e);
    for (int b = 0; b < ENGINE_BUSES; b++)
        if (e->bus[b].track) memset(e->bus[b].buf, 0, (size_t)frames * 2 * sizeof(float));
    if (e->hook) e->hook(e->hook_ctx, e, frames);
    unsigned char playing[ENGINE_SLOTS] = {0};
    int bend = atomic_load_explicit(&e->bend, memory_order_relaxed);
    int mod = atomic_load_explicit(&e->mod, memory_order_relaxed);
    int active = 0;
    int sends_on = atomic_load_explicit(&e->sends_on, memory_order_relaxed) && e->fx_hook;
    int fx_pad = e->fx_hook ? atomic_load_explicit(&e->fx_pad, memory_order_relaxed) : -1;
    if (sends_on || fx_pad >= 0)
        for (int b = 0; b <= ENGINE_SENDS; b++) memset(e->fxbus[b], 0, (size_t)frames * 2 * sizeof(float));
    for (int i = 0; i < ENGINE_VOICES; i++) {
        struct voice *v = &e->voice[i];
        if (v->stage == V_FREE) continue;
        float *to = out; // a track with its EQ on mixes into its bus
        for (int b = 0; b < ENGINE_BUSES && v->track > 0; b++)
            if (e->bus[b].track == v->track) to = e->bus[b].buf;
        int on_pad = fx_pad >= 0 && (v->pad == fx_pad || v->pad == fx_pad + ENGINE_S600);
        const float *send = sends_on && v->track > 0 && v->track < ENGINE_TRACKS ? e->send[v->track] : 0;
        int sending = 0;
        for (int k = 0; send && k < ENGINE_SENDS; k++) sending |= send[k] > 0;
        if (!on_pad && !sending) {
            render_voice(v, to, frames, bend, mod);
        } else { // rendered alone, then to the pad bus, or to its place and its sends
            float one[ENGINE_MAX_BLOCK * 2];
            memset(one, 0, (size_t)frames * 2 * sizeof(float));
            render_voice(v, one, frames, bend, mod);
            float *dest = on_pad ? e->fxbus[ENGINE_SENDS] : to;
            for (int n = 0; n < frames * 2; n++) dest[n] += one[n];
            for (int k = 0; sending && !on_pad && k < ENGINE_SENDS; k++)
                if (send[k] > 0)
                    for (int n = 0; n < frames * 2; n++) e->fxbus[k][n] += one[n] * send[k];
        }
        if (v->stage != V_FREE) {
            active++;
            if (v->pad >= 0 && v->pad < ENGINE_SLOTS) playing[v->pad] = 1;
        }
    }
    for (int b = 0; b < ENGINE_BUSES; b++) { // each EQ'd track through its three bands, into the outputs
        if (!e->bus[b].track) continue;
        for (int k = 0; k < 3; k++) biquad(e->bus[b].buf, frames, e->bus[b].c[k], e->bus[b].z[k]);
        if (guard(e->bus[b].buf, frames * 2)) memset(e->bus[b].z, 0, sizeof e->bus[b].z);
        for (int i = 0; i < frames * 2; i++) out[i] += e->bus[b].buf[i];
    }
    if (sends_on || fx_pad >= 0) e->fx_hook(e->fx_ctx, e, out, frames); // the effects' returns
    guard(out, frames * 2); // nothing out of range reaches the input, master or outputs
    for (int i = 0; i < ENGINE_SLOTS; i++) atomic_store_explicit(&e->playing[i], playing[i], memory_order_relaxed);
    atomic_store_explicit(&e->voices, active, memory_order_relaxed);
    float pl = atomic_load_explicit(&e->peak_l, memory_order_relaxed);
    float pr = atomic_load_explicit(&e->peak_r, memory_order_relaxed);
    for (int i = 0; i < frames; i++) {
        float l = fabsf(out[i * 2]), r = fabsf(out[i * 2 + 1]);
        if (l > pl) pl = l;
        if (r > pr) pr = r;
    }
    atomic_store_explicit(&e->peak_l, pl, memory_order_relaxed);
    atomic_store_explicit(&e->peak_r, pr, memory_order_relaxed);
    atomic_fetch_add_explicit(&e->clock, frames, memory_order_release);
}
