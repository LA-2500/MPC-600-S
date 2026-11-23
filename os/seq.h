// The sequencer, after the MPC3000: 99 sequences of 99 tracks, each track DRUM (plays the
// pads) or MIDI (sends notes out), 96 ticks per quarter note.
//
// The audio thread plays it once per audio block. Everything shared sits behind `lock`.
// The audio thread only ever try-locks it: if the UI thread holds it, that block is skipped
// and the next one catches up, so the audio thread never waits on the UI.
#pragma once
#include "engine.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define SEQ_PPQ 96
#define SEQ_COUNT 99
#define SEQ_TRACKS 99
#define SEQ_MAX_BARS 999
#define SEQ_PENDING 1024 // note-offs waiting; full, a release would be lost and its note held
#define SEQ_NAME 17

enum { TRACK_DRUM, TRACK_MIDI, TRACK_TYPES }; // a DRUM track on an S600 program plays it as S600
enum { SEQ_STOPPED, SEQ_PLAYING, SEQ_COUNT_IN, SEQ_RECORDING, SEQ_OVERDUB, SEQ_WAITING };
enum { CLICK_4, CLICK_4T, CLICK_8, CLICK_8T, CLICK_16, CLICK_16T, CLICK_32, CLICK_RATES };
#define SEQ_COUNT_BARS_MAX 4
enum { REC_ARM_OFF, REC_ARM_REC, REC_ARM_OVERDUB };
// Sequencer resolution and feel (Additional Settings): 96 PPQN with the MPC3000's swing, 96 with
// the MPC2000's, or the SP-1200's 24 PPQN, its six swing settings placed where the SP-1200
// puts them (measured from its grooves).
enum { SEQ_RES_MPC3000, SEQ_RES_MPC2000, SEQ_RES_SP1200, SEQ_RES_MPC60, SEQ_RES_COUNT }; // MPC60 added last: saved numbers stay
#define SP_SWINGS 6
enum { METRO_OFF, METRO_ON, METRO_REC };
enum { TC_4, TC_4T, TC_8, TC_8T, TC_16, TC_16T, TC_32, TC_32T, TC_64, TC_64T, TC_OFF, TC_COUNT }; // the MPC3000's note values, and 1/4, 1/64
enum { REP_4, REP_4T, REP_8, REP_8T, REP_16, REP_16T, REP_32, REP_32T, REP_RATES }; // Note Repeat's rates

struct seq_note {
    uint32_t tick;
    uint16_t dur;
    uint8_t note; // DRUM: the pad, 0..127 across the banks. MIDI: the note number.
    uint8_t vel;
    uint8_t fresh; // recorded this pass ahead of the play position, so not played again until next pass
    int8_t tune;   // DRUM and S600: semitones from the pad's pitch (16 Levels Tune, MIDI notes in)
};

// Recorded fader moves: one sound parameter of one pad, at a tick. value is 0..127.
enum { AUTO_VOL, AUTO_PAN, AUTO_CUTOFF, AUTO_RESO, AUTO_SEMI, AUTO_FINE, AUTO_PARAMS };

struct seq_auto {
    uint32_t tick;
    uint8_t pad, param, value;
};

enum { EQ_1073, EQ_4000E, EQ_TYPES };
// A track's three-band EQ, set on the mixer. Gains in tenths of a dB, mq the mid's Q in tenths.
struct track_eq {
    int type, on;
    int lf, lg;     // low shelf: Hz, gain
    int mf, mg, mq; // mid bell
    int hf, hg;     // high shelf
};

struct seq_track {
    struct seq_note *notes; // sorted by tick
    int count, cap;
    struct seq_auto *autos; // sorted by tick
    int nauto, capauto;
    int type, on, channel; // channel 0 is OFF
    int vel_pct;           // 1..200
    int level, pan;        // the track mixer: 0..100 and -50..50 (L50..R50)
    int mute, solo;        // the mixer's B1 and B2: a muted track, or any but the soloed ones, plays no notes
    struct track_eq eq;
    unsigned char send[9]; // Track FX: the level sent to each of Input Effects' nine effects, 0..100
    int program;           // MIDI program change: 0 OFF, else 1..128
    int kit;               // the pad program a DRUM track plays, 0..ENGINE_PROGRAMS-1. Kept apart
                           // from `program` so changing the track type loses neither.
    char name[SEQ_NAME];
};

struct seq_sequence {
    char name[SEQ_NAME];
    int bars, sig_num, sig_den;
    int tempo10;  // beats per minute times ten
    int loop_bar; // 0 OFF, else loop back to this bar at the end
    struct seq_track track[SEQ_TRACKS];
};

