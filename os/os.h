// The OS screens. Everything here is driven through os_frame, so the host tests can run it.
#pragma once
#include "engine.h"
#include "files.h"
#include "fx.h"
#include "panel.h"
#include "recorder.h"
#include "seq.h"
#include "usbmode.h"
#include <stdatomic.h>
#include <stdint.h>

#define OS_WAVE_W 304
#define OS_BANKS 8

#define OS_NAME "MPC 600 S"
enum { RENAME_NONE, RENAME_SEQ, RENAME_TRACK, RENAME_PROGRAM, RENAME_SOUND, RENAME_SONG };
enum { PADTPL_DEFAULT, PADTPL_CHROMATIC, PADTPL_SCALE, PADTPL_KINDS }; // Pad Notes templates
enum { INPUT_LINE, INPUT_MIC, INPUT_USB, INPUT_INST, INPUT_SOURCES }; // where sampling and Input Effects take their audio
// (INST, a guitar or bass in the left input, mono through the instrument stage, added last: saved numbers stay)
#define OS_NUMBER "2.0J"
#define OS_VERSION "LA Version " OS_NUMBER " (11/27/26)" // 26 columns: a four-digit year will not fit
#define OS_TITLE_MS 7500

#define OS_POWER_FORCE_MS 15000 // if an orderly shutdown has not finished by then, cut the power

enum { OS_SAMPLE, OS_BROWSER, OS_PROJECT, OS_TITLE, OS_SHUTDOWN, OS_MAIN, OS_TC, OS_METRO, OS_PROGRAM, OS_ERASE, OS_LOOP, OS_S600, OS_LEVELS, OS_MIXER, OS_RECORD, OS_REPEAT, OS_ZONES, OS_EQ, OS_MIDI, OS_CCMAP, OS_PADNOTES, OS_PRINT, OS_INFX, OS_MUTE, OS_HELP, OS_MASTER, OS_SENDS, OS_SONG, OS_STEP, OS_WARP, OS_PREFS, OS_DISK, OS_SEQCOPY, OS_ARPEDIT, OS_MULTI, OS_INSERT, OS_USBMODE, OS_COPYEV, OS_COPYBARS, OS_OUTS };
enum { DISK_SAVE, DISK_LOAD, DISK_EXPORT, DISK_USB, DISK_MODES }; // DISK_USB: USB DATA MODE
enum { DISK_PROJECT, DISK_PROGRAM, DISK_SAMPLE, DISK_SETTINGS, DISK_AUTOLOAD, DISK_FX, DISK_ARP, DISK_TRACKFX, DISK_WAVE,
       DISK_SND,                                       // an Akai .SND: the sound on the selected pad
       DISK_SEQFILE, DISK_MIDI,                        // a sequence on its own; LOAD only: a MIDI file,
       DISK_XLSEQ, DISK_XLPGM, DISK_XLPRJ,             // an MPC 2000XL .ALL or .MID, .PGM or .APS; a folder of both
       DISK_MPC, DISK_MPCPGM, DISK_MPCPRJ,             // the MPC 2500's, together and last
                                                       // an MPC .ALL/.SEQ, an MPC .PGM, an MPC .PRJ
       DISK_SEQUENCE, DISK_SONG,
       DISK_MIX_SEQ, DISK_MIX_SONG, DISK_TYPES }; // the last four: EXPORT's; the mixdowns write only the stereo mix
#define DISK_FILES_MAX 200
enum { BUTTON_RED, BUTTON_WHITE, BUTTON_PINK, BUTTON_OFF, BUTTON_LIGHTS };
// Knob Mode (Additional Settings): how K1-K3 and the slider take over a value.
enum { KNOB_CATCH, KNOB_JUMP, KNOB_SCALE, KNOB_MODES };
enum { PAD_LIGHT_BLUE, PAD_LIGHT_RED, PAD_LIGHT_WHITE, PAD_LIGHT_OFF, PAD_LIGHTS };
enum { MUTE_TRACK, MUTE_TRACK_SOLO, MUTE_PAD, MUTE_PAD_SOLO, MUTE_SEQUENCE, MUTE_TYPES };
enum { LEVELS_VELOCITY, LEVELS_TUNE };
enum { RLEN_SEQ, RLEN_DOUBLE, RLEN_MAX };
// Live Looper: idle; counting in from stopped; waiting for the next pass; recording it; ending; made into a sound; stopped
enum { LP_IDLE, LP_COUNT, LP_WAIT, LP_REC, LP_ENDING, LP_ABORT };
enum { LOOP_CONV_IMMEDIATE, LOOP_CONV_AFTER, LOOP_CONVS }; // Live Loop Conversion
#define LP_TAKES 16 // Live Looper takes <Del Take> can go back through
enum { STEM_DRUMS, STEM_BASS, STEM_VOCALS, STEM_MUSIC, STEM_KINDS };
#define STEMS_MAX_SECONDS 20
struct stem_job;
struct warp_job;
struct disk_job;
struct seq_import;
struct umx_model;
enum { PGM_NONE, PGM_COPY, PGM_SWAP, PGM_DELETE };
enum { MCOMP_OFF, MCOMP_DBX, MCOMP_VINYL, MCOMP_TYPES };
struct mbq {
    float c[5], z[2][2];
};
// Master Effects (Shift + Pad 5): set by the UI thread, run by the audio thread just before the
// outputs. Pultec knobs are kept in tenths (0..100 for 0-10).
struct master {
    atomic_int bypass, eq_on, comp; // comp: MCOMP_*
    atomic_int low_freq, low_boost, low_atten, high_freq, high_boost, bandwidth, atten_sel, high_atten;
    atomic_int thresh, ratio, out_db, overeasy; // dbx 160: dB below full scale, a ratio step, dB, on/off
    atomic_int vinyl[3];                        // Vinyl Sim: Comp, Noise, Wow Flut, 0..127
    _Atomic float gr_meter;                     // the dbx's gain reduction lately, dB
    struct mbq eq[4];                           // audio thread from here
    float ms, gr;
    struct fx *vfx;
};
enum { SEQ_ASK_NONE, SEQ_ASK_BARS, SEQ_ASK_BPM_DOUBLE, SEQ_ASK_BPM_HALF };
enum { SYNC_OFF, SYNC_SEND, SYNC_RECEIVE };
enum { SUS_NORMAL, SUS_DURATION, SUS_FOOT };
enum { FOOT_PLAY, FOOT_REC, FOOT_AUDIO, FOOT_LOOP_NEXT, FOOT_MODES }; // FOOT_LOOP_NEXT: Live Looper, each press the next empty pad
enum { FPOL_AUTO, FPOL_NORMAL, FPOL_INVERTED, FPOL_COUNT }; // the footswitch's polarity // FOOT_AUDIO: <Rec>/<Stop> on Audiotrack Record or Live Looper
enum { MCC_LEVEL, MCC_PAN, MCC_LOW, MCC_MID, MCC_HIGH, MCC_MUTE, MCC_SOLO, MCC_COUNT };
enum { CCT_BANK, CCT_SELECTED };
// The MIDI Settings page, saved to the card with the other settings.
struct midi_settings {
    int ext;             // Ext MIDI: the MIDI IN/OUT jacks
    int usb;             // USB MIDI, with a computer plugged in
    int sync_in;         // Sync RECEIVE follows this port's clock: MIDI_PORT_EXT or MIDI_PORT_USB
    int base;            // Base Chn 1..16: where mixer CC (SELECTED) is read
    int recv;            // Receive Chn: 0 ALL, else 1..16, for notes played on the active track
    int sync;            // SYNC_*
    int local;           // Local Mode: pads play the sounds (on) or only send MIDI (off)
    int thru;            // Soft Thru
    int sustain, foot;   // SUS_*, and FOOT_* when the pedal is a footswitch
    int mixer_cc;        // the mixer answers MIDI CC
    int cc_target;       // CCT_*: channels 1-16 are the bank's strips, or all on Base Chn for the selected one
    int cc[MCC_COUNT];   // the CC each mixer control answers to, or -1 for none
    int foot_chn;        // the footswitch's channel: 0 ANY, else 1..16
    int foot_cc;         // its CC (64, the sustain pedal, to start)
    int foot_pol;        // FPOL_*
};
enum { ZONE_PAD, ZONE_REGIONS, ZONE_TRANSIENT, ZONE_MODES };
enum { ZP_SETUP, ZP_WAIT, ZP_PLAY, ZP_ASSIGN };
#define ZONES_MAX 128
#define OS_UNDO_STEPS 2
struct snapshot;
// What <Do> on the Trim page does to the zone between Zone Start and Zone End.
enum {
    DO_DISCARD, DO_NORMALIZE, DO_EXTRACT, DO_SILENCE, DO_REVERSE, DO_DELETE, DO_COPY, DO_INS_START, DO_INS_END, DO_CONVERT, DO_PURGE,
    DO_COUNT
};
// Function 1 steps Trim, Vol. Function 2 Tune, Play, Envel, Vel Mod. Function 3 Filter, Flt Env.
enum { PAGE_TRIM, PAGE_MIX, PAGE_TUNE, PAGE_PLAY, PAGE_AMP, PAGE_VEL, PAGE_FILTER, PAGE_FLT_ENV, PAGE_COUNT };

