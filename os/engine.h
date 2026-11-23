// Sample playback engine. The UI thread sends events. The audio thread renders.
// Nothing here touches a device, so the host tests run the same code.
#pragma once
#include <stdatomic.h>
#include <stdint.h>

#define ENGINE_RATE 44100
#define ENGINE_PROGRAM_PADS 128 // eight banks of sixteen
#define ENGINE_PROGRAMS 24      // as on the MPC3000
// Every pad of every program has its own slot: program * ENGINE_PROGRAM_PADS + pad.
#define ENGINE_PADS (ENGINE_PROGRAM_PADS * ENGINE_PROGRAMS)
// S600 tracks play the same pads through their own settings, kept in a second set of slots
// starting here, so a pad can sound as a drum on one track and an S600 voice on another.
#define ENGINE_S600 ENGINE_PADS
#define ENGINE_SLOTS (ENGINE_PADS * 2)
#define ENGINE_KEY_ANY (-1000) // a note off for every key the pad is playing
#define ENGINE_BUSES 16    // tracks that can have their EQ on at once
#define ENGINE_MAX_BLOCK 1024 // frames engine_render does in one go
#define ENGINE_VOICES 32
#define ENGINE_RING 1024 // room to send a whole program's pads, both DRUM and S600, at once
#define ENGINE_RETIRE_MAX 32
#define ENGINE_SENDS 9    // Track FX: each track's sends to the nine effects
#define ENGINE_TRACKS 100 // track numbers as events carry them: 1 + the track, 0 none

// In the order the Filter pages list them. FILTER_MOOG, S600 LPF: a four-pole transistor
// ladder after the Minimoog Model D's, resonant to self-oscillation.
enum { FILTER_OFF, FILTER_LP2, FILTER_HP2, FILTER_BP2, FILTER_LP4, FILTER_HP4, FILTER_BP4, FILTER_MOOG, FILTER_TYPES };

struct sample {
    float *data; // interleaved, `channels` values per frame
    int32_t frames;
    int channels; // 1 or 2
    int rate;
    int refs; // pads using it, counted by the UI thread. It is freed when the last lets go.
};

// Every field is an int32_t so the parameter table in app.c can address them alike.
struct pad_params {
    int32_t start, end, loop; // frames
    int32_t loop_on, loop_lock, reverse, gate;
    int32_t volume;           // 0..100
    int32_t pan;              // -50..50
    int32_t attack_ms;        // 0 starts at full level
    int32_t decay_ms;         // one-shot: 0 plays to the end. gate: the release time.
    int32_t vel_sens;         // 0..100: how much velocity sets the level
    int32_t semi, fine;       // -36..36 semitones, -100..100 cents
    int32_t mono;             // a new hit cuts this pad's earlier hit
    int32_t mute_group;       // 0 none. A hit cuts other pads in the group.
    int32_t filter;           // FILTER_*
    int32_t cutoff, reso;     // 0..100
    int32_t flt_attack_ms, flt_decay_ms;
    int32_t flt_depth;        // -100..100, six octaves at the extremes
    int32_t also_pad;         // 0 off, else 1 + a pad that sounds with this one
    int32_t decay_end;        // 0: decay begins after the attack. 1: it is timed to end with the sample.
    int32_t vel_attack;       // 0..100: how much a hard hit shortens the attack
    int32_t soft_start_ms;    // 0..5000: how far into the sample the softest hit starts
    // S600 voices (synth 1). attack_ms and decay_ms are Env1's A and D, flt_attack_ms,
    // flt_decay_ms and flt_depth Env2's. With gate (Note On) the note holds at the sustain
    // level until released; without, it plays attack and decay, then goes on to the release.
    int32_t synth;
    int32_t sustain, release_ms;           // Env1 S (0..100) and R
    int32_t flt_sustain, flt_release_ms;   // Env2 S (0..100) and R
    int32_t lfo_rate;                      // 0..100: 0.05 Hz to 20 Hz
    int32_t lfo_shape;                     // LFO_*
    int32_t lfo_delay_ms;                  // fades the LFO in after the note starts
    int32_t lfo_pitch, lfo_filter, lfo_amp; // 0..100 depths
    int32_t porta_ms;                      // glide from the last note, 0 off
    int32_t bend_up, bend_down;            // semitones the bend wheel reaches, 0..24
    int32_t mod_pitch;                     // 0..100: LFO pitch depth the mod wheel adds at full
    // A second layer: another voice on each note, through the same envelopes, filter and LFO,
    // with its own level and tuning, playing the event's s2 with its own trim and loop.
    int32_t layers;                        // 1 or 2
    int32_t l2_volume, l2_semi, l2_fine;
    int32_t l2_start, l2_end, l2_loop, l2_loop_on;
    int32_t legato; // mono: a note played while one is held slides to its pitch, the envelopes carry on
    // The mod wheel, beside its vibrato (mod_pitch): on the filter (LFO: the LFO's movement added;
    // OPEN: the cutoff opened, up to three octaves) and the level (the LFO as tremolo). 0..100 at full.
    int32_t mod_filter, mod_flt_open, mod_level;
};