// A song: sequences in order, each played its number of times (Reps). It ends after the last.
#define SEQ_SONGS 20
#define SONG_STEPS 250
struct seq_song {
    int nsteps;
    struct {
        unsigned char seq, reps; // a sequence 0..98, played 1..99 times
    } step[SONG_STEPS];
};

// A step of a step pattern: its note (0..11, C to B, from the lowest pad held), octave
// (-1, 0, +1), and whether it plays, ties the note before on, or rests; accent and slide.
#define ARP_STEPS 16
#define ARP_USER 8
enum { ARP_NOTE, ARP_TIE, ARP_REST };
struct seq_arp_step {
    signed char note, oct;
    unsigned char time, accent, slide;
};
struct seq_arp_steps {
    int len; // 1..16 steps
    struct seq_arp_step step[ARP_STEPS];
};

struct seq_pending {
    double at; // absolute tick
    int midi, note, channel, key;
};

struct seq {
    pthread_mutex_t lock;
    struct seq_sequence sequence[SEQ_COUNT];
    int current, next; // next is the sequence queued while playing, or -1
    int track;         // the active track
    int rec_arm, metronome, count_in;
    int click_audio;  // the metronome during audio track and Live Looper takes (and Resample SEQUENCE): 0 off
    int audio_take;   // set by the OS while one of those takes runs (the count-in clicks either way)
    int click_loop;   // after a Live Looper take, while the sequence plays on: 0 no click, 1 click
    int loop_after;   // set by the OS: Live Looper takes made since the sequence started, none under way
    int click_rate;   // CLICK_*: how often the metronome clicks
    int click_volume; // 0..100
    int count_bars;   // bars of count-in
    int count_wait;   // instead of counting in, wait for a pad or a MIDI note to start
    int tc, swing;     // Timing Correct note value and swing percent (50..75; on the SP-1200 one of its six)
    int res;           // SEQ_RES_*
    int shift_later, shift_ticks;
    int mode, count_target;
    double pos;  // ticks into the sequence, negative during the count-in
    double abs;  // ticks since start-up, for note lengths
    int missed;  // audio frames skipped while the UI held the lock
    unsigned char kit_s600[ENGINE_PROGRAMS]; // programs whose Type is S600: they play as S600 on DRUM tracks too
    // The Mute page's PAD MUTE and PAD SOLO, a program's pads at a time: a muted pad's notes do not
    // play from the sequence; with any pad of a program soloed, only its soloed pads do.
    unsigned char pad_mute[ENGINE_PROGRAMS][128], pad_solo[ENGINE_PROGRAMS][128];
    long long zero_clock; // the engine clock frame where a count-in ended and the sequence began
    int zero_seen;        // set then; seq_play_count_in clears it
    long long wrap_clock; // the engine clock frame where the sequence last looped back
    int wrap_count;       // how many times it has looped back since the OS started
    int stop_flush;
    int erase_note; // while ERASE and a pad are held during playback, or -1
    struct {
        int open, seq, track, note, tune;
        uint32_t tick;
        double abs;
    } rec[256]; // by seq_rec_index
    struct seq_pending pending[SEQ_PENDING];
    int npending;
    // Note Repeat: while on, held pads retrigger on the Timing Correct grid, with swing.
    int repeat;
    int rep_rate; // REP_*: how often held pads repeat. 1/8 and 1/16 take the swing.
    int rep_alt;  // a rate held on B1-B3 while NOTE REPEAT is held, or -1 for rep_rate
    struct {
        int on, note, vel, channel, record; // channel > 0 sends MIDI instead of playing the pad
        int tune;
        int order; // when it was pressed, for the arpeggiator's AS PLAYED
        int held;  // the pad is still down (Latch keeps a pad playing after it lets go)
    } rep[16];
    // Arpeggiator: the Note Repeat page's Type ARPEGGIATOR. The held pads play one step at a
    // time in the pattern, on the same grid and rate.
    int arp, arp_pattern;
    int arp_oct;   // Octaves: 1..4, chord patterns climb across them
    int arp_gate;  // Gate: 10..100, each note's length as a share of a step
    int arp_latch; // Latch: the notes go on after the pads let go, until a new chord
    int arp_trans; // Transpose: -12..12 semitones
    int arp_pos, arp_order;
    unsigned arp_rand;
    int arp_slide; // the last step played slides into the next note (step patterns)
    int arp_last;  // and the semitones it played at
    struct seq_arp_steps arp_user[ARP_USER]; // USER 1..8: step patterns made on the Pattern Edit page
    int rep_restart;   // a first pad went down: the free-running grid starts now
    double rep_origin; // where that grid starts, in absolute ticks
    // Notes the audio thread recorded, for the UI thread to insert (it may allocate).
    struct {
        uint32_t tick;
        int seq, track, note, vel, dur, tune;
    } recq[128];
    int nrecq;
    // While recording fader moves: the last tick recorded for each parameter this take, so a
    // new move replaces what it passes over instead of piling up on the old one.
    uint32_t auto_last[AUTO_PARAMS];
    int auto_live[AUTO_PARAMS];
    // Sends raw MIDI. NULL when there is no MIDI port. Called from both threads.
    void (*midi_send)(const unsigned char *m, int n);
    int midi_off;  // Ext MIDI off: nothing goes out
    int sync_send; // Sync SEND: MIDI clock, start and stop go out with the sequence
    struct seq_song song[SEQ_SONGS];
    int song_on;              // playing a song: each sequence's end goes on to the next step
    int song_cur, song_step;  // which song, and the step playing
    int song_rep;             // passes of that step done
};