#define OS_ZOOM_MAX 12
#define OS_MEMORY 512 // sounds kept in memory without a pad

// A pad's S600 settings, used when an S600 track plays it. Volume, tuning, trim and loop
// are the pad's own (shared with DRUM tracks); everything here is the S600's alone, so
// switching a track between DRUM and S600 changes nothing on either side.
struct s600_params {
    int32_t attack, decay, sustain, release, env1_tpl;              // Env1, the level
    int32_t f_attack, f_decay, f_sustain, f_release, env2_tpl;      // Env2, the filter
    int32_t filter, cutoff, reso, f_depth;
    int32_t lfo_shape, lfo_rate, lfo_delay, lfo_pitch, lfo_filter, lfo_amp;
    int32_t mono, porta, bend_up, bend_down, mod_pitch;
    int32_t note_on; // 1: plays while held, then releases. 0: one-shot, plays its envelope through
    int32_t legato;  // Mono Legato: a note over a held one changes pitch without starting again
    // Layers, like a two-oscillator synth: layer 1 is the pad's own sound, level and tuning;
    // layer 2 another voice through the same envelopes, filter and LFO.
    int32_t layers;                    // 1 or 2
    int32_t l2_pad;                    // 0: the pad's own sample, else 1 + the pad whose sample it plays
    int32_t l2_vol, l2_semi, l2_fine;
    // MULTI: the pad is a multisample instrument, its keygroups (struct s600_kgs) spreading
    // other pads' samples across the keyboard. KEY>PAN, KEY>FILTER, KEY>LEVEL, KEY>ENV as in
    // struct keymap; the instrument template last applied.
    int32_t multi; // (no longer used: an S600 MULTI program's Instrument pad is its instrument)
    int32_t kt_pan, kt_filter, kt_level, kt_env;
    int32_t inst_tpl;
    // ROOT: the note the sample is at its own pitch (60 = C4), with its cents (ROOT FINE puts it in
    // tune): MIDI keys play it from there. Found from its name and pitch when loaded (Auto Root).
    int32_t root, root_fine;
    int32_t ranges_manual; // MULTI: Lo and Hi set by hand (else AUTO: midway between the roots)
    int32_t mod_filter, mod_flt_open, mod_level; // the Mod page: the mod wheel on the filter (LFO or OPEN) and level
};

// A MULTI instrument's keygroups, as on the S3000XL: LO NOTE to HI NOTE play ZONE 1's sample,
// or with ZONE 2 set, ZONE 1 up to VEL SPLIT and ZONE 2 above it. Each zone is a pad of the
// same program (1 + its number, 0 none): its sample, with that pad's trim and loop. ROOT is the
// note the samples play at their own pitch (60 = C4).
#define S600_KGS 32
struct s600_kg {
    int32_t lo, hi, root;
    int32_t semi, fine, level, pan; // TUNE, LEVEL 0..100, PAN -50..50
    int32_t split;                  // VEL SPLIT: ZONE 1 to here, ZONE 2 above (with ZONE 2 set)
    int32_t zone[2];
};
struct s600_kgs {
    int32_t n;
    struct s600_kg kg[S600_KGS];
};

struct os_pad {
    struct sample *s;
    int32_t *onsets; // detected transients, the first is always 0. The fader steps Start through them.
    int nonsets;
    char name[40];
    signed char lo[OS_WAVE_W], hi[OS_WAVE_W];
    struct pad_params p;
    struct s600_params sp;
    struct s600_kgs kgs; // MULTI: the keygroups (kept when MULTI is off, so turning it back on finds them)
};

#define OS_PADS ENGINE_PROGRAM_PADS
#define OS_PROGRAMS ENGINE_PROGRAMS

// A program: a whole pad setup, which sound is on each pad and every pad setting.
enum { PROG_MPC, PROG_S600, PROG_MULTI, PROG_TYPES }; // a program's Type: MPC, S600, S600 MULTI
struct os_program {
    char name[17];
    int s600; // Type (PROG_*): S600 and S600 MULTI play through the pads' S600 settings on any track
    int inst; // S600 and S600 MULTI: the Instrument, the pad MIDI keys play (0..127), or -1 LAST PAD (S600)
    struct os_pad pads[OS_PADS];
};