enum { LFO_TRI, LFO_SINE, LFO_SQUARE, LFO_SAW_UP, LFO_SAW_DOWN, LFO_RANDOM, LFO_SHAPES };

enum { EV_ON, EV_OFF, EV_UPDATE, EV_KILL_ALL, EV_RETIRE, EV_SET_PAD, EV_MIX, EV_EQ, EV_SEND, EV_KEYMAP, EV_INSERTS };
#define ENGINE_INSERTS 8 // tracks that can have insert effects at once (the slots they use are counted by the OS)

// A MULTI S600 instrument's key map, as on the S-series: keygroups across the keyboard, each
// with up to two velocity zones, every zone a sample with its own trim and loop. Notes on an
// S600 slot with a key map take their sample from the zone their note and velocity fall in;
// the instrument's envelopes, filter and LFO are shared. Notes are MIDI numbers, 60 = C4, the
// key the S600 plays a pad at its own pitch.
#define ENGINE_KEYMAPS 64  // MULTI instruments, across every program
#define ENGINE_KM_ZONES 64 // 32 keygroups of two zones
#define ENGINE_KM_STAGE 8  // key maps on their way to the audio thread
struct km_zone {
    const struct sample *s;
    int32_t start, end, loop, loop_on;
    int16_t lo, hi, root;  // the keygroup's notes, and the note the sample plays at its own pitch
    int16_t vlo, vhi;      // the velocities this zone answers (1..127)
    int16_t semi, fine;    // TUNE: semitones and cents
    int16_t level, pan;    // LEVEL 0..100, PAN -50..50
};
struct keymap {
    int n;
    // Key tracking around C4 (60): KEY>PAN at two octaves away (-50..50), KEY>FILTER the
    // cutoff following the key (0..100: 100 one octave a octave), KEY>LEVEL (-100..100:
    // 6 dB an octave at 100), KEY>ENV (0..100: decays and releases halving an octave up at 100).
    int kt_pan, kt_filter, kt_level, kt_env;
    struct km_zone z[ENGINE_KM_ZONES];
};

#define ENGINE_CLICK_PAD (-2) // the metronome's voices

struct event {
    int type, pad, vel;
    int key; // semitones from the pad's own pitch: 16 Levels and S600 notes
    uint32_t seq;
    const struct sample *s;
    const struct sample *s2; // an S600 sound's second layer, or NULL
    // The track mixer: which track played it (1 + its number, 0 none), how far its level is
    // down (0 full, 100 silent) and its pan, added to the pad's.
    int track, track_cut, track_pan;
    int restart; // from the sequencer: a looping one-shot starts over each time, never toggles off
    int glide_ms; // a slide (step patterns): a voice of this pad still sounding glides to the new key instead
    const struct keymap *km; // EV_KEYMAP: the key map to copy (pad: the slot; key: its place, or -1 none)
    int ins[ENGINE_INSERTS]; // EV_INSERTS: the tracks (1 + their number) with insert effects, 0 none
    float eq[3][5]; // EV_EQ: three biquads, b0 b1 b2 a1 a2; track_cut 1 turns the EQ on, 0 off. EV_SEND: the sends, 0..1
    struct pad_params p;
};

struct filter_state {
    float s1[2], s2[2];
};

struct voice {
    int stage; // 0 is free
    int pad;
    int key;      // semitones from the pad's pitch
    int layer;    // 1 for an S600 note's second layer
    int track, track_cut, track_pan; // from the track mixer, as in struct event
    float glide;  // portamento: semitones still to slide, shrinking to 0
    int glide_ms; // a slide's own time, overriding Portamento's while it lasts (0: none)
    double lfo_phase;
    float lfo_ms, lfo_held; // time since the note started; Random's current value
    uint32_t lfo_rand;
    uint32_t age;
    const struct sample *s;
    struct pad_params p;
    double pos;
    int delay; // frames of the first block to stay silent, so a note starts on its exact frame
    float env, attack_step, decay_coef, release_coef, vel_gain;
    float fade; // 1, falling to 0 once fading
    int fading;
    float fenv;
    int fenv_falling;
    struct filter_state f[2]; // two so the four pole types can cascade
    float lad_y[2][4], lad_x[2][4]; // S600 LPF: the ladder's four stages, outputs and last inputs, a channel each
};