void seq_init(struct seq *s);
void seq_sequence_reset(struct seq_sequence *q, int i); // sequence i as at power-on, its notes freed

// The audio thread's part, installed as the engine's block hook.
void seq_audio_block(void *seq, struct engine *e, int frames);

// UI thread. Each takes the lock itself.
void seq_play(struct seq *s, int from_start);
void seq_restart(struct seq *s); // PLAY again while playing: from the start
void seq_stop(struct seq *s, int to_start);
// Plays from the start after `bars` of count-in (0: at once), whatever REC is armed: for
// audio track and resample takes, which need the frame where the sequence starts.
void seq_play_count_in(struct seq *s, int bars);
// Plays song `song` from `step`. STOP ends it like a sequence.
void seq_song_play(struct seq *s, int song, int step);
// The song as one new sequence in `to` (an empty one): each step's sequence, Reps times, end to end.
// 0, -1 when the song is empty, too long (999 bars) or memory runs out.
int seq_song_export(struct seq *s, int song, int to);
int seq_copy(struct seq *s, int from, int to); // Copy Sequence: from over to
void seq_rec_button(struct seq *s); // stopped: arms OVERDUB, then REC, then off. Playing: punches in or out.
int seq_wait_start(struct seq *s); // waiting for a key: starts recording now. 1 when it did.
int seq_click_ticks(int rate);
int seq_record_hit(struct seq *s, int note, int vel); // 0 when it was recorded
void seq_record_release(struct seq *s, int note);
// The same for a pad played `tune` semitones from its pitch.
int seq_record_hit_tuned(struct seq *s, int note, int tune, int vel);
void seq_record_release_tuned(struct seq *s, int note, int tune);
void seq_set_erase(struct seq *s, int note);
void seq_live_midi(struct seq *s, int channel, int note, int vel); // vel 0 is note off
void seq_send_cc(struct seq *s, int channel, int cc, int value);
int seq_add_note(struct seq *s, int sequence, int track, uint32_t tick, int note, int vel, int dur);
// Timing Correct on recorded notes: track's notes in [from, to). notes[n] picks which
// note numbers move; NULL moves them all.
int seq_move_existing(struct seq *s, int track, uint32_t from, uint32_t to, const unsigned char *notes);
void seq_sync_tempo_tap(struct seq *s, int tempo10);
void seq_set_repeat(struct seq *s, int on);
void seq_repeat_press(struct seq *s, int pad, int note, int vel, int channel, int record);
void seq_repeat_press_tuned(struct seq *s, int pad, int note, int tune, int vel, int channel, int record);
void seq_repeat_pressure(struct seq *s, int pad, int vel);
void seq_repeat_release(struct seq *s, int pad);
void seq_drain(struct seq *s); // inserts what the audio thread recorded. Call every frame.
// A fader move on the active track, while recording. 0 when it was recorded.
int seq_record_auto(struct seq *s, int pad, int param, int value);
// In the current sequence's active track. param -1 means every parameter of the pad.
int seq_auto_count(struct seq *s, int pad, int param);
int seq_erase_autos(struct seq *s, int pad, int param);
// The Erase screen: a sequence's notes in [from, to) on one track, or every track when track
// is -1, with the recorded changes there too. notes[n] picks which notes (and which pads'
// changes) go; NULL takes everything. Returns how many events went.
int seq_erase_range(struct seq *s, int sequence, int track, uint32_t from, uint32_t to, const unsigned char *notes);