struct os {
    struct engine *engine;
    struct os_program programs[OS_PROGRAMS];
    int program;          // the program being played and edited: the active DRUM track's
    struct os_pad *pads;  // programs[program].pads
    int screen, bank, selected, page;
    int group_page[3]; // the page each of B1, B2, B3 last showed
    int frame_slot;    // which of the three parameters the wheel edits
    int knob_last[3], knob_caught[3];
    int knob_mode; // KNOB_CATCH (to start), KNOB_JUMP or KNOB_SCALE
    int sounding[16]; // engine pad held under each physical pad, or -1
    int sounding_key[16];
    struct { int id, key; } live_on[32]; // notes started from the pads since they were last all up
    int nlive_on;
    int rec_tune[16];
    // 16 Levels: one sound across the pads, at 16 velocities or (with PAD BANK reaching
    // further) 128 tunings around the root pad.
    int levels_on, levels_type, levels_pad, levels_root, cursor_levels, levels_return;
    int levels_pad_set; // Sound: was picked on the 16 Levels page, so turning it on keeps it
    int edit_all;       // Shift + B3 on the sound screens: an edit goes to every pad with a sound
    int s600_page, s600_cursor; // the S600 screen
    int s600_trim;              // the Sample screen's Trim page is showing as the S600's Trim
    int mixer_track;            // the track mixer's selected channel, 0..SEQ_TRACKS-1
    int mixer_ask;              // the mixer's Reset EQ question is up
    struct midi_settings midi;
    int km_owner[ENGINE_KEYMAPS]; // MULTI: which S600 slot (1 + it) each engine key map is, or 0
    int prog_refresh, prog_inst;  // Program Config: a program (1 + it) whose Type or Inst changed, and its new Inst (+ 2)
    int auto_root;                // Auto Root (Additional Settings 2): a sound's ROOT found when it is loaded
    struct fx_inserts ins;        // the eight insert slots (types and settings the audio thread follows)
    int ins_cursor, ins_sel;      // the Insert page: its field, and which of the track's chain
    int multi_pressed;            // the pad last pressed on the MULTI screen (the instrument stays selected)
    int multi_page, multi_cursor, multi_kg, multi_view; // the MULTI screen: KEYGROUP / KEY TRACK / AUTO MAP, its field, the keygroup
    int map_from, map_to, map_by, map_span, map_zones, map_exact; // AUTO MAP's settings
    // The Auto Sampler: you play each sample asked for (one or two velocity layers each), the
    // takes go onto pads in pitch order and become the S600 MULTI program's instrument. Its
    // settings (Length in tenths of a second, 0 MANUAL; Threshold -61 OFF), then where a run is.
    int as_samples, as_layers, as_len, as_thres, as_norm, as_tpl;
    int as_state, as_at, as_total, as_silent, as_ask, as_clear, as_note_k, as_why;
    int as_root[OS_PADS], as_fine[OS_PADS]; // each sample's note (-1 none found, -2 left out)
    int64_t as_t0;
    struct sample *as_take[OS_PADS];
    int midi_return, cursor_midi, cursor_ccmap, pad_note_sel;
    int cc_learn;               // CC Map <Learn>: 1 + the control waiting for a CC, or 0
    int note_learn;             // Pad Notes <Learn>: the next note in goes to the selected pad
    // Pad Notes <Template>: open, which (PADTPL_*), the scale, its root (0..11) and octave (-1..8).
    int padtpl_open, padtpl_kind, padtpl_scale, padtpl_root, padtpl_oct, cursor_padtpl;
    int sus_down;               // the sustain pedal is down (Normal, To Duration)
    // The footswitch: <Learn> waiting for it; what it last sent (shown on MIDI Settings); and for
    // AUTO polarity, its level and how long each level last lasted (the resting one is longer).
    int foot_learn, foot_known, foot_high, foot_inv;
    int foot_seen, foot_last_ch, foot_last_cc, foot_last_val;
    int64_t foot_t, foot_dur[2];
    int sus_n, sus_rn;          // releases held back until the pedal comes up
    struct { int id, key; } sus_snd[64];
    struct { int note, tune; } sus_rec[64];
    int64_t clk_t0;             // Sync RECEIVE: MIDI clocks counted since then, for the tempo
    int clk_n;
    int eq_return, cursor_eq;   // the EQ page
    int eq_seq;                 // the sequence whose track EQs the engine has, or -1 to send them again
    // Sample New Sound (SAMPLE REC).
    struct recorder rec;
    int input_thru;              // Monitor: Input Thru, on every page
    int input_src;               // Input: INPUT_LINE (the jack), INPUT_MIC (built in) or INPUT_USB (audio from a computer)
    int (*set_input)(int src);   // sets the hardware's input; 0, or -1 when that input is not there. Or NULL
    int usb_missing;             // USB was chosen but the computer's audio is not there: said once
    int inst_gain, inst_bright;  // INST: the instrument stage's gain (0..30 dB) and Bright (0..100)
    int pad_threshold;           // the lightest pad hit that counts (Additional Settings)
    void (*set_pad_threshold)(int velocity); // tells the panel, or NULL
    int usb_out;                  // USB Audio Out: the master mix to a computer (OFF to start)
    // USB individual outs (SHIFT + Pad 9 twice): eight more channels to the computer, a second USB
    // audio device beside the main one; by pad or by track; assigned sounds kept in the main mix too.
    int usb_outs, outs_by_track, outs_keep, outs_sel, cursor_outs;
    int outs_layout;              // (kept for older settings files; the eight are always outputs)
    void (*set_outs_mix)(int mix); // tells the audio side the main mix's pair (1 for 1-2 .. 4 for 7-8, 0 none), or NULL
    void (*set_usb_outs)(int on); // puts the outputs' USB device on or takes it off, or NULL
    int usb_outs_state;           // what that last did: 0 off, 1 on, -1 failed
    void (*set_usb_out)(int on);  // tells the audio side, or NULL
    // Stems (Shift + SAMPLE REC again): one stem of a pad's sound, separated by Open-Unmix.
    int stem_kind, stem_remove;  // STEM_*; 0 EXTRACT (only it), 1 REMOVE (all but it)
    int stem_too_long;           // the 20 second message is up
    struct stem_job *stem_job;   // a separation running in the background, or NULL
    const char *stems_dir;       // where the models are (MPC600S/stems on the card), or NULL
    struct umx_model *stem_model; // the last model loaded, kept for the next run
    int stem_model_kind;
    // Each page's settings: [0] Sample New Sound, [1] Audiotrack Record, [2] Convert Sample.
    struct rec_settings {
        int mode, thres_db;      // REC_*; -60..0 dB, below -60 is OFF (starts at once)
        int bits, rate, pitch, aa, normalize;
        int count_bars, length;  // Audiotrack: 1 or 2 bars of count-in; RLEN_*
        int start_force, what;   // Resample: start at once (else at the threshold); 0 LIVE, 1 SEQUENCE
        int thru_record;         // Resample: Input Thru 0 MUTE, 1 RECORD (in the take)
    } rec_set[7]; // [3]: Stems, which has no recording settings; [4]: Resample; [5]: Live Looper; [6]: Auto Sampler
    int record_page;             // which is shown: 0-1 SAMPLE REC, 2-3 Shift + SAMPLE REC, 4 Resample, 5 Live Looper
    // Live Looper: a take of one pass of the playing sequence, dropped onto the selected pad as it loops.
    int lp_state;                // LP_*
    int lp_slot, lp_track, lp_wraps, lp_latency, lp_pitch;
    int lp_session;              // a take was started since the sequence last started: After Loop applies
    int lp_next;                 // footswitch LOOP NEXT: the pad (+1) armed for the take after this one, or 0
    long long lp_zero;           // the engine clock frame the take's pass begins
    long lp_length;              // the pass in frames
    struct lp_job *lp_job;       // IMMEDIATE: the take made into a sound while it is recorded
    struct lp_job *lp_conv;      // AFTER 1ST LOOP: the take made into a sound while it already plays
    int loop_conv;               // Live Loop Conversion (Additional Settings): LOOP_CONV_*
    struct lp_take {
        int slot, seq, track;
        const struct sample *s;  // what the pad held when the take went on it
    } lp_takes[LP_TAKES];
    int lp_ntakes;
    int at_latency;              // frames from the sequence's start to its sound in the take
    int cursor_record, record_return, rec_count;
    int at_running;              // an audio track take is under way with the sequence
    long at_length;              // its length in frames, or 0 to run until stopped
    int rec_sync;                // the waiting take is an audio track's: a pad also gets a note
    int rec_result_pitch;        // its Pitch: the pad it goes to is tuned down this much
    int rec_result_counted;      // its name used up a number (Sample01, Audio02 ...)
    char rec_result_name[40];
    float rec_meter[2];
    struct sample *rec_result;   // a finished take waiting for a pad, ENTER or <Cancel>
    int rec_previewed;           // <Play> sent it to the engine, so it must be retired, not freed
    long (*mem_available)(void); // bytes free, or NULL to allow a minute
    // MIDI in, a message at a time from either port: 1 with it in m and its port (MIDI_PORT_*)
    // in *from. NULL without a port.
    int (*midi_in_read)(unsigned char m[3], int *from);
    void (*midi_usb_send)(const unsigned char *m, int n); // USB MIDI only (the outputs' message to LA DAW), or NULL
    unsigned char outs_sent[160]; // that message as last sent, to send again only when it changes
    int outs_sent_n, outs_sent_playing;
    int outs_daw; // Additional Settings 3, Outs to DAW: that message sent (ON to start)
    void (*midi_ports)(int ext, int usb); // which ports are switched on, or NULL
    int midi_held[128]; // engine slot + 1 a MIDI note is sounding on, or 0
    int midi_key[128], midi_rec[128]; // its key, and the pad it was recorded as (or -1)
    int full_level;
    int64_t last_stop_ms;
    int64_t title_until;
    char toast[48];
    int64_t toast_until;
    float meter_l, meter_r;
    _Atomic float main_gain;
    const char *audio_status;
    unsigned (*xruns)(void);
    struct files browser;          // Sample Select's folder
    const char *browser_root; // NULL finds the real storage. Tests point it at a folder.
    char browser_dir[FILES_PATH]; // the last folder Sample Select showed
    char home_dir[FILES_PATH];    // where Sample Select opens after power-on, set with Shift + B1
    const char *settings_path;        // the settings file on the card, or NULL to keep nothing
    int browser_cursor;
    struct sample *preview;
    int preview_on;
    int64_t preview_at; // when to play the highlighted file, or 0
    void (*audio_poll)(void); // follows the headphone and line out jacks
    int64_t audio_poll_at;
    int (*power_pressed)(void); // NULL when no power key was found
    void (*power_off)(int force);
    const char *power_status;
    int return_screen;   // where No on the shutdown prompt goes back to
    int64_t shutdown_at; // when Yes was chosen, or 0
    int shutdown_stage;  // 0 message drawn, 1 shutdown requested, 2 power cut requested