// A record of what happened to notes, kept by the audio thread, for finding a note left on:
// the last ENGINE_LOG note starts and releases, legato pitch changes, steals and mono cuts.
#define ENGINE_LOG 400
enum { ELOG_LIVE_ON = 1, ELOG_SEQ_ON, ELOG_LIVE_OFF, ELOG_SEQ_OFF, ELOG_LEGATO, ELOG_STEAL, ELOG_MONO_CUT, ELOG_KILL };
struct engine_log {
    long long frame;
    int kind, pad, key, a, b; // a, b: the voice and more, by kind
};

struct engine {
    struct event ring[ENGINE_RING];
    atomic_uint head, tail; // head: UI thread writes. tail: audio thread writes.
    struct voice voice[ENGINE_VOICES];
    uint32_t age;
    atomic_uint retire_ack;
    uint32_t retire_seq;
    struct {
        struct sample *s;
        uint32_t seq;
    } retired[ENGINE_RETIRE_MAX];
    atomic_uchar playing[ENGINE_SLOTS];
    atomic_int voices;
    _Atomic float peak_l, peak_r;
    // The audio thread's copy of every pad, kept current by EV_SET_PAD and EV_UPDATE,
    // so the sequencer can play pads without reaching into the UI's data.
    struct {
        const struct sample *s, *s2;
        struct pad_params p;
    } pad[ENGINE_SLOTS];
    float last_key[ENGINE_SLOTS]; // audio thread: each slot's last note, for portamento
    unsigned char has_last[ENGINE_SLOTS];
    atomic_llong clock; // frames rendered before the current block: one clock for input and output
    struct engine_log log[ENGINE_LOG];
    atomic_uint log_n; // entries written (the audio thread writes, anyone may read)
    // Track EQ (audio thread): each bus is a track (1 + its number, 0 free) whose notes mix
    // there, then run through its three bands into the outputs.
    struct {
        int track;
        float c[3][5];
        float z[3][2][2]; // each band's state, per channel
        float buf[ENGINE_MAX_BLOCK * 2];
    } bus[ENGINE_BUSES];
    atomic_int bend; // the bend wheel, -8192..8191, for S600 voices
    atomic_int mod;  // the mod wheel, 0..127
    // Runs on the audio thread at the start of each block, after queued events.
    // Set before audio starts and left alone.
    void (*hook)(void *ctx, struct engine *e, int frames);
    void *hook_ctx;
    // Effects on the engine's own sound (Input Effects as Track FX or on a pad). Audio thread:
    // each voice also feeds its track's sends, or (the pad picked) goes to the pad bus instead
    // of the outputs; fx_hook then takes the buses and adds what comes back to out.
    float send[ENGINE_TRACKS][ENGINE_SENDS];
    float fxbus[ENGINE_SENDS + 1][ENGINE_MAX_BLOCK * 2];
    atomic_int sends_on; // Track FX is working: the sends are read
    atomic_int fx_pad;   // the DRUM slot (its S600 slot too) whose voices go to the pad bus, or -1
    void (*fx_hook)(void *ctx, struct engine *e, float *out, int frames);
    void *fx_ctx;
    const struct sample *click[2]; // metronome: normal and accented
    // Insert effects (audio thread): each track with them has its own bus. Its notes play into it
    // at full level and centre; insert_hook runs its effects; then its fader and pan, its sends,
    // and on to its EQ or the outputs. The bus runs every block, so a reverb's tail rings on.
    int ins_track[ENGINE_INSERTS];
    float ins_buf[ENGINE_INSERTS][ENGINE_MAX_BLOCK * 2];
    int trk_cut[ENGINE_TRACKS], trk_pan[ENGINE_TRACKS]; // each track's fader (as track_cut) and pan
    void (*insert_hook)(void *ctx, struct engine *e, int track, float *buf, int frames);
    void *insert_ctx;
    _Atomic float load; // the share of each block engine_render took, 0..1 (peaks, easing down)
    // MULTI instruments: the audio thread's key maps, and which (1 + place) each slot plays.
    struct keymap km[ENGINE_KEYMAPS];
    unsigned char km_of[ENGINE_SLOTS];
    // The UI thread's copies in flight: free once the audio thread has passed their seq.
    struct keymap km_stage[ENGINE_KM_STAGE];
    uint32_t km_stage_seq[ENGINE_KM_STAGE];
    uint32_t km_seq;
    atomic_uint km_ack;
};

