// Effects, shared by Print Effects (PAD FX: one effect printed onto a pad's sound) and Input
// Effects (KNOB FX: a chain of up to seven on the live input). Each has three controls, set
// 0..127 like the knobs. All run at 44.1 kHz, in stereo, a block at a time.
//
// They are digital models inspired by the units they are named for: each aims for the
// character of the original (how it clips, its EQ, how it moves), not a copy of its circuit.
#pragma once
#include <stdatomic.h>
#include <stddef.h>

enum {
    FX_OFF,
    // Reverbs: Decay, Tone, Mix
    FX_ASR_HALL, FX_ROOM, FX_PLATE, FX_SPRING, FX_SNARE, FX_GATED,
    // Delays
    FX_DUB, FX_DDL, FX_TAPE, FX_ANALOG, FX_PINGPONG, FX_SLAP,
    // Modulation
    FX_JUNO, FX_UNIVIBE, FX_DP4, FX_MXR_PH, FX_SMALLSTONE, FX_FLANGER, FX_RHODES, FX_AUTOWAH,
    // Drive and dynamics
    FX_KLON, FX_OD1, FX_DS1, FX_DYNACOMP, FX_CS3, FX_GATE,
    // Amps and EQ
    FX_BASSMAN, FX_AC30, FX_FENDER_EQ, FX_EQ3,
    // Added later, kept at the end so saved numbers stay valid. Their places in the lists are
    // set by the display order in fx.c.
    FX_WAH, FX_SPEAKER,
    FX_PARAM_EQ, FX_EQ_KICK, FX_EQ_SNARE, FX_EQ_HAT, FX_EQ_CLAP, FX_EQ_TOM, FX_EQ_808,
    FX_VINYL,
    FX_JTM45, FX_FUZZ, FX_JC, FX_DC30, FX_MFC42, FX_B15,
    FX_COUNT
};

// Categories, in the order Print Effects scrolls them: Reverb first, then EQ, then Delay.
enum { FXC_REVERB, FXC_EQ, FXC_DELAY, FXC_MOD, FXC_DRIVE, FXC_AMP, FX_CATS };
const char *fx_cat_name(int c); // "REVERB"
int fx_cat_of(int type);
int fx_cat_first(int c); // the first effect of a category, in display order
// The effect d places on from `type` in display order, staying within category `cat`, or with
// cat < 0 across all of them (FX_OFF before the first). Stops at the ends.
int fx_step(int type, int d, int cat);

struct fx_state;

struct fx {
    int type;
    int p[3];            // the three controls, 0..127
    double bpm;          // for delay times synced to the tempo
    struct fx_state *st; // its buffers, from fx_new
};

const char *fx_name(int type);                 // "JUNO CHORUS"
const char *fx_short(int type);                // "Juno": for naming printed sounds
const char *fx_param_name(int type, int i);    // "Mode"
int fx_param_default(int type, int i);
// A control's value as shown: "I+II", "1/8D", "-6dB", "64".
void fx_param_text(int type, int i, int v, double bpm, char *b, size_t n);
// How long the effect rings on after the sound stops, for printing (AUTO tail).
double fx_tail_seconds(const struct fx *f);

struct fx *fx_new(void);       // its buffers are big enough for any effect. NULL: no memory.
void fx_free(struct fx *f);
void fx_set(struct fx *f, int type); // a type, its controls at their defaults, its state cleared
void fx_reset(struct fx *f);         // clears the state (echoes, reverb), keeps the controls
// Processes frames interleaved stereo in place.
void fx_process(struct fx *f, float *buf, int frames);
// As a send effect (Track FX): only what the effect adds comes back, the dry part taken out,
// so a send level of 0 is silence and the track itself is never doubled.
void fx_process_send(struct fx *f, float *buf, int frames);

// ---- The Input Effects chain ----

#define FX_SLOTS 9

// What the chain takes from the input: one side to both (a guitar in one jack), both, or the
// built-in mic.
enum { FXIN_MONO_L, FXIN_MONO_R, FXIN_STEREO, FXIN_MIC, FXIN_USB, FXIN_INST, FXIN_COUNT }; // USB: a computer's audio, in stereo;
// INST: a guitar or bass straight into the line jack, mono, through the instrument stage
// Where the chain works: on the input (INPUT), as nine send effects for the track mixer
// (TRACK, each slot its own send), or on one pad's sound (PAD).
enum { FXM_INPUT, FXM_TRACK, FXM_PAD, FXM_COUNT };

struct fx_chain {
    atomic_int on;    // Input Effect ON: the input goes through the chain before it is heard or recorded
    atomic_int input; // FXIN_*
    atomic_int mode;  // FXM_*
    atomic_int type[FX_SLOTS], p[FX_SLOTS][3]; // set by the UI thread
    atomic_int bpm10;
    _Atomic float load; // the share of each audio block the chain took, 0..1
    struct fx *slot[FX_SLOTS]; // the audio thread's own, following type and p
    // The instrument stage (Input INST): on, its gain in dB (0..30) and Bright (0..100).
    atomic_int inst, inst_gain, inst_bright;
    float inst_z[8];     // its filters' state (audio thread)
    int inst_set_gain, inst_set_bright; // what the filters were last set for
    float inst_c[2][5];  // the high-pass and the bright peak, as biquads
};

int fx_chain_init(struct fx_chain *c); // 0, or -1 when there is no memory for its buffers
// Audio thread: buf (the input) through the input select and every slot in order; the time it
// took is in c->load. Call only with the chain on.
void fx_chain_process(struct fx_chain *c, float *buf, int frames);
// Audio thread, before anything else when Input is INST: the jack's tip (left) alone, to both
// sides; a high-pass under the instrument's range; Bright, a resonant lift around 3.5 kHz that
// puts back some of what a passive pickup loses into a line input; the gain; and a soft limit,
// so a hard-played note never clips the converter's full scale.
void fx_instrument(struct fx_chain *c, float *buf, int frames);
// Audio thread, after the voices: TRACK, each slot on its send bus, its return added to out;
// PAD, the pad's bus through the chain into out. buses[i] for slot i, buses[FX_SLOTS] the pad.
void fx_chain_buses(struct fx_chain *c, float *const *buses, float *out, int frames);