    struct seq seq;
    int cursor_play, cursor_tc, cursor_metro, cursor_program, cursor_erase; // the field each screen's wheel edits
    // The Erase screen: which sequence and track (0 is all), the tick range and the picked notes.
    int erase_seq, erase_track;
    uint32_t erase_from, erase_to;
    unsigned char erase_notes[128];
    int erase_count;
    int erase_return;
    int copy_from, copy_to, copy_ask, cursor_copy; // Copy Sequence (Shift+B3 on the main page)
    int copy_return;    // the screen Copy Sequence goes back to
    // Copy Events (SHIFT + ERASE), after the MPC3000's Edit 6: from a sequence and track (0 all),
    // a tick range and the picked notes, to a sequence, track and start; MERGE or REPLACE, copies.
    int ce_seq, ce_track, ce_to_seq, ce_to_track, ce_replace, ce_copies, cursor_ce, ce_return;
    uint32_t ce_from, ce_to, ce_at;
    unsigned char ce_notes[128];
    int ce_count;
    // Copy Bars (B2 on Copy Events), after the MPC3000's Edit 5: COPY, INSERT BLANK or DELETE.
    // Bars count from 1; Before bar is bars + 1 for after the last.
    int cb_mode, cb_seq, cb_first, cb_last, cb_to_seq, cb_before, cb_copies, cb_count, cursor_cb;
    int metro_return;   // the screen the metronome page goes back to
    int repeat_return, cursor_repeat; // the Note Repeat page
    // Pattern Edit: the USER pattern, its selected step, the field, Clear asked, the preview playing.
    int arpedit_slot, arpedit_step, cursor_arpedit, arpedit_ask, arpedit_play;
    int arpedit_turn; // K1's movement not yet a whole step
    int knob_turn[3]; // the Arpeggiator page's knobs: movement not yet a whole step
    // The main page's question about a change to the whole sequence: SEQ_ASK_*, and the bars asked for.
    int seq_ask, ask_bars;
    // Renaming from the main page (Shift + B2): what (RENAME_*), the name so far, the letter being set.
    int rename_what, rename_pos;
    char rename_buf[17];
    int rep_toggle;    // Note Repeat's B keys: 0 HOLD (the rate while held), 1 TOGGLE (on until pressed again)
    // Zone Assign (CHOP): a sound divided into zones, each put on a pad of its own.
    int zone_mode, zone_regions, zone_sens; // ZONE_*; 1..32; transient snap 0 OFF, 1..10
    int zone_phase;                         // ZP_*
    int32_t zone_start[ZONES_MAX];          // where each zone starts; each ends where the next starts
    int zone_count, zone_cur, cursor_zones, zone_return;
    long long zone_clock0;                  // the engine clock when the sound started playing through
    int zone_dest, zone_mgrp;               // the assign prompt: 0 a new program, else 1 + an empty bank; mute group
    struct snapshot *undo[OS_UNDO_STEPS], *redo[OS_UNDO_STEPS]; // oldest first
    int nundo, nredo;
    int32_t zone_onsets[256];
    int zone_nonsets;
    int metro_on_mode;  // METRO_ON or METRO_REC: what Click ON turns back on
    int (*midi_note_in)(void); // 1 when a note came in on MIDI IN since last asked. NULL without a port.
    uint32_t tc_from, tc_to; // Move Existing Notes range, in ticks
    int tc_track;
    unsigned char tc_notes[128]; // the notes Move Existing works on, picked with pads
    int tc_count;                // how many are picked; 0 is ALL
    int tc_last;             // the note value Shift + Pad 10 turns Timing Correct back on with
    int64_t taps[4];
    int ntaps;
    int rec_note[16];  // note being recorded under each physical pad, or -1
    int live_midi[16]; // channel << 8 | note sounding out of MIDI under each pad, or -1
    int erase_pad;     // the pad held with ERASE, or -1
    int erase_used;    // a pad was hit with ERASE held, so letting go of ERASE erases nothing more
    int erase_ask;     // the erase prompt for recorded fader moves: 0 closed, else 1 + the AUTO_* value
    int erase_this;    // whether that value itself has recorded moves (else only <All> is offered)
    int pgm_ask;       // the Program screen's Clear Pgm prompt: the program to empty + 1, or 0 closed
    int pgm_mode;      // PGM_*: a pad command waiting for its other pad, or a pad delete to confirm
    int pgm_target;    // Copy and Swap: the program the other pad is in (the wheel picks it)
    int toast_unused;  // Snd: SHIFT + wheel found no unused sound: said on the next frame
    // Extract's new sound, waiting for a pad to be hit (assign) or ENTER (keep in memory).
    struct sample *extract;
    char extract_name[40];
    // Sounds kept in memory without a pad, as on the MPC3000. Each holds a reference.
    struct {
        struct sample *s;
        char name[40];
    } memory[OS_MEMORY];
    int nmemory;
    int repeat_pad[16]; // pads held while Note Repeat plays them
    int fader_last, fader_caught;
    int do_op;           // the Trim page's selected <Do> action
    int cursor_loop;     // the Loop screen's field: 0 Loop on/off, 1 Loop St, 2 Loop End, 3 Do
    int loop_op;         // the Loop screen's selected <Do> action
    int zoom;            // waveform zoom, 0 shows the whole sample, each step doubles
    struct sample *temp; // audio from Copy Zone->Temp, held for the inserts. Never played.
    int temp_pad;        // the pad temp was copied from: it can only go back into that sample
    // Print Effects (PAD FX): one effect printed onto the selected pad's sound.
    struct fx *print_fx;          // the effect, NULL until first used
    int print_type, print_p[3];   // FX_*, its three controls
    int print_tail;               // 0 AUTO, else tenths of a second
    int cursor_print, print_return;
    struct sample *print_preview; // what <Play> rendered, playing on the engine
    // Input Effects (KNOB FX): a chain of up to FX_SLOTS on the live input.
    struct fx_chain fx;
    int infx_slot, cursor_infx, infx_preset, infx_edited, infx_return;
    int infx_kind;                // the preset Type: guitar, bass, keyboard, other
    int infx_line;                // Input when it is not MIC: FXIN_MONO_L, MONO_R or STEREO
    int64_t fx_save_at;           // when to write the chain to the card, or 0
    int mute_type, cursor_mute, mute_return; // the Track Mute page (MUTE): MUTE_*
    int help_return, help_page;   // Help: the page it is about, and which help page shows
    struct master master;
    int master_page, cursor_master, master_return, master_dirty; // the Master Effects page: 0 EQ, 1 Comp
    // Input Effects' three modes each keep their own chain, so a mix's Track FX are there again
    // after recording through Input FX. The one in use is also in fx (the audio thread's copy).
    struct fx_config {
        int type[FX_SLOTS], p[FX_SLOTS][3];
        int kind, preset, edited;
    } fx_cfg[FXM_COUNT];
    int infx_mode;                // FXM_*
    int infx_pad;                 // PAD: the pad (in the program) the chain is on
    int sends_fx, cursor_sends;   // the mixer's FX Sends page: which effect's sends show
    int song_sel, song_step_sel, cursor_song, song_return; // the Song page: which song, which step
    int song_ask; // <Export> asked: 1 + the sequence it goes into, waiting for <Export> again or <Cancel>
    uint32_t step_tick;           // Step Edit: Now
    int cursor_step, step_return, step_armed;
    int step_grid, step_row;      // Step Edit's Grid view (Shift + SEQ again), and its selected pad row
    int step_mnote, step_mtop;    // the Grid on a MIDI track: the selected note, and the note on the top row
    // Warp: the page (0 Warp, 1 S-Series), its settings, the render under way and its result.
    int warp_page, warp_cursor, warp_return, warp_pad, warp_dirty;
    int warp_pct, warp_pitch, warp_fine, warp_bars;
    long ts_start, ts_end;        // S-Series Timestretch: the zone stretched, in the sample's frames
    int ts_factor, ts_mode, ts_cycle, ts_qual, ts_width; // Time Factor %, 0 CYCLIC / 1 INTELL, Cycle Len, QUAL, WIDTH
    struct warp_job *warp_job;
    struct sample *warp_result;   // the one warped sound, until <To Pad>
    int64_t frame_ms;             // the time this frame started
    int64_t ctl_ms;               // the last frame a K knob or the slider moved
    // The mixer pages: a K knob or the slider moved in the last 300 ms. Turning one can shake the
    // panel into a wheel step or a pad hit; meanwhile those do not pan or pick another channel.
    #define OS_CTL_QUIET_MS 300
    #define os_ctl_busy(s) ((s)->ctl_ms && (s)->frame_ms - (s)->ctl_ms < OS_CTL_QUIET_MS)
    int64_t start_ms;             // the first frame's time
    int layout; // Additional Settings Layout: LAYOUT_* (lcd.h)
    int button_lights, pad_lights, screen_scheme, cursor_prefs; // Additional Settings: BUTTON_*, PAD_LIGHT_*, SCREEN_*
    int start_bpm10, start_bars, start_tc, start_swing; // Additional Settings: the tempo, length, TC and swing at power-on
    // Additional Settings, page 2: what every recording page starts with (Sample New Sound,
    // Audiotrack Record, Convert Sample, Resample, Live Looper), and Input Thru.
    int start_rate, start_bits, start_aa, start_norm, start_thru, start_mode;
    int start_rate_set;           // Sample Rate shows SET (any rate) rather than one of the slider's
    int rate_edit;                // SET's number being changed with the wheel (pushed in)
    int pad_level[16];            // how hard each pad is pressed now (0 up), for the pad lights
    int pads_lit;
    // Save/Load (Shift + Pad 16).
    const char *data_dir;         // MPC600S on the card, or NULL (tests point it at a folder)
    int disk_mode, disk_type, disk_which, disk_cursor, disk_return, disk_pos, disk_file, disk_nfiles;
    int disk_used;                // opened since power-on: it opens again on the mode and type last used
    int disk_clear;               // LOAD: Clear all first (asked each time; stays set until changed back)
    int disk_page;                // LOAD: 0 the settings (Mode, Type, Into, Clear all), 1 the files (as Load Files)
    int disk_snd3000;             // SAVE .SND: as the MPC 3000 writes one (else the 2000XL's)
    int disk_tracks, disk_mix;    // EXPORT: tracks 0 none, 1 mono, 2 stereo; the mix with Master FX
    int disk_ask, disk_pending, disk_renaming; // a question up, an action next frame ('S' 'L' 'E' 'D' 'R')
    char disk_name[40];
    struct disk_job *disk_job;    // an export running in the background, or NULL
    struct seq_import *imp;       // a MIDI or MPC file read, its tracks being chosen; or NULL
    char disk_files[DISK_FILES_MAX][40];
    unsigned char disk_isdir[DISK_FILES_MAX]; // each entry: 0 a saved item, 1 a folder, 2 ".." (up a folder)
    char disk_load_at[512]; // the folder last loaded from (or browsed to, loading): every type's loads open there
    char disk_at[512];    // the folder browsed to, anywhere on the card; "" for the type's own: where saves go and loads come from
    int disk_newdir;      // naming a new folder (the name field holds its name)
    char disk_keep[40];   // the item's name, kept while a folder is named
    char disk_last[DISK_TYPES][512]; // each type's folder when last used, until power-off ("": its own)
    char disk_last_item[DISK_TYPES][40]; // and the item last loaded or saved there, to start on
    char disk_last_saved[DISK_TYPES][40]; // the name each type was last saved under this session: the next save's name
    int mpc_missing; // the last MPC program's samples with no WAV found
    struct { int id, key, channel, note; int64_t until; } aud[16]; // Step Edit's auditions, to be released
    int led_style; // which colours of the two-colour button LEDs light: 0 both, 1 A, 2 B
    void (*set_led_style)(int style);
    const struct usbmode_hooks *usbmode; // USB DATA MODE, or NULL where there is none
    int usbm_op, usbm_ask, usbm_watch;   // the step last started; the "Exit anyway?" question; left while starting
};