void engine_init(struct engine *e);

// UI thread. 0 when queued, -1 when the queue is full.
int engine_send(struct engine *e, const struct event *ev);
int engine_note_on(struct engine *e, int pad, int vel, const struct sample *s, const struct pad_params *p);
int engine_note_off(struct engine *e, int pad);
// The same, `key` semitones from the pad's pitch. A note off with ENGINE_KEY_ANY releases them all.
int engine_note_on_key(struct engine *e, int pad, int vel, int key, const struct sample *s, const struct pad_params *p);
int engine_note_off_key(struct engine *e, int pad, int key);
// With an S600 sound's second layer sample (used when p->layers is 2), for a track (1 + its
// number, or 0) at a mixer level (0..100) and pan (-50..50).
int engine_note_on_layers(struct engine *e, int pad, int vel, int key, const struct sample *s, const struct sample *s2,
                          const struct pad_params *p, int track, int level, int pan);
// The track mixer moved: notes the track is playing follow at once.
int engine_track_mix(struct engine *e, int track, int level, int pan);
// A track's EQ (track is 1 + its number): three biquads (b0 b1 b2 a1 a2, a0 = 1), or off.
int engine_track_eq(struct engine *e, int track, int on, const float c[3][5]);
// A track's nine sends to the Track FX, 0..100 each; 0 sends nothing.
int engine_track_sends(struct engine *e, int track, const unsigned char *levels);
int engine_set_pad_layers(struct engine *e, int pad, const struct sample *s, const struct sample *s2,
                          const struct pad_params *p);
void engine_set_bend(struct engine *e, int value); // -8192..8191, any thread
void engine_set_mod(struct engine *e, int value);  // 0..127
int engine_update(struct engine *e, int pad, const struct pad_params *p);
int engine_kill_all(struct engine *e);
// The tracks (1 + their number) that have insert effects, up to ENGINE_INSERTS; the rest 0.
int engine_set_inserts(struct engine *e, const int *tracks);
// A MULTI instrument's key map for an S600 slot, in place `at` (0..ENGINE_KEYMAPS-1), or km
// NULL for none. Copied: km can change at once. 0 when sent, -1 when it could not be.
int engine_set_keymap(struct engine *e, int slot, int at, const struct keymap *km);
// What the sequencer plays on this pad from now on. s NULL empties it.
int engine_set_pad(struct engine *e, int pad, const struct sample *s, const struct pad_params *p);

// Audio thread, from the block hook. offset is the frame in this block to start on.
void engine_play_pad(struct engine *e, int pad, int vel, int offset);
void engine_release_pad(struct engine *e, int pad);
void engine_play_pad_key(struct engine *e, int pad, int vel, int key, int offset);
void engine_play_pad_mix(struct engine *e, int pad, int vel, int key, int offset, int track, int level, int pan);
// As engine_play_pad_mix, sliding a voice of the pad still sounding to the new key over glide_ms
// (its envelopes carry on), or starting one if none is.
void engine_play_pad_glide(struct engine *e, int pad, int vel, int key, int offset, int track, int level, int pan, int glide_ms);
void engine_release_pad_key(struct engine *e, int pad, int key);
// A recorded fader move: one parameter (the sequencer's AUTO_* order) of a pad, value 0..127.
// Changes what the pad plays from now on, and the notes it is already playing.
void engine_apply_param(struct engine *e, int pad, int param, int value);
void engine_click(struct engine *e, int accent, int offset, int volume); // volume 0..100
// Before audio starts: a recorded click to use instead of the built-in one.
void engine_set_click(struct engine *e, int accent, const struct sample *s);

// UI thread. Stops voices using `s`, then frees it once the audio thread has
// confirmed. Call engine_reap every frame. A sample that cannot be queued is
// left allocated, because freeing it could pull it out from under a voice.
void engine_retire(struct engine *e, struct sample *s);
void engine_reap(struct engine *e);

// Audio thread. Mixes `frames` stereo frames over `out`, as interleaved floats.
void engine_render(struct engine *e, float *out, int frames);
unsigned engine_guard_trips(void); // times a blown-up filter, EQ or mix was silenced and reset

struct sample *sample_new(int32_t frames, int channels, int rate);
void sample_free(struct sample *s);