// Bars: longer, the new bars empty or (dup) repeating the old ones from bar 1 round again;
// shorter, everything past the new end goes. 0, or -1 when memory ran out.
int seq_set_bars(struct seq *s, int sequence, int bars, int dup);
// Copy Events: notes and fader moves in [from, to) of a track (from_track -1: every track, each
// onto its own number) copied to start at `at` in to_seq's to_track, `copies` times end to end.
// replace: what was there in that span goes first. notes NULL: all. Past to_seq's end is left
// out (*dropped counts it). How many went in, or -1.
struct seq_copy_ev {
    int from_seq, from_track;
    uint32_t from, to;
    const unsigned char *notes;
    int to_seq, to_track;
    uint32_t at;
    int replace, copies;
};
int seq_copy_events(struct seq *s, const struct seq_copy_ev *c, int *dropped);
// Bars, 0 the first: blank bars before bar `before`; bars first..last taken out (a sequence keeps
// one); first..last of one sequence copied in before `before` of another, `copies` times. 0, or -1.
int seq_insert_bars(struct seq *s, int sequence, int before, int count);
int seq_delete_bars(struct seq *s, int sequence, int first, int last);
int seq_copy_bars(struct seq *s, int from_seq, int first, int last, int to_seq, int before, int copies);
// BPM doubled (twice) or halved. comp, note-value compensation: the music keeps its speed
// (doubled: every event twice as far along and the bars doubled; halved: squeezed into the
// first half and repeated in the second). Without it only the BPM changes. -1 out of range.
int seq_tempo_scale(struct seq *s, int sequence, int twice, int comp);

// Under the lock, or from the audio thread.
int seq_events_from(const struct seq_sequence *q, uint32_t from); // notes and fader moves at or after from
int seq_bar_ticks(const struct seq_sequence *q);
int seq_beat_ticks(const struct seq_sequence *q);
int seq_length(const struct seq_sequence *q);
int seq_used(const struct seq_sequence *q);
int seq_track_used(const struct seq_track *t);
uint32_t seq_correct(const struct seq *s, uint32_t tick, int length); // Timing Correct, swing and shift
int seq_tc_grid(int tc);
// Where the second of a pair of grid steps lands, from the pair's start, with the swing (ticks).
int seq_swing_ticks(const struct seq *s, int grid);
extern const int SEQ_SP_SWINGS[SP_SWINGS]; // 50, 54, 58, 63, 67, 71
int seq_swing_step(const struct seq *s, int swing, int d); // the next swing setting d steps on
int seq_repeat_ticks(int rate); // a REP_* rate in ticks
// The arpeggiator's patterns: name, group (ARP_GROUP_*), and the rate it starts at.
enum { ARP_GROUP_STANDARD, ARP_GROUP_VARIATION, ARP_GROUP_SYNTH, ARP_GROUP_BASS, ARP_GROUP_CHORD, ARP_GROUP_GAME,
       ARP_GROUP_FAMOUS, ARP_GROUP_CLASSICAL, ARP_GROUP_JAZZ, ARP_GROUP_STEP, ARP_GROUP_USER, ARP_GROUPS };
// Step patterns (STEP and USER) and their steps.
const struct seq_arp_steps *seq_arp_steps_of(const struct seq *s, int pattern); // NULL: not a step pattern
int seq_arp_user_slot(int pattern);    // USER n's slot (0..ARP_USER-1), or -1
int seq_arp_user_pattern(int slot);    // the pattern number of USER slot+1
void seq_arp_steps_clear(struct seq_arp_steps *p); // 16 steps, all C, no accent or slide
void seq_arp_steps_save(const struct seq_arp_steps *p, char *b, size_t n); // "16,0a1..." for settings
int seq_arp_steps_load(struct seq_arp_steps *p, const char *line);
const char *seq_arp_group_name(int g);
int seq_arp_first(int group);               // the first pattern of a category
int seq_arp_step(int pattern, int d);        // d patterns on within its category, stopping at the ends
void seq_arp_set_latch(struct seq *s, int on); // off: pads let go stop now
int seq_arp_patterns(void);
const char *seq_arp_name(int i);
int seq_arp_group(int i);
int seq_arp_rate(int i);
int seq_arp_is_riff(int i); // a melody played from the lowest pad held, not a chord pattern
// What B1, B2 or B3 (b 0..2) switches Note Repeat to from `rate`: B1 the triplet one step
// slower, B2 the next faster triplet, B3 one full rate faster. -1 past either end.
int seq_repeat_alt(int rate, int b);
const char *seq_repeat_name(int rate); // "1/16T" and so on
int seq_pad_note(int pad); // the MIDI note a pad sends, and answers to coming in (the pad note map)
int seq_note_pad(int note); // the pad a MIDI note plays, or -1
void seq_set_pad_note(int pad, int note);
void seq_default_pad_notes(void); // as on the MPC3000: pad A01 is note 35, and on up
void seq_send_raw(struct seq *s, const unsigned char *m, int n); // unless Ext MIDI is off
// Whether a track plays through the pads' S600 settings: a DRUM track on a program with S600
// Pgm Mode on. Under the lock.
int seq_plays_s600(const struct seq *s, const struct seq_track *k);
int seq_track_slot(const struct seq *s, const struct seq_track *k, int pad); // the engine slot it plays a pad on