int64_t os_playhead(const struct os *s); // where the selected pad's sound is playing (frames), or -1
void os_draw_playhead(struct ui *u, int x, int top, int h); // its line, at screen x
void os_toast(struct os *s, const struct ui *u, const char *text);
// A knob or the slider at `now` (0..127), `last` before (-1: not read yet), on a value at `at`
// (0..127): the position to set the value to, by Knob Mode, or -1 to leave it as it is.
int os_knob_take(const struct os *s, int *caught, int last, int now, int at);

// Programs. The engine slot of a pad in the current program.
int os_pad_slot(const struct os *s, int pad);
// Makes `program` the one played and edited from the pads.
void os_set_program(struct os *s, int program);
// Copies a whole program over another. The samples are shared, not duplicated.
void os_program_copy(struct os *s, int from, int to);
// Empties every pad of a program.
void os_program_clear(struct os *s, int program);
int os_program_sounds(const struct os *s, int program); // pads with a sound
// Pads of the current program.
void os_pad_clear(struct os *s, int slot);
void os_pad_copy(struct os *s, int from, int to); // the sound is shared, the settings copied
void os_pad_swap(struct os *s, int a, int b);
// The same with pad `to` of another program (its own program: as above).
void os_pad_copy_to(struct os *s, int from, int program, int to);
void os_pad_swap_with(struct os *s, int a, int program, int b);
// Every sound in memory, once each: on any pad of any program, or kept without one.
#define OS_SOUND_LIST 4096 // every sound: every pad of every program, and memory
int os_sounds(const struct os *s, struct sample **list, const char **names, int max);
// Gives a pad another sound. A pad that had one keeps its settings, with the trim reset to the
// new sound; an empty one starts from the defaults.
void os_pad_assign(struct os *s, int slot, struct sample *n, const char *name);
void os_pad_step_sound(struct os *s, int slot, int d); // d places on through os_sounds
int os_pad_step_free(struct os *s, int slot, int d); // the same through the sounds no pad plays; 0: none

