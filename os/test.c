// Host tests for the engine, the WAV reader, and the Sample screen.
// Pass a directory to also write a screenshot of the Sample screen there.
#include "engine.h"
#include "onset.h"
#include "os.h"
#include "lcd.h"
#include "seq_screen.h"
#include "wav.h"
#include "snd.h"
#include "seqimport.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static int failures, checks;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

static struct engine e;
static struct os os;

static struct sample *ramp(int frames) {
    struct sample *s = sample_new(frames, 1, ENGINE_RATE);
    for (int i = 0; i < frames; i++) s->data[i] = (float)(i + 1) / (float)frames;
    return s;
}

static struct sample *constant(int frames) {
    struct sample *s = sample_new(frames, 1, ENGINE_RATE);
    for (int i = 0; i < frames; i++) s->data[i] = 1.0f;
    return s;
}

static struct pad_params params(int frames) {
    struct pad_params p = {0};
    p.end = frames;
    p.volume = 100;
    p.cutoff = 100;
    return p;
}

static float block[2048];

static void render(int frames) {
    engine_render(&e, block, frames);
}

static int voices(void) {
    return atomic_load(&e.voices);
}

// A MULTI instrument's key map: the zone for the note and velocity gives the sample and its
// tuning (from its root); KEY>PAN moves with the note; no zone, no note; no key map, the pad's own.
static const struct voice *voice_of(const struct sample *smp) {
    for (int i = 0; i < ENGINE_VOICES; i++)
        if (e.voice[i].stage != 0 && e.voice[i].s == smp) return &e.voice[i];
    return 0;
}

static void test_keymap(void) {
    engine_init(&e);
    struct sample *a = constant(44100), *b = constant(44100), *c = constant(44100), *own = constant(44100);
    struct pad_params p = params(44100);
    p.synth = 1, p.gate = 1, p.sustain = 100;
    int slot = ENGINE_S600 + 5;
    engine_set_pad(&e, slot, own, &p);
    static struct keymap km;
    memset(&km, 0, sizeof km);
    km.n = 3, km.kt_pan = 50;
    km.z[0] = (struct km_zone){a, 0, 44100, 0, 0, 0, 59, 48, 1, 127, 0, 0, 100, 0};
    km.z[1] = (struct km_zone){b, 0, 44100, 0, 0, 60, 127, 72, 1, 80, 0, 0, 100, 0};
    km.z[2] = (struct km_zone){c, 0, 44100, 0, 0, 60, 127, 72, 81, 127, 2, 0, 50, 0};
    CHECK(engine_set_keymap(&e, slot, 0, &km) == 0);
    render(16);
    engine_note_on_layers(&e, slot, 100, -12, own, 0, &p, 0, 100, 0); // C3: zone 1, at its root
    render(16);
    const struct voice *v = voice_of(a);
    CHECK(v && v->p.semi == 12 && v->key == -12 && v->p.pan == -25 && !voice_of(own));
    engine_note_off_key(&e, slot, -12);
    engine_note_on_layers(&e, slot, 50, 12, own, 0, &p, 0, 100, 0); // C5 soft: zone 2
    render(16);
    v = voice_of(b);
    CHECK(v && v->p.semi == -12 && v->p.pan == 25); // root C5: key +12 and -12, its own pitch
    engine_note_on_layers(&e, slot, 120, 0, own, 0, &p, 0, 100, 0); // C4 hard: zone 3, TUNE +2, LEVEL 50
    render(16);
    v = voice_of(c);
    CHECK(v && v->p.semi == -12 + 2 && v->p.volume == 50 && v->p.pan == 0);
    engine_play_pad_mix(&e, slot, 100, -12, 0, 0, 100, 0); // the sequencer's way in, the same
    CHECK(voice_of(a) != 0);
    // KEY>FILTER: an octave up opens the cutoff by an octave's worth (100 / 9.97 of the scale).
    engine_kill_all(&e);
    km.kt_pan = 0, km.kt_filter = 100;
    p.cutoff = 50;
    engine_set_keymap(&e, slot, 0, &km);
    for (int i = 0; i < 40; i++) render(128); // the old notes faded out
    CHECK(atomic_load(&e.voices) == 0 || !voice_of(b));
    engine_note_on_layers(&e, slot, 100, 12, own, 0, &p, 0, 100, 0); // C5 hard: zone 3
    render(16);
    v = voice_of(c);
    CHECK(v && v->p.cutoff == 60);
    // No key map: the pad's own sound again.
    engine_kill_all(&e);
    engine_set_keymap(&e, slot, 0, 0);
    render(512);
    engine_note_on_layers(&e, slot, 100, 0, own, 0, &p, 0, 100, 0);
    render(16);
    CHECK(voice_of(own) && !voice_of(c));
    engine_kill_all(&e);
    render(512);
    sample_free(a), sample_free(b), sample_free(c), sample_free(own);
}

// The worst the audio thread can be given: 32 S600 voices (ladder filter, LFO) across 10 tracks,
// all nine sends into heavy effects, every insert slot filled with the costliest effect per slot
// (four JTM45 amps), and Master Effects. How long a block takes here, with and without the inserts.
static double worst_block_us(int with_inserts, int with_sends) {
    static struct engine w;
    engine_init(&w);
    static struct fx_chain c;
    static struct fx_inserts ins;
    static int ready;
    if (!ready) fx_chain_init(&c), fx_inserts_init(&ins), ready = 1;
    static const int sends[FX_SLOTS] = {FX_ASR_HALL, FX_PLATE, FX_SPRING, FX_DP4, FX_UNIVIBE, FX_TAPE, FX_AC30, FX_DUB, FX_SMALLSTONE};
    for (int i = 0; i < FX_SLOTS; i++) {
        atomic_store(&c.type[i], sends[i]);
        for (int k = 0; k < 3; k++) atomic_store(&c.p[i][k], fx_param_default(sends[i], k));
    }
    atomic_store(&c.mode, FXM_TRACK), atomic_store(&c.on, 1), atomic_store(&c.bpm10, 1200);
    w.fx_hook = fx_engine_hook, w.fx_ctx = &c;
    atomic_store(&w.sends_on, with_sends);
    unsigned char lv[ENGINE_SENDS];
    memset(lv, 40, sizeof lv);
    int tracks[ENGINE_INSERTS] = {0};
    for (int i = 0; i < FX_INSERTS; i++) atomic_store(&ins.track[i], 0);
    if (with_inserts) // four amps, two slots each: all eight
        for (int i = 0; i < 4; i++) {
            atomic_store(&ins.track[i], i + 1), atomic_store(&ins.pos[i], 0), atomic_store(&ins.type[i], FX_JTM45);
            for (int k = 0; k < 3; k++) atomic_store(&ins.p[i][k], 80);
            tracks[i] = i + 1;
        }
    w.insert_hook = fx_insert_hook, w.insert_ctx = &ins;
    engine_set_inserts(&w, tracks);
    struct sample *saw = sample_new(44100, 1, 44100);
    for (int i = 0; i < 44100; i++) saw->data[i] = 0.3f * (float)(fmod(i * 110.0 / 44100, 1.0) * 2 - 1);
    struct pad_params p = params(44100);
    p.synth = 1, p.gate = 1, p.sustain = 100, p.loop_on = 1, p.filter = FILTER_MOOG, p.cutoff = 60, p.reso = 60;
    p.lfo_filter = 30, p.lfo_pitch = 20, p.flt_depth = 40;
    for (int t = 1; t <= 10; t++) engine_track_sends(&w, t, lv);
    for (int v = 0; v < ENGINE_VOICES; v++) engine_note_on_layers(&w, ENGINE_S600 + v, 100, v % 12, saw, 0, &p, v % 10 + 1, 80, 0);
    struct master *m = calloc(1, sizeof *m);
    master_init(m);
    atomic_store(&m->eq_on, 1), atomic_store(&m->comp, 1);
    static float blk[256];
    for (int i = 0; i < 40; i++) engine_render(&w, blk, 128), master_process(m, blk, 128); // settled in
    struct timespec a, z;
    int blocks = 400;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < blocks; i++) engine_render(&w, blk, 128), master_process(m, blk, 128);
    clock_gettime(CLOCK_MONOTONIC, &z);
    CHECK(atomic_load(&w.voices) == ENGINE_VOICES);
    int finite = 1;
    for (int i = 0; i < 256; i++) finite &= isfinite(blk[i]) != 0;
    CHECK(finite);
    if (m->vfx) fx_free(m->vfx);
    free(m);
    sample_free(saw);
    return ((z.tv_sec - a.tv_sec) * 1e9 + (z.tv_nsec - a.tv_nsec)) / 1e3 / blocks;
}

static void test_cpu_worst(void) {
    double block_us = 128.0 / 44100 * 1e6, voices = worst_block_us(0, 0), without = worst_block_us(0, 1), with = worst_block_us(1, 1);
    printf("  worst case parts here: 32 voices and Master %.1f%%, the 9 sends %.1f%%, the 8 insert slots %.1f%% of a block\n",
           voices / block_us * 100, (without - voices) / block_us * 100, (with - without) / block_us * 100);
    printf("  worst case, a 2.9 ms block here: %.0f us without inserts (%.1f%%), %.0f us with all 8 insert slots (%.1f%%); "
           "the inserts %.0f%% of it\n",
           without, without / block_us * 100, with, with / block_us * 100, (with - without) / with * 100);
    CHECK(with > without * 0.9); // measured, not skipped
}

static void test_one_shot(void) {
    engine_init(&e);
    struct sample *s = ramp(256);
    struct pad_params p = params(256);
    engine_note_on(&e, 3, 127, s, &p);
    render(128);
    CHECK(voices() == 1);
    CHECK(atomic_load(&e.playing[3]) == 1);
    CHECK(fabsf(block[0] - 1.0f / 256.0f) < 1e-4f);
    CHECK(fabsf(block[1] - block[0]) < 1e-4f);
    CHECK(fabsf(block[127 * 2] - 128.0f / 256.0f) < 1e-4f);
    render(128);
    render(128);
    CHECK(voices() == 0);
    CHECK(atomic_load(&e.playing[3]) == 0);
    sample_free(s);
}

static void test_reverse(void) {
    engine_init(&e);
    struct sample *s = ramp(100);
    struct pad_params p = params(100);
    p.reverse = 1;
    engine_note_on(&e, 0, 127, s, &p);
    render(50);
    CHECK(fabsf(block[0] - 1.0f) < 1e-4f);
    CHECK(block[2] < block[0]);
    sample_free(s);
}

static void test_loop_toggle(void) {
    engine_init(&e);
    struct sample *s = constant(32);
    struct pad_params p = params(32);
    p.loop_on = 1;
    engine_note_on(&e, 1, 127, s, &p);
    for (int i = 0; i < 20; i++) render(128);
    CHECK(voices() == 1);
    CHECK(fabsf(block[100] - 1.0f) < 1e-3f);
    engine_note_on(&e, 1, 127, s, &p); // the second hit stops a looping one-shot
    for (int i = 0; i < 40; i++) render(128);
    CHECK(voices() == 0);
    sample_free(s);
}

static void test_reverse_loop_stays_in_bounds(void) {
    engine_init(&e);
    struct sample *s = constant(64);
    struct pad_params p = params(64);
    p.loop_on = p.reverse = 1;
    p.loop = 10;
    p.semi = 24;
    engine_note_on(&e, 1, 127, s, &p);
    for (int i = 0; i < 50; i++) render(128);
    CHECK(voices() == 1);
    CHECK(fabsf(block[20] - 1.0f) < 1e-3f);
    sample_free(s);
}

static void test_gate(void) {
    engine_init(&e);
    struct sample *s = constant(44100);
    struct pad_params p = params(44100);
    p.gate = 1;
    p.decay_ms = 10;
    engine_note_on(&e, 2, 127, s, &p);
    for (int i = 0; i < 20; i++) render(128);
    CHECK(voices() == 1);
    engine_note_off(&e, 2);
    for (int i = 0; i < 30; i++) render(128);
    CHECK(voices() == 0);
    sample_free(s);
}

static void test_one_shot_ignores_release(void) {
    engine_init(&e);
    struct sample *s = constant(4096);
    struct pad_params p = params(4096);
    engine_note_on(&e, 2, 127, s, &p);
    render(128);
    engine_note_off(&e, 2);
    render(128);
    CHECK(voices() == 1);
    sample_free(s);
}

static void test_mono_and_poly(void) {
    engine_init(&e);
    struct sample *s = constant(44100);
    struct pad_params p = params(44100);
    engine_note_on(&e, 4, 127, s, &p);
    engine_note_on(&e, 4, 127, s, &p);
    render(128);
    CHECK(voices() == 2);
    engine_init(&e);
    p.mono = 1;
    engine_note_on(&e, 4, 127, s, &p);
    render(128);
    engine_note_on(&e, 4, 127, s, &p);
    for (int i = 0; i < 3; i++) render(128);
    CHECK(voices() == 1);
    sample_free(s);
}

static void test_mute_group(void) {
    engine_init(&e);
    struct sample *s = constant(44100);
    struct pad_params p = params(44100);
    p.mute_group = 1;
    engine_note_on(&e, 5, 127, s, &p);
    render(128);
    engine_note_on(&e, 6, 127, s, &p);
    for (int i = 0; i < 3; i++) render(128);
    CHECK(voices() == 1);
    CHECK(atomic_load(&e.playing[5]) == 0);
    CHECK(atomic_load(&e.playing[6]) == 1);
    sample_free(s);
}

static void test_voice_limit(void) {
    engine_init(&e);
    struct sample *s = constant(44100);
    struct pad_params p = params(44100);
    for (int i = 0; i < ENGINE_VOICES + 10; i++) {
        engine_note_on(&e, i % 16, 127, s, &p);
        render(16);
    }
    CHECK(voices() == ENGINE_VOICES);
    sample_free(s);
}

static void test_velocity_and_pan(void) {
    engine_init(&e);
    struct sample *s = constant(4096);
    struct pad_params p = params(4096);
    p.vel_sens = 100;
    engine_note_on(&e, 0, 64, s, &p);
    render(16);
    CHECK(fabsf(block[0] - 64.0f / 127.0f) < 1e-3f);
    engine_init(&e);
    p = params(4096);
    p.pan = -50;
    engine_note_on(&e, 0, 127, s, &p);
    render(16);
    CHECK(block[0] > 1.3f);
    CHECK(fabsf(block[1]) < 1e-3f);
    sample_free(s);
}

static void test_filter_removes_highs(void) {
    engine_init(&e);
    struct sample *s = sample_new(44100, 1, ENGINE_RATE);
    for (int i = 0; i < 44100; i++) s->data[i] = (i & 1) ? 1.0f : -1.0f;
    struct pad_params p = params(44100);
    p.filter = FILTER_LP4;
    p.cutoff = 30;
    engine_note_on(&e, 0, 127, s, &p);
    render(1024);
    render(128);
    CHECK(fabsf(block[100]) < 0.01f);
    sample_free(s);
}

static void test_retire_waits_for_audio(void) {
    engine_init(&e);
    struct sample *s = constant(44100);
    struct pad_params p = params(44100);
    engine_note_on(&e, 0, 127, s, &p);
    render(128);
    engine_retire(&e, s);
    engine_reap(&e);
    CHECK(e.retired[0].s == s); // not freed until the audio thread has seen it
    render(128);
    CHECK(voices() == 0);
    engine_reap(&e);
    CHECK(e.retired[0].s == 0);
}

static void test_ring_full(void) {
    engine_init(&e);
    struct pad_params p = params(1);
    int sent = 0;
    while (engine_update(&e, 0, &p) == 0 && sent < ENGINE_RING * 2) sent++;
    CHECK(sent == ENGINE_RING);
    render(16);
    CHECK(engine_update(&e, 0, &p) == 0);
}

static size_t make_wav(unsigned char *b, int format, int bits, int ch, const void *pcm, size_t n, int odd_chunk) {
    size_t at = 12;
    memcpy(b, "RIFF\0\0\0\0WAVE", 12);
    if (odd_chunk) {
        memcpy(b + at, "junk\x03\0\0\0abc\0", 12);
        at += 12;
    }
    int block = ch * bits / 8;
    unsigned char fmt[24] = "fmt ";
    fmt[4] = 16;
    fmt[8] = (unsigned char)format;
    fmt[10] = (unsigned char)ch;
    fmt[12] = 0x44, fmt[13] = 0xac;
    fmt[20] = (unsigned char)block;
    fmt[22] = (unsigned char)bits;
    memcpy(b + at, fmt, 24);
    at += 24;
    memcpy(b + at, "data", 4);
    b[at + 4] = (unsigned char)n, b[at + 5] = (unsigned char)(n >> 8), b[at + 6] = 0, b[at + 7] = 0;
    memcpy(b + at + 8, pcm, n);
    return at + 8 + n;
}

static void test_wav(void) {
    unsigned char b[512];
    const int16_t pcm16[4] = {0, 16384, -32768, 32767};
    struct sample *s = wav_parse(b, make_wav(b, 1, 16, 2, pcm16, sizeof pcm16, 0));
    CHECK(s && s->frames == 2 && s->channels == 2 && s->rate == 44100);
    if (s) {
        CHECK(fabsf(s->data[1] - 0.5f) < 1e-4f);
        CHECK(fabsf(s->data[2] + 1.0f) < 1e-4f);
        sample_free(s);
    }
    const unsigned char pcm24[6] = {0, 0, 0x40, 0, 0, 0xc0};
    s = wav_parse(b, make_wav(b, 1, 24, 1, pcm24, sizeof pcm24, 1));
    CHECK(s && s->frames == 2 && s->channels == 1);
    if (s) {
        CHECK(fabsf(s->data[0] - 0.5f) < 1e-4f);
        CHECK(fabsf(s->data[1] + 0.5f) < 1e-4f);
        sample_free(s);
    }
    const float pcmf[2] = {0.25f, -0.75f};
    s = wav_parse(b, make_wav(b, 3, 32, 1, pcmf, sizeof pcmf, 0));
    CHECK(s && s->frames == 2);
    if (s) {
        CHECK(fabsf(s->data[1] + 0.75f) < 1e-5f);
        sample_free(s);
    }
    size_t full = make_wav(b, 1, 16, 2, pcm16, sizeof pcm16, 0);
    for (size_t cut = 0; cut < full - 4; cut++) {
        s = wav_parse(b, cut);
        CHECK(!s);
        if (s) sample_free(s);
    }
    s = wav_parse(b, full - 4); // a header that overstates its data still loads what is there
    CHECK(s && s->frames == 1);
    if (s) sample_free(s);
    full = make_wav(b, 1, 12, 1, pcm16, 4, 0);
    CHECK(!wav_parse(b, full));
}

static uint32_t px[UI_W * UI_H];

static struct input blank(void) {
    struct input in;
    input_begin(&in, 0);
    return in;
}

// MPC_TEST_LAYOUT=1: every frame drawn on the 2000XL layout, to look at its screenshots (checks
// that read the screen may fail then).
static int forced_layout = -1;
static void force_layout(struct os *o) {
    if (forced_layout >= 0) o->layout = forced_layout, lcd_layout = forced_layout;
}
#define os_frame(...) (force_layout(&os), os_frame(__VA_ARGS__))

static void frame(struct input *in) {
    struct ui u = {.px = px, .now_ms = 1000, .track_led = -1};
    // Each test frame stands for its own moment: a knob turned in an earlier one is long past.
    if (!(in->knob_moved[0] || in->knob_moved[1] || in->knob_moved[2] || in->fader_moved)) os.ctl_ms = 0;
    os_frame(&u, in, &os);
    render(128);
}

static void tap(int button) {
    struct input in = blank();
    in.down = in.held = 1u << button;
    frame(&in);
}

static void shift_pad16(void) { // SHIFT + Pad 16: Save/Load
    struct input in = blank();
    in.held = 1u << IN_SHIFT, in.shift = 1;
    in.pad_hit[15] = 100, in.pad_down[15] = 1;
    frame(&in);
    in = blank();
    frame(&in);
}

static void write_shot(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
    for (int i = 0; i < UI_W * UI_H; i++) {
        unsigned char rgb[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void test_title(const char *shot_dir) {
    engine_init(&e);
    os_init(&os, &e);
    os.page = os.group_page[0] = PAGE_TRIM; // these tests start from Trim (the default is Vol)
    struct sample *s = constant(4096);
    os_pad_load(&os, 0, s, "Kick");
    CHECK(os.screen == OS_TITLE);

    struct input in = blank();
    frame(&in);
    CHECK(os.screen == OS_TITLE);
    CHECK(os.title_until == 1000 + OS_TITLE_MS);
    if (shot_dir) write_shot(shot_dir, "title");

    // The pad that skips the title selects nothing and makes no sound.
    in = blank();
    in.pad_hit[0] = 127, in.pad_down[0] = 1;
    frame(&in);
    CHECK(os.screen == OS_MAIN); // the MPC3000 starts on its main screen
    CHECK(os.cursor_play == 0);
    CHECK(voices() == 0);
    CHECK(os.sounding[0] == -1);

    os.screen = OS_TITLE;
    os.title_until = 2000;
    in = blank();
    frame(&in);
    CHECK(os.screen == OS_TITLE);
    struct ui u = {.px = px, .now_ms = 2000, .track_led = -1};
    in = blank();
    os_frame(&u, &in, &os);
    CHECK(os.screen == OS_MAIN); // it leaves by itself when the time is up
}

static void test_screen(const char *shot_dir) {
    engine_init(&e);
    os_init(&os, &e);
    os.page = os.group_page[0] = PAGE_TRIM; // these tests start from Trim (the default is Vol)
    os.screen = OS_SAMPLE;
    int frames = 44100;
    struct sample *s = sample_new(frames, 1, ENGINE_RATE);
    for (int i = 0; i < frames; i++)
        s->data[i] = (float)sin(i * 0.05) * (float)exp(-(double)i / 9000.0);
    os_pad_load(&os, 0, s, "Kick Long");
    CHECK(os.pads[0].p.end == frames);

    struct input in = blank();
    in.pad_hit[0] = 127, in.pad_down[0] = 1;
    frame(&in);
    CHECK(voices() == 1);
    CHECK(os.sounding[0] == 0);

    in = blank();
    in.pad_release[0] = 1;
    frame(&in);
    CHECK(os.sounding[0] == -1);

    in = blank();
    in.down = 0, in.encoder = 3;
    frame(&in);
    CHECK(os.pads[0].p.start == 3 * frames / OS_WAVE_W);

    tap(IN_B1);
    CHECK(os.page == PAGE_MIX);
    CHECK(os.frame_slot == 0);
    in = blank();
    in.encoder_press = 1;
    frame(&in); // Volume, then Pan; the empty K3 on Mix is skipped, then Sample: above them
    CHECK(os.frame_slot == 1);
    frame(&in);
    CHECK(os.frame_slot == -1);
    frame(&in);
    CHECK(os.frame_slot == 0);
    tap(IN_B1);
    CHECK(os.page == PAGE_TRIM); // function 1 is just Trim and Vol
    tap(IN_B2);
    CHECK(os.page == PAGE_TUNE);
    tap(IN_B2);
    CHECK(os.page == PAGE_PLAY);
    tap(IN_B2);
    CHECK(os.page == PAGE_AMP); // then Envel and Vel Mod, after Play
    tap(IN_B2);
    CHECK(os.page == PAGE_VEL);
    tap(IN_B2);
    CHECK(os.page == PAGE_TUNE);
    tap(IN_B2);
    CHECK(os.page == PAGE_PLAY);
    tap(IN_B3);
    CHECK(os.page == PAGE_FILTER);
    tap(IN_B2);
    CHECK(os.page == PAGE_PLAY); // a tab comes back to the page it last showed

    tap(IN_LOOP); // the Loop screen, on Loop: Off
    CHECK(os.screen == OS_LOOP && os.cursor_loop == 0 && os.pads[0].p.loop_on == 0);
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.pads[0].p.loop_on == 1);
    tap(IN_SAMPLE);
    CHECK(os.screen == OS_SAMPLE);
    tap(IN_PAD_BANK);
    CHECK(os.bank == 1);
    in = blank();
    in.down = in.held = (1u << IN_PAD_BANK) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.bank == 0);

    tap(IN_B1); // back to Trim for the knob and limit checks
    CHECK(os.page == PAGE_TRIM);

    // A knob far from the value does nothing until it reaches the value.
    int end_before = os.pads[0].p.end;
    in = blank();
    in.knob_moved[1] = 1, in.knob[1] = 10;
    frame(&in);
    CHECK(os.pads[0].p.end == end_before);
    in = blank();
    in.knob_moved[1] = 1, in.knob[1] = 127;
    frame(&in);
    CHECK(os.knob_caught[1] == 1);
    in = blank();
    in.knob_moved[1] = 1, in.knob[1] = 64;
    frame(&in);
    CHECK(os.pads[0].p.end < end_before);
    CHECK(os.pads[0].p.start < os.pads[0].p.end);

    in = blank();
    in.pad_hit[1] = 100;
    in.held = 1u << IN_SAMPLE;
    int before = voices();
    frame(&in);
    CHECK(os.selected == 1); // SAMPLE + pad selects without sound
    CHECK(voices() <= before);
    in = blank();
    in.pad_hit[0] = 100;
    in.held = 1u << IN_SAMPLE;
    frame(&in);

    if (shot_dir) {
        in = blank();
        frame(&in);
        write_shot(shot_dir, "sample");
        tap(IN_B3);
        write_shot(shot_dir, "filter");
        in = blank();
        in.pad_hit[15] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
        frame(&in);
        in = blank();
        frame(&in);
        write_shot(shot_dir, "project");
        tap(IN_B1);
        tap(IN_PAD_BANK);
        in = blank();
        in.pad_hit[3] = 100, in.held = 1u << IN_SAMPLE;
        frame(&in);
        os.toast_until = 0;
        in = blank();
        frame(&in);
        write_shot(shot_dir, "empty");
        tap(IN_SAMPLE_SELECT);
        in = blank();
        frame(&in);
        write_shot(shot_dir, "browser");
        tap(IN_B1);
    }
}

static void test_browser(const char *shot_dir) {
    const char *root = "build/os/fbtest";
    system("rm -rf build/os/fbtest");
    mkdir(root, 0755);
    mkdir("build/os/fbtest/Drums", 0755);
    unsigned char b[128];
    const int16_t pcm[4] = {100, -100, 200, -200};
    size_t len = make_wav(b, 1, 16, 1, pcm, sizeof pcm, 0);
    FILE *f = fopen("build/os/fbtest/Drums/Snare.wav", "wb");
    CHECK(f != 0);
    if (!f) return;
    fwrite(b, 1, len, f);
    fclose(f);

    engine_init(&e);
    os_init(&os, &e);
    os.page = os.group_page[0] = PAGE_TRIM; // these tests start from Trim (the default is Vol)
    os.screen = OS_SAMPLE;
    os.browser_root = root;
    tap(IN_SAMPLE_SELECT);
    CHECK(os.screen == OS_BROWSER);
    CHECK(os.browser.count >= 1);
    CHECK(os.browser.entries[0].place);
    struct input in = blank();
    in.encoder_press = 1;
    frame(&in);
    CHECK(strstr(os.browser.cwd, "fbtest") != 0);
    int drums = -1;
    for (int i = 0; i < os.browser.count; i++)
        if (strcmp(os.browser.entries[i].name, "Drums") == 0) drums = i;
    CHECK(drums >= 0);
    in = blank();
    in.encoder = drums;
    frame(&in);
    CHECK(os.browser.cursor == drums);
    tap(IN_B3);
    CHECK(strstr(os.browser.cwd, "Drums") != 0);
    int snare = -1;
    for (int i = 0; i < os.browser.count; i++)
        if (strcmp(os.browser.entries[i].name, "Snare.wav") == 0) snare = i;
    CHECK(snare >= 0);
    in = blank();
    in.encoder = snare;
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "folder");
    tap(IN_B1);
    CHECK(strstr(os.browser.cwd, "Drums") == 0); // Back goes up one folder
    CHECK(os.screen == OS_BROWSER);
    CHECK(os.browser.cursor == drums); // on the folder just left, not the top
    tap(IN_B3);
    CHECK(os.browser.cursor == snare); // and the folder keeps its place
    in = blank();
    in.encoder = -snare;
    frame(&in); // up the list to ..
    tap(IN_B3); // .. goes up, on Drums
    CHECK(strstr(os.browser.cwd, "Drums") == 0 && os.browser.cursor == drums);
    tap(IN_B3);
    CHECK(os.browser.cursor == snare); // the file last on, not ..
    tap(IN_B1);
    tap(IN_B1); // the storage list, on the card just left
    CHECK(os.browser.cwd[0] == 0 && os.browser.entries[os.browser.cursor].place);
    tap(IN_B3);
    CHECK(os.browser.cursor == drums);
    tap(IN_B3);
    tap(IN_B3);
    CHECK(os.screen == OS_SAMPLE);
    CHECK(os.pads[0].s && os.pads[0].s->frames == 4);
    CHECK(strcmp(os.pads[0].name, "Snare") == 0);

    // Coming back opens the same folder on the same file.
    tap(IN_SAMPLE_SELECT);
    CHECK(os.screen == OS_BROWSER);
    CHECK(strstr(os.browser.cwd, "Drums") != 0);
    CHECK(os.browser.cursor == snare);
    CHECK(os.preview == 0); // nothing plays until the cursor has rested
    CHECK(os.preview_at == 1000 + 180);

    render(16);
    struct ui later = {.px = px, .now_ms = 2000, .track_led = -1};
    in = blank();
    os_frame(&later, &in, &os);
    CHECK(os.preview != 0);
    render(2);
    CHECK(voices() == 1);
    tap(IN_SAMPLE_SELECT); // pressed again: the top of the list
    CHECK(os.screen == OS_BROWSER && os.browser.cursor == 0);

    in = blank();
    in.down = in.held = 1u << IN_B2;
    frame(&in); // turning preview off stops it
    CHECK(os.preview_on == 0);
    CHECK(os.preview == 0);
    CHECK(voices() == 0);
    in = blank();
    in.encoder = -1;
    frame(&in);
    CHECK(os.preview_at == 0); // and scrolling stays silent while it is off

    // The SAMPLE button leaves from inside a folder and keeps that folder.
    tap(IN_SAMPLE);
    CHECK(os.screen == OS_SAMPLE);
    tap(IN_SAMPLE_SELECT);
    CHECK(strstr(os.browser.cwd, "Drums") != 0);
    tap(IN_SEQ); // SEQ goes back to the main screen too
    CHECK(os.screen == OS_MAIN);
    os.screen = OS_PROJECT;
    tap(IN_SEQ);
    CHECK(os.screen == OS_MAIN);

    // Shift + B1 makes this folder home: after a power-on Sample Select opens there.
    tap(IN_SAMPLE_SELECT);
    os.settings_path = "build/os/fbtest/settings.txt";
    in = blank();
    in.down = in.held = (1u << IN_B1) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(strstr(os.home_dir, "Drums") != 0);
    CHECK(os.screen == OS_BROWSER && strstr(os.browser.cwd, "Drums") != 0); // and does not go back a folder
    if (shot_dir) write_shot(shot_dir, "folder_home"); // the hint is gone once a home is set
    tap(IN_SAMPLE);
    engine_init(&e);
    os_init(&os, &e); // power off and on
    os.screen = OS_SAMPLE;
    os.browser_root = root;
    os.settings_path = "build/os/fbtest/settings.txt";
    os_load_settings(&os);
    CHECK(strstr(os.home_dir, "Drums") != 0);
    tap(IN_SAMPLE_SELECT);
    CHECK(os.screen == OS_BROWSER && strstr(os.browser.cwd, "Drums") != 0);
    tap(IN_B1); // the last folder of the session wins over home from here on
    tap(IN_SAMPLE);
    tap(IN_SAMPLE_SELECT);
    CHECK(strstr(os.browser.cwd, "Drums") == 0);
    tap(IN_SAMPLE);
    os.settings_path = 0;
    os.home_dir[0] = 0;

    // A folder that has disappeared falls back to the storage list.
    snprintf(os.browser_dir, sizeof os.browser_dir, "%s", "build/os/fbtest/Gone");
    tap(IN_SAMPLE_SELECT);
    CHECK(os.screen == OS_BROWSER);
    CHECK(os.browser.cwd[0] == 0);
    tap(IN_SAMPLE);
    system("rm -rf build/os/fbtest");
}

static void test_no_pad_lights(void) {
    struct input in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    // In the first seconds after power-on, Pad Lights OFF still sends every pad dark (the stock
    // app may have left one lit); after that, nothing.
    struct ui u0 = {.px = px, .now_ms = os.start_ms + 500, .track_led = -1};
    os_frame(&u0, &in, &os);
    CHECK(u0.nled == 16 && u0.led[11].r == 0 && u0.led[11].g == 0 && u0.led[11].b == 0);
    in = blank();
    struct ui u = {.px = px, .now_ms = os.start_ms + 60000, .track_led = -1};
    os_frame(&u, &in, &os);
    CHECK(u.nled == 0);
}

static int fake_press, off_calls, forced_calls;

static int fake_power_pressed(void) {
    int pressed = fake_press;
    fake_press = 0;
    return pressed;
}

static void fake_power_off(int force) {
    if (force) forced_calls++;
    else off_calls++;
}

// USB DATA MODE, with a stand-in for the device side.
static int um_state = USBM_IDLE, um_flags, um_elapsed, um_begun[3], um_refuse;
static char um_msg[96];

static int um_begin(int op) {
    if (um_refuse) return -1;
    um_begun[op]++;
    um_state = USBM_WORKING, um_elapsed = 0;
    return 0;
}
static int um_get(int *flags, char *msg, size_t n) {
    if (flags) *flags = um_state == USBM_CONNECTED ? um_flags : 0;
    if (msg && n) snprintf(msg, n, "%s", um_msg);
    return um_state;
}
static int um_ms(void) {
    return um_elapsed;
}

static void test_usbmode(const char *shot_dir) {
    static const struct usbmode_hooks hooks = {um_begin, um_get, um_ms};
    engine_init(&e);
    os_init(&os, &e);
    os.screen = OS_MAIN;
    os.usbmode = &hooks;
    mkdir("build/os/umtest", 0755);
    os.data_dir = "build/os/umtest";
    um_state = USBM_IDLE, um_flags = 0, um_msg[0] = 0, um_refuse = 0;
    memset(um_begun, 0, sizeof um_begun);
    struct sample *smp = constant(44100);
    os_pad_load(&os, 0, smp, "Hat");
    os.pads[0].p.gate = 1;

    // Save/Load, Mode USB DATA, <Start>.
    disk_open(&os, DISK_USB, DISK_PROJECT);
    struct input in = blank();
    frame(&in);
    CHECK(os.screen == OS_DISK);
    if (shot_dir) write_shot(shot_dir, "usb_data_start");
    in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    CHECK(voices() == 1);
    tap(IN_B3);
    CHECK(os.screen == OS_USBMODE && um_begun[USBM_CONNECT] == 1);
    render(128), render(128);
    CHECK(voices() == 0); // everything stops as it starts
    frame(&in); // "Starting..."
    CHECK(os.screen == OS_USBMODE);

    // Connected: locked. Pads, SEQ, SAMPLE and the Save/Load keys do nothing.
    um_state = USBM_CONNECTED, um_flags = USBM_CABLE | USBM_HOST;
    in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    render(128);
    CHECK(voices() == 0);
    tap(IN_SEQ), tap(IN_SAMPLE), tap(IN_PLAY), tap(IN_B1);
    CHECK(os.screen == OS_USBMODE && os.seq.mode == SEQ_STOPPED);
    if (shot_dir) write_shot(shot_dir, "usb_data_connected");

    // <Exit> before the computer ejects: asked first. <No> stays, <Yes> goes.
    tap(IN_B3);
    CHECK(os.usbm_ask == 1 && um_begun[USBM_EXIT] == 0);
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "usb_data_ask");
    tap(IN_B1);
    CHECK(os.usbm_ask == 0 && os.screen == OS_USBMODE && um_begun[USBM_EXIT] == 0);
    tap(IN_B3);
    tap(IN_B3);
    CHECK(um_begun[USBM_EXIT] == 1 && os.screen == OS_USBMODE);

    // A step that hangs: after 90 s <Leave> gets out regardless.
    tap(IN_B3);
    CHECK(os.screen == OS_USBMODE); // not yet
    um_elapsed = 91000;
    tap(IN_B3);
    CHECK(os.screen == OS_DISK);
    um_state = USBM_CONNECTED, um_flags = USBM_CABLE; // and if that step ends connected after all
    in = blank();
    frame(&in);
    CHECK(os.screen == OS_USBMODE); // locked again
    um_state = USBM_IDLE;
    frame(&in);
    CHECK(os.screen == OS_DISK);

    // Again, ejected on the computer: <Exit> goes at once; then the card back, <OK>.
    um_state = USBM_IDLE;
    tap(IN_B3);
    CHECK(os.screen == OS_USBMODE && um_begun[USBM_CONNECT] == 2);
    um_state = USBM_CONNECTED, um_flags = USBM_CABLE | USBM_HOST | USBM_EJECTED;
    tap(IN_B3);
    CHECK(um_begun[USBM_EXIT] == 2 && os.usbm_ask == 0);
    um_state = USBM_DONE;
    in = blank();
    frame(&in);
    CHECK(os.screen == OS_USBMODE);
    tap(IN_B3);
    CHECK(os.screen == OS_DISK);

    // No cable: <Exit> asks nothing.
    tap(IN_B3);
    um_state = USBM_CONNECTED, um_flags = 0;
    tap(IN_B3);
    CHECK(um_begun[USBM_EXIT] == 3 && os.usbm_ask == 0);

    // Errors: <Repair>, then <OK>.
    um_state = USBM_ERRORS;
    snprintf(um_msg, sizeof um_msg, "The card has errors");
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "usb_data_errors");
    tap(IN_B1);
    CHECK(um_begun[USBM_REPAIR] == 1 && os.screen == OS_USBMODE);
    um_state = USBM_DONE, um_msg[0] = 0;
    tap(IN_B3);
    CHECK(os.screen == OS_DISK);

    // Could not start: back to Save/Load, nothing changed.
    tap(IN_B3);
    um_state = USBM_FAILED;
    snprintf(um_msg, sizeof um_msg, "The card is in use");
    in = blank();
    frame(&in);
    tap(IN_B3);
    CHECK(os.screen == OS_DISK);
    um_msg[0] = 0;

    // Still busy from before: <Start> refused, not locked.
    um_refuse = 1;
    tap(IN_B3);
    CHECK(os.screen == OS_DISK);
    um_refuse = 0;

    // The power key still works, and <No> comes back to the page.
    os.power_pressed = fake_power_pressed;
    os.power_off = fake_power_off;
    fake_press = 0;
    tap(IN_B3);
    um_state = USBM_CONNECTED, um_flags = USBM_CABLE | USBM_HOST;
    fake_press = 1;
    in = blank();
    frame(&in);
    CHECK(os.screen == OS_SHUTDOWN);
    if (shot_dir) write_shot(shot_dir, "usb_data_power");
    tap(IN_B1);
    CHECK(os.screen == OS_USBMODE);
    os.power_pressed = 0, os.power_off = 0;

    // Without the device side (the tests elsewhere): <Start> says so.
    os.usbmode = 0;
    um_state = USBM_IDLE;
    disk_open(&os, DISK_USB, DISK_PROJECT);
    tap(IN_B3);
    CHECK(os.screen == OS_DISK);
    disk_open(&os, DISK_SAVE, DISK_PROJECT);
}

static void power_frame(int64_t now, struct input *in) {
    struct ui u = {.px = px, .now_ms = now, .track_led = -1};
    os_frame(&u, in, &os);
    render(128);
}

static int audio_polls;

static void fake_audio_poll(void) {
    audio_polls++;
}

static void test_audio_poll(void) {
    engine_init(&e);
    os_init(&os, &e);
    os.page = os.group_page[0] = PAGE_TRIM; // these tests start from Trim (the default is Vol)
    os.screen = OS_SAMPLE;
    os.audio_poll = fake_audio_poll;
    audio_polls = 0;
    struct input in = blank();
    power_frame(1000, &in);
    CHECK(audio_polls == 1);
    in = blank();
    power_frame(1100, &in); // too soon to look at the jacks again
    CHECK(audio_polls == 1);
    in = blank();
    power_frame(1300, &in);
    CHECK(audio_polls == 2);
    os.audio_poll = 0;
}

static void test_shutdown(const char *shot_dir) {
    engine_init(&e);
    os_init(&os, &e);
    os.page = os.group_page[0] = PAGE_TRIM; // these tests start from Trim (the default is Vol)
    os.screen = OS_SAMPLE;
    os.power_pressed = fake_power_pressed;
    os.power_off = fake_power_off;
    off_calls = forced_calls = fake_press = 0; // a clean start: the key not pressed
    struct sample *s = constant(44100);
    os_pad_load(&os, 0, s, "Hat");
    os.pads[0].p.gate = 1;

    struct input in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    power_frame(1000, &in);
    CHECK(voices() == 1);
    if (voices() != 1) printf("  voices %d\n", voices());

    fake_press = 1;
    in = blank();
    power_frame(1000, &in);
    CHECK(os.screen == OS_SHUTDOWN);
    CHECK(os.sounding[0] == -1);
    for (int i = 0; i < 3; i++) render(128);
    CHECK(voices() == 0); // nothing keeps sounding behind the prompt
    if (shot_dir) write_shot(shot_dir, "shutdown");

    in = blank();
    in.pad_hit[1] = 100; // pads do nothing on the prompt
    power_frame(1000, &in);
    CHECK(os.screen == OS_SHUTDOWN);
    CHECK(voices() == 0);

    in = blank();
    in.down = in.held = 1u << IN_B1;
    power_frame(1000, &in);
    CHECK(os.screen == OS_SAMPLE); // No goes back
    CHECK(off_calls == 0);

    fake_press = 1;
    in = blank();
    power_frame(2000, &in);
    in = blank();
    in.down = in.held = 1u << IN_B3;
    power_frame(2000, &in);
    CHECK(os.shutdown_at == 2000);
    CHECK(off_calls == 0); // the message reaches the screen first
    in = blank();
    power_frame(2100, &in);
    CHECK(off_calls == 1);
    in = blank();
    power_frame(2300, &in);
    CHECK(off_calls == 1); // asked only once
    CHECK(forced_calls == 0);
    in = blank();
    power_frame(2000 + OS_POWER_FORCE_MS, &in);
    CHECK(forced_calls == 1);
    in = blank();
    power_frame(2100 + OS_POWER_FORCE_MS, &in);
    CHECK(forced_calls == 1);
    CHECK(os.screen == OS_SHUTDOWN);
    os.power_pressed = 0;
}

// ---- sequencer ----

#define TPF (120.0 / 60.0 * SEQ_PPQ / ENGINE_RATE) // ticks per frame at the default 120 BPM

static void seq_fresh(void) {
    engine_init(&e);
    os_init(&os, &e);
    os.page = os.group_page[0] = PAGE_TRIM; // these tests start from Trim (the default is Vol)
    os.screen = OS_MAIN;
    os.seq.count_in = 0; // on by default; these tests start recording at once
}

static void load_pad(int slot) {
    os_pad_load(&os, slot, constant(44100), "Hit");
    render(16); // the audio thread learns the pad
}

static void render_until_tick(double tick) {
    for (int guard = 0; os.seq.pos < tick && guard < 100000; guard++) render(128);
}

// Renders until the play position jumps back: the sequence has looped or changed.
static void render_until_wrap(void) {
    double last = os.seq.pos;
    for (int guard = 0; guard < 100000; guard++) {
        render(128);
        if (os.seq.pos < last) return;
        last = os.seq.pos;
    }
}

static unsigned char midi_log[64][3];
static int midi_count;

static void fake_midi(const unsigned char *m, int n) {
    if (midi_count < 64 && n >= 2) {
        midi_log[midi_count][0] = m[0];
        midi_log[midi_count][1] = m[1];
        midi_log[midi_count][2] = n > 2 ? m[2] : 0;
        midi_count++;
    }
}

static void test_seq_sample_accurate(void) {
    seq_fresh();
    load_pad(0);
    seq_add_note(&os.seq, 0, 0, SEQ_PPQ, 0, 127, 24); // beat 2: 0.5 s, frame 22050 at 120 BPM
    seq_play(&os.seq, 1);
    int quiet = 1;
    for (int done = 0; done + 128 <= 22016; done += 128) {
        render(128);
        if (voices() != 0) quiet = 0;
    }
    CHECK(quiet);
    render(128); // frames 22016..22143: the note starts on frame 34 of this block
    CHECK(block[33 * 2] == 0.0f);
    CHECK(block[34 * 2] > 0.1f);
}

static void test_seq_loop_and_stop(void) {
    seq_fresh();
    load_pad(0);
    os.seq.sequence[0].bars = 1;
    seq_add_note(&os.seq, 0, 0, 0, 0, 127, 24);
    seq_play(&os.seq, 1);
    render(128);
    CHECK(voices() == 1);
    for (int i = 0; i < 400; i++) render(128); // past the sample's one second
    CHECK(voices() == 0);
    render_until_wrap(); // the bar ends and loops to bar 1
    CHECK(voices() == 1);
    CHECK(os.seq.mode == SEQ_PLAYING);

    seq_fresh();
    os.seq.sequence[0].bars = 1;
    os.seq.sequence[0].loop_bar = 0; // Loop OFF plays once and stops
    seq_play(&os.seq, 1);
    for (int i = 0; i < 700; i++) render(128);
    CHECK(os.seq.mode == SEQ_STOPPED);
}

// A note longer than the loop (MPC 2500 projects have them): its release, still waiting when the
// next pass plays the pad again, must not cut the new note off. The new note's own release counts.
static void test_seq_long_note_next_pass(void) {
    seq_fresh();
    load_pad(0);
    os.seq.sequence[0].bars = 1; // 384 ticks
    seq_add_note(&os.seq, 0, 0, 0, 0, 127, SEQ_PPQ * 6); // a bar and a half
    seq_play(&os.seq, 1);
    render(128);
    render_until_wrap(); // pass 2: the pad again, while pass 1's release (tick 576) still waits
    render(128);
    int n = 0;
    double at = 0;
    for (int i = 0; i < os.seq.npending; i++)
        if (!os.seq.pending[i].midi) n++, at = os.seq.pending[i].at;
    CHECK(n == 1);                                 // only the new note's release
    CHECK(at > SEQ_PPQ * 4 + SEQ_PPQ * 6 - 2);     // at 384 + 576, not 576
    seq_stop(&os.seq, 1);
}

// MPC_PRJ: the long sounds of a real project, played three passes; every early end of one logged with
// why (a look at a user's project, not a check).
static void mpc_prj_playback(int q) {
    struct seq *sq = &os.seq;
    sq->current = q;
    struct seq_sequence *qq = &sq->sequence[q];
    int len = qq->bars * qq->sig_num * SEQ_PPQ * 4 / qq->sig_den;
    printf("  seq %d: %d bars, %d ticks, %.1f BPM, loop bar %d\n", q + 1, qq->bars, len, qq->tempo10 / 10.0, qq->loop_bar);
    for (int g = 0; g < 4; g++)
        for (int i = 0; i < OS_PADS; i++) {
            const struct os_pad *pad = &os.programs[g].pads[i];
            if (!pad->s || pad->s->frames < 44100 * 4) continue;
            const struct pad_params *pp = &pad->p;
            printf("    pgm%d pad %c%02d %-12s %d frames, start %d end %d, loop %d/%d, gate %d, mono %d, mute %d, "
                   "decay %d (end %d), attack %d\n", g + 1, 'A' + i / 16, i % 16 + 1, pad->name, pad->s->frames, pp->start,
                   pp->end, pp->loop_on, pp->loop, pp->gate, pp->mono, pp->mute_group, pp->decay_ms, pp->decay_end, pp->attack_ms);
        }
    for (int t = 0; t < SEQ_TRACKS; t++) {
        const struct seq_track *k = &qq->track[t];
        for (int n = 0; n < k->count; n++) {
            int slot = seq_track_slot(sq, k, k->notes[n].note);
            const struct os_pad *pad = &os.programs[slot / OS_PADS].pads[slot % OS_PADS];
            if (pad->s && pad->s->frames >= 44100 * 4)
                printf("    track %d note %d (%s): tick %u dur %u vel %d\n", t + 1, k->notes[n].note, pad->name,
                       k->notes[n].tick, k->notes[n].dur, k->notes[n].vel);
        }
    }
    render(16);
    seq_play(sq, 1);
    struct { uint32_t age; int pad; double pos; int fading; const struct sample *s; } was[ENGINE_VOICES] = {{0}};
    double secs_per_tick = 60.0 / (qq->tempo10 / 10.0) / SEQ_PPQ;
    int max_voices = 0;
    for (long b = 0; b < (long)(3.0 * len * secs_per_tick * 44100 / 128) + 200; b++) {
        render(128);
        int nv = voices();
        if (nv > max_voices) max_voices = nv;
        for (int i = 0; i < ENGINE_VOICES; i++) {
            const struct voice *v = &e.voice[i];
            if (was[i].s && was[i].s->frames >= 44100 * 4) {
                int same = v->stage != 0 && v->age == was[i].age;
                double left = (double)was[i].s->frames - was[i].pos;
                if ((!same || (v->fading && !was[i].fading)) && left > 4410) {
                    double at = b * 128.0 / 44100.0, tick = at / secs_per_tick;
                    const char *why = !same ? "voice taken (stolen)" : "fading";
                    // who started just now
                    uint32_t newest = 0; int np = -1;
                    for (int j = 0; j < ENGINE_VOICES; j++)
                        if (e.voice[j].stage && e.voice[j].age >= newest) newest = e.voice[j].age, np = e.voice[j].pad;
                    printf("    CUT %.2fs pass %d bar %.2f: pad %d (%.1fs of sound left) %s; %d voices; newest voice pad %d\n",
                           at, (int)(tick / len) + 1, fmod(tick, len) / (SEQ_PPQ * 4) + 1, was[i].pad, left / 44100, why, nv, np);
                }
            }
            was[i].age = v->age, was[i].pad = v->pad, was[i].pos = v->pos, was[i].fading = v->fading;
            was[i].s = v->stage ? v->s : 0;
        }
    }
    printf("    most voices at once: %d of %d\n", max_voices, ENGINE_VOICES);
    seq_stop(sq, 1);
}

static void test_seq_correct(void) {
    struct seq *q = &os.seq;
    seq_fresh();
    int len = SEQ_PPQ * 8;
    q->tc = TC_16;
    CHECK(seq_correct(q, 100, len) == 96);
    CHECK(seq_correct(q, 11, len) == 0);
    CHECK(seq_correct(q, 13, len) == 24);
    CHECK(seq_correct(q, len - 5, len) == 0); // rounds onto the next pass
    q->swing = 66; // the second 16th of each pair lands at 66% of the 8th
    CHECK(seq_correct(q, 24, len) == 32);
    CHECK(seq_correct(q, 48, len) == 48);
    CHECK(seq_correct(q, 70, len) == 80);
    q->tc = TC_8;
    q->swing = 75;
    CHECK(seq_correct(q, 48, len) == 72);
    q->tc = TC_16T; // triplets never swing
    CHECK(seq_correct(q, 17, len) == 16);
    q->tc = TC_OFF;
    CHECK(seq_correct(q, 17, len) == 17);
    q->tc = TC_16;
    q->swing = 50;
    q->shift_ticks = 2;
    CHECK(seq_correct(q, 0, len) == 2);
    q->shift_later = 0;
    CHECK(seq_correct(q, 0, len) == (uint32_t)len - 2);
}

static void test_seq_record(void) {
    seq_fresh();
    load_pad(0);
    struct seq_track *k = &os.seq.sequence[0].track[0];
    seq_add_note(&os.seq, 0, 0, 300, 7, 100, 24); // already on the track
    os.seq.metronome = METRO_OFF; // only the pads should make voices here
    seq_rec_button(&os.seq);
    CHECK(os.seq.rec_arm == REC_ARM_OVERDUB); // overdub is the first press
    seq_rec_button(&os.seq);
    CHECK(os.seq.rec_arm == REC_ARM_REC);
    seq_play(&os.seq, 1);
    CHECK(os.seq.mode == SEQ_RECORDING);
    render_until_tick(100);
    CHECK(seq_record_hit(&os.seq, 5, 100) == 0);
    CHECK(k->count == 2 && k->notes[0].tick == 96 && k->notes[0].note == 5);
    double start = os.seq.pos;
    render_until_tick(start + 48);
    seq_record_release(&os.seq, 5);
    CHECK(k->notes[0].dur >= 47 && k->notes[0].dur <= 50);
    render_until_tick(400); // REC replaces what it passes on the first pass
    CHECK(k->count == 1);
    render_until_wrap();
    CHECK(os.seq.mode == SEQ_OVERDUB); // and overdubs from the second pass, as on the MPC3000
    render_until_tick(200);
    CHECK(k->count == 1); // overdub keeps what is there

    // A hit corrected ahead of the play position sounded live, so it is not played again this pass.
    render_until_tick(SEQ_PPQ * 3 - 10);
    int before = voices();
    CHECK(seq_record_hit(&os.seq, 0, 100) == 0);
    struct seq_note *n = 0;
    for (int i = 0; i < k->count; i++)
        if (k->notes[i].tick == SEQ_PPQ * 3) n = &k->notes[i];
    CHECK(n && n->fresh);
    render_until_tick(SEQ_PPQ * 3 + 5);
    CHECK(voices() == before);
    CHECK(n && !n->fresh);
    seq_stop(&os.seq, 1);
    CHECK(os.seq.mode == SEQ_STOPPED && os.seq.rec_arm == REC_ARM_OFF && os.seq.pos == 0);
}

static void test_seq_count_in_and_click(void) {
    seq_fresh();
    os.seq.count_in = 1;
    seq_rec_button(&os.seq);
    CHECK(os.seq.rec_arm == REC_ARM_OVERDUB);
    seq_play(&os.seq, 1);
    CHECK(os.seq.mode == SEQ_COUNT_IN);
    CHECK(os.seq.pos == -SEQ_PPQ * 4);
    render(128);
    CHECK(voices() == 1); // the count-in clicks
    for (int i = 0; i < 700; i++) render(128); // one bar is two seconds
    CHECK(os.seq.mode == SEQ_OVERDUB);

    seq_fresh();
    os.seq.metronome = METRO_OFF;
    seq_play(&os.seq, 1);
    render(128);
    CHECK(voices() == 0);
    seq_stop(&os.seq, 1);
    os.seq.metronome = METRO_ON;
    seq_play(&os.seq, 1);
    render(128);
    render(128);
    CHECK(voices() == 1);
}

static void test_seq_erase_and_catch_up(void) {
    seq_fresh();
    load_pad(0);
    struct seq_track *k = &os.seq.sequence[0].track[0];
    for (int t = 0; t < 4; t++) seq_add_note(&os.seq, 0, 0, (uint32_t)(t * SEQ_PPQ), 0, 100, 24);
    seq_add_note(&os.seq, 0, 0, SEQ_PPQ / 2, 3, 100, 24);
    seq_play(&os.seq, 1);
    render(16);
    seq_set_erase(&os.seq, 0);
    render_until_tick(SEQ_PPQ * 4);
    CHECK(k->count == 2); // only the pad held with ERASE goes; the first one had already played
    seq_set_erase(&os.seq, -1);

    seq_fresh();
    load_pad(0);
    seq_add_note(&os.seq, 0, 0, 10, 0, 100, 24);
    seq_play(&os.seq, 1);
    pthread_mutex_lock(&os.seq.lock); // the UI holds the lock across the note
    for (int i = 0; i < 20; i++) render(128);
    CHECK(voices() == 0);
    pthread_mutex_unlock(&os.seq.lock);
    render(128); // the next block catches up and plays it
    CHECK(voices() == 1);
    CHECK(os.seq.pos > 10);
}

static void test_seq_midi_track(void) {
    seq_fresh();
    midi_count = 0;
    os.seq.midi_send = fake_midi;
    struct seq_track *k = &os.seq.sequence[0].track[0];
    k->type = TRACK_MIDI;
    k->channel = 2;
    k->program = 5;
    seq_add_note(&os.seq, 0, 0, 0, 60, 100, 24);
    seq_play(&os.seq, 1);
    CHECK(midi_count == 1 && midi_log[0][0] == 0xc1 && midi_log[0][1] == 4); // program change first
    render(128);
    CHECK(midi_count == 2 && midi_log[1][0] == 0x91 && midi_log[1][1] == 60 && midi_log[1][2] == 100);
    CHECK(voices() == 0); // a MIDI track does not play the pads
    render_until_tick(30);
    CHECK(midi_count == 3 && midi_log[2][0] == 0x81 && midi_log[2][1] == 60);

    // Pads on the main screen with a MIDI track send their notes and record them.
    seq_stop(&os.seq, 1);
    midi_count = 0;
    os.seq.rec_arm = REC_ARM_OVERDUB;
    seq_play(&os.seq, 1);
    render_until_tick(100);
    struct input in = blank();
    in.pad_hit[0] = 90, in.pad_down[0] = 1;
    frame(&in);
    int found = 0;
    for (int i = 0; i < midi_count; i++)
        if (midi_log[i][0] == 0x91 && midi_log[i][1] == seq_pad_note(0) && midi_log[i][2] == 90) found = 1;
    CHECK(found);
    CHECK(k->count == 2);
    in = blank();
    in.pad_release[0] = 1;
    frame(&in);
    CHECK(midi_log[midi_count - 1][0] == 0x81);
    os.seq.midi_send = 0;
}

static void test_seq_next_sequence(void) {
    seq_fresh();
    os.seq.sequence[0].bars = 1;
    seq_play(&os.seq, 1);
    render(128);
    struct input in = blank();
    in.encoder = 1; // the cursor starts on Seq
    frame(&in);
    CHECK(os.seq.current == 0 && os.seq.next == 1);
    render_until_wrap();
    CHECK(os.seq.current == 1 && os.seq.next == -1);
}

static void test_seq_screens(const char *shot_dir) {
    seq_fresh();
    load_pad(0);
    struct input in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "main_play");
    tap(IN_SEQ); // SEQ jumps to the top of Track Data
    CHECK(os.screen == OS_MAIN && os.cursor_play == 6);
    tap(IN_B3);
    CHECK(os.seq.track == 1);
    tap(IN_B2);
    CHECK(os.seq.track == 0);
    tap(IN_B1);
    CHECK(os.seq.sequence[0].track[0].on == 0);
    tap(IN_B1);
    CHECK(os.seq.sequence[0].track[0].on == 1);
    in = blank();
    in.encoder_press = 1;
    frame(&in); // to Type
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.seq.sequence[0].track[0].type == TRACK_MIDI);
    frame(&in);
    CHECK(os.seq.sequence[0].track[0].type == TRACK_DRUM);
    if (shot_dir) write_shot(shot_dir, "main_track");
    tap(IN_SEQ); // on to Pgm:
    CHECK(os.cursor_play == 11);
    tap(IN_SEQ); // and back to the top of Play/Record
    CHECK(os.cursor_play == 0);
    tap(IN_SEQ);
    tap(IN_SAMPLE);
    CHECK(os.screen == OS_SAMPLE);
    tap(IN_SEQ); // from elsewhere, SEQ opens the main screen on Seq
    CHECK(os.screen == OS_MAIN && os.cursor_play == 0);

    in = blank();
    in.encoder_press = 1;
    frame(&in); // to BPM
    in = blank();
    in.encoder = 2;
    frame(&in);
    CHECK(os.seq.sequence[0].tempo10 == 1220);
    in.shift = 1, in.held = 1u << IN_SHIFT;
    in.encoder = -1;
    frame(&in);
    CHECK(os.seq.sequence[0].tempo10 == 1219);

    // Arm, play, hit a pad: it sounds and lands in the track.
    tap(IN_SEQ_REC);
    CHECK(os.seq.rec_arm == REC_ARM_OVERDUB);
    tap(IN_PLAY);
    CHECK(os.seq.mode == SEQ_OVERDUB);
    render_until_tick(50);
    in = blank();
    in.pad_hit[0] = 110, in.pad_down[0] = 1;
    frame(&in);
    CHECK(os.seq.sequence[0].track[0].count == 1);
    CHECK(voices() >= 1);
    in = blank();
    in.pad_release[0] = 1;
    frame(&in);
    tap(IN_SEQ_REC); // punch out
    CHECK(os.seq.mode == SEQ_PLAYING);
    if (shot_dir) write_shot(shot_dir, "main_playing");
    tap(IN_STOP);
    CHECK(os.seq.mode == SEQ_STOPPED);
    in = blank();
    in.down = in.held = 1u << IN_PLAY, in.shift = 1;
    in.held |= 1u << IN_SHIFT;
    frame(&in); // Shift + PLAY continues from where it stopped
    CHECK(os.seq.mode == SEQ_PLAYING && os.seq.pos > 0);
    tap(IN_STOP);

    // Timing Correct: Shift + Pad 14, then Move Existing on the whole track.
    struct seq_track *k = &os.seq.sequence[0].track[0];
    k->count = 0;
    seq_add_note(&os.seq, 0, 0, 5, 0, 100, 24);
    seq_add_note(&os.seq, 0, 0, 30, 1, 100, 24);
    in = blank();
    in.pad_hit[13] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.screen == OS_TC);
    CHECK(os.tc_to == (uint32_t)seq_length(&os.seq.sequence[0]));
    in = blank();
    os.toast_until = 0; // the shot without the message
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "timing_correct");
    tap(IN_B3);
    CHECK(k->notes[0].tick == 0 && k->notes[1].tick == 24);
    in = blank();
    in.encoder = 2; // the note value goes from 1/16 to 1/32 and Swing% hides
    frame(&in);
    CHECK(os.seq.tc == TC_32);
    if (shot_dir) write_shot(shot_dir, "timing_correct_32");
    // Pads pick which notes Move Existing works on: more than one, and a second hit drops one.
    k->count = 0;
    for (int note = 0; note < 4; note++) seq_add_note(&os.seq, 0, 0, (uint32_t)(note * 100 + 5), note, 100, 24);
    for (int pad = 0; pad < 3; pad++) {
        in = blank();
        in.pad_hit[pad] = 100, in.pad_down[pad] = 1;
        frame(&in);
    }
    in = blank();
    in.pad_hit[2] = 100, in.pad_down[2] = 1;
    frame(&in); // pad 3 again: off the list
    CHECK(os.tc_count == 2 && os.tc_notes[0] && os.tc_notes[1] && !os.tc_notes[2]);
    os.cursor_tc = 11; // Notes
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "timing_correct_pads");
    os.seq.tc = TC_16;
    tap(IN_B3);
    CHECK(k->notes[0].tick == 0 && k->notes[1].tick == 96); // pads 1 and 2 moved
    CHECK(k->notes[2].tick == 205 && k->notes[3].tick == 305); // pads 3 and 4 did not
    in = blank();
    in.encoder = 1;
    frame(&in); // the wheel on Notes goes back to ALL
    CHECK(os.tc_count == 0);
    tap(IN_SEQ);
    CHECK(os.screen == OS_MAIN);
}

static struct sample *counting(int frames) {
    struct sample *s = sample_new(frames, 1, ENGINE_RATE);
    for (int i = 0; i < frames; i++) s->data[i] = (float)i;
    return s;
}

static void set_zone(int start, int end) {
    os.pads[0].p.start = start;
    os.pads[0].p.end = end;
    os.pads[0].p.loop_lock = 0;
}

static void do_op(int op) {
    os.do_op = op;
    os.frame_slot = 2;
    tap(IN_B3);
}

static float at(int pad, int i) {
    return os.pads[pad].s->data[i];
}

// Stepping a pad's Sample: field goes through every sound in memory in turn, not back and forth
// between two: the pad's own sound changing must not reorder the list it steps through.
static void test_step_sound(void) {
    engine_init(&e);
    os_init(&os, &e);
    struct sample *a = constant(100), *b = constant(101), *c = constant(102), *d = constant(103);
    os_pad_load(&os, 0, a, "A");
    os_pad_load(&os, 1, b, "B");
    os_pad_load(&os, 2, c, "C");
    os_memory_add(&os, d, "D");
    const struct sample *seen[8];
    for (int i = 0; i < 8; i++) os_pad_step_sound(&os, 0, 1), seen[i] = os.pads[0].s;
    int hits[4] = {0};
    for (int i = 0; i < 8; i++) hits[seen[i] == a ? 0 : seen[i] == b ? 1 : seen[i] == c ? 2 : 3]++;
    CHECK(hits[0] == 2 && hits[1] == 2 && hits[2] == 2 && hits[3] == 2); // each twice: a full cycle, twice
    for (int i = 0; i < 4; i++) CHECK(seen[i] != seen[(i + 1) % 4] && seen[i] == seen[i + 4]);
    // and back the other way, the same cycle reversed
    const struct sample *back[4];
    for (int i = 0; i < 4; i++) os_pad_step_sound(&os, 0, -1), back[i] = os.pads[0].s;
    for (int i = 0; i < 4; i++) CHECK(back[i] == seen[(6 - i) % 4]);
    // a pad later in the bank cycles the same way
    for (int i = 0; i < 4; i++) os_pad_step_sound(&os, 2, 1), seen[i] = os.pads[2].s;
    CHECK(seen[0] != seen[1] && seen[1] != seen[2] && seen[2] != seen[3] && seen[0] != seen[2] && seen[1] != seen[3]);
}

static void test_zone_ops(const char *shot_dir) {
    engine_init(&e);
    os_init(&os, &e);
    os.page = os.group_page[0] = PAGE_TRIM; // these tests start from Trim (the default is Vol)
    os.screen = OS_SAMPLE;

    // K3 is Do: turning it takes the cursor there and picks the action.
    os_pad_load(&os, 0, counting(1000), "Count");
    struct input in = blank();
    in.knob_moved[2] = 1, in.knob[2] = 127;
    frame(&in);
    CHECK(os.frame_slot == 2 && os.do_op == DO_COUNT - 1);
    in.knob[2] = 20;
    frame(&in);
    CHECK(os.do_op == 1);
    if (shot_dir) write_shot(shot_dir, "trim_do");
    in = blank();
    in.knob_moved[1] = 1, in.knob[1] = 60;
    frame(&in); // another knob takes it back to Zone Start
    CHECK(os.frame_slot == 0);
    os.frame_slot = 2;
    tap(IN_SAMPLE);
    CHECK(os.frame_slot == 0);
    in = blank();
    in.encoder = 2;
    os.frame_slot = 2;
    os.do_op = 0;
    frame(&in); // the wheel scrolls the list too
    CHECK(os.do_op == 2);
    os.frame_slot = 0;
    tap(IN_B3); // off Do, the third key is still the Filter tab
    CHECK(os.page == PAGE_FILTER);
    tap(IN_B1);
    CHECK(os.page == PAGE_TRIM);

    set_zone(100, 200);
    os.pads[0].p.loop_lock = 0, os.pads[0].p.loop = 150;
    do_op(DO_DISCARD);
    CHECK(os.pads[0].s->frames == 100 && at(0, 0) == 100 && at(0, 99) == 199);
    CHECK(os.pads[0].p.start == 0 && os.pads[0].p.end == 100 && os.pads[0].p.loop == 50);

    os_pad_load(&os, 0, counting(1000), "Count");
    set_zone(100, 200);
    do_op(DO_EXTRACT);
    CHECK(os.pads[0].s->frames == 1000); // the original stays
    CHECK(os.extract && !os.pads[1].s); // nothing assigned until a pad is hit
    {
        struct input hit = blank();
        hit.pad_hit[4] = 100, hit.pad_down[4] = 1;
        frame(&hit);
        hit = blank();
        hit.pad_release[4] = 1;
        frame(&hit);
    }
    CHECK(!os.extract && os.pads[4].s && os.pads[4].s->frames == 100 && at(4, 0) == 100);
    CHECK(strcmp(os.pads[4].name, "Count-Ext") == 0);
    os.selected = 0;
    do_op(DO_EXTRACT); // ENTER keeps it in memory instead
    {
        struct input enter = blank();
        enter.encoder_press = 1;
        frame(&enter);
    }
    CHECK(!os.extract && os.nmemory == 1 && os.memory[0].s->frames == 100);
    { // loaded over a pad (as from the browser): the sound it had stays in memory, for Sample: and Snd:
        struct sample *was = os.pads[0].s;
        os_pad_load_over(&os, 0, counting(300), "Over");
        CHECK(os.nmemory == 2 && os.memory[1].s == was && os.pads[0].s->frames == 300);
    }

    set_zone(100, 200);
    do_op(DO_SILENCE);
    CHECK(at(0, 99) == 99 && at(0, 100) == 0 && at(0, 199) == 0 && at(0, 200) == 200);

    os_pad_load(&os, 0, counting(1000), "Count");
    set_zone(100, 200);
    do_op(DO_REVERSE);
    CHECK(at(0, 100) == 199 && at(0, 199) == 100 && at(0, 99) == 99 && at(0, 200) == 200);

    os_pad_load(&os, 0, counting(1000), "Count");
    set_zone(100, 200);
    do_op(DO_DELETE);
    CHECK(os.pads[0].s->frames == 900 && at(0, 99) == 99 && at(0, 100) == 200);

    os_pad_load(&os, 0, counting(1000), "Count");
    set_zone(0, 1000);
    do_op(DO_DELETE); // refuses to delete everything
    CHECK(os.pads[0].s->frames == 1000);

    do_op(DO_INS_START);
    CHECK(os.pads[0].s->frames == 1000); // nothing in temp yet
    set_zone(100, 200);
    do_op(DO_COPY);
    CHECK(os.temp && os.temp->frames == 100 && os.pads[0].s->frames == 1000);
    set_zone(500, 600);
    do_op(DO_INS_START);
    CHECK(os.pads[0].s->frames == 1100 && at(0, 499) == 499 && at(0, 500) == 100 && at(0, 600) == 500);
    CHECK(os.pads[0].p.start == 500 && os.pads[0].p.end == 600); // the inserted audio is the new zone

    set_zone(900, 1000); // edits to the same sample keep temp, so it can go in again
    do_op(DO_INS_END);
    CHECK(os.pads[0].s->frames == 1200 && at(0, 1000) == 100 && at(0, 1099) == 199);

    // Temp belongs to its sample: loading another sample over it deletes temp.
    os_pad_load(&os, 0, counting(1000), "Count");
    CHECK(os.temp == 0);
    set_zone(500, 600);
    do_op(DO_INS_END);
    CHECK(os.pads[0].s->frames == 1000);

    // So does picking another sample.
    set_zone(100, 200);
    do_op(DO_COPY);
    CHECK(os.temp != 0);
    in = blank();
    in.pad_hit[1] = 100, in.held = 1u << IN_SAMPLE;
    frame(&in);
    CHECK(os.selected == 1 && os.temp == 0);
    in = blank();
    in.pad_hit[0] = 100, in.held = 1u << IN_SAMPLE;
    frame(&in);
    set_zone(500, 600);
    do_op(DO_INS_START);
    CHECK(os.pads[0].s->frames == 1000);
    render(16);
    engine_reap(&e);
}

static void test_sound_params(const char *shot_dir) {
    // Also Play: pad A01 brings A02 with it, live and from the sequencer, but not A02 alone.
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    os.screen = OS_SAMPLE;
    load_pad(0);
    load_pad(1);
    os.pads[0].p.also_pad = 2;
    engine_update(&e, 0, &os.pads[0].p);
    struct input in = blank();
    in.pad_hit[0] = 127, in.pad_down[0] = 1;
    frame(&in);
    CHECK(voices() == 2);
    CHECK(atomic_load(&e.playing[0]) && atomic_load(&e.playing[1]));
    engine_kill_all(&e);
    for (int i = 0; i < 3; i++) render(128);
    in = blank();
    in.pad_hit[1] = 127, in.pad_down[1] = 1;
    frame(&in);
    CHECK(voices() == 1);
    engine_kill_all(&e);
    for (int i = 0; i < 3; i++) render(128);
    os.pads[1].p.also_pad = 1; // two pads naming each other still play once each
    engine_update(&e, 1, &os.pads[1].p);
    in = blank();
    in.pad_hit[0] = 127, in.pad_down[0] = 1;
    frame(&in);
    CHECK(voices() == 2);
    engine_kill_all(&e);
    for (int i = 0; i < 3; i++) render(128);
    seq_add_note(&os.seq, 0, 0, 0, 0, 100, 24);
    seq_play(&os.seq, 1);
    render(128);
    CHECK(voices() == 2);
    seq_stop(&os.seq, 1);

    // Dcy Md End: the level holds, then decays to silence right at the end of the sample.
    engine_init(&e);
    struct sample *s = constant(44100);
    struct pad_params p = params(44100);
    p.decay_ms = 100;
    p.decay_end = 1;
    engine_note_on(&e, 0, 127, s, &p);
    render(1024);
    CHECK(fabsf(block[1000] - 1.0f) < 1e-3f); // no decay yet
    for (int i = 0; i < 35; i++) render(1024); // to about 0.85 s
    CHECK(fabsf(block[1000] - 1.0f) < 1e-3f);
    for (int i = 0; i < 4; i++) render(1024); // inside the last 100 ms
    CHECK(block[1000] < 0.9f && block[1000] > 0.0f);
    p.decay_end = 0; // Start: the decay begins right after the attack
    engine_init(&e);
    engine_note_on(&e, 0, 127, s, &p);
    render(1024);
    render(1024);
    CHECK(block[1000] < 0.6f);

    // Vel Mod Volume 100 (the default): a hit at half strength plays at half level.
    engine_init(&e);
    p = params(44100);
    p.vel_sens = 100;
    engine_note_on(&e, 0, 64, s, &p);
    render(16);
    CHECK(fabsf(block[0] - 64.0f / 127.0f) < 1e-3f);

    // Vel Mod Sft St: the softest hit starts the full time later, the hardest at Zone Start.
    sample_free(s);
    s = ramp(44100);
    engine_init(&e);
    p = params(44100);
    p.soft_start_ms = 500;
    p.vel_sens = 0;
    engine_note_on(&e, 0, 1, s, &p);
    render(16);
    CHECK(fabsf(block[0] - 22051.0f / 44100.0f) < 1e-3f);
    engine_init(&e);
    engine_note_on(&e, 0, 127, s, &p);
    render(16);
    CHECK(fabsf(block[0] - 1.0f / 44100.0f) < 1e-4f);

    // Vel Mod Attack: a hard hit at 100 skips the attack, a soft one keeps most of it.
    sample_free(s);
    s = constant(44100);
    engine_init(&e);
    p = params(44100);
    p.attack_ms = 100;
    p.vel_attack = 100;
    engine_note_on(&e, 0, 127, s, &p);
    render(16);
    CHECK(fabsf(block[0] - 1.0f) < 1e-3f);
    engine_init(&e);
    engine_note_on(&e, 0, 1, s, &p);
    render(16);
    CHECK(block[0] < 0.01f);
    sample_free(s);

    // Holding SAMPLE turns the keys into zoom.
    seq_fresh();
    os.screen = OS_SAMPLE;
    load_pad(0);
    in = blank();
    in.down = in.held = (1u << IN_B1) | (1u << IN_SAMPLE);
    frame(&in);
    CHECK(os.zoom == 1 && os.page == PAGE_TRIM);
    frame(&in);
    CHECK(os.zoom == 2);
    in.down = (1u << IN_B2);
    frame(&in);
    CHECK(os.zoom == 1);
    if (shot_dir) write_shot(shot_dir, "zoom");
    in = blank();
    in.down = in.held = 1u << IN_B1;
    frame(&in); // without SAMPLE, B1 is the page key again
    CHECK(os.page == PAGE_MIX && os.zoom == 1);

    // Normalize is on the Do list, after Truncate.
    os.page = PAGE_TRIM;
    os.pads[0].s->data[10] = 0.5f;
    for (int i = 0; i < os.pads[0].s->frames; i++) os.pads[0].s->data[i] *= 0.5f;
    do_op(DO_NORMALIZE);
    float top = 0;
    for (int i = 0; i < os.pads[0].s->frames; i++)
        if (fabsf(os.pads[0].s->data[i]) > top) top = fabsf(os.pads[0].s->data[i]);
    CHECK(fabsf(top - 1.0f) < 1e-5f);
    render(16);
    engine_reap(&e);
}

static int fake_midi_note;

static int fake_midi_in(void) {
    int n = fake_midi_note;
    fake_midi_note = 0;
    return n;
}

static void test_metronome_and_count_in(const char *shot_dir) {
    // Count-in is on by default. Shift + Tap Tempo opens the metronome page.
    engine_init(&e);
    os_init(&os, &e);
    os.page = os.group_page[0] = PAGE_TRIM; // these tests start from Trim (the default is Vol)
    os.screen = OS_MAIN;
    CHECK(os.seq.count_in == 1 && os.seq.click_volume == 100 && os.seq.count_bars == 1);
    struct input in = blank();
    in.down = in.held = (1u << IN_TAP_TEMPO) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_METRO);
    if (shot_dir) write_shot(shot_dir, "metronome");
    in = blank();
    in.encoder = 1;
    frame(&in); // Click: ON to OFF
    CHECK(os.seq.metronome == METRO_OFF);
    frame(&in); // and back on as it was
    CHECK(os.seq.metronome == METRO_REC);
    in = blank();
    in.encoder_press = 1;
    frame(&in); // to In Play
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.seq.metronome == METRO_ON); // clicks while playing too
    os.cursor_metro = 3; // Rate
    frame(&in);
    frame(&in);
    CHECK(os.seq.click_rate == CLICK_8);
    os.cursor_metro = 4; // Audio Rec: off unless asked
    CHECK(os.seq.click_audio == 0);
    frame(&in);
    CHECK(os.seq.click_audio == 1);
    in.encoder = -1;
    frame(&in);
    CHECK(os.seq.click_audio == 0);
    in.encoder = 1;
    os.cursor_metro = 5; // After Loop: STOP unless asked
    CHECK(os.seq.click_loop == 0);
    frame(&in);
    CHECK(os.seq.click_loop == 1);
    in.encoder = -1;
    frame(&in);
    CHECK(os.seq.click_loop == 0);
    in.encoder = 1;
    os.cursor_metro = 8; // Bars
    frame(&in);
    CHECK(os.seq.count_bars == 2);
    tap(IN_B1);
    CHECK(os.screen == OS_MAIN); // <Back>

    // 1/8 clicks, two bars of count-in.
    seq_rec_button(&os.seq);
    seq_play(&os.seq, 1);
    CHECK(os.seq.mode == SEQ_COUNT_IN && os.seq.pos == -2 * SEQ_PPQ * 4);
    render(128);
    CHECK(voices() == 1);
    for (int i = 0; i < 84; i++) render(128); // to just before the next 8th, at frame 11025
    CHECK(voices() == 0);
    render(128);
    render(128);
    CHECK(voices() == 1);

    // A hit just before the downbeat lands on beat one, even with swing.
    seq_stop(&os.seq, 1);
    load_pad(0);
    os.seq.count_bars = 1;
    os.seq.swing = 66;
    os.seq.metronome = METRO_OFF;
    seq_rec_button(&os.seq);
    seq_play(&os.seq, 1);
    render_until_tick(-5);
    in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    struct seq_track *k = &os.seq.sequence[0].track[0];
    CHECK(k->count == 1 && k->notes[0].tick == 0 && k->notes[0].fresh);
    int live = voices();
    render_until_tick(10); // played live already: the downbeat does not play it twice
    CHECK(voices() <= live);
    CHECK(os.seq.mode == SEQ_OVERDUB);
    seq_stop(&os.seq, 1);

    // Wait Key: Play holds until the first pad, which starts it and lands on beat one.
    k->count = 0;
    os.seq.count_wait = 1;
    seq_rec_button(&os.seq);
    seq_play(&os.seq, 1);
    CHECK(os.seq.mode == SEQ_WAITING);
    for (int i = 0; i < 50; i++) render(128);
    CHECK(os.seq.pos == 0);
    os.screen = OS_PROJECT; // and pads record from any screen
    in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    CHECK(os.seq.mode == SEQ_OVERDUB);
    CHECK(k->count == 1 && k->notes[0].tick == 0);
    render(128);
    CHECK(os.seq.pos > 0);
    seq_stop(&os.seq, 1);

    // ... or the first note on MIDI IN.
    os.screen = OS_MAIN;
    os.midi_note_in = fake_midi_in;
    seq_rec_button(&os.seq);
    seq_play(&os.seq, 1);
    in = blank();
    frame(&in);
    CHECK(os.seq.mode == SEQ_WAITING);
    fake_midi_note = 1;
    frame(&in);
    CHECK(os.seq.mode == SEQ_OVERDUB);
    seq_stop(&os.seq, 1);
    os.midi_note_in = 0;

    // Recording works from the Sample Select screen too.
    os.seq.count_in = 0;
    k->count = 0;
    seq_rec_button(&os.seq);
    seq_play(&os.seq, 1);
    render_until_tick(50);
    os.screen = OS_BROWSER;
    in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    CHECK(k->count == 1);
    seq_stop(&os.seq, 1);
}

// The click keeps the MPC3000's shape: a ramp that peaks about 1.1 ms in, then a small
// undershoot, with the accent 4.1 times the normal click.
static void test_click_shape(void) {
    engine_init(&e);
    const struct sample *a = e.click[1], *n = e.click[0];
    int top = 0;
    float low = 0;
    for (int i = 0; i < a->frames; i++) {
        if (a->data[i] > a->data[top]) top = i;
        if (a->data[i] < low) low = a->data[i];
    }
    CHECK(top >= 44 && top <= 56);
    CHECK(fabsf(a->data[top] - 1.0f) < 1e-3f); // the accent peaks at full scale
    CHECK(low < -0.04f && low > -0.15f);
    CHECK(fabsf(a->data[top] / n->data[top] - 4.1f) < 1e-3f);
}

static void test_note_repeat(void) {
    seq_fresh();
    load_pad(0);
    os.pads[0].p.mono = 0; // POLY, so the repeats can be counted as they overlap
    engine_update(&e, 0, &os.pads[0].p);
    os.seq.metronome = METRO_OFF;
    tap(IN_NOTE_REPEAT); // pressed: on while it is held, as it stays through what follows
    CHECK(os.seq.repeat == 1);
    struct input in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in); // stopped, the grid starts with the pad: the first hit is now
    CHECK(voices() == 1);
    CHECK(os.sounding[0] == -1); // the pad itself is not played on top
    for (int i = 0; i < 44; i++) render(128); // a 16th later
    CHECK(voices() == 2);
    in = blank();
    in.pad_pressure[0] = 40, in.pad_down[0] = 1;
    frame(&in);
    CHECK(os.seq.rep[0].vel == 40); // harder or softer pressure changes the level
    in = blank();
    in.pad_release[0] = 1;
    frame(&in);
    CHECK(os.seq.rep[0].on == 0);
    int now = voices();
    for (int i = 0; i < 100; i++) render(128);
    CHECK(voices() <= now);

    // Recording: repeats land on Note Repeat's rate, 1/16 to start with, swung.
    os.seq.tc = TC_16;
    os.seq.swing = 66;
    os.seq.rec_arm = REC_ARM_OVERDUB;
    seq_play(&os.seq, 1);
    in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    render_until_tick(100);
    in = blank();
    in.pad_down[0] = 1;
    frame(&in); // takes in what the audio thread recorded
    struct seq_track *k = &os.seq.sequence[0].track[0];
    CHECK(k->count == 5);
    static const uint32_t want[5] = {0, 32, 48, 80, 96};
    for (int i = 0; i < 5 && i < k->count; i++) CHECK(k->notes[i].tick == want[i]);
    in = blank();
    in.pad_release[0] = 1;
    frame(&in);
    seq_stop(&os.seq, 1);

    // Let go, it stops: even a quick tap is not left on.
    in = blank();
    in.up = 1u << IN_NOTE_REPEAT;
    frame(&in);
    CHECK(os.seq.repeat == 0);
    tap(IN_NOTE_REPEAT);
    CHECK(os.seq.repeat == 1);
    frame(&in);
    CHECK(os.seq.repeat == 0);
}

static uint32_t noise_seed = 1;

static float noise(void) {
    noise_seed = noise_seed * 1664525u + 1013904223u;
    return (float)(noise_seed >> 8) / 8388608.0f - 1.0f;
}

// Silence, then kick, snare, kick, snare.
static struct sample *drum_loop(const int32_t *hits) {
    struct sample *s = sample_new(ENGINE_RATE * 2, 1, ENGINE_RATE);
    memset(s->data, 0, (size_t)s->frames * sizeof(float));
    for (int h = 0; h < 4; h++) {
        for (int32_t i = 0; i < ENGINE_RATE / 4 && hits[h] + i < s->frames; i++) {
            float t = (float)i / ENGINE_RATE, v;
            if (h % 2 == 0) v = 0.9f * sinf(2 * 3.14159265f * 60 * t) * expf(-t / 0.12f);
            else v = 0.6f * noise() * expf(-t / 0.08f);
            s->data[hits[h] + i] += v;
        }
    }
    return s;
}

static void test_onsets(void) {
    static const int32_t hits[4] = {8820, 30870, 52920, 74970};
    struct sample *s = drum_loop(hits);
    int32_t on[ONSETS_MAX];
    int n = onset_detect(s, on, ONSETS_MAX);
    CHECK(n == 5);
    CHECK(on[0] == 0);
    for (int h = 0; h < 4 && h + 1 < n; h++) {
        // A few milliseconds of air before each hit, never after it.
        CHECK(on[h + 1] <= hits[h] && on[h + 1] >= hits[h] - 300);
    }
    if (n != 5)
        for (int i = 0; i < n; i++) printf("  onset %d at %d\n", i, on[i]);

    // The fader steps Start through them on Trim, and moves the framed value elsewhere.
    engine_init(&e);
    os_init(&os, &e);
    os.page = os.group_page[0] = PAGE_TRIM; // these tests start from Trim (the default is Vol)
    os.screen = OS_SAMPLE;
    os_pad_load(&os, 0, s, "Loop");
    CHECK(os.pads[0].p.filter == FILTER_LP2); // LPF is on by default
    CHECK(os.pads[0].p.mono == 1);           // and each pad is MONO
    CHECK(os.pads[0].nonsets == n);
    struct input in = blank();
    in.fader_moved = 1, in.fader = 127;
    frame(&in);
    CHECK(os.pads[0].p.start == on[n - 1]);
    in.fader = 43; // a third of the way: the second transient
    frame(&in);
    CHECK(os.pads[0].p.start == on[(43 * (n - 1) + 63) / 127]);
    in.fader = 0;
    frame(&in);
    CHECK(os.pads[0].p.start == 0);

    tap(IN_B2); // Tune: Semi is framed
    CHECK(os.page == PAGE_TUNE);
    in = blank();
    in.fader_moved = 1, in.fader = 30; // not yet up to the value's position: nothing moves
    frame(&in);
    CHECK(os.pads[0].p.semi == 0);
    in.fader = 70; // passing through it takes over
    frame(&in);
    CHECK(os.pads[0].p.semi == -36 + 72 * 70 / 127);
    in.fader = 127;
    frame(&in);
    CHECK(os.pads[0].p.semi == 36);
}

static void fader(int value) {
    struct input in = blank();
    in.fader_moved = 1, in.fader = value;
    frame(&in);
}

static void erase_tap(void) {
    struct input in = blank();
    in.down = in.held = 1u << IN_ERASE;
    frame(&in);
    in = blank();
    in.up = 1u << IN_ERASE;
    frame(&in);
}

static void test_fader_moves(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    os.screen = OS_SAMPLE;
    load_pad(0);
    struct seq_track *k = &os.seq.sequence[0].track[0];

    // Not recording: the fader only edits.
    os.page = PAGE_MIX, os.frame_slot = 0;
    fader(127);
    CHECK(k->nauto == 0);

    // Recording on the Vol page with the cursor on Volume: moves go into the track.
    os.seq.rec_arm = REC_ARM_OVERDUB;
    seq_play(&os.seq, 1);
    render_until_tick(10);
    fader(126);
    CHECK(k->nauto == 1 && k->autos[0].param == AUTO_VOL && k->autos[0].value == 126 && k->autos[0].pad == 0);
    render_until_tick(100);
    fader(40);
    CHECK(k->nauto == 2);

    // The next pass, moving it again replaces what it passes over instead of adding to it.
    render_until_wrap();
    render_until_tick(20);
    fader(60);
    CHECK(k->nauto == 2 && k->autos[0].value == 60 && k->autos[1].value == 40);

    // Played back, the moves change the pad's sound, and go out as MIDI with a channel set.
    seq_stop(&os.seq, 1);
    midi_count = 0;
    os.seq.midi_send = fake_midi;
    k->channel = 3;
    seq_play(&os.seq, 1);
    render_until_tick(30);
    CHECK(e.pad[0].p.volume == 100 * 60 / 127);
    int cc = 0;
    for (int i = 0; i < midi_count; i++)
        if (midi_log[i][0] == 0xb2 && midi_log[i][1] == 7 && midi_log[i][2] == 60) cc = 1;
    CHECK(cc);
    os.seq.midi_send = 0;
    k->channel = 0;

    // Pan, Cutoff and the Tune page record too, each only with the cursor on it.
    seq_stop(&os.seq, 1);
    seq_rec_button(&os.seq);
    seq_play(&os.seq, 1);
    render_until_tick(200);
    os.frame_slot = 1; // Pan
    fader(63);
    os.page = PAGE_FILTER, os.frame_slot = 0; // Cutoff
    fader(127);
    os.page = PAGE_TUNE, os.frame_slot = 0; // Semi
    fader(63);
    os.page = PAGE_AMP, os.frame_slot = 0; // Attack is not recorded
    fader(0);
    int seen[AUTO_PARAMS] = {0};
    for (int i = 0; i < k->nauto; i++) seen[k->autos[i].param]++;
    CHECK(seen[AUTO_PAN] == 1 && seen[AUTO_CUTOFF] == 1 && seen[AUTO_SEMI] == 1 && k->nauto == 5);
    seq_stop(&os.seq, 1);

    // ERASE with the cursor on Volume asks first: <Yes> erases Volume only.
    os.page = PAGE_MIX, os.frame_slot = 0;
    erase_tap();
    CHECK(os.erase_ask == AUTO_VOL + 1 && os.erase_this);
    if (shot_dir) write_shot(shot_dir, "erase_prompt");
    tap(IN_B3);
    CHECK(os.erase_ask == 0 && seq_auto_count(&os.seq, 0, AUTO_VOL) == 0 && k->nauto == 3);

    // With no Volume moves left, the prompt still offers <All>, which erases every value.
    erase_tap();
    CHECK(os.erase_ask == AUTO_VOL + 1 && !os.erase_this);
    tap(IN_B3); // no <Yes> to press
    CHECK(os.erase_ask != 0 && k->nauto == 3);
    tap(IN_B2);
    CHECK(os.erase_ask == 0 && k->nauto == 0);

    // With nothing recorded there is no prompt: the tap opens the Erase screen instead.
    erase_tap();
    CHECK(os.erase_ask == 0 && os.screen == OS_ERASE);
}

static void test_programs(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    os.screen = OS_MAIN;
    struct seq_track *t1 = &os.seq.sequence[0].track[0], *t2 = &os.seq.sequence[0].track[1];

    // Track 1 plays program 1, where a kick goes on pad A01.
    load_pad(0);
    CHECK(os.program == 0 && os.pads[0].s);
    // Track 2 on program 2: its pads are empty, so the pads follow the active track.
    t2->kit = 1;
    tap(IN_B3); // <Trk+>
    CHECK(os.seq.track == 1 && os.program == 1 && os.pads == os.programs[1].pads && !os.pads[0].s);
    os_pad_load(&os, 0, constant(22050), "Snare");
    render(16);
    tap(IN_B2); // back to track 1: the kick is still there
    CHECK(os.program == 0 && os.pads[0].s->frames == 44100);

    // Both tracks play at once, each from its own program.
    seq_add_note(&os.seq, 0, 0, 0, 0, 100, 24);
    seq_add_note(&os.seq, 0, 1, 0, 0, 100, 24);
    seq_play(&os.seq, 1);
    render(128);
    CHECK(voices() == 2);
    CHECK(atomic_load(&e.playing[0]) && atomic_load(&e.playing[OS_PADS]));
    seq_stop(&os.seq, 1);
    engine_kill_all(&e);
    for (int i = 0; i < 3; i++) render(128);

    // Changing the track type keeps both its program and its MIDI program change.
    t1->program = 5;
    t1->type = TRACK_MIDI;
    t1->type = TRACK_DRUM;
    CHECK(t1->kit == 0 && t1->program == 5);

    // Shift + SAMPLE opens the Program screen, but not on a MIDI track.
    struct input in = blank();
    in.down = in.held = (1u << IN_SAMPLE) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_PROGRAM);
    if (shot_dir) write_shot(shot_dir, "program");

    // Shift + <Copy Pgm> copies into the first empty program and switches the track to it. The
    // sample is shared.
    struct sample *kick = os.pads[0].s;
    in = blank();
    in.down = in.held = (1u << IN_B1) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(t1->kit == 2 && os.program == 2 && os.pads[0].s == kick && kick->refs >= 2); // and any undo steps holding it
    CHECK(strcmp(os.programs[2].name, "Program01") == 0);

    // Shift + <Empty Pgm> asks first, then empties the program. The kick stays for program 1.
    in = blank();
    in.down = in.held = (1u << IN_B3) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.pgm_ask);
    tap(IN_B3);
    CHECK(!os.pgm_ask && os_program_sounds(&os, 2) == 0 && kick->refs >= 1 && os.programs[0].pads[0].s == kick);
    render(16);
    engine_reap(&e);
    CHECK(os.programs[0].pads[0].s->frames == 44100);

    // The wheel on Pgm: picks the track's program.
    in = blank();
    in.encoder = -2;
    frame(&in);
    CHECK(t1->kit == 0 && os.program == 0);

    // A MIDI track has no program screen.
    tap(IN_SEQ);
    t1->type = TRACK_MIDI;
    in = blank();
    in.down = in.held = (1u << IN_SAMPLE) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_MAIN && strcmp(os.toast, "MIDI track: no program") == 0);
    t1->type = TRACK_DRUM;

    // Also Play and mute groups stay inside one program.
    os.pads[0].p.mute_group = 1;
    engine_update(&e, 0, &os.pads[0].p);
    tap(IN_B3); // track 2, program 2
    os.pads[0].p.mute_group = 1;
    engine_update(&e, OS_PADS, &os.pads[0].p);
    render(16);
    engine_play_pad(&e, 0, 100, 0);
    engine_play_pad(&e, OS_PADS, 100, 0);
    render(128);
    CHECK(voices() == 2); // same group number, different programs: both keep playing
}

static void test_erase_screen(const char *shot_dir) {
    seq_fresh();
    struct seq_track *t1 = &os.seq.sequence[0].track[0], *t2 = &os.seq.sequence[0].track[1];
    for (int i = 0; i < 4; i++) seq_add_note(&os.seq, 0, 0, (uint32_t)(i * SEQ_PPQ), i % 2, 100, 24); // pads 1 and 2
    seq_add_note(&os.seq, 0, 1, 0, 5, 100, 24);

    // A tap of ERASE, stopped, opens it: current sequence, active track, whole sequence, ALL.
    erase_tap();
    CHECK(os.screen == OS_ERASE && os.erase_seq == 0 && os.erase_track == 1);
    CHECK(os.erase_from == 0 && os.erase_to == (uint32_t)seq_length(&os.seq.sequence[0]) && os.erase_count == 0);
    struct input idle = blank();
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "erase");

    // Pick pad 2 only, then <Do It>: pad 1's notes and track 2 stay.
    struct input in = blank();
    in.pad_hit[1] = 100, in.pad_down[1] = 1;
    frame(&in);
    CHECK(os.erase_count == 1 && os.erase_notes[1]);
    tap(IN_B3);
    CHECK(t1->count == 2 && t1->notes[0].note == 0 && t1->notes[1].note == 0 && t2->count == 1);

    // K1 the sequence and K2 the track, each once it reaches the value.
    {
        int seq0 = os.erase_seq, trk0 = os.erase_track;
        struct input k = blank();
        k.knob_moved[0] = k.knob_moved[1] = 1, k.knob[0] = 100, k.knob[1] = 120; // far from both: nothing yet
        frame(&k);
        CHECK(os.erase_seq == seq0 && os.erase_track == trk0);
        k.knob[0] = 0, k.knob[1] = 0; // swept past both: they follow from here
        frame(&k);
        k.knob[0] = 127, k.knob[1] = 127;
        frame(&k);
        CHECK(os.erase_seq == SEQ_COUNT - 1 && os.erase_track == SEQ_TRACKS);
        k.knob[0] = 0, k.knob[1] = 0;
        frame(&k);
        CHECK(os.erase_seq == 0 && os.erase_track == 0 && os.erase_to == (uint32_t)seq_length(&os.seq.sequence[0]));
        os.erase_seq = seq0, os.erase_track = trk0, os.erase_to = (uint32_t)seq_length(&os.seq.sequence[seq0]);
    }

    // The fader picks tracks 1 to 99, never 0; the wheel reaches 0, every track.
    in = blank();
    in.fader_moved = 1, in.fader = 0;
    frame(&in);
    CHECK(os.erase_track == 1);
    in.fader = 127;
    frame(&in);
    CHECK(os.erase_track == SEQ_TRACKS);
    os.cursor_erase = 1;
    in = blank();
    in.encoder = -SEQ_TRACKS;
    frame(&in);
    CHECK(os.erase_track == 0);

    // A tick range: only bar 1, beat 1 to 3, every track, ALL notes.
    os.erase_notes[1] = 0, os.erase_count = 0;
    os.erase_to = SEQ_PPQ * 2;
    tap(IN_B3);
    CHECK(t1->count == 1 && t1->notes[0].tick == SEQ_PPQ * 2 && t2->count == 0);
    tap(IN_B1); // <Back>
    CHECK(os.screen == OS_MAIN);

    // Held with a pad while playing, ERASE still erases as it plays and opens nothing.
    seq_play(&os.seq, 1);
    in = blank();
    in.held = 1u << IN_ERASE, in.down = 1u << IN_ERASE;
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    in = blank();
    in.up = 1u << IN_ERASE;
    frame(&in);
    CHECK(os.screen == OS_MAIN);
    seq_stop(&os.seq, 1);

    // On the main screen the fader sets the field the cursor is on.
    os.cursor_play = 1; // BPM
    in = blank();
    in.fader_moved = 1, in.fader = 127;
    frame(&in);
    CHECK(os.seq.sequence[0].tempo10 == 3000);
    in.fader = 0;
    frame(&in);
    CHECK(os.seq.sequence[0].tempo10 == 300);
    os.cursor_play = 6; // Trk
    in.fader = 127;
    frame(&in);
    CHECK(os.seq.track == SEQ_TRACKS - 1);
    in.fader = 0;
    frame(&in);
    CHECK(os.seq.track == 0);
    os.cursor_play = 9; // Chn
    in.fader = 127;
    frame(&in);
    CHECK(t1->channel == 16);
    os.cursor_play = 11; // Pgm on a DRUM track: the pad program
    frame(&in);
    CHECK(t1->kit == OS_PROGRAMS - 1);
    t1->type = TRACK_MIDI; // on a MIDI track: the program change
    frame(&in);
    CHECK(t1->program == 128 && t1->kit == OS_PROGRAMS - 1);
}

// Pressing a screen's own button again puts the cursor back on its first field.
static void test_second_press(void) {
    seq_fresh();
    struct input in = blank();
    in.pad_hit[13] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.screen == OS_TC);
    os.cursor_tc = 5;
    frame(&in);
    CHECK(os.screen == OS_TC && os.cursor_tc == 0);

    in = blank();
    in.down = in.held = (1u << IN_TAP_TEMPO) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_METRO);
    os.cursor_metro = 3;
    frame(&in);
    CHECK(os.screen == OS_METRO && os.cursor_metro == 0 && os.metro_return == OS_TC);

    in = blank();
    in.down = in.held = (1u << IN_SAMPLE) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_PROGRAM);
    os.cursor_program = 1;
    frame(&in);
    CHECK(os.screen == OS_PROGRAM && os.cursor_program == 0);

    erase_tap();
    CHECK(os.screen == OS_ERASE);
    os.cursor_erase = 2;
    erase_tap();
    CHECK(os.screen == OS_ERASE && os.cursor_erase == 0);

    tap(IN_SAMPLE);
    CHECK(os.screen == OS_SAMPLE);
    os.frame_slot = 2;
    tap(IN_SAMPLE);
    CHECK(os.screen == OS_SAMPLE && os.frame_slot == 0);
    struct input idle = blank();
    frame(&idle);
}

// A sine whose loop end sits mid-cycle: Find St should land on the same phase.
static struct sample *sine(int frames, double period) {
    struct sample *s = sample_new(frames, 1, 44100);
    for (int i = 0; i < frames; i++) s->data[i] = (float)sin(2 * M_PI * i / period);
    return s;
}

static void test_loop_screen(const char *shot_dir) {
    seq_fresh();
    os.screen = OS_SAMPLE;
    os.selected = 0;
    os_pad_load(&os, 0, sine(44100, 100.0), "Sine");
    struct pad_params *p = &os.pads[0].p;
    p->end = 30050; // half a cycle in
    tap(IN_LOOP);
    CHECK(os.screen == OS_LOOP && os.cursor_loop == 0);
    struct input in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(p->loop_on == 1);
    in.encoder = -1;
    frame(&in);
    CHECK(p->loop_on == 0);

    // K1 moves Loop St (letting go of Loop Lock) and frames it.
    in = blank();
    in.knob_moved[0] = 1, in.knob[0] = 0;
    frame(&in);
    in.knob[0] = 64;
    frame(&in);
    CHECK(os.cursor_loop == 1 && p->loop_lock == 0 && p->loop > 20000 && p->loop < 30050);
    os.cursor_loop = 1;
    p->loop = 10020;
    // K3 picks Find St, B3 runs it: same phase as the end, near where it was.
    in = blank();
    in.knob_moved[2] = 1, in.knob[2] = 0;
    frame(&in);
    CHECK(os.cursor_loop == 3 && os.loop_op == 0);
    tap(IN_B3);
    CHECK(p->loop >= 10000 && p->loop <= 10100 && (p->loop - 50) % 100 == 0);
    // Find End from Loop St 10050: the end moves to the same phase too.
    p->end = 30080;
    os.loop_op = 1;
    tap(IN_B3);
    CHECK((p->end - 50) % 100 == 0 && p->end >= 29980 && p->end <= 30180);
    // X-Fade: the last frame before the end becomes the frame before Loop St.
    os.loop_op = 2;
    p->loop = 10037, p->end = 30000;
    tap(IN_B3);
    const struct sample *sm = os.pads[0].s;
    CHECK(fabsf(sm->data[p->end - 1] - sm->data[p->loop - 1]) < 1e-5f);
    struct input idle = blank();
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "loop");
    // Discard keeps Zone Start to Loop End and nothing else.
    os.loop_op = 3;
    p->start = 1000, p->loop = 10000, p->end = 20000;
    tap(IN_B3);
    CHECK(os.pads[0].s->frames == 19000 && p->start == 0 && p->loop == 9000 && p->end == 19000);
    tap(IN_LOOP); // pressed again: back to Loop
    CHECK(os.cursor_loop == 0);
    tap(IN_SAMPLE);
}

static const struct voice *voice_on(int pad) {
    for (int i = 0; i < ENGINE_VOICES; i++)
        if (e.voice[i].stage != 0 && e.voice[i].pad == pad) return &e.voice[i];
    return 0;
}

// An S600 voice holds at its sustain level until released, then fades by its release time.
// Its key and the bend wheel set its pitch.
static void test_s600_engine(void) {
    engine_init(&e);
    struct sample *s = constant(44100);
    struct pad_params p = params(44100);
    p.loop_on = 1;
    p.synth = 1, p.gate = 1; // Note On
    p.attack_ms = 0, p.decay_ms = 50, p.sustain = 50, p.release_ms = 20;
    p.filter = FILTER_LP2, p.cutoff = 100;
    p.bend_up = 2, p.bend_down = 12;
    engine_note_on_key(&e, 7, 127, 12, s, &p);
    render(128);
    const struct voice *v = voice_on(7);
    CHECK(v && v->key == 12);
    double before = v ? v->pos : 0;
    render(128);
    CHECK(v && fabs((v->pos - before) - 256.0) < 1.0); // an octave up: two frames a frame
    for (int i = 0; i < 100; i++) render(128); // 290 ms: well into the sustain
    CHECK(v && v->stage != 0 && fabsf(v->env - 0.5f) < 0.01f);
    engine_note_on_key(&e, 7, 127, 0, s, &p); // a second key on the same pad
    render(128);
    CHECK(voices() == 2);
    engine_note_off_key(&e, 7, 12); // releases only the octave
    for (int i = 0; i < 30; i++) render(128);
    CHECK(voices() == 1 && voice_on(7) && voice_on(7)->key == 0);
    engine_set_bend(&e, -8192); // all the way down: 12 semitones
    v = voice_on(7);
    render(128);
    before = v->pos;
    render(128);
    CHECK(fabs((v->pos - before) - 64.0) < 1.0);
    engine_set_bend(&e, 0);
    engine_note_off(&e, 7);
    for (int i = 0; i < 30; i++) render(128);
    CHECK(voices() == 0);
    // Note On off: one-shot. A note off does nothing; after the decay it releases on its own.
    p.gate = 0;
    engine_note_on_key(&e, 7, 127, 0, s, &p);
    render(128);
    engine_note_off(&e, 7);
    for (int i = 0; i < 4; i++) render(128);
    CHECK(voices() == 1);
    for (int i = 0; i < 100; i++) render(128); // 50 ms decay, then the 20 ms release
    CHECK(voices() == 0);
    sample_free(s);
}

// A pad's S600 settings are its own: editing them leaves its DRUM ones alone. An S600 track
// plays its notes at their tune through the S600 slots.
// Two layers: one note makes two voices through the same envelope, the second with its own
// level and tuning, playing its own sample. A note off releases both.
static void test_s600_layers(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    load_pad(0);
    load_pad(5);
    os.selected = 0;
    os_program_set_s600(&os, 0, 1); // S600 Pgm Mode on
    tap(IN_SAMPLE);
    tap(IN_B3);
    tap(IN_B3); // Main, then Layer
    CHECK(os.screen == OS_S600 && os.s600_page == 5);
    struct os_pad *pad = &os.pads[0];
    CHECK(pad->sp.layers == 1);
    struct input in = blank();
    in.encoder = 1; // Layers: 2
    frame(&in);
    CHECK(pad->sp.layers == 2);
    in = blank();
    in.knob_moved[1] = 1, in.knob[1] = 64; // K2: layer 2's Semi, caught at 0
    frame(&in);
    in.knob[1] = 85;
    frame(&in);
    CHECK(pad->sp.l2_semi == 12 && pad->p.semi == 0 && os.s600_cursor == 6);
    os.s600_cursor = 4; // Sound: pad A06
    in = blank();
    in.encoder = 6;
    frame(&in);
    CHECK(pad->sp.l2_pad == 6);
    if (shot_dir) write_shot(shot_dir, "s600_layer");
    in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    render(128);
    const struct voice *a = 0, *b = 0;
    for (int i = 0; i < ENGINE_VOICES; i++) {
        const struct voice *v = &e.voice[i];
        if (v->stage == 0 || v->pad != ENGINE_S600) continue;
        if (v->layer) b = v;
        else a = v;
    }
    CHECK(a && b && a->s == os.pads[0].s && b->s == os.pads[5].s && b->p.semi == 12 && a->p.semi == 0);
    in = blank();
    in.pad_release[0] = 1;
    frame(&in);
    for (int i = 0; i < 40; i++) render(128);
    CHECK(voice_on(ENGINE_S600) == 0);
    // A new sample on the layer's pad reaches the layer.
    os_pad_load(&os, 5, constant(100), "Short");
    render(16);
    CHECK(e.pad[ENGINE_S600].s2 == os.pads[5].s);
    struct input idle = blank();
    frame(&idle);
    os.screen = OS_MAIN;
}

static void test_s600_track(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    load_pad(0);
    struct os_pad *pad = &os.pads[0];
    int all_on = pad->sp.env1_tpl; // All On, the default: the sound as it is, held while held
    CHECK(all_on > 3 && pad->sp.attack == 0 && pad->sp.decay == 0 && pad->sp.sustain == 100 && pad->sp.release == 0 &&
          pad->sp.env2_tpl == all_on && pad->sp.f_sustain == 100);
    struct seq_track *k = &os.seq.sequence[0].track[0];
    os_program_set_s600(&os, 0, 1); // S600 Pgm Mode on
    tap(IN_SAMPLE);
    CHECK(os.screen == OS_S600 && os.s600_page == 0 && os.s600_cursor == 0);
    struct input in = blank();
    in.encoder = 2; // Template: All On (shown first), then MPC Bass, then Piano
    frame(&in);
    CHECK(pad->sp.env1_tpl == 1 && pad->sp.decay == 2000 && pad->sp.sustain == 0);
    in = blank();
    in.encoder = -2; // back to All On, the first
    frame(&in);
    CHECK(pad->sp.env1_tpl == all_on && pad->sp.sustain == 100 && pad->sp.release == 0);
    in.encoder = -1; // nothing before it
    frame(&in);
    CHECK(pad->sp.env1_tpl == all_on);
    in.encoder = 2; // MPC Bass, Piano
    frame(&in);
    CHECK(pad->sp.env1_tpl == 1);
    in.encoder = 2; // Synth Brass, then Synth Lead
    frame(&in);
    CHECK(pad->sp.env1_tpl == 3 && pad->sp.attack == 5 && pad->sp.sustain == 80 && pad->sp.release == 200);
    in.encoder = -2;
    frame(&in);
    CHECK(pad->p.decay_ms == 0 && pad->p.attack_ms == 0); // the DRUM settings did not move
    render(16);
    CHECK(e.pad[ENGINE_S600 + 0].p.synth == 1 && e.pad[ENGINE_S600 + 0].p.decay_ms == 2000);
    CHECK(e.pad[0].p.synth == 0);
    tap(IN_B3); // Main: Volume, Semi, Fine are the pad's own, on K1-K3
    CHECK(os.s600_page == 4 && os.s600_cursor == 3); // so the cursor starts on Poly
    in = blank();
    in.knob_moved[0] = 1, in.knob[0] = 127; // K1 catches Volume at the top
    frame(&in);
    in.knob[0] = 114;
    frame(&in);
    CHECK(pad->p.volume == 90 && os.s600_cursor == 0);
    tap(IN_B3); // B3 goes on to Layer, then back to Main (MULTI is an S600 MULTI program's): the DRUM pages are not reachable
    CHECK(os.screen == OS_S600 && os.s600_page == 5 && os.s600_cursor == 0);
    tap(IN_B3);
    CHECK(os.screen == OS_S600 && os.s600_page == 4);
    CHECK(pad->sp.note_on == 1);
    in = blank();
    in.down = in.held = (1u << IN_CHOP) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(pad->sp.note_on == 0 && pad->p.gate == 0);
    frame(&in);
    CHECK(pad->sp.note_on == 1);
    tap(IN_SAMPLE_SELECT); // Load Files, and back: to the S600 screen, not the Sample screen
    CHECK(os.screen == OS_BROWSER);
    tap(IN_SAMPLE);
    CHECK(os.screen == OS_S600);
    tap(IN_SAMPLE); // pressed again: Main's first field, Poly
    CHECK(os.s600_cursor == 3);

    seq_add_note(&os.seq, 0, 0, 0, 0, 100, 24);
    k->notes[0].tune = 7;
    seq_play(&os.seq, 1);
    render(256);
    const struct voice *v = voice_on(ENGINE_S600 + 0);
    CHECK(v && v->key == 7 && v->p.synth == 1);
    seq_stop(&os.seq, 1);
    render(16);
    os_program_set_s600(&os, 0, 0); // the same notes with it off: the pad's DRUM settings
    engine_kill_all(&e);
    for (int i = 0; i < 4; i++) render(128);
    seq_play(&os.seq, 1);
    render(256);
    v = voice_on(0);
    CHECK(v && v->p.synth == 0 && v->key == 7);
    seq_stop(&os.seq, 1);
    struct input idle = blank();
    frame(&idle);
    os_program_set_s600(&os, 0, 1);
    static const char *const names[5] = {"s600_env1", "s600_env2", "s600_filter", "s600_lfo", "s600_main"};
    for (int pg = 0; pg < 5; pg++) {
        os.screen = OS_S600, os.s600_page = pg, os.s600_cursor = 0;
        frame(&idle);
        if (shot_dir) write_shot(shot_dir, names[pg]);
    }
    os.screen = OS_MAIN;
}

// LEVELS turns 16 Levels on (Velocity); Shift + LEVELS picks Tune, and a pad sets the root.
static void test_levels(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    load_pad(0);
    load_pad(3);
    os.selected = 3;
    tap(IN_LEVELS);
    CHECK(os.levels_on && os.levels_type == LEVELS_VELOCITY && os.levels_pad == 3);
    CHECK(os.levels_root == 7); // Tune's root starts on A08
    if (shot_dir) { // the 16 Levels page in Velocity
        struct input shot = blank();
        shot.down = shot.held = (1u << IN_LEVELS) | (1u << IN_SHIFT), shot.shift = 1;
        frame(&shot);
        shot = blank();
        frame(&shot);
        write_shot(shot_dir, "levels_velocity");
        tap(IN_B3);
    }
    struct input in = blank();
    in.pad_hit[0] = 127, in.pad_down[0] = 1; // pad 1: the softest, of pad A04's sound
    frame(&in);
    const struct voice *v = voice_on(3);
    CHECK(v && v->key == 0 && fabsf(v->vel_gain - 7.0f / 127.0f) < 0.01f);
    in = blank();
    in.pad_release[0] = 1;
    frame(&in);

    in = blank();
    in.down = in.held = (1u << IN_LEVELS) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_LEVELS && os.levels_on && os.cursor_levels == 0); // on Type
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.levels_type == LEVELS_TUNE);
    in.encoder = -1;
    frame(&in);
    CHECK(os.levels_type == LEVELS_VELOCITY);
    tap(IN_B2);
    CHECK(os.levels_type == LEVELS_TUNE);
    in = blank();
    in.pad_hit[4] = 100, in.pad_down[4] = 1; // pad A05 is now the root
    frame(&in);
    CHECK(os.levels_root == 4);
    in = blank();
    in.pad_release[4] = 1;
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "levels");
    tap(IN_B3); // <Exit>: back where it was opened from
    CHECK(os.screen == OS_MAIN);
    engine_kill_all(&e);
    for (int i = 0; i < 4; i++) render(128);
    tap(IN_PAD_BANK); // bank B: 16 semitones further up
    in = blank();
    in.pad_hit[6] = 100, in.pad_down[6] = 1;
    frame(&in);
    v = voice_on(3);
    CHECK(v && v->key == 16 + 6 - 4);
    in = blank();
    in.pad_release[6] = 1;
    frame(&in);
    tap(IN_LEVELS); // from Tune: off
    CHECK(!os.levels_on);
    os.selected = 0; // no Sound picked on the page: the last pad played
    tap(IN_LEVELS);
    CHECK(os.levels_on && os.levels_type == LEVELS_VELOCITY && os.levels_pad == 0);
    tap(IN_LEVELS);
    tap(IN_LEVELS);
    in = blank(); // on the page, Sound: A04 picked with the wheel
    in.down = in.held = (1u << IN_LEVELS) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    os.cursor_levels = 2; // on Tune: Type, Root pad, Sound, 16 Levels
    in = blank();
    in.encoder = 3;
    frame(&in);
    CHECK(os.levels_pad == 3);
    {
        int root = os.levels_root;
        os.cursor_levels = 1; // the root pad, second
        in.encoder = 1;
        frame(&in);
        CHECK(os.levels_root == root + 1 && os.levels_pad == 3);
        in.encoder = -1;
        frame(&in);
    }
    tap(IN_B3);
    tap(IN_LEVELS); // it was on Tune: off
    CHECK(!os.levels_on);
    os.selected = 0; // another pad played: the page's Sound stays
    tap(IN_LEVELS); // then Velocity, then Tune, keeping the root that was picked
    CHECK(os.levels_on && os.levels_type == LEVELS_VELOCITY && os.levels_pad == 3);
    tap(IN_LEVELS);
    CHECK(os.levels_on && os.levels_type == LEVELS_TUNE && os.levels_root == 4);
    tap(IN_LEVELS);
    CHECK(!os.levels_on);
    // Opening the 16 Levels page turns it on. Set OFF there, it stays off after leaving.
    in = blank();
    in.down = in.held = (1u << IN_LEVELS) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_LEVELS && os.levels_on);
    os.cursor_levels = 3; // 16 Levels: ON (on Tune, the last)
    in = blank();
    in.encoder = -1;
    frame(&in);
    CHECK(!os.levels_on);
    tap(IN_B3);
    CHECK(os.screen == OS_MAIN && !os.levels_on);
}

static unsigned char midi_q[8][3];
static int midi_qn, midi_qat;
static int fake_port; // the port the fake messages arrive on: MIDI_PORT_EXT unless set
static int fake_midi_read(unsigned char m[3], int *from) {
    if (midi_qat >= midi_qn) return 0;
    memcpy(m, midi_q[midi_qat++], 3);
    if (from) *from = fake_port;
    return 1;
}
static void midi_push(int a, int b, int c) {
    midi_q[midi_qn][0] = (unsigned char)a, midi_q[midi_qn][1] = (unsigned char)b, midi_q[midi_qn][2] = (unsigned char)c;
    midi_qn++;
}

// MIDI IN on an S600 track plays the selected pad from note 60, and records the tuning.
static void test_midi_in_s600(void) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    load_pad(2);
    os.selected = 2;
    os_program_set_s600(&os, 0, 1);
    os.programs[0].inst = -1; // Instrument LAST PAD: the pad pressed last
    os.midi_in_read = fake_midi_read;
    midi_qn = midi_qat = 0;
    seq_rec_button(&os.seq);
    seq_play(&os.seq, 1);
    render_until_tick(10);
    midi_push(0x90, 72, 100);
    midi_push(0xe0, 0, 0x60); // bend up
    midi_push(0xb0, 1, 127);  // mod wheel
    struct input idle = blank();
    frame(&idle);
    const struct voice *v = voice_on(ENGINE_S600 + 2);
    CHECK(v && v->key == 12);
    CHECK(atomic_load(&e.bend) == (0x60 << 7) - 8192 && atomic_load(&e.mod) == 127);
    struct seq_track *k = &os.seq.sequence[0].track[0];
    CHECK(k->count == 1 && k->notes[0].note == 2 && k->notes[0].tune == 12);
    midi_qn = midi_qat = 0;
    midi_push(0x80, 72, 0);
    frame(&idle);
    for (int i = 0; i < 40; i++) render(128);
    CHECK(voice_on(ENGINE_S600 + 2) == 0);
    seq_stop(&os.seq, 1);
    os.midi_in_read = 0;
    engine_set_bend(&e, 0);
    engine_set_mod(&e, 0);
}

// S600 resonance rings: a step into the low-pass nearly doubles before it settles, where the
// MPC3000-style drum filter at the same setting overshoots by half.
static float step_peak(int synth) {
    engine_init(&e);
    struct sample *s = constant(44100);
    struct pad_params p = params(44100);
    p.synth = synth, p.sustain = 100, p.decay_ms = 0, p.release_ms = 10;
    p.filter = FILTER_LP2, p.cutoff = 30, p.reso = 100;
    engine_note_on(&e, 1, 127, s, &p);
    float peak = 0;
    for (int b = 0; b < 20; b++) {
        render(128);
        for (int i = 0; i < 256; i++) peak = fabsf(block[i]) > peak ? fabsf(block[i]) : peak;
    }
    for (int b = 0; b < 200; b++) render(128); // long enough for the ringing to die
    peak /= fabsf(block[0]); // overshoot, against the level it settles at
    engine_kill_all(&e);
    render(512);
    sample_free(s);
    return peak;
}

static void test_s600_resonance(void) {
    float drum = step_peak(0), synth = step_peak(1);
    CHECK(drum < 1.6f && synth > 1.85f);
}

// Shift + Pad 10 turns Timing Correct off, then back on at the note value it had.
// On the main screen the slider picks the track on any field but BPM, Chn and Pgm.
static void test_main_slider(void) {
    seq_fresh();
    os.cursor_play = 0; // Seq:
    struct input in = blank();
    in.fader_moved = 1, in.fader = 127;
    frame(&in);
    CHECK(os.seq.track == SEQ_TRACKS - 1 && os.seq.current == 0);
    os.cursor_play = 1; // BPM: the tempo, not the track
    in.fader = 0;
    frame(&in);
    CHECK(os.seq.track == SEQ_TRACKS - 1 && os.seq.sequence[0].tempo10 == 300);
    os.cursor_play = 7; // Type: the track again
    frame(&in);
    CHECK(os.seq.track == 0);
}

static void test_tc_toggle(void) {
    seq_fresh();
    os.seq.tc = TC_8T;
    struct input in = blank();
    in.pad_hit[9] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.seq.tc == TC_OFF);
    frame(&in);
    CHECK(os.seq.tc == TC_8T);
}

// Shift + Pad 9: the track mixer. Pads pick channels (bank B is 17 to 32), the slider sets the
// level, the wheel the pan, and notes the track is playing follow.
static void test_mixer(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    load_pad(0);
    struct input in = blank();
    in.pad_hit[8] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.screen == OS_MIXER && os.mixer_track == 0);
    in = blank();
    in.pad_hit[5] = 100, in.pad_down[5] = 1; // pad A06: track 6, and no sound
    frame(&in);
    CHECK(os.mixer_track == 5 && voices() == 0);
    in = blank();
    in.fader_moved = 1, in.fader = 64; // far from 100: nothing until the slider reaches it
    frame(&in);
    CHECK(os.seq.sequence[0].track[5].level == 100);
    in.fader = 127;
    frame(&in);
    in.fader = 64;
    frame(&in);
    CHECK(os.seq.sequence[0].track[5].level == 50);
    in = blank();
    in.encoder = -10;
    frame(&in);
    CHECK(os.seq.sequence[0].track[5].pan == -10);
    tap(IN_PAD_BANK); // bank B: the same place, track 22
    CHECK(os.bank == 1);
    frame(&in);
    CHECK(os.mixer_track == 21);
    in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1; // pad B01 is track 17
    frame(&in);
    CHECK(os.mixer_track == 16);
    for (int i = 0; i < 5; i++) tap(IN_PAD_BANK); // to G, then round to A: no H past track 99
    CHECK(os.bank == 6);
    tap(IN_PAD_BANK);
    CHECK(os.bank == 0);
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "mixer");
    in = blank();
    in.pad_hit[8] = 100, in.shift = 1, in.held = 1u << IN_SHIFT; // pressed again: USB Indiv Out
    frame(&in);
    CHECK(os.screen == OS_OUTS);
    in = blank(), in.pad_release[8] = 1, frame(&in);
    in = blank();
    in.pad_hit[8] = 100, in.shift = 1, in.held = 1u << IN_SHIFT; // and again: the mixer
    frame(&in);
    CHECK(os.screen == OS_MIXER);
    in = blank(), in.pad_release[8] = 1, frame(&in);
    os.mixer_track = 0;

    // Track 1 at half level: its notes play at half, and follow the slider while they sound.
    struct seq_track *k = &os.seq.sequence[0].track[0];
    k->level = 50, k->pan = 20;
    seq_add_note(&os.seq, 0, 0, 0, 0, 100, 400);
    seq_play(&os.seq, 1);
    render(256);
    const struct voice *v = voice_on(0);
    CHECK(v && v->track == 1 && v->track_cut == 50 && v->track_pan == 20);
    in = blank();
    in.fader_moved = 1, in.fader = 63; // catches at 50, then up
    frame(&in);
    in.fader = 127;
    frame(&in);
    render(16);
    CHECK(v->track_cut == 0);
    seq_stop(&os.seq, 1);
    tap(IN_SEQ);
}

// Feeds the recorder a block of a stereo sine (or silence at amp 0), as the audio thread does.
static void feed(struct recorder *r, double hz, float amp, int frames, long *phase) {
    static float buf[2 * 512];
    while (frames > 0) {
        int n = frames > 512 ? 512 : frames;
        for (int i = 0; i < n; i++) {
            float v = amp * (float)sin(2 * M_PI * hz * (double)(*phase + i) / ENGINE_RATE);
            buf[i * 2] = buf[i * 2 + 1] = v;
        }
        recorder_input(r, buf, n, *phase);
        *phase += n;
        frames -= n;
    }
}

static float sound_peak(const struct sample *s, long from) {
    float p = 0;
    for (long i = from * s->channels; i < (long)s->frames * s->channels; i++) p = fabsf(s->data[i]) > p ? fabsf(s->data[i]) : p;
    return p;
}

// The recorder: waits for the threshold, keeps the moments before it, and makes a take into
// a sound at the set rate and bits. Anti-aliasing off lets a tone above the new limit fold
// back at full strength, as an unfiltered sampler does; on, it is gone.
static void test_recorder(void) {
    static struct recorder r;
    memset(&r, 0, sizeof r);
    long ph = 0;
    atomic_store(&r.threshold, 3277); // -20 dB
    CHECK(recorder_arm(&r, 10L * ENGINE_RATE) == 0 && atomic_load(&r.state) == REC_ARMED);
    feed(&r, 1000, 0.01f, 4410, &ph); // quiet: still waiting
    CHECK(atomic_load(&r.state) == REC_ARMED && atomic_load(&r.frames) == 0);
    feed(&r, 1000, 0.5f, ENGINE_RATE, &ph);
    CHECK(atomic_load(&r.state) == REC_RECORDING && atomic_load(&r.frames) > ENGINE_RATE);
    recorder_stop(&r);
    feed(&r, 1000, 0.5f, 128, &ph);
    CHECK(atomic_load(&r.state) == REC_DONE);
    long n = atomic_load(&r.frames);
    struct rec_format f = {REC_STEREO, 22050, 12, 0, 1, 0, 0, 0};
    struct sample *s = recorder_take(&r, &f);
    CHECK(s && s->rate == 22050 && s->channels == 2 && labs(s->frames - n / 2) <= 1);
    int on_grid = 1;
    for (long i = 0; s && i < (long)s->frames * 2; i++) {
        float q = s->data[i] * 2048.0f;
        if (fabsf(q - roundf(q)) > 1e-3f) on_grid = 0;
    }
    CHECK(on_grid && s && fabsf(sound_peak(s, 0) - 2047.0f / 2048.0f) < 1e-4f); // 12-bit steps, normalized
    CHECK(atomic_load(&r.state) == REC_IDLE && atomic_load(&r.ready) == 0); // the take's memory is freed
    sample_free(s);

    // 15 kHz into 22.05 kHz: above its 11 kHz limit.
    for (int aa = 0; aa < 2; aa++) {
        atomic_store(&r.threshold, 0);
        recorder_arm(&r, 10L * ENGINE_RATE);
        recorder_service(&r);
        feed(&r, 15000, 0.5f, ENGINE_RATE / 2, &ph);
        recorder_stop(&r);
        feed(&r, 15000, 0.5f, 128, &ph);
        struct rec_format g = {REC_MONO_L, 22050, 16, aa, 0, 0, 0, 0};
        s = recorder_take(&r, &g);
        float level = s ? sound_peak(s, 2000) : 0;
        if (aa) CHECK(level < 0.02f);
        else CHECK(level > 0.4f);
        CHECK(s && s->channels == 1);
        sample_free(s);
    }

    // Pitch +12: the take sped up twice, so half the length again.
    atomic_store(&r.threshold, 0);
    recorder_arm(&r, 10L * ENGINE_RATE);
    feed(&r, 500, 0.5f, ENGINE_RATE, &ph);
    recorder_stop(&r);
    feed(&r, 500, 0.5f, 128, &ph);
    n = atomic_load(&r.frames);
    struct rec_format h = {REC_STEREO, 22050, 16, 1, 0, 12, 0, 0};
    s = recorder_take(&r, &h);
    CHECK(s && labs(s->frames - n / 4) <= 2);
    sample_free(s);
}

// A sound below 44.1 kHz plays each sample held, as a vintage converter did; Convert 44.1k
// makes a 44.1 kHz copy that sounds the same.
static void test_hold_playback(void) {
    engine_init(&e);
    struct sample *s = sample_new(64, 1, 22050);
    for (int i = 0; i < 64; i++) s->data[i] = (float)i / 64.0f;
    struct pad_params p = params(64);
    engine_note_on(&e, 2, 127, s, &p);
    render(32);
    // Left channel of frames 0-3: each sample held for two output frames, then the next.
    CHECK(fabsf(block[0] - block[2]) < 1e-6f && fabsf(block[4] - block[6]) < 1e-6f && fabsf(block[2] - block[4]) > 1e-4f);
    engine_kill_all(&e);
    render(512);
    struct sample *c = rec_convert_44k(s);
    CHECK(c && c->rate == ENGINE_RATE && c->frames == 128 && c->data[2] == s->data[1] && c->data[3] == s->data[1]);
    sample_free(c);
    sample_free(s);
}

// SAMPLE REC opens Sample New Sound. A take, once done, waits for a pad (which gets it, tuned
// back down by the Pitch) or ENTER (which keeps it in memory).
// A take goes to the pad pressed in the program being played (the active track's), never to
// program 1 when another is selected.
static void test_record_other_program(void) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    os.seq.sequence[os.seq.current].track[os.seq.track].kit = 2;
    struct input idle = blank();
    frame(&idle);
    CHECK(os.program == 2);
    tap(IN_SAMPLE_REC);
    atomic_store(&os.rec.present, 1);
    os.rec_set[0].thres_db = -61;
    tap(IN_B3);
    long ph = 0;
    feed(&os.rec, 440, 0.5f, ENGINE_RATE, &ph);
    frame(&idle);
    tap(IN_B3);
    feed(&os.rec, 440, 0.5f, 128, &ph);
    frame(&idle);
    CHECK(os.rec_result != 0);
    struct input in = blank();
    in.pad_hit[5] = 100, in.pad_down[5] = 1;
    frame(&in);
    in = blank(), in.pad_release[5] = 1;
    frame(&in);
    CHECK(!os.rec_result && os.programs[2].pads[5].s && !os.programs[0].pads[5].s && !os.programs[0].pads[0].s);
    os.screen = OS_MAIN;
}

// The master effects go with the file: a sequence or project loads its own, and one saved with
// none leaves none on, never the last file's.
// Saving: the name goes past those already in the folder (PATTERN01 there: PATTERN02, both: PATTERN03),
// and after a save (or a load) the next save of that type starts on that name.
static void test_save_name_next(void) {
    seq_fresh();
    char dir[] = "/tmp/claude-mpc-names-XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    os.data_dir = dir;
    memset(os.disk_last_saved, 0, sizeof os.disk_last_saved);
    memset(os.disk_last, 0, sizeof os.disk_last);
    struct input idle = blank();
    disk_open(&os, DISK_SAVE, DISK_ARP);
    CHECK(strcmp(os.disk_name, "PATTERN01") == 0);
    tap(IN_B3), frame(&idle); // <Save>
    disk_open(&os, DISK_SAVE, DISK_ARP);
    CHECK(strcmp(os.disk_name, "PATTERN01") == 0); // the name just saved, to save over it
    memset(os.disk_last_saved, 0, sizeof os.disk_last_saved); // a new session
    disk_open(&os, DISK_SAVE, DISK_ARP);
    CHECK(strcmp(os.disk_name, "PATTERN02") == 0);
    tap(IN_B3), frame(&idle);
    memset(os.disk_last_saved, 0, sizeof os.disk_last_saved);
    disk_open(&os, DISK_SAVE, DISK_ARP);
    CHECK(strcmp(os.disk_name, "PATTERN03") == 0);
    snprintf(os.disk_name, sizeof os.disk_name, "MYLINE07");
    tap(IN_B3), frame(&idle);
    disk_open(&os, DISK_SAVE, DISK_ARP);
    CHECK(strcmp(os.disk_name, "MYLINE07") == 0);
    os.screen = OS_MAIN, os.data_dir = 0;
    memset(os.disk_last_saved, 0, sizeof os.disk_last_saved);
    memset(os.disk_last, 0, sizeof os.disk_last);
}

// .SND: saved from a pad with its trim, loop and tune, read back the same; and
// ERASE while naming takes a letter off (Shift + ERASE, the whole name).
static void test_snd_and_name_erase(void) {
    seq_fresh();
    char dir[] = "/tmp/claude-mpc-snd-XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    os.data_dir = dir;
    memset(os.disk_last_saved, 0, sizeof os.disk_last_saved);
    memset(os.disk_last, 0, sizeof os.disk_last);
    struct sample *x = sample_new(1000, 2, 44100);
    for (int i = 0; i < 1000; i++) x->data[2 * i] = (i % 100) / 100.0f - 0.5f, x->data[2 * i + 1] = -0.25f;
    os.selected = 0;
    os_pad_load(&os, 0, x, "Bass");
    struct pad_params *p = &os.pads[0].p;
    p->start = 10, p->end = 900, p->loop = 500, p->loop_on = 1, p->semi = -3, p->fine = 50;
    struct input idle = blank();
    disk_open(&os, DISK_SAVE, DISK_SND);
    snprintf(os.disk_name, sizeof os.disk_name, "BASS1");
    tap(IN_B3), frame(&idle); // <Save>
    char path[700];
    snprintf(path, sizeof path, "%s/samples/BASS1.snd", dir);
    FILE *f = fopen(path, "rb");
    unsigned char h[42] = {0};
    long size = 0;
    if (f) {
        CHECK(fread(h, 1, 42, f) == 42);
        fseek(f, 0, SEEK_END), size = ftell(f), fclose(f);
    }
    CHECK(f && size == 42 + 1000 * 2 * 2);
    CHECK(h[0] == 1 && h[1] == 4 && memcmp(h + 2, "BASS1           ", 16) == 0 && h[21] == 1);
    CHECK((int8_t)h[20] == -25 && h[38] == 1 && (h[40] | h[41] << 8) == 44100);
    struct snd_info in;
    struct sample *y = snd_load(path, &in);
    CHECK(y && y->frames == 1000 && y->channels == 2 && y->rate == 44100);
    CHECK(in.start == 10 && in.end == 900 && in.loop_len == 400 && in.loop_on && in.tune == -25 && strcmp(in.name, "BASS1") == 0);
    CHECK(y && fabsf(y->data[2 * 50] - x->data[2 * 50]) < 0.001f && fabsf(y->data[2 * 50 + 1] + 0.25f) < 0.001f);
    free(y ? y->data : 0), free(y);
    // LOAD onto pad 2: its trim, loop and tune with it
    os.selected = 1;
    disk_open(&os, DISK_LOAD, DISK_SND);
    CHECK(os.disk_nfiles >= 1);
    for (int i = 0; i < os.disk_nfiles; i++)
        if (strcmp(os.disk_files[i], "BASS1") == 0) os.disk_file = i;
    os.disk_page = 1; // the folder's items
    tap(IN_B3), frame(&idle); // <Load>
    const struct os_pad *q = &os.pads[1];
    CHECK(q->s && q->s->frames == 1000 && q->p.start == 10 && q->p.end == 900 && q->p.loop == 500 && q->p.loop_on);
    CHECK(q->p.semi == -2 && q->p.fine == -50 && strcmp(q->name, "BASS1") == 0);
    // The MPC 3000's .SND: version 2, a 38-byte header. This one's header as the MPC 3000 (OS 3.12,
    // in MAME) saved it: TESTV0, level 100, tune -17, 441 frames, bytes 34-37 d0 07 01 00.
    {
        static const unsigned char H3000[38] = {1, 2, 'T', 'E', 'S', 'T', 'V', '0', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                                               0, 100, 0xef, 0, 0, 0, 0, 0, 0xb9, 1, 0, 0, 0xb9, 1, 0, 0, 0xd0, 0x07, 1, 0};
        size_t len = 38 + 441 * 2;
        unsigned char *b3 = calloc(1, len);
        memcpy(b3, H3000, 38);
        b3[38] = 0x10, b3[39] = 0x27; // the first sample, 10000
        struct snd_info i3;
        struct sample *z = snd_parse(b3, len, &i3);
        CHECK(z && z->frames == 441 && z->channels == 1 && z->rate == 44100 && i3.mpc3000 && i3.tune == -17 && i3.end == 441);
        CHECK(z && fabsf(z->data[0] - 10000 / 32768.0f) < 0.0001f && strcmp(i3.name, "TESTV0") == 0 && !i3.loop_on);
        if (z) free(z->data), free(z);
        // and saved for the MPC 3000: the same header, byte for byte
        struct sample *w3 = sample_new(441, 1, 44100);
        char p3[700];
        snprintf(p3, sizeof p3, "%s/t3000.snd", dir);
        struct snd_info o3 = {.name = "TESTV0", .level = 100, .tune = -17, .start = 0, .end = 441, .mpc3000 = 1};
        CHECK(snd_save(p3, w3, &o3) == 0);
        FILE *g = fopen(p3, "rb");
        unsigned char back[40] = {0};
        long sz = 0;
        if (g) {
            CHECK(fread(back, 1, 38, g) == 38);
            fseek(g, 0, SEEK_END), sz = ftell(g), fclose(g);
        }
        CHECK(sz == (long)len && memcmp(back, H3000, 38) == 0);
        free(w3->data), free(w3), free(b3);
    }
    // A file that is not a .SND: nothing
    unsigned char junk[60] = {9, 9};
    CHECK(snd_parse(junk, sizeof junk, 0) == 0);
    // ERASE while naming
    disk_open(&os, DISK_SAVE, DISK_ARP);
    snprintf(os.disk_name, sizeof os.disk_name, "PROJECT01");
    os.disk_pos = 8;
    struct input e = blank();
    e.down = e.held = 1u << IN_ERASE;
    frame(&e);
    e = blank(), e.up = 1u << IN_ERASE;
    frame(&e);
    CHECK(strncmp(os.disk_name, "PROJECT0 ", 9) == 0 && os.screen == OS_DISK); // the 1 at the cursor; not the Erase screen
    tap(IN_ERASE); // the cursor is past the end now: the letter before it
    CHECK(strncmp(os.disk_name, "PROJECT ", 8) == 0 && os.disk_pos == 7);
    os.disk_pos = 0;
    tap(IN_ERASE);
    CHECK(strncmp(os.disk_name, "ROJECT ", 7) == 0 && os.disk_pos == 0);
    e = blank();
    e.down = e.held = (1u << IN_ERASE) | (1u << IN_SHIFT), e.shift = 1;
    frame(&e);
    CHECK(strspn(os.disk_name, " ") == strlen(os.disk_name) && os.disk_pos == 0 && os.screen == OS_DISK); // all of it, and not Copy
    os.screen = OS_MAIN, os.data_dir = 0;
    memset(os.disk_last_saved, 0, sizeof os.disk_last_saved);
    memset(os.disk_last, 0, sizeof os.disk_last);
}

// PLAY while playing: back to the start, and what was sounding lets go.
static void test_play_retrigger(void) {
    seq_fresh();
    struct seq *sq = &os.seq;
    os.screen = OS_MAIN;
    seq_play(sq, 1);
    render_until_tick(SEQ_PPQ * 6);
    CHECK(sq->mode == SEQ_PLAYING && sq->pos >= SEQ_PPQ * 6);
    tap(IN_PLAY);
    CHECK(sq->mode == SEQ_PLAYING && sq->pos < SEQ_PPQ); // from the start, still playing
    struct input in = blank();
    in.down = in.held = (1u << IN_PLAY) | (1u << IN_SHIFT), in.shift = 1;
    render_until_tick(SEQ_PPQ * 3);
    double was = sq->pos;
    frame(&in); // SHIFT + PLAY while playing: nothing
    CHECK(sq->mode == SEQ_PLAYING && sq->pos >= was);
    seq_stop(sq, 1);
    for (int i = 0; i < 20; i++) render(128);
}

// Step Edit (and its Grid) while the sequence runs: a pad is not also put in at Now, so one
// press makes one note (recorded at the play position when recording), never a second, stray one.
static void test_step_pad_no_ghost(void) {
    seq_fresh();
    load_pad(0);
    struct seq *sq = &os.seq;
    sq->sequence[0].bars = 2;
    struct seq_track *k = &sq->sequence[0].track[0];
    os.screen = OS_MAIN;
    struct input in = blank();
    in.down = in.held = (1u << IN_SEQ) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in), frame(&in); // Step Edit, then the Grid
    CHECK(os.screen == OS_STEP && os.step_grid == 1);
    os.step_tick = 0, os.step_row = 5;
    // On the Grid a pad only selects its row: nothing goes in
    in = blank(), in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    in = blank(), in.pad_release[0] = 1;
    frame(&in);
    CHECK(k->count == 0 && os.step_row == 0);
    // The list, stopped: a pad puts its note in at Now
    os.step_grid = 0;
    in = blank(), in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    in = blank(), in.pad_release[0] = 1;
    frame(&in);
    CHECK(k->count == 1 && k->notes[0].tick == 0);
    os.step_grid = 1;
    // Overdubbing: one note, where the sequence is, none at Now
    sq->rec_arm = REC_ARM_OVERDUB;
    seq_play(sq, 1);
    render_until_tick(SEQ_PPQ * 5 + 10);
    in = blank(), in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    for (int i = 0; i < 4; i++) render(128);
    in = blank(), in.pad_release[0] = 1;
    frame(&in);
    for (int i = 0; i < 4; i++) render(128);
    seq_stop(sq, 1);
    for (int i = 0; i < 20; i++) render(128);
    seq_drain(sq);
    int at0 = 0, later = 0;
    for (int i = 0; i < k->count; i++) at0 += k->notes[i].tick == 0, later += k->notes[i].tick >= SEQ_PPQ * 5;
    CHECK(k->count == 2 && at0 == 1 && later == 1);
    // Only playing: nothing goes in at all
    seq_play(sq, 1);
    render_until_tick(SEQ_PPQ * 2);
    in = blank(), in.pad_hit[1] = 100, in.pad_down[1] = 1;
    frame(&in);
    in = blank(), in.pad_release[1] = 1;
    frame(&in);
    seq_stop(sq, 1);
    for (int i = 0; i < 20; i++) render(128);
    CHECK(k->count == 2);
    os.screen = OS_MAIN;
}

// The Sample page, the cursor on Sample: a tap of ERASE renames the sound (every pad playing it),
// not the Erase page. ERASE in the box takes a letter out; B3 keeps the name, B1 cancels.
static void test_sample_erase_rename(void) {
    seq_fresh();
    load_pad(0);
    os.selected = 0;
    os.screen = OS_SAMPLE;
    os.frame_slot = -1;
    snprintf(os.pads[0].name, sizeof os.pads[0].name, "Kick");
    os.pads[5].s = os.pads[0].s, os.pads[0].s->refs++; // another pad, the same sound
    snprintf(os.pads[5].name, sizeof os.pads[5].name, "Kick");
    struct input in = blank();
    in.down = in.held = 1u << IN_ERASE;
    frame(&in);
    in = blank(), in.up = 1u << IN_ERASE;
    frame(&in);
    CHECK(os.screen == OS_SAMPLE && os.rename_what == RENAME_SOUND && strncmp(os.rename_buf, "Kick ", 5) == 0);
    os.rename_pos = 4;
    in = blank(), in.encoder = 1; // ' ' to 'A'
    frame(&in);
    os.rename_pos = 0;
    tap(IN_ERASE); // the K out
    CHECK(strncmp(os.rename_buf, "ickA", 4) == 0 && os.screen == OS_SAMPLE);
    tap(IN_B3); // <OK>
    CHECK(os.rename_what == RENAME_NONE && strcmp(os.pads[0].name, "ickA") == 0 && strcmp(os.pads[5].name, "ickA") == 0);
    // B1 cancels: the name stays
    in = blank(), in.down = in.held = 1u << IN_ERASE, frame(&in);
    in = blank(), in.up = 1u << IN_ERASE, frame(&in);
    CHECK(os.rename_what == RENAME_SOUND);
    tap(IN_B1);
    CHECK(os.rename_what == RENAME_NONE && strcmp(os.pads[0].name, "ickA") == 0 && os.screen == OS_SAMPLE);
    // On another field ERASE is as before (not a rename)
    os.frame_slot = 0;
    in = blank(), in.down = in.held = 1u << IN_ERASE, frame(&in);
    in = blank(), in.up = 1u << IN_ERASE, frame(&in);
    CHECK(os.rename_what == RENAME_NONE);
    os.pads[5].s = 0, os.pads[0].s->refs--;
    os.screen = OS_MAIN;
}

// The mixer: turning K1-K3 (the EQ) or the slider can shake the panel into a wheel step or a pad
// hit. For 300 ms after a knob moves, those neither pan the channel nor pick another one.
static void test_mixer_knob_shake(void) {
    seq_fresh();
    struct seq *sq = &os.seq;
    seq_mixer_open(&os);
    os.mixer_track = 2, os.bank = 0;
    struct seq_track *k = &sq->sequence[sq->current].track[2];
    int pan = k->pan;
    os.knob_mode = KNOB_JUMP;
    struct input in = blank();
    in.knob_moved[0] = 1, in.knob[0] = 100, in.encoder = 3; // a knob turn with a wheel step in it
    in.pad_hit[7] = 90, in.pad_down[7] = 1;                 // and a firm phantom hit
    frame(&in);
    CHECK(os.screen == OS_MIXER && os.mixer_track == 2 && k->pan == pan && k->eq.lg > 0);
    in = blank(), in.pad_release[7] = 1;
    frame(&in);
    // Later, on their own, the wheel pans and a pad picks a channel. (Each frame here is 1000 ms.)
    struct ui w = {.px = px, .now_ms = 2000, .track_led = -1};
    in = blank(), in.encoder = 3;
    os_frame(&w, &in, &os);
    CHECK(k->pan == pan + 3);
    w.now_ms = 3000;
    in = blank(), in.pad_hit[7] = 90, in.pad_down[7] = 1;
    os_frame(&w, &in, &os);
    CHECK(os.mixer_track == 7);
    in = blank(), in.pad_release[7] = 1;
    w.now_ms = 4000;
    os_frame(&w, &in, &os);
    os.knob_mode = KNOB_CATCH;
    os.screen = OS_MAIN;
}

// USB individual outs: a pad (or a track) onto one of the eight output channels, mono or a stereo
// pair; its own level there (Follow ster vol: NO), and stereo Volume 0 taking it out of the mix.
static float outs_peak(int c) {
    float m = 0;
    for (int n = 0; n < 128; n++) m = fmaxf(m, fabsf(e.outs[c][n]));
    return m;
}
static float mix_peak(void) {
    float m = 0;
    for (int n = 0; n < 256; n++) m = fmaxf(m, fabsf(block[n]));
    return m;
}
static void outs_hit(void) { // pad A01, hard, and a block of what it plays
    engine_kill_all(&e);
    render(16);
    struct input in = blank();
    in.pad_hit[0] = 127, in.pad_down[0] = 1;
    frame(&in);
    render(128);
    in = blank(), in.pad_release[0] = 1;
    frame(&in);
}
static void test_usb_outs(const char *shot_dir) {
    seq_fresh();
    load_pad(0);
    os.screen = OS_MAIN, os.selected = 0, os.bank = 0;
    os.usb_outs = 1, os.outs_by_track = 0, os.outs_keep = 1;
    struct pad_params *p = &os.pads[0].p;
    p->usb_out = 3, os_pad_push(&os, 0);
    outs_hit();
    float mix = mix_peak(), o3 = outs_peak(2);
    CHECK(atomic_load(&e.outs_on) == 1 && mix > 0.01f && fabsf(o3 - mix) < 0.002f && outs_peak(0) == 0 && outs_peak(3) == 0);
    p->usb_out = ENGINE_OUTS + 1, os_pad_push(&os, 0); // stereo 1-2
    outs_hit();
    CHECK(outs_peak(0) > 0.01f && outs_peak(1) > 0.01f && outs_peak(2) == 0);
    p->usb_out = 3, p->usb_follow_off = 1, p->usb_vol_cut = 50, os_pad_push(&os, 0); // its own level, 50
    outs_hit();
    CHECK(fabsf(outs_peak(2) - mix_peak() * 0.25f) < 0.002f && mix_peak() > 0.01f);
    p->volume = 0, p->usb_vol_cut = 0, os_pad_push(&os, 0); // stereo Volume 0: on the output only
    outs_hit();
    CHECK(mix_peak() == 0 && outs_peak(2) > 0.01f);
    p->volume = 100, p->usb_follow_off = 0, p->usb_out = 0, os_pad_push(&os, 0);
    // By track: the active track (1) on output 5; kept in the mix, then not
    os.outs_by_track = 1;
    os.seq.sequence[0].track[0].usb_out = 5;
    outs_hit();
    CHECK(outs_peak(4) > 0.01f && mix_peak() > 0.01f && outs_peak(2) == 0);
    float full = outs_peak(4), in_mix = mix_peak();
    os.seq.sequence[0].track[0].usb_vol_cut = 50; // Indiv Out Volume 50: a quarter (squared), the mix as it was
    outs_hit();
    CHECK(fabsf(outs_peak(4) - full * 0.25f) < 0.002f && fabsf(mix_peak() - in_mix) < 0.002f);
    os.seq.sequence[0].track[0].usb_vol_cut = 0;
    os.outs_keep = 0;
    outs_hit();
    CHECK(outs_peak(4) > 0.01f && mix_peak() == 0);
    os.outs_keep = 1, os.outs_by_track = 0;
    os.seq.sequence[0].track[0].usb_out = 0;
    // Off: the engine leaves the outputs alone
    os.usb_outs = 0;
    outs_hit();
    CHECK(atomic_load(&e.outs_on) == 0 && mix_peak() > 0.01f);
    // The page: SHIFT + Pad 9 twice; the cursor opens on Output; the wheel sets it; B3 <Set All>
    os.usb_outs = 1;
    seq_mixer_open(&os);
    struct input in = blank();
    in.pad_hit[8] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    in = blank(), in.pad_release[8] = 1, frame(&in);
    CHECK(os.screen == OS_OUTS);
    // MIX + 6 OUTS (to start): the mix on 1-2, so the wheel goes OFF, 3 ... 8, 3-4 ... 7-8
    in = blank(), in.encoder = 7;
    frame(&in);
    CHECK(os.outs_layout == 0 && p->usb_out == ENGINE_OUTS + 3); // 3-4
    in = blank(), in.encoder = -7;
    frame(&in);
    CHECK(p->usb_out == 0);
    os.outs_layout = 1; // 8 OUTS: OFF, 1 ... 8, 1-2, 2-3
    in = blank(), in.encoder = 10;
    frame(&in);
    CHECK(p->usb_out == 10);
    if (shot_dir) write_shot(shot_dir, "usb_outs");
    tap(IN_B3); // <Set All>
    CHECK(os.pads[7].p.usb_out == 10 && os.pads[100].p.usb_out == 10);
    os.outs_layout = 0;
    os.cursor_outs = 1; // Src: TRACKS
    in = blank(), in.encoder = 1, frame(&in);
    CHECK(os.outs_by_track == 1);
    load_pad(3);
    for (int v = 0; v < ENGINE_VOICES; v++) e.voice[v].stage = 0; // nothing sounding from before
    in = blank(), in.pad_hit[3] = 100, in.pad_down[3] = 1, frame(&in); // a pad picks track 4, and is not heard
    render(16);
    int heard = 0;
    for (int v = 0; v < ENGINE_VOICES; v++) heard += e.voice[v].stage != 0;
    CHECK(heard == 0);
    in = blank(), in.pad_release[3] = 1, frame(&in);
    os.outs_by_track = 0; // Source PADS: the press is heard
    in = blank(), in.pad_hit[3] = 100, in.pad_down[3] = 1, frame(&in);
    render(16);
    heard = 0;
    for (int v = 0; v < ENGINE_VOICES; v++) heard += e.voice[v].stage != 0;
    CHECK(heard > 0);
    in = blank(), in.pad_release[3] = 1, frame(&in);
    os.outs_by_track = 1, os.cursor_outs = 8, os.outs_sel = 3;
    CHECK(os.outs_sel == 3 && os.cursor_outs == 8);
    os.cursor_outs = 4; // Level: the track's mixer level
    in = blank(), in.encoder = -10, frame(&in);
    CHECK(os.seq.sequence[0].track[3].level == 90);
    os.cursor_outs = 8;
    in = blank(), in.encoder = 2, frame(&in);
    CHECK(os.seq.sequence[0].track[3].usb_out == 4); // OFF, 3, 4: the mix has 1-2
    if (shot_dir) write_shot(shot_dir, "usb_outs_tracks");
    tap(IN_B1); // <Mixer>
    CHECK(os.screen == OS_MIXER);
    for (int i = 0; i < OS_PADS; i++) os.pads[i].p.usb_out = 0;
    os.usb_outs = 0, os.outs_by_track = 0;
    os.screen = OS_MAIN;
}

// A chopped pad (a zone of a longer sound) with another sound loaded over it by mistake: put the
// long sound back on that pad and its chop comes back; on another pad, the whole sound.
static void test_chop_recall(void) {
    seq_fresh();
    struct sample *longs = ramp(20000), *drum = ramp(3000);
    os_pad_load(&os, 4, longs, "Break");
    struct pad_params *p = &os.pads[4].p;
    p->start = 5000, p->end = 9000, p->loop = 6000, p->loop_on = 1, p->loop_lock = 0; // the chop, its loop set by hand
    snprintf(os.pads[4].name, sizeof os.pads[4].name, "Break-03");
    os_pad_assign(&os, 5, longs, "Break"); // another pad plays the same sound, so it stays in memory
    os.pads[5].p.start = 9000, os.pads[5].p.end = 14000;
    os_pad_load_over(&os, 4, drum, "Kick"); // the mistake
    CHECK(os.pads[4].s == drum && p->start == 0 && p->end == 3000);
    os_pad_assign(&os, 4, longs, "Break"); // put back
    CHECK(os.pads[4].s == longs && p->start == 5000 && p->end == 9000 && p->loop == 6000 && p->loop_on);
    CHECK(!strcmp(os.pads[4].name, "Break-03")); // and its name
    os_pad_assign(&os, 7, longs, "Break"); // a pad that never had it: the whole sound
    CHECK(os.pads[7].p.start == 0 && os.pads[7].p.end == 20000);
    CHECK(os.pads[5].p.start == 9000 && os.pads[5].p.end == 14000); // the other chop untouched
    os_program_clear(&os, os.program);
}

static void test_master_with_file(void) {
    seq_fresh();
    char dir[] = "/tmp/claude-mpc-master-XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    char p1[700], p2[700];
    snprintf(p1, sizeof p1, "%s/one.sq6", dir), snprintf(p2, sizeof p2, "%s/two.sq6", dir);
    master_defaults(&os.master);
    atomic_store(&os.master.eq_on, 1), atomic_store(&os.master.thresh, -20);
    CHECK(disk_seq_save(&os, 0, p1) == 0);
    master_defaults(&os.master);
    CHECK(disk_seq_save(&os, 0, p2) == 0);
    CHECK(disk_seq_load(&os, 0, p1) == 0 && atomic_load(&os.master.eq_on) == 1 && atomic_load(&os.master.thresh) == -20);
    CHECK(disk_seq_load(&os, 0, p2) == 0 && atomic_load(&os.master.eq_on) == 0 && atomic_load(&os.master.thresh) == -6);
    atomic_store(&os.master.eq_on, 1);
    CHECK(disk_project_save(&os, dir) == 0);
    master_defaults(&os.master);
    CHECK(disk_project_load(&os, dir) == 0 && atomic_load(&os.master.eq_on) == 1);
    master_defaults(&os.master);
}

// 16 Levels (Tune) on an S600 program: whatever the order of presses and releases (one in the
// same read as its press, overlapping, a bank between), every note ends when every pad is up.
static void test_levels_s600_no_held(void) {
    unsigned rnd = 12345;
    for (int mode = 0; mode < 3; mode++) { // POLY, MONO, MONO + Legato
        seq_fresh();
        os.seq.metronome = METRO_OFF;
        os_pad_load(&os, 0, constant(44100), "Held");
        os_program_set_s600(&os, 0, 1);
        os.pads[0].p.loop_on = 1, os.pads[0].sp.mono = mode > 0, os.pads[0].sp.legato = mode == 2;
        os_pad_push(&os, 0);
        os.levels_on = 1, os.levels_type = LEVELS_TUNE, os.levels_pad = 0, os.levels_root = 4;
        os.screen = OS_MAIN;
        int down[16] = {0};
        for (int step = 0; step < 400; step++) {
            struct input in = blank();
            for (int k = 0; k < 3; k++) {
                rnd = rnd * 1103515245u + 12345u;
                int p = (int)(rnd >> 16) % 6, what = (int)(rnd >> 8) % 4;
                if (!down[p] && what < 2) {
                    in.pad_hit[p] = 90, down[p] = 1;
                    if (what == 1) in.pad_release[p] = 1, down[p] = 0; // a tap inside one read
                } else if (down[p] && what >= 2) {
                    in.pad_release[p] = 1, down[p] = 0;
                }
            }
            for (int p = 0; p < 16; p++) in.pad_down[p] = down[p];
            frame(&in);
            render(64);
        }
        struct input in = blank();
        for (int p = 0; p < 16; p++) if (down[p]) in.pad_release[p] = 1;
        frame(&in);
        for (int b = 0; b < 400; b++) render(128); // past any release
        CHECK(voices() == 0);
        if (voices()) printf("  16 Levels S600 mode %d: %d notes left on\n", mode, voices());
        os.levels_on = 0;
        os_program_set_s600(&os, 0, 0);
    }
}

// The same, with the sequencer playing and overdubbing what is pressed, several passes, then
// playing it back with the pads up: a looping sound never left on after Stop.
static void test_levels_s600_seq_no_held(void) {
    unsigned rnd = 777;
    for (int mode = 0; mode < 4; mode++) { // POLY, MONO, MONO + Legato, POLY with Note On off
        seq_fresh();
        os.seq.metronome = METRO_OFF;
        os.seq.sequence[0].bars = 1;
        os_pad_load(&os, 0, constant(44100), "Held");
        os_program_set_s600(&os, 0, 1);
        os.pads[0].p.loop_on = 1, os.pads[0].sp.mono = mode == 1 || mode == 2, os.pads[0].sp.legato = mode == 2;
        if (mode == 3) os.pads[0].sp.note_on = 0;
        os_pad_push(&os, 0);
        os.levels_on = 1, os.levels_type = LEVELS_TUNE, os.levels_pad = 0, os.levels_root = 4;
        os.screen = OS_MAIN;
        seq_play(&os.seq, 1);
        seq_rec_button(&os.seq); // overdub
        int down[16] = {0};
        for (int step = 0; step < 900; step++) {
            struct input in = blank();
            for (int k = 0; k < 2; k++) {
                rnd = rnd * 1103515245u + 12345u;
                int p = (int)(rnd >> 16) % 6, what = (int)(rnd >> 8) % 6;
                if (!down[p] && what < 2) {
                    in.pad_hit[p] = 90, down[p] = 1;
                    if (what == 1) in.pad_release[p] = 1, down[p] = 0;
                } else if (down[p] && what >= 3) {
                    in.pad_release[p] = 1, down[p] = 0;
                }
            }
            for (int p = 0; p < 16; p++) in.pad_down[p] = down[p];
            frame(&in);
            render(256);
        }
        struct input in = blank();
        for (int p = 0; p < 16; p++) if (down[p]) in.pad_release[p] = 1;
        frame(&in);
        seq_rec_button(&os.seq); // punch out: it keeps playing what was recorded
        for (int pass = 0; pass < 3; pass++) render_until_wrap();
        seq_stop(&os.seq, 1);
        for (int b = 0; b < 600; b++) render(128); // past any release
        CHECK(voices() == 0);
        if (voices()) printf("  16 Levels S600 with the sequencer, mode %d: %d notes left on\n", mode, voices());
        os.levels_on = 0;
        os_program_set_s600(&os, 0, 0);
    }
}

// Everything a player does around 16 Levels, at random, with a looping S600 sound: banks and
// the root changed under held pads, the Levels page, NOTE REPEAT held, the sequencer overdubbing
// or not. Once every pad is up and it stops, nothing is left sounding.
static void test_levels_s600_wild(void) {
    unsigned rnd = 4242;
    for (int run = 0; run < 8; run++) {
        int mode = run % 4, playing = run >= 4;
        seq_fresh();
        os.seq.metronome = METRO_OFF;
        os.seq.sequence[0].bars = 1;
        os_pad_load(&os, 0, constant(44100), "Held");
        os_program_set_s600(&os, 0, 1);
        os.pads[0].p.loop_on = 1, os.pads[0].sp.mono = mode == 1 || mode == 2, os.pads[0].sp.legato = mode == 2;
        if (mode == 3) os.pads[0].sp.note_on = 0;
        os_pad_push(&os, 0);
        os.levels_on = 1, os.levels_type = LEVELS_TUNE, os.levels_pad = 0, os.levels_root = 4;
        os.screen = OS_MAIN;
        if (playing) seq_play(&os.seq, 1), seq_rec_button(&os.seq);
        int down[16] = {0}, rep = 0;
        for (int step = 0; step < 1500; step++) {
            struct input in = blank();
            rnd = rnd * 1103515245u + 12345u;
            int ev = (int)(rnd >> 20) % 100;
            if (ev < 3) in.down |= 1u << IN_PAD_BANK;                             // another bank under held pads
            else if (ev < 5) os.levels_root = (int)(rnd >> 8) % 32;                // the root moved
            else if (ev < 7) os.screen = os.screen == OS_MAIN ? OS_LEVELS : OS_MAIN; // the Levels page: a hit sets the root
            else if (ev < 9) rep = !rep;                                           // NOTE REPEAT held, or let go
            if (rep) in.held |= 1u << IN_NOTE_REPEAT;
            if (ev == 9 && rep) in.down |= 1u << IN_NOTE_REPEAT;
            for (int k = 0; k < 2; k++) {
                rnd = rnd * 1103515245u + 12345u;
                int p = (int)(rnd >> 16) % 8, what = (int)(rnd >> 8) % 6;
                if (!down[p] && what < 2) {
                    in.pad_hit[p] = 30 + (int)(rnd >> 4) % 97, down[p] = 1;
                    if (what == 1) in.pad_release[p] = 1, down[p] = 0;
                } else if (down[p] && what >= 3) {
                    in.pad_release[p] = 1, down[p] = 0;
                }
            }
            for (int p = 0; p < 16; p++) in.pad_down[p] = down[p];
            frame(&in);
            render(256);
        }
        struct input in = blank();
        for (int p = 0; p < 16; p++) if (down[p]) in.pad_release[p] = 1;
        frame(&in);
        in = blank();
        frame(&in);
        if (playing) {
            seq_rec_button(&os.seq);
            for (int pass = 0; pass < 2; pass++) render_until_wrap();
            seq_stop(&os.seq, 1);
        }
        for (int b = 0; b < 600; b++) render(128);
        CHECK(voices() == 0);
        if (voices()) printf("  16 Levels S600 at random, mode %d%s: %d notes left on\n", mode, playing ? ", overdubbing" : "", voices());
        os.levels_on = 0, os.screen = OS_MAIN, os.bank = 0;
        os_program_set_s600(&os, 0, 0);
    }
}

// A bassline recorded the way it is played: Mono, Legato, Note On and Portamento, one pad at a
// time but quickly (the next often pressed before the last is let go), REC with Timing Correct,
// pass after pass. Checked while the sequencer still plays: with the track cleared, once its
// last notes have had time to end, nothing may still sound (Stop would hide a note left on).
static void test_levels_s600_bassline(void) {
    unsigned rnd = 99;
    for (int run = 0; run < 6; run++) {
        seq_fresh();
        os.seq.metronome = METRO_OFF;
        os.seq.sequence[0].bars = 1;
        os_pad_load(&os, 0, constant(44100), "Bass");
        os_program_set_s600(&os, 0, 1);
        os.pads[0].p.loop_on = 1;
        os.pads[0].sp.mono = 1, os.pads[0].sp.legato = 1, os.pads[0].sp.note_on = 1, os.pads[0].sp.porta = 60;
        os_pad_push(&os, 0);
        os.levels_on = run < 4, os.levels_type = LEVELS_TUNE, os.levels_pad = 0, os.levels_root = 4;
        os.screen = OS_MAIN;
        seq_play(&os.seq, 1);
        seq_rec_button(&os.seq); // OVERDUB
        if (run & 1) os.seq.mode = SEQ_RECORDING; // REC: what is played over replaces what was there
        int held = -1, left = 0;
        for (int step = 0; step < 2500; step++) {
            struct input in = blank();
            rnd = rnd * 1103515245u + 12345u;
            if (held >= 0 && --left <= 0) in.pad_release[held] = 1, held = -1;
            if ((rnd >> 24) % 3 == 0) {
                int p = (int)(rnd >> 8) % 8;
                if (p != held) {
                    in.pad_hit[p] = 60 + (int)(rnd >> 4) % 60;
                    if (held >= 0 && (rnd >> 16) % 2) in.pad_release[held] = 1; // let go as the next goes down, or just after
                    else if (held >= 0) { // let go a moment later: the two overlap
                        frame(&in);
                        render(128);
                        in = blank();
                        in.pad_release[held] = 1;
                    }
                    held = p, left = 1 + (int)(rnd >> 12) % 6;
                }
            }
            for (int p = 0; p < 16; p++) in.pad_down[p] = p == held;
            frame(&in);
            render(256);
        }
        struct input in = blank();
        if (held >= 0) in.pad_release[held] = 1;
        frame(&in);
        in = blank();
        frame(&in);
        os.seq.mode = SEQ_PLAYING;
        render_until_wrap();
        pthread_mutex_lock(&os.seq.lock); // the track cleared, still playing
        os.seq.sequence[0].track[0].count = 0;
        pthread_mutex_unlock(&os.seq.lock);
        render_until_wrap();
        render_until_wrap();
        for (int b = 0; b < 400; b++) render(128);
        CHECK(voices() == 0);
        if (voices()) printf("  bassline run %d (%s, %s): %d notes still on while playing\n", run, os.levels_on ? "16 Levels" : "one pad",
                             run & 1 ? "REC" : "OVERDUB", voices());
        seq_stop(&os.seq, 1);
        for (int b = 0; b < 400; b++) render(128);
        os.levels_on = 0;
        os_program_set_s600(&os, 0, 0);
    }
}

// STOP with a note still sounding and no pad held: held-note-log.txt on the card says what
// sounds, what the sequencer still means to release, and the last note events.
static void test_held_note_log(void) {
    seq_fresh();
    char dir[] = "/tmp/claude-mpc-held-XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    os.data_dir = dir;
    os_pad_load(&os, 0, constant(44100), "Bass");
    os_program_set_s600(&os, 0, 1);
    os.pads[0].p.loop_on = 1, os.pads[0].sp.note_on = 1;
    os_pad_push(&os, 0);
    render(16);
    engine_play_pad_mix(&e, ENGINE_S600 + 0, 100, 3, 0, 1, 100, 0); // a note nothing will release
    render(128);
    tap(IN_STOP);
    char p[700];
    snprintf(p, sizeof p, "%s/held-note-log.txt", dir);
    FILE *f = fopen(p, "r");
    CHECK(f != 0);
    char buf[8192] = {0};
    if (f) fread(buf, 1, sizeof buf - 1, f), fclose(f);
    CHECK(strstr(buf, "SEQ ON") && strstr(buf, "key 3") && strstr(buf, "Voices"));
    engine_kill_all(&e);
    for (int b = 0; b < 400; b++) render(128);
    remove(p);
    tap(IN_STOP); // nothing sounding: nothing written
    CHECK(access(p, F_OK) != 0);
    os.data_dir = 0;
}

static void test_record_page(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    tap(IN_SAMPLE_REC);
    CHECK(os.screen == OS_RECORD && os.record_page == 0 && os.cursor_record == 0);
    // Every recording page starts as Additional Settings 2 says: 29.76 kHz, 12 bits, AA and
    // Normalize on, STEREO, Input Thru on.
    for (int p = 0; p < 6; p++)
        if (p != 3)
            CHECK(os.rec_set[p].rate == 29760 && os.rec_set[p].bits == 12 && os.rec_set[p].aa && os.rec_set[p].normalize &&
                  os.rec_set[p].mode == (p == 2 ? REC_MONO_LR : REC_STEREO)); // Convert Sample: mono
    CHECK(os.input_thru == 1);
    os.rec_set[0].rate = 26040;
    os.cursor_record = 5; // Sample Rate: the wheel moves in half kHz and lands on the classic rates
    struct input in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.rec_set[0].rate == 26500);
    in.encoder = -2;
    frame(&in);
    CHECK(os.rec_set[0].rate == 25500);
    in.encoder = 1;
    frame(&in);
    CHECK(os.rec_set[0].rate == 26040);
    in = blank();
    in.fader_moved = 1, in.fader = 127; // the slider jumps between the six, 26.04 among them
    frame(&in);
    CHECK(os.rec_set[0].rate == 44100);
    in.fader = 70;
    frame(&in);
    CHECK(os.rec_set[0].rate == 26040);
    in.fader = 50;
    frame(&in);
    CHECK(os.rec_set[0].rate == 22050);
    os.cursor_record = 2; // Pitch
    in = blank();
    in.encoder = 12;
    frame(&in);
    CHECK(os.rec_set[0].pitch == 12);
    struct input idle = blank();
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "record");

    atomic_store(&os.rec.present, 1);
    os.rec_set[0].thres_db = -61; // OFF: starts at once
    tap(IN_B3); // <Rec>
    CHECK(atomic_load(&os.rec.state) == REC_ARMED);
    long ph = 0;
    feed(&os.rec, 440, 0.5f, ENGINE_RATE, &ph);
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "record_wave");
    tap(IN_B3); // <Stop>
    feed(&os.rec, 440, 0.5f, 128, &ph);
    frame(&idle);
    CHECK(os.rec_result && os.rec_result->rate == 22050);
    tap(IN_B2); // <Play> previews it, an octave down as the pad will play it
    render(128);
    CHECK(voice_on(-3) && voice_on(-3)->s == os.rec_result && voice_on(-3)->p.semi == -12 && os.rec_result);
    if (shot_dir) write_shot(shot_dir, "record_done");
    in = blank();
    in.pad_hit[3] = 100, in.pad_down[3] = 1; // pad A04 gets it, tuned down an octave
    frame(&in);
    in = blank();
    in.pad_release[3] = 1;
    frame(&in);
    CHECK(!os.rec_result && os.pads[3].s && os.pads[3].s->rate == 22050 && os.pads[3].p.semi == -12);
    CHECK(strcmp(os.pads[3].name, "Sample01") == 0);

    tap(IN_B3); // another take, kept in memory with ENTER
    feed(&os.rec, 440, 0.5f, 4410, &ph);
    tap(IN_B3);
    feed(&os.rec, 440, 0.5f, 128, &ph);
    frame(&idle);
    CHECK(os.rec_result != 0);
    in = blank();
    in.encoder_press = 1;
    frame(&in);
    CHECK(!os.rec_result && os.nmemory == 1 && strcmp(os.memory[0].name, "Sample02") == 0);
    tap(IN_B3); // a third, previewed then cancelled: the engine lets go before it is freed
    feed(&os.rec, 440, 0.5f, 4410, &ph);
    tap(IN_B3);
    feed(&os.rec, 440, 0.5f, 128, &ph);
    frame(&idle);
    tap(IN_B2);
    render(128);
    CHECK(voice_on(-3) != 0);
    tap(IN_B1);
    render(128);
    frame(&idle);
    CHECK(!os.rec_result && voice_on(-3) == 0);

    // Convert 44.1k on the pad's Trim page.
    os.selected = 3;
    os.screen = OS_SAMPLE;
    os.page = PAGE_TRIM;
    os.do_op = DO_CONVERT;
    os.frame_slot = 2;
    long before = os.pads[3].s->frames;
    tap(IN_B3);
    CHECK(os.pads[3].s->rate == ENGINE_RATE && labs(os.pads[3].s->frames - before * 2) <= 1);
    os.screen = OS_MAIN;
}

// Live Looper (<Live Loop> on Audiotrack Record): a take of one pass while the sequence keeps
// looping, made into a sound in the background and dropped onto the pad selected beforehand;
// another can follow on the next pass without stopping.
static float lp_buf[256];
static void lp_block(int loud_after_zero) { // a block of input, as the audio thread runs, then output
    long long clock = atomic_load(&e.clock);
    for (int i = 0; i < 128; i++) {
        int loud = !loud_after_zero || (os.seq.zero_seen && clock + i >= os.seq.zero_clock + os.rec.latency);
        lp_buf[i * 2] = lp_buf[i * 2 + 1] = loud ? 0.5f : 0.0f;
    }
    recorder_input(&os.rec, lp_buf, 128, clock);
    render(128);
}

static int lp_run(int loud_after_zero) { // until the take is in (or let go): the blocks it took
    struct input idle = blank();
    int b = 0;
    for (; b < 20000 && (os.lp_state != LP_IDLE || os.lp_conv); b++) {
        lp_block(loud_after_zero);
        int near = os.lp_state == LP_ENDING || os.lp_conv;
        if (b % 16 == 0 || near) frame(&idle);
        if (near) usleep(200); // the conversion runs on its own thread
    }
    frame(&idle);
    return b;
}

static void test_live_looper(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_REC;
    os.seq.sequence[0].bars = 1; // 2 s at 120 BPM
    CHECK(os.seq.sequence[0].loop_bar == 1);
    load_pad(0);
    tap(IN_SAMPLE_REC);
    tap(IN_SAMPLE_REC);
    CHECK(os.record_page == 1);
    tap(IN_B2); // <Live Loop>
    CHECK(os.screen == OS_RECORD && os.record_page == 5 && os.cursor_record == 0);
    struct input idle = blank();
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "livelooper");
    atomic_store(&os.rec.present, 1);
    os.rec.latency = 300;
    os.rec_set[5].count_bars = 1, os.rec_set[5].rate = 44100; // lengths below in 44.1 kHz frames
    os.selected = 0; // A01 has a sound: a pad must be chosen first, and empty
    tap(IN_B3);
    CHECK(os.lp_state == LP_IDLE && strcmp(os.toast, "Select an empty pad first") == 0);

    // From stopped: a count-in, then the first pass is the take. No click in it (Audio Rec NO).
    os.selected = 6;
    tap(IN_B3);
    CHECK(os.lp_state == LP_COUNT && os.seq.mode == SEQ_COUNT_IN);
    for (int b = 0; b < 2000 && os.lp_state == LP_COUNT; b++) lp_block(1), frame(&idle);
    CHECK(os.lp_state == LP_REC && os.seq.audio_take);
    render(128);
    CHECK(voices() == 0); // the take's first beat: no click
    lp_run(1);
    const struct seq_track *k = &os.seq.sequence[0].track[0];
    CHECK(os.pads[6].s && strcmp(os.pads[6].name, "Loop01") == 0 && labs(os.pads[6].s->frames - 2 * ENGINE_RATE) <= 2);
    CHECK(os.pads[6].s->data[0] > 0.9f); // normalized, from the pass's first frame
    CHECK(k->count == 1 && k->notes[0].tick == 0 && k->notes[0].note == 6 && k->notes[0].dur == 384);
    CHECK(os.seq.mode != SEQ_STOPPED); // still looping
    CHECK(os.seq.loop_after && !os.seq.audio_take);
    if (shot_dir) write_shot(shot_dir, "livelooper_done");

    // While it plays: another pad, another take, on the next pass. No stopping.
    tap(IN_B3); // A07 is now full
    CHECK(os.lp_state == LP_IDLE && strcmp(os.toast, "Select an empty pad first") == 0);
    os.selected = 7;
    int wraps = os.seq.wrap_count;
    tap(IN_B3);
    CHECK(os.lp_state == LP_WAIT);
    lp_run(0);
    CHECK(os.seq.wrap_count > wraps && os.pads[7].s && strcmp(os.pads[7].name, "Loop02") == 0);
    CHECK(k->count == 2 && os.seq.mode != SEQ_STOPPED);

    // After Loop CLICK: the metronome goes on after the take; STOP (the default) keeps it quiet.
    os.seq.click_loop = 1;
    int clicked = 0;
    for (int b = 0; b < 800; b++) {
        render(128);
        if (os.seq.pos >= 0 && voices() > 2) clicked = 1; // two loops sounding, and a click
    }
    CHECK(clicked);
    os.seq.click_loop = 0;

    // AFTER 1ST LOOP: the raw take drops in at once (44.1 kHz, no pitch), and the converted one
    // takes its place while it plays.
    os.loop_conv = LOOP_CONV_AFTER;
    os.rec_set[5].rate = 26040, os.rec_set[5].pitch = 12;
    os.selected = 9;
    tap(IN_B3);
    CHECK(os.lp_state == LP_WAIT);
    for (int b = 0; b < 20000 && os.lp_state != LP_IDLE; b++) {
        lp_block(0);
        if (b % 16 == 0 || os.lp_state == LP_ENDING) frame(&idle);
    }
    CHECK(os.pads[9].s && os.pads[9].s->rate == ENGINE_RATE && os.pads[9].p.semi == 0 && os.lp_conv);
    lp_run(0);
    CHECK(os.pads[9].s && os.pads[9].s->rate == 26040 && os.pads[9].p.semi == -12 && !os.lp_conv);
    CHECK(strcmp(os.pads[9].name, "Loop03") == 0 && k->count == 3);
    os.loop_conv = LOOP_CONV_IMMEDIATE;
    os.rec_set[5].rate = 44100, os.rec_set[5].pitch = 0;

    // <Del Take>: the last take's note and sound go, its pad selected; again, the one before.
    tap(IN_B2);
    CHECK(!os.pads[9].s && k->count == 2 && os.selected == 9 && strcmp(os.toast, "Take deleted: Pad A10") == 0);
    tap(IN_B2);
    CHECK(!os.pads[7].s && k->count == 1 && os.selected == 7 && os.pads[6].s);

    // <Stop> during a take lets it go; the sequence plays on.
    os.selected = 8;
    tap(IN_B3);
    CHECK(os.lp_state == LP_WAIT);
    tap(IN_B3);
    lp_run(0);
    CHECK(os.lp_state == LP_IDLE && !os.pads[8].s && os.seq.mode != SEQ_STOPPED);

    // SAMPLE + pad selects a looping pad to edit without playing it: not with Note Repeat on,
    // not out with Local Mode off, not on a MIDI track, and nothing recorded.
    os.screen = OS_SAMPLE, os.bank = 0, os.selected = 0;
    seq_rec_button(&os.seq); // overdubbing as it loops
    int notes = k->count;
    for (int mode = 0; mode < 3; mode++) {
        os.seq.repeat = mode == 0, os.midi.local = mode != 1;
        os.seq.sequence[0].track[0].type = mode == 2 ? TRACK_MIDI : TRACK_DRUM;
        os.seq.sequence[0].track[0].channel = mode == 2 ? 1 : 0;
        int before = voices();
        struct input sel = blank();
        sel.pad_hit[6] = 100, sel.pad_down[6] = 1, sel.held = 1u << IN_SAMPLE;
        frame(&sel);
        render(128);
        CHECK(os.selected == 6 && !os.repeat_pad[6] && os.live_midi[6] < 0 && os.sounding[6] < 0 && voices() <= before);
        sel = blank();
        sel.pad_release[6] = 1, sel.held = 1u << IN_SAMPLE;
        frame(&sel);
        os.selected = 0;
    }
    CHECK(k->count == notes);
    os.seq.repeat = 0, os.midi.local = 1, os.seq.sequence[0].track[0].type = TRACK_DRUM, os.seq.sequence[0].track[0].channel = 0;

    // How long a take takes to become a sound: 10 s of stereo at 26.04 kHz, 12 bits, +12.
    struct recorder r;
    memset(&r, 0, sizeof r);
    recorder_arm(&r, 10 * ENGINE_RATE + 1000);
    for (int b = 0; b < 10 * ENGINE_RATE / 128 + 1; b++) {
        for (int i = 0; i < 256; i++) lp_buf[i] = 0.3f * sinf((float)(b * 128 + i / 2) * 0.05f);
        recorder_service(&r);
        recorder_input(&r, lp_buf, 128, 0);
    }
    recorder_stop(&r);
    recorder_input(&r, lp_buf, 128, 0);
    struct rec_format f = {REC_STEREO, 26040, 12, 1, 1, 12, 0, 10 * ENGINE_RATE};
    struct timespec t0, t1, t2;
    // IMMEDIATE: converted as it comes in, in pieces; at the end only the last of it and the
    // normalize and bits are left. The result is exactly the after-the-pass one.
    struct rec_stream *st = rec_stream_new(&f, 0, 10 * ENGINE_RATE);
    CHECK(st != 0);
    while (!rec_stream_run(st, &r)) CHECK(0); // all the frames are in already: done in one go
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct sample *streamed = rec_stream_finish(st);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    struct sample *t = recorder_take(&r, &f);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    CHECK(t && streamed && t->frames == streamed->frames && t->channels == streamed->channels);
    CHECK(t && streamed && memcmp(t->data, streamed->data, (size_t)t->frames * t->channels * sizeof(float)) == 0);
    printf("  Live Looper: a 10 s stereo take at 26.04 kHz, 12 bits, +12: converted after the pass in %.0f ms, "
           "the last step of IMMEDIATE %.1f ms, on this computer\n",
           (t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6,
           (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    sample_free(t);
    sample_free(streamed);
    // And in pieces as frames arrive: the same.
    memset(&r, 0, sizeof r);
    recorder_arm(&r, 3 * ENGINE_RATE);
    struct rec_format g = {REC_MONO_LR, 11020, 8, 1, 1, 5, 0, 0};
    st = rec_stream_new(&g, 1000, 2 * ENGINE_RATE);
    int finished = 0;
    for (int b = 0; b < 3 * ENGINE_RATE / 128 - 1; b++) {
        for (int i = 0; i < 128; i++) lp_buf[i * 2] = 0.3f * sinf((float)(b * 128 + i) * 0.031f), lp_buf[i * 2 + 1] = 0.1f;
        recorder_service(&r);
        recorder_input(&r, lp_buf, 128, 0);
        finished = rec_stream_run(st, &r);
    }
    CHECK(finished);
    streamed = rec_stream_finish(st);
    recorder_stop(&r);
    recorder_input(&r, lp_buf, 128, 0);
    g.skip = 1000, g.length = 2 * ENGINE_RATE;
    t = recorder_take(&r, &g);
    CHECK(t && streamed && t->frames == streamed->frames &&
          memcmp(t->data, streamed->data, (size_t)t->frames * sizeof(float)) == 0);
    sample_free(t);
    sample_free(streamed);
    seq_stop(&os.seq, 1);
    os.screen = OS_MAIN;
}

// ---- MULTI ----

static struct sample *tone(double hz, int rate, int frames, float level) {
    struct sample *t = sample_new(frames, 1, rate);
    for (int i = 0; i < frames; i++) t->data[i] = level * (float)sin(2 * M_PI * hz * i / rate);
    return t;
}

static double note_hz(double note) {
    return 440.0 * pow(2.0, (note - 69) / 12.0);
}

// A synth outside, played by MIDI: what it sends back to the input for the notes it is given.
static int synth_note = -1, synth_vel;
static long synth_t;
static __attribute__((unused)) void synth_midi(const unsigned char *m, int n) {
    if (n < 3) return;
    if ((m[0] & 0xf0) == 0x90 && m[2]) synth_note = m[1], synth_vel = m[2], synth_t = 0;
}

static void test_multi(const char *shot_dir) {
    // Note names in sample names: 60 is C4.
    CHECK(multi_name_note("Piano C3", 0) == 48 && multi_name_note("pno_F#2", 0) == 42 && multi_name_note("Rhodes-Bb1", 0) == 34);
    CHECK(multi_name_note("Bass E-1", 0) == 4 && multi_name_note("Grand", 0) < 0 && multi_name_note("Kick 01", 0) < 0);
    CHECK(multi_name_note("C10", 0) < 0 && multi_name_note("AS C3 v2", 0) == 48 && multi_name_note("Cb4", 0) == 59 &&
          multi_name_note("B#3", 0) == 60 && multi_name_note("EP_A4", 0) == 69);
    char nn[8];
    multi_note_name(61, nn, sizeof nn);
    CHECK(strcmp(nn, "C#4") == 0);
    multi_note_name(0, nn, sizeof nn);
    CHECK(strcmp(nn, "C-1") == 0);

    // Pitch heard: a sine and a bright saw, at a low rate too; noise has none.
    double heard = 0;
    struct sample *a4 = tone(440, 29760, 29760, 0.5f);
    CHECK(multi_detect_pitch(a4, &heard) && fabs(heard - 69) < 0.05);
    struct sample *saw = sample_new(44100, 1, 44100);
    for (int i = 0; i < 44100; i++) saw->data[i] = 0.4f * (float)(fmod(i * note_hz(45) / 44100, 1.0) * 2 - 1);
    CHECK(multi_detect_pitch(saw, &heard) && fabs(heard - 45) < 0.1);
    struct sample *noise = sample_new(44100, 1, 44100);
    uint32_t r = 1;
    for (int i = 0; i < 44100; i++) r = r * 1664525u + 1013904223u, noise->data[i] = (float)(r >> 8) / (1 << 24) - 0.5f;
    CHECK(!multi_detect_pitch(noise, &heard));
    sample_free(a4), sample_free(saw), sample_free(noise);

    // S600: a sound loaded gets its ROOT (Auto Root); MIDI keys play the Instrument (A01 to start)
    // from there: E2 played sounds it at its own pitch.
    seq_fresh();
    os_program_set_s600(&os, 0, PROG_S600);
    CHECK(os.programs[0].inst == 0 && os.auto_root == 1);
    os_pad_load(&os, 0, tone(note_hz(40.1), 44100, 22050, 0.5f), "Bass");
    os_auto_root(&os, 0);
    CHECK(os.pads[0].sp.root == 40 && os.pads[0].sp.root_fine == -10);
    os_pad_load(&os, 3, tone(note_hz(60), 44100, 22050, 0.5f), "Other");
    os.selected = 3; // pressed last; the Instrument is still A01
    os.midi_in_read = fake_midi_read;
    midi_qn = midi_qat = 0;
    midi_push(0x90, 40, 100);
    struct input idle = blank();
    frame(&idle);
    const struct voice *v = voice_on(ENGINE_S600 + 0);
    CHECK(v && v->key == 0 && v->p.fine == os.pads[0].p.fine - 10);
    midi_qn = midi_qat = 0;
    midi_push(0x80, 40, 0);
    frame(&idle);
    os.midi_in_read = 0;

    // S600 MULTI: the Instrument's keygroups; AUTO MAP from every pad, roots from names and
    // pitch (a set counting middle C as C3 is heard an octave up: the octave heard wins).
    seq_fresh();
    os_program_set_s600(&os, 0, PROG_MULTI);
    os_pad_load(&os, 0, tone(note_hz(48), 44100, 22050, 0.5f), "Pno C3");
    os_pad_load(&os, 1, tone(note_hz(52), 44100, 22050, 0.5f), "Pno E3");
    os_pad_load(&os, 2, tone(note_hz(55), 44100, 22050, 0.5f), "Pno G3");
    os_pad_load(&os, 3, tone(note_hz(64.2), 44100, 22050, 0.5f), "Sound");
    os_pad_load(&os, 4, tone(note_hz(72), 44100, 22050, 0.5f), "Set C4");
    int fine, skipped;
    CHECK(multi_root_of(&os.pads[3], 0, &fine) == 64 && fine == -20);
    CHECK(multi_root_of(&os.pads[4], 1, &fine) == 60 && multi_root_of(&os.pads[4], 0, &fine) == 72);
    CHECK(multi_auto_map(&os, 0, 0, OS_PADS - 1, 0, 0, 1, 1, &skipped) == 5 && skipped == 0);
    const struct s600_kgs *k = &os.pads[0].kgs;
    CHECK(k->kg[0].root == 48 && k->kg[0].lo == 0 && k->kg[0].hi == 50 && k->kg[1].lo == 51 && k->kg[1].hi == 53 &&
          k->kg[3].root == 64 && k->kg[3].fine == -20 && k->kg[4].hi == 127 && k->kg[4].zone[0] == 5);
    render(16);
    int slot = ENGINE_S600 + 0;
    CHECK(e.km_of[slot] && e.km[e.km_of[slot] - 1].n == 5 && e.km[e.km_of[slot] - 1].z[1].s == os.pads[1].s);
    CHECK(!e.km_of[ENGINE_S600 + 1]); // only the Instrument plays keygroups
    // A MIDI key plays the keygroup's sample, through the Instrument whatever pad was pressed.
    os.selected = 2;
    os.midi_in_read = fake_midi_read;
    midi_qn = midi_qat = 0;
    midi_push(0x90, 52, 100); // E3
    frame(&idle);
    CHECK(voice_of(os.pads[1].s) && voice_of(os.pads[1].s)->key == 52 - 60);
    midi_qn = midi_qat = 0;
    midi_push(0x80, 52, 0);
    frame(&idle);
    os.midi_in_read = 0;
    // NAME only, ROOT UP, not extended, 2 ZONES: the unnamed pad is left out; two on one root are
    // its soft and loud zones.
    os_pad_load(&os, 5, tone(note_hz(48), 44100, 22050, 0.9f), "Pno C3 loud");
    CHECK(multi_auto_map(&os, 0, 0, OS_PADS - 1, 1, 1, 2, 0, &skipped) == 4 && skipped == 1);
    CHECK(k->kg[0].lo == 48 && k->kg[0].hi == 51 && k->kg[0].zone[0] == 1 && k->kg[0].zone[1] == 6 && k->kg[3].root == 60 &&
          k->kg[3].hi == 60 && os.pads[0].sp.ranges_manual);
    CHECK(multi_auto_map(&os, 0, 0, OS_PADS - 1, 0, 0, 2, 1, &skipped) == 5 && !os.pads[0].sp.ranges_manual);

    // Saved and loaded with the project: Type, Instrument and keygroups.
    char dir[] = "/tmp/claude-mpc-multi-XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    os.data_dir = dir;
    os.programs[0].inst = 0;
    CHECK(disk_project_save(&os, dir) == 0);
    struct s600_kgs keep = *k;
    os.pads[0].kgs.n = 0;
    os_program_set_s600(&os, 0, PROG_MPC);
    CHECK(disk_project_load(&os, dir) == 0);
    render(16);
    CHECK(os.programs[0].s600 == PROG_MULTI && os.programs[0].inst == 0 && !memcmp(&os.pads[0].kgs, &keep, sizeof keep) &&
          e.km_of[slot]);
    // Loading a program asks first, naming the program it goes into; Cancel leaves it be.
    char pp[700];
    snprintf(pp, sizeof pp, "%s/programs/PNO", dir);
    CHECK(disk_program_save(&os, 0, pp) == 0);
    disk_open(&os, DISK_LOAD, DISK_PROGRAM);
    int at_pno = -1;
    for (int i = 0; i < os.disk_nfiles; i++)
        if (!strcmp(os.disk_files[i], "PNO")) at_pno = i;
    CHECK(at_pno >= 0);
    os.disk_file = at_pno, os.disk_which = 3, os.disk_page = 1; // on the files page
    tap(IN_B3); // <Load>
    CHECK(os.disk_ask == 'P');
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "disk_load_program");
    tap(IN_B1); // <Cancel>
    frame(&idle);
    CHECK(os.disk_ask == 0 && !os_program_sounds(&os, 3));
    tap(IN_B3);
    struct input wh = blank();
    wh.encoder = 2; // the wheel: into Program 06 instead
    frame(&wh);
    CHECK(os.disk_ask == 'P' && os.disk_which == 5);
    tap(IN_B3); // <Yes>
    frame(&idle);
    CHECK(os.programs[5].s600 == PROG_MULTI && os.programs[5].pads[0].kgs.n == keep.n && !os_program_sounds(&os, 3));
    // <To Empty>: straight into the first program with no sounds.
    tap(IN_B3);
    CHECK(os.disk_ask == 'P');
    int first_empty = -1;
    for (int g = 0; g < OS_PROGRAMS && first_empty < 0; g++)
        if (!os_program_sounds(&os, g)) first_empty = g;
    tap(IN_B2);
    frame(&idle);
    CHECK(os.disk_ask == 0 && first_empty >= 0 && os.programs[first_empty].s600 == PROG_MULTI &&
          os.programs[first_empty].pads[0].kgs.n == keep.n);
    os.screen = OS_MAIN;
    // A zone's pad changes: the instrument plays the new sound.
    struct sample *was = os.pads[1].s;
    os_pad_assign(&os, 1, tone(note_hz(52), 44100, 11025, 0.5f), "Pno E3 new");
    render(16);
    int found_new = 0;
    for (int i = 0; i < e.km[e.km_of[slot] - 1].n; i++) found_new |= e.km[e.km_of[slot] - 1].z[i].s == os.pads[1].s;
    CHECK(found_new && os.pads[1].s != was);
    // A new Instrument: the keygroups move with it.
    os_set_instrument(&os, 0, 7);
    os_pad_load(&os, 7, tone(note_hz(60), 44100, 4410, 0.5f), "Inst");
    os_pad_push(&os, 7);
    render(16);
    CHECK(os.pads[7].kgs.n == keep.n && os.pads[0].kgs.n == 0 && e.km_of[ENGINE_S600 + 7] && !e.km_of[slot]);
    os_set_instrument(&os, 0, 0);
    render(16);
    CHECK(os.pads[0].kgs.n == keep.n && e.km_of[slot]);

    // Program Config: Type MPC, S600, S600 MULTI; Inst set by the wheel or a pad pressed.
    struct input sp = blank();
    sp.down = sp.held = (1u << IN_SAMPLE) | (1u << IN_SHIFT), sp.shift = 1;
    frame(&sp);
    CHECK(os.screen == OS_PROGRAM);
    os.cursor_program = 2; // Inst
    struct input pd = blank();
    pd.pad_hit[2] = 100, pd.pad_down[2] = 1;
    frame(&pd);
    pd = blank(), pd.pad_release[2] = 1;
    frame(&pd);
    CHECK(os.programs[0].inst == 2 && os.pads[2].kgs.n == keep.n);
    os.toast_until = 0;
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "program_multi");
    os_set_instrument(&os, 0, 0);

    // The MULTI screen: on the Instrument whatever pad is pressed; K1 the keygroup; a key played
    // selects its keygroup, or (the cursor on Root) sets it; a pad pressed on a Zone sets it.
    os.selected = 0;
    s600_open(&os);
    os.s600_page = 4; // Main: in an S600 MULTI program B3 goes straight to MULTI (no Layer page)
    tap(IN_B3);
    CHECK(os.screen == OS_MULTI && os.multi_page == 0);
    tap(IN_SAMPLE);
    CHECK(os.screen == OS_S600 && os.s600_page == 4);
    os.s600_page = 2; // Filter: the program and its type, not one sample
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "s600_filter_multi");
    os.s600_page = 5; // a Layer page left over from an S600 program: shown as Main
    frame(&idle);
    CHECK(os.s600_page == 4);
    tap(IN_B3);
    CHECK(os.screen == OS_MULTI);
    pd = blank(), pd.pad_hit[3] = 100, pd.pad_down[3] = 1;
    frame(&pd);
    pd = blank(), pd.pad_release[3] = 1;
    frame(&pd);
    CHECK(os.selected == 0 && os.multi_pressed == 3);
    struct input kn = blank();
    kn.knob_moved[0] = 1, kn.knob[0] = 127;
    frame(&kn);
    CHECK(os.multi_kg == k->n - 1);
    os.toast_until = 0;
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "multi_kg");
    os.midi_in_read = fake_midi_read;
    midi_qn = midi_qat = 0;
    midi_push(0x90, 50, 100); // a key played selects its keygroup
    midi_push(0x80, 50, 0);
    frame(&idle);
    CHECK(os.multi_kg == multi_kg_at(&os.pads[0], 50) && os.multi_kg == 0);
    os.multi_cursor = 2; // Root: a key played sets it, and AUTO ranges follow
    midi_qn = midi_qat = 0;
    midi_push(0x90, 47, 100);
    midi_push(0x80, 47, 0);
    frame(&idle);
    CHECK(k->kg[0].root == 47 && k->kg[0].hi == (47 + k->kg[1].root) / 2);
    os.midi_in_read = 0;
    os.multi_cursor = 7; // Zone 2 (Zone 2 From Velocity shows once it is set)
    pd = blank(), pd.pad_hit[4] = 100, pd.pad_down[4] = 1;
    frame(&pd);
    pd = blank(), pd.pad_release[4] = 1;
    frame(&pd);
    CHECK(k->kg[0].zone[1] == 5);
    os.toast_until = 0;
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "multi_kg_zone2");
    // A message too long for one line takes two.
    snprintf(os.toast, sizeof os.toast, "Press a pad with a sound first");
    os.toast_until = 999999;
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "toast_two_lines");
    os.toast_until = 0;
    struct input sh = blank();
    // <Add Pad>: a pad loaded by hand, at the root its pitch gives; SHIFT + B2 <Remove>.
    os_pad_load(&os, 10, tone(note_hz(84), 44100, 22050, 0.5f), "Whistle");
    os.multi_cursor = 0; // off the Zones: a pad pressed is only remembered
    int n0 = k->n;
    pd = blank(), pd.pad_hit[10] = 100, pd.pad_down[10] = 1;
    frame(&pd);
    pd = blank(), pd.pad_release[10] = 1;
    frame(&pd);
    sh.down = sh.held = (1u << IN_B3) | (1u << IN_SHIFT), sh.shift = 1; // SHIFT + B3 <Add Pad>
    frame(&sh);
    CHECK(k->n == n0 + 1 && k->kg[k->n - 1].root == 84 && k->kg[k->n - 1].zone[0] == 11 && k->kg[k->n - 1].hi == 127);
    sh.down = sh.held = (1u << IN_B2) | (1u << IN_SHIFT), sh.shift = 1; // SHIFT + B2 <Remove>
    frame(&sh);
    CHECK(k->n == n0);
    tap(IN_B1); // KEY TRACK
    CHECK(os.multi_page == 1);
    struct input w = blank();
    w.encoder = 1; // Template: Flat to Piano
    frame(&w);
    CHECK(os.pads[0].sp.inst_tpl == 1 && os.pads[0].sp.kt_pan == 20 && os.pads[0].sp.kt_env == 60 && os.pads[0].sp.release == 400);
    render(16);
    CHECK(e.km[e.km_of[slot] - 1].kt_pan == 20);
    os.toast_until = 0;
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "multi_kt");
    tap(IN_B1);
    tap(IN_B3); // AUTO MAP: its box, then <Do It> on every pad
    CHECK(os.multi_page == 2);
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "multi_map");
    tap(IN_B3);
    CHECK(os.multi_page == 0 && k->n >= 5);

    // The Auto Sampler, played by hand: two samples with two velocity layers; a Re-Do; a sample
    // with no pitch is asked its note. The program has sounds: asked to clear them (kept in memory).
    tap(IN_B2); // B2 <Smplr>: the Auto Sampler
    CHECK(os.screen == OS_RECORD && os.record_page == 6);
    CHECK(os.rec_set[6].rate == 29760 && os.rec_set[6].bits == 12 && os.rec_set[6].mode == REC_MONO_LR && os.as_layers == 1 &&
          os.as_thres == -30 && os.as_len == 30);
    os.toast_until = 0;
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "autosampler");
    os.as_samples = 2, os.as_layers = 2, os.as_len = 3; // 0.3 s each
    atomic_store(&os.rec.present, 1);
    int memory0 = os.nmemory;
    tap(IN_B3); // <Rec>
    CHECK(os.as_ask == 'C');
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "autosampler_clear");
    tap(IN_B1); // Yes
    CHECK(os.as_ask == 0 && os_program_sounds(&os, 0) == 0 && os.nmemory > memory0 && os.as_total == 4);
    struct ui au = {.px = px, .now_ms = 5000, .track_led = -1};
    static float in2[256];
    // What is played: sample 1 an A3, soft then loud; sample 2 noise (no pitch), soft then loud.
    // The first loud take is played wrong and done again (<Re-Do>).
    int redone = 0, noted = 0;
    for (int b = 0; b < 400000 && (os.as_state != 0 || os.as_ask); b++) {
        long long clock = atomic_load(&e.clock);
        if (os.as_ask == 'N' && !noted) {
            CHECK(os.as_note_k == 1);
            os.toast_until = 0;
            frame(&idle);
            if (shot_dir) write_shot(shot_dir, "autosampler_note");
            struct input wn = blank();
            wn.encoder = 2; // C4 to D4
            frame(&wn);
            tap(IN_B3); // <OK>
            noted = 1;
            continue;
        }
        int take = os.as_at, layer = take % 2, sample = take / 2;
        int waiting = os.as_state == 1, recording = os.as_state == 2;
        if (take == 2 && waiting && !redone) { // after the first loud take: Re-Do it
            tap(IN_B2);
            redone = 1;
            CHECK(os.as_at == 2 || os.as_state == 3);
        }
        float level = layer ? 0.6f : 0.2f;
        for (int i = 0; i < 128; i++) {
            float x = 0;
            if (waiting || recording) {
                if (sample == 0) x = level * (float)sin(2 * M_PI * note_hz(57) * (clock + i) / 44100);
                else r = r * 1664525u + 1013904223u, x = level * ((float)(r >> 8) / (1 << 24) - 0.5f) * 2;
            }
            in2[i * 2] = in2[i * 2 + 1] = x;
        }
        recorder_input(&os.rec, in2, 128, clock);
        recorder_service(&os.rec);
        render(128);
        au.now_ms += 3;
        os_frame(&au, &idle, &os);
        if (os.as_state == 2 && take == 1 && shot_dir && b % 50 == 0) {
            os.toast_until = 0;
            write_shot(shot_dir, "autosampler_rec");
        }
    }
    CHECK(redone && noted && os.as_state == 0);
    int inst = os_instrument(&os, 0);
    const struct s600_kgs *ak = &os.pads[inst].kgs;
    CHECK(os.screen == OS_MULTI && inst == 0 && ak->n == 2);
    CHECK(ak->kg[0].root == 57 && ak->kg[0].zone[0] == 1 && ak->kg[0].zone[1] == 2 && ak->kg[0].split == 64);
    CHECK(ak->kg[1].root == 62 && ak->kg[1].zone[0] == 3 && ak->kg[1].zone[1] == 4 && ak->kg[0].lo == 0 && ak->kg[1].hi == 127);
    CHECK(strcmp(os.pads[0].name, "AS A3") == 0 && strcmp(os.pads[1].name, "AS A3 v2") == 0 && strcmp(os.pads[3].name, "AS D4 v2") == 0);
    CHECK(os.pads[0].s->rate == 29760 && os.pads[0].s->channels == 1);
    float soft = 0, loud = 0;
    for (int i = 0; i < os.pads[0].s->frames; i++) soft = fmaxf(soft, fabsf(os.pads[0].s->data[i]));
    for (int i = 0; i < os.pads[1].s->frames; i++) loud = fmaxf(loud, fabsf(os.pads[1].s->data[i]));
    CHECK(soft > 0.25f && soft < 0.4f && loud > 0.6f); // normalized ALL: the layers keep their levels
    CHECK(multi_detect_pitch(os.pads[0].s, &heard) && fabs(heard - 57) < 0.1);
    os.toast_until = 0;
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "autosampler_done");

    // <End> part way: map what is recorded.
    record_open_autosampler(&os);
    os.as_samples = 3, os.as_layers = 1;
    tap(IN_B3);
    CHECK(os.as_ask == 'C');
    tap(IN_B2); // No: the pads stay; takes go on empty pads after them
    CHECK(os.as_ask == 0 && os.as_state == 1);
    for (int b = 0; b < 400000 && os.as_at < 1; b++) {
        long long clock = atomic_load(&e.clock);
        for (int i = 0; i < 128; i++) in2[i * 2] = in2[i * 2 + 1] = 0.3f * (float)sin(2 * M_PI * note_hz(69) * (clock + i) / 44100);
        recorder_input(&os.rec, in2, 128, clock);
        recorder_service(&os.rec);
        render(128);
        au.now_ms += 3;
        os_frame(&au, &idle, &os);
    }
    tap(IN_B1); // <End>
    for (int b = 0; b < 10 && os.as_ask != 'E'; b++) {
        memset(in2, 0, sizeof in2);
        recorder_input(&os.rec, in2, 128, atomic_load(&e.clock));
        au.now_ms += 3;
        os_frame(&au, &idle, &os);
    }
    CHECK(os.as_ask == 'E');
    tap(IN_B1); // Yes
    CHECK(os.as_state == 0 && os.pads[4].s && strcmp(os.pads[4].name, "AS A4") == 0 && os.pads[0].kgs.n == 1);
    os.seq.midi_send = 0;
    atomic_store(&os.rec.present, 0);
    os.data_dir = 0;
    os.screen = OS_MAIN;
}

// SAMPLE REC again: Audiotrack Record. The take starts on the sequence's first tick after the
// count-in (as it comes back through the input) and runs the sequence's length; a pad, which
// must be empty, gets it with a note at the start of the active track.
static void test_audio_track(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    os.seq.sequence[0].bars = 1; // 2 s at 120 BPM
    load_pad(0);
    tap(IN_SAMPLE_REC);
    tap(IN_SAMPLE_REC);
    CHECK(os.screen == OS_RECORD && os.record_page == 1 && os.cursor_record == 0);
    CHECK(os.rec_set[1].bits == 12 && os.rec_set[1].rate == 29760 && os.rec_set[1].count_bars == 2 &&
          os.rec_set[1].length == RLEN_SEQ);
    os.rec_set[1].rate = 44100; // the timings below are in 44.1 kHz frames
    struct input idle = blank();
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "audiotrack");
    os.rec_set[1].count_bars = 1; // the timings below count one bar in
    atomic_store(&os.rec.present, 1);
    os.rec.latency = 300;
    tap(IN_B3); // <Rec>: the count-in starts
    CHECK(atomic_load(&os.rec.state) == REC_ARMED && os.seq.mode == SEQ_COUNT_IN && os.at_running);
    // As the audio thread runs: a block of input, then a block of output. The input is
    // silent until the sequence's start comes back through the input, then loud.
    static float buf[256];
    for (int b = 0; b < 2000 && os.at_running; b++) {
        long long clock = atomic_load(&e.clock);
        for (int i = 0; i < 128; i++) {
            int loud = os.seq.zero_seen && clock + i >= os.seq.zero_clock + os.rec.latency;
            buf[i * 2] = buf[i * 2 + 1] = loud ? 0.5f : 0.0f;
        }
        recorder_input(&os.rec, buf, 128, clock);
        render(128);
        if (b % 16 == 0) frame(&idle);
    }
    for (int i = 0; i < 4; i++) {
        recorder_input(&os.rec, buf, 128, atomic_load(&e.clock));
        render(128);
        frame(&idle);
    }
    CHECK(os.seq.mode == SEQ_STOPPED && os.rec_result);
    const struct sample *t = os.rec_result;
    CHECK(t && labs(t->frames - 2 * ENGINE_RATE) <= 2 && t->data[0] > 0.9f && t->data[(t->frames - 1) * 2] > 0.9f);
    if (shot_dir) write_shot(shot_dir, "audiotrack_done");
    struct input in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1; // A01 has a sound: not allowed
    frame(&in);
    in = blank();
    in.pad_release[0] = 1;
    frame(&in);
    CHECK(os.rec_result != 0);
    in = blank();
    in.pad_hit[5] = 100, in.pad_down[5] = 1;
    frame(&in);
    in = blank();
    in.pad_release[5] = 1;
    frame(&in);
    const struct seq_track *k = &os.seq.sequence[0].track[0];
    CHECK(!os.rec_result && os.pads[5].s && strcmp(os.pads[5].name, "Audio01") == 0);
    CHECK(k->count == 1 && k->notes[0].tick == 0 && k->notes[0].note == 5 && k->notes[0].dur >= 380);

    // SAMPLE REC again, from Audiotrack Record: Convert Sample, here pad A06 down to 11.02 kHz.
    tap(IN_SAMPLE_REC);
    CHECK(os.record_page == 2 && os.cursor_record == 0 && os.rec_set[2].rate == 29760 && os.rec_set[2].bits == 12 &&
          os.rec_set[2].mode == REC_MONO_LR);
    CHECK(os.selected == 5);
    os.rec_set[2].rate = 11020;
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "convert");
    struct sample *original = os.pads[5].s;
    int memory = os.nmemory;
    tap(IN_B3); // <Convert>: the pad's sound is replaced at once, the original kept in memory
    const struct os_pad *cv = &os.pads[5];
    CHECK(!os.rec_result && cv->s && cv->s->rate == 11020 && labs(cv->s->frames - 2 * 11020) <= 2);
    CHECK(strcmp(cv->name, "Audio01-11k12") == 0 && labs(cv->p.end - cv->s->frames) <= 1);
    CHECK(os.nmemory == memory + 1 && os.memory[memory].s == original);
    os.screen = OS_MAIN;
}

// Shift + NOTE REPEAT: the page where Note Repeat's rate is picked, without turning it on or off.
static void test_repeat_rate(const char *shot_dir) {
    seq_fresh();
    CHECK(os.seq.rep_rate == REP_16 && seq_repeat_ticks(REP_16) == 24 && seq_repeat_ticks(REP_8T) == 32);
    struct input in = blank();
    in.down = in.held = (1u << IN_NOTE_REPEAT) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_REPEAT && os.seq.repeat == 0);
    in = blank();
    in.up = 1u << IN_NOTE_REPEAT;
    frame(&in);
    CHECK(os.seq.repeat == 0);
    CHECK(os.cursor_repeat == 0); // Type: first
    os.cursor_repeat = 1; // Rate
    in = blank();
    in.encoder = -1; // 1/16 to 1/8T
    frame(&in);
    CHECK(os.seq.rep_rate == REP_8T);
    in = blank();
    in.fader_moved = 1, in.fader = 0;
    frame(&in);
    CHECK(os.seq.rep_rate == REP_4);
    in.fader = 127;
    frame(&in);
    CHECK(os.seq.rep_rate == REP_32T);
    struct input idle = blank();
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "repeat");
    tap(IN_B1); // <Back>
    CHECK(os.screen == OS_MAIN);

    // From 1/16: B1 1/8T, B2 1/16T, B3 1/32. From 1/16T: 1/8T, 1/32T, 1/32.
    CHECK(seq_repeat_alt(REP_16, 0) == REP_8T && seq_repeat_alt(REP_16, 1) == REP_16T && seq_repeat_alt(REP_16, 2) == REP_32);
    CHECK(seq_repeat_alt(REP_16T, 0) == REP_8T && seq_repeat_alt(REP_16T, 1) == REP_32T && seq_repeat_alt(REP_16T, 2) == REP_32);
    CHECK(seq_repeat_alt(REP_4, 0) == -1 && seq_repeat_alt(REP_32T, 2) == -1);
    os.seq.rep_rate = REP_16;
    in = blank();
    in.down = in.held = 1u << IN_NOTE_REPEAT;
    frame(&in);
    int track = os.seq.track;
    in = blank();
    in.down = in.held = (1u << IN_NOTE_REPEAT) | (1u << IN_B3); // B3 held with it: 1/32
    frame(&in);
    CHECK(os.seq.rep_alt == REP_32 && os.seq.track == track); // and the main screen's <Trk+> did nothing
    if (shot_dir) write_shot(shot_dir, "repeat_keys");
    in = blank();
    in.held = 1u << IN_NOTE_REPEAT, in.up = 1u << IN_B3;
    frame(&in);
    CHECK(os.seq.rep_alt == -1 && os.seq.repeat == 1); // let go: back to 1/16
    in = blank();
    in.down = in.held = (1u << IN_NOTE_REPEAT) | (1u << IN_B1);
    frame(&in);
    CHECK(os.seq.rep_alt == REP_8T);
    in = blank();
    in.up = 1u << IN_NOTE_REPEAT; // NOTE REPEAT let go first: all of it stops
    frame(&in);
    CHECK(os.seq.repeat == 0 && os.seq.rep_alt == -1);
}

// The Sample screen's Sample: field steps a pad through every sound in memory. A sound
// that leaves its last pad is kept in memory, so scrolling past never loses it.
static void test_pick_sound(void) {
    seq_fresh();
    os_pad_load(&os, 0, constant(100), "One");
    os_pad_load(&os, 1, constant(200), "Two");
    struct sample *one = os.pads[0].s, *two = os.pads[1].s;
    os.pads[0].p.volume = 70;
    os.screen = OS_SAMPLE;
    os.selected = 0;
    os.page = PAGE_TRIM;
    os.frame_slot = -1;
    struct input in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.pads[0].s == two && strcmp(os.pads[0].name, "Two") == 0 && os.pads[0].p.end == 200);
    CHECK(os.pads[0].p.volume == 70); // the pad's own settings stay
    CHECK(os.nmemory == 1 && os.memory[0].s == one); // "One" had no other pad: kept in memory
    frame(&in); // on round: "One" again, from memory
    CHECK(os.pads[0].s == one);
    tap(IN_SAMPLE); // pressed again: Zone Start, as before
    CHECK(os.frame_slot == 0);
    os.screen = OS_MAIN;
}

// The Program screen's pad commands: Copy and Swap take the next pad hit, Delete asks first,
// and Snd: gives the selected pad any sound in memory.
static void test_program_pads(const char *shot_dir) {
    seq_fresh();
    os_pad_load(&os, 0, constant(100), "Kick");
    os_pad_load(&os, 1, constant(200), "Snare");
    os.selected = 0;
    struct input in = blank();
    in.down = in.held = (1u << IN_SAMPLE) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_PROGRAM);
    tap(IN_B1); // <Copy>, then hit A05
    CHECK(os.pgm_mode == PGM_COPY);
    struct input idle = blank();
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "program_copy");
    in = blank();
    in.pad_hit[4] = 100, in.pad_down[4] = 1;
    frame(&in);
    CHECK(os.pgm_mode == PGM_NONE && os.pads[4].s == os.pads[0].s && os.pads[4].s->refs >= 2 && voices() == 0);
    CHECK(strcmp(os.pads[4].name, "Kick") == 0 && os.selected == 4);
    os.selected = 1;
    tap(IN_B2); // <Swap> A02 with A05
    in = blank();
    in.pad_hit[4] = 100, in.pad_down[4] = 1;
    frame(&in);
    CHECK(strcmp(os.pads[1].name, "Kick") == 0 && strcmp(os.pads[4].name, "Snare") == 0);
    tap(IN_B3); // <Delete> A05: asks, then empties it
    CHECK(os.pgm_mode == PGM_DELETE && os.pads[4].s);
    tap(IN_B3);
    CHECK(os.pgm_mode == PGM_NONE && !os.pads[4].s);
    os.cursor_program = 3; // Snd: the empty pad takes a sound from memory
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.pads[4].s != 0);
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "program_pads");
    // Copy to another program: the wheel picks it, the pad hit is that program's pad; Swap too.
    {
        int here = os.program, there = here + 1;
        os.selected = 1; // Kick
        tap(IN_B1);
        in = blank();
        in.encoder = 1;
        frame(&in);
        CHECK(os.pgm_mode == PGM_COPY && os.pgm_target == there);
        frame(&idle);
        if (shot_dir) write_shot(shot_dir, "program_copy_other");
        in = blank();
        in.pad_hit[6] = 100, in.pad_down[6] = 1;
        frame(&in);
        CHECK(os.pgm_mode == PGM_NONE && os.programs[there].pads[6].s == os.pads[1].s && os.program == here &&
              !strcmp(os.programs[there].pads[6].name, "Kick") && os.selected == 1);
        tap(IN_B2); // Swap A02 (Kick) with Pgm there's A07 (also Kick): then with its empty A08
        in = blank();
        in.encoder = 1;
        frame(&in);
        in = blank();
        in.pad_hit[7] = 100, in.pad_down[7] = 1;
        frame(&in);
        CHECK(!os.pads[1].s && os.programs[there].pads[7].s && !strcmp(os.programs[there].pads[7].name, "Kick"));
    }
    // SHIFT + wheel on Snd: only sounds no pad plays. One goes into memory, unused; the empty A02 takes it.
    {
        struct sample *loose = constant(300);
        os_memory_add(&os, loose, "Loose");
        os.cursor_program = 3;
        os.selected = 1;
        in = blank();
        in.encoder = 1, in.shift = 1, in.held = 1u << IN_SHIFT;
        frame(&in);
        CHECK(os.pads[1].s == loose && !strcmp(os.pads[1].name, "Loose"));
        frame(&in); // the only unused one: stays
        CHECK(os.pads[1].s == loose);
    }
    os.screen = OS_MAIN;
}

// CHOP: Zone Assign. REGIONS divides the sound evenly; PAD marks zones with pad hits as it
// plays through. Either way the zones go, in order, to empty pads: Note On, a free mute group.
// A drum break, made here: kicks (a falling sine with a click), snares (noise and a body tone),
// soft ghost snares, and hats (bright noise) on every 8th, some right on a kick or a snare, at 95
// BPM. Where each hit starts, and which are ghosts.
static struct sample *synth_break(int32_t *hits, int *nhits) {
    const int rate = 44100;
    double eighth = 60.0 / 95.0 / 2.0;
    int n = (int)(rate * eighth * 16) + rate / 2;
    struct sample *t = sample_new(n, 1, rate);
    memset(t->data, 0, (size_t)n * sizeof(float));
    uint32_t r = 7;
    // 16 eighths: K . S . K K S . | K . S g K . S g  (g: ghost snare); a hat on every eighth too
    const char *pat = "K.S.KKS.K.S.K.S.";
    const char *gho = "..........g...g.";
    *nhits = 0;
    for (int e = 0; e < 16; e++) {
        int at = (int)(e * eighth * rate) + rate / 20; // a little lead-in first
        char k = pat[e], g = gho[e];
        hits[(*nhits)++] = at; // a hat, or more, starts here every eighth
        for (int i = 0; i < rate / 3 && at + i < n; i++) {
            double tt = (double)i / rate;
            float x = 0;
            r = r * 1664525u + 1013904223u;
            float nz = (float)(r >> 8) / (1 << 24) * 2 - 1;
            x += 0.18f * nz * expf(-(float)tt * 60) * (i % 2 ? 1 : -1); // the hat: bright noise
            if (k == 'K') x += 0.8f * (float)sin(2 * M_PI * (50 + 80 * exp(-tt * 30)) * tt) * expf(-(float)tt * 9);
            if (k == 'S') x += 0.5f * nz * expf(-(float)tt * 18) + 0.3f * (float)sin(2 * M_PI * 190 * tt) * expf(-(float)tt * 20);
            if (g == 'g') x += 0.12f * nz * expf(-(float)tt * 25); // a ghost: soft
            t->data[at + i] += x;
        }
        if (g == 'g') { // the ghost lands a 16th after this eighth
            int ga = at + (int)(eighth / 2 * rate);
            hits[(*nhits)++] = ga;
            for (int i = 0; i < rate / 6 && ga + i < n; i++) {
                double tt = (double)i / rate;
                r = r * 1664525u + 1013904223u;
                float nz = (float)(r >> 8) / (1 << 24) * 2 - 1;
                t->data[ga + i] += 0.14f * nz * expf(-(float)tt * 25) + 0.08f * (float)sin(2 * M_PI * 190 * tt) * expf(-(float)tt * 25);
            }
        }
    }
    // in time order
    for (int i = 0; i < *nhits; i++)
        for (int j = i + 1; j < *nhits; j++)
            if (hits[j] < hits[i]) {
                int32_t x = hits[i];
                hits[i] = hits[j], hits[j] = x;
            }
    return t;
}

// TRANSIENT: a zone at every hit of a break, the ghosts too, each starting just before its hit.
static void test_zones_transient(const char *shot_dir) {
    int32_t hits[64], found[ZONES_MAX];
    int nhits;
    struct sample *brk = synth_break(hits, &nhits);
    int n = onset_detect_transient(brk, found, 0, ZONES_MAX, 5);
    int matched = 0;
    for (int i = 0; i < nhits; i++)
        for (int k = 0; k < n; k++)
            if (found[k] <= hits[i] && hits[i] - found[k] <= 44100 * 4 / 1000) { // within 4 ms before it
                matched++;
                break;
            }
    CHECK(matched == nhits && n == nhits);
    // Sensitivity: lower finds no more than higher; at 10 nothing is lost.
    CHECK(onset_detect_transient(brk, found, 0, ZONES_MAX, 1) <= onset_detect_transient(brk, found, 0, ZONES_MAX, 10) &&
          onset_detect_transient(brk, found, 0, ZONES_MAX, 10) >= nhits);
    // Silence and a single hit: one zone.
    struct sample *quiet = sample_new(44100, 1, 44100);
    memset(quiet->data, 0, 44100 * sizeof(float));
    CHECK(onset_detect_transient(quiet, found, 0, ZONES_MAX, 5) == 1 && found[0] == 0);
    sample_free(quiet);
    // On the Zone Assign page.
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    os_pad_load(&os, 0, brk, "Break");
    os.selected = 0;
    tap(IN_CHOP);
    struct input in = blank();
    in.encoder = 2; // Mode: PAD, REGIONS, TRANSIENT
    frame(&in);
    CHECK(os.zone_mode == ZONE_TRANSIENT && os.zone_count == nhits && os.zone_start[0] < hits[0]);
    struct input idle = blank();
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "zones_transient");
    os.cursor_zones = 2, os.zone_cur = 0; // Zone 3: deleted (B2)
    in.encoder = 2;
    frame(&in);
    CHECK(os.zone_cur == 2);
    int32_t third = os.zone_start[3];
    tap(IN_B2);
    CHECK(os.zone_count == nhits - 1 && os.zone_start[2] == third);
    os.cursor_zones = 3; // Start: a millisecond a click
    int32_t was = os.zone_start[2];
    in.encoder = -1;
    frame(&in);
    CHECK(was - os.zone_start[2] == 44);
    tap(IN_B3); // <Do It>, then <Do It>: a new program, a zone a pad
    tap(IN_B3);
    CHECK(os.pads[0].s == brk && os.pads[nhits - 2].s == brk && !os.pads[nhits - 1].s);
    os.screen = OS_MAIN;
}

static void test_zones(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    os_pad_load(&os, 0, counting(40000), "Loop");
    os.pads[3].p.mute_group = 1; // so a bank of this program gets group 2
    os_pad_load(&os, 3, constant(10), "Hat");
    os.pads[3].p.mute_group = 1;
    os.selected = 0;
    tap(IN_CHOP);
    CHECK(os.screen == OS_ZONES && os.zone_mode == ZONE_PAD && os.cursor_zones == 0);
    struct input in = blank();
    in.encoder = 1; // Mode: REGIONS
    frame(&in);
    CHECK(os.zone_mode == ZONE_REGIONS && os.zone_count == 8 && os.zone_start[1] == 5000);
    os.cursor_zones = 1; // Regions: 4
    in.encoder = -4;
    frame(&in);
    CHECK(os.zone_count == 4 && os.zone_start[2] == 20000);
    os.cursor_zones = 2; // Zone 2, its start moved later
    in.encoder = 1;
    frame(&in);
    os.cursor_zones = 3;
    in.encoder = 2;
    frame(&in);
    CHECK(os.zone_cur == 1 && os.zone_start[1] > 10000);
    struct input idle = blank();
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "zones");
    tap(IN_B3); // <Do It>: where to
    CHECK(os.zone_phase == ZP_ASSIGN && os.zone_dest == 0 && os.zone_mgrp == 1);
    in = blank();
    in.encoder = 1; // bank A has sounds: on to bank B
    frame(&in);
    CHECK(os.zone_dest == 2 && os.zone_mgrp == 2);
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "zones_assign");
    tap(IN_B3);
    const struct os_pad *z = &os.pads[16];
    CHECK(z[0].s == os.pads[0].s && z[0].p.start == 0 && z[0].p.end == z[1].p.start); // each ends where the next starts
    CHECK(z[1].p.start > 10000 && z[3].p.end == 40000 && z[2].p.gate == 0 && z[3].p.mute_group == 2);
    CHECK(strcmp(z[1].name, "Loop-02") == 0 && !os.pads[20].s && os.pads[0].s->refs >= 5);

    // PAD: a pad starts it, later hits mark zones, and it ends by itself.
    os.selected = 0;
    in = blank();
    in.encoder = -1; // Mode: PAD
    os.cursor_zones = 0;
    frame(&in);
    os.zone_sens = 0; // no snapping, so the marks land where they were hit
    tap(IN_B3);
    CHECK(os.zone_phase == ZP_WAIT);
    in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    CHECK(os.zone_phase == ZP_PLAY && os.zone_count == 1 && voice_on(-4));
    for (int i = 0; i < 160; i++) render(128); // 20480 frames in
    in = blank();
    in.pad_hit[5] = 100, in.pad_down[5] = 1;
    frame(&in);
    CHECK(os.zone_count == 2 && os.zone_start[1] > 19500 && os.zone_start[1] < 21000); // as heard, less the output delay
    for (int i = 0; i < 200; i++) render(128);
    frame(&idle);
    CHECK(os.zone_phase == ZP_ASSIGN);
    tap(IN_B3); // a new program
    CHECK(os.program == 1 && os.seq.sequence[0].track[0].kit == 1 && os.pads[0].s && os.pads[1].s && !os.pads[2].s);
    CHECK(os.pads[1].p.start > 19500 && os.pads[0].p.end == os.pads[1].p.start && os.pads[1].p.gate == 0);
    os.screen = OS_MAIN;
}

// Shift + -: Undo, Shift + +: Redo, two steps each way, for audio commands and sequence
// changes. A setting like volume is not a step.
static void test_undo(void) {
    seq_fresh();
    os_pad_load(&os, 0, counting(1000), "Count");
    struct sample *orig = os.pads[0].s;
    os.screen = OS_SAMPLE;
    os.selected = 0;
    os.page = PAGE_TRIM;
    set_zone(100, 200);
    do_op(DO_DISCARD); // step 1
    CHECK(os.pads[0].s->frames == 100);
    os.pads[0].p.volume = 50; // not a step
    set_zone(10, 20);
    do_op(DO_DISCARD); // step 2
    CHECK(os.pads[0].s->frames == 10);
    seq_add_note(&os.seq, 0, 0, 0, 0, 100, 24); // (as if recorded)
    os_undo_save(&os, "Record"); // step 3, and the oldest falls off: two steps back at most
    seq_add_note(&os.seq, 0, 0, 96, 0, 100, 24);
    struct input in = blank();
    in.down = in.held = (1u << IN_MINUS) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in); // undo the recording
    CHECK(os.seq.sequence[0].track[0].count == 1 && os.pads[0].s->frames == 10);
    frame(&in); // undo the second Discard
    CHECK(os.pads[0].s->frames == 100 && os.pads[0].p.volume == 50); // the pad as it was just before
    frame(&in); // nothing more: two steps
    CHECK(os.pads[0].s->frames == 100 && os.pads[0].s != orig);
    in.down = in.held = (1u << IN_PLUS) | (1u << IN_SHIFT);
    frame(&in); // redo the second Discard
    CHECK(os.pads[0].s->frames == 10);
    frame(&in); // and the recording
    CHECK(os.seq.sequence[0].track[0].count == 2);
    render(16);
    engine_reap(&e);
    CHECK(os.pads[0].s->frames == 10); // the engine still has a sound for it
    os.screen = OS_MAIN;
}

// S600 Pgm Mode on the Program screen: a program played through its pads' S600 settings.
static void test_program_type(void) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    load_pad(0);
    struct input in = blank();
    in.down = in.held = (1u << IN_SAMPLE) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_PROGRAM && !os.programs[0].s600);
    os.cursor_program = 1; // S600 Pgm Mode: ON
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.programs[0].s600 && os.seq.kit_s600[0]);
    seq_add_note(&os.seq, 0, 0, 0, 0, 100, 24);
    seq_play(&os.seq, 1);
    render(256);
    const struct voice *v = voice_on(ENGINE_S600 + 0);
    CHECK(v && v->p.synth == 1);
    seq_stop(&os.seq, 1);
    tap(IN_SAMPLE); // and SAMPLE opens the S600 screen
    CHECK(os.screen == OS_S600);
    os_program_set_s600(&os, 0, 0);
    os.screen = OS_MAIN;
}

// Shift + B3 on the sound screens: Edit All Pads. A change goes to every pad with a sound;
// the trim stays each pad's own.
static void test_edit_all(void) {
    seq_fresh();
    os_pad_load(&os, 0, constant(100), "One");
    os_pad_load(&os, 1, constant(200), "Two");
    os.screen = OS_SAMPLE;
    os.selected = 0;
    tap(IN_B1);
    CHECK(os.page == PAGE_MIX); // Vol: Volume, Pan
    struct input in = blank();
    in.down = in.held = (1u << IN_B3) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.edit_all == 1);
    os.frame_slot = 0;
    in = blank();
    in.encoder = -20;
    frame(&in);
    CHECK(os.pads[0].p.volume == 80 && os.pads[1].p.volume == 80 && !os.pads[2].s);
    tap(IN_B1); // Trim: each pad's own
    os.frame_slot = 1;
    in = blank();
    in.encoder = -1, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.pads[0].p.end == 99 && os.pads[1].p.end == 200);
    in = blank();
    in.down = in.held = (1u << IN_B3) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.edit_all == 0);
    os.screen = OS_MAIN;
}

// Edit All Pads only ever reaches the program being edited: the wheel, the knobs, the
// slider and a toggle on the Sample screen, and the S600 screen, leave other programs alone.
static void test_edit_all_one_program(void) {
    seq_fresh();
    os_set_program(&os, 1);
    os_pad_load(&os, 0, constant(100), "Other A");
    os_pad_load(&os, 1, constant(100), "Other B");
    struct os_pad other[2] = {os.programs[1].pads[0], os.programs[1].pads[1]};
    os_set_program(&os, 0);
    os_pad_load(&os, 0, constant(100), "One");
    os_pad_load(&os, 1, constant(200), "Two");
    os.screen = OS_SAMPLE, os.selected = 0;
    tap(IN_B1);
    struct input in = blank();
    in.down = in.held = (1u << IN_B3) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.edit_all == 1);
    os.frame_slot = 0;
    in = blank(), in.encoder = -15;
    frame(&in);
    for (int k = 0; k < 3; k++) { // every knob, swept
        in = blank(), in.knob[k] = 0;
        frame(&in);
        in = blank(), in.knob[k] = 127;
        frame(&in);
        in = blank(), in.knob[k] = 20;
        frame(&in);
    }
    in = blank(), in.fader = 0;
    frame(&in);
    in = blank(), in.fader = 90;
    frame(&in);
    tap(IN_B2), tap(IN_B3); // the other page groups, edited too
    in = blank(), in.encoder = 7;
    frame(&in);
    CHECK(os.pads[0].p.volume == os.pads[1].p.volume);
    for (int i = 0; i < 2; i++)
        CHECK(!memcmp(&os.programs[1].pads[i].p, &other[i].p, sizeof other[i].p) &&
              !memcmp(&os.programs[1].pads[i].sp, &other[i].sp, sizeof other[i].sp));
    os.edit_all = 0;
    os.screen = OS_MAIN;
}

static struct sample *sine_at(double hz, int frames) {
    struct sample *s = sample_new(frames, 1, ENGINE_RATE);
    for (int i = 0; i < frames; i++) s->data[i] = 0.25f * (float)sin(2 * M_PI * hz * i / ENGINE_RATE);
    return s;
}

static float out_peak(int blocks) {
    float p = 0;
    for (int b = 0; b < blocks; b++) {
        render(128);
        for (int i = 0; i < 256; i++) p = fabsf(block[i]) > p ? fabsf(block[i]) : p;
    }
    return p;
}

// The mixer's EQ: K1-K3 set a track's low, mid and high, and its notes go through it. B3 opens
// its page; B1 mutes, B2 solos; Shift + B1 resets every track's EQ once confirmed.
static void test_track_eq(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    os_pad_load(&os, 0, sine_at(80, 44100), "Low");
    struct seq_track *k = &os.seq.sequence[0].track[0];
    seq_add_note(&os.seq, 0, 0, 0, 0, 127, 300);
    struct input in = blank();
    in.pad_hit[8] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.screen == OS_MIXER && k->eq.type == EQ_1073 && k->eq.lf == 110);
    seq_play(&os.seq, 1);
    for (int i = 0; i < 20; i++) render(128);
    float flat = out_peak(20);
    seq_stop(&os.seq, 1);
    for (int i = 0; i < 20; i++) render(128);
    in = blank(); // K1 from the middle (0 dB) to the top: the low shelf +16 dB
    in.knob_moved[0] = 1, in.knob[0] = 63;
    frame(&in);
    in.knob[0] = 127;
    frame(&in);
    CHECK(k->eq.lg == 160);
    { struct input idle = blank(); frame(&idle); }
    if (shot_dir) write_shot(shot_dir, "mixer_eq");
    seq_play(&os.seq, 1);
    for (int i = 0; i < 20; i++) render(128);
    float boosted = out_peak(20);
    seq_stop(&os.seq, 1);
    CHECK(boosted > flat * 3.0f); // about +14 dB at 80 Hz under a 110 Hz shelf
    for (int i = 0; i < 20; i++) render(128);

    tap(IN_B1); // <Mute>: its notes stop
    CHECK(k->mute == 1);
    engine_kill_all(&e);
    for (int i = 0; i < 4; i++) render(128);
    seq_play(&os.seq, 1);
    render(256);
    CHECK(voices() == 0);
    seq_stop(&os.seq, 1);
    tap(IN_B1);
    tap(IN_B2); // <Solo> on another track: this one is silent
    os.mixer_track = 1;
    tap(IN_B2);
    os.mixer_track = 0;
    tap(IN_B2);
    CHECK(k->solo == 0 && os.seq.sequence[0].track[1].solo == 1);
    engine_kill_all(&e);
    for (int i = 0; i < 4; i++) render(128);
    seq_play(&os.seq, 1);
    render(256);
    CHECK(voices() == 0);
    seq_stop(&os.seq, 1);
    os.seq.sequence[0].track[1].solo = 0;

    tap(IN_B3); // <EQ>: its page
    CHECK(os.screen == OS_EQ && os.cursor_eq == 0);
    in = blank();
    in.encoder = 1; // Type: 4000 E, the gains kept, +16 brought to its +15
    frame(&in);
    CHECK(k->eq.type == EQ_4000E && k->eq.lg == 150 && k->eq.hf == 8000);
    { struct input idle = blank(); frame(&idle); }
    if (shot_dir) write_shot(shot_dir, "eq_page");
    tap(IN_B1); // <Back>
    CHECK(os.screen == OS_MIXER);
    in = blank();
    in.down = in.held = (1u << IN_B1) | (1u << IN_SHIFT), in.shift = 1; // <Reset EQ>: asks first
    frame(&in);
    CHECK(os.mixer_ask && k->eq.lg == 150);
    tap(IN_B3); // <Yes>
    CHECK(!os.mixer_ask && k->eq.lg == 0 && k->eq.type == EQ_4000E);
    os.screen = OS_MAIN;
}

static int rt_start, rt_clock, rt_stop;
static unsigned char out_log[32][3];
static int out_n;
static void capture_midi(const unsigned char *m, int n) {
    if (n == 1) {
        rt_start += m[0] == 0xfa, rt_clock += m[0] == 0xf8, rt_stop += m[0] == 0xfc;
    } else if (out_n < 32) {
        out_log[out_n][0] = m[0], out_log[out_n][1] = m[1], out_log[out_n][2] = n > 2 ? m[2] : 0;
        out_n++;
    }
}

static void midi_send_in(int a, int b, int c) {
    midi_qn = midi_qat = 0;
    midi_push(a, b, c);
    struct input idle = blank();
    frame(&idle);
}

// Shift + Pad 8: MIDI Settings, its CC Map and Pad Notes, and what they do.
static void test_midi_settings(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    os.seq.midi_send = capture_midi;
    os.midi_in_read = fake_midi_read;
    load_pad(0);
    os.pads[0].p.gate = 1; // Note On, so a held note sounds until released
    struct input in = blank();
    in.pad_hit[7] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.screen == OS_MIDI && os.midi.ext && os.midi.base == 1 && !os.midi.recv && os.midi.local && !os.midi.thru);
    CHECK(!os.midi.mixer_cc && os.midi.cc[MCC_LEVEL] == -1 && os.midi.cc[MCC_PAN] == -1); // all OFF at first
    { struct input idle = blank(); frame(&idle); }
    if (shot_dir) write_shot(shot_dir, "midi");

    // Pad Notes: A01 to 36; note 36 in plays it.
    tap(IN_B3);
    CHECK(os.screen == OS_PADNOTES && os.pad_note_sel == 0);
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(seq_pad_note(0) == 36 && seq_note_pad(36) == 0);
    if (shot_dir) write_shot(shot_dir, "pad_notes");
    midi_send_in(0x90, 36, 100);
    render(128);
    CHECK(voice_on(0) != 0);
    midi_send_in(0x80, 36, 0);
    for (int i = 0; i < 10; i++) render(128);
    CHECK(voice_on(0) == 0);
    tap(IN_B3); // <Template>: DEFAULT MPC first
    CHECK(os.padtpl_open && os.padtpl_kind == PADTPL_DEFAULT);
    tap(IN_B3); // <Apply>
    CHECK(!os.padtpl_open && seq_pad_note(0) == 35 && seq_pad_note(1) == 36);
    // CHROMATIC from C3: a semitone a pad, through the banks. SCALE: D minor pentatonic from C2.
    tap(IN_B3);
    struct input tp = blank();
    tp.encoder = 1;
    frame(&tp); // Template: CHROMATIC
    os.padtpl_oct = 3;
    tap(IN_B3);
    CHECK(seq_pad_note(0) == 48 && seq_pad_note(1) == 49 && seq_pad_note(16) == 64);
    tap(IN_B3);
    frame(&tp); // Template: SCALE
    os.padtpl_root = 2, os.padtpl_oct = 2;
    CHECK(padnotes_scales() >= 30);
    os.padtpl_scale = 10; // MIN PENTATONIC
    if (shot_dir) {
        struct input idle = blank();
        os.toast_until = 0;
        frame(&idle);
        write_shot(shot_dir, "pad_template");
    }
    tap(IN_B3);
    CHECK(seq_pad_note(0) == 38 && seq_pad_note(1) == 41 && seq_pad_note(4) == 48 && seq_pad_note(5) == 50);
    tap(IN_B3); // back to the default for what follows
    tp.encoder = -2;
    frame(&tp);
    tap(IN_B3);
    CHECK(seq_pad_note(0) == 35);
    tap(IN_B1);
    CHECK(os.screen == OS_MIDI);

    // Sustain NORMAL: a released note keeps sounding while the pedal is down.
    midi_send_in(0xb0, 64, 127);
    midi_send_in(0x90, 35, 100);
    midi_send_in(0x80, 35, 0);
    for (int i = 0; i < 10; i++) render(128);
    CHECK(voice_on(0) != 0);
    midi_send_in(0xb0, 64, 0);
    for (int i = 0; i < 10; i++) render(128);
    CHECK(voice_on(0) == 0);

    // The mixer under CC: off until turned on; BANK: channel 3 is the bank's third strip.
    midi_send_in(0xb2, 7, 64);
    CHECK(os.seq.sequence[0].track[2].level == 100);
    os.midi.mixer_cc = 1;
    midi_send_in(0xb2, 7, 64); // on, but no CC set for anything yet: still nothing
    CHECK(os.seq.sequence[0].track[2].level == 100);
    os.midi.cc[MCC_LEVEL] = 7, os.midi.cc[MCC_LOW] = 20; // Level on CC 7, Low on CC 20
    midi_send_in(0xb2, 7, 64);
    CHECK(os.seq.sequence[0].track[2].level == 50);
    midi_send_in(0xb2, 20, 127);
    CHECK(os.seq.sequence[0].track[2].eq.lg == 160);

    // CC Map: <Learn> takes the next CC; <Default> puts them back.
    tap(IN_B2);
    CHECK(os.screen == OS_CCMAP);
    os.cursor_ccmap = 0, os.midi.mixer_cc = 0; // on Mixer CC (OFF): <Learn> starts at the first control
    tap(IN_B2);
    CHECK(os.cc_learn == 1 && os.cursor_ccmap == 2);
    midi_send_in(0xb0, 30, 5);
    // Learned: Mixer CC on, so it answers at once, and the cursor on the next control.
    CHECK(os.midi.cc[MCC_LEVEL] == 30 && os.cc_learn == 0 && os.midi.mixer_cc == 1 && os.cursor_ccmap == 3);
    tap(IN_B2);
    CHECK(os.cc_learn == 2);
    midi_send_in(0xb0, 31, 5);
    CHECK(os.midi.cc[1] == 31 && os.cursor_ccmap == 4);
    { struct input idle = blank(); frame(&idle); }
    if (shot_dir) write_shot(shot_dir, "ccmap");
    tap(IN_B3); // <Default>: all OFF again
    CHECK(os.midi.cc[MCC_LEVEL] == -1);
    tap(IN_B1);

    // Sync SEND: start, clocks and stop go out with the sequence.
    os.midi.sync = SYNC_SEND;
    { struct input idle = blank(); frame(&idle); }
    rt_start = rt_clock = rt_stop = 0;
    seq_play(&os.seq, 1);
    render_until_tick(96);
    seq_stop(&os.seq, 1);
    CHECK(rt_start == 1 && rt_clock >= 24 && rt_clock <= 26 && rt_stop == 1);
    os.midi.sync = SYNC_RECEIVE; // RECEIVE: a start from outside plays it, from the Sync In port only
    fake_port = 1; // USB: not the port it follows
    midi_send_in(0xfa, 0, 0);
    CHECK(os.seq.mode == SEQ_STOPPED);
    fake_port = 0;
    midi_send_in(0xfa, 0, 0);
    CHECK(os.seq.mode == SEQ_PLAYING);
    midi_send_in(0xfc, 0, 0);
    CHECK(os.seq.mode == SEQ_STOPPED);
    os.midi.sync = SYNC_OFF;

    // Footswitch PLAY/STOP on Base Chn.
    os.midi.sustain = SUS_FOOT;
    midi_send_in(0xb0, 64, 127);
    CHECK(os.seq.mode == SEQ_PLAYING);
    midi_send_in(0xb0, 64, 0);
    midi_send_in(0xb0, 64, 127);
    CHECK(os.seq.mode == SEQ_STOPPED);
    midi_send_in(0xb0, 64, 0);

    // The footswitch's own channel (ANY to start), CC and polarity. AUTO: an inverted pedal (it
    // rests high) is found out from its first press and release, and works from the next press.
    struct ui fu0 = {.px = px, .track_led = -1};
    #define PEDAL(status, cc, v, t) (midi_qn = midi_qat = 0, midi_push(status, cc, v), fu0.now_ms = (t), os_frame(&fu0, &idle_in, &os))
    struct input idle_in = blank();
    os.foot_known = 0, os.foot_dur[0] = os.foot_dur[1] = 0, os.foot_inv = 0;
    CHECK(os.midi.foot_chn == 0 && os.midi.foot_cc == 64 && os.midi.foot_pol == FPOL_AUTO);
    PEDAL(0xb4, 64, 0, 10000); // channel 5, first heard "up": not acted on
    CHECK(os.seq.mode == SEQ_STOPPED);
    PEDAL(0xb4, 64, 127, 10200); // let go
    CHECK(os.seq.mode == SEQ_STOPPED);
    PEDAL(0xb4, 64, 0, 15000); // pressed: it rests high, so this is the press
    CHECK(os.seq.mode == SEQ_PLAYING && os.foot_inv);
    PEDAL(0xb4, 64, 127, 15150);
    PEDAL(0xb4, 64, 0, 19000);
    CHECK(os.seq.mode == SEQ_STOPPED);
    PEDAL(0xb4, 64, 127, 19100);
    // A set channel: another channel's pedal is not the footswitch.
    os.midi.foot_chn = 2;
    PEDAL(0xb4, 64, 0, 25000);
    CHECK(os.seq.mode == SEQ_STOPPED);
    os.midi.foot_chn = 0;
    // <Learn> on the footswitch's fields: one press sets channel, CC and polarity.
    os.screen = OS_MIDI;
    os.cursor_midi = 9; // Foot Chn (Sync In hidden)
    tap(IN_B2);
    CHECK(os.foot_learn);
    os.toast_until = 0;
    frame(&idle_in);
    if (shot_dir) write_shot(shot_dir, "midi_foot");
    PEDAL(0xb2, 66, 127, 30000); // a controller sending its pedal as CC 66 on channel 3
    CHECK(!os.foot_learn && os.midi.foot_chn == 3 && os.midi.foot_cc == 66 && os.midi.foot_pol == FPOL_NORMAL);
    CHECK(strncmp(os.toast, "Footswitch: Chn 3 CC 66", 23) == 0 && os.seq.mode == SEQ_STOPPED);
    os.foot_known = 0;
    PEDAL(0xb2, 66, 0, 30500);
    PEDAL(0xb2, 66, 127, 32000); // pressed: NORMAL now
    CHECK(os.seq.mode == SEQ_PLAYING);
    PEDAL(0xb2, 66, 0, 32100);
    PEDAL(0xb0, 64, 127, 33000); // the old CC is not the footswitch any more
    CHECK(os.seq.mode == SEQ_PLAYING);
    PEDAL(0xb2, 66, 127, 34000);
    CHECK(os.seq.mode == SEQ_STOPPED);
    PEDAL(0xb2, 66, 0, 34100);
    CHECK(os.foot_seen && os.foot_last_ch == 3 && os.foot_last_cc == 66 && os.foot_last_val == 0);
    #undef PEDAL
    os.midi.foot_chn = 0, os.midi.foot_cc = 64, os.midi.foot_pol = FPOL_AUTO, os.foot_known = 0;
    os.foot_dur[0] = os.foot_dur[1] = 0, os.foot_inv = 0;
    os.screen = OS_MAIN;

    // Footswitch AUDIO REC: <Rec> and <Stop> on Audiotrack Record, and Live Looper takes.
    os.midi.foot = FOOT_AUDIO;
    atomic_store(&os.rec.present, 1);
    int screen_was = os.screen;
    os.screen = OS_MAIN;
    midi_send_in(0xb0, 64, 127); // not on either page: nothing starts
    midi_send_in(0xb0, 64, 0);
    CHECK(!os.at_running && os.lp_state == LP_IDLE && os.seq.mode == SEQ_STOPPED);
    os.screen = OS_RECORD, os.record_page = 1;
    midi_send_in(0xb0, 64, 127);
    midi_send_in(0xb0, 64, 0);
    CHECK(os.at_running && os.seq.mode == SEQ_COUNT_IN && atomic_load(&os.rec.state) == REC_ARMED);
    midi_send_in(0xb0, 64, 127); // <Stop>
    midi_send_in(0xb0, 64, 0);
    CHECK(!os.at_running && os.seq.mode == SEQ_STOPPED);
    static float quiet[256];
    recorder_input(&os.rec, quiet, 128, 0); // the take stops on the next block
    recorder_discard(&os.rec);
    os.record_page = 5; // Live Looper: a take on the selected empty pad
    int sel_was = os.selected;
    os.selected = 120;
    os.seq.sequence[os.seq.current].loop_bar = 1;
    midi_send_in(0xb0, 64, 127);
    midi_send_in(0xb0, 64, 0);
    CHECK(os.lp_state == LP_COUNT && os.seq.mode == SEQ_COUNT_IN);
    midi_send_in(0xb0, 64, 127); // and again: let go
    midi_send_in(0xb0, 64, 0);
    CHECK(os.lp_state == LP_ABORT);
    recorder_input(&os.rec, quiet, 128, 0);
    seq_stop(&os.seq, 1);
    struct ui fu = {.px = px, .now_ms = 1000, .track_led = -1};
    looper_service(&os, &fu);
    CHECK(os.lp_state == LP_IDLE);
    os.lp_session = 0, os.selected = sel_was, os.screen = screen_was, os.record_page = 0;
    atomic_store(&os.rec.present, 0);
    os.midi.foot = FOOT_PLAY;
    os.midi.sustain = SUS_NORMAL;

    // Local Mode off: a pad goes out as MIDI on Base Chn and makes no sound here.
    os.midi.local = 0;
    engine_kill_all(&e);
    for (int i = 0; i < 4; i++) render(128);
    out_n = 0;
    in = blank();
    in.pad_hit[0] = 100, in.pad_down[0] = 1;
    frame(&in);
    render(128);
    CHECK(out_n >= 1 && out_log[0][0] == 0x90 && out_log[0][1] == 35 && voice_on(0) == 0);
    in = blank();
    in.pad_release[0] = 1;
    frame(&in);
    os.midi.local = 1;

    // Soft Thru: a note in goes out again; Ext MIDI off: nothing in or out.
    os.midi.thru = 1;
    out_n = 0;
    midi_send_in(0x95, 40, 90);
    CHECK(out_n >= 1 && out_log[0][0] == 0x90 && out_log[0][1] == 40);
    midi_send_in(0x85, 40, 0);
    os.midi.thru = 0;
    os.midi.ext = 0;
    engine_kill_all(&e);
    for (int i = 0; i < 4; i++) render(128);
    midi_send_in(0x90, 35, 100);
    render(128);
    CHECK(voice_on(0) == 0);
    os.midi.ext = 1;
    os.midi.usb = 0; // USB MIDI off: what comes in on it is dropped
    fake_port = 1;
    midi_send_in(0x90, 35, 100);
    render(128);
    CHECK(voice_on(0) == 0);
    fake_port = 0;
    os.midi.usb = 1;

    // Saved to the card and read back.
    char path[] = "/tmp/claude-mpc-settings-XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    os.settings_path = path;
    os.midi.base = 9, os.midi.cc[MCC_PAN] = 77, os.midi.sync_in = 1;
    seq_set_pad_note(5, 60);
    CHECK(os_save_settings(&os) == 0);
    os.midi.base = 1, os.midi.cc[MCC_PAN] = 10, os.midi.sync_in = 0;
    seq_default_pad_notes();
    os_load_settings(&os);
    CHECK(os.midi.base == 9 && os.midi.cc[MCC_PAN] == 77 && os.midi.sync_in == 1 && seq_pad_note(5) == 60);
    // Master Effects are not settings: they go with a project or Autoload, so a blank start has them off.
    atomic_store(&os.master.eq_on, 1), atomic_store(&os.master.comp, 1);
    CHECK(os_save_settings(&os) == 0);
    master_defaults(&os.master);
    os_load_settings(&os);
    CHECK(atomic_load(&os.master.eq_on) == 0 && atomic_load(&os.master.comp) == MCOMP_OFF);
    unlink(path);
    os.settings_path = 0;
    seq_default_pad_notes();
    os.midi_in_read = 0;
    os.seq.midi_send = 0;
    os.screen = OS_MAIN;
}

// A tiny stand-in for an Open-Unmix model, laid out the same way: every weight zero but the
// output mean, so its mask is 1 everywhere and EXTRACT returns the mix, REMOVE silence.
static void fake_umx(const char *path) {
    FILE *f = fopen(path, "wb");
    int dims[3] = {16, 8, 2049}, B = 16, H = 8, O = 2049, L = 4;
    fwrite("UMX1", 1, 4, f);
    fwrite(dims, sizeof dims, 1, f);
    long zeros = 2L * B + (long)H * 2 * B + 2 * H + 3L * 2 * (4L * L * H + 4L * L * L + 4 * L) + (long)H * 2 * H + 2 * H +
                 2L * O * H + 2 * 2L * O + O; // all up to the output mean
    float z = 0, one = 1;
    for (long i = 0; i < zeros; i++) fwrite(&z, 4, 1, f);
    for (int i = 0; i < O; i++) fwrite(&one, 4, 1, f);
    fclose(f);
}

// Shift + SAMPLE REC, twice: Stems. It separates the pad's zone in the background (at most
// 20 s), then the stem waits for a pad or ENTER like a take.
static void test_stems(const char *shot_dir) {
    seq_fresh();
    char dir[] = "/tmp/claude-mpc-stems-XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    char path[128];
    static const char *const files[] = {"drums", "bass", "vocals"};
    for (int i = 0; i < 3; i++) snprintf(path, sizeof path, "%s/umxhq-%s.bin", dir, files[i]), fake_umx(path);
    os.stems_dir = dir;
    struct sample *src = sample_new(44100, 2, 44100);
    for (int i = 0; i < 44100 * 2; i++) src->data[i] = 0.5f * (float)sin(i * 0.01);
    os_pad_load(&os, 0, src, "Loop");
    os.selected = 0;
    struct input in = blank();
    for (int i = 0; i < 4; i++) tap(IN_SAMPLE_REC); // Sample New Sound, Audiotrack, Convert, Stems
    CHECK(os.screen == OS_RECORD && os.record_page == 3 && os.cursor_record == 0);
    struct input idle = blank();
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "stems");
    tap(IN_B3); // <Do It>: DRUMS, EXTRACT
    for (int i = 0; i < 400 && os.stem_job; i++) {
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, 0);
        frame(&idle);
    }
    CHECK(!os.stem_job && os.rec_result && os.rec_result->frames == 44100 && os.rec_result->channels == 2);
    float err = 0;
    for (int i = 0; os.rec_result && i < 44100 * 2; i++) err = fmaxf(err, fabsf(os.rec_result->data[i] - src->data[i]));
    CHECK(err < 1e-4f); // a mask of 1: the mix back exactly
    CHECK(strcmp(os.rec_result_name, "Loop-Drums") == 0);
    in = blank();
    in.pad_hit[2] = 100, in.pad_down[2] = 1;
    frame(&in);
    in = blank();
    in.pad_release[2] = 1;
    frame(&in);
    CHECK(!os.rec_result && os.pads[2].s && strcmp(os.pads[2].name, "Loop-Drums") == 0);

    os.selected = 0; // (assigning selected the new pad)
    os.stem_kind = STEM_MUSIC; // Music: the vocals model, the vocals taken out: here nothing is left
    tap(IN_B3);
    for (int i = 0; i < 400 && os.stem_job; i++) {
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, 0);
        frame(&idle);
    }
    err = 0;
    for (int i = 0; os.rec_result && i < 44100 * 2; i++) err = fmaxf(err, fabsf(os.rec_result->data[i]));
    CHECK(os.rec_result && err < 1e-4f && strcmp(os.rec_result_name, "Loop-Music") == 0);
    tap(IN_B1); // <Cancel>

    os.pads[0].p.end = 44100; // a longer sound: over 20 s, the limit and a tip
    struct sample *big = sample_new(44100 * 21, 1, 44100);
    memset(big->data, 0, sizeof(float) * 44100 * 21);
    os_pad_load(&os, 1, big, "Long");
    os.selected = 1;
    tap(IN_B3);
    CHECK(os.stem_too_long && !os.stem_job);
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "stems_long");
    tap(IN_B3); // <OK>
    CHECK(!os.stem_too_long);
    for (int i = 0; i < 3; i++) snprintf(path, sizeof path, "%s/umxhq-%s.bin", dir, files[i]), unlink(path);
    rmdir(dir);
    os.stems_dir = 0;
    os.screen = OS_MAIN;
}

// Input: LINE or MIC on the record pages, never both; LINE to start with.
static int input_set = -1;
static int fake_set_input(int src) {
    input_set = src;
    return src == INPUT_USB ? -1 : 0; // no USB audio here
}

static void test_input_select(void) {
    seq_fresh();
    os.set_input = fake_set_input;
    CHECK(os.input_src == 0);
    tap(IN_SAMPLE_REC);
    CHECK(os.screen == OS_RECORD && os.cursor_record == 0); // Input: first
    struct input in = blank();
    in.encoder = 2; // LINE, INST L, MIC
    frame(&in);
    CHECK(os.input_src == INPUT_MIC && input_set == INPUT_MIC);
    tap(IN_SAMPLE_REC); // Audiotrack Record: the same input
    { struct input idle = blank(); frame(&idle); }
    CHECK(os.record_page == 1 && os.input_src == INPUT_MIC);
    in.encoder = 1; // USB: chosen, though not there, and said so
    frame(&in);
    CHECK(os.input_src == INPUT_USB && input_set == INPUT_USB && os.usb_missing == 1);
    { struct input idle = blank(); frame(&idle); } // said, once
    CHECK(os.usb_missing == 0);
    in.encoder = -2; // back over MIC to INST L: the line jack, through the instrument stage
    frame(&in);
    CHECK(os.input_src == INPUT_INST && input_set == INPUT_INST);
    in.encoder = -1;
    frame(&in);
    CHECK(os.input_src == 0 && input_set == 0);
    os.set_input = 0;
    os.screen = OS_MAIN;
}

// Shift + Pad 11: Resample. The main output, 16 bit 44.1 to start with, from the threshold (or
// MANUAL: at once); LIVE, or the whole sequence, trimmed to its exact length.
static void test_resample(const char *shot_dir) {
    seq_fresh();
    os.seq.metronome = METRO_OFF;
    os.seq.sequence[0].bars = 1;
    struct input in = blank();
    in.pad_hit[10] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.screen == OS_RECORD && os.record_page == 4 && os.cursor_record == 0);
    const struct rec_settings *r = &os.rec_set[4];
    CHECK(r->bits == 12 && r->rate == 29760 && !r->start_force && !r->what && !r->thru_record);
    os.rec_set[4].bits = 16, os.rec_set[4].rate = 44100; // the main output as it is, below
    os.rec_set[4].start_force = 1;                        // MANUAL, for the take below
    CHECK(atomic_load(&os.rec.source) == 1); // its meter: the main output
    struct input idle = blank();
    frame(&idle);
    if (shot_dir) write_shot(shot_dir, "resample");

    tap(IN_B3); // <Rec>, LIVE, MANUAL: no input needed, it takes the main output at once
    CHECK(atomic_load(&os.rec.state) == REC_ARMED && atomic_load(&os.rec.source) == 1);
    static float buf[256];
    for (int i = 0; i < 256; i++) buf[i] = 0.25f;
    for (int b = 0; b < 100; b++) recorder_input(&os.rec, buf, 128, atomic_load(&e.clock)), render(128);
    tap(IN_B3); // <Stop>
    recorder_input(&os.rec, buf, 128, atomic_load(&e.clock));
    frame(&idle);
    CHECK(os.rec_result && os.rec_result->rate == 44100 && strcmp(os.rec_result_name, "Resample01") == 0);
    tap(IN_B1); // <Cancel>

    os.cursor_record = 0; // Record: SEQUENCE; Start and Thres go
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(r->what == 1);
    in = blank(); // <Rec>, without rendering: the audio thread feeds the recorder before each block
    in.down = in.held = 1u << IN_B3;
    struct ui u0 = {.px = px, .now_ms = 1000, .track_led = -1};
    os_frame(&u0, &in, &os);
    CHECK(os.at_running && os.seq.mode == SEQ_COUNT_IN); // a count-in too short to hear
    for (int b = 0; b < 2000 && os.at_running; b++) {
        recorder_input(&os.rec, buf, 128, atomic_load(&e.clock));
        render(128);
        if (b % 16 == 0) frame(&idle);
    }
    for (int i = 0; i < 4; i++) recorder_input(&os.rec, buf, 128, atomic_load(&e.clock)), render(128), frame(&idle);
    CHECK(os.seq.mode == SEQ_STOPPED && os.rec_result && os.rec_result->frames == 2 * 44100); // one bar at 120, exactly
    tap(IN_B1);
    os.screen = OS_MAIN;
}

// ---- Effects ----

// Two seconds of a guitar-like test: a plucked, decaying chord with an impulse in front.
static void fx_signal(float *b, int frames) {
    for (int i = 0; i < frames; i++) {
        float tt = (float)(i % 44100) / 44100.0f, env = expf(-3 * tt);
        float x = env * 0.3f * (sinf(2 * 3.14159f * 110 * tt) + 0.5f * sinf(2 * 3.14159f * 165 * tt) + 0.3f * sinf(2 * 3.14159f * 440 * tt));
        if (i % 44100 == 0) x += 0.8f;
        b[i * 2] = x, b[i * 2 + 1] = x * 0.9f;
    }
}

static float rms(const float *b, int from, int to) {
    double acc = 0;
    for (int i = from; i < to; i++) acc += (double)b[i * 2] * b[i * 2] + (double)b[i * 2 + 1] * b[i * 2 + 1];
    return to > from ? (float)sqrt(acc / (2.0 * (to - from))) : 0;
}

static void test_fx_all(void) {
    enum { N = 88200 };
    static float b[N * 2];
    struct fx *f = fx_new();
    CHECK(f != 0);
    for (int type = 1; type < FX_COUNT; type++) {
        for (int setting = 0; setting < 3; setting++) { // defaults, all down, all up
            fx_set(f, type);
            if (setting) for (int k = 0; k < 3; k++) f->p[k] = setting == 1 ? 0 : 127;
            fx_signal(b, N);
            for (int i = 0; i < N; i += 128) fx_process(f, b + i * 2, 128);
            int finite = 1;
            float peak = 0;
            for (int i = 0; i < N * 2; i++) {
                if (!isfinite(b[i])) finite = 0;
                if (fabsf(b[i]) > peak) peak = fabsf(b[i]);
            }
            CHECK(finite);
            CHECK(peak <= 4.0f);
            if (!finite || peak > 4.0f) printf("  fx %s setting %d: peak %g\n", fx_name(type), setting, peak);
            if (setting == 0) { // at its defaults every effect passes sound, and not wildly louder
                float r = rms(b, 0, N);
                CHECK(r > 0.02f && r < 1.0f);
                if (!(r > 0.02f && r < 1.0f)) printf("  fx %s rms %g\n", fx_name(type), r);
            }
        }
        // Names, controls and categories.
        CHECK(fx_name(type)[0] && fx_short(type)[0] && fx_param_name(type, 0)[0] && fx_param_name(type, 2)[0]);
        int c = fx_cat_of(type), seen = 0; // every effect is in its category's list exactly once
        for (int t2 = fx_cat_first(c), guard = 0; guard < FX_COUNT; guard++) {
            seen += t2 == type;
            int next = fx_step(t2, 1, c);
            if (next == t2) break;
            t2 = next;
        }
        CHECK(seen == 1);
    }
    // Print Effects' order: Reverb, then EQ, then Delay.
    CHECK(FXC_REVERB == 0 && FXC_EQ == 1 && FXC_DELAY == 2 && strcmp(fx_cat_name(FXC_EQ), "EQ") == 0);
    CHECK(fx_cat_of(FX_ASR_HALL) == FXC_REVERB && fx_cat_of(FX_DUB) == FXC_DELAY && fx_cat_of(FX_EQ_KICK) == FXC_EQ &&
          fx_cat_of(FX_EQ3) == FXC_EQ && fx_cat_of(FX_SPEAKER) == FXC_AMP && fx_cat_of(FX_WAH) == FXC_MOD);
    CHECK(fx_cat_first(FXC_EQ) == FX_EQ3 && fx_step(FX_GATED, 1, -1) == FX_EQ3 && fx_step(FX_ASR_HALL, -1, -1) == FX_OFF);
    int all = 0; // the whole list, OFF then every effect once
    for (int t2 = FX_OFF, guard = 0; guard < 100; guard++) {
        int next = fx_step(t2, 1, -1);
        if (next == t2) break;
        t2 = next, all++;
    }
    CHECK(all == FX_COUNT - 1);
    char v[16];
    fx_param_text(FX_JUNO, 0, 127, 120, v, sizeof v);
    CHECK(strcmp(v, "I+II") == 0);
    fx_param_text(FX_DDL, 0, 103, 120, v, sizeof v);
    CHECK(strcmp(v, "1/8D") == 0);

    // Every reverb, Decay all the way up, dies away: silent by 10 seconds after a short hit.
    {
        enum { L = 44100 * 10 };
        float *r = calloc(L * 2, sizeof(float));
        for (int type = FX_ASR_HALL; type <= FX_GATED; type++) {
            fx_set(f, type);
            f->p[0] = 127;
            memset(r, 0, sizeof(float) * L * 2);
            for (int i = 0; i < 2205; i++) r[i * 2] = r[i * 2 + 1] = 0.8f * sinf(i * 0.2f);
            for (int i = 0; i < L; i += 128) fx_process(f, r + i * 2, 128);
            float tail = rms(r, L - 22050, L);
            CHECK(tail < 0.0001f); // below -80 dB
            if (tail >= 0.0001f) printf("  %s still ringing: %g\n", fx_name(type), tail);
            CHECK(fx_tail_seconds(f) <= 8);
        }
        free(r);
    }

    // DDL: an impulse comes back after the set time, a quarter note at 120 BPM is 22050 frames.
    fx_set(f, FX_DDL);
    f->p[0] = 110, f->p[1] = 0, f->p[2] = 127, f->bpm = 120; // 1/4
    fx_param_text(FX_DDL, 0, 110, 120, v, sizeof v);
    CHECK(strcmp(v, "1/4") == 0);
    memset(b, 0, sizeof b);
    b[0] = b[1] = 1;
    for (int i = 0; i < N; i += 128) fx_process(f, b + i * 2, 128);
    int at = 0;
    for (int i = 1000; i < N; i++)
        if (fabsf(b[i * 2]) > fabsf(b[at * 2]) || at == 0) at = i;
    CHECK(abs(at - 22050) <= 2);

    // A compressor narrows the gap between a loud and a quiet part.
    fx_set(f, FX_DYNACOMP);
    for (int i = 0; i < N; i++) {
        float a = i < N / 2 ? 0.8f : 0.08f, x = a * sinf(2 * 3.14159f * 220 * i / 44100.0f);
        b[i * 2] = b[i * 2 + 1] = x;
    }
    for (int i = 0; i < N; i += 128) fx_process(f, b + i * 2, 128);
    float loud = rms(b, N / 4, N / 2), quiet = rms(b, N * 3 / 4, N);
    CHECK(loud / quiet < 5); // 20 dB in, much less out

    // The noise gate shuts out a quiet hiss after the playing stops.
    fx_set(f, FX_GATE);
    for (int i = 0; i < N; i++) {
        float x = i < N / 2 ? 0.5f * sinf(2 * 3.14159f * 220 * i / 44100.0f) : 0.001f * ((i * 7919 % 200) / 100.0f - 1);
        b[i * 2] = b[i * 2 + 1] = x;
    }
    for (int i = 0; i < N; i += 128) fx_process(f, b + i * 2, 128);
    CHECK(rms(b, N * 3 / 4, N) < 0.0001f);
    fx_free(f);

    // The whole chain, seven reverbs and delays at once: how long a block takes here.
    struct fx_chain c;
    CHECK(fx_chain_init(&c) == 0);
    atomic_store(&c.load, 0.0f);
    static const int heavy[FX_SLOTS] = {FX_PLATE, FX_ASR_HALL, FX_DP4, FX_AC30, FX_TAPE, FX_UNIVIBE, FX_SPRING};
    for (int i = 0; i < FX_SLOTS; i++) {
        atomic_store(&c.type[i], heavy[i]);
        for (int k = 0; k < 3; k++) atomic_store(&c.p[i][k], fx_param_default(heavy[i], k));
    }
    fx_signal(b, N);
    for (int i = 0; i < N; i += 128) fx_chain_process(&c, b + i * 2, 128);
    int finite = 1;
    for (int i = 0; i < N * 2; i++) finite &= isfinite(b[i]) != 0;
    CHECK(finite);
    printf("  input chain, 7 heavy effects: %.0f%% of a block on this computer\n", atomic_load(&c.load) * 100);
    for (int i = 0; i < FX_SLOTS; i++) fx_free(c.slot[i]);
}

static void test_print_fx(const char *shot_dir) {
    seq_fresh();
    struct sample *snd = sample_new(4410, 1, 22050); // a fifth of a second at 22 kHz
    for (int i = 0; i < 4410; i++) snd->data[i] = i < 400 ? 0.8f * sinf(i * 0.3f) : 0;
    os_pad_load(&os, 3, snd, "Snare");
    os.pads[3].p.start = 100;
    render(16);
    tap(IN_PAD_FX);
    CHECK(os.screen == OS_PRINT && os.cursor_print == 0);
    CHECK(os.print_type == FX_ASR_HALL); // Category REVERB, its first effect
    struct input in = blank();
    in.pad_hit[3] = 100;
    frame(&in);
    CHECK(os.selected == 3);
    in = blank();
    in.encoder = 2; // Category: DELAY, after EQ
    frame(&in);
    CHECK(os.print_type == FX_DUB);
    in.encoder = -2; // back to REVERB
    frame(&in);
    CHECK(os.print_type == FX_ASR_HALL);
    tap(IN_PAD_FX); // pressed again: the first field
    in = blank();
    in.encoder_press = 1; // Effect
    frame(&in);
    in = blank();
    in.encoder = 20; // stays inside REVERB
    frame(&in);
    CHECK(os.print_type == FX_GATED);
    in.encoder = -2;
    frame(&in);
    CHECK(os.print_type == FX_SPRING);
    in = blank();
    in.knob_moved[2] = 1, in.knob[2] = fx_param_default(FX_SPRING, 2); // K3 catches Mix where it is
    frame(&in);
    in.knob[2] = 100;
    frame(&in);
    CHECK(os.print_p[2] == 100 && os.cursor_print == 4);
    if (shot_dir) write_shot(shot_dir, "print_fx");

    tap(IN_B2); // <Play>
    CHECK(os.print_preview && voices() > 0);
    tap(IN_B3); // <Print>
    struct sample *n = os.pads[3].s;
    CHECK(n != snd && n->rate == 44100 && n->channels == 2);
    long body = (4410 - 100) * 2; // the trim from Start, at 44.1 kHz
    CHECK(n->frames > body && os.pads[3].p.start == 0 && os.pads[3].p.end == n->frames);
    CHECK(strcmp(os.pads[3].name, "Snare-Spring") == 0);
    int kept = 0;
    for (int i = 0; i < os.nmemory; i++) kept |= os.memory[i].s == snd;
    CHECK(kept); // the original stays in memory
    in = blank();
    in.down = in.held = (1u << IN_MINUS) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in); // Undo
    CHECK(os.pads[3].s == snd);
    render(64);
}

static void test_input_fx(const char *shot_dir) {
    seq_fresh();
    CHECK(os.rec.fx == &os.fx && !atomic_load(&os.fx.on) && atomic_load(&os.fx.input) == FXIN_MONO_L);
    tap(IN_KNOB_FX);
    CHECK(os.screen == OS_INFX && os.cursor_infx == 7); // Effect #
    CHECK(os.infx_kind == 0 && strcmp(infx_preset_name(os.infx_preset), "EMPTY") == 0);
    if (shot_dir) write_shot(shot_dir, "input_fx_empty");

    // Every preset keeps its kind's places: guitar and bass in nine, keyboard in six.
    int n = infx_presets(), kinds[5] = {0};
    static const int gtr_role[FX_SLOTS][6] = { // what each place may hold (categories, or a type)
        {FX_GATE}, {FX_DYNACOMP, FX_CS3}, {FX_WAH, FX_AUTOWAH}, {FX_KLON, FX_OD1, FX_DS1, FX_FUZZ},
        {FX_BASSMAN, FX_AC30, FX_JTM45, FX_DC30, FX_JC, FX_B15},
        {FX_EQ3, FX_FENDER_EQ, FX_PARAM_EQ}, {100 + FXC_MOD}, {100 + FXC_REVERB, 100 + FXC_DELAY}, {FX_SPEAKER}};
    static const int key_role[6][6] = {{FX_EQ3, FX_PARAM_EQ}, {FX_DYNACOMP, FX_CS3}, {FX_WAH, FX_AUTOWAH},
                                       {FX_KLON, FX_OD1, FX_DS1}, {100 + FXC_MOD}, {100 + FXC_REVERB, 100 + FXC_DELAY}};
    for (int i = 0; i < n; i++) {
        int kind = infx_preset_kind(i);
        kinds[kind]++;
        infx_load_preset(&os, i);
        int reverbs = 0, delays = 0, any = 0;
        for (int k = 0; k < FX_SLOTS; k++) {
            int ty = atomic_load(&os.fx.type[k]), c = fx_cat_of(ty);
            if (ty == FX_OFF) continue;
            any = 1;
            reverbs += c == FXC_REVERB, delays += c == FXC_DELAY;
            int ok = kind >= 3; // OTHER and MIX: no fixed places
            const int *role = kind <= 1 ? gtr_role[k] : k < 6 ? key_role[k] : 0;
            for (int r = 0; role && r < 6; r++)
                if ((role[r] < 100 && role[r] == ty && ty) || (role[r] >= 100 && c == role[r] - 100))
                    ok = 1;
            CHECK(ok);
            if (!ok) printf("  preset %s: %s in place %d\n", infx_preset_name(i), fx_name(ty), k + 1);
        }
        CHECK(kind == 4 || (reverbs <= 1 && delays <= 1)); // MIX: sends, side by side
        CHECK(any == (strcmp(infx_preset_name(i), "EMPTY") != 0 && strcmp(infx_preset_name(i), "USER TEMPLATE") != 0));
        CHECK(!any || atomic_load(&os.fx.on)); // a preset with effects turns Input Effect on
    }
    CHECK(kinds[0] >= 12 && kinds[1] >= 3 && kinds[2] >= 8 && kinds[3] >= 2);
    int jazz = 0;
    for (int i = 0; i < n; i++) jazz += strstr(infx_preset_name(i), "JAZZ") != 0;
    CHECK(jazz >= 6);

    // Type, then Preset within it.
    infx_load_preset(&os, 0);
    atomic_store(&os.fx.on, 0);
    os.cursor_infx = 5; // Category
    struct input in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.infx_kind == 1 && strcmp(infx_preset_name(os.infx_preset), "EMPTY") == 0);
    in.encoder = -1;
    frame(&in);
    os.cursor_infx = 6; // Preset: guitar's first after EMPTY
    in.encoder = 1;
    frame(&in);
    CHECK(strcmp(infx_preset_name(os.infx_preset), "SPARKLE CLEAN") == 0 && !os.infx_edited && atomic_load(&os.fx.on));
    in.encoder = 100; // the last guitar one: added at the end of the list, still reached in order
    frame(&in);
    CHECK(infx_preset_kind(os.infx_preset) == 0 && strcmp(infx_preset_name(os.infx_preset), "DC30 DRIVE") == 0);
    in.encoder = -10; // back over the nine added before it, to the last of the first run
    frame(&in);
    CHECK(strcmp(infx_preset_name(os.infx_preset), "JAZZ BALLAD") == 0);
    in.encoder = -50; // stays within guitar
    frame(&in);
    CHECK(infx_preset_kind(os.infx_preset) == 0 && strcmp(infx_preset_name(os.infx_preset), "EMPTY") == 0);
    in.encoder = 2; // CLEAN CHORUS
    frame(&in);
    CHECK(atomic_load(&os.fx.type[1]) == FX_CS3 && atomic_load(&os.fx.type[4]) == FX_BASSMAN &&
          atomic_load(&os.fx.type[6]) == FX_JUNO && atomic_load(&os.fx.type[7]) == FX_PLATE &&
          atomic_load(&os.fx.type[8]) == FX_SPEAKER && atomic_load(&os.fx.type[3]) == FX_OFF);

    // <Select> moves through the effects; Effect # sets one, past a second reverb.
    os.infx_slot = 2; // place 3: the wah's, empty
    os.cursor_infx = 7;
    in = blank();
    in.encoder = 1; // from OFF: the reverbs are passed (the Plate is the one), the 3-BAND EQ
    frame(&in);
    CHECK(atomic_load(&os.fx.type[2]) == FX_EQ3 && os.infx_edited);
    tap(IN_B2); // <Clear>
    CHECK(atomic_load(&os.fx.type[2]) == FX_OFF);
    tap(IN_B1); // <Select>: place 4
    CHECK(os.infx_slot == 3);
    tap(IN_B3); // <Bypass>
    CHECK(!atomic_load(&os.fx.on));
    tap(IN_B3);
    CHECK(atomic_load(&os.fx.on));
    tap(IN_KNOB_FX); // pressed again: Effect #
    CHECK(os.cursor_infx == 7);

    // The wah: K1, the slider with the cursor on Pedal, and the mod wheel from anywhere.
    os.infx_slot = 2;
    in = blank();
    for (int i = 0; i < 40 && atomic_load(&os.fx.type[2]) != FX_WAH; i++) in.encoder = 1, frame(&in);
    CHECK(atomic_load(&os.fx.type[2]) == FX_WAH);
    os.cursor_infx = 8;
    in = blank();
    in.fader_moved = 1, in.fader = 100;
    frame(&in);
    CHECK(atomic_load(&os.fx.p[2][0]) == 100);
    os.cursor_infx = 9; // elsewhere, the slider leaves it
    in.fader = 20;
    frame(&in);
    CHECK(atomic_load(&os.fx.p[2][0]) == 100);
    os.cursor_infx = 8;
    if (shot_dir) write_shot(shot_dir, "input_fx");
    infx_wah_pedal(&os, 30); // as the mod wheel does
    CHECK(atomic_load(&os.fx.p[2][0]) == 30);

    // Input: MONO L, MONO R, STEREO, INST L, MIC, USB (the hardware's input, as the record pages
    // set it). INST L shows its Gain and Bright, and turns the instrument stage on.
    os.cursor_infx = 2;
    in = blank();
    in.encoder = 3;
    frame(&in);
    CHECK(os.input_src == INPUT_INST && atomic_load(&os.fx.input) == FXIN_INST && atomic_load(&os.fx.inst) == 1 &&
          atomic_load(&os.fx.inst_gain) == 12);
    os.cursor_infx = 3; // Gain
    in.encoder = 6;
    frame(&in);
    CHECK(os.inst_gain == 18);
    if (shot_dir) {
        os.toast_until = 0;
        struct input idle = blank();
        frame(&idle);
        write_shot(shot_dir, "input_fx_inst");
    }
    os.cursor_infx = 2;
    in.encoder = 1;
    frame(&in);
    CHECK(os.input_src == 1 && atomic_load(&os.fx.input) == FXIN_MIC && atomic_load(&os.fx.inst) == 0);
    in.encoder = -2;
    frame(&in);
    CHECK(os.input_src == 0 && atomic_load(&os.fx.input) == FXIN_STEREO && os.infx_line == FXIN_STEREO);
    os.input_src = 1; // LINE/MIC changed on a record page
    in = blank();
    frame(&in);
    CHECK(atomic_load(&os.fx.input) == FXIN_MIC);
    os.input_src = INPUT_USB; // USB chosen on a record page: Input Effects' Input is USB
    in = blank();
    frame(&in);
    CHECK(atomic_load(&os.fx.input) == FXIN_USB);
    os.input_src = 0;
    in = blank();
    frame(&in);
    CHECK(atomic_load(&os.fx.input) == FXIN_STEREO);

    // MONO L: the left side to both before the chain.
    atomic_store(&os.fx.input, FXIN_MONO_L);
    for (int i = 0; i < FX_SLOTS; i++) atomic_store(&os.fx.type[i], FX_OFF);
    static float buf[256];
    for (int i = 0; i < 128; i++) buf[i * 2] = 0.3f, buf[i * 2 + 1] = -0.1f;
    fx_chain_process(&os.fx, buf, 128);
    CHECK(buf[255] == 0.3f);
    infx_load_preset(&os, 2);

    // Kept on the card.
    char path[] = "/tmp/claude-mpc-settings-XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    os.settings_path = path;
    os.infx_line = FXIN_MONO_R;
    atomic_store(&os.fx.p[7][0], 99);
    CHECK(os_save_settings(&os) == 0);
    infx_load_preset(&os, 0);
    os.infx_line = FXIN_MONO_L;
    os_load_settings(&os);
    CHECK(os.infx_preset == 2 && atomic_load(&os.fx.on) && os.infx_line == FXIN_MONO_R &&
          atomic_load(&os.fx.input) == FXIN_MONO_R && atomic_load(&os.fx.type[7]) == FX_PLATE &&
          atomic_load(&os.fx.p[7][0]) == 99 && atomic_load(&os.fx.type[8]) == FX_SPEAKER);
    unlink(path);
    os.settings_path = 0;
    infx_load_preset(&os, 0);
    atomic_store(&os.fx.on, 0);
}

static void test_eq_category(const char *shot_dir) {
    seq_fresh();
    tap(IN_PAD_FX);
    struct input in = blank();
    in.encoder = 1; // Category: EQ, right after REVERB
    frame(&in);
    CHECK(os.print_type == FX_EQ3);
    in.encoder = 1; // then DELAY
    frame(&in);
    CHECK(os.print_type == FX_DUB);
    in.encoder = -1;
    frame(&in);
    os.cursor_print = 1; // Effect: through the EQs, staying in the category
    in.encoder = 2;
    frame(&in);
    CHECK(os.print_type == FX_EQ_KICK && os.print_p[0] > 64 && os.print_p[1] < 64); // its curve: Sub up, Box down
    in.encoder = 50;
    frame(&in);
    CHECK(os.print_type == FX_EQ_808);
    if (shot_dir) write_shot(shot_dir, "print_eq");

    // KICK EQ's Sub lifts the lows and its Box takes out the boxy mids.
    struct fx *f = fx_new();
    enum { N = 44100 };
    static float lo[N * 2], mid[N * 2];
    for (int i = 0; i < N; i++) lo[i * 2] = lo[i * 2 + 1] = 0.1f * sinf(2 * 3.14159f * 50 * i / 44100.0f),
                                mid[i * 2] = mid[i * 2 + 1] = 0.1f * sinf(2 * 3.14159f * 400 * i / 44100.0f);
    fx_set(f, FX_EQ_KICK);
    for (int i = 0; i < N; i += 128) fx_process(f, lo + i * 2, 128);
    fx_set(f, FX_EQ_KICK);
    for (int i = 0; i < N; i += 128) fx_process(f, mid + i * 2, 128);
    float before = 0.1f / sqrtf(2);
    CHECK(rms(lo, N / 2, N) > before * 1.3f && rms(mid, N / 2, N) < before * 0.65f);
    if (!(rms(lo, N / 2, N) > before * 1.3f)) printf("  kick sub %g mid %g of %g\n", rms(lo, N / 2, N), rms(mid, N / 2, N), before);
    char v[16];
    fx_param_text(FX_EQ_HAT, 0, fx_param_default(FX_EQ_HAT, 0), 120, v, sizeof v);
    CHECK(strcmp(v, "303Hz") == 0);
    fx_param_text(FX_EQ_KICK, 1, fx_param_default(FX_EQ_KICK, 1), 120, v, sizeof v);
    CHECK(strcmp(v, "-5dB") == 0);
    fx_free(f);
}

static void test_s600_legato(void) {
    engine_init(&e);
    struct sample *s = constant(44100);
    struct pad_params p = params(44100);
    p.loop_on = 1;
    p.synth = 1, p.gate = 1, p.mono = 1, p.legato = 1;
    p.attack_ms = 0, p.decay_ms = 50, p.sustain = 50, p.release_ms = 20;
    p.filter = FILTER_LP2, p.cutoff = 100;
    engine_note_on_key(&e, 7, 127, 0, s, &p);
    for (int i = 0; i < 100; i++) render(128); // into the sustain
    const struct voice *v = voice_on(7);
    CHECK(v && fabsf(v->env - 0.5f) < 0.01f);
    engine_note_on_key(&e, 7, 127, 12, s, &p); // over the held note: the same voice, a new pitch
    render(128);
    CHECK(voices() == 1 && voice_on(7) == v && v->key == 12 && fabsf(v->env - 0.5f) < 0.01f);
    engine_note_off_key(&e, 7, 0); // the first key let go: nothing, the voice is the new key's
    for (int i = 0; i < 10; i++) render(128);
    CHECK(voices() == 1 && fabsf(v->env - 0.5f) < 0.01f); // still holding: not released
    engine_note_off_key(&e, 7, 12);
    for (int i = 0; i < 30; i++) render(128);
    CHECK(voices() == 0);
    // Legato off: the second note starts over (the envelope from the top).
    p.legato = 0;
    engine_note_on_key(&e, 7, 127, 0, s, &p);
    for (int i = 0; i < 100; i++) render(128);
    engine_note_on_key(&e, 7, 127, 12, s, &p);
    render(16);
    const struct voice *n = 0;
    for (int i = 0; i < ENGINE_VOICES; i++)
        if (e.voice[i].stage != 0 && e.voice[i].key == 12) n = &e.voice[i];
    CHECK(n && n->env > 0.9f);
    engine_kill_all(&e);
    for (int i = 0; i < 30; i++) render(128);
}

static void test_s600_trim(const char *shot_dir) {
    seq_fresh();
    load_pad(0);
    os.selected = 0;
    os_program_set_s600(&os, 0, 1);
    os.s600_page = 1; // Env2
    s600_open(&os);
    struct input in = blank();
    frame(&in);
    tap(IN_B1); // after Env2: Trim, the Sample screen's
    CHECK(os.screen == OS_SAMPLE && os.s600_trim && os.page == PAGE_TRIM);
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "s600_trim");
    tap(IN_B2); // <Filter>
    CHECK(os.screen == OS_S600 && os.s600_page == 2 && !os.s600_trim);
    os.s600_page = 1;
    tap(IN_B1);
    CHECK(os.screen == OS_SAMPLE && os.s600_trim);
    tap(IN_B1); // round to Env1
    CHECK(os.screen == OS_S600 && os.s600_page == 0);
    // Main: Mono Legato after Note On, before Portamento.
    os.s600_page = 4;
    s600_open(&os);
    os.s600_cursor = 4; // Poly, then Legato
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.pads[0].sp.legato == 1);
    if (shot_dir) write_shot(shot_dir, "s600_main_legato");
    // B2: Filter, LFO, then Mod: the mod wheel's Pitch (vibrato, 30), Filter (LFO or OPEN), Level;
    // the pitch bend's Up and Down.
    os.s600_page = 2;
    s600_open(&os);
    tap(IN_B2);
    tap(IN_B2);
    CHECK(os.screen == OS_S600 && os.s600_page == 6 && os.pads[0].sp.mod_pitch == 30 && os.pads[0].sp.bend_up == 2);
    os.s600_cursor = 3; // Filter Mode: LFO to OPEN
    in = blank(), in.encoder = 1;
    frame(&in);
    os.s600_cursor = 1; // K2 Filter
    in = blank(), in.encoder = 50;
    frame(&in);
    render(16);
    CHECK(os.pads[0].sp.mod_flt_open == 1 && os.pads[0].sp.mod_filter == 50 && e.pad[ENGINE_S600 + 0].p.mod_filter == 50);
    os.toast_until = 0;
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "s600_mod");
    tap(IN_B2);
    CHECK(os.s600_page == 2); // round to Filter
    // The wheel OPEN on a closed low-pass: pushed up, the highs come through.
    {
        engine_kill_all(&e);
        for (int i = 0; i < 20; i++) render(128);
        struct sample *buzz = sample_new(44100, 1, ENGINE_RATE);
        for (int i = 0; i < 44100; i++) buzz->data[i] = (i / 4) % 2 ? 0.5f : -0.5f; // 5.5 kHz square
        struct pad_params p = params(44100);
        p.synth = 1, p.gate = 1, p.sustain = 100, p.filter = FILTER_LP4, p.cutoff = 30, p.mod_filter = 100, p.mod_flt_open = 1;
        float level[2];
        for (int w = 0; w < 2; w++) {
            engine_set_mod(&e, w ? 127 : 0);
            engine_note_on_key(&e, 40, 127, 0, buzz, &p);
            for (int i = 0; i < 8; i++) render(128);
            float peak = 0;
            for (int i = 0; i < 128 * 2; i++) peak = fmaxf(peak, fabsf(block[i]));
            level[w] = peak;
            engine_kill_all(&e);
            for (int i = 0; i < 20; i++) render(128);
        }
        CHECK(level[1] > level[0] * 3);
        // Level: the wheel's tremolo on the LFO, here a square: half the time down.
        p.filter = FILTER_OFF, p.mod_filter = 0, p.mod_level = 100, p.lfo_shape = LFO_SQUARE, p.lfo_rate = 60;
        engine_set_mod(&e, 127);
        engine_note_on_key(&e, 40, 127, 0, buzz, &p);
        float lo = 1e9f, hi = 0;
        for (int b = 0; b < 200; b++) {
            render(128);
            float pk = 0;
            for (int i = 0; i < 256; i++) pk = fmaxf(pk, fabsf(block[i]));
            lo = fminf(lo, pk), hi = fmaxf(hi, pk);
        }
        CHECK(hi > lo * 1.8f);
        engine_set_mod(&e, 0);
        engine_kill_all(&e);
        for (int i = 0; i < 20; i++) render(128);
        sample_free(buzz);
    }
}

// Notes on a track at the start of each bar, the note number its bar (1, 2, ...).
static void bar_notes(int track, int bars) {
    const struct seq_sequence *q = &os.seq.sequence[os.seq.current];
    for (int b = 0; b < bars; b++) seq_add_note(&os.seq, os.seq.current, track, (uint32_t)(b * seq_bar_ticks(q)), b + 1, 100, 24);
}

// The note at the start of bar b (from 1), or 0.
static int note_at_bar(int track, int b) {
    const struct seq_sequence *q = &os.seq.sequence[os.seq.current];
    const struct seq_track *k = &q->track[track];
    for (int i = 0; i < k->count; i++)
        if (k->notes[i].tick == (uint32_t)((b - 1) * seq_bar_ticks(q))) return k->notes[i].note;
    return 0;
}

static void test_seq_bars_and_tempo(const char *shot_dir) {
    seq_fresh();
    struct seq_sequence *q = &os.seq.sequence[0];
    q->bars = 4;
    bar_notes(0, 4);
    bar_notes(5, 4);
    // Longer, duplicated: new bars repeat the old ones from bar 1.
    CHECK(seq_set_bars(&os.seq, 0, 6, 1) == 0);
    CHECK(q->bars == 6 && note_at_bar(0, 5) == 1 && note_at_bar(0, 6) == 2 && note_at_bar(5, 6) == 2);
    CHECK(seq_set_bars(&os.seq, 0, 4, 0) == 0 && q->track[0].count == 4); // shorter: bars 5-6 go
    CHECK(seq_set_bars(&os.seq, 0, 10, 1) == 0);
    int want[11] = {0, 1, 2, 3, 4, 1, 2, 3, 4, 1, 2}, ok = 1;
    for (int b = 1; b <= 10; b++) ok &= note_at_bar(0, b) == want[b];
    CHECK(ok);
    seq_set_bars(&os.seq, 0, 4, 0);
    CHECK(seq_set_bars(&os.seq, 0, 8, 0) == 0 && q->track[0].count == 4); // longer, empty

    // Bars: on the main page asks first, with notes there: Yes copies.
    seq_set_bars(&os.seq, 0, 4, 0);
    os.screen = OS_MAIN;
    os.cursor_play = 2; // Bars
    struct input in = blank();
    in.encoder = 2;
    frame(&in);
    CHECK(os.seq_ask == SEQ_ASK_BARS && os.ask_bars == 6 && q->bars == 4);
    in = blank();
    frame(&in); // drawn while it waits
    if (shot_dir) write_shot(shot_dir, "seq_ask_bars");
    tap(IN_B3); // <Yes>
    CHECK(os.seq_ask == SEQ_ASK_NONE && q->bars == 6 && note_at_bar(0, 6) == 2);
    // Fewer bars, with notes in them: "Delete bars 5-6?" <No> keeps them.
    in = blank();
    in.encoder = -2;
    frame(&in);
    CHECK(os.seq_ask == SEQ_ASK_BARS && os.ask_bars == 4);
    tap(IN_B1);
    CHECK(os.seq_ask == SEQ_ASK_NONE && q->bars == 6);
    frame(&in);
    tap(IN_B3);
    CHECK(q->bars == 4 && q->track[0].count == 4);

    // Shift + Pad 3: doubled; <No> adds empty bars. Shift + Pad 2: halved, after asking.
    in = blank();
    in.pad_hit[2] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.seq_ask == SEQ_ASK_BARS && os.ask_bars == 8);
    tap(IN_B2);
    CHECK(q->bars == 8 && q->track[0].count == 4);
    in = blank();
    in.pad_hit[1] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in); // bars 5-8 are empty: no question
    CHECK(os.seq_ask == SEQ_ASK_NONE && q->bars == 4);
    frame(&in);
    CHECK(os.seq_ask == SEQ_ASK_BARS && os.ask_bars == 2);
    tap(IN_B3);
    CHECK(q->bars == 2 && q->track[0].count == 2);

    // Shift + Pad 7: double speed. <Yes> keeps the speed: every tick and the bars doubled.
    q->tempo10 = 900;
    in = blank();
    in.pad_hit[6] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.seq_ask == SEQ_ASK_BPM_DOUBLE);
    if (shot_dir) write_shot(shot_dir, "seq_ask_bpm");
    tap(IN_B3);
    CHECK(q->tempo10 == 1800 && q->bars == 4 && note_at_bar(0, 3) == 2 && q->track[0].notes[0].dur == 48);
    // Shift + Pad 6: half speed, <Yes>: squeezed into the first half, repeated in the second.
    in = blank();
    in.pad_hit[5] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.seq_ask == SEQ_ASK_BPM_HALF);
    tap(IN_B3);
    CHECK(q->tempo10 == 900 && q->bars == 4 && note_at_bar(0, 1) == 1 && note_at_bar(0, 2) == 2 && note_at_bar(0, 3) == 1 &&
          note_at_bar(0, 4) == 2 && q->track[0].count == 4);
    // <No>: only the BPM.
    frame(&in);
    tap(IN_B2);
    CHECK(q->tempo10 == 450 && q->bars == 4 && q->track[0].count == 4);
    q->tempo10 = 900;
    frame(&in);
    CHECK(os.seq_ask == SEQ_ASK_BPM_HALF);
    tap(IN_B1); // <Cancel>
    CHECK(q->tempo10 == 900 && os.seq_ask == SEQ_ASK_NONE);
    q->tempo10 = 450;

    // K1 the track, K2 the BPM, K3 the program, each once it reaches the value.
    in = blank();
    in.knob_moved[1] = 1, in.knob[1] = (450 - 300) * 127 / 2700;
    frame(&in);
    in.knob[1] = 127;
    frame(&in);
    CHECK(q->tempo10 == 3000);
    in = blank();
    in.knob_moved[0] = 1, in.knob[0] = 0;
    frame(&in);
    in.knob[0] = 127;
    frame(&in);
    CHECK(os.seq.track == SEQ_TRACKS - 1);
    if (shot_dir) write_shot(shot_dir, "seq_main_knobs");
}

static void test_sample_rec_cycle_and_pages(void) {
    engine_init(&e);
    os_init(&os, &e);
    CHECK(os.page == PAGE_MIX); // the Sample screen opens on Vol
    os.screen = OS_MAIN;
    int pages[5];
    for (int i = 0; i < 5; i++) tap(IN_SAMPLE_REC), pages[i] = os.record_page;
    CHECK(pages[0] == 0 && pages[1] == 1 && pages[2] == 2 && pages[3] == 3 && pages[4] == 0);
    // Shift + Pad 13: Trim.
    load_pad(0);
    struct input in = blank();
    in.pad_hit[12] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.screen == OS_SAMPLE && os.page == PAGE_TRIM);
    // Note Repeat's B keys: TOGGLE keeps a rate until its key is pressed again.
    os.rep_toggle = 1;
    os.seq.rep_rate = REP_16;
    in = blank();
    in.down = in.held = 1u << IN_NOTE_REPEAT;
    frame(&in);
    in = blank();
    in.held = 1u << IN_NOTE_REPEAT;
    in.down = 1u << IN_B3;
    in.held |= 1u << IN_B3;
    frame(&in);
    int alt = seq_repeat_alt(REP_16, 2);
    CHECK(os.seq.rep_alt == alt);
    in = blank();
    in.held = 1u << IN_NOTE_REPEAT;
    in.up = 1u << IN_B3;
    frame(&in);
    CHECK(os.seq.rep_alt == alt); // let go: still on
    in = blank();
    in.up = 1u << IN_NOTE_REPEAT;
    frame(&in);
    CHECK(os.seq.rep_alt == alt); // and with NOTE REPEAT let go
    in = blank();
    in.held = 1u << IN_NOTE_REPEAT;
    in.down = in.held | (1u << IN_B3);
    in.held |= 1u << IN_B3;
    frame(&in);
    CHECK(os.seq.rep_alt == -1); // pressed again: off
    in = blank();
    in.up = (1u << IN_NOTE_REPEAT) | (1u << IN_B3);
    frame(&in);
    os.rep_toggle = 0;
}

// The arpeggiator, played straight from the audio thread's hook: which pads and tunings sound.
static int arp_heard[64][2], arp_n;

static void arp_collect(void) { // the voices started since last asked, by pad and key
    for (int i = 0; i < ENGINE_VOICES; i++) {
        struct voice *v = &e.voice[i];
        if (v->stage == 0 || v->age == 0xffffffffu) continue;
        if (arp_n < 64) arp_heard[arp_n][0] = v->pad, arp_heard[arp_n][1] = v->key, arp_n++;
        v->age = 0xffffffffu; // counted
    }
}

static void test_arpeggiator(const char *shot_dir) {
    seq_fresh();
    os.levels_on = 0;
    for (int p = 0; p < 3; p++) load_pad(p);
    struct seq *sq = &os.seq;
    // The page: Type, Rate, then Pattern; a pattern brings its rate; <Rate> steps the rate.
    struct input in = blank();
    in.down = in.held = (1u << IN_NOTE_REPEAT) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    in = blank();
    in.up = 1u << IN_NOTE_REPEAT;
    frame(&in);
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(sq->arp == 1);
    os.cursor_repeat = 3; // Pattern
    int canon = -1, feel = -1, n = seq_arp_patterns(), groups[ARP_GROUPS] = {0};
    for (int i = 0; i < n; i++) {
        groups[seq_arp_group(i)]++;
        if (strcmp(seq_arp_name(i), "CANON IN D") == 0) canon = i;
        if (strcmp(seq_arp_name(i), "I FEEL LOVE") == 0) feel = i;
    }
    CHECK(groups[ARP_GROUP_STANDARD] >= 6 && groups[ARP_GROUP_VARIATION] >= 6 && groups[ARP_GROUP_SYNTH] >= 8 &&
          groups[ARP_GROUP_BASS] >= 8 && groups[ARP_GROUP_CHORD] >= 8 && groups[ARP_GROUP_GAME] >= 6 &&
          groups[ARP_GROUP_FAMOUS] >= 5 && groups[ARP_GROUP_CLASSICAL] >= 15 && canon > 0 && feel > 0);
    os.cursor_repeat = 2; // Category: CLASSICAL, its first pattern
    in = blank();
    in.encoder = ARP_GROUP_CLASSICAL;
    frame(&in);
    CHECK(sq->arp_pattern == canon && sq->rep_rate == REP_4 && sq->arp_gate == 80);
    os.cursor_repeat = 3; // Pattern: stays inside its category
    in.encoder = 100;
    frame(&in);
    CHECK(seq_arp_group(sq->arp_pattern) == ARP_GROUP_CLASSICAL && strcmp(seq_arp_name(sq->arp_pattern), "GYMNOPEDIE 1") == 0);
    in.encoder = -100;
    frame(&in);
    CHECK(sq->arp_pattern == canon);
    if (shot_dir) write_shot(shot_dir, "arpeggiator");
    tap(IN_B2); // <Rate>
    CHECK(sq->rep_rate == REP_4T && os.cursor_repeat == 1);
    tap(IN_B1);

    // UP across three held pads: 0, 1, 2, 0 ... one a step.
    sq->arp_pattern = 0, sq->rep_rate = REP_16;
    seq_set_repeat(sq, 1);
    for (int p = 0; p < 3; p++) seq_repeat_press(sq, p, p, 100, 0, 0);
    arp_n = 0;
    for (int b = 0; b < 4000 && arp_n < 6; b++) render(32), arp_collect();
    CHECK(arp_n >= 6 && arp_heard[0][0] == 0 && arp_heard[1][0] == 1 && arp_heard[2][0] == 2 && arp_heard[3][0] == 0);
    // Octaves 2: up through the pads, then again an octave higher.
    seq_set_repeat(sq, 0);
    for (int i = 0; i < 200; i++) render(64), arp_collect();
    sq->arp_oct = 2;
    seq_set_repeat(sq, 1);
    for (int p = 0; p < 3; p++) seq_repeat_press(sq, p, p, 100, 0, 0);
    arp_n = 0;
    for (int b = 0; b < 8000 && arp_n < 6; b++) render(32), arp_collect();
    CHECK(arp_n >= 6 && arp_heard[2][0] == 2 && arp_heard[2][1] == 0 && arp_heard[3][0] == 0 && arp_heard[3][1] == 12 &&
          arp_heard[5][1] == 12);
    sq->arp_oct = 1;
    // Latch: let go, it keeps going; a new pad after that starts a new chord.
    sq->arp_latch = 1;
    for (int p = 0; p < 3; p++) seq_repeat_release(sq, p);
    int still = 0;
    for (int p = 0; p < 3; p++) still += sq->rep[p].on;
    CHECK(still == 3);
    seq_repeat_press(sq, 4, 4, 100, 0, 0);
    still = 0;
    for (int p = 0; p < 16; p++) still += sq->rep[p].on;
    CHECK(still == 1 && sq->rep[4].on);
    seq_arp_set_latch(sq, 0);
    // A riff: I FEEL LOVE on the lowest pad held, retuned 0, 12, 7, 10.
    seq_set_repeat(sq, 0);
    for (int i = 0; i < 200; i++) render(64), arp_collect();
    sq->arp_pattern = feel;
    seq_set_repeat(sq, 1);
    seq_repeat_press(sq, 2, 2, 100, 0, 0);
    seq_repeat_press(sq, 1, 1, 100, 0, 0);
    arp_n = 0;
    for (int b = 0; b < 4000 && arp_n < 4; b++) render(32), arp_collect();
    CHECK(arp_n >= 4 && arp_heard[0][0] == 1 && arp_heard[0][1] == 0 && arp_heard[1][1] == 12 && arp_heard[2][1] == 7 &&
          arp_heard[3][1] == 10);
    seq_set_repeat(sq, 0);
    sq->arp = 0;
    engine_kill_all(&e);
    for (int i = 0; i < 30; i++) render(128);
}

static void test_s600_lpf(void) {
    // S600 LPF: four poles (steep), a strong peak at resonance, oscillating on its own at the
    // top, and never running away.
    struct pad_params p = params(44100 * 2);
    p.synth = 1, p.gate = 1, p.sustain = 100, p.filter = FILTER_MOOG, p.cutoff = 57; // about 1 kHz
    double lv[3];
    double hzs[3] = {200, 1000, 4000};
    for (int reso = 0; reso <= 80; reso += 80)
        for (int h = 0; h < 3; h++) {
            engine_init(&e);
            struct sample *s = sample_new(44100 * 2, 1, 44100);
            for (int i = 0; i < 44100 * 2; i++) s->data[i] = 0.03f * (float)sin(2 * M_PI * hzs[h] * i / 44100);
            p.reso = reso;
            engine_note_on(&e, 0, 127, s, &p);
            double acc = 0;
            int cnt = 0;
            for (int b = 0; b < 40; b++) {
                render(1024);
                if (b > 20) for (int i = 0; i < 2048; i += 2) acc += block[i] * block[i], cnt++;
            }
            lv[h] = 10 * log10(acc / cnt / (0.03 * 0.03 / 2));
            if (reso == 0 && h == 2) CHECK(lv[2] < -40); // two octaves up: about 24 dB an octave
            if (reso == 80 && h == 1) CHECK(lv[1] > 10);   // the resonant peak
            if (reso == 80 && h == 0) CHECK(lv[0] > -3);   // and the bass still there
            engine_kill_all(&e);
            render(64);
            sample_free(s);
        }
    engine_init(&e);
    struct sample *click = sample_new(44100 * 2, 1, 44100);
    memset(click->data, 0, sizeof(float) * 44100 * 2);
    for (int i = 0; i < 40; i++) click->data[i] = 0.5f;
    p.reso = 100;
    engine_note_on(&e, 0, 127, click, &p);
    double late = 0;
    float peak = 0;
    for (int b = 0; b < 70; b++) {
        render(1024);
        for (int i = 0; i < 2048; i++) {
            if (b > 50) late += block[i] * block[i];
            if (fabsf(block[i]) > peak || block[i] != block[i]) peak = block[i] != block[i] ? 99 : fabsf(block[i]);
        }
    }
    CHECK(late / (19 * 2048) > 0.001 && peak < 1.5f); // still ringing a second later, and bounded
    engine_kill_all(&e);
    render(64);
    sample_free(click);
    // The S600's default filter, and the order the pages list.
    struct s600_params q;
    s600_defaults(&q);
    CHECK(q.filter == FILTER_MOOG && FILTER_LP2 == 1 && FILTER_HP2 == 2 && FILTER_BP2 == 3 && FILTER_LP4 == 4 &&
          FILTER_HP4 == 5 && FILTER_BP4 == 6 && FILTER_MOOG == 7);
}

static void test_help_and_locate(const char *shot_dir) {
    seq_fresh();
    load_pad(0);
    // The main page: Shift + SEQ REC on a bar, Shift + SAMPLE REC back.
    struct seq_sequence *q = &os.seq.sequence[0];
    q->bars = 4;
    int bar = seq_bar_ticks(q);
    struct input in = blank();
    in.down = in.held = (1u << IN_SEQ_REC) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    frame(&in);
    CHECK(os.screen == OS_MAIN && os.seq.pos == 2 * bar);
    os.seq.pos = 2 * bar + 10;
    in.down = in.held = (1u << IN_SAMPLE_REC) | (1u << IN_SHIFT);
    frame(&in);
    CHECK(os.seq.pos == 2 * bar); // to this bar's start first
    frame(&in);
    CHECK(os.seq.pos == bar);
    for (int i = 0; i < 5; i++) frame(&in);
    CHECK(os.seq.pos == 0);
    in.down = in.held = (1u << IN_SEQ_REC) | (1u << IN_SHIFT);
    for (int i = 0; i < 9; i++) frame(&in);
    CHECK(os.seq.pos == 3 * bar); // stops at the last bar

    // Elsewhere: help. Shift + SAMPLE REC its first page, Shift + SEQ REC its second.
    os.screen = OS_SAMPLE;
    in.down = in.held = (1u << IN_SAMPLE_REC) | (1u << IN_SHIFT);
    frame(&in);
    CHECK(os.screen == OS_HELP && os.help_return == OS_SAMPLE && os.help_page == 0);
    struct ui u0 = {.px = px, .now_ms = 1000 + 280 * 9, .track_led = -1}; // the helper pointing
    struct input idle = blank();
    os_frame(&u0, &idle, &os);
    if (shot_dir) write_shot(shot_dir, "help");
    if (shot_dir) { // a title too long for the big letters: in the small ones, whole
        int was = os.help_return;
        os.screen = OS_PREFS;
        CHECK(help_open(&os, 0) == 1);
        os_frame(&u0, &idle, &os);
        write_shot(shot_dir, "help_prefs");
        os.screen = OS_SAMPLE;
        CHECK(help_open(&os, 0) == 1 && was == OS_SAMPLE);
    }
    in.down = in.held = (1u << IN_SEQ_REC) | (1u << IN_SHIFT);
    frame(&in);
    CHECK(os.screen == OS_HELP && os.help_page == 1);
    tap(IN_B3); // <Next>: a third page
    CHECK(os.help_page == 2);
    tap(IN_B1);
    CHECK(os.help_page == 1);
    tap(IN_B2); // <Exit>
    CHECK(os.screen == OS_SAMPLE);
    // The record pages each have their own.
    os.screen = OS_RECORD, os.record_page = 3;
    in.down = in.held = (1u << IN_SAMPLE_REC) | (1u << IN_SHIFT);
    frame(&in);
    CHECK(os.screen == OS_HELP && os.help_return == OS_RECORD);
    tap(IN_B2);
    CHECK(os.screen == OS_RECORD && os.record_page == 3);
    // Every page with help wraps inside the text area and has at least one page.
    int screens[] = {OS_SAMPLE, OS_LOOP, OS_S600, OS_LEVELS, OS_MIXER, OS_EQ, OS_REPEAT, OS_ZONES, OS_PROGRAM,
                     OS_TC, OS_METRO, OS_ERASE, OS_MIDI, OS_CCMAP, OS_PADNOTES, OS_PRINT, OS_INFX, OS_MUTE, OS_BROWSER, OS_SEQCOPY, OS_ARPEDIT,
                     OS_COPYEV, OS_COPYBARS, OS_OUTS};
    for (int i = 0; i < (int)(sizeof screens / sizeof screens[0]); i++) {
        os.screen = screens[i];
        CHECK(help_open(&os, 0) == 1);
        os.screen = os.help_return;
    }
    if (getenv("HELP_DUMP")) { // every help page, as laid out
        int all[] = {OS_SAMPLE, OS_LOOP, OS_S600, OS_LEVELS, OS_MIXER, OS_EQ, OS_REPEAT, OS_ZONES, OS_PROGRAM, OS_TC, OS_METRO,
                     OS_ERASE, OS_MIDI, OS_CCMAP, OS_PADNOTES, OS_PRINT, OS_INFX, OS_SENDS, OS_MASTER, OS_MUTE, OS_SONG, OS_STEP,
                     OS_WARP, OS_PREFS, OS_DISK, OS_BROWSER, OS_SEQCOPY, OS_ARPEDIT, OS_INSERT, OS_COPYEV, OS_COPYBARS, OS_OUTS};
        for (int i = 0; i < (int)(sizeof all / sizeof all[0]); i++) help_dump(&os, all[i]);
        for (int r = 0; r < 7; r++) os.record_page = r, help_dump(&os, OS_RECORD);
        for (int m = 0; m < 3; m++) os.multi_page = m, help_dump(&os, OS_MULTI);
    }
    os.screen = OS_MAIN;
    CHECK(help_open(&os, 0) == 0); // the main page has none
}

// SHIFT + ERASE: Copy Events (after the MPC3000's Edit 6), with Copy Sequence on B1 and Copy
// Bars (Edit 5: copy, insert blank, delete) on B2.
static void test_shift_erase_copy(const char *shot_dir) {
    seq_fresh();
    struct seq *sq = &os.seq;
    struct seq_sequence *q = &sq->sequence[0];
    q->bars = 4;
    int bar = seq_bar_ticks(q);
    sq->track = 0;
    seq_add_note(sq, 0, 0, 0, 0, 100, 10);                // A01 in bar 1
    seq_add_note(sq, 0, 0, (uint32_t)bar + 48, 1, 90, 10); // A02 in bar 2
    seq_add_note(sq, 0, 0, (uint32_t)bar * 3, 2, 80, 10);  // A03 in bar 4
    os.screen = OS_MAIN;
    struct input in = blank();
    in.down = in.held = (1u << IN_ERASE) | (1u << IN_SHIFT);
    in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_COPYEV && os.ce_seq == 0 && os.ce_track == 1 && os.ce_from == 0 && os.ce_to == (uint32_t)bar * 4);
    CHECK(os.ce_to_seq == 0 && os.ce_to_track == 1 && os.ce_at == 0 && os.ce_replace && os.ce_copies == 1);
    in = blank(), in.up = 1u << IN_ERASE;
    frame(&in);
    CHECK(os.screen == OS_COPYEV); // letting go of ERASE does not open the Erase page
    if (shot_dir) write_shot(shot_dir, "copy_events");
    os.ce_to_track = 2;
    tap(IN_B3); // <Do It>: track 1 onto track 2
    CHECK(q->track[1].count == 3 && q->track[1].notes[1].tick == (uint32_t)bar + 48 && q->track[1].notes[1].vel == 90);
    // Bars 2-3 only, twice, from bar 3 on: what falls past the end is left out
    os.ce_from = (uint32_t)bar, os.ce_to = (uint32_t)bar * 3, os.ce_at = (uint32_t)bar * 2, os.ce_copies = 2, os.ce_to_track = 3;
    tap(IN_B3);
    CHECK(q->track[2].count == 1 && q->track[2].notes[0].tick == (uint32_t)bar * 2 + 48); // the second copy starts at bar 5: left out
    // REPLACE: what was in the span goes first; and pads pick the notes
    os.ce_from = 0, os.ce_to = (uint32_t)bar * 4, os.ce_at = 0, os.ce_copies = 1, os.ce_replace = 1, os.ce_to_track = 3;
    seq_copyev_pad(&os, 2); // A03 only
    tap(IN_B3);
    CHECK(q->track[2].count == 1 && q->track[2].notes[0].note == 2 && q->track[2].notes[0].tick == (uint32_t)bar * 3);
    seq_copyev_pad(&os, 2);
    CHECK(os.ce_count == 0);
    // Every track (0) into sequence 2, each onto its own track; sequence 2 is given 4 bars first
    sq->sequence[1].bars = 4;
    os.ce_track = 0, os.ce_to_seq = 1, os.ce_replace = 0;
    tap(IN_B3);
    CHECK(sq->sequence[1].track[0].count == 3 && sq->sequence[1].track[1].count == 3 && sq->sequence[1].track[2].count == 1);
    // K2 the To track (JUMP), K1 the sequence, K3 the end by bars, the slider the From track
    os.knob_mode = KNOB_JUMP;
    os.ce_track = 1;
    in = blank();
    in.knob_moved[1] = 1, in.knob[1] = 127;
    frame(&in);
    CHECK(os.ce_to_track == SEQ_TRACKS);
    in = blank();
    in.knob_moved[2] = 1, in.knob[2] = 0;
    frame(&in);
    CHECK(os.ce_to == (uint32_t)bar * (uint32_t)sq->sequence[os.ce_seq].bars || os.ce_to == (uint32_t)bar);
    in = blank();
    in.fader_moved = 1, in.fader = 127;
    frame(&in);
    CHECK(os.ce_track == SEQ_TRACKS);
    os.knob_mode = KNOB_CATCH;
    // SHIFT shows <Back>; SHIFT + B1 leaves
    in = blank();
    in.held = 1u << IN_SHIFT, in.shift = 1;
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "copy_events_shift");
    // B1: Copy Sequence, to the first empty after; its Back comes back here
    tap(IN_B1);
    CHECK(os.screen == OS_SEQCOPY && os.copy_from == os.ce_seq && os.copy_to == 2);
    tap(IN_B1);
    CHECK(os.screen == OS_COPYEV);
    // B2: Copy Bars, COPY to start: bars 1-4 of sequence 1 after its end, then twice
    tap(IN_B2);
    CHECK(os.screen == OS_COPYBARS && os.cb_mode == 0 && os.cb_first == 1 && os.cb_last == 4 && os.cb_before == 5);
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "copy_bars");
    os.cb_first = 2, os.cb_last = 2, os.cb_before = 1, os.cb_copies = 2; // bar 2, twice, before bar 1
    tap(IN_B3);
    CHECK(q->bars == 6 && q->track[0].count == 5);
    CHECK(q->track[0].notes[0].tick == 48 && q->track[0].notes[1].tick == (uint32_t)bar + 48); // the copies first
    CHECK(q->track[0].notes[2].tick == (uint32_t)bar * 2 && q->track[0].notes[4].tick == (uint32_t)bar * 5); // the old bars after
    // INSERT BLANK: 2 bars before bar 1; DELETE: them again
    tap(IN_B2);
    CHECK(os.cb_mode == 1);
    os.cb_before = 1, os.cb_count = 2;
    tap(IN_B3);
    CHECK(q->bars == 8 && q->track[0].notes[0].tick == (uint32_t)bar * 2 + 48);
    tap(IN_B2);
    CHECK(os.cb_mode == 2);
    os.cb_first = 1, os.cb_last = 2;
    tap(IN_B3);
    CHECK(q->bars == 6 && q->track[0].notes[0].tick == 48);
    os.cb_first = 1, os.cb_last = 6;
    tap(IN_B3); // all of them: refused
    CHECK(q->bars == 6);
    tap(IN_B1);
    CHECK(os.screen == OS_COPYEV);
    in = blank();
    in.down = in.held = (1u << IN_B1) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_MAIN);
}

// Knob Mode: CATCH waits for the knob to reach the value, JUMP takes it at once, SCALE moves it
// from where it is, so the two meet at the ends.
// Timing Correct: K1 Swing, K2 Shift amount, K3 the Move Existing track (JUMP, so at once).
static void test_tc_knobs(void) {
    seq_fresh();
    os.seq.tc = TC_16;
    seq_tc_open(&os);
    os.knob_mode = KNOB_JUMP;
    struct input in = blank();
    in.knob_moved[0] = 1, in.knob[0] = 127;
    in.knob_moved[1] = 1, in.knob[1] = 127;
    in.knob_moved[2] = 1, in.knob[2] = 0;
    for (int i = 0; i < 3; i++) os.knob_last[i] = 64;
    frame(&in);
    CHECK(os.seq.swing == 75 && os.seq.shift_ticks == SEQ_PPQ / 2 - 1 && os.tc_track == 0 && os.cursor_tc == 4);
    os.seq.res = SEQ_RES_SP1200;
    in = blank();
    in.knob_moved[0] = 1, in.knob[0] = 60; // about 61%: the SP12's 63
    frame(&in);
    CHECK(os.seq.swing == 63 && os.cursor_tc == 1);
    os.seq.res = SEQ_RES_MPC3000, os.knob_mode = KNOB_CATCH;
    os.screen = OS_MAIN;
}

static void test_knob_modes(void) {
    seq_fresh();
    int c = 0;
    os.knob_mode = KNOB_CATCH;
    CHECK(os_knob_take(&os, &c, 10, 20, 64) == -1 && !c);
    CHECK(os_knob_take(&os, &c, 60, 70, 64) == 70 && c); // crossed it
    c = 0;
    os.knob_mode = KNOB_JUMP;
    CHECK(os_knob_take(&os, &c, 10, 90, 40) == 90);
    os.knob_mode = KNOB_SCALE;
    int a = os_knob_take(&os, &c, 64, 96, 32);
    CHECK(a > 32 && a < 96);                              // up, but less than the knob went
    CHECK(os_knob_take(&os, &c, 96, 127, a) == 127);      // they meet at the top
    CHECK(os_knob_take(&os, &c, 64, 0, 100) == 0);        // and at the bottom
    a = os_knob_take(&os, &c, 64, 63, 100);
    CHECK(a >= 98 && a < 100);                           // a small turn moves it a little
    CHECK(os_knob_take(&os, &c, -1, 63, 100) == -1);      // the first reading moves nothing
    // A pad's Volume on the Vol page, by K1, in JUMP: straight to where the knob is.
    load_pad(0);
    os.selected = 0;
    os.page = PAGE_MIX;
    os.knob_mode = KNOB_JUMP;
    os.knob_last[0] = 10;
    struct input in = blank();
    in.knob_moved[0] = 1, in.knob[0] = 127;
    frame(&in);
    CHECK(os.pads[0].p.volume == 100);
    os.knob_mode = KNOB_CATCH;
}

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)(v >> 16), p[3] = (unsigned char)(v >> 24);
}

// A sequence saved on its own and loaded into another; a MIDI file's tracks onto the first empty
// tracks; an MPC .ALL's sequence, its drum notes onto pads by the program file beside it; an MPC
// program and its WAV.
static void test_seq_files(const char *shot_dir) {
    seq_fresh();
    char cmd0[1600];
    char dir[] = "/tmp/claude-mpc-seqfiles-XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    char p[600];
    struct seq *sq = &os.seq;
    // One sequence, saved and loaded into sequence 4.
    snprintf(sq->sequence[0].name, sizeof sq->sequence[0].name, "GROOVE");
    sq->sequence[0].tempo10 = 935, sq->sequence[0].bars = 3;
    seq_add_note(sq, 0, 2, 96, 5, 101, 24);
    snprintf(p, sizeof p, "%s/one.sq6", dir);
    CHECK(disk_seq_save(&os, 0, p) == 0);
    CHECK(disk_seq_load(&os, 3, p) == 0);
    struct seq_sequence *q = &sq->sequence[3];
    CHECK(q->tempo10 == 935 && q->bars == 3 && !strcmp(q->name, "GROOVE") && q->track[2].count == 1 &&
          q->track[2].notes[0].tick == 96 && q->track[2].notes[0].note == 5 && q->track[2].notes[0].vel == 101);

    // A MIDI file, format 1, 480 a beat: a tempo map, a bass on channel 1, drums on channel 10.
    static const unsigned char mid[] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 3, 0x01, 0xe0,
        'M', 'T', 'r', 'k', 0, 0, 0, 19, 0, 0xff, 0x51, 3, 0x07, 0xa1, 0x20, 0, 0xff, 0x58, 4, 3, 2, 24, 8, 0, 0xff, 0x2f, 0,
        'M', 'T', 'r', 'k', 0, 0, 0, 21, 0, 0xff, 0x03, 4, 'B', 'a', 's', 's', 0, 0x90, 48, 100, 0x83, 0x60, 0x80, 48, 0, 0, 0xff, 0x2f, 0,
        'M', 'T', 'r', 'k', 0, 0, 0, 13, 0x87, 0x40, 0x99, 37, 90, 60, 37, 0, 0, 0xff, 0x2f, 0};
    snprintf(p, sizeof p, "%s/song.mid", dir);
    FILE *f = fopen(p, "wb");
    fwrite(mid, 1, sizeof mid, f);
    fclose(f);
    struct seq_import im;
    CHECK(imp_midi_load(&im, p) == 0 && im.nseq == 1 && im.seq[0].ntracks == 2);
    CHECK(im.seq[0].tempo10 == 1200 && im.seq[0].sig_num == 3 && im.seq[0].sig_den == 4);
    CHECK(im.seq[0].track[0].midi && !im.seq[0].track[1].midi && !strcmp(im.seq[0].track[0].name, "Bass"));
    CHECK(im.seq[0].track[0].notes[0].dur == 96 && im.seq[0].track[1].notes[0].tick == 192);
    if (shot_dir) { // the tracks to choose from, on Save/Load
        struct seq_import *show = calloc(1, sizeof *show);
        CHECK(imp_midi_load(show, p) == 0);
        snprintf(show->file, sizeof show->file, "song.mid");
        os.imp = show, os.disk_ask = 'T', os.screen = OS_DISK, os.disk_which = 5, os.data_dir = dir;
        struct input in = blank();
        frame(&in);
        write_shot(shot_dir, "disk_midi_tracks");
        in = blank();
        in.down = in.held = 1u << IN_B1; // <Cancel>
        frame(&in);
        CHECK(os.imp == 0 && os.disk_ask == 0);
        os.screen = OS_MAIN, os.data_dir = 0;
    }
    seq_add_note(sq, 5, 0, 0, 0, 100, 24); // sequence 6 has track 1 in use
    int first, dropped, n = imp_apply(&os, &im, 5, &first, &dropped);
    CHECK(n == 2 && first == 1 && dropped == 0);
    q = &sq->sequence[5];
    CHECK(q->track[1].type == TRACK_MIDI && q->track[1].channel == 1 && q->track[1].notes[0].note == 48);
    CHECK(q->track[2].type == TRACK_DRUM && seq_pad_note(q->track[2].notes[0].note) == 37);
    imp_free(&im);

    // An MPC .ALL: one sequence at 98 BPM; track 1 KCK on program TESTPGM, its note 40 on pad 3.
    enum { TRK = 0x30 + 4000, EV = TRK + 65 * 0x30 + 16 };
    int L = EV + 2 * 16 + 16;
    unsigned char *all = calloc(1, (size_t)(0x30 + L));
    put32(all, (uint32_t)(0x30 + L));
    memcpy(all + 4, "MPC1000 ALL 1.00", 16);
    put32(all + 0x2c, 0x30);
    unsigned char *c = all + 0x30;
    put32(c, (uint32_t)L);
    memcpy(c + 4, "BEAT", 4);
    put32(c + 0x1c, 2), put32(c + 0x20, 980);
    put32(c + 0x30, 0x60000000), put32(c + 0x34, 0x60000180); // bars of 384 ticks: 4/4
    memcpy(c + TRK + 1 * 0x30, "KCK", 3);
    memcpy(c + TRK + 1 * 0x30 + 0x11, "TESTPGM", 7);
    unsigned char *e1 = c + EV, *e2 = c + EV + 16;
    put32(e1, 0), e1[4] = 1, e1[5] = 0x90, e1[6] = 40, e1[7] = 120, put32(e1 + 8, 12);
    put32(e2, 192), e2[4] = 1, e2[5] = 0x90, e2[6] = 99, e2[7] = 120, put32(e2 + 8, 12); // note 99: no pad
    memset(c + EV + 32, 0xff, 3), c[EV + 35] = 0x7f;
    snprintf(p, sizeof p, "%s/ALL_SEQS.ALL", dir);
    f = fopen(p, "wb");
    fwrite(all, 1, (size_t)(0x30 + L), f);
    fclose(f);
    memcpy(c + 4, "MPC1000 SEQ 1.00", 16); // the same sequence saved on its own: a .SEQ
    snprintf(p, sizeof p, "%s/MY BEAT.SEQ", dir);
    f = fopen(p, "wb");
    fwrite(c, 1, (size_t)L, f);
    fclose(f);
    free(all);
    CHECK(imp_mpc_load(&im, p) == 0 && im.nseq == 1 && !strcmp(im.seq[0].name, "MY BEAT") && im.seq[0].tempo10 == 980 &&
          im.seq[0].ntracks == 1 && im.seq[0].track[0].count == 2);
    imp_free(&im);
    // The program beside it: pad 1 plays KICK, level 100, mixer 80, pan 60, tuned down a semitone.
    unsigned char *pgm = calloc(1, 0x2a04);
    put32(pgm, 0x2a04);
    memcpy(pgm + 4, "MPC1000 PGM 1.01", 16);
    unsigned char *pad = pgm + 0x18;
    memcpy(pad, "KICK", 4);
    pad[0x11] = 100, pad[0x13] = 127, pad[0x14] = (unsigned char)(-100 & 0xff), pad[0x15] = 0xff;
    pad[0x67] = 5, pad[0x8f] = 80, pad[0x90] = 60, pad[0x63] = 2;
    memset(pgm + 0x2958, 0x40, 128);
    pgm[0x2958 + 40] = 3;
    snprintf(p, sizeof p, "%s/TESTPGM.PGM", dir);
    f = fopen(p, "wb");
    fwrite(pgm, 1, 0x2a04, f);
    fclose(f);
    free(pgm);
    snprintf(p, sizeof p, "%s/ALL_SEQS.ALL", dir);
    CHECK(imp_mpc_load(&im, p) == 0 && im.nseq == 1 && im.seq[0].tempo10 == 980 && im.seq[0].ntracks == 1);
    CHECK(im.seq[0].track[0].has_map && !im.seq[0].track[0].midi && !strcmp(im.seq[0].track[0].name, "KCK"));
    n = imp_apply(&os, &im, 8, &first, &dropped);
    q = &sq->sequence[8];
    CHECK(n == 1 && first == 0 && dropped == 1 && q->tempo10 == 980 && q->loop_bar == 1 && q->track[0].count == 1 && q->track[0].notes[0].note == 3);
    imp_free(&im);
    // The program, and its WAV.
    snprintf(p, sizeof p, "%s/KICK.wav", dir);
    CHECK(wav_save(p, tone(60, 44100, 4410, 0.5f), 16) == 0);
    { // with the MPC's trim in it: an Akai smpl chunk, start 100, last frame 1999
        FILE *w = fopen(p, "r+b");
        fseek(w, 0, SEEK_END);
        long riff = ftell(w);
        unsigned char ch[8 + 56] = {'s', 'm', 'p', 'l', 56, 0, 0, 0, 0x47, 0, 0, 1, 0x5e};
        unsigned char *x = ch + 8 + 36;
        ch[8 + 32] = 20; // sampler data: 20 bytes
        x[0] = 1, x[2] = 60;
        put32(x + 10, 100), put32(x + 14, 1999);
        fwrite(ch, 1, sizeof ch, w);
        unsigned char sz[4];
        put32(sz, (uint32_t)(riff + (long)sizeof ch - 8));
        fseek(w, 4, SEEK_SET);
        fwrite(sz, 1, 4, w);
        fclose(w);
    }
    snprintf(p, sizeof p, "%s/TESTPGM.PGM", dir);
    int layers;
    CHECK(disk_mpc_program_load(&os, 2, p, &layers) == 1 && layers == 0);
    const struct os_pad *pp = &os.programs[2].pads[0];
    CHECK(pp->s && pp->p.volume == 80 && pp->p.pan == 10 && pp->p.semi == -1 && pp->p.fine == 0 && pp->p.mute_group == 2);
    CHECK(pp->p.start == 100 && pp->p.end == 2000); // the trim from the MPC
    CHECK(!strcmp(os.programs[2].name, "TESTPGM") && os.program == 0);
    // The whole project: TESTPGM into the first empty program, then its sequence into the first empty
    // sequence, playing it.
    snprintf(p, sizeof p, "%s/Project01.PRJ", dir);
    f = fopen(p, "wb");
    fputs("ALL_SEQS.ALL\r\nTESTPGM.PGM\r\nKICK.WAV\r\n", f);
    fclose(f);
    int npg, nsq, empty_pg = -1, empty_sq = -1;
    for (int g = 0; g < OS_PROGRAMS && empty_pg < 0; g++)
        if (!os_program_sounds(&os, g)) empty_pg = g;
    for (int g = 0; g < SEQ_COUNT && empty_sq < 0; g++)
        if (!seq_used(&sq->sequence[g])) empty_sq = g;
    CHECK(disk_mpc_project_load(&os, p, &npg, &nsq) == 0 && npg == 1 && nsq == 1);
    CHECK(os.programs[empty_pg].pads[0].s && sq->sequence[empty_sq].track[0].kit == empty_pg &&
          sq->sequence[empty_sq].tempo10 == 980);
    if (getenv("MPC_PRJ")) { // a real one, to look at: MPC_PRJ=path/Project01.PRJ
        seq_fresh();
        CHECK(disk_mpc_project_load(&os, getenv("MPC_PRJ"), &npg, &nsq) == 0);
        printf("  %s: %d programs, %d sequences\n", getenv("MPC_PRJ"), npg, nsq);
        for (int g = 0; g < 4; g++) printf("    pgm %d %s: %d pads\n", g + 1, os.programs[g].name, os_program_sounds(&os, g));
        for (int q = 0; q < 3; q++) {
            const struct seq_sequence *qq = &os.seq.sequence[q];
            printf("    seq %d %s %d.%d BPM %d bars:", q + 1, qq->name, qq->tempo10 / 10, qq->tempo10 % 10, qq->bars);
            for (int t = 0; t < 8; t++)
                if (qq->track[t].count) printf(" [%s pgm%d n%d]", qq->track[t].name, qq->track[t].kit + 1, qq->track[t].count);
            printf("\n");
        }
        mpc_prj_playback(getenv("MPC_PRJ_SEQ") ? atoi(getenv("MPC_PRJ_SEQ")) - 1 : 0);
    }
    // On Save/Load: MPC PROJECT opens at the top of the card, with an MPC's folder there; the MPC 600 S's
    // own saved projects are not shown; B1 goes up out of a folder.
    {
        char top[] = "/tmp/claude-mpc-card-XXXXXX", q2[700];
        CHECK(mkdtemp(top) != 0);
        snprintf(q2, sizeof q2, "%s/MPC600S/projects/MINE/samples", top);
        CHECK(system((snprintf(cmd0, sizeof cmd0, "mkdir -p '%s' '%s/MPC BEAT' && touch '%s/MPC600S/projects/MINE/project.bin' && cp '%s'/* '%s/MPC BEAT/'",
                               q2, top, top, dir, top), cmd0)) == 0);
        snprintf(q2, sizeof q2, "%s/MPC600S", top);
        os.data_dir = q2;
        disk_open(&os, DISK_LOAD, DISK_MPCPRJ);
        CHECK(os.disk_nfiles == 2 && !strcmp(os.disk_files[0], "MPC BEAT") && !strcmp(os.disk_files[1], "MPC600S"));
        os.disk_page = 1; // the files
        struct input in = blank();
        in.down = in.held = 1u << IN_B3; // <Open>
        frame(&in);
        CHECK(os.disk_isdir[0] == 2 && !strcmp(os.disk_files[os.disk_file], "Project01") && !os.disk_isdir[os.disk_file]);
        os.screen = OS_MAIN; // opened again, and on another type and back: still in that folder
        disk_open(&os, DISK_LOAD, DISK_MPCPRJ);
        CHECK(os.disk_isdir[0] == 2 && !strcmp(os.disk_files[os.disk_file], "Project01"));
        os.disk_cursor = 1; // Type
        in = blank();
        in.encoder = -1;
        frame(&in);
        in.encoder = 1;
        frame(&in);
        CHECK(os.disk_type == DISK_MPCPRJ && !strcmp(os.disk_files[os.disk_file], "Project01"));
        os.disk_file = 0, os.disk_page = 1; // the files, onto ..
        in = blank();
        in.down = in.held = 1u << IN_B1; // <Back>: up a folder
        frame(&in);
        CHECK(os.disk_nfiles == 2 && !strcmp(os.disk_files[0], "MPC BEAT"));
        os.disk_file = 1; // into MPC600S, then projects: MINE is not listed
        in = blank();
        in.down = in.held = 1u << IN_B3;
        frame(&in);
        int i = 0;
        while (i < os.disk_nfiles && strcmp(os.disk_files[i], "projects")) i++;
        os.disk_file = i;
        in = blank();
        in.down = in.held = 1u << IN_B3;
        frame(&in);
        CHECK(os.disk_nfiles == 1 && os.disk_isdir[0] == 2); // only ..
        os.screen = OS_MAIN, os.data_dir = 0;
        snprintf(cmd0, sizeof cmd0, "rm -rf %s", top);
        CHECK(system(cmd0) == 0);
    }
    // The sequence again, now its program is loaded: the track plays it.
    snprintf(p, sizeof p, "%s/ALL_SEQS.ALL", dir);
    CHECK(imp_mpc_load(&im, p) == 0);
    n = imp_apply(&os, &im, 9, &first, &dropped);
    CHECK(n == 1 && !strcmp(os.programs[sq->sequence[9].track[0].kit].name, "TESTPGM"));
    imp_free(&im);
    // Saved as an MPC 2500 project, and loaded back: the same sample and trim, pads and notes.
    {
        seq_fresh();
        snprintf(p, sizeof p, "%s/KICK.wav", dir);
        os_pad_load(&os, 3, wav_load(p), "KICK");
        os.pads[3].p.start = 100, os.pads[3].p.end = 2000, os.pads[3].p.volume = 70, os.pads[3].p.pan = -20, os.pads[3].p.semi = 2;
        snprintf(os.programs[0].name, sizeof os.programs[0].name, "MY KIT");
        snprintf(sq->sequence[2].name, sizeof sq->sequence[2].name, "VERSE");
        sq->sequence[2].tempo10 = 935, sq->sequence[2].bars = 4;
        seq_add_note(sq, 2, 0, 96, 3, 110, 24);
        seq_add_note(sq, 2, 0, 288, 3, 90, 24);
        snprintf(sq->sequence[2].track[0].name, sizeof sq->sequence[2].track[0].name, "KICKS");
        char out[700];
        snprintf(out, sizeof out, "%s/OUT", dir);
        int pg, nq, dr;
        CHECK(disk_mpc_project_save(&os, out, "SONG", &pg, &nq, &dr) == 0 && pg == 1 && nq == 1 && dr == 0);
        snprintf(p, sizeof p, "%s/SONG.PRJ", out);
        seq_fresh();
        CHECK(disk_mpc_project_load(&os, p, &pg, &nq) == 0 && pg == 1 && nq == 1);
        const struct os_pad *k3 = &os.programs[0].pads[3];
        CHECK(!strcmp(os.programs[0].name, "MY KIT") && k3->s && k3->p.start == 100 && k3->p.end == 2000 && k3->p.volume == 70 &&
              k3->p.pan == -20 && k3->p.semi == 2);
        const struct seq_sequence *v = &os.seq.sequence[0];
        CHECK(!strcmp(v->name, "VERSE") && v->tempo10 == 935 && v->bars == 4 && v->track[0].count == 2 &&
              v->track[0].notes[0].tick == 96 && v->track[0].notes[0].note == 3 && v->track[0].notes[1].vel == 90 &&
              !strcmp(v->track[0].name, "KICKS") && v->track[0].kit == 0);
        snprintf(p, sizeof p, "%s/ALL_SEQS&SONGS.ALL", out);
        CHECK(imp_mpc_load(&im, p) == 0 && im.nseq == 1); // and its index points at sequence 3, not 1
        imp_free(&im);
        // Two pads on zones of one sound: each its own WAV, only its part, so each keeps its chop
        seq_fresh();
        struct sample *brk = ramp(20000);
        os_pad_load(&os, 0, brk, "BREAK");
        os_pad_assign(&os, 1, brk, "BREAK");
        os.pads[0].p.start = 1000, os.pads[0].p.end = 5000, os.pads[1].p.start = 5000, os.pads[1].p.end = 12000;
        snprintf(out, sizeof out, "%s/CHOPS", dir);
        CHECK(disk_mpc_project_save(&os, out, "CHOPS", &pg, &nq, &dr) == 0 && pg == 1);
        snprintf(p, sizeof p, "%s/CHOPS.PRJ", out);
        seq_fresh();
        CHECK(disk_mpc_project_load(&os, p, &pg, &nq) == 0 && pg == 1);
        const struct os_pad *c0 = &os.programs[0].pads[0], *c1 = &os.programs[0].pads[1];
        CHECK(c0->s && c1->s && c0->s != c1->s && c0->p.end - c0->p.start == 4000 && c1->p.end - c1->p.start == 7000);
        seq_fresh();
    }
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
}

// Purge Unused (the Sample page's last <Do>): the sounds kept in memory that no pad plays go; a
// sound on any program's pad stays; Undo brings them back.
// KNOB FX (note 0) as the panel sends it: each packet's message has its status byte, then zeros.
// The zeros are not note 0 at velocity 0 (a release a moment after the press, which threw every
// real press away as too short): a press, padding, and its release 30 ms later is a whole press.
static void test_knob_fx_padding(void) {
    unsigned char m[16][3];
    const unsigned char press[] = {0x90, 0x00, 0x7f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, release[] = {0x90, 0x00, 0x00, 0, 0, 0, 0, 0, 0},
                        wheel[] = {0x90, 0x6f, 0x7f, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(panel_parse(wheel, sizeof wheel, m, 16) == 1 && m[0][1] == 0x6f); // no KNOB FX after it
    struct input_dev dev = {0};
    struct input in;
    input_begin(&in, &dev);
    int n = panel_parse(press, sizeof press, m, 16);
    CHECK(n == 1);
    for (int i = 0; i < n; i++) input_push(&dev, &in, m[i]);
    CHECK(!input_down(&in, IN_KNOB_FX)); // not yet: a page button acts on its release
    struct timespec ts = {0, 30 * 1000000};
    nanosleep(&ts, 0);
    input_begin(&in, &dev);
    n = panel_parse(release, sizeof release, m, 16);
    CHECK(n == 1);
    for (int i = 0; i < n; i++) input_push(&dev, &in, m[i]);
    CHECK(input_down(&in, IN_KNOB_FX) && dev.ghosts == 0);
}

// STEREO ENHANCER: a click the same on both sides stays on the left, and comes on the right Time
// later (Width full), so a mono sound is heard wide; Side LEFT the other way round.
static void test_stereo_enhancer(void) {
    struct fx *f = fx_new();
    CHECK(f != 0);
    fx_set(f, FX_STEREO);
    f->p[0] = 34; // 10 ms
    char t[16];
    fx_param_text(FX_STEREO, 0, f->p[0], 120, t, sizeof t);
    CHECK(!strcmp(t, "10ms") && !strcmp(fx_name(FX_STEREO), "STEREO ENHANCER"));
    static float b[2 * 2048];
    memset(b, 0, sizeof b);
    b[0] = b[1] = 1; // a click, both sides
    fx_process(f, b, 2048);
    int peak = 0;
    for (int i = 1; i < 2048; i++)
        if (fabsf(b[i * 2 + 1]) > fabsf(b[peak * 2 + 1])) peak = i;
    CHECK(b[0] == 1 && peak >= 435 && peak <= 447); // 10 ms at 44.1 kHz: 441 frames
    fx_set(f, FX_STEREO);
    f->p[0] = 34, f->p[2] = 0; // Side LEFT
    memset(b, 0, sizeof b);
    b[0] = b[1] = 1;
    fx_process(f, b, 2048);
    CHECK(b[1] == 1 && fabsf(b[0]) < 0.01f);
    fx_free(f);
}

// PAD BANK held, pad 1-8: straight to bank A-H. The pad plays nothing, and letting PAD BANK go
// steps no bank on top of the jump. On the mixer the banks stop at G.
static void test_bank_jump(void) {
    struct input_dev dev = {0};
    struct input in;
    const unsigned char bank_on[3] = {0x90, 35, 127}, bank_off[3] = {0x80, 35, 0};
    const unsigned char pad2_on[3] = {0x99, INPUT_PAD_NOTE[1], 100}, pad2_off[3] = {0x89, INPUT_PAD_NOTE[1], 0};
    input_begin(&in, &dev), input_push(&dev, &in, bank_on);
    CHECK(!input_down(&in, IN_PAD_BANK));
    input_begin(&in, &dev), input_push(&dev, &in, pad2_on);
    CHECK(in.bank_jump == 2 && in.pad_hit[1] < 0); // bank B, and no pad hit
    input_begin(&in, &dev), input_push(&dev, &in, pad2_off);
    CHECK(!in.pad_release[1]);
    struct timespec ts = {0, 30 * 1000000};
    nanosleep(&ts, 0);
    input_begin(&in, &dev), input_push(&dev, &in, bank_off);
    CHECK(!input_down(&in, IN_PAD_BANK) && dev.ghosts == 0); // no step after the jump
    input_begin(&in, &dev), input_push(&dev, &in, pad2_on);
    CHECK(in.pad_hit[1] == 100 && !in.bank_jump); // PAD BANK let go: a pad again
    // The OS: to the bank
    seq_fresh();
    struct input f = blank();
    f.bank_jump = 2;
    frame(&f);
    CHECK(os.bank == 1);
    f = blank(), f.bank_jump = 8;
    frame(&f);
    CHECK(os.bank == 7);
    os.screen = OS_MIXER;
    f = blank(), f.bank_jump = 8;
    frame(&f);
    CHECK(os.bank == 6); // the mixer's last bank: G
    seq_fresh();
}

static void test_disk_type_order(void) {
    CHECK(DISK_MIDI + 1 == DISK_XLSEQ && DISK_XLSEQ + 1 == DISK_XLPGM && DISK_XLPGM + 1 == DISK_XLPRJ && DISK_XLPRJ + 1 == DISK_MPC && DISK_MPC + 1 == DISK_MPCPGM &&
          DISK_MPCPGM + 1 == DISK_MPCPRJ && DISK_MPCPRJ + 1 == DISK_SEQUENCE);
}

static void test_purge_unused(void) {
    seq_fresh();
    struct sample *a = ramp(1000), *b = ramp(2000), *c = ramp(3000);
    os_pad_load(&os, 0, a, "A");
    os_pad_load_over(&os, 0, b, "B"); // A kept in memory
    os_set_program(&os, 1);
    os_pad_load(&os, 5, c, "C");
    os_memory_add(&os, c, "C");        // in memory and on a pad of another program: stays
    os_set_program(&os, 0);
    CHECK(os.nmemory == 2);
    CHECK(os_purge_unused(&os) == 1 && os.nmemory == 1 && os.memory[0].s == c && os.pads[0].s == b);
    CHECK(os_purge_unused(&os) == 0);
    struct input in = blank();
    in.down = in.held = (1u << IN_MINUS) | (1u << IN_SHIFT), in.shift = 1; // Undo
    frame(&in);
    CHECK(os.nmemory == 2);
    seq_fresh();
}

// MPC 2000XL files: a chopped program saved as .PGM (each zone its own .SND, trimmed) and all of
// them as .APS; the sequences as .ALL and one as .MID; each loaded back the same. With XL_DIR set,
// the user's own 2000XL files there too.
static void test_xl_files(void) {
    seq_fresh();
    char dir[] = "/tmp/xltestXXXXXX", p[700];
    CHECK(mkdtemp(dir) != 0);
    struct sample *brk = ramp(20000), *sn = ramp(3000);
    os_pad_load(&os, 0, brk, "Break Loop");
    os_pad_assign(&os, 1, brk, "Break Loop");
    os_pad_load(&os, 2, sn, "Snare");
    os_pad_load(&os, 70, sn, "Extra"); // bank E: the 2000XL has four banks
    struct pad_params *a = &os.pads[0].p, *b = &os.pads[1].p;
    a->start = 5000, a->end = 9000, a->volume = 80, a->pan = -10, a->semi = 2, a->fine = 30;
    a->filter = FILTER_LP2, a->cutoff = 60, a->reso = 40, a->gate = 1, a->mute_group = 1, a->attack_ms = 300;
    b->start = 9000, b->end = 14000, b->mute_group = 1;
    snprintf(os.programs[0].name, sizeof os.programs[0].name, "MY KIT");
    int dr = -1, pg = 0, n = 0;
    snprintf(p, sizeof p, "%s/MYKIT.PGM", dir);
    CHECK(disk_xl_pgm_save(&os, 0, p, &dr) == 0 && dr == 1);
    char f1[700], f2[700], f3[700];
    snprintf(f1, sizeof f1, "%s/BREAK_LO.SND", dir), snprintf(f2, sizeof f2, "%s/BREAK_L2.SND", dir), snprintf(f3, sizeof f3, "%s/SNARE.SND", dir);
    CHECK(access(f1, F_OK) == 0 && access(f2, F_OK) == 0 && access(f3, F_OK) == 0); // each zone its own sound, 8 letters
    CHECK(disk_xl_program_load(&os, 3, p) == 3 && os.mpc_missing == 0);
    const struct os_program *g = &os.programs[3];
    const struct pad_params *x = &g->pads[0].p;
    CHECK(!strcmp(g->name, "MY KIT") && g->pads[0].s && g->pads[0].s->frames == 4000 && x->start == 0 && x->end == 4000);
    CHECK(x->volume == 80 && x->pan == -10 && x->semi == 2 && x->fine == 30 && x->filter == FILTER_LP2 && x->cutoff == 60 && x->gate);
    CHECK(x->attack_ms > 250 && x->attack_ms < 350);                                   // 300 ms, in the 2000XL's steps
    CHECK(x->mute_group && x->mute_group == g->pads[1].p.mute_group && !g->pads[2].p.mute_group); // the zones cut each other
    CHECK(g->pads[1].s && g->pads[1].s->frames == 5000 && g->pads[2].s && !g->pads[70].s);
    // All programs as an .APS, loaded into empty ones
    snprintf(p, sizeof p, "%s/ALL.APS", dir);
    CHECK(disk_xl_aps_save(&os, p, "ALL", &pg, &dr) == 0 && pg == 2);
    os_program_clear(&os, 3);
    CHECK(disk_xl_aps_load(&os, p, &pg) == 6 && pg == 2); // programs 1 and 4 into the empty 2 and 3
    CHECK(!strcmp(os.programs[1].name, "MY KIT") && os.programs[1].pads[1].s && os.programs[1].pads[1].s->frames == 5000);
    // Sequences: a drum track and a MIDI track, and another sequence
    struct seq *sq = &os.seq;
    struct seq_sequence *v = &sq->sequence[2];
    snprintf(v->name, sizeof v->name, "VERSE");
    v->tempo10 = 935, v->bars = 4;
    seq_add_note(sq, 2, 0, 96, 0, 110, 24);
    seq_add_note(sq, 2, 0, 288, 2, 90, 30);
    v->track[0].kit = 0;
    snprintf(v->track[0].name, sizeof v->track[0].name, "DRUMS");
    v->track[1].type = TRACK_MIDI, v->track[1].channel = 3;
    seq_add_note(sq, 2, 1, 0, 60, 100, 96);
    snprintf(sq->sequence[5].name, sizeof sq->sequence[5].name, "CHORUS");
    seq_add_note(sq, 5, 0, 0, 2, 100, 12);
    sq->song[0].nsteps = 2, sq->song[0].step[0].seq = 2, sq->song[0].step[0].reps = 2, sq->song[0].step[1].seq = 5, sq->song[0].step[1].reps = 1;
    snprintf(p, sizeof p, "%s/ALL.ALL", dir);
    CHECK(disk_xl_all_save(&os, p, &n, &dr) == 0 && n == 2 && dr == 0);
    struct seq_import im;
    CHECK(imp_xl_all_load(&im, p) == 0 && im.nseq == 2 && im.kind == IMP_XL);
    const struct imp_seq *q = &im.seq[0];
    CHECK(!strcmp(q->name, "VERSE") && q->tempo10 == 935 && q->bars == 4 && q->sig_num == 4 && q->sig_den == 4 && q->ntracks == 2);
    const struct imp_track *t0 = &q->track[0], *t1 = &q->track[1];
    CHECK(!t0->midi && !strcmp(t0->name, "DRUMS") && t0->count == 2 && t0->notes[0].note == 37 && t0->notes[1].note == 42 &&
          t0->notes[1].vel == 90 && t0->notes[1].dur == 30 && t0->has_map && t0->map[37] == 0 && t0->map[42] == 2 && !strcmp(t0->prog, "MY KIT"));
    CHECK(t1->midi && t1->channel == 3 && t1->count == 1 && t1->notes[0].note == 60);
    CHECK(!strcmp(im.seq[1].name, "CHORUS"));
    int first, dropped;
    CHECK(imp_apply(&os, &im, 10, &first, &dropped) == 2 && dropped == 0);
    CHECK(sq->sequence[10].track[0].notes[1].note == 2 && sq->sequence[10].track[0].type == TRACK_DRUM);
    imp_free(&im);
    // One sequence as a 2000XL .MID: its drum track found by its TRACK DATA, on its pads
    snprintf(p, sizeof p, "%s/VERSE.MID", dir);
    CHECK(disk_xl_mid_save(&os, 2, p, &dr) == 0);
    CHECK(imp_midi_load(&im, p) == 0);
    imp_xl_mid_fix(&im, p);
    q = &im.seq[0];
    CHECK(im.kind == IMP_XL && !strcmp(q->name, "VERSE") && q->tempo10 == 935 && q->ntracks == 2);
    CHECK(!q->track[0].midi && q->track[0].map[42] == 2 && q->track[1].midi && q->track[1].channel == 3);
    imp_free(&im);
    // All at once: a folder of ALL_PGMS.APS, ALL_SEQS.ALL and the sounds, every one at 44.1 kHz
    {
        struct sample *r48 = ramp(4800);
        r48->rate = 48000;
        os_set_program(&os, 4);
        os_pad_load(&os, 0, r48, "Hi Rate");
        os.pads[0].p.start = 2400, os.pads[0].p.end = 4800;
        os_set_program(&os, 0);
        char prj[700];
        snprintf(prj, sizeof prj, "%s/PROJECT", dir);
        int sqn = 0;
        CHECK(disk_xl_project_save(&os, prj, &pg, &sqn, &dr) == 0 && pg == 4 && sqn == 3 && dr == 1);
        snprintf(p, sizeof p, "%s/HI_RATE.SND", prj);
        struct snd_info hi;
        struct sample *h = snd_load(p, &hi);
        CHECK(h && h->rate == 44100 && h->frames == 4410 && hi.start == 2205 && hi.end == 4410); // whole, its trim in it, at 44.1
        sample_free(h);
        seq_fresh();
        CHECK(disk_xl_project_load(&os, prj, &pg, &sqn) == 0 && pg == 4 && sqn == 3 && os.mpc_missing == 0);
        CHECK(!strcmp(os.programs[0].name, "MY KIT") && os.seq.sequence[0].track[0].count == 2 &&
              !strcmp(os.programs[os.seq.sequence[0].track[0].kit].name, "MY KIT"));
    }
    // The user's own files, from a 2000XL
    const char *real = getenv("XL_DIR");
    if (real) {
        seq_fresh();
        snprintf(p, sizeof p, "%s/ALL_PGMS.APS", real);
        int pads = disk_xl_aps_load(&os, p, &pg);
        CHECK(pads == 14 && pg == 1 && os.mpc_missing == 0 && !strcmp(os.programs[0].name, "NewPgm-A")); // notes 43 and 45 have no sound
        printf("2000XL APS: %d pads, %d missing; A01 %s, A03 %s cutoff %d group %d\n", pads, os.mpc_missing, os.programs[0].pads[0].name,
               os.programs[0].pads[2].name, os.programs[0].pads[2].p.cutoff, os.programs[0].pads[2].p.mute_group);
        snprintf(p, sizeof p, "%s/ALL_SEQ_.all", real);
        CHECK(imp_xl_all_load(&im, p) == 0 && im.nseq == 2);
        for (int k = 0; k < im.nseq; k++) {
            int notes = 0;
            for (int t = 0; t < im.seq[k].ntracks; t++) notes += im.seq[k].track[t].count;
            printf("2000XL ALL: %s %d.%d BPM %d bars, %d tracks, %d notes\n", im.seq[k].name, im.seq[k].tempo10 / 10, im.seq[k].tempo10 % 10,
                   im.seq[k].bars, im.seq[k].ntracks, notes);
        }
        CHECK(!strcmp(im.seq[0].name, "Sequence01") && !strcmp(im.seq[1].name, "Song01") && im.seq[1].bars == 8);
        im.pick = 1, imp_select_all(&im, 1);
        n = imp_apply(&os, &im, 0, &first, &dropped);
        printf("2000XL Song01 in: %d tracks, %d notes off pads, track 1 plays %s\n", n, dropped, os.programs[os.seq.sequence[0].track[0].kit].name);
        CHECK(n == 6 && dropped == 0);
        imp_free(&im);
    }
    seq_fresh();
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
}

// SHIFT + MUTE (UNMUTE ALL): from any page, every track of the sequence unmuted, solos left alone.
static void test_unmute_all(void) {
    seq_fresh();
    struct seq_sequence *q = &os.seq.sequence[0];
    q->track[1].mute = q->track[5].mute = 1;
    q->track[2].solo = 1;
    os.screen = OS_MAIN;
    struct input in = blank();
    in.down = in.held = (1u << IN_MUTE) | (1u << IN_SHIFT);
    in.shift = 1;
    frame(&in);
    CHECK(!q->track[1].mute && !q->track[5].mute && q->track[2].solo);
    CHECK(os.screen == OS_MAIN); // it does not open Track Mute
    q->track[2].solo = 0;
}

// The MPC2000XL layout: its letters, tabs and rules on every page; its main screen and Track
// Mute page; the keys work as before; saved with the settings.
static void test_layout_xl(const char *shot_dir) {
    seq_fresh();
    load_pad(0);
    struct seq *sq = &os.seq;
    seq_add_note(sq, 0, 0, 0, 0, 100, 24);
    seq_add_note(sq, 0, 1, 0, 0, 100, 24);
    snprintf(sq->sequence[0].name, sizeof sq->sequence[0].name, "Sequence01");
    snprintf(sq->sequence[0].track[0].name, sizeof sq->sequence[0].track[0].name, "Drums");
    snprintf(sq->sequence[0].track[1].name, sizeof sq->sequence[0].track[1].name, "Hats");
    prefs_open(&os);
    os.cursor_prefs = 4; // Layout:
    struct input in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.layout == LAYOUT_2000XL && lcd_layout == LAYOUT_2000XL);
    if (shot_dir) write_shot(shot_dir, "prefs_layout");
    char line[200];
    prefs_save_line(&os, line, sizeof line);
    os.layout = 0;
    prefs_load_line(&os, line);
    CHECK(os.layout == LAYOUT_2000XL && lcd_layout == LAYOUT_2000XL);
    os.screen = OS_MAIN;
    sq->track = 0;
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "xl_main");
    sq->next = 4; // a change waiting: Next: beside Now:
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "xl_main_next");
    sq->next = -1;
    frame(&in);
    tap(IN_B1); // TrMUTE: the track off, as before
    CHECK(!sq->sequence[0].track[0].on);
    tap(IN_B1);
    tap(IN_B3); // Tr +
    CHECK(sq->track == 1);
    tap(IN_MUTE);
    CHECK(os.screen == OS_MUTE);
    in = blank();
    in.pad_hit[1] = 100; // track 2 muted
    frame(&in);
    CHECK(sq->sequence[0].track[1].mute);
    if (shot_dir) write_shot(shot_dir, "xl_mute");
    tap(IN_B3);
    os.screen = OS_SAMPLE; // other pages: the XL's letters, tabs and rules
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "xl_sample");
    os.page = PAGE_AMP; // the envelope: values by their names, the shape beside them
    os.pads[0].p.attack_ms = 300, os.pads[0].p.decay_ms = 2000;
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "xl_env");
    os.page = PAGE_TRIM;
    static const int MORE[] = {OS_MIXER, OS_SENDS, OS_INFX, OS_MASTER};
    static const char *const MORE_NAMES[] = {"xl_mixer", "xl_sends", "xl_infx", "xl_master"};
    for (int i = 0; i < 4; i++) {
        os.screen = MORE[i];
        frame(&in);
        frame(&in);
        if (shot_dir) write_shot(shot_dir, MORE_NAMES[i]);
    }
    os.screen = OS_MAIN;
    frame(&in);
    tap(IN_MUTE);
    tap(IN_B2);
    tap(IN_B3);
    os.layout = LAYOUT_DEFAULT;
    prefs_apply(&os);
    CHECK(lcd_layout == LAYOUT_DEFAULT);
}

static void test_track_mute(const char *shot_dir) {
    seq_fresh();
    load_pad(0);
    load_pad(1);
    struct seq *sq = &os.seq;
    seq_add_note(sq, 0, 0, 0, 0, 100, 24);
    seq_add_note(sq, 0, 0, 0, 1, 100, 24);
    seq_add_note(sq, 0, 1, 0, 0, 100, 24);
    tap(IN_MUTE);
    CHECK(os.screen == OS_MUTE && os.mute_type == MUTE_TRACK);
    struct input in = blank();
    in.pad_hit[1] = 100; // track 2
    frame(&in);
    CHECK(sq->sequence[0].track[1].mute && voices() == 0); // the pad played nothing
    if (shot_dir) write_shot(shot_dir, "track_mute");
    tap(IN_B2); // <All Off>
    CHECK(!sq->sequence[0].track[1].mute);
    tap(IN_B1); // <Type>: TRACK SOLO
    frame(&in);
    CHECK(os.mute_type == MUTE_TRACK_SOLO && sq->sequence[0].track[1].solo);
    tap(IN_B2);
    tap(IN_B1); // PAD MUTE: pad A02 muted, so track 1 plays only A01
    in.pad_hit[1] = 100;
    frame(&in);
    CHECK(os.mute_type == MUTE_PAD && sq->pad_mute[0][1]);
    seq_play(sq, 1);
    for (int i = 0; i < 4; i++) render(128);
    CHECK(voice_on(0) && !voice_on(1));
    seq_stop(sq, 1);
    engine_kill_all(&e);
    for (int i = 0; i < 30; i++) render(128);
    tap(IN_B2);
    tap(IN_B1); // PAD SOLO: only A02 plays
    frame(&in);
    CHECK(os.mute_type == MUTE_PAD_SOLO && sq->pad_solo[0][1]);
    seq_play(sq, 1);
    for (int i = 0; i < 4; i++) render(128);
    CHECK(!voice_on(0) && voice_on(1));
    seq_stop(sq, 1);
    engine_kill_all(&e);
    for (int i = 0; i < 30; i++) render(128);
    tap(IN_B2);
    // SEQUENCE: stopped, a pad makes it current; playing, it queues next. Next Seq: on the wheel.
    tap(IN_B1);
    in = blank();
    in.pad_hit[4] = 100;
    frame(&in);
    CHECK(os.mute_type == MUTE_SEQUENCE && sq->current == 4);
    if (shot_dir) write_shot(shot_dir, "track_mute_seq");
    sq->current = 0;
    seq_play(sq, 1);
    frame(&in);
    CHECK(sq->current == 0 && sq->next == 4);
    os.cursor_mute = 1;
    in = blank();
    in.encoder = 2;
    frame(&in);
    CHECK(sq->next == 6);
    seq_stop(sq, 1);
    tap(IN_B3); // <Back>
    CHECK(os.screen != OS_MUTE);
}

static void test_master_fx(const char *shot_dir) {
    seq_fresh();
    struct master *m = &os.master;
    CHECK(atomic_load(&m->comp) == MCOMP_OFF && !atomic_load(&m->eq_on) && atomic_load(&m->low_freq) == 2 &&
          atomic_load(&m->low_boost) == 20 && atomic_load(&m->low_atten) == 10 && atomic_load(&m->high_freq) == 4 &&
          atomic_load(&m->high_boost) == 20 && atomic_load(&m->bandwidth) == 40 && atomic_load(&m->high_atten) == 0 &&
          atomic_load(&m->thresh) == -6 && atomic_load(&m->ratio) == 4 && atomic_load(&m->out_db) == 0 &&
          !atomic_load(&m->overeasy) && atomic_load(&m->vinyl[0]) == 40 && atomic_load(&m->vinyl[1]) == 0 &&
          atomic_load(&m->vinyl[2]) == 10);
    // Shift + Pad 5 opens it.
    struct input in = blank();
    in.pad_hit[4] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.screen == OS_MASTER);
    in = blank();
    in.encoder = 1; // EQ: ON
    frame(&in);
    CHECK(atomic_load(&m->eq_on) == 1);
    if (shot_dir) write_shot(shot_dir, "master_eq");
    tap(IN_B2); // the Comp page
    in = blank();
    in.encoder = 1; // DBX 160
    frame(&in);
    CHECK(atomic_load(&m->comp) == MCOMP_DBX);
    // A loud tone, 0 dBFS peaks: off it passes untouched; the dbx at -6 dB, 2:1 brings it down.
    enum { N = 44100 };
    static float a[N * 2], b[N * 2];
    for (int i = 0; i < N; i++) a[i * 2] = a[i * 2 + 1] = sinf(2 * 3.14159f * 220 * i / 44100.0f);
    memcpy(b, a, sizeof b);
    atomic_store(&m->eq_on, 0), atomic_store(&m->comp, MCOMP_OFF);
    master_process(m, b, N);
    CHECK(memcmp(a, b, sizeof a) == 0); // off is a true bypass
    atomic_store(&m->comp, MCOMP_DBX);
    for (int i = 0; i < N; i += 128) master_process(m, b + i * 2, 128);
    float r_in = rms(a, N / 2, N), r_out = rms(b, N / 2, N);
    float gr = 20 * log10f(r_in / r_out);
    CHECK(gr > 2.0f && gr < 4.5f); // 6 dB over at 2:1: about 3 dB down
    if (shot_dir) write_shot(shot_dir, "master_comp");
    // The Pultec's low boost lifts 50 Hz.
    for (int i = 0; i < N; i++) a[i * 2] = a[i * 2 + 1] = 0.1f * sinf(2 * 3.14159f * 25 * i / 44100.0f);
    memcpy(b, a, sizeof b);
    atomic_store(&m->comp, MCOMP_OFF), atomic_store(&m->eq_on, 1), atomic_store(&m->low_atten, 0);
    for (int i = 0; i < N; i += 128) master_process(m, b + i * 2, 128);
    CHECK(rms(b, N / 2, N) > rms(a, N / 2, N) * 1.2f); // knob 2: about +2.7 dB down low
    // Vinyl Sim: bounded, and it moves the sound.
    atomic_store(&m->eq_on, 0), atomic_store(&m->comp, MCOMP_VINYL);
    atomic_store(&m->vinyl[1], 100);
    for (int i = 0; i < N; i += 128) master_process(m, b + i * 2, 128);
    int ok = 1;
    for (int i = 0; i < N * 2; i++) ok &= isfinite(b[i]) && fabsf(b[i]) <= 1.0f;
    CHECK(ok);
    // Kept with the settings.
    char line[256];
    master_save_line(m, line, sizeof line);
    master_defaults(m);
    master_load_line(m, line);
    CHECK(atomic_load(&m->comp) == MCOMP_VINYL && atomic_load(&m->vinyl[1]) == 100 && atomic_load(&m->low_atten) == 0);
    master_defaults(m);
}

static void test_fx_modes(const char *shot_dir) {
    seq_fresh();
    load_pad(0);
    load_pad(1);
    struct fx_chain *c = &os.fx;
    tap(IN_KNOB_FX);
    // INPUT: a guitar preset in use.
    infx_load_preset(&os, 1);
    int input_first = atomic_load(&c->type[4]); // its amp
    CHECK(input_first != FX_OFF && os.infx_mode == FXM_INPUT);
    // Type TRACK/MIXER: its own chain (empty), MIX presets.
    os.cursor_infx = 0;
    struct input in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.infx_mode == FXM_TRACK && atomic_load(&c->mode) == FXM_TRACK && atomic_load(&c->type[1]) == FX_OFF);
    CHECK(strcmp(infx_preset_name(os.infx_preset), "USER TEMPLATE") == 0 && infx_preset_kind(os.infx_preset) == 4);
    int studio = -1;
    for (int i = 0; i < infx_presets(); i++)
        if (infx_preset_kind(i) == 4 && strcmp(infx_preset_name(i), "STUDIO") == 0) studio = i;
    CHECK(studio > 0);
    infx_load_preset(&os, studio);
    CHECK(atomic_load(&c->type[0]) == FX_ROOM && atomic_load(&c->type[1]) == FX_PLATE && atomic_load(&c->type[2]) == FX_ASR_HALL);
    if (shot_dir) write_shot(shot_dir, "track_fx");
    // Sends at 0: a true bypass, the track sounds the same with the effects on as off.
    seq_add_note(&os.seq, 0, 0, 0, 0, 100, 96);
    in = blank();
    frame(&in);
    static float dry[44100 * 2], wet[44100 * 2];
    atomic_store(&c->on, 0);
    os_frame(&(struct ui){.px = px, .now_ms = 1000, .track_led = -1}, &in, &os);
    seq_play(&os.seq, 1);
    for (int i = 0; i < 100; i++) engine_render(&e, dry + i * 256, 128);
    seq_stop(&os.seq, 1);
    engine_kill_all(&e);
    for (int i = 0; i < 40; i++) render(128);
    atomic_store(&c->on, 1);
    os_frame(&(struct ui){.px = px, .now_ms = 1000, .track_led = -1}, &in, &os);
    seq_play(&os.seq, 1);
    for (int i = 0; i < 100; i++) engine_render(&e, wet + i * 256, 128);
    seq_stop(&os.seq, 1);
    engine_kill_all(&e);
    for (int i = 0; i < 40; i++) render(128);
    CHECK(memcmp(dry, wet, sizeof(float) * 100 * 256) == 0);
    // A send to the Plate: the track now has reverb (more energy after the note ends).
    os.seq.sequence[0].track[0].send[1] = 100;
    os_sends_push(&os);
    seq_play(&os.seq, 1);
    for (int i = 0; i < 100; i++) engine_render(&e, wet + i * 256, 128);
    seq_stop(&os.seq, 1);
    engine_kill_all(&e);
    CHECK(rms(wet, 0, 12800) > rms(dry, 0, 12800) * 1.01f);
    for (int i = 0; i < 400; i++) render(128);
    // Back to INPUT: the guitar chain is there again; to TRACK: the mix is as it was.
    os.cursor_infx = 0;
    in = blank();
    in.encoder = -1;
    frame(&in);
    frame(&in);
    CHECK(os.infx_mode == FXM_INPUT && atomic_load(&c->type[4]) == input_first && !e.sends_on);
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.infx_mode == FXM_TRACK && atomic_load(&c->type[1]) == FX_PLATE && os.seq.sequence[0].track[0].send[1] == 100);
    // The FX Sends page: Shift + B3 on the mixer.
    seq_mixer_open(&os);
    in = blank();
    frame(&in);
    in = blank();
    in.down = in.held = (1u << IN_B3) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_SENDS);
    os.sends_fx = 2;
    in = blank();
    in.encoder = 30;
    frame(&in);
    CHECK(os.seq.sequence[0].track[0].send[2] == 30);
    if (shot_dir) write_shot(shot_dir, "fx_sends");
    // SHIFT + B3 on FX Sends: the channel's Insert Effects; B1 <Back>.
    in = blank();
    in.down = in.held = (1u << IN_B3) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_INSERT && inserts_used(&os) == 0);
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "inserts_empty");
    tap(IN_B3); // <Add>: an EQ to start, one slot
    CHECK(inserts_used(&os) == 1 && atomic_load(&os.ins.track[0]) == 1 && fx_cat_of(atomic_load(&os.ins.type[0])) == FXC_EQ);
    render(16);
    CHECK(e.ins_track[0] == 1);
    os.ins_cursor = 1; // Category: up to REVERB (four slots)
    in = blank(), in.encoder = -1;
    frame(&in);
    CHECK(fx_cat_of(atomic_load(&os.ins.type[0])) == FXC_REVERB && inserts_used(&os) == 4);
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "inserts_reverb");
    tap(IN_B3); // a second reverb: four more, all eight
    os.ins_cursor = 1, in = blank(), in.encoder = -1;
    frame(&in);
    CHECK(inserts_used(&os) == 8);
    tap(IN_B3); // no room for a third: refused
    CHECK(inserts_used(&os) == 8 && strcmp(os.toast, "All 8 insert slots in use") == 0);
    tap(IN_B2); // <Remove> the second
    CHECK(inserts_used(&os) == 4);
    tap(IN_B3), tap(IN_B3); // two EQs (one slot each): six used. The second asking for a reverb: four needed, three left
    os.ins_cursor = 1, in = blank(), in.encoder = -1;
    frame(&in);
    CHECK(inserts_used(&os) == 6 && strstr(os.toast, "needs 4 slots: 3 left") != 0);
    tap(IN_B2), tap(IN_B2), tap(IN_B2);
    CHECK(inserts_used(&os) == 0);
    // A drive in a channel: its sound comes through changed; then gone, as it was.
    {
        engine_kill_all(&e);
        for (int i = 0; i < 20; i++) render(128);
        struct sample *tone = sample_new(44100, 1, 44100);
        for (int i = 0; i < 44100; i++) tone->data[i] = 0.2f * sinf(2 * 3.14159f * 220 * i / 44100.0f);
        struct pad_params tp = params(44100);
        float peak[2];
        for (int with = 0; with < 2; with++) {
            if (with) {
                tap(IN_B3);
                os.ins_cursor = 2;
                int guard = 0;
                while (atomic_load(&os.ins.type[0]) != FX_DS1 && guard++ < FX_COUNT) {
                    in = blank(), in.encoder = 1;
                    frame(&in);
                }
                CHECK(atomic_load(&os.ins.type[0]) == FX_DS1);
                atomic_store(&os.ins.p[0][0], 127);
                render(16);
            }
            engine_note_on_layers(&e, 7, 100, 0, tone, 0, &tp, 1, 100, 0);
            for (int i = 0; i < 20; i++) render(128);
            float pk = 0;
            for (int i = 0; i < 256; i++) pk = fmaxf(pk, fabsf(block[i]));
            peak[with] = pk;
            engine_kill_all(&e);
            for (int i = 0; i < 20; i++) render(128);
        }
        CHECK(fabsf(peak[1] - peak[0]) > 0.05f); // through the drive, the level and shape change
        // Saved with the project and read back.
        char line[512];
        inserts_save_line(&os, line, sizeof line);
        tap(IN_B2);
        CHECK(inserts_used(&os) == 0);
        inserts_load_line(&os, line);
        CHECK(atomic_load(&os.ins.type[0]) == FX_DS1 && atomic_load(&os.ins.track[0]) == 1 && atomic_load(&os.ins.p[0][0]) == 127);
        tap(IN_B2);
        sample_free(tone);
    }
    tap(IN_B1); // <Back>: FX Sends
    CHECK(os.screen == OS_SENDS);
    tap(IN_B3);
    CHECK(os.screen == OS_MIXER);
    // PAD: the chain on one pad's sound; the pad gets it, another pad does not.
    infx_open(&os);
    os.cursor_infx = 0;
    in = blank();
    in.encoder = 1;
    os.selected = 1;
    frame(&in);
    CHECK(os.infx_mode == FXM_PAD && os.infx_pad == 1);
    in = blank();
    frame(&in);
    CHECK(atomic_load(&e.fx_pad) == os_pad_slot(&os, 1) && !e.sends_on);
    if (shot_dir) write_shot(shot_dir, "pad_fx");
    // Saved and read back: all three chains and the mode.
    char path[] = "/tmp/claude-mpc-settings-XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    os.settings_path = path;
    CHECK(os_save_settings(&os) == 0);
    struct fx_config keep[FXM_COUNT];
    memcpy(keep, os.fx_cfg, sizeof keep);
    os_load_settings(&os);
    CHECK(os.infx_mode == FXM_PAD && os.fx_cfg[FXM_TRACK].type[1] == FX_PLATE && os.fx_cfg[FXM_INPUT].type[4] == input_first);
    unlink(path);
    os.settings_path = 0;
}

// Copy Sequence: Shift + B3 on the main page, to the first empty sequence; a full one asks first.
static void test_seq_copy(const char *shot_dir) {
    seq_fresh();
    struct seq *sq = &os.seq;
    sq->sequence[0].bars = 2, sq->sequence[0].tempo10 = 935;
    snprintf(sq->sequence[0].name, sizeof sq->sequence[0].name, "Verse");
    seq_add_note(sq, 0, 0, 0, 36, 100, 24);
    seq_add_note(sq, 0, 3, 96, 40, 90, 24);
    seq_add_note(sq, 2, 0, 0, 50, 100, 24); // seq 3 has notes: seq 2 is the first empty one
    sq->sequence[0].track[3].level = 70;
    os.screen = OS_MAIN;
    struct input in = blank();
    in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in); // held SHIFT shows <Copy> over B3
    in.down = in.held = (1u << IN_B3) | (1u << IN_SHIFT);
    int track = sq->track;
    frame(&in);
    CHECK(os.screen == OS_SEQCOPY && os.copy_from == 0 && os.copy_to == 1 && sq->track == track);
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "seq_copy");
    tap(IN_B3);
    const struct seq_sequence *q = &sq->sequence[1];
    CHECK(q->bars == 2 && q->tempo10 == 935 && strcmp(q->name, "Verse") == 0);
    CHECK(q->track[0].count == 1 && q->track[3].count == 1 && q->track[3].notes[0].tick == 96 && q->track[3].level == 70);
    CHECK(q->track[0].notes != sq->sequence[0].track[0].notes); // its own copy
    seq_add_note(sq, 1, 0, 48, 38, 100, 24); // changing the copy leaves the original
    CHECK(sq->sequence[0].track[0].count == 1);
    // Onto seq 3, which has notes: it asks, <Cancel> keeps it, <Replace> copies.
    in = blank();
    in.pad_hit[2] = 100; // A03: Seq 3
    frame(&in);
    CHECK(os.copy_to == 2);
    tap(IN_B3);
    CHECK(os.copy_ask == 1 && sq->sequence[2].track[0].notes[0].note == 50);
    tap(IN_B1);
    CHECK(os.copy_ask == 0 && os.screen == OS_SEQCOPY && sq->sequence[2].track[0].notes[0].note == 50);
    tap(IN_B3);
    tap(IN_B3);
    CHECK(sq->sequence[2].track[0].notes[0].note == 36 && sq->sequence[2].track[3].count == 1);
    in = blank();
    in.down = in.held = (1u << IN_MINUS) | (1u << IN_SHIFT), in.shift = 1; // Undo
    frame(&in);
    CHECK(sq->sequence[2].track[0].notes[0].note == 50 && sq->sequence[2].track[3].count == 0);
    tap(IN_B1);
    CHECK(os.screen == OS_MAIN);
}

// Step patterns: the STEP presets, USER patterns made on Pattern Edit, and how they play:
// accents full velocity, ties held on, rests silent, a slide overlapping into the next note.
static void test_arp_steps(const char *shot_dir) {
    seq_fresh();
    struct seq *sq = &os.seq;
    int user1 = seq_arp_user_pattern(0), preset = seq_arp_first(ARP_GROUP_STEP);
    CHECK(seq_arp_user_slot(user1) == 0 && seq_arp_group(user1) == ARP_GROUP_USER &&
          seq_arp_user_slot(seq_arp_user_pattern(ARP_USER - 1)) == ARP_USER - 1 && seq_arp_user_slot(preset) < 0);
    CHECK(seq_arp_group(preset) == ARP_GROUP_STEP && seq_arp_steps_of(sq, preset) && seq_arp_steps_of(sq, preset)->len == 16 &&
          !seq_arp_steps_of(sq, 0));
    // Kept as a line of the settings, and read back the same.
    struct seq_arp_steps a = *seq_arp_steps_of(sq, preset), b;
    char line[80];
    seq_arp_steps_save(&a, line, sizeof line);
    CHECK(seq_arp_steps_load(&b, line) == 0 && memcmp(&a, &b, sizeof a) == 0);
    CHECK(seq_arp_steps_load(&b, "16,zz") != 0);
    // B3 <Edit> on a STEP pattern: copied into the first empty USER one, and opened there.
    os.screen = OS_REPEAT;
    sq->arp = 1, sq->arp_pattern = preset;
    tap(IN_B3);
    CHECK(os.screen == OS_ARPEDIT && sq->arp_pattern == user1 && memcmp(&sq->arp_user[0], &a, sizeof a) == 0);
    tap(IN_B1);
    seq_arp_steps_clear(&sq->arp_user[0]);
    // From Note Repeat (or any other pattern): the arpeggiator on the USER pattern edited last.
    sq->arp = 0;
    tap(IN_B3);
    CHECK(os.screen == OS_ARPEDIT && sq->arp && sq->arp_pattern == user1 && os.arpedit_slot == 0);
    tap(IN_B1);
    // B3 <Edit> on USER 1: pads 15, 1 (an accented C), <Tie>, <Rest>, pads 16, 4 (a D# that
    // slides), pad 6 (F); Len 5.
    tap(IN_B3);
    CHECK(os.screen == OS_ARPEDIT && os.arpedit_slot == 0 && os.arpedit_step == 0);
    int pads[] = {14, 0, -2, -3, 15, 3, 5};
    for (int k = 0; k < 7; k++) {
        if (pads[k] == -2) tap(IN_B3);
        else if (pads[k] == -3) tap(IN_B2);
        else {
            struct input in = blank();
            in.pad_hit[pads[k]] = 100;
            frame(&in);
        }
    }
    os.cursor_arpedit = 1; // Len
    struct input in = blank();
    in.encoder = -11;
    frame(&in);
    const struct seq_arp_steps *p = &sq->arp_user[0];
    CHECK(p->len == 5 && os.arpedit_step == 4); // shortened past the selected step: the last one
    CHECK(p->step[0].time == ARP_NOTE && p->step[0].note == 0 && p->step[0].accent && !p->step[0].slide);
    CHECK(p->step[1].time == ARP_TIE && p->step[2].time == ARP_REST);
    CHECK(p->step[3].note == 3 && p->step[3].slide && !p->step[3].accent && p->step[4].note == 5 && !p->step[4].slide);
    // The knobs, from where they are: K1 moves the step, K2 and K3 turned right set accent and
    // slide on it, left clear them.
    {
        struct input k = blank();
        k.knob_moved[0] = k.knob_moved[1] = k.knob_moved[2] = 1, k.knob[0] = 60, k.knob[1] = 60, k.knob[2] = 60;
        frame(&k); // where they are: nothing changes
        CHECK(os.arpedit_step == 4 && !p->step[4].accent && !p->step[4].slide);
        k = blank();
        k.knob_moved[0] = 1, k.knob[0] = 72; // two steps on, round past the end of five
        frame(&k);
        CHECK(os.arpedit_step == 1);
        k = blank();
        k.knob_moved[0] = 1, k.knob[0] = 66;
        frame(&k);
        CHECK(os.arpedit_step == 0);
        k = blank();
        k.knob_moved[1] = k.knob_moved[2] = 1, k.knob[1] = 70, k.knob[2] = 50;
        frame(&k);
        CHECK(p->step[0].accent && !p->step[0].slide);
        k = blank();
        k.knob_moved[1] = k.knob_moved[2] = 1, k.knob[1] = 60, k.knob[2] = 60;
        frame(&k);
        CHECK(!p->step[0].accent && p->step[0].slide);
        k = blank();
        k.knob_moved[1] = k.knob_moved[2] = 1, k.knob[1] = 70, k.knob[2] = 50; // back as entered
        frame(&k);
        CHECK(p->step[0].accent && !p->step[0].slide);
    }
    os.arpedit_step = 3, os.toast_until = 0;
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "arp_edit");
    tap(IN_B1);
    CHECK(os.screen == OS_REPEAT);
    // Played on MIDI channel 2, so each note can be seen.
    os.seq.midi_send = fake_midi;
    midi_count = 0;
    seq_set_repeat(sq, 1);
    seq_repeat_press(sq, 0, 0, 100, 2, 0);
    double bpm = sq->sequence[sq->current].tempo10 / 10.0;
    int step = (int)(44100 * 60 / bpm / 4);
    for (int f = 0; f < step * 9 / 2; f += 128) render(128);
    int on[8], nv[8], n = 0, slide_off = -1, f_on = -1;
    for (int i = 0; i < midi_count; i++) {
        if (midi_log[i][0] == 0x91 && n < 8) on[n] = midi_log[i][1], nv[n++] = midi_log[i][2];
        if (midi_log[i][0] == 0x91 && midi_log[i][1] == 5) f_on = i;
        if (midi_log[i][0] == 0x81 && midi_log[i][1] == 3) slide_off = i;
    }
    CHECK(n == 3 && on[0] == 0 && on[1] == 3 && on[2] == 5); // the tie and rest play nothing
    CHECK(nv[0] == 100 && nv[1] == 70 && nv[2] == 70);      // the accent at full velocity
    CHECK(f_on >= 0 && slide_off > f_on);                  // the slide holds over into the F
    seq_set_repeat(sq, 0);
    os.seq.midi_send = 0;
    // On a pad's own sound, a slide takes the voice still sounding to the new pitch.
    engine_kill_all(&e);
    for (int i = 0; i < 40; i++) render(128);
    load_pad(0);
    int slot = os_pad_slot(&os, 0);
    engine_play_pad_mix(&e, slot, 100, 0, 0, 1, 100, 0);
    render(128);
    engine_play_pad_glide(&e, slot, 70, 5, 0, 1, 100, 0, 60);
    render(128);
    int sounding = 0, key = -99;
    for (int i = 0; i < ENGINE_VOICES; i++)
        if (e.voice[i].stage != 0) sounding++, key = e.voice[i].key;
    CHECK(sounding == 1 && key == 5);
}

// Additional Settings: Start BPM, TC and Swing are kept with the settings and set at power-on;
// a line saved before them still loads, the Start settings at their defaults.
static void test_prefs_start(void) {
    seq_fresh();
    CHECK(os.start_bpm10 == 1200 && os.start_tc == TC_16 && os.start_swing == 50 && os.button_lights == BUTTON_RED &&
          os.pad_lights == PAD_LIGHT_OFF && os.seq.res == 0 && os.screen_scheme == SCREEN_MPC3000);
    os.start_bpm10 = 935, os.start_tc = TC_8, os.start_swing = 62, os.start_bars = 4;
    char line[64];
    prefs_save_line(&os, line, sizeof line);
    seq_fresh();
    prefs_load_line(&os, line);
    prefs_startup(&os);
    CHECK(os.start_bpm10 == 935 && os.seq.sequence[0].tempo10 == 935 && os.seq.sequence[98].tempo10 == 935);
    CHECK(os.seq.tc == TC_8 && os.seq.swing == 62 && os.seq.sequence[0].bars == 4 && os.seq.sequence[50].bars == 4);
    seq_fresh();
    prefs_load_line(&os, "1,2,0,3");
    CHECK(os.button_lights == 1 && os.pad_lights == 2 && os.start_bpm10 == 1200 && os.start_tc == TC_16 && os.start_bars == 2);
    seq_fresh();
}

// The guards: a sound gone to NaN (a blown-up filter, say) is silenced and counted, and the
// mix after it is clean again at once.
static void test_guards(void) {
    engine_init(&e);
    struct sample *s = constant(4096);
    for (int i = 100; i < 200; i++) s->data[i] = NAN;
    struct pad_params p = params(4096);
    p.filter = FILTER_MOOG, p.cutoff = 50, p.reso = 90;
    unsigned before = engine_guard_trips();
    engine_note_on(&e, 0, 127, s, &p);
    int clean = 1;
    for (int k = 0; k < 20; k++) {
        engine_render(&e, block, 128);
        for (int i = 0; i < 256; i++) clean &= block[i] == block[i] && fabsf(block[i]) < 64.0f;
    }
    CHECK(clean && engine_guard_trips() > before);
    CHECK(fabsf(block[255]) > 0.01f); // the sound plays on after the bad stretch
    engine_kill_all(&e);
    for (int i = 0; i < 40; i++) render(128);
    sample_free(s);
}

// The panel: a second note-on for a pad still held, within 100 ms, is a bounce and plays
// nothing; after a note-off the pad plays again at once.
static void test_pad_bounce(void) {
    struct input_dev dev = {0};
    struct input in;
    const unsigned char on[3] = {0x99, INPUT_PAD_NOTE[0], 100}, off[3] = {0x89, INPUT_PAD_NOTE[0], 0};
    input_begin(&in, &dev);
    input_push(&dev, &in, on);
    CHECK(in.pad_hit[0] == 100 && dev.pad_bounces == 0);
    input_begin(&in, &dev);
    input_push(&dev, &in, on); // again, still held: ignored
    CHECK(in.pad_hit[0] < 0 && dev.pad_bounces == 1);
    input_push(&dev, &in, off);
    input_begin(&in, &dev);
    input_push(&dev, &in, on); // a real second press
    CHECK(in.pad_hit[0] == 100 && dev.pad_bounces == 1);
}

// The Arpeggiator page's knobs (K1 rate, K2 category, K3 pattern, from where they are) and the
// added presets: JAZZ, BASSLINES, more STEP patterns and game cues.
// Pattern Edit from a MIDI keyboard: each key enters its note at the step and moves on, the
// octave from where the key is.
static void test_arpedit_midi(void) {
    engine_init(&e);
    os_init(&os, &e);
    struct seq *sq = &os.seq;
    memset(&sq->arp_user[0], 0, sizeof sq->arp_user[0]);
    sq->arp_user[0].len = 8;
    arpedit_open(&os, 0);
    os.midi_in_read = fake_midi_read;
    fake_port = 0, midi_qn = midi_qat = 0; // the MIDI jacks
    midi_push(0x90, 64, 100); // E above middle C: octave 0
    midi_push(0x80, 64, 0);
    midi_push(0x90, 47, 90);  // B below middle C: -1
    midi_push(0x90, 84, 90);  // two octaves up: +1, the nearest
    struct input in = blank();
    frame(&in);
    const struct seq_arp_steps *p = &sq->arp_user[0];
    CHECK(p->step[0].note == 4 && p->step[0].oct == 0 && p->step[0].time == ARP_NOTE);
    CHECK(p->step[1].note == 11 && p->step[1].oct == -1);
    CHECK(p->step[2].note == 0 && p->step[2].oct == 1);
    CHECK(os.arpedit_step == 3 && os.screen == OS_ARPEDIT);
    os.midi_in_read = 0, midi_qn = midi_qat = 0;
}

static void test_arp_more(void) {
    seq_fresh();
    struct seq *sq = &os.seq;
    int groups[ARP_GROUPS] = {0}, mario = -1, n = seq_arp_patterns();
    for (int i = 0; i < n; i++) {
        groups[seq_arp_group(i)]++;
        if (strcmp(seq_arp_name(i), "MARIO THEME") == 0) mario = i;
    }
    CHECK(groups[ARP_GROUP_JAZZ] >= 15 && groups[ARP_GROUP_BASS] >= 58 && groups[ARP_GROUP_STEP] >= 16 &&
          groups[ARP_GROUP_GAME] >= 11 && groups[ARP_GROUP_FAMOUS] >= 15);
    CHECK(strcmp(seq_arp_group_name(ARP_GROUP_BASS), "BASSLINES") == 0 && strcmp(seq_arp_group_name(ARP_GROUP_JAZZ), "JAZZ") == 0);
    CHECK(mario == 73); // the main melody now, at the same number it had
    for (int i = 0; i < n; i++) { // every step pattern from 1 to 16 steps, its notes in range
        const struct seq_arp_steps *p = seq_arp_steps_of(sq, i);
        if (!p || seq_arp_user_slot(i) >= 0) continue;
        int ok = p->len >= 1 && p->len <= ARP_STEPS;
        for (int k = 0; k < p->len; k++) ok &= p->step[k].note >= 0 && p->step[k].note < 12 && p->step[k].oct >= -1 && p->step[k].oct <= 1;
        CHECK(ok);
        if (!ok) printf("  step pattern %s\n", seq_arp_name(i));
    }
    // The knobs.
    seq_repeat_open(&os);
    sq->arp = 1, sq->arp_pattern = 0, sq->rep_rate = REP_16;
    struct input k = blank();
    k.knob_moved[0] = k.knob_moved[1] = k.knob_moved[2] = 1, k.knob[0] = k.knob[1] = k.knob[2] = 60;
    frame(&k); // where they are: nothing changes
    CHECK(sq->rep_rate == REP_16 && sq->arp_pattern == 0);
    k = blank();
    k.knob_moved[0] = 1, k.knob[0] = 66; // a step faster
    frame(&k);
    CHECK(sq->rep_rate == REP_16T && os.cursor_repeat == 1);
    k = blank();
    k.knob_moved[1] = 1, k.knob[1] = 66; // the next category, its first pattern
    frame(&k);
    CHECK(seq_arp_group(sq->arp_pattern) == ARP_GROUP_VARIATION && os.cursor_repeat == 2);
    int first = sq->arp_pattern;
    k = blank();
    k.knob_moved[2] = 1, k.knob[2] = 72; // two patterns on, in the category
    frame(&k);
    CHECK(sq->arp_pattern == seq_arp_step(first, 2) && os.cursor_repeat == 3);
    os.screen = OS_MAIN;
}

// Save/Load's folders: <New Dir> makes a folder and goes into it, SAVE saves there, LOAD
// browses into it and back up with .., as far as the card and no further, ARP PATTERN keeps a
// USER pattern in a file, and MIXDOWN SEQ exports the mix alone as one WAV.
static void test_disk_folders(const char *shot_dir) {
    seq_fresh();
    char dir[] = "/tmp/claude-mpc-card-XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    os.data_dir = dir;
    struct seq *sq = &os.seq;
    sq->arp_user[2] = *seq_arp_steps_of(sq, seq_arp_first(ARP_GROUP_STEP));
    disk_open(&os, DISK_SAVE, DISK_ARP);
    os.disk_which = 2;
    struct input idle = blank();
    tap(IN_B1); // <New Dir>
    CHECK(os.disk_newdir == 1);
    snprintf(os.disk_name, sizeof os.disk_name, "BEATS");
    tap(IN_B3); // <OK>
    frame(&idle);
    char p[800];
    snprintf(p, sizeof p, "%s/arp/BEATS", dir);
    size_t at = strlen(os.disk_at);
    CHECK(at > 10 && strcmp(os.disk_at + at - 10, "/arp/BEATS") == 0 && access(p, F_OK) == 0 && os.disk_newdir == 0);
    snprintf(os.disk_name, sizeof os.disk_name, "LINE1");
    tap(IN_B3); // <Save>, into BEATS
    frame(&idle);
    snprintf(p, sizeof p, "%s/arp/BEATS/LINE1.arp", dir);
    CHECK(access(p, F_OK) == 0);
    if (shot_dir) {
        os.toast_until = 0;
        frame(&idle);
        write_shot(shot_dir, "disk_folders");
    }
    // LOAD: the folder, into it, the pattern into USER 6, and back up.
    seq_arp_steps_clear(&sq->arp_user[5]);
    disk_open(&os, DISK_LOAD, DISK_ARP);
    CHECK(os.disk_nfiles == 2 && os.disk_isdir[0] == 2 && os.disk_isdir[1] == 1 && strcmp(os.disk_files[1], "BEATS") == 0);
    os.disk_file = 1, os.disk_page = 1;
    tap(IN_B3); // <Open>
    at = strlen(os.disk_at);
    CHECK(at > 6 && strcmp(os.disk_at + at - 6, "/BEATS") == 0 && os.disk_nfiles == 2 && os.disk_isdir[0] == 2 &&
          strcmp(os.disk_files[1], "LINE1") == 0);
    os.disk_file = 1, os.disk_which = 5;
    tap(IN_B3); // <Load>
    frame(&idle);
    CHECK(memcmp(&sq->arp_user[5], &sq->arp_user[2], sizeof sq->arp_user[2]) == 0);
    os.disk_file = 0;
    tap(IN_B3); // .. : back up, to arp
    CHECK(strcmp(os.disk_files[1], "BEATS") == 0 && os.disk_file == 1); // on the folder just left
    tap(IN_B3); // into BEATS again: on LINE1, where the list was
    CHECK(strcmp(os.disk_files[os.disk_file], "LINE1") == 0);
    tap(IN_B1); // <Back> up again
    for (int up = 0; up < 2; up++) os.disk_file = 0, tap(IN_B3); // up to MPC600S, then the card: no .. there
    char root[700];
    snprintf(root, sizeof root, "%s/..", dir);
    char *real = realpath(root, 0);
    CHECK(real && strcmp(os.disk_at, real) == 0 && (os.disk_nfiles == 0 || os.disk_isdir[0] != 2));
    free(real);
    // Loads share their folder: browsed to in one type, the next type opens there too.
    {
        char want[700];
        snprintf(want, sizeof want, "%s", os.disk_at);
        disk_open(&os, DISK_LOAD, DISK_XLSEQ);
        CHECK(!strcmp(os.disk_at, want));
        disk_open(&os, DISK_LOAD, DISK_XLPGM);
        CHECK(!strcmp(os.disk_at, want));
        disk_open(&os, DISK_SAVE, DISK_PROGRAM); // saving: its own folder still
        CHECK(strcmp(os.disk_at, want) != 0);
    }
    // The S600 waveforms: written to MPC600S/s600 at power-on; LOAD's S600 OSC opens there.
    disk_install_waves(&os);
    disk_open(&os, DISK_LOAD, DISK_WAVE);
    {
        int sine = -1;
        for (int i = 0; i < os.disk_nfiles; i++)
            if (strcmp(os.disk_files[i], "SINE") == 0) sine = i;
        CHECK(sine >= 0 && os.disk_nfiles == 5); // .. and the four waves
        os.disk_file = sine, os.selected = 7, os.disk_page = 1;
        tap(IN_B3); // <Load>
        frame(&idle);
        CHECK(os.pads[7].s && os.pads[7].s->frames == 674 && strcmp(os.pads[7].name, "SINE") == 0);
    }
    // MIXDOWN SEQ: the stereo mix alone, one WAV in the folder browsed to.
    load_pad(0);
    seq_add_note(sq, 0, 0, 0, 0, 100, 24);
    sq->sequence[0].bars = 1;
    disk_open(&os, DISK_EXPORT, DISK_MIX_SEQ);
    snprintf(os.disk_name, sizeof os.disk_name, "MIX01");
    if (shot_dir) {
        os.toast_until = 0;
        frame(&idle);
        write_shot(shot_dir, "export_mixdown");
        disk_open(&os, DISK_EXPORT, DISK_SEQUENCE);
        frame(&idle);
        write_shot(shot_dir, "export_seq");
        disk_open(&os, DISK_EXPORT, DISK_MIX_SEQ);
        snprintf(os.disk_name, sizeof os.disk_name, "MIX01");
    }
    tap(IN_B3); // <Export>
    for (int i = 0; i < 20000 && (os.disk_pending || disk_busy(&os)); i++) {
        frame(&idle);
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, 0);
    }
    snprintf(p, sizeof p, "%s/exports/MIX01.wav", dir);
    CHECK(access(p, F_OK) == 0);
    snprintf(p, sizeof p, "%s/exports/MIX01", dir);
    CHECK(access(p, F_OK) != 0); // no folder of tracks
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf %s", dir);
    CHECK(system(rm) == 0);
    os.data_dir = 0;
    os.screen = OS_MAIN;
}

// The instrument stage (Input INST L): the left input alone, to both sides; the gain applied
// (Bright at 0 leaves the tone flat); a hard hit rounded off under full scale.
static void test_instrument_stage(void) {
    static struct fx_chain c;
    memset(&c, 0, sizeof c);
    atomic_store(&c.inst_gain, 12), atomic_store(&c.inst_bright, 0);
    static float b[4410 * 2];
    for (int i = 0; i < 4410; i++) b[i * 2] = 0.05f * sinf(2 * 3.14159265f * 440 * i / 44100.0f), b[i * 2 + 1] = 0.3f;
    fx_instrument(&c, b, 4410);
    float peak = 0;
    int same = 1;
    for (int i = 2205; i < 4410; i++) { // settled
        same &= b[i * 2] == b[i * 2 + 1];
        if (fabsf(b[i * 2]) > peak) peak = fabsf(b[i * 2]);
    }
    CHECK(same && peak > 0.05f * 3.6f && peak < 0.05f * 4.4f); // +12 dB: four times, the right side ignored
    atomic_store(&c.inst_gain, 30);
    for (int i = 0; i < 4410; i++) b[i * 2] = 0.9f * sinf(2 * 3.14159265f * 110 * i / 44100.0f), b[i * 2 + 1] = 0;
    fx_instrument(&c, b, 4410);
    peak = 0;
    for (int i = 0; i < 4410 * 2; i++) peak = fabsf(b[i]) > peak ? fabsf(b[i]) : peak;
    CHECK(peak < 1.0f && peak > 0.9f);
}

// The panel's messages: buttons (a press, a release, a quick tap's release on the next frame),
// the wheel and its push, the knobs, the slider, SHIFT, and the pads (light touches ignored).
static void test_panel_input(void) {
    struct input_dev dev = {0};
    struct input in;
    const unsigned char play_on[3] = {0x90, 82, 127}, play_off[3] = {0x80, 82, 0}, odd[3] = {0x90, 82, 40};
    input_begin(&in, &dev);
    input_push(&dev, &in, odd); // not a press: the panel only sends 127
    CHECK(!input_down(&in, IN_PLAY) && !in.held);
    input_push(&dev, &in, play_on);
    CHECK(input_down(&in, IN_PLAY) && (in.held >> IN_PLAY & 1) && (dev.held >> IN_PLAY & 1));
    input_begin(&in, &dev);
    CHECK(!input_down(&in, IN_PLAY) && (in.held >> IN_PLAY & 1)); // still held, no new press
    input_push(&dev, &in, play_off);
    CHECK((in.up >> IN_PLAY & 1) && !in.held && !dev.held);
    // A quick tap: pressed and let go in one frame; the release comes on the next.
    input_begin(&in, &dev);
    input_push(&dev, &in, play_on);
    input_push(&dev, &in, play_off);
    CHECK(input_down(&in, IN_PLAY) && !(in.up >> IN_PLAY & 1) && dev.late_up);
    input_begin(&in, &dev);
    CHECK(input_late_releases(&dev, &in) == 1 && (in.up >> IN_PLAY & 1) && !in.held && !dev.held && !dev.late_up);
    // A page button (KNOB FX) counts only as a whole press: a note-on alone opens nothing, a
    // blip too quick for a finger opens nothing, a real press opens it on its release (with SHIFT
    // as it was when it went down).
    {
        const unsigned char fx_on[3] = {0x90, 0, 127}, fx_off[3] = {0x80, 0, 0}, sh_on[3] = {0x90, 49, 127},
                            sh_off[3] = {0x80, 49, 0};
        input_begin(&in, &dev);
        input_push(&dev, &in, fx_on);
        CHECK(!input_down(&in, IN_KNOB_FX) && !(in.held >> IN_KNOB_FX & 1));
        input_begin(&in, &dev);
        input_push(&dev, &in, fx_on); // another note-on with no release between: the first was not a press
        CHECK(!input_down(&in, IN_KNOB_FX) && dev.ghosts == 1);
        input_push(&dev, &in, fx_off); // a blip: released at once
        CHECK(!input_down(&in, IN_KNOB_FX) && dev.ghosts == 2);
        input_push(&dev, &in, fx_off); // a release with nothing pressed
        CHECK(!input_down(&in, IN_KNOB_FX));
        input_begin(&in, &dev);
        input_push(&dev, &in, fx_on);
        usleep(30 * 1000); // a finger's press
        input_push(&dev, &in, fx_off);
        CHECK(input_down(&in, IN_KNOB_FX) && !in.shift && (dev.late_up >> IN_KNOB_FX & 1));
        input_begin(&in, &dev);
        CHECK(input_late_releases(&dev, &in) == 1 && !in.held);
        input_begin(&in, &dev); // SHIFT + KNOB FX, SHIFT let go first
        input_push(&dev, &in, sh_on);
        input_push(&dev, &in, fx_on);
        input_push(&dev, &in, sh_off);
        usleep(30 * 1000);
        input_push(&dev, &in, fx_off);
        CHECK(input_down(&in, IN_KNOB_FX) && in.shift);
        input_begin(&in, &dev);
        input_late_releases(&dev, &in);
        CHECK(!in.shift && !in.held);
    }
    // ERASE acts on its release: a release with no press is nothing (it opened the Erase page);
    // a blip too short for a finger is nothing; a real tap is a release.
    {
        const unsigned char er_on[3] = {0x90, 9, 127}, er_off[3] = {0x80, 9, 0}, er_zero[3] = {0x90, 9, 0};
        input_begin(&in, &dev);
        input_push(&dev, &in, er_off);
        input_push(&dev, &in, er_zero);
        CHECK(!(in.up >> IN_ERASE & 1) && !in.held);
        input_begin(&in, &dev);
        input_push(&dev, &in, er_on);
        CHECK(in.held >> IN_ERASE & 1); // held at once, for ERASE with the pads
        input_push(&dev, &in, er_off);
        CHECK(!(in.up >> IN_ERASE & 1) && !in.held && !dev.held && !dev.late_up);
        input_begin(&in, &dev);
        input_push(&dev, &in, er_on);
        input_begin(&in, &dev);
        usleep(30 * 1000);
        input_push(&dev, &in, er_off);
        CHECK((in.up >> IN_ERASE & 1) && !in.held);
    }
    // SHIFT, the wheel and its push, K2, the slider.
    const unsigned char shift_on[3] = {0x90, 49, 127}, wheel_up[3] = {0xB0, 100, 1}, wheel_down[3] = {0xB0, 100, 127},
                        push[3] = {0x90, 111, 127}, k2[3] = {0xB0, 17, 90}, slider[3] = {0xB0, 20, 33};
    input_begin(&in, &dev);
    input_push(&dev, &in, shift_on);
    input_push(&dev, &in, wheel_up);
    input_push(&dev, &in, wheel_up);
    input_push(&dev, &in, wheel_down);
    input_push(&dev, &in, push);
    input_push(&dev, &in, k2);
    input_push(&dev, &in, slider);
    CHECK(in.shift && in.encoder == 1 && in.encoder_press && in.knob_moved[1] && in.knob[1] == 90 && in.fader_moved && in.fader == 33);
    // Pads: a touch too light to count, a press, pressure, a release.
    const unsigned char light[3] = {0x99, INPUT_PAD_NOTE[4], 3}, press[3] = {0x99, INPUT_PAD_NOTE[4], 90},
                        at[3] = {0xA9, INPUT_PAD_NOTE[4], 70}, off[3] = {0x89, INPUT_PAD_NOTE[4], 0};
    input_begin(&in, &dev);
    input_push(&dev, &in, light);
    CHECK(in.pad_hit[4] < 0 && !in.pad_down[4]);
    input_push(&dev, &in, press);
    input_push(&dev, &in, at);
    CHECK(in.pad_hit[4] == 90 && in.pad_down[4] && in.pad_pressure[4] == 70 && dev.pad_down[4]);
    input_begin(&in, &dev);
    CHECK(in.pad_down[4] && in.pad_hit[4] < 0);
    input_push(&dev, &in, off);
    CHECK(in.pad_release[4] && !in.pad_down[4] && !dev.pad_down[4]);
    // Noise: a lone knob reading far from the last is thrown away; a far one the next reading
    // confirms is a fast turn, taken; the main volume's spike to 0 never reaches it.
    {
        struct input_dev d2 = {0};
        struct input i2;
        const unsigned char k60[3] = {0xB0, 16, 60}, k62[3] = {0xB0, 16, 62}, k120[3] = {0xB0, 16, 120},
                            k100[3] = {0xB0, 16, 100}, k103[3] = {0xB0, 16, 103}, m90[3] = {0xB0, 19, 90}, m0[3] = {0xB0, 19, 0};
        input_begin(&i2, &d2), input_push(&d2, &i2, k60);
        CHECK(i2.knob_moved[0] && i2.knob[0] == 60);
        input_begin(&i2, &d2), input_push(&d2, &i2, k120);
        CHECK(!i2.knob_moved[0]); // held back
        input_begin(&i2, &d2), input_push(&d2, &i2, k62);
        CHECK(i2.knob_moved[0] && i2.knob[0] == 62 && d2.rejected == 0); // the spike was not confirmed
        input_begin(&i2, &d2), input_push(&d2, &i2, k100), input_push(&d2, &i2, k103);
        CHECK(i2.knob_moved[0] && i2.knob[0] == 103); // a fast turn: confirmed
        input_begin(&i2, &d2), input_push(&d2, &i2, m90), input_push(&d2, &i2, m0), input_push(&d2, &i2, m90);
        CHECK(i2.main == 90); // the dropout to 0 never got through
        input_begin(&i2, &d2), input_push(&d2, &i2, m0), input_push(&d2, &i2, m0), input_push(&d2, &i2, m90);
        CHECK(!i2.main_moved || i2.main == 90); // 0 twice is still a dropout: the volume stays
        {
            const unsigned char m2[3] = {0xB0, 19, 2};
            // A stray 0 sent four times at once: held (it must hold MAIN_HOLD_MS), the volume stays
            input_begin(&i2, &d2), input_push(&d2, &i2, m0), input_push(&d2, &i2, m2), input_push(&d2, &i2, m0), input_push(&d2, &i2, m0);
            CHECK(!i2.main_moved || i2.main == 90);
            // ...but the same reading still there a tenth of a second on: taken (a real move)
            d2.ctl_held_ms[3] -= 150;
            input_begin(&i2, &d2), input_push(&d2, &i2, m0);
            CHECK(i2.main_moved && i2.main == 0);
            char note[400];
            CHECK(panel_take_note(note, sizeof note) && strstr(note, "MAIN volume jump 90 -> 0"));
            input_begin(&i2, &d2), input_push(&d2, &i2, m90); // and back up (far: held, then taken)
            input_push(&d2, &i2, m90), input_push(&d2, &i2, m90), input_push(&d2, &i2, m90);
            d2.ctl_held_ms[3] -= 150;
            input_push(&d2, &i2, m90);
            CHECK(i2.main == 90);
            // A real turn comes in small steps: each taken at once
            const unsigned char m80[3] = {0xB0, 19, 80}, m70[3] = {0xB0, 19, 70};
            input_begin(&i2, &d2), input_push(&d2, &i2, m80);
            CHECK(i2.main_moved && i2.main == 80);
            input_begin(&i2, &d2), input_push(&d2, &i2, m70);
            CHECK(i2.main_moved && i2.main == 70);
        }
        // Pads: under the threshold, and a light hit right after a hard one elsewhere, ignored.
        const unsigned char soft[3] = {0x99, INPUT_PAD_NOTE[0], 8}, hard[3] = {0x99, INPUT_PAD_NOTE[0], 120},
                            ghost[3] = {0x99, INPUT_PAD_NOTE[1], 20}, real[3] = {0x99, INPUT_PAD_NOTE[2], 70};
        d2.ctl_ms = 0; // long after the knob turns above
        unsigned before = d2.rejected;
        input_begin(&i2, &d2), input_push(&d2, &i2, soft);
        CHECK(i2.pad_hit[0] < 0 && d2.rejected == before + 1);
        input_push(&d2, &i2, hard), input_push(&d2, &i2, ghost), input_push(&d2, &i2, real);
        CHECK(i2.pad_hit[0] == 120 && i2.pad_hit[1] < 0 && i2.pad_hit[2] == 70 && d2.rejected == before + 2);
        panel_set_pad_threshold(5);
        for (int q = 0; q < 16; q++) d2.pad_on_ms[q] = 0; // long after the hard hit
        input_begin(&i2, &d2), input_push(&d2, &i2, (const unsigned char[3]){0x99, INPUT_PAD_NOTE[5], 8});
        CHECK(i2.pad_hit[5] == 8);
        panel_set_pad_threshold(12);
        // A moderate hit: its neighbours' light note-ons (side or corner) go, a pad two away still
        // plays, and a neighbour played nearly as hard is a real press.
        for (int q = 0; q < 16; q++) d2.pad_on_ms[q] = 0;
        before = d2.rejected;
        input_begin(&i2, &d2);
        input_push(&d2, &i2, (const unsigned char[3]){0x99, INPUT_PAD_NOTE[5], 60});
        input_push(&d2, &i2, (const unsigned char[3]){0x99, INPUT_PAD_NOTE[6], 30});  // beside it
        input_push(&d2, &i2, (const unsigned char[3]){0x99, INPUT_PAD_NOTE[10], 25}); // corner
        input_push(&d2, &i2, (const unsigned char[3]){0x99, INPUT_PAD_NOTE[7], 30});  // two away
        input_push(&d2, &i2, (const unsigned char[3]){0x99, INPUT_PAD_NOTE[1], 50});  // corner, nearly as hard
        CHECK(i2.pad_hit[5] == 60 && i2.pad_hit[6] < 0 && i2.pad_hit[10] < 0 && i2.pad_hit[7] == 30 &&
              i2.pad_hit[1] == 50 && d2.rejected == before + 2);
        // A knob turned, the wheel turned or pushed: light pad hits meanwhile are the panel shaking;
        // a firm press still plays; long after, a light one plays again.
        for (int q = 0; q < 16; q++) d2.pad_on_ms[q] = 0;
        before = d2.rejected;
        input_begin(&i2, &d2);
        input_push(&d2, &i2, (const unsigned char[3]){0xB0, 17, 70});                 // K2 turned
        input_push(&d2, &i2, (const unsigned char[3]){0x99, INPUT_PAD_NOTE[13], 30}); // shaken
        input_push(&d2, &i2, (const unsigned char[3]){0x99, INPUT_PAD_NOTE[0], 90});  // pressed
        CHECK(i2.pad_hit[13] < 0 && i2.pad_hit[0] == 90 && d2.rejected == before + 1);
        // Just after a pad press, a press of the wheel, B1-B3 or -/+, and a lone wheel step, are the
        // pad shaking the panel: nothing. A turn under way (steps close together) still turns.
        {
            unsigned g0 = d2.ghosts, r0 = d2.rejected;
            input_begin(&i2, &d2);
            input_push(&d2, &i2, (const unsigned char[3]){0x90, 111, 127});           // the wheel "pushed"
            input_push(&d2, &i2, (const unsigned char[3]){0x90, 12, 127});            // B1 "pressed"
            input_push(&d2, &i2, (const unsigned char[3]){0xB0, 100, 1});             // a lone step
            CHECK(!i2.encoder_press && !(i2.down & (1u << IN_B1)) && i2.encoder == 0 && d2.ghosts == g0 + 2 && d2.rejected == r0 + 1);
            input_push(&d2, &i2, (const unsigned char[3]){0x80, 12, 0});              // its release: nothing
            CHECK(!(i2.up & (1u << IN_B1)));
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            d2.wheel_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;            // a turn under way
            input_push(&d2, &i2, (const unsigned char[3]){0xB0, 100, 1});
            CHECK(i2.encoder == 1);
            d2.pad_hit_ms = 0, d2.wheel_ms = 0;                                        // long after the pad
            before = d2.rejected - 1; // as before this block: the knob's one, counted below from there
        }
        input_begin(&i2, &d2);
        input_push(&d2, &i2, (const unsigned char[3]){0x90, 111, 127});               // the wheel pushed, held
        d2.ctl_ms = 0;
        input_push(&d2, &i2, (const unsigned char[3]){0x99, INPUT_PAD_NOTE[14], 40});
        CHECK(i2.pad_hit[14] < 0 && d2.rejected == before + 2);
        input_begin(&i2, &d2);
        input_push(&d2, &i2, (const unsigned char[3]){0x80, 111, 0});                  // let go
        input_begin(&i2, &d2);
        input_push(&d2, &i2, (const unsigned char[3]){0xB0, 100, 1});                  // the wheel turned
        input_push(&d2, &i2, (const unsigned char[3]){0x99, INPUT_PAD_NOTE[15], 20});
        CHECK(i2.pad_hit[15] < 0 && d2.rejected == before + 3);
        d2.ctl_ms = 0;                                                                     // long after
        for (int q = 0; q < 16; q++) d2.pad_on_ms[q] = 0;
        input_begin(&i2, &d2), input_push(&d2, &i2, (const unsigned char[3]){0x99, INPUT_PAD_NOTE[15], 20});
        CHECK(i2.pad_hit[15] == 20);
    }
    // Drawing: lights and a wake ask go into the frame.
    static uint32_t frame_px[UI_W * UI_H];
    struct ui u = {.px = frame_px, .track_led = -1};
    ui_clear(&u, 0x123456);
    ui_led(&u, 3, 200, 10, -5);
    ui_button_led(&u, IN_SEQ, 2);
    ui_wake(&u, 40);
    ui_wake(&u, 10);
    ui_meter(&u, 0xFF, 3);
    CHECK(frame_px[UI_W * UI_H - 1] == 0x123456 && u.nled == 1 && u.led[0].r == 127 && u.led[0].b == 0 &&
          (u.button_mask >> IN_SEQ & 1) && u.button_level[IN_SEQ] == 2 && u.wake_ms == 10 && u.meter_left == 0x1F);
}

// Renaming from the main page (Shift + B2 with the cursor on Seq, Trk or Pgm); and Input
// Effects, which power on off and as new.
static void test_rename(const char *shot_dir) {
    seq_fresh();
    struct seq *sq = &os.seq;
    struct input in;
    struct input sb2 = blank();
    sb2.down = sb2.held = (1u << IN_B2) | (1u << IN_SHIFT), sb2.shift = 1;
    os.cursor_play = 0; // Seq
    int track = sq->track;
    frame(&sb2);
    CHECK(os.rename_what == RENAME_SEQ && sq->track == track); // B2 did not step the track
    os.rename_pos = 0;
    in = blank();
    in.encoder = 2; // " " -> "B"? the first letter moved on two
    frame(&in);
    snprintf(os.rename_buf, sizeof os.rename_buf, "%-16s", "VERSE");
    if (shot_dir) {
        in = blank();
        frame(&in);
        write_shot(shot_dir, "rename");
    }
    tap(IN_B3);
    CHECK(os.rename_what == RENAME_NONE && strcmp(sq->sequence[sq->current].name, "VERSE") == 0);
    os.cursor_play = 6;
    frame(&sb2);
    snprintf(os.rename_buf, sizeof os.rename_buf, "%-16s", "KICKS");
    tap(IN_B3);
    CHECK(strcmp(sq->sequence[sq->current].track[sq->track].name, "KICKS") == 0);
    os.cursor_play = 11;
    frame(&sb2);
    snprintf(os.rename_buf, sizeof os.rename_buf, "%-16s", "BOOM");
    tap(IN_B1); // <Cancel>: kept as it was
    CHECK(os.rename_what == RENAME_NONE && strcmp(os.programs[sq->sequence[sq->current].track[sq->track].kit].name, "BOOM") != 0);
    frame(&sb2);
    snprintf(os.rename_buf, sizeof os.rename_buf, "%-16s", "BOOM");
    tap(IN_B3);
    CHECK(strcmp(os.programs[sq->sequence[sq->current].track[sq->track].kit].name, "BOOM") == 0);
    sq->sequence[sq->current].track[sq->track].type = TRACK_MIDI; // a MIDI program is a number: no renaming
    frame(&sb2);
    CHECK(os.rename_what == RENAME_NONE);
    sq->sequence[sq->current].track[sq->track].type = TRACK_DRUM;
    // Input Effects at power-on: off, every Type's chain empty, Type INPUT.
    atomic_store(&os.fx.on, 1);
    os.fx_cfg[FXM_TRACK].type[0] = FX_PLATE;
    infx_power_on(&os);
    CHECK(!atomic_load(&os.fx.on) && os.infx_mode == FXM_INPUT && os.fx_cfg[FXM_TRACK].type[0] == FX_OFF &&
          atomic_load(&os.fx.type[0]) == FX_OFF);
    os.screen = OS_MAIN;
}

static void test_song(const char *shot_dir) {
    seq_fresh();
    struct seq *sq = &os.seq;
    CHECK(seq_tc_grid(TC_4) == 96 && seq_tc_grid(TC_4T) == 64 && seq_tc_grid(TC_64) == 6 && seq_tc_grid(TC_64T) == 4);
    for (int n = 0; n < 2; n++) { // two one-bar sequences, a note at the start of each
        sq->sequence[n].bars = 1;
        seq_add_note(sq, n, 0, 0, n, 100, 24);
    }
    struct input in = blank();
    in.pad_hit[11] = 100, in.shift = 1, in.held = 1u << IN_SHIFT; // Shift + Pad 12
    frame(&in);
    CHECK(os.screen == OS_SONG);
    // Pads type it in: A01 (seq 1), A01, A02.
    for (int p = 0; p < 3; p++) {
        in = blank();
        in.pad_hit[p == 2 ? 1 : 0] = 100;
        frame(&in);
    }
    const struct seq_song *g = &sq->song[0];
    CHECK(g->nsteps == 3 && g->step[0].seq == 0 && g->step[1].seq == 0 && g->step[2].seq == 1 && os.song_step_sel == 3);
    CHECK(voices() == 0); // the pads only add steps here
    // The wheel on (end) adds a step; <Delete> takes it off again.
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(g->nsteps == 4 && g->step[3].seq == 2);
    tap(IN_B1);
    CHECK(g->nsteps == 3);
    if (shot_dir) write_shot(shot_dir, "song");
    // PLAY: seq 1, seq 1, seq 2, then the end.
    os.song_step_sel = 0;
    tap(IN_PLAY);
    CHECK(sq->song_on && sq->current == 0);
    int bar = seq_bar_ticks(&sq->sequence[0]), seen[3] = {0}, steps = 0;
    for (int b = 0; b < 4000 && sq->mode != SEQ_STOPPED; b++) {
        render(128);
        if (sq->song_on && sq->song_step != steps) steps = sq->song_step;
        if (sq->song_on) seen[sq->song_step] = sq->current + 1;
    }
    CHECK(sq->mode == SEQ_STOPPED && !sq->song_on && seen[0] == 1 && seen[1] == 1 && seen[2] == 2);
    (void)bar;
    // <Export>: asks first (<Cancel> keeps everything), then one sequence of three bars, in
    // the first empty one.
    tap(IN_B3);
    CHECK(os.song_ask == 3 && sq->sequence[2].track[0].count == 0);
    tap(IN_B1);
    CHECK(os.song_ask == 0 && sq->sequence[2].track[0].count == 0 && os.screen == OS_SONG);
    tap(IN_B3);
    CHECK(os.song_ask == 3);
    tap(IN_B3);
    CHECK(os.song_ask == 0);
    const struct seq_sequence *x = &sq->sequence[2];
    CHECK(x->bars == 3 && x->track[0].count == 3 && x->track[0].notes[2].tick == (uint32_t)(2 * bar) &&
          x->track[0].notes[2].note == 1);
    // SHIFT + B2 <Rename>: the song's name, not a step inserted
    int nsteps0 = sq->song[os.song_sel].nsteps;
    struct input r = blank();
    r.held = 1u << IN_SHIFT, r.shift = 1;
    frame(&r);
    r.down = 1u << IN_B2, r.held |= 1u << IN_B2;
    frame(&r);
    CHECK(os.rename_what == RENAME_SONG && sq->song[os.song_sel].nsteps == nsteps0 && !strncmp(os.rename_buf, "Song01", 6));
    r = blank(), r.down = r.held = 1u << IN_ERASE, r.shift = 1, r.held |= 1u << IN_SHIFT; // SHIFT + ERASE: the whole name
    frame(&r);
    r = blank(), r.encoder = 2; // the first letter: from the blank, two on
    frame(&r);
    char want = os.rename_buf[0];
    tap(IN_B3); // <OK>
    CHECK(os.rename_what == RENAME_NONE && sq->song[os.song_sel].name[0] == want && want != ' ');
    char keep[SEQ_NAME];
    snprintf(keep, sizeof keep, "%s", sq->song[os.song_sel].name);
    // Saved with the project and loaded back
    char dir[] = "/tmp/songnameXXXXXX", p[600];
    CHECK(mkdtemp(dir) != 0);
    snprintf(p, sizeof p, "%s/P", dir);
    CHECK(disk_project_save(&os, p) == 0);
    sq->song[os.song_sel].name[0] = 0;
    CHECK(disk_project_load(&os, p) == 0 && !strcmp(sq->song[0].name, keep));
    snprintf(p, sizeof p, "rm -rf %s", dir);
    CHECK(system(p) == 0);
}

static void test_step_edit(const char *shot_dir) {
    seq_fresh();
    load_pad(0);
    load_pad(1);
    struct seq *sq = &os.seq;
    sq->sequence[0].bars = 1;
    struct input in = blank();
    in.down = in.held = (1u << IN_SEQ) | (1u << IN_SHIFT), in.shift = 1; // Shift + SEQ
    frame(&in);
    CHECK(os.screen == OS_STEP && os.step_tick == 0);
    in = blank();
    in.pad_hit[0] = 90; // a note at Now
    frame(&in);
    tap(IN_PLUS); // a 1/16 on
    CHECK(os.step_tick == 24);
    in = blank();
    in.pad_hit[1] = 80;
    frame(&in);
    const struct seq_track *k = &sq->sequence[0].track[0];
    CHECK(k->count == 2 && k->notes[0].tick == 0 && k->notes[0].note == 0 && k->notes[0].vel == 90 && k->notes[1].tick == 24 &&
          k->notes[1].note == 1);
    tap(IN_B2); // <<Note: back to the first
    CHECK(os.step_tick == 0);
    os.cursor_step = 2 + 1; // its velocity
    in = blank();
    in.encoder = 5;
    frame(&in);
    CHECK(k->notes[0].vel == 95);
    if (shot_dir) write_shot(shot_dir, "step_edit");
    tap(IN_B1); // <Delete>
    CHECK(k->count == 1 && k->notes[0].tick == 24);
    tap(IN_B3); // Note>>
    CHECK(os.step_tick == 24);
    // Shift + SEQ again: the Grid. The wheel picks a row, B1 <Insert> puts its note in at Now; on a note it reads <Delete> and takes it out.
    in = blank();
    in.down = in.held = (1u << IN_SEQ) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_STEP && os.step_grid == 1);
    os.step_row = 0;
    in = blank();
    in.encoder = 3; // row 4
    frame(&in);
    CHECK(os.step_row == 3);
    tap(IN_B1); // <Insert>
    CHECK(k->count == 2);
    int found = 0;
    for (int i = 0; i < k->count; i++) found += k->notes[i].tick == 24 && k->notes[i].note == 3;
    CHECK(found == 1);
    // Inserted at full level. SHIFT + B1 <Ghost>: 48 (a hollow diamond); again <Full>: 127.
    // SHIFT + wheel: Now a tick at a time.
    {
        int at = -1;
        for (int i = 0; i < k->count; i++)
            if (k->notes[i].tick == 24 && k->notes[i].note == 3) at = i;
        CHECK(at >= 0 && k->notes[at].vel == 127);
        struct input sh = blank();
        sh.down = sh.held = (1u << IN_B1) | (1u << IN_SHIFT), sh.shift = 1;
        frame(&sh);
        CHECK(k->notes[at].vel == 48);
        frame(&sh);
        CHECK(k->notes[at].vel == 127);
        sh = blank(), sh.held = 1u << IN_SHIFT, sh.shift = 1, sh.encoder = -7;
        frame(&sh);
        CHECK(os.step_tick == 17 && k->notes[at].vel == 127 && os.step_row == 3); // Now a tick at a time, on a note too
        sh.encoder = 7;
        frame(&sh);
        CHECK(os.step_tick == 24);
    }
    tap(IN_B1); // again: a note there, so <Delete>: out
    CHECK(k->count == 1);
    in = blank();
    in.pad_hit[0] = 70; // a pad on the Grid: its row only, no note
    frame(&in);
    CHECK(os.step_row == 0 && k->count == 1);
    os.step_grid = 0; // the list puts pads' notes in at Now
    for (int st = 0; st < 16; st += 4) { // a beat to show
        os.step_tick = (uint32_t)(st * 24);
        in = blank(), in.pad_hit[0] = 110, frame(&in);
        os.step_tick = (uint32_t)(st * 24 + 48);
        in = blank(), in.pad_hit[1] = 50, frame(&in);
    }
    os.step_grid = 1;
    os.step_tick = 24 * 6, os.step_row = 1;
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "step_grid");
    tap(IN_B3); // Page>>: past the 1-bar end, nowhere to go
    CHECK(os.step_tick == 24 * 6);
    // 16 Levels Tune: a chord on pad A01 (+0 +4 +7) is three rows of the Grid, each its own note.
    os.levels_on = 1, os.levels_type = LEVELS_TUNE, os.levels_pad = 0, os.levels_root = 0;
    os.step_tick = 24 * 8;
    os.step_grid = 0; // entered on the list
    in = blank();
    in.pad_hit[0] = 100, in.pad_hit[4] = 100, in.pad_hit[7] = 100;
    frame(&in);
    os.step_grid = 1;
    int chord = 0;
    for (int i = 0; i < k->count; i++) chord += k->notes[i].tick == 24 * 8 && k->notes[i].note == 0;
    CHECK(chord == 3 && os.step_row == 7);
    int tunes = 0;
    for (int i = 0; i < k->count; i++)
        if (k->notes[i].tick == 24 * 8 && k->notes[i].note == 0) tunes |= 1 << k->notes[i].tune;
    CHECK(tunes == (1 | 1 << 4 | 1 << 7)); // +0 +4 +7, none lost to another's tuning
    os.step_row = 4;
    in = blank();
    if (shot_dir) frame(&in), write_shot(shot_dir, "step_grid_levels");
    tap(IN_B1); // <Delete> on +4: that one out
    chord = 0;
    for (int i = 0; i < k->count; i++) chord += k->notes[i].tick == 24 * 8 && k->notes[i].note == 0 && k->notes[i].tune != 4;
    CHECK(chord == 2 && k->count >= 2);
    tap(IN_B1); // and back in, tuned +4
    chord = 0;
    for (int i = 0; i < k->count; i++) chord += k->notes[i].tick == 24 * 8 && k->notes[i].note == 0 && k->notes[i].tune == 4;
    CHECK(chord == 1);
    // 16 Levels off: the three share A01's row, a music note there, and <Delete> takes them all out.
    os.levels_on = 0, os.step_row = 0;
    if (shot_dir) frame(&in), write_shot(shot_dir, "step_grid_chord");
    tap(IN_B1);
    chord = 0;
    for (int i = 0; i < k->count; i++) chord += k->notes[i].tick == 24 * 8 && k->notes[i].note == 0;
    CHECK(chord == 0);
    // A MIDI track: every note 0-127, opening on the lowest the track plays.
    sq->track = 1;
    sq->sequence[0].track[1].type = TRACK_MIDI;
    seq_add_note(sq, 0, 1, 0, 60, 100, 20), seq_add_note(sq, 0, 1, 0, 64, 100, 20), seq_add_note(sq, 0, 1, 0, 67, 100, 20);
    seq_add_note(sq, 0, 1, 48, 50, 100, 20);
    os.step_tick = 0;
    in = blank();
    in.down = in.held = (1u << IN_SEQ) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in), frame(&in); // to the list and back: the Grid opens again
    CHECK(os.step_grid == 1 && os.step_mtop == 50 && os.step_mnote == 50);
    in = blank();
    in.encoder = 14; // to 64, still on screen
    frame(&in);
    CHECK(os.step_mnote == 64 && os.step_mtop == 50);
    in.encoder = 3; // 67: the list scrolls
    frame(&in);
    CHECK(os.step_mnote == 67 && os.step_mtop == 52);
    if (shot_dir) write_shot(shot_dir, "step_grid_midi");
    tap(IN_B1); // <Delete>: 67 out of the chord
    const struct seq_track *m = &sq->sequence[0].track[1];
    CHECK(m->count == 3);
    sq->track = 0;
    in = blank();
    in.down = in.held = (1u << IN_SEQ) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in), frame(&in);
    // Playing, the Grid follows: the page the play position is on, with a line there.
    CHECK(os.step_grid == 1);
    sq->sequence[0].bars = 2;
    os.step_tick = 0;
    os.step_row = 0;
    seq_add_note(sq, 0, 0, SEQ_PPQ * 4 + SEQ_PPQ, 2, 100, SEQ_PPQ / 2); // notes on the page the play line is on
    seq_add_note(sq, 0, 0, SEQ_PPQ * 4 + SEQ_PPQ * 3, 5, 100, SEQ_PPQ / 4);
    seq_play(sq, 1);
    render_until_tick(SEQ_PPQ * 4 + SEQ_PPQ + 30); // bar 2, past beat 2
    in = blank();
    {
        struct ui w = {.px = px, .now_ms = 1000, .track_led = -1};
        os_frame(&w, &in, &os);
        CHECK(w.wake_ms > 0 && w.wake_ms <= 40); // drawn again as it plays: the page turns by itself
    }
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "step_grid_play");
    seq_stop(sq, 1);
    for (int i = 0; i < 40; i++) render(128);
    sq->sequence[0].bars = 1;
    in = blank();
    in.down = in.held = (1u << IN_SEQ) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.step_grid == 0); // back to the list
    for (int i = 0; i < 40; i++) render(128);
}

// Zero crossings a second, over the middle of a channel.
static double crossings(const float *x, int ch, long n) {
    long a = n / 4, b = n * 3 / 4, c = 0;
    for (long i = a + 1; i < b; i++) c += (x[(i - 1) * ch] < 0) != (x[i * ch] < 0);
    return c / 2.0 / ((b - a) / 44100.0);
}

static void wait_warp(void) {
    struct input idle = blank();
    for (int i = 0; i < 20000 && warp_busy(&os); i++) {
        frame(&idle);
        struct timespec t = {0, 200000};
        nanosleep(&t, 0);
    }
    frame(&idle);
}

static void test_warp(const char *shot_dir) {
    seq_fresh();
    enum { N = 44100 * 2 };
    struct sample *tone = sample_new(N, 2, 44100);
    for (int i = 0; i < N; i++) tone->data[i * 2] = tone->data[i * 2 + 1] = 0.5f * sinf(2 * 3.14159265f * 440 * i / 44100.0f);
    os_pad_load(&os, 0, tone, "Tone");
    os.selected = 0;
    render(16);
    struct input in = blank();
    in.pad_hit[14] = 100, in.shift = 1, in.held = 1u << IN_SHIFT; // Shift + Pad 15
    frame(&in);
    CHECK(os.screen == OS_WARP && os.warp_page == 0 && os.warp_bars == 1); // 2 s: a bar at 120
    // Length 200%: twice as long, the same pitch and level.
    os.warp_cursor = 0;
    in = blank();
    in.encoder = 100;
    frame(&in);
    wait_warp();
    const struct sample *w = os.warp_result;
    CHECK(w && labs(w->frames - 2 * N) <= 2);
    if (w) {
        double f = crossings(w->data, 2, w->frames);
        float r_in = rms(tone->data, N / 4, N * 3 / 4), r_out = rms(w->data, w->frames / 4, w->frames * 3 / 4);
        CHECK(fabs(f - 440) < 4);
        CHECK(fabsf(20 * log10f(r_out / r_in)) < 1.0f);
        if (fabs(f - 440) >= 4 || fabsf(20 * log10f(r_out / r_in)) >= 1.0f) printf("  warp 200%%: %.1f Hz, %.2f dB\n", f, 20 * log10f(r_out / r_in));
    }
    if (shot_dir) write_shot(shot_dir, "warp");
    // Pitch +12 at 100%: the same length, twice the frequency.
    os.warp_pct = 100;
    os.warp_cursor = 1;
    in = blank();
    in.encoder = 12;
    frame(&in);
    wait_warp();
    w = os.warp_result;
    CHECK(w && labs(w->frames - N) <= 2);
    if (w) {
        double f = crossings(w->data, 2, w->frames);
        CHECK(fabs(f - 880) < 8);
        if (fabs(f - 880) >= 8) printf("  warp +12: %.1f Hz\n", f);
    }
    // Drums: a click every quarter second, stretched to 150%, stays a sharp click.
    struct sample *clicks = sample_new(N, 1, 44100);
    memset(clicks->data, 0, sizeof(float) * N);
    for (int c = 0; c < 8; c++)
        for (int i = 0; i < 200; i++) clicks->data[c * 11025 + i] = 0.8f * expf(-i / 30.0f) * (i % 2 ? -1 : 1);
    os_pad_load(&os, 1, clicks, "Clicks");
    os.selected = 1;
    in = blank();
    frame(&in);
    os.warp_pct = 150, os.warp_pitch = 0, os.warp_dirty = 1;
    wait_warp();
    w = os.warp_result;
    CHECK(w && w->channels == 1);
    if (w) {
        float peak = 0;
        for (long i = 0; i < w->frames; i++) peak = fabsf(w->data[i]) > peak ? fabsf(w->data[i]) : peak;
        CHECK(peak > 0.45f); // not smeared into mush
    }
    // <To Pad>: the warped sound replaces the pad's; the original is kept in memory.
    tap(IN_B3);
    CHECK(os.pads[1].s && os.pads[1].s != clicks && os.warp_result == 0 && strcmp(os.pads[1].name, "Clicks-W") == 0);
    int kept = 0;
    for (int i = 0; i < os.nmemory; i++) kept |= os.memory[i].s == clicks;
    CHECK(kept);
    // WARP again: the S-Series Timestretch, the same length change its own way.
    in = blank();
    in.pad_hit[14] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.warp_page == 1);
    os.selected = 0;
    in = blank();
    frame(&in);
    CHECK(os.ts_mode == 1 && os.ts_qual == 90 && os.ts_width == 90 && os.ts_start == 0 && os.ts_end == N); // INTELL 90/90
    // INTELL, Time Factor 50%: half as long, the same pitch.
    os.ts_factor = 50, os.warp_dirty = 1;
    wait_warp();
    w = os.warp_result;
    CHECK(w && labs(w->frames - N / 2) <= 2);
    if (w) CHECK(fabs(crossings(w->data, 2, w->frames) - 440) < 10); // still 440: spliced closer, not sped up
    if (shot_dir) { // without the toast
        os.toast_until = 0;
        in = blank();
        frame(&in);
        write_shot(shot_dir, "timestretch");
    }
    // CYCLIC with <AutoC>: the cycle a whole number of the tone's periods (100.2 samples at 440 Hz).
    os.warp_cursor = 3; // Mode
    in = blank();
    in.encoder = -1;
    frame(&in);
    CHECK(os.ts_mode == 0);
    tap(IN_B2); // <AutoC>
    double periods = os.ts_cycle / (44100.0 / 440);
    CHECK(fabs(periods - round(periods)) < 0.05 && os.ts_cycle > 500);
    os.ts_factor = 200, os.warp_dirty = 1;
    wait_warp();
    w = os.warp_result;
    CHECK(w && labs(w->frames - 2 * N) <= 2);
    if (w) CHECK(fabs(crossings(w->data, 2, w->frames) - 440) < 10);
    if (shot_dir) { // without the toast
        os.toast_until = 0;
        in = blank();
        frame(&in);
        write_shot(shot_dir, "timestretch_cyclic");
    }
    // A zone: only the first half stretched to 200%, the second half as it was.
    os.ts_end = N / 2, os.warp_dirty = 1;
    wait_warp();
    w = os.warp_result;
    CHECK(w && labs(w->frames - (N + N / 2)) <= 2);
    tap(IN_SEQ); // leaving lets the warped sound go
    in = blank();
    frame(&in);
    CHECK(os.warp_result == 0 && os.warp_job == 0);
    for (int i = 0; i < 40; i++) render(128);
}

static void test_prefs(const char *shot_dir) {
    seq_fresh();
    CHECK(os.button_lights == BUTTON_RED && os.pad_lights == PAD_LIGHT_OFF && os.led_style == 1 && lcd_bg == 0x408cfa);
    midi_open(&os);
    tap(IN_B1); // MIDI Settings' B1: Additional Settings
    CHECK(os.screen == OS_PREFS);
    struct input in = blank();
    in.encoder = 1; // Button Lights: WHITE
    frame(&in);
    CHECK(os.button_lights == BUTTON_WHITE && os.led_style == 2);
    os.cursor_prefs = 1; // Pad Lights: from OFF back to WHITE
    in.encoder = -1;
    frame(&in);
    CHECK(os.pad_lights == PAD_LIGHT_WHITE);
    struct ui u = {.px = px, .now_ms = 1000, .track_led = -1};
    struct input idle = blank();
    os_frame(&u, &idle, &os);
    CHECK(u.nled == 16 && u.led[0].r == 127 && u.led[0].b == 127);
    load_pad(0);
    in = blank();
    in.pad_hit[0] = 127, in.pad_down[0] = 1; // pressed hard: dimmer
    struct ui u2 = {.px = px, .now_ms = 1000, .track_led = -1};
    os_frame(&u2, &in, &os);
    int lit = -1;
    for (int i = 0; i < u2.nled; i++)
        if (u2.led[i].index == 0) lit = u2.led[i].r;
    CHECK(lit > 0 && lit < 60);
    os.cursor_prefs = 5; // Seq Res (after Screen): MPC 3000, MPC60, MPC 2000, SP12
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.seq.res == SEQ_RES_MPC60);
    in.encoder = 2;
    frame(&in);
    CHECK(os.seq.res == SEQ_RES_SP1200);
    { // USB Audio Out (after Seq Res): OFF to start, on and off; saved with the settings
        CHECK(os.usb_out == 0);
        os.cursor_prefs = 6;
        struct input w = blank();
        w.encoder = 1;
        frame(&w);
        CHECK(os.usb_out == 1);
        char line[200];
        prefs_save_line(&os, line, sizeof line);
        os.usb_out = 0;
        prefs_load_line(&os, line);
        CHECK(os.usb_out == 1);
        w.encoder = -1;
        frame(&w);
        CHECK(os.usb_out == 0);
    }
    os.cursor_prefs = 3; // Screen: the MPC60's green
    in.encoder = 1;
    frame(&in);
    CHECK(os.screen_scheme == SCREEN_MPC60 && lcd_bg != 0x408cfa);
    if (shot_dir) write_shot(shot_dir, "prefs");
    tap(IN_B3); // <Page 2>
    CHECK(os.cursor_prefs == 11);
    in.encoder = 1; // Loop Conv: IMMEDIATE (the default), AFTER 1ST LOOP
    CHECK(os.loop_conv == LOOP_CONV_IMMEDIATE);
    frame(&in);
    CHECK(os.loop_conv == LOOP_CONV_AFTER);
    // Auto Root: ON to start; OFF and back.
    in = blank();
    in.encoder_press = 1;
    frame(&in);
    CHECK(os.cursor_prefs == 12 && os.auto_root == 1);
    in = blank(), in.encoder = -1;
    frame(&in);
    CHECK(os.auto_root == 0);
    in.encoder = 1;
    frame(&in);
    CHECK(os.auto_root == 1);
    // Sample Rate: the wheel steps the slider's rates, then SET; pushed in on SET, any rate.
    in = blank();
    in.encoder_press = 1;
    frame(&in);
    CHECK(os.cursor_prefs == 13 && os.start_rate == 29760);
    in = blank();
    in.encoder = 1;
    frame(&in);
    CHECK(os.start_rate == 44100 && !os.start_rate_set && os.rec_set[0].rate == 44100 && os.rec_set[5].rate == 44100);
    frame(&in);
    CHECK(os.start_rate_set && os.start_rate == 44100); // SET
    frame(&in);
    CHECK(os.start_rate_set); // the end of the list
    in = blank();
    in.encoder_press = 1;
    frame(&in);
    CHECK(os.rate_edit && os.cursor_prefs == 13); // pushed in: the number, the cursor stays
    in = blank();
    in.encoder = -3;
    frame(&in);
    CHECK(os.start_rate == 43000 && os.rec_set[1].rate == 43000); // 44.1, 44.0, 43.5, 43.0
    if (shot_dir) write_shot(shot_dir, "prefs2");
    in = blank();
    in.encoder_press = 1;
    frame(&in);
    CHECK(!os.rate_edit && os.cursor_prefs == 13);
    in = blank();
    in.encoder = -1; // off SET: back to the slider's rates
    frame(&in);
    CHECK(!os.start_rate_set && os.start_rate == 44100);
    in.encoder = -1;
    frame(&in);
    CHECK(os.start_rate == 29760);
    os.cursor_prefs = 14; // Bit Rate
    frame(&in);
    CHECK(os.start_bits == 8 && os.rec_set[2].bits == 8);
    in.encoder = 1;
    frame(&in);
    os.cursor_prefs = 17; // Input Thru: OFF, then on by hand
    in.encoder = -1;
    frame(&in);
    CHECK(os.start_thru == 0 && os.input_thru == 0);
    in.encoder = 1;
    frame(&in);
    os.cursor_prefs = 18; // Rec Mode
    frame(&in);
    CHECK(os.start_mode == REC_MONO_L && os.rec_set[4].mode == REC_MONO_L && os.rec_set[2].mode == REC_MONO_LR);
    in.encoder = -1;
    frame(&in);
    tap(IN_B3); // <Page 1>
    CHECK(os.cursor_prefs == 0);
    // SP-1200 swing: a 16th at 58% lands 4 ticks late; the six settings only.
    os.seq.tc = TC_16, os.seq.swing = 58;
    CHECK(seq_correct(&os.seq, 24, 0) == 28 && seq_correct(&os.seq, 48, 0) == 48);
    os.seq.swing = 71;
    CHECK(seq_correct(&os.seq, 24, 0) == 34);
    CHECK(seq_swing_step(&os.seq, 58, 1) == 63 && seq_swing_step(&os.seq, 71, 1) == 71 && seq_swing_step(&os.seq, 60, 0) == 63);
    os.seq.tc = TC_OFF;
    CHECK(seq_correct(&os.seq, 13, 0) == 12); // recorded on its 24 PPQN clock
    // Kept with the settings.
    char path[] = "/tmp/claude-mpc-settings-XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    os.settings_path = path;
    os_save_settings(&os);
    os.button_lights = 0, os.pad_lights = PAD_LIGHT_OFF, os.seq.res = 0, os.screen_scheme = 0, os.loop_conv = 0;
    os.start_rate = 44100, os.start_bits = 16, os.start_aa = 1, os.start_mode = REC_STEREO;
    os.rec_set[1].rate = 44100;
    os_load_settings(&os);
    CHECK(os.start_rate == 29760 && os.start_bits == 12 && os.rec_set[1].rate == 29760 && os.rec_set[1].bits == 12);
    CHECK(os.loop_conv == LOOP_CONV_AFTER);
    os.loop_conv = LOOP_CONV_IMMEDIATE;
    CHECK(os.button_lights == BUTTON_WHITE && os.pad_lights == PAD_LIGHT_WHITE && os.seq.res == SEQ_RES_SP1200 &&
          os.screen_scheme == SCREEN_MPC60 && lcd_bg != 0x408cfa);
    unlink(path);
    os.settings_path = 0;
    // Back to the defaults for the rest.
    os.button_lights = BUTTON_RED, os.pad_lights = PAD_LIGHT_OFF, os.seq.res = 0, os.screen_scheme = 0;
    prefs_apply(&os);
    struct ui u3 = {.px = px, .now_ms = 1000, .track_led = -1};
    os_frame(&u3, &idle, &os); // the pads dark again, once
    // Button Lights OFF: every button dark.
    os.button_lights = BUTTON_OFF;
    struct ui u4 = {.px = px, .now_ms = 1000, .track_led = -1};
    os_frame(&u4, &idle, &os);
    int any = 0;
    for (int b = 0; b < IN_N; b++) any |= u4.button_level[b] != 0;
    CHECK(!any);
    os.button_lights = BUTTON_RED;
    prefs_apply(&os);
}

static int file_exists(const char *p) {
    return access(p, F_OK) == 0;
}

static void test_disk(const char *shot_dir) {
    seq_fresh();
    char dir[] = "/tmp/claude-mpc-card-XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    os.data_dir = dir;
    // Something to save: two pads (one sound on both), one in memory, notes, a song, effects.
    struct sample *a = counting(1000), *b2 = constant(500);
    os_pad_load(&os, 0, a, "Kick");
    os_pad_load(&os, 5, a, "Kick2");
    os_pad_load(&os, 1, b2, "Snare");
    os.pads[1].p.volume = 77;
    os.pads[1].sp.cutoff = 33;
    os_memory_add(&os, constant(300), "Spare");
    struct seq *sq = &os.seq;
    seq_add_note(sq, 0, 0, 0, 0, 100, 24);
    seq_add_note(sq, 0, 0, 96, 1, 90, 24);
    seq_add_note(sq, 0, 3, 48, 5, 80, 12);
    sq->sequence[0].tempo10 = 955;
    snprintf(sq->sequence[0].track[3].name, SEQ_NAME, "Hats");
    sq->song[0].nsteps = 2, sq->song[0].step[0].seq = 0, sq->song[0].step[0].reps = 1, sq->song[0].step[1].seq = 0, sq->song[0].step[1].reps = 3;
    atomic_store(&os.master.comp, MCOMP_DBX);
    render(16);
    // Shift + Pad 16: Save/Load, SAVE PROJECT, a name ready.
    struct input in = blank();
    in.pad_hit[15] = 100, in.shift = 1, in.held = 1u << IN_SHIFT;
    frame(&in);
    CHECK(os.screen == OS_DISK && os.disk_mode == DISK_SAVE && os.disk_type == DISK_PROJECT && os.disk_name[0]);
    if (shot_dir) write_shot(shot_dir, "disk_save");
    snprintf(os.disk_name, sizeof os.disk_name, "MYSONG");
    tap(IN_B3); // <Save>
    in = blank();
    frame(&in); // done the frame after
    char p[600];
    snprintf(p, sizeof p, "%s/projects/MYSONG/project.bin", dir);
    CHECK(file_exists(p));
    snprintf(p, sizeof p, "%s/projects/MYSONG/samples/000.wav", dir);
    CHECK(file_exists(p));
    // Saving again under the name asks first.
    tap(IN_B3);
    CHECK(os.disk_ask == 'S');
    tap(IN_B1);
    CHECK(os.disk_ask == 0);
    // Change everything, then load it back.
    os_pad_clear(&os, 1);
    seq_erase_range(sq, 0, -1, 0, 100000, 0);
    sq->sequence[0].tempo10 = 1200;
    sq->song[0].nsteps = 0;
    atomic_store(&os.master.comp, MCOMP_OFF);
    snprintf(p, sizeof p, "%s/projects/MYSONG", dir);
    CHECK(disk_project_load(&os, p) == 0);
    render(16);
    CHECK(os.pads[0].s && os.pads[0].s == os.pads[5].s && os.pads[0].s->frames == 1000 && os.pads[0].s->data[999] == 999);
    CHECK(os.pads[1].s && os.pads[1].p.volume == 77 && os.pads[1].sp.cutoff == 33 && strcmp(os.pads[1].name, "Snare") == 0);
    CHECK(os.nmemory == 1 && os.memory[0].s->frames == 300 && strcmp(os.memory[0].name, "Spare") == 0);
    CHECK(sq->sequence[0].track[0].count == 2 && sq->sequence[0].track[0].notes[1].tick == 96 && sq->sequence[0].track[3].count == 1 &&
          sq->sequence[0].tempo10 == 955 && strcmp(sq->sequence[0].track[3].name, "Hats") == 0);
    CHECK(sq->song[0].nsteps == 2 && sq->song[0].step[1].reps == 3 && atomic_load(&os.master.comp) == MCOMP_DBX);
    // A program into another program; a sample to its own WAV and back.
    snprintf(p, sizeof p, "%s/programs/KIT", dir);
    CHECK(disk_program_save(&os, 0, p) == 0 && disk_program_load(&os, 3, p) == 0);
    CHECK(os.programs[3].pads[1].s && os.programs[3].pads[1].p.volume == 77);
    os.selected = 1;
    disk_open(&os, DISK_SAVE, DISK_SAMPLE);
    CHECK(strcmp(os.disk_name, "Snare") == 0);
    tap(IN_B3);
    in = blank();
    frame(&in);
    snprintf(p, sizeof p, "%s/samples/Snare.wav", dir);
    CHECK(file_exists(p));
    // LOAD: the list, rename, delete (asked first).
    disk_open(&os, DISK_LOAD, DISK_PROJECT);
    in = blank();
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "disk_load");
    CHECK(os.disk_nfiles == 2 && os.disk_isdir[0] == 2 && strcmp(os.disk_files[1], "MYSONG") == 0 && os.disk_file == 1);
    CHECK(os.disk_page == 0);
    tap(IN_B3); // <Browse>: the files, as Load Files
    CHECK(os.disk_page == 1);
    {
        struct input w = blank();
        w.encoder = -1; // the wheel moves the bar, and back
        frame(&w);
        CHECK(os.disk_file == 0);
        w.encoder = 1;
        frame(&w);
        CHECK(os.disk_file == 1);
    }
    if (shot_dir) write_shot(shot_dir, "disk_browse");
    tap(IN_B2); // <Rename>
    CHECK(os.disk_renaming);
    snprintf(os.disk_name, sizeof os.disk_name, "TAKE2");
    tap(IN_B3); // <OK>
    in = blank();
    frame(&in);
    CHECK(os.disk_nfiles == 2 && strcmp(os.disk_files[1], "TAKE2") == 0);
    CHECK(os.disk_page == 1 && os.disk_ask == 0); // back on the files
    {
        struct input sh = blank();
        sh.down = 1u << IN_B2, sh.held = 1u << IN_B2 | 1u << IN_SHIFT, sh.shift = 1;
        frame(&sh); // SHIFT + B2: <Delete>, asked first
    }
    CHECK(os.disk_ask == 'D');
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "disk_delete");
    tap(IN_B3); // <Yes>
    in = blank();
    frame(&in);
    CHECK(os.disk_nfiles == 1 && os.disk_isdir[0] == 2); // only .. left
    // Save/Load opens again on the mode and type last used; the first time, SAVE PROJECT.
    os.disk_used = 0;
    os.screen = OS_MAIN;
    shift_pad16();
    CHECK(os.screen == OS_DISK && os.disk_mode == DISK_SAVE && os.disk_type == DISK_PROJECT);
    disk_open(&os, DISK_LOAD, DISK_MPCPRJ);
    os.screen = OS_MAIN;
    shift_pad16();
    CHECK(os.screen == OS_DISK && os.disk_mode == DISK_LOAD && os.disk_type == DISK_MPCPRJ);
    // Clear all: shown on a load into memory, asked first, then everything emptied before the
    // load; once set it stays set for the next load, until changed back.
    disk_open(&os, DISK_LOAD, DISK_PROGRAM);
    CHECK(os.disk_clear == 0);
    {
        struct input w = blank();
        os.disk_cursor = 4; // DF_CLEAR
        w.encoder = 1;
        frame(&w);
        CHECK(os.disk_clear == 1);
        if (shot_dir) write_shot(shot_dir, "disk_clear");
        disk_open(&os, DISK_LOAD, DISK_PROGRAM); // opened again: still YES
        CHECK(os.disk_clear == 1);
        os.disk_cursor = 4;
        w.encoder = -1;
        frame(&w);
        CHECK(os.disk_clear == 0);
    }
    // An effects chain, out and back in.
    infx_load_preset(&os, 2);
    snprintf(p, sizeof p, "%s/chain.fxc", dir);
    int t2 = atomic_load(&os.fx.type[1]);
    CHECK(infx_chain_save(&os, p, 0) == 0);
    infx_load_preset(&os, 0);
    CHECK(infx_chain_load(&os, p, 0) == 0 && atomic_load(&os.fx.type[1]) == t2 && os.infx_edited);
    infx_load_preset(&os, 0);
    atomic_store(&os.fx.on, 0);
    // TRACK FX: the Track/Mixer chain, saved and loaded while the page is on INPUT; the input
    // chain is left as it is, and the sends come back when Track/Mixer is chosen.
    {
        int studio = -1;
        for (int i = 0; i < infx_presets(); i++)
            if (infx_preset_kind(i) == 4 && strcmp(infx_preset_name(i), "STUDIO") == 0) studio = i;
        os.infx_mode = FXM_TRACK, atomic_store(&os.fx.mode, FXM_TRACK);
        infx_load_preset(&os, studio);
        int track1 = atomic_load(&os.fx.type[0]);
        os.cursor_infx = 0;
        os.screen = OS_INFX;
        in = blank();
        in.encoder = -1; // back to INPUT: the Track chain kept
        frame(&in);
        CHECK(os.infx_mode == FXM_INPUT && track1 != FX_OFF);
        snprintf(p, sizeof p, "%s/mix.tfx", dir);
        CHECK(infx_chain_save(&os, p, 1) == 0);
        int input1 = atomic_load(&os.fx.type[0]);
        os.fx_cfg[FXM_TRACK].type[0] = FX_OFF; // forgotten
        CHECK(infx_chain_load(&os, p, 1) == 0 && atomic_load(&os.fx.type[0]) == input1 && os.fx_cfg[FXM_TRACK].type[0] == track1);
        in.encoder = 1; // Track/Mixer: the loaded sends
        frame(&in);
        CHECK(os.infx_mode == FXM_TRACK && atomic_load(&os.fx.type[0]) == track1);
        in.encoder = -1;
        frame(&in);
        os.screen = OS_DISK;
    }
    // EXPORT: the sequence's two tracks (stereo) and the mix.
    load_pad(0);
    load_pad(1);
    sq->sequence[0].bars = 1;
    snprintf(p, sizeof p, "%s/exports/SEQ01", dir);
    int n = disk_export(&os, -1, 0, 2, 1, p);
    CHECK(n == 3);
    snprintf(p, sizeof p, "%s/exports/SEQ01/Mix.wav", dir);
    struct sample *mix = wav_load(p);
    CHECK(mix && mix->channels == 2 && mix->frames > 44100 * 2 && mix->frames < 44100 * 6);
    float peak = 0;
    for (long i = 0; mix && i < (long)mix->frames * 2; i++) peak = fabsf(mix->data[i]) > peak ? fabsf(mix->data[i]) : peak;
    CHECK(peak > 0.1f);
    if (mix) sample_free(mix);
    snprintf(p, sizeof p, "%s/exports/SEQ01/T04 Hats.wav", dir);
    CHECK(file_exists(p));
    // AUTOLOAD: saved, then found at power-on.
    disk_open(&os, DISK_SAVE, DISK_AUTOLOAD);
    tap(IN_B3);
    in = blank();
    frame(&in);
    snprintf(p, sizeof p, "%s/autoload/project.bin", dir);
    CHECK(file_exists(p));
    os_pad_clear(&os, 1);
    disk_autoload(&os);
    CHECK(os.pads[1].s != 0);
    // Shift + SAMPLE SELECT: Save/Load, on SAVE SAMPLE.
    os.screen = OS_MAIN;
    in = blank();
    in.down = in.held = (1u << IN_SAMPLE_SELECT) | (1u << IN_SHIFT), in.shift = 1;
    frame(&in);
    CHECK(os.screen == OS_DISK && os.disk_mode == DISK_SAVE && os.disk_type == DISK_SAMPLE);
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf %s", dir);
    CHECK(system(rm) == 0);
    os.data_dir = 0;
    render(64);
    // Clear all: everything emptied (last here: the tests above use what is loaded).
    {
        struct sample *x = sample_new(100, 1, 44100);
        os_pad_load(&os, os_pad_slot(&os, 3), x, "Kick");
        os_memory_add(&os, x, "Kick");
        os.seq.sequence[4].bars = 9;
        os.seq.song[2].nsteps = 3;
        disk_clear_all(&os);
        CHECK(!os.pads[3].s && os.nmemory == 0 && os.seq.sequence[4].bars == 2 && os.seq.song[2].nsteps == 0);
    }
}

static void test_swing_tables(void) {
    // The MPC500/2000 manual's table (Open500 supplement, page 13): the swung step's delay in ticks.
    static const int sw[7] = {50, 54, 58, 62, 66, 71, 75};
    static const int m3000_16[7] = {0, 2, 4, 6, 8, 10, 12}, m2000_16[7] = {0, 1, 3, 5, 7, 10, 12};
    static const int m3000_8[7] = {0, 4, 8, 12, 16, 20, 24}, m2000_8[7] = {0, 3, 7, 11, 15, 20, 24};
    static const int m3000_32[7] = {0, 1, 2, 3, 4, 5, 6};
    struct seq q;
    memset(&q, 0, sizeof q);
    int ok = 1;
    for (int i = 0; i < 7; i++) {
        q.swing = sw[i];
        q.res = SEQ_RES_MPC3000;
        ok &= seq_swing_ticks(&q, 24) - 24 == m3000_16[i] && seq_swing_ticks(&q, 48) - 48 == m3000_8[i] &&
              seq_swing_ticks(&q, 12) - 12 == m3000_32[i];
        q.res = SEQ_RES_MPC2000;
        ok &= seq_swing_ticks(&q, 24) - 24 == m2000_16[i] && seq_swing_ticks(&q, 48) - 48 == m2000_8[i];
        if (!ok) printf("  swing %d: 3000 %d/%d 2000 %d/%d\n", sw[i], seq_swing_ticks(&q, 24) - 24, m3000_16[i], 0, m2000_16[i]);
    }
    CHECK(ok);
    // 1/32 swings too, on the TC grid and in Note Repeat.
    q.res = SEQ_RES_MPC3000, q.swing = 66, q.tc = TC_32;
    CHECK(seq_correct(&q, 12, 0) == 16 && seq_correct(&q, 24, 0) == 24);
    q.res = SEQ_RES_MPC2000, q.tc = TC_16;
    CHECK(seq_correct(&q, 24, 0) == 31); // 7.68 ticks late, the fraction dropped
    // The MPC60, as measured: a tick at a time at 1/16 (64 on 6), 1/8 twice that, 1/32 half.
    static const int m60_16[26] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 6, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12};
    q.res = SEQ_RES_MPC60;
    ok = 1;
    for (int s60 = 50; s60 <= 75; s60++) {
        q.swing = s60;
        int d = m60_16[s60 - 50];
        ok &= seq_swing_ticks(&q, 24) - 24 == d && seq_swing_ticks(&q, 48) - 48 == 2 * d && seq_swing_ticks(&q, 12) - 12 == (d + 1) / 2;
    }
    CHECK(ok);
    q.swing = 57; // where the MPC60 and the MPC 3000 formula part: 3 ticks against 4
    CHECK(seq_swing_ticks(&q, 24) - 24 == 3);
    q.res = SEQ_RES_MPC3000;
    CHECK(seq_swing_ticks(&q, 24) - 24 == 4);
}

static void test_export_progress(const char *shot_dir) {
    seq_fresh();
    char dir[] = "/tmp/claude-mpc-card-XXXXXX";
    CHECK(mkdtemp(dir) != 0);
    os.data_dir = dir;
    load_pad(0);
    os.seq.sequence[0].bars = 4;
    seq_add_note(&os.seq, 0, 0, 0, 0, 100, 24);
    seq_add_note(&os.seq, 0, 1, 96, 0, 100, 24);
    disk_open(&os, DISK_EXPORT, DISK_SEQUENCE);
    os.disk_tracks = 2, os.disk_mix = 1;
    tap(IN_B3); // <Export>: "Starting export" shows first
    if (shot_dir) write_shot(shot_dir, "export_wait");
    struct input in = blank();
    frame(&in); // now it runs in the background
    CHECK(disk_busy(&os));
    frame(&in);
    if (shot_dir) write_shot(shot_dir, "export_progress");
    os.screen = OS_MAIN; // nothing moves it off the page while it runs
    frame(&in);
    CHECK(!disk_busy(&os) || os.screen == OS_DISK);
    for (int i = 0; i < 20000 && disk_busy(&os); i++) {
        frame(&in);
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, 0);
    }
    CHECK(!disk_busy(&os));
    char p[700];
    snprintf(p, sizeof p, "%s/exports/SEQ01/Mix.wav", dir);
    CHECK(access(p, F_OK) == 0);
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf %s", dir);
    CHECK(system(rm) == 0);
    os.data_dir = 0;
}

static void test_loop_in_sequence(void) {
    // A looping one-shot at the start of each pass sounds every pass (it does not toggle off).
    seq_fresh();
    load_pad(0);
    os.pads[0].p.loop_on = 1;
    os_pad_push(&os, 0);
    render(16);
    struct seq *sq = &os.seq;
    sq->sequence[0].bars = 1;
    sq->sequence[0].loop_bar = 1;
    seq_add_note(sq, 0, 0, 0, 0, 100, 24);
    seq_play(sq, 1);
    int bar_frames = (int)(4 * 60.0 / (sq->sequence[0].tempo10 / 10.0) * 44100), ok = 1;
    for (int pass = 0; pass < 4; pass++) {
        for (int f = 0; f < bar_frames - 256; f += 128) render(128);
        for (int f = 0; f < 1024; f += 128) render(128); // over the bar line: the new pass's note
        ok &= voices() >= 1;
    }
    CHECK(ok);
    seq_stop(sq, 1);
    engine_kill_all(&e);
    for (int i = 0; i < 40; i++) render(128);
    // Pressed on the pad, it is still a toggle.
    struct input in = blank();
    in.pad_hit[0] = 100;
    frame(&in);
    CHECK(voices() == 1);
    in = blank();
    in.pad_release[0] = 1;
    frame(&in);
    in = blank();
    in.pad_hit[0] = 100;
    frame(&in);
    for (int i = 0; i < 400; i++) render(128);
    CHECK(voices() == 0);
}

static void test_zoomed_knobs(void) {
    seq_fresh();
    load_pad(0); // 44100 frames
    os.screen = OS_SAMPLE, os.selected = 0, os.page = PAGE_TRIM;
    os.pads[0].p.start = 10000;
    os.zoom = 3; // x8: the view is 5512 frames, a knob step 43 of them
    struct input in = blank();
    in.knob_moved[0] = 1, in.knob[0] = 60;
    frame(&in); // first touch: where the knob is, nothing moves
    CHECK(os.pads[0].p.start == 10000);
    in.knob[0] = 62;
    frame(&in);
    CHECK(os.pads[0].p.start == 10000 + 2 * 43); // two fine steps on, no jump to the knob's place
    os.zoom = 0; // whole view: the knob catches up and maps the whole sound again
    in.knob[0] = 63;
    frame(&in);
    CHECK(os.pads[0].p.start == 10086);
}

static void test_s600_automation(void) {
    seq_fresh();
    load_pad(0);
    os.selected = 0;
    os_program_set_s600(&os, 0, 1);
    os.s600_page = 2; // Filter
    s600_open(&os);
    struct input in = blank();
    frame(&in);
    struct seq *sq = &os.seq;
    seq_add_note(sq, 0, 0, 0, 0, 100, 24);
    pthread_mutex_lock(&sq->lock); // overdubbing, part way in
    sq->mode = SEQ_OVERDUB, sq->pos = 96;
    pthread_mutex_unlock(&sq->lock);
    int at = os.pads[0].sp.cutoff * 127 / 100;
    in = blank();
    in.knob_moved[0] = 1, in.knob[0] = at; // K1 Cutoff: caught where it is
    frame(&in);
    in.knob[0] = 30;
    frame(&in);
    const struct seq_track *k = &sq->sequence[0].track[0];
    int found = 0;
    for (int i = 0; i < k->nauto; i++) found |= k->autos[i].param == AUTO_CUTOFF && k->autos[i].value == 30 && k->autos[i].pad == 0;
    CHECK(found);
    // Played back, the S600 sound's cutoff follows.
    seq_stop(sq, 1);
    os.pads[0].sp.cutoff = 100;
    os_pad_push(&os, 0);
    render(16);
    seq_play(sq, 1);
    for (int i = 0; i < 200; i++) render(128);
    int slot = ENGINE_S600 + os_pad_slot(&os, 0);
    CHECK(e.pad[slot].p.cutoff == 30 * 100 / 127);
    seq_stop(sq, 1);
    engine_kill_all(&e);
    for (int i = 0; i < 40; i++) render(128);
}

// ---- The demo piano (DEMO_PIANO=folder): an S600 MULTI program for the card ----
//
// Synthesized, not sampled: each note a struck string, its partials a little sharp as a real
// string's are (stiffness), each decaying faster the higher it is, three strings a fraction of a
// cent apart so they beat, and a short felt-hammer knock at the start. The loud layer is struck
// harder: more partials, brighter. 16 notes, C2 to C7 every major third, two layers each, as
// 12-bit mono at 29.76 kHz.
static struct sample *piano_note(int note, int loud) {
    const int rate = 29760;
    double f0 = note_hz(note), secs = 4.5 - (note - 36) * 0.05;
    if (secs < 1.8) secs = 1.8;
    int n = (int)(secs * rate);
    struct sample *t = sample_new(n, 1, rate);
    double B = 0.00012 * pow(2.0, (note - 36) / 18.0); // inharmonicity: more in the treble
    double t60 = 9.0 * pow(0.5, (note - 36) / 14.0);   // how long the fundamental rings
    double bright = loud ? 1.0 : 0.45;
    uint32_t r = 12345u + (uint32_t)note * 7u + (uint32_t)loud;
    double detune[3] = {1.0, pow(2.0, 0.6 / 1200.0), pow(2.0, -0.45 / 1200.0)};
    double lp = 0;
    for (int i = 0; i < n; i++) {
        double tt = (double)i / rate, x = 0;
        for (int k = 1; k <= 24; k++) {
            double fk = k * f0 * sqrt(1.0 + B * k * k);
            if (fk > rate * 0.45) break;
            double amp = pow(k, -1.1) * exp(-(k - 1) * (1.6 - bright) * 0.35); // a harder hit: more highs
            double decay = exp(-tt * 6.9 / (t60 / (1.0 + 0.35 * (k - 1))));
            for (int st = 0; st < 3; st++) x += amp * decay * sin(2 * M_PI * fk * detune[st] * tt + st * 0.7);
        }
        x /= 3.0;
        if (tt < 0.012) { // the hammer: a soft knock of filtered noise
            r = r * 1664525u + 1013904223u;
            double nz = (double)(r >> 8) / (1 << 24) * 2 - 1;
            lp += 0.25 * (nz - lp);
            x += lp * (0.6 + 0.6 * bright) * (1.0 - tt / 0.012);
        }
        double att = tt < 0.004 ? tt / 0.004 : 1.0;
        t->data[i] = (float)(x * att * (loud ? 0.55 : 0.28));
    }
    float q = 2048.0f; // 12 bits
    for (int i = 0; i < n; i++) t->data[i] = roundf(t->data[i] * q) / q;
    return t;
}

static int demo_piano(const char *dir) {
    seq_fresh();
    os_program_set_s600(&os, 0, PROG_MULTI);
    snprintf(os.programs[0].name, sizeof os.programs[0].name, "DEMO PIANO");
    int pad = 0;
    for (int note = 36; note <= 96; note += 4)
        for (int loud = 0; loud < 2; loud++) {
            char nn[8], name[24];
            multi_note_name(note, nn, sizeof nn);
            snprintf(name, sizeof name, loud ? "Piano %s v2" : "Piano %s", nn);
            os_pad_load(&os, pad++, piano_note(note, loud), name);
        }
    os.programs[0].inst = 0;
    int skipped, got = multi_auto_map(&os, 0, 0, OS_PADS - 1, 0, 0, 2, 1, &skipped);
    multi_apply_template(&os, 0, 1); // Piano
    os_multi_refresh(&os, 0);
    char p[700];
    snprintf(p, sizeof p, "%s/MPC600S/programs/DEMO PIANO", dir);
    int rc = disk_program_save(&os, 0, p);
    printf("demo piano: %d pads, %d keygroups, %d skipped, saved %s (%s)\n", pad, got, skipped, p, rc == 0 ? "ok" : "FAILED");
    // Read back into another program: it must load as it was saved.
    CHECK(rc == 0 && got == 16 && skipped == 0 && disk_program_load(&os, 1, p) == 0);
    CHECK(os.programs[1].s600 == PROG_MULTI && os.programs[1].pads[0].kgs.n == 16 && os.programs[1].pads[0].kgs.kg[0].zone[1] == 2);
    return rc;
}


// ---- The demo project (DEMO_PROJECT=folder): three sequences and a song for the card ----
//
// 80s dance-pop at 105 BPM, written at 210 (twice the resolution): a musical bar is two
// sequence bars, a musical 16th a sequence 8th. In F. Program 1 is empty: its A01, A02 and A03
// are the kick, snare and hi-hat the user loads. Program 2 a saw bass (S600), 3 the demo piano
// (S600 MULTI), 4 a smooth lead and 5 bells (S600, from the S600 waveforms).
#include "s600_waves.h"
#define DM_BAR 768 // a musical bar in ticks at 210
#define DM_16 48   // a musical 16th

static struct seq_note *dm_buf[SEQ_TRACKS];
static int dm_n[SEQ_TRACKS], dm_cap[SEQ_TRACKS];

static void dm_note(int t, int bar, int step, int len16, int pad, int tune, int vel) {
    if (dm_n[t] == dm_cap[t]) dm_cap[t] = dm_cap[t] ? dm_cap[t] * 2 : 256, dm_buf[t] = realloc(dm_buf[t], dm_cap[t] * sizeof **dm_buf);
    struct seq_note *n = &dm_buf[t][dm_n[t]++];
    memset(n, 0, sizeof *n);
    n->tick = (uint32_t)(bar * DM_BAR + step * DM_16);
    int d = len16 * DM_16 - 6;
    n->dur = (uint16_t)(d < 12 ? 12 : d);
    n->note = (uint8_t)pad, n->vel = (uint8_t)vel, n->tune = (int8_t)tune;
}

static int dm_cmp(const void *a, const void *b) {
    const struct seq_note *x = a, *y = b;
    return x->tick < y->tick ? -1 : x->tick > y->tick;
}

static void dm_commit(int sq) {
    for (int t = 0; t < SEQ_TRACKS; t++) {
        if (!dm_n[t]) continue;
        qsort(dm_buf[t], dm_n[t], sizeof **dm_buf, dm_cmp);
        struct seq_track *k = &os.seq.sequence[sq].track[t];
        free(k->notes);
        k->notes = dm_buf[t], k->count = k->cap = dm_n[t];
        dm_buf[t] = 0, dm_n[t] = dm_cap[t] = 0;
    }
}

enum { DT_KICK, DT_SNARE, DT_HAT, DT_BASS, DT_KEYS, DT_LEAD, DT_BELLS };
enum { C_F, C_AM, C_BB, C_C, C_DM, C_GM, C_CSUS };
static const int DM_VOICE[][4] = {{53, 57, 60, 65}, {52, 57, 60, 64}, {53, 58, 62, 65}, {52, 55, 60, 64},
                                  {53, 57, 62, 65}, {55, 58, 62, 67}, {53, 55, 60, 65}};
static const int DM_ROOT[] = {29, 33, 34, 36, 38, 31, 36}; // the bass: F1 .. D2
#define DM_BASS_ROOT 36 // the SAW waveform plays C2 at its own pitch
#define DM_SYN_ROOT 36

static void dm_drums(int bar, int hats16, int kick, int snare, int fill) {
    for (int st = 0; st < 16; st++) {
        if (hats16 == 2 || (hats16 == 1 && st % 2 == 0)) dm_note(DT_HAT, bar, st, 1, 2, 0, st % 4 == 0 ? 104 : st % 2 == 0 ? 84 : 58);
    }
    if (kick) {
        for (int st = 0; st < 16; st += 4) dm_note(DT_KICK, bar, st, 1, 0, 0, st == 0 ? 120 : 108);
        if (kick == 2) dm_note(DT_KICK, bar, 14, 1, 0, 0, 90);
    }
    if (snare) {
        dm_note(DT_SNARE, bar, 4, 1, 1, 0, 112);
        if (fill) {
            dm_note(DT_SNARE, bar, 12, 1, 1, 0, 112), dm_note(DT_SNARE, bar, 13, 1, 1, 0, 70);
            dm_note(DT_SNARE, bar, 14, 1, 1, 0, 90), dm_note(DT_SNARE, bar, 15, 1, 1, 0, 105);
        } else {
            dm_note(DT_SNARE, bar, 12, 1, 1, 0, 115);
        }
    }
}

// Eighths, the root and its octave: the 80s pulse.
static void dm_bass(int bar, int ch, int walk) {
    int r = DM_ROOT[ch];
    for (int st = 0; st < 16; st += 2) {
        int n = st % 4 == 2 ? r + 12 : r;
        if (walk && st == 14) n = r + 7;
        dm_note(DT_BASS, bar, st, 2, 0, n - DM_BASS_ROOT, st % 4 == 0 ? 112 : 92);
    }
}

static void dm_chord(int bar, int step, int len, int ch, int vel) {
    for (int i = 0; i < 4; i++) dm_note(DT_KEYS, bar, step, len, 0, DM_VOICE[ch][i] - 60, vel - i * 4);
}

static void dm_bells(int bar, int ch, int vel) { // eighths up and down the chord, two octaves up
    static const int ORDER[8] = {0, 1, 2, 3, 2, 1, 2, 3};
    for (int i = 0; i < 8; i++) dm_note(DT_BELLS, bar, i * 2, 2, 0, DM_VOICE[ch][ORDER[i]] + 12 - DM_SYN_ROOT, vel - (i % 2) * 12);
}

static void dm_lead(int bar, int step, int len, int note, int vel) {
    dm_note(DT_LEAD, bar, step, len, 0, note - DM_SYN_ROOT, vel);
}

static void dm_tracks(int sq, const char *name, int bars) {
    struct seq_sequence *q = &os.seq.sequence[sq];
    seq_sequence_reset(q, sq);
    snprintf(q->name, sizeof q->name, "%s", name);
    q->bars = bars, q->tempo10 = 2100, q->loop_bar = 1;
    static const char *const NAMES[7] = {"Kick", "Snare", "Hihat", "Saw Bass", "Synth Keys", "Smooth Lead", "Bells"};
    static const int KIT[7] = {0, 0, 0, 1, 2, 3, 4}, LEVEL[7] = {86, 82, 66, 58, 44, 88, 56}, PAN[7] = {0, 0, 12, 0, -12, 0, 18};
    for (int t = 0; t < 7; t++) {
        struct seq_track *k = &q->track[t];
        snprintf(k->name, sizeof k->name, "%s", NAMES[t]);
        k->type = TRACK_DRUM, k->kit = KIT[t], k->level = LEVEL[t], k->pan = PAN[t];
    }
}

static void dm_intro(void) { // 4 musical bars (8 at 210)
    static const int CH[4] = {C_BB, C_C, C_AM, C_CSUS};
    dm_tracks(0, "Intro", 8);
    for (int b = 0; b < 4; b++) {
        dm_drums(b, b < 2 ? 1 : 2, b >= 2, b >= 2, b == 3);
        dm_chord(b, 0, b == 3 ? 8 : 16, CH[b], 92);
        if (b == 3) dm_chord(b, 8, 8, C_C, 96);
        dm_bells(b, CH[b] == C_CSUS ? C_C : CH[b], 96);
        if (b >= 2) dm_bass(b, CH[b] == C_CSUS ? C_C : CH[b], b == 3);
    }
    dm_lead(3, 12, 4, 72, 90); // a pickup into the main
    dm_commit(0);
}

static void dm_main(void) { // 16 musical bars (32 at 210)
    static const int CH[16] = {C_F, C_AM, C_BB, C_C, C_DM, C_AM, C_BB, C_C, C_F, C_AM, C_BB, C_C, C_DM, C_BB, C_GM, C_CSUS};
    dm_tracks(1, "Main", 32);
    for (int b = 0; b < 16; b++) {
        int ch = CH[b] == C_CSUS ? C_C : CH[b];
        dm_drums(b, 2, b % 4 == 3 ? 2 : 1, 1, b == 15);
        // the keys: stabs pushed ahead of the beat
        dm_chord(b, 0, 5, CH[b], 98), dm_chord(b, 6, 3, CH[b], 86), dm_chord(b, 10, 6, CH[b], 92);
        dm_bass(b, ch, b % 4 == 3);
        if (b % 2 == 0) dm_note(DT_BELLS, b, 0, 8, 0, DM_VOICE[ch][3] + 12 - DM_SYN_ROOT, 84);
    }
    // the lead answers in the second half
    dm_lead(9, 8, 4, 76, 86), dm_lead(9, 12, 2, 74, 80), dm_lead(9, 14, 2, 72, 82);
    dm_lead(10, 0, 8, 70, 84);
    dm_lead(11, 8, 8, 67, 80);
    dm_lead(13, 8, 4, 74, 86), dm_lead(13, 12, 4, 72, 82);
    dm_lead(14, 0, 8, 70, 84);
    dm_lead(15, 0, 6, 72, 86), dm_lead(15, 6, 2, 74, 84), dm_lead(15, 8, 8, 76, 90);
    dm_commit(1);
}

static void dm_hook(void) { // 8 musical bars (16 at 210)
    static const int CH[8] = {C_BB, C_C, C_AM, C_DM, C_BB, C_C, C_F, C_C};
    dm_tracks(2, "Hook", 16);
    for (int b = 0; b < 8; b++) {
        dm_drums(b, 2, b % 2 == 1 ? 2 : 1, 1, b == 7);
        dm_chord(b, 0, 14, CH[b], 96), dm_chord(b, 14, 2, CH[(b + 1) % 8], 90);
        dm_bass(b, CH[b], b == 3 || b == 7);
        dm_bells(b, CH[b], 92);
    }
    static const struct { int bar, step, len, note, vel; } MEL[] = {
        {0, 0, 6, 72, 100}, {0, 6, 2, 74, 92}, {0, 8, 6, 77, 104}, {0, 14, 2, 74, 90},
        {1, 0, 4, 76, 100}, {1, 4, 4, 74, 92}, {1, 8, 8, 72, 96},
        {2, 0, 3, 72, 94}, {2, 3, 3, 76, 96}, {2, 6, 2, 76, 88}, {2, 8, 4, 74, 94}, {2, 12, 4, 72, 90},
        {3, 0, 12, 69, 96},
        {4, 0, 6, 72, 100}, {4, 6, 2, 74, 92}, {4, 8, 6, 77, 104}, {4, 14, 2, 79, 96},
        {5, 0, 4, 79, 104}, {5, 4, 4, 77, 94}, {5, 8, 8, 76, 98},
        {6, 0, 6, 77, 100}, {6, 6, 2, 76, 90}, {6, 8, 8, 72, 94},
        {7, 0, 4, 74, 94}, {7, 4, 4, 76, 96}, {7, 8, 8, 72, 92},
    };
    for (size_t i = 0; i < sizeof MEL / sizeof MEL[0]; i++) dm_lead(MEL[i].bar, MEL[i].step, MEL[i].len, MEL[i].note, MEL[i].vel);
    dm_commit(2);
}

static struct sample *dm_wave(int w) {
    struct sample *x = sample_new(S600_WAVE_POINTS, 1, 44100);
    for (int i = 0; i < S600_WAVE_POINTS; i++) x->data[i] = S600_WAVE_DATA[w][i] / 32768.0f;
    return x;
}

static void dm_wave_pad(int program, int pad, int w, const char *name) {
    os_set_program(&os, program);
    os_pad_load(&os, pad, dm_wave(w), name);
    struct os_pad *p = &os.pads[pad];
    p->p.loop = 0, p->p.loop_on = 1;
    p->sp.root = DM_SYN_ROOT;
}

static void dm_programs(void) {
    snprintf(os.programs[0].name, sizeof os.programs[0].name, "DRUMS");
    // 2: SAW BASS. Two saws a few cents apart, through the S600 LPF, mono.
    os_program_set_s600(&os, 1, PROG_S600);
    snprintf(os.programs[1].name, sizeof os.programs[1].name, "SAW BASS");
    dm_wave_pad(1, 0, 1, "SAW");
    {
        struct s600_params *q = &os.pads[0].sp;
        s600_env_template(q, 1, "Synth Bass 1"), s600_env_template(q, 2, "Synth Bass 1");
        q->filter = FILTER_MOOG, q->cutoff = 38, q->reso = 22, q->f_depth = 45;
        q->mono = 1, q->layers = 2, q->l2_pad = 0, q->l2_vol = 75, q->l2_fine = 8;
        os.pads[0].p.volume = 82;
        os_pad_push(&os, 0);
    }
    os.programs[1].inst = 0;
    // 4: SMOOTH LEAD. A saw with a sine an octave up, soft filter, delayed vibrato, a little glide.
    os_program_set_s600(&os, 3, PROG_S600);
    snprintf(os.programs[3].name, sizeof os.programs[3].name, "SMOOTH LEAD");
    dm_wave_pad(3, 1, 0, "SINE");
    dm_wave_pad(3, 0, 1, "SAW");
    {
        struct s600_params *q = &os.pads[0].sp;
        s600_env_template(q, 1, "Synth Lead"), s600_env_template(q, 2, "Synth Lead");
        q->attack = 25, q->release = 260;
        q->filter = FILTER_MOOG, q->cutoff = 52, q->reso = 12, q->f_depth = 18;
        q->lfo_shape = LFO_SINE, q->lfo_rate = 42, q->lfo_delay = 450, q->lfo_pitch = 7;
        q->mono = 1, q->legato = 1, q->porta = 45;
        q->layers = 2, q->l2_pad = 2, q->l2_vol = 55, q->l2_semi = 12;
        os.pads[0].p.volume = 78;
        os_pad_push(&os, 0);
    }
    os.programs[3].inst = 0;
    // 5: BELLS. A sine with a sine far above it, a little off a harmonic: struck metal.
    os_program_set_s600(&os, 4, PROG_S600);
    snprintf(os.programs[4].name, sizeof os.programs[4].name, "BELLS");
    dm_wave_pad(4, 0, 0, "SINE");
    {
        struct s600_params *q = &os.pads[0].sp;
        s600_env_template(q, 1, "Bell"), s600_env_template(q, 2, "Bell");
        q->decay = 1400, q->release = 900;
        q->note_on = 0;
        q->filter = FILTER_OFF, q->cutoff = 100;
        q->layers = 2, q->l2_pad = 0, q->l2_vol = 40, q->l2_semi = 27, q->l2_fine = 30;
        os.pads[0].p.volume = 70;
        os_pad_push(&os, 0);
    }
    os.programs[4].inst = 0;
    // 3: SYNTH KEYS, the demo piano.
    os_set_program(&os, 2);
    os_program_set_s600(&os, 2, PROG_MULTI);
    snprintf(os.programs[2].name, sizeof os.programs[2].name, "SYNTH KEYS");
    int pad = 0;
    for (int note = 36; note <= 96; note += 4)
        for (int loud = 0; loud < 2; loud++) {
            char nn[8], name[24];
            multi_note_name(note, nn, sizeof nn);
            snprintf(name, sizeof name, loud ? "Piano %s v2" : "Piano %s", nn);
            os_pad_load(&os, pad++, piano_note(note, loud), name);
        }
    os.programs[2].inst = 0;
    int skipped;
    multi_auto_map(&os, 0, 0, OS_PADS - 1, 0, 0, 2, 1, &skipped);
    multi_apply_template(&os, 0, 1);
    os_multi_refresh(&os, 2);
    os_set_program(&os, 0);
}

static struct sample *dm_drum(int kind) { // stand-ins for the preview only
    int n = kind == 2 ? 6000 : 16000;
    struct sample *x = sample_new(n, 1, 44100);
    uint32_t r = 99u + (uint32_t)kind;
    double ph = 0, lp = 0, prev = 0;
    for (int i = 0; i < n; i++) {
        double t = i / 44100.0, v;
        r = r * 1664525u + 1013904223u;
        double nz = (double)(r >> 8) / (1 << 24) * 2 - 1;
        if (kind == 0) { ph += 2 * M_PI * (48 + 110 * exp(-t * 30)) / 44100; v = sin(ph) * exp(-t * 7); }
        else if (kind == 1) { ph += 2 * M_PI * 190 / 44100; lp += 0.5 * (nz - lp); v = 0.45 * sin(ph) * exp(-t * 25) + 0.6 * lp * exp(-t * 14); }
        else { v = (nz - prev) * 0.5 * exp(-t * 60); prev = nz; }
        x->data[i] = (float)(v * (kind == 0 ? 0.55 : kind == 1 ? 0.6 : 0.35));
    }
    return x;
}

static int demo_project(const char *dir) {
    seq_fresh();
    dm_programs();
    dm_intro(), dm_main(), dm_hook();
    os.seq.current = 0, os.seq.tc = TC_8; // a musical 16th at 210
    static const int SONG[] = {0, 1, 2, 1, 2, 2};
    os.seq.song[0].nsteps = 6;
    for (int i = 0; i < 6; i++) os.seq.song[0].step[i].seq = (unsigned char)SONG[i], os.seq.song[0].step[i].reps = 1;
    char p[700];
    snprintf(p, sizeof p, "%s/MPC600S/projects/DEMO 105", dir);
    int rc = disk_project_save(&os, p);
    printf("demo project: saved %s (%s)\n", p, rc == 0 ? "ok" : "FAILED");
    for (int sq = 0; sq < 3; sq++) {
        printf("  %-6s %2d bars:", os.seq.sequence[sq].name, os.seq.sequence[sq].bars);
        for (int t = 0; t < 7; t++) printf(" %d", os.seq.sequence[sq].track[t].count);
        printf("\n");
    }
    // Read back: it must load as it was saved.
    seq_fresh();
    CHECK(rc == 0 && disk_project_load(&os, p) == 0);
    CHECK(os.seq.sequence[1].bars == 32 && os.seq.sequence[1].tempo10 == 2100 && os.seq.sequence[1].track[5].count > 0);
    CHECK(os.programs[1].s600 == PROG_S600 && os.programs[2].s600 == PROG_MULTI && !os.programs[0].pads[0].s);
    // A preview with stand-in drums (not in the project): the song, mixed.
    if (getenv("DEMO_PROBE")) { // one keys note, C4, and one lead note, C4
        dm_tracks(5, "Probe", 2);
        dm_note(DT_KEYS, 0, 0, 8, 0, 0, 100);
        dm_note(DT_LEAD, 0, 8, 8, 0, 60 - DM_SYN_ROOT, 100);
        dm_commit(5);
        for (int i = 0; i < 400; i++) render(128);
        disk_export(&os, -1, 5, 2, 0, getenv("DEMO_PROBE"));
    }
    const char *prev = getenv("DEMO_PREVIEW");
    if (prev) {
        os_set_program(&os, 0);
        for (int k = 0; k < 3; k++) os_pad_load(&os, k, dm_drum(k), k == 0 ? "Kick" : k == 1 ? "Snare" : "Hat");
        for (int i = 0; i < 400; i++) render(128); // the engine takes in every pad
        int n = disk_export(&os, 0, 0, getenv("DEMO_STEMS") ? 2 : 0, 1, prev);
        printf("preview: %d file(s) in %s\n", n, prev);
    }
    return rc;
}

int main(int argc, char **argv) {
    if (getenv("MPC_TEST_LAYOUT")) forced_layout = atoi(getenv("MPC_TEST_LAYOUT"));
    if (getenv("DEMO_PROJECT")) return demo_project(getenv("DEMO_PROJECT")) != 0 || failures != 0;
    if (getenv("DEMO_PIANO")) return demo_piano(getenv("DEMO_PIANO")) != 0 || failures != 0;
    test_one_shot();
    test_keymap();
    test_cpu_worst();
    test_reverse();
    test_loop_toggle();
    test_reverse_loop_stays_in_bounds();
    test_gate();
    test_one_shot_ignores_release();
    test_mono_and_poly();
    test_mute_group();
    test_voice_limit();
    test_velocity_and_pan();
    test_filter_removes_highs();
    test_retire_waits_for_audio();
    test_ring_full();
    test_wav();
    test_title(argc > 1 ? argv[1] : 0);
    test_screen(argc > 1 ? argv[1] : 0);
    test_browser(argc > 1 ? argv[1] : 0);
    test_usbmode(argc > 1 ? argv[1] : 0);
    test_arpedit_midi();
    test_no_pad_lights();
    test_seq_sample_accurate();
    test_seq_loop_and_stop();
    test_seq_long_note_next_pass();
    test_seq_correct();
    test_seq_record();
    test_seq_count_in_and_click();
    test_seq_erase_and_catch_up();
    test_seq_midi_track();
    test_seq_next_sequence();
    test_seq_screens(argc > 1 ? argv[1] : 0);
    test_zone_ops(argc > 1 ? argv[1] : 0);
    test_sound_params(argc > 1 ? argv[1] : 0);
    test_click_shape();
    test_metronome_and_count_in(argc > 1 ? argv[1] : 0);
    test_fader_moves(argc > 1 ? argv[1] : 0);
    test_programs(argc > 1 ? argv[1] : 0);
    test_erase_screen(argc > 1 ? argv[1] : 0);
    test_second_press();
    test_tc_toggle();
    test_pick_sound();
    test_undo();
    test_edit_all();
    test_edit_all_one_program();
    test_record_other_program();
    test_master_with_file();
    test_save_name_next();
    test_snd_and_name_erase();
    test_play_retrigger();
    test_step_pad_no_ghost();
    test_sample_erase_rename();
    test_mixer_knob_shake();
    test_usb_outs(argc > 1 ? argv[1] : 0);
    test_chop_recall();
    test_xl_files();
    test_purge_unused();
    test_disk_type_order();
    test_bank_jump();
    test_stereo_enhancer();
    test_knob_fx_padding();
    test_levels_s600_no_held();
    test_levels_s600_seq_no_held();
    test_levels_s600_wild();
    test_levels_s600_bassline();
    test_held_note_log();
    test_program_type();
    test_zones(argc > 1 ? argv[1] : 0);
    test_zones_transient(argc > 1 ? argv[1] : 0);
    test_program_pads(argc > 1 ? argv[1] : 0);
    test_main_slider();
    test_repeat_rate(argc > 1 ? argv[1] : 0);
    test_mixer(argc > 1 ? argv[1] : 0);
    test_track_eq(argc > 1 ? argv[1] : 0);
    test_recorder();
    test_hold_playback();
    test_record_page(argc > 1 ? argv[1] : 0);
    test_audio_track(argc > 1 ? argv[1] : 0);
    test_live_looper(argc > 1 ? argv[1] : 0);
    test_multi(argc > 1 ? argv[1] : 0);
    test_stems(argc > 1 ? argv[1] : 0);
    test_input_select();
    test_resample(argc > 1 ? argv[1] : 0);
    test_fx_all();
    test_print_fx(argc > 1 ? argv[1] : 0);
    test_input_fx(argc > 1 ? argv[1] : 0);
    test_eq_category(argc > 1 ? argv[1] : 0);
    test_loop_screen(argc > 1 ? argv[1] : 0);
    test_s600_engine();
    test_s600_automation();
    test_zoomed_knobs();
    test_loop_in_sequence();
    test_swing_tables();
    test_prefs(argc > 1 ? argv[1] : 0);
    test_disk(argc > 1 ? argv[1] : 0);
    test_export_progress(argc > 1 ? argv[1] : 0);
    test_song(argc > 1 ? argv[1] : 0);
    test_seq_copy(argc > 1 ? argv[1] : 0);
    test_arp_steps(argc > 1 ? argv[1] : 0);
    test_prefs_start();
    test_guards();
    test_pad_bounce();
    test_panel_input();
    test_rename(argc > 1 ? argv[1] : 0);
    test_instrument_stage();
    test_arp_more();
    test_disk_folders(argc > 1 ? argv[1] : 0);
    test_step_edit(argc > 1 ? argv[1] : 0);
    test_warp(argc > 1 ? argv[1] : 0);
    test_master_fx(argc > 1 ? argv[1] : 0);
    test_fx_modes(argc > 1 ? argv[1] : 0);
    test_help_and_locate(argc > 1 ? argv[1] : 0);
    test_track_mute(argc > 1 ? argv[1] : 0);
    test_layout_xl(argc > 1 ? argv[1] : 0);
    test_unmute_all();
    test_knob_modes();
    test_tc_knobs();
    test_seq_files(argc > 1 ? argv[1] : 0);
    test_shift_erase_copy(argc > 1 ? argv[1] : 0);
    test_arpeggiator(argc > 1 ? argv[1] : 0);
    test_s600_lpf();
    test_seq_bars_and_tempo(argc > 1 ? argv[1] : 0);
    test_sample_rec_cycle_and_pages();
    test_s600_legato();
    test_s600_trim(argc > 1 ? argv[1] : 0);
    test_s600_resonance();
    test_s600_track(argc > 1 ? argv[1] : 0);
    test_s600_layers(argc > 1 ? argv[1] : 0);
    test_levels(argc > 1 ? argv[1] : 0);
    test_midi_in_s600();
    test_midi_settings(argc > 1 ? argv[1] : 0);
    test_note_repeat();
    test_onsets();
    test_step_sound();
    test_audio_poll();
    test_shutdown(argc > 1 ? argv[1] : 0);
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