// Reads the settings file at s->settings_path, if there is one.
void os_load_settings(struct os *s);

void os_init(struct os *s, struct engine *e);
int os_frame(struct ui *u, const struct input *in, void *state);

// Puts a sample on a pad and resets the pad's settings. The pad owns `n`.
void os_pad_load(struct os *s, int slot, struct sample *n, const char *name);
void os_pad_load_over(struct os *s, int slot, struct sample *n, const char *name); // the same, the old sound kept in memory
// Sends a pad of the current program, both its DRUM and S600 settings, to the engine.
void os_pad_push(struct os *s, int slot);
int os_track_type(struct os *s); // the active track's TRACK_*
int os_plays_s600(struct os *s);  // the active track plays as S600: an S600 track, or an S600 program
int s600_env_template(struct s600_params *q, int env, const char *name); // Env1 or Env2 from a template, by name
void os_multi_refresh(struct os *s, int program);
int os_instrument(const struct os *s, int program); // the pad MIDI keys play in an S600 or S600 MULTI program
void os_set_instrument(struct os *s, int program, int pad); // MULTI: its keygroups move with it
void os_auto_root(struct os *s, int slot); // a sound just loaded or recorded: its ROOT from its name and pitch
// MULTI (multi.c)
void multi_open(struct os *s);
void multi_frame(struct os *s, struct ui *u, const struct input *in);
void multi_note_in(struct os *s, int note);
void multi_pad(struct os *s, int slot);
void multi_respan(struct os_pad *p, int extend);
void multi_note_name(int note, char *b, size_t n);
int multi_name_note(const char *name, int *pitch_class);
int multi_detect_pitch(const struct sample *smp, double *note);
int multi_root_of(const struct os_pad *p, int by, int *fine);
int multi_auto_map(struct os *s, int inst, int from, int to, int by, int span, int zones, int extend, int *skipped);
int multi_kg_at(const struct os_pad *p, int note);
int multi_template_count(void);
const char *multi_template_name(int t);
void multi_apply_template(struct os *s, int pad, int t);
void record_open_autosampler(struct os *s); // the Auto Sampler (record.c)
void record_note_in(struct os *s, int note); // a key played: the note the Auto Sampler is asking for // every MULTI instrument's key map to the engine again
void os_program_set_s600(struct os *s, int program, int on); // its Type, also told to the sequencer
// Keeps a sound in memory without a pad. The pool takes a reference. 0, or -1 when full.
int os_memory_add(struct os *s, struct sample *n, const char *name);
int os_purge_unused(struct os *s); // the sounds in memory no pad plays, deleted (undoable): how many

// Sample New Sound, in record.c.
void record_defaults(struct os *s);
void record_apply_start(struct os *s); // the Start recording settings onto every recording page
int record_rates(int *out);            // every rate the wheel can reach, lowest first
int record_classic(int *out);          // the rates the slider jumps between
void record_open(struct os *s);
void record_open_convert(struct os *s); // Shift + SAMPLE REC: Convert Sample
void record_open_resample(struct os *s); // Shift + Pad 11: Resample
void record_frame(struct os *s, struct ui *u, const struct input *in);
void looper_service(struct os *s, struct ui *u); // every frame, on any page: the Live Looper's take
void record_foot(struct os *s, struct ui *u);    // the footswitch as AUDIO REC: <Rec> or <Stop> there
void record_foot_next(struct os *s, struct ui *u); // the footswitch as LOOP NEXT: a take on the next empty pad
int record_pad(struct os *s, struct ui *u, int slot); // 1 when the pad took the finished take

// Undo and Redo, in undo.c. Call os_undo_save just before an audio command or a change to
// the sequences, with a word or two for what it is ("Discard").
void os_undo_save(struct os *s, const char *label);
// Pattern Edit (arpedit.c), for the arpeggiator's USER step patterns.
#define ARPEDIT_SILENT (-1000)
void arpedit_open(struct os *s, int slot);
void arpedit_frame(struct os *s, struct ui *u, const struct input *in);
int arpedit_pad(struct os *s, int i); // the semitones to sound the selected pad at, or ARPEDIT_SILENT
int arpedit_key(struct os *s, int key); // a MIDI key: its note at the step; as arpedit_pad
void arpedit_leave(struct os *s);     // the preview stops
void os_undo(struct os *s, struct ui *u);
void os_undo_clear(struct os *s); // every step let go
void os_redo(struct os *s, struct ui *u);
// For undo.c: a pad's engine slots brought up to date, and a reference let go.
void os_slot_refresh(struct os *s, int program, int slot);
void os_sample_release(struct os *s, struct sample *x);

// Track Mute (MUTE), in mute.c, and Help (Shift + SAMPLE REC, Shift + SEQ REC), in help.c.
void mute_open(struct os *s);
void mute_frame(struct os *s, struct ui *u, const struct input *in);
void mute_pad(struct os *s, struct ui *u, int i); // a pad on the page, 0..15
void mute_unmute_all(struct os *s, struct ui *u);  // SHIFT + MUTE: every track unmuted
int help_open(struct os *s, int page); // 1 when the current page has help
void help_frame(struct os *s, struct ui *u, const struct input *in);
int help_pages(struct os *s, int screen, int page, int *small); // tests
void help_dump(struct os *s, int screen); // tests
// Shift + SAMPLE REC (back) and Shift + SEQ REC (on): the main page's play position, a bar.
void seq_locate_bar(struct seq *sq, int dir);

// Song (song.c), Step Edit (step.c), Warp (warp.c).
void song_open(struct os *s);
void song_frame(struct os *s, struct ui *u, const struct input *in);
void song_pad(struct os *s, struct ui *u, int i);
void step_open(struct os *s);
void step_frame(struct os *s, struct ui *u, const struct input *in);
void step_grid_pad(struct os *s, int note, int tune, int vel); // the Grid: a pad selects its row
void step_pad(struct os *s, int note, int tune, int vel);
void warp_open(struct os *s);
void warp_frame(struct os *s, struct ui *u, const struct input *in);
void warp_close(struct os *s);
int warp_busy(const struct os *s);
// A note heard for a moment (Step Edit stepping onto it), as the active track would play it.
void os_audition(struct os *s, int note, int tune, int vel);

// Save/Load, in disk.c.
void disk_open(struct os *s, int mode, int type);
void usbmode_open(struct os *s, struct ui *u); // USB DATA MODE (usbmode.c)
void usbmode_frame(struct ui *u, struct os *s, const struct input *in);
void disk_frame(struct os *s, struct ui *u, const struct input *in);
int disk_naming(const struct os *s); // the Save/Load name field is being set
// ERASE while naming: the letter at the cursor (past the name's end, the one before it); all, the whole name.
void os_name_erase(char *name, int *pos, int all);
void disk_autoload(struct os *s); // power-on: the autoload project, if one was saved
void disk_install_waves(struct os *s); // power-on: the S600 waveforms into MPC600S/s600, any missing
int disk_project_save(struct os *s, const char *dir);
int disk_project_load(struct os *s, const char *dir);
void disk_clear_all(struct os *s); // every program, sound, sequence and song emptied, as at power-on
void disk_new_project(struct os *s); // LOAD's <New>: Clear all and the rest of a project as at power-on, settings kept
int disk_program_save(struct os *s, int program, const char *dir);
int disk_program_load(struct os *s, int program, const char *dir);
int disk_seq_save(struct os *s, int which, const char *path); // one sequence, all its tracks
// An MPC 1000 / 2500 / 500 program (.PGM) and its samples (WAVs beside it) into `program`.
// Returns the pads loaded, or -1; *layers: pads whose extra layers were left out.
int disk_mpc_program_load(struct os *s, int program, const char *path, int *layers);
int disk_find_sound(const char *dir, const char *name, char *out, size_t n); // NAME.WAV, else NAME.SND, any case
// MPC 2000XL files (xl.c). Loads: the pads loaded (-1 not one); saves: 0, or -1.
extern const unsigned char XL_PAD_NOTES[64]; // its pads A01..D16: the notes it gives them
int disk_xl_program_load(struct os *s, int to, const char *path);
int disk_xl_aps_load(struct os *s, const char *path, int *programs);
int disk_xl_pgm_save(struct os *s, int prog, const char *path, int *dropped);
int disk_xl_aps_save(struct os *s, const char *path, const char *name, int *programs, int *dropped);
int disk_xl_all_save(struct os *s, const char *path, int *sequences, int *dropped);
int disk_xl_mid_save(struct os *s, int which, const char *path, int *dropped);
int disk_xl_project_save(struct os *s, const char *dir, int *programs, int *sequences, int *dropped);
int disk_xl_project_load(struct os *s, const char *dir, int *programs, int *sequences);
int os_seq_in_use(struct os *s, int which); // any notes or moves in it
// An MPC 1000 / 2500 / 500 project (.PRJ: the list of its files): each program into the next empty program, then
// each sequence of its .ALL into the next empty sequence. Returns 0, or -1; how many of each went in.
int disk_mpc_project_load(struct os *s, const char *path, int *programs, int *sequences);
// The project as an MPC 2500 one, into folder `dir`: NAME.PRJ, ALL_SEQS&SONGS.ALL, a .PGM for each
// program with sounds, a WAV for each sample. Returns 0 or -1; *dropped: pads, tracks or notes an
// MPC has no room for (pads past D16, tracks past 64).
int disk_mpc_project_save(struct os *s, const char *dir, const char *name, int *programs, int *sequences, int *dropped);
int disk_seq_load(struct os *s, int which, const char *path); // into sequence `which`, replacing it
int disk_export(struct os *s, int song, int which, int tracks, int mix, const char *dir); // files written
int disk_busy(const struct os *s); // an export is running

// Additional Settings, in prefs.c.
void prefs_open(struct os *s);
void prefs_frame(struct os *s, struct ui *u, const struct input *in);
void prefs_apply(struct os *s);
void prefs_pad_lights(struct os *s, struct ui *u);
void prefs_save_line(const struct os *s, char *b, size_t n);
void prefs_load_line(struct os *s, const char *line);
void prefs_startup(struct os *s); // at power-on: Start BPM, TC and Swing into the sequencer
const char *seq_tc_name(int tc);  // "1/16 NOTE"

// Master Effects, in master.c.
void master_init(struct master *m);
void master_defaults(struct master *m);
void master_process(struct master *m, float *buf, int frames); // audio thread
void master_open(struct os *s);
void master_frame(struct os *s, struct ui *u, const struct input *in);
void master_save_line(const struct master *m, char *b, size_t n);
void master_load_line(struct master *m, const char *line);
// The mixer's FX Sends page (Shift + B3 on the mixer), in fxpage.c.
void sends_open(struct os *s);
// Insert effects (SHIFT + B3 on FX Sends): the selected channel's chain, from the eight slots.
void inserts_open(struct os *s);
void inserts_frame(struct os *s, struct ui *u, const struct input *in);
void inserts_push(struct os *s);
void fx_insert_hook(void *ctx, struct engine *e, int track, float *buf, int frames);                        // the tracks with inserts, to the engine
void inserts_save_line(const struct os *s, char *b, size_t n);
void inserts_load_line(struct os *s, const char *line);
int inserts_used(const struct os *s);                   // insert slots in use, of FX_INSERT_SLOTS
void sends_frame(struct os *s, struct ui *u, const struct input *in);
void os_sends_push(struct os *s); // every track's sends to the engine, and whether they work

// Print Effects and Input Effects, in fxpage.c.
void fx_pages_init(struct os *s);
void print_open(struct os *s);
void print_frame(struct os *s, struct ui *u, const struct input *in);
void infx_open(struct os *s);
void infx_frame(struct os *s, struct ui *u, const struct input *in);
void infx_power_on(struct os *s);       // Input Effects off and as new, at power-on
void infx_cursor_effect(struct os *s); // the cursor on Effect #, as KNOB FX pressed again puts it
int infx_presets(void);
const char *infx_preset_name(int i);
int infx_preset_kind(int i);
void infx_follow_input(struct os *s);
void infx_wah_pedal(struct os *s, int value); // the mod wheel or the slider
void infx_pad_pressed(struct os *s, int slot); // PAD: a pad pressed on the page takes the chain
// An effects chain to a file and back: track 0 the input chain (INPUT, or PAD when that is in
// use), track 1 the Track/Mixer chain (its nine send effects), whichever the page is on.
int infx_chain_save(struct os *s, const char *path, int track);
int infx_chain_load(struct os *s, const char *path, int track);
void fx_engine_hook(void *ctx, struct engine *e, float *out, int frames); // Track FX and Pad FX, for an engine
void infx_load_preset(struct os *s, int i);
void infx_save_line(const struct os *s, char *b, size_t n); // the chain, for settings.txt
void infx_load_line(struct os *s, const char *line);

// Track EQ, in eq.c.
int eq_gain_max(int type); // tenths of a dB
void eq_defaults(struct track_eq *q, int type);
void eq_freq_text(int hz, char *b, size_t n);
void eq_coeffs(const struct track_eq *q, float c[3][5]);
void os_eq_push(struct os *s, int track);
void os_eq_push_all(struct os *s); // every track of the current sequence
void eq_open(struct os *s);
void eq_frame(struct os *s, struct ui *u, const struct input *in);

// MIDI, in midi.c: its settings and pages, the mixer under MIDI CC.
void midi_defaults(struct midi_settings *m);
void midi_cc_defaults(struct midi_settings *m);
void midi_open(struct os *s);
void midi_frame(struct os *s, struct ui *u, const struct input *in);
void ccmap_frame(struct os *s, struct ui *u, const struct input *in);
void ccmap_learned(struct os *s, const struct ui *u, int cc); // a CC came in while <Learn> waited
void padnotes_frame(struct os *s, struct ui *u, const struct input *in);
void padnotes_apply(struct os *s); // the Pad Notes template chosen, onto every pad
int padnotes_scales(void);
int padnotes_pad(struct os *s, struct ui *u, int slot); // 1 when the hit picked a pad to set
void seq_mixer_cc(struct os *s, int track, int control, int value); // MCC_*, 0..127
int os_save_settings(const struct os *s);

// Zone Assign, in zones.c.
void zones_defaults(struct os *s);
void zones_open(struct os *s);
void zones_frame(struct os *s, struct ui *u, const struct input *in);
int zones_pad(struct os *s, struct ui *u, int slot); // 1 when the hit marked a zone or started the sound

// The S600 screen and 16 Levels, in s600.c.
void s600_defaults(struct s600_params *q);
// What an S600 track plays on pad i of a program's pads, and its second layer's sample (or NULL).
void s600_effective(const struct os_pad *pads, int i, struct pad_params *out, const struct sample **s2);
void s600_open(struct os *s);
void os_open_s600_trim(struct os *s); // the Trim page, reached from S600 Env2
void s600_frame(struct os *s, struct ui *u, const struct input *in);
void levels_open(struct os *s);
void levels_frame(struct os *s, struct ui *u, const struct input *in);
