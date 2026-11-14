// SAMPLE REC's two pages, after the MPC3000's Sample New Sound.
//
// Sample New Sound: the settings, a level meter that becomes the waveform while recording,
// and once a take is done, <Play> to hear it, a pad to assign it or ENTER to keep it.
//
// Audiotrack Record (SAMPLE REC again): a take in time with the sequence. <Rec> plays the
// sequence from the start after a count-in; the take begins exactly on its first tick and
// ends after the set length, or at STOP or <Stop>. A pad (an empty one) gets it, with a note
// at the start of the active track, so it plays back with the sequence like an audio track.
//
// Live Looper (<Live Loop> on Audiotrack Record): one pass of the sequence recorded while it
// keeps looping, made into a sound in the background and dropped onto the pad selected
// beforehand, with a note at the start of the active track. It comes in on the pass it is
// ready in, from where that pass is, and plays from the start of every pass after.
#include "lcd.h"
#include "os.h"
#include "seq.h"
#include "umx.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

enum { F_MODE, F_MONITOR, F_THRES, F_BITS, F_RATE, F_PITCH, F_AA, F_NORMALIZE, F_COUNT, F_LENGTH, F_PAD, F_STEM, F_SMODE,
       F_INPUT, F_WHAT, F_START, F_RTHRU, F_KINDS };
static const struct {
    const char *label;
    int col; // where the value goes
} KINDS[F_KINDS] = {
    {"Mode:", 9}, {"Monitor:", 9}, {"Thres:", 9}, {"Bit Rate:", 9}, {"Sample Rate:", 12},
    {"Pitch:", 9}, {"Anti-Aliasing:", 14}, {"Normalize:", 14}, {"Count In:", 9}, {"Length:", 9}, {"Pad:", 9},
    {"Stem:", 9}, {"Mode:", 9}, {"Input:", 9}, {"Record:", 9}, {"Start:", 9}, {"Input Thru:", 12},
};
#define PAGE_FIELDS 10
static const int PAGE[6][PAGE_FIELDS] = {
    {F_INPUT, F_MODE, F_PITCH, F_THRES, F_BITS, F_RATE, F_AA, F_NORMALIZE, F_MONITOR, -1},
    {F_INPUT, F_MODE, F_PITCH, F_COUNT, F_LENGTH, F_BITS, F_RATE, F_AA, F_NORMALIZE, -1}, // Input Thru: on Sample New Sound
    {F_MODE, F_BITS, F_RATE, F_PITCH, F_AA, F_NORMALIZE, F_PAD, -1, -1, -1}, // Pad: last, a pad hit picks it anyway
    {F_STEM, F_SMODE, F_PAD, -1, -1, -1, -1, -1, -1, -1},
    {F_WHAT, F_START, F_THRES, F_MODE, F_PITCH, F_BITS, F_RATE, F_AA, F_NORMALIZE, F_RTHRU}, // Thres beside Start
    {F_INPUT, F_MODE, F_PAD, F_PITCH, F_COUNT, F_BITS, F_RATE, F_AA, F_NORMALIZE, -1}, // Pad: the one it goes on
};
static const char *const TITLES[6] = {"Sample New Sound", "Audiotrack Record", "Convert Sample", "Stems", "Resample",
                                      "Live Looper"};
static const char *const STEM_NAMES[STEM_KINDS] = {"DRUMS", "BASS", "VOCALS", "MUSIC"};

// The fields a page shows now. Resample shows Start only for LIVE, and Thres only for a
// THRESHOLD start: a sequence starts with the sequence.
static int fields_of(const struct os *s, int page, int *out) {
    const struct rec_settings *r = &s->rec_set[4];
    int n = 0;
    for (int i = 0; i < PAGE_FIELDS && PAGE[page][i] >= 0; i++) {
        int k = PAGE[page][i];
        if (page == 4 && k == F_START && r->what) continue;
        if (page == 4 && k == F_THRES && (r->what || r->start_force)) continue;
        out[n++] = k;
    }
    return n;
}
static const char *const MODES[REC_MODES] = {"STEREO", "MONO LFT", "MONO RGT", "MONO LR"};
static const char *const LENGTHS[3] = {"SEQUENCE", "DOUBLE SEQ", "MAX"};
static const int BITS[] = {4, 8, 12, 16};
#define NBITS 4
#define THRES_OFF (-61) // below the lowest level: recording starts at once
#define TAKE_PAD (-3)   // the engine pad <Play> plays a finished take on
#define LP_FADE_MS 5   // Live Looper: the fade-in of a take coming in partway through a pass
#define METER_ROW 12 // the meter takes the last three rows: nine fields above it
#define WAVE_TOP (13 * CELL_H)
#define WAVE_ROWS_H (2 * CELL_H)

// The classic rates the data wheel lands on, and the slider jumps between.
static const int CLASSIC[] = {7000, 11020, 22050, 26040, 29760, 44100};
#define NCLASSIC 6

// Every rate the wheel can reach: half kHz steps from 7.0 to 44.0, with the classic rates.
static int rates(int *out) {
    int n = 0;
    for (int hz = 7000; hz <= 44100; hz += 500) {
        int near = 0;
        for (int c = 0; c < NCLASSIC; c++)
            if (CLASSIC[c] != hz && CLASSIC[c] - hz < 100 && hz - CLASSIC[c] < 100) near = 1;
        if (!near) out[n++] = hz;
        for (int c = 0; c < NCLASSIC; c++) // a classic rate between this step and the next
            if (CLASSIC[c] > hz && CLASSIC[c] < hz + 500) out[n++] = CLASSIC[c];
    }
    return n;
}

void record_defaults(struct os *s) {
    s->input_thru = 1;
    struct rec_settings *n = &s->rec_set[0], *a = &s->rec_set[1];
    *n = (struct rec_settings){.mode = REC_STEREO, .thres_db = -30, .bits = 12, .rate = 26040, .aa = 1, .normalize = 1};
    *a = (struct rec_settings){.mode = REC_STEREO, .thres_db = THRES_OFF, .bits = 12, .rate = 44100, .aa = 1,
                               .normalize = 1, .count_bars = 2, .length = RLEN_SEQ};
    s->rec_set[2] = (struct rec_settings){.mode = REC_MONO_LR, .bits = 12, .rate = 26040, .aa = 1, .normalize = 1};
    s->rec_set[4] = (struct rec_settings){.mode = REC_STEREO, .thres_db = -30, .bits = 16, .rate = 44100, .aa = 1,
                                          .start_force = 1};
    s->rec_set[5] = *a, s->rec_set[5].length = RLEN_SEQ; // Live Looper: as Audiotrack Record, one pass
    record_apply_start(s);
}

// Additional Settings' Start values: the same rate, bits, Anti-Aliasing, Normalize and Mode on
// every recording page, and Input Thru.
void record_apply_start(struct os *s) {
    static const int PAGES[] = {0, 1, 2, 4, 5};
    for (int i = 0; i < 5; i++) {
        struct rec_settings *r = &s->rec_set[PAGES[i]];
        r->rate = s->start_rate, r->bits = s->start_bits, r->aa = s->start_aa != 0, r->normalize = s->start_norm != 0;
        r->mode = PAGES[i] == 2 ? REC_MONO_LR : s->start_mode; // Convert Sample stays mono (MONO LR)
    }
    s->input_thru = s->start_thru != 0;
}

int record_rates(int *out) {
    return rates(out);
}

int record_classic(int *out) {
    for (int i = 0; i < NCLASSIC; i++) out[i] = CLASSIC[i];
    return NCLASSIC;
}

// SAMPLE REC: opens Sample New Sound, then flips between it and Audiotrack Record.
// SAMPLE REC: Sample New Sound, then each press the next of Audiotrack Record, Convert Sample
// and Stems, and round again.
void record_open(struct os *s) {
    if (s->screen == OS_RECORD) {
        s->record_page = s->record_page >= 3 ? 0 : s->record_page + 1;
    } else {
        s->record_return = s->screen;
        s->screen = OS_RECORD;
        s->record_page = 0;
    }
    s->cursor_record = 0;
}

// Shift + Pad 11: Resample; pressed again, the first field.
void record_open_resample(struct os *s) {
    if (s->screen != OS_RECORD) s->record_return = s->screen;
    s->screen = OS_RECORD;
    s->record_page = 4;
    s->cursor_record = 0;
}

// Shift + SAMPLE REC: Convert Sample, then flips between it and Stems.
void record_open_convert(struct os *s) {
    if (s->screen == OS_RECORD && (s->record_page == 2 || s->record_page == 3)) {
        s->record_page = s->record_page == 2 ? 3 : 2;
    } else {
        if (s->screen != OS_RECORD) s->record_return = s->screen;
        s->screen = OS_RECORD;
        s->record_page = 2;
    }
    s->cursor_record = 0;
}

static void change(struct os *s, struct rec_settings *r, int kind, int d) {
    switch (kind) {
    case F_MODE: r->mode = (r->mode + (d > 0 ? 1 : REC_MODES - 1)) % REC_MODES; break;
    case F_MONITOR: s->input_thru = d > 0; break;
    case F_THRES:
        r->thres_db += d;
        r->thres_db = r->thres_db < THRES_OFF ? THRES_OFF : r->thres_db > 0 ? 0 : r->thres_db;
        break;
    case F_BITS: {
        int i = 0;
        while (i < NBITS - 1 && BITS[i] != r->bits) i++;
        i += d > 0 ? 1 : -1;
        r->bits = BITS[i < 0 ? 0 : i >= NBITS ? NBITS - 1 : i];
        break;
    }
    case F_RATE: {
        int list[96], n = rates(list), i = 0;
        while (i < n - 1 && list[i] < r->rate) i++;
        i += d;
        r->rate = list[i < 0 ? 0 : i >= n ? n - 1 : i];
        break;
    }
    case F_PITCH: r->pitch = r->pitch + d < 0 ? 0 : r->pitch + d > 24 ? 24 : r->pitch + d; break;
    case F_AA: r->aa = d > 0; break;
    case F_NORMALIZE: r->normalize = d > 0; break;
    case F_COUNT: r->count_bars = d > 0 ? 2 : 1; break;
    case F_LENGTH: r->length = r->length + d < 0 ? 0 : r->length + d > RLEN_MAX ? RLEN_MAX : r->length + d; break;
    case F_PAD: s->selected = s->selected + d < 0 ? 0 : s->selected + d >= OS_PADS ? OS_PADS - 1 : s->selected + d; break;
    case F_STEM: s->stem_kind = s->stem_kind + d < 0 ? 0 : s->stem_kind + d >= STEM_KINDS ? STEM_KINDS - 1 : s->stem_kind + d; break;
    case F_SMODE: s->stem_remove = d > 0; break;
    case F_WHAT: r->what = d > 0; break;
    case F_RTHRU: r->thru_record = d > 0; break;
    case F_START: r->start_force = d < 0; break;
    case F_INPUT: { // LINE, INST, MIC or USB: one at a time
        static const int ORDER[INPUT_SOURCES] = {INPUT_LINE, INPUT_INST, INPUT_MIC, INPUT_USB};
        int at = 0;
        while (at < INPUT_SOURCES - 1 && ORDER[at] != s->input_src) at++;
        at = at + d < 0 ? 0 : at + d >= INPUT_SOURCES ? INPUT_SOURCES - 1 : at + d;
        s->input_src = ORDER[at];
        if (s->set_input && s->set_input(s->input_src) != 0 && s->input_src == INPUT_USB) s->usb_missing = 1;
        os_save_settings(s);
        break;
    }
    }
}

static void value(const struct os *s, const struct rec_settings *r, int kind, char *b, size_t n) {
    switch (kind) {
    case F_MODE: snprintf(b, n, " %s ", MODES[r->mode]); break;
    case F_MONITOR: snprintf(b, n, " Input Thru %s ", s->input_thru ? "ON" : "OFF"); break;
    case F_THRES:
        if (r->thres_db <= THRES_OFF) snprintf(b, n, " OFF ");
        else snprintf(b, n, " %ddB ", r->thres_db);
        break;
    case F_BITS: snprintf(b, n, " %d ", r->bits); break;
    case F_RATE: snprintf(b, n, " %d.%02d kHz ", r->rate / 1000, r->rate % 1000 / 10); break;
    case F_PITCH: snprintf(b, n, " +%d ", r->pitch); break;
    case F_AA: snprintf(b, n, " %s ", r->aa ? "ON" : "OFF"); break;
    case F_NORMALIZE: snprintf(b, n, " %s ", r->normalize ? "ON" : "OFF"); break;
    case F_COUNT: snprintf(b, n, " %d BAR%s ", r->count_bars, r->count_bars > 1 ? "S" : ""); break;
    case F_LENGTH: snprintf(b, n, " %s ", LENGTHS[r->length]); break;
    case F_STEM: snprintf(b, n, " %s ", STEM_NAMES[s->stem_kind]); break;
    case F_SMODE: snprintf(b, n, " %s ", s->stem_remove ? "REMOVE" : "EXTRACT"); break;
    case F_INPUT:
        snprintf(b, n, " %s ", s->input_src == INPUT_MIC ? "MIC" : s->input_src == INPUT_USB ? "USB" : s->input_src == INPUT_INST ? "INST L" : "LINE");
        break;
    case F_WHAT: snprintf(b, n, " %s ", r->what ? "SEQUENCE" : "LIVE"); break;
    case F_RTHRU: snprintf(b, n, " %s ", r->thru_record ? "RECORD" : "MUTE"); break;
    case F_START: snprintf(b, n, " %s ", r->start_force ? "FORCE" : "THRESHOLD"); break;
    case F_PAD: {
        const struct os_pad *p = &s->pads[s->selected];
        snprintf(b, n, " %c%02d %.11s ", 'A' + s->selected / 16, s->selected % 16 + 1, p->s ? p->name : "(empty)");
        break;
    }
    }
}

// How long a take can run: what memory allows, leaving room to turn it into a sound
// (the take, a working copy and the sound: about 20 bytes a frame) and for everything else.
static long max_frames(const struct os *s) {
    long avail = s->mem_available ? s->mem_available() : 60L * ENGINE_RATE * 20 + (128L << 20);
    long frames = (avail - (128L << 20)) / 20;
    long most = (long)REC_MAX_CHUNKS * REC_CHUNK;
    return frames < ENGINE_RATE ? ENGINE_RATE : frames > most ? most : frames;
}

static long ticks_to_frames(const struct seq_sequence *q, long ticks) {
    return (long)llround((double)ticks * 600.0 * ENGINE_RATE / ((double)q->tempo10 * SEQ_PPQ));
}

// The finished take as a sound. An audio track's starts on the sequence's first tick, as
// it came back through the input.
static void take_done(struct os *s, struct ui *u) {
    const struct rec_settings *r = &s->rec_set[s->record_page];
    int page = s->record_page, pitched = page == 0 || page == 1 || page == 4;
    // An instrument is mono: its take is one channel, whatever Mode says (Resample records the mix).
    int mode = s->input_src == INPUT_INST && page != 4 ? REC_MONO_L : r->mode;
    struct rec_format f = {mode, r->rate, r->bits, r->aa, r->normalize, pitched ? r->pitch : 0, 0, 0};
    s->rec_sync = page == 1; // an audio track: an empty pad, and a note on the track
    if (page == 1 || (page == 4 && r->what)) { // in time with the sequence: trimmed to it
        pthread_mutex_lock(&s->seq.lock);
        int seen = s->seq.zero_seen;
        long long zero = s->seq.zero_clock;
        pthread_mutex_unlock(&s->seq.lock);
        if (!seen) { // stopped during the count-in
            recorder_discard(&s->rec);
            os_toast(s, u, "Stopped before the start");
            return;
        }
        f.skip = (long)(zero - s->rec.first_clock) + s->at_latency;
        f.length = s->at_length;
    }
    s->rec_result = recorder_take(&s->rec, &f);
    s->rec_previewed = 0;
    s->rec_result_pitch = f.pitch;
    s->rec_result_counted = 1;
    snprintf(s->rec_result_name, sizeof s->rec_result_name, "%s%02d", page == 1 ? "Audio" : page == 4 ? "Resample" : "Sample",
             s->rec_count + 1);
    if (!s->rec_result) {
        recorder_discard(&s->rec);
        os_toast(s, u, "Out of memory");
    }
}

// Convert Sample: the selected pad's sound, sampled again with this page's settings.
// The converted sound replaces the original on the pad at once, its trim and loop following
// it, tuned down by the Pitch. The original is kept in memory.
static void convert_pad(struct os *s, struct ui *u) {
    int slot = s->selected;
    struct os_pad *p = &s->pads[slot];
    if (!p->s) {
        os_toast(s, u, "Pad is empty");
        return;
    }
    const struct rec_settings *r = &s->rec_set[2];
    struct rec_format f = {r->mode, r->rate, r->bits, r->aa, r->normalize, r->pitch, 0, 0};
    struct sample *old = p->s, *n = rec_convert(old, &f);
    if (!n) {
        os_toast(s, u, "Out of memory");
        return;
    }
    int kept = 0;
    for (int i = 0; i < s->nmemory; i++) kept |= s->memory[i].s == old;
    if (!kept && os_memory_add(s, old, p->name) != 0) {
        sample_free(n);
        os_toast(s, u, "Memory full");
        return;
    }
    os_undo_save(s, "Convert");
    double k = (double)n->frames / old->frames;
    struct pad_params keep = p->p;
    char name[40];
    snprintf(name, sizeof name, "%.20s-%dk%d", p->name, r->rate / 1000, r->bits);
    os_pad_assign(s, slot, n, name);
    p->p = keep; // the pad's settings, its trim and loop moved to the converted sound
    p->p.start = (int32_t)(keep.start * k);
    p->p.end = (int32_t)(keep.end * k + 0.5);
    p->p.loop = (int32_t)(keep.loop * k);
    if (p->p.end > n->frames || p->p.end < 1) p->p.end = n->frames;
    if (p->p.start >= p->p.end) p->p.start = 0;
    if (p->p.loop < p->p.start || p->p.loop >= p->p.end) p->p.loop = p->p.start;
    p->p.semi -= r->pitch;
    if (p->p.semi < -36) p->p.semi = -36;
    os_pad_push(s, slot);
    os_toast(s, u, "Done"); // the original is in memory
}

int record_pad(struct os *s, struct ui *u, int slot) {
    if (s->screen != OS_RECORD || !s->rec_result) return 0;
    char msg[40];
    if (s->rec_sync && s->pads[slot].s) { // an audio track needs a pad of its own
        os_toast(s, u, "Pad not empty: select again");
        return 1;
    }
    const struct sample *t = s->rec_result;
    long frames = t->frames, rate = t->rate;
    os_undo_save(s, s->rec_sync ? "Audio Track" : "Assign");
    os_pad_load(s, slot, s->rec_result, s->rec_result_name);
    s->pads[slot].p.semi = -s->rec_result_pitch; // a pitched-up sound plays at its own pitch
    os_pad_push(s, slot);
    s->rec_result = 0;
    s->rec_count += s->rec_result_counted;
    s->selected = slot;
    snprintf(msg, sizeof msg, "Assigned to: Pad %c%02d", 'A' + slot / 16, slot % 16 + 1);
    if (s->rec_sync) { // a note on the first tick of the active track starts it with the sequence
        pthread_mutex_lock(&s->seq.lock);
        const struct seq_sequence *q = &s->seq.sequence[s->seq.current];
        int track = s->seq.track, type = q->track[track].type;
        long ticks = (long)llround((double)frames / rate * q->tempo10 * SEQ_PPQ / 600.0);
        pthread_mutex_unlock(&s->seq.lock);
        if (type == TRACK_MIDI) snprintf(msg, sizeof msg, "Pad %c%02d: MIDI track, no note", 'A' + slot / 16, slot % 16 + 1);
        else seq_add_note(&s->seq, s->seq.current, track, 0, slot, 127, ticks < 1 ? 1 : ticks > 65535 ? 65535 : (int)ticks);
    }
    os_toast(s, u, msg);
    return 1;
}

// -48..0 dB across the meter, as the MPC3000's.
static int meter_x(float db) {
    if (db < -48) db = -48;
    if (db > 0) db = 0;
    return LCD_X + 2 * CELL_W + (int)((db + 48) / 48 * (22 * CELL_W - 2));
}

static void draw_meter(struct ui *u, struct os *s, int thres_db) {
    for (int c = 0; c < 2; c++) {
        float peak = recorder_peak(&s->rec, c);
        s->rec_meter[c] = peak > s->rec_meter[c] ? peak : s->rec_meter[c] * 0.85f;
        int row = METER_ROW + 1 + c, y = row * CELL_H + 4;
        lcd_text(u, 0, row, c ? "R" : "L");
        float db = s->rec_meter[c] > 0 ? 20.0f * log10f(s->rec_meter[c]) : -96.0f;
        int end = meter_x(db);
        for (int x = meter_x(-48); x < end; x += 8) lcd_fill(u, x, y, 6, 8, LCD_FG); // lit segments
        for (int x = meter_x(-48); x < meter_x(0); x += 8) lcd_dot(u, x, y + 10, LCD_FG); // the scale
    }
    if (thres_db > THRES_OFF) { // the threshold, across both bars
        int x = meter_x((float)thres_db) & ~1;
        for (int y = (METER_ROW + 1) * CELL_H; y < (METER_ROW + 3) * CELL_H; y += 2) lcd_dot(u, x, y, LCD_FG);
    }
}

// The take so far, the last few seconds scrolling past; or a sound (r), whole.
static void draw_wave(struct ui *u, struct os *s, const struct sample *r) {
    int width = LCD_COLS * CELL_W - 8, mid = WAVE_TOP + WAVE_ROWS_H / 2, half = WAVE_ROWS_H / 2 - 2;
    long n = r ? r->frames : atomic_load(&s->rec.frames);
    long span = r ? n : 3L * ENGINE_RATE, from = r ? 0 : n - span;
    for (int x = 0; x < width; x += 2) {
        long a = from + span * x / width, b = from + span * (x + 2) / width;
        float top = 0;
        for (long i = a < 0 ? 0 : a; i < b && i < n; i += (b - a) / 64 + 1) {
            float l, rr;
            if (r) {
                l = r->data[i * r->channels];
                rr = r->data[i * r->channels + r->channels - 1];
            } else {
                recorder_frame(&s->rec, i, &l, &rr);
            }
            l = fabsf(l) > fabsf(rr) ? fabsf(l) : fabsf(rr);
            if (l > top) top = l;
        }
        int h = (int)(top * half);
        for (int y = mid - h; y <= mid + h; y += 2) lcd_dot(u, LCD_X + 4 + x, y & ~1, LCD_FG);
    }
}

// <Rec> on Audiotrack Record: the take runs from now, and the sequence from its start after
// the count-in. The take is trimmed to the sequence once it is done.
static void start_audio_track(struct os *s) {
    const struct rec_settings *r = &s->rec_set[1];
    pthread_mutex_lock(&s->seq.lock);
    const struct seq_sequence *q = &s->seq.sequence[s->seq.current];
    long length = ticks_to_frames(q, seq_length(q));
    pthread_mutex_unlock(&s->seq.lock);
    s->at_length = r->length == RLEN_SEQ ? length : r->length == RLEN_DOUBLE ? 2 * length : 0;
    s->at_latency = s->rec.latency; // played along to the click: the take is that much later
    atomic_store(&s->rec.source, 0);
    atomic_store(&s->rec.threshold, 0);
    seq_stop(&s->seq, 1);
    recorder_arm(&s->rec, max_frames(s));
    seq_play_count_in(&s->seq, r->count_bars);
    s->at_running = 1;
}

// <Rec> on Resample, SEQUENCE: the sequence from its start, no count-in, its exact length off
// the main output. Nothing to allow for: the take is the engine's own sound.
static void start_resample_sequence(struct os *s) {
    pthread_mutex_lock(&s->seq.lock);
    const struct seq_sequence *q = &s->seq.sequence[s->seq.current];
    s->at_length = ticks_to_frames(q, seq_length(q));
    pthread_mutex_unlock(&s->seq.lock);
    s->at_latency = 0;
    atomic_store(&s->rec.thru_resample, s->rec_set[4].thru_record);
    atomic_store(&s->rec.source, 1);
    atomic_store(&s->rec.threshold, 0);
    seq_stop(&s->seq, 1);
    recorder_arm(&s->rec, max_frames(s));
    seq_play_count_in(&s->seq, 0);
    s->at_running = 1;
}

// While an audio track take runs: it ends at its length, or when the sequence is stopped.
static void watch_audio_track(struct os *s) {
    if (!s->at_running) return;
    int state = atomic_load(&s->rec.state);
    if (state == REC_DONE || state == REC_IDLE) {
        s->at_running = 0;
        return;
    }
    pthread_mutex_lock(&s->seq.lock);
    int stopped = s->seq.mode == SEQ_STOPPED, seen = s->seq.zero_seen;
    long long zero = s->seq.zero_clock;
    pthread_mutex_unlock(&s->seq.lock);
    long have = atomic_load(&s->rec.frames);
    int full = seen && s->at_length > 0 && state == REC_RECORDING &&
               have >= (long)(zero - s->rec.first_clock) + s->at_latency + s->at_length;
    if (stopped || full) {
        recorder_stop(&s->rec);
        if (!stopped) seq_stop(&s->seq, 1);
        s->at_running = 0;
    }
}

// ---- Live Looper ----

// The background work on a take: IMMEDIATE converts it while it comes in (st); AFTER 1ST LOOP
// converts the raw take (src) while it already plays.
struct lp_job {
    pthread_t thread;
    atomic_int done, cancel;
    const struct recorder *rec;
    struct rec_stream *st;
    struct sample *src;
    struct rec_format f;
    struct sample *out;
    int slot;
};

static void *lp_stream_thread(void *a) {
    struct lp_job *j = a;
    while (!atomic_load(&j->cancel) && !rec_stream_run(j->st, j->rec)) usleep(2000);
    if (atomic_load(&j->cancel)) rec_stream_free(j->st);
    else j->out = rec_stream_finish(j->st);
    j->st = 0;
    atomic_store(&j->done, 1);
    return 0;
}

static void *lp_convert_thread(void *a) {
    struct lp_job *j = a;
    j->out = rec_convert(j->src, &j->f);
    atomic_store(&j->done, 1);
    return 0;
}

static struct rec_format lp_format(const struct os *s) {
    const struct rec_settings *set = &s->rec_set[5];
    int mode = s->input_src == INPUT_INST ? REC_MONO_L : set->mode; // an instrument is mono
    return (struct rec_format){mode, set->rate, set->bits, set->aa, set->normalize, set->pitch, 0, 0};
}

static struct lp_job *lp_start(void *(*fn)(void *), struct lp_job *j) {
    if (pthread_create(&j->thread, 0, fn, j) != 0) {
        free(j);
        return 0;
    }
    return j;
}

// Waits for a job and takes its result; the job goes.
static struct sample *lp_join(struct lp_job **jp) {
    struct lp_job *j = *jp;
    if (!j) return 0;
    pthread_join(j->thread, 0);
    struct sample *out = j->out;
    rec_stream_free(j->st);
    free(j);
    *jp = 0;
    return out;
}

// <Rec>: the pass to record is the first after the count-in (stopped), or the next one (playing).
static void looper_start(struct os *s, struct ui *u) {
    const struct rec_settings *r = &s->rec_set[5];
    int slot = s->selected;
    if (!atomic_load(&s->rec.present)) {
        os_toast(s, u, "No audio input");
        return;
    }
    if (s->pads[slot].s) {
        os_toast(s, u, "Select an empty pad first");
        return;
    }
    pthread_mutex_lock(&s->seq.lock);
    const struct seq_sequence *q = &s->seq.sequence[s->seq.current];
    int track = s->seq.track, midi = q->track[track].type == TRACK_MIDI, loop = q->loop_bar;
    int stopped = s->seq.mode == SEQ_STOPPED, wraps = s->seq.wrap_count;
    long length = ticks_to_frames(q, seq_length(q));
    pthread_mutex_unlock(&s->seq.lock);
    if (midi) {
        os_toast(s, u, "MIDI track: select a drum track");
        return;
    }
    if (loop != 1) {
        os_toast(s, u, "Set the sequence Loop TO BAR 1");
        return;
    }
    s->lp_slot = slot, s->lp_track = track, s->lp_length = length, s->lp_pitch = r->pitch;
    s->lp_latency = s->rec.latency; // played along to the sequence: the take is that much later
    atomic_store(&s->rec.source, 0);
    atomic_store(&s->rec.threshold, 0);
    if (recorder_arm(&s->rec, max_frames(s)) != 0) return;
    s->lp_session = 1;
    if (stopped) {
        seq_play_count_in(&s->seq, r->count_bars);
        s->lp_state = LP_COUNT;
    } else {
        s->lp_wraps = wraps;
        s->lp_state = LP_WAIT;
    }
}

// <Stop>, or the sequence stopped: the take is let go; the sequence plays on.
static void looper_abort(struct os *s) {
    if (s->lp_state == LP_IDLE) return;
    if (s->lp_job) atomic_store(&s->lp_job->cancel, 1);
    recorder_stop(&s->rec);
    s->lp_state = LP_ABORT;
}

// A take onto its pad, with a note at the start of the track, and in from where the pass is.
static void looper_drop_in(struct os *s, struct ui *u, struct sample *t, int semi) {
    int slot = s->lp_slot;
    char name[24], msg[40];
    snprintf(name, sizeof name, "Loop%02d", s->rec_count + 1);
    if (s->pads[slot].s) { // filled meanwhile: kept in memory instead
        if (os_memory_add(s, t, name) != 0) sample_free(t), os_toast(s, u, "Memory full");
        else s->rec_count++, os_toast(s, u, "Pad not empty: kept in memory");
        return;
    }
    os_undo_save(s, "Live Looper");
    os_pad_load(s, slot, t, name);
    s->pads[slot].p.semi = semi; // a pitched-up take plays at its own pitch
    os_pad_push(s, slot);
    s->rec_count++;
    pthread_mutex_lock(&s->seq.lock);
    const struct seq_sequence *q = &s->seq.sequence[s->seq.current];
    long ticks = seq_length(q);
    int playing = s->seq.mode != SEQ_STOPPED && s->seq.mode != SEQ_COUNT_IN && s->seq.mode != SEQ_WAITING;
    double pos = s->seq.pos;
    const struct seq_track *k = &q->track[s->lp_track];
    int level = k->level, pan = k->pan, seqn = s->seq.current;
    pthread_mutex_unlock(&s->seq.lock);
    seq_add_note(&s->seq, seqn, s->lp_track, 0, slot, 127, ticks < 1 ? 1 : ticks > 65535 ? 65535 : (int)ticks);
    if (s->lp_ntakes == LP_TAKES) memmove(s->lp_takes, s->lp_takes + 1, (LP_TAKES - 1) * sizeof s->lp_takes[0]);
    else s->lp_ntakes++;
    s->lp_takes[s->lp_ntakes - 1] = (struct lp_take){slot, seqn, s->lp_track, t};
    long at = ticks > 0 ? (long)((double)t->frames * pos / ticks) : 0;
    if (playing && at > 0 && at < t->frames - 64) { // this pass: from where it is, faded in so it does not click
        struct pad_params p = s->pads[slot].p;
        p.start = (int32_t)at;
        if (p.attack_ms < LP_FADE_MS) p.attack_ms = LP_FADE_MS;
        engine_note_on_layers(s->engine, os_pad_slot(s, slot), 127, 0, t, 0, &p, s->lp_track + 1, level, pan);
    }
    snprintf(msg, sizeof msg, "Looping on Pad %c%02d", 'A' + slot / 16, slot % 16 + 1);
    os_toast(s, u, msg);
}

// AFTER 1ST LOOP: the converted take replaces the raw one on its pad, from the next pass.
static void looper_swap(struct os *s, struct ui *u) {
    int slot = s->lp_conv->slot;
    struct sample *raw = s->lp_conv->src, *t = lp_join(&s->lp_conv);
    int still = s->pads[slot].s == raw; // not deleted or replaced meanwhile
    char name[24];
    snprintf(name, sizeof name, "%s", s->pads[slot].name);
    os_sample_release(s, raw); // the job's hold on it
    if (!t) return;
    if (!still) {
        sample_free(t);
        return;
    }
    os_pad_load(s, slot, t, name); // the raw one plays out its pass, then goes
    s->pads[slot].p.semi = -s->rec_set[5].pitch;
    os_pad_push(s, slot);
    for (int i = 0; i < s->lp_ntakes; i++)
        if (s->lp_takes[i].s == raw) s->lp_takes[i].s = t;
    ui_wake(u, 1);
}

// <Del Take>: the last take's note and sound go, and its pad is selected for another try.
static void looper_delete(struct os *s, struct ui *u) {
    while (s->lp_ntakes > 0) {
        struct lp_take k = s->lp_takes[--s->lp_ntakes];
        if (s->pads[k.slot].s != k.s) continue; // changed since: not the take any more
        unsigned char notes[128] = {0};
        notes[k.slot & 127] = 1;
        os_undo_save(s, "Delete Take");
        seq_erase_range(&s->seq, k.seq, k.track, 0, 1, notes);
        os_pad_clear(s, k.slot);
        s->selected = k.slot;
        char msg[40];
        snprintf(msg, sizeof msg, "Take deleted: Pad %c%02d", 'A' + k.slot / 16, k.slot % 16 + 1);
        os_toast(s, u, msg);
        return;
    }
    os_toast(s, u, "No take to delete");
}

void looper_service(struct os *s, struct ui *u) {
    if (s->lp_conv && atomic_load(&s->lp_conv->done)) looper_swap(s, u);
    if (s->lp_conv) ui_wake(u, 20);
    if (s->lp_state == LP_IDLE) return;
    struct recorder *r = &s->rec;
    int state = atomic_load(&r->state);
    if (s->lp_state == LP_ABORT) {
        if (state == REC_ARMED || state == REC_RECORDING) return; // stops on the next block
        sample_free(lp_join(&s->lp_job));
        recorder_discard(r);
        s->lp_state = LP_IDLE;
        os_toast(s, u, "Take stopped");
        ui_wake(u, 1);
        return;
    }
    pthread_mutex_lock(&s->seq.lock);
    int stopped = s->seq.mode == SEQ_STOPPED, seen = s->seq.zero_seen, wraps = s->seq.wrap_count;
    long long zero = s->seq.zero_clock, wrap = s->seq.wrap_clock;
    pthread_mutex_unlock(&s->seq.lock);
    if (stopped && s->lp_state != LP_ENDING) {
        looper_abort(s);
        return;
    }
    if (s->lp_state == LP_COUNT && seen) s->lp_zero = zero, s->lp_state = LP_REC;
    if (s->lp_state == LP_WAIT && wraps != s->lp_wraps) s->lp_zero = wrap, s->lp_state = LP_REC;
    if (s->lp_state == LP_REC && state != REC_ARMED && state != REC_RECORDING) { // out of memory before the end
        sample_free(lp_join(&s->lp_job));
        recorder_discard(r);
        s->lp_state = LP_IDLE;
        os_toast(s, u, "Out of memory");
        return;
    }
    long skip = (long)(s->lp_zero - r->first_clock) + s->lp_latency;
    if (s->lp_state == LP_REC && state == REC_RECORDING) {
        if (!s->lp_job && s->loop_conv == LOOP_CONV_IMMEDIATE) { // converted as it comes in
            struct lp_job *j = calloc(1, sizeof *j);
            if (j) {
                j->rec = r, j->f = lp_format(s);
                j->st = rec_stream_new(&j->f, skip, s->lp_length);
                if (!j->st) free(j);
                else s->lp_job = lp_start(lp_stream_thread, j);
            }
        }
        long left = skip + s->lp_length - atomic_load(&r->frames);
        if (left <= 0) {
            recorder_stop(r);
            s->lp_state = LP_ENDING;
        }
        if (left < ENGINE_RATE / 5) ui_wake(u, 3); // close to the end: checked often, for a quick drop-in
    }
    if (s->lp_state != LP_ENDING) return;
    ui_wake(u, 3);
    if (state != REC_DONE) return;
    if (s->loop_conv == LOOP_CONV_IMMEDIATE && s->lp_job) {
        if (!atomic_load(&s->lp_job->done)) return;
        struct sample *t = lp_join(&s->lp_job);
        recorder_discard(r);
        s->lp_state = LP_IDLE;
        if (t) looper_drop_in(s, u, t, -s->lp_pitch);
        else os_toast(s, u, "Out of memory");
        return;
    }
    // AFTER 1ST LOOP (or no memory for the stream): the take as it is now, converted while it plays.
    struct sample *raw = recorder_raw(r, lp_format(s).mode, skip, s->lp_length);
    recorder_discard(r);
    s->lp_state = LP_IDLE;
    if (!raw) {
        os_toast(s, u, "Out of memory");
        return;
    }
    if (s->lp_conv) looper_swap(s, u); // the one before, finished first
    looper_drop_in(s, u, raw, 0);
    if (s->pads[s->lp_slot].s != raw) return; // kept in memory instead
    struct lp_job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->src = raw, j->f = lp_format(s), j->slot = s->lp_slot;
    raw->refs++; // held while it is converted, even if the pad lets go
    s->lp_conv = lp_start(lp_convert_thread, j);
    if (!s->lp_conv) os_sample_release(s, raw);
}

// The Live Looper page: its settings while idle; while a take is under way, where it is.
static void looper_frame(struct os *s, struct ui *u, const struct input *in) {
    struct rec_settings *set = &s->rec_set[5];
    int busy = s->lp_state != LP_IDLE;
    int nf, list[PAGE_FIELDS];
    nf = fields_of(s, 5, list);
    if (!busy) {
        if (input_down(in, IN_B1)) { // <Back>: Audiotrack Record
            s->record_page = 1;
            s->cursor_record = 0;
            ui_wake(u, 1);
            return;
        }
        if (input_down(in, IN_B3)) looper_start(s, u);
        else if (input_down(in, IN_B2)) looper_delete(s, u); // <Del Take>
        if (s->cursor_record >= nf) s->cursor_record = 0;
        if (in->encoder_press) s->cursor_record = (s->cursor_record + (in->shift ? nf - 1 : 1)) % nf;
        int kind = list[s->cursor_record];
        int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
        if (d && s->lp_state == LP_IDLE) change(s, set, kind, d);
        if (in->fader_moved && kind == F_RATE) set->rate = CLASSIC[in->fader * NCLASSIC / 128];
    } else if (input_down(in, IN_B3)) {
        looper_abort(s);
    }
    busy = s->lp_state != LP_IDLE;
    if (!busy) {
        lcd_text(u, 0, KEY_ROW, "<Back>");
        if (s->lp_ntakes > 0) lcd_text(u, 8, KEY_ROW, "<Del Take>");
        lcd_text(u, 20, KEY_ROW, "<Rec>");
    } else if (s->lp_state != LP_ABORT) {
        lcd_text(u, 20, KEY_ROW, "<Stop>");
    }
    lcd_rule(u, TITLE_ROW, TITLES[5]);
    for (int i = 0, row = 3; i < nf; i++, row++) {
        char b[LCD_COLS + 1];
        lcd_text(u, 0, row, KINDS[list[i]].label);
        value(s, set, list[i], b, sizeof b);
        if (i == s->cursor_record && !busy) lcd_inverse(u, KINDS[list[i]].col, row, b);
        else lcd_text(u, KINDS[list[i]].col, row, b);
    }
    char line[LCD_COLS + 8];
    if (s->lp_state == LP_REC && atomic_load(&s->rec.state) == REC_RECORDING) {
        long left = s->lp_length - (atomic_load(&s->rec.frames) - (long)(s->lp_zero - s->rec.first_clock) - s->lp_latency);
        if (left < 0) left = 0;
        long tenths = left * 10 / ENGINE_RATE;
        snprintf(line, sizeof line, "Loop Rec  %ld.%lds to go", tenths / 10, tenths % 10);
        lcd_rule(u, METER_ROW, line);
        draw_wave(u, s, 0);
    } else {
        lcd_rule(u, METER_ROW, s->lp_state == LP_COUNT ? "Count In" : s->lp_state == LP_WAIT ? "Waiting for bar 1"
                               : s->lp_state == LP_ENDING ? "Dropping in"
                               : "Meter");
        draw_meter(u, s, THRES_OFF);
    }
    ui_wake(u, 40);
}

// ---- Stems: one stem of the pad's zone, separated in the background ----

struct stem_job {
    pthread_t thread;
    atomic_int done;
    struct umx_model *m;
    float *in, *out;
    long n;
    int remove, rc, mono;
    double started;
    char name[40];
};

static double seconds_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void *stem_thread(void *a) {
    struct stem_job *j = a;
    int threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    j->rc = umx_separate(j->m, j->in, j->n, j->out, j->remove, threads > 0 ? threads : 1, 0);
    atomic_store(&j->done, 1);
    return 0;
}

// The model for a stem, kept loaded for the next run of the same one. Music is the vocals
// model's mix less the vocals.
static struct umx_model *stem_model(struct os *s, int kind) {
    static const char *const file[STEM_KINDS] = {"drums", "bass", "vocals", "vocals"};
    int which = kind == STEM_MUSIC ? STEM_VOCALS : kind;
    if (s->stem_model && s->stem_model_kind == which) return s->stem_model;
    umx_free(s->stem_model);
    s->stem_model = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/umxhq-%s.bin", s->stems_dir ? s->stems_dir : ".", file[which]);
    s->stem_model = umx_load(path);
    s->stem_model_kind = which;
    return s->stem_model;
}

static void stems_start(struct os *s, struct ui *u) {
    const struct os_pad *p = &s->pads[s->selected];
    if (!p->s) {
        os_toast(s, u, "Pad is empty");
        return;
    }
    const struct sample *src = p->s;
    long a = p->p.start, b = p->p.end > src->frames ? src->frames : p->p.end;
    if (b - a > (long)STEMS_MAX_SECONDS * src->rate) { // the zone is what is separated
        s->stem_too_long = 1;
        return;
    }
    struct umx_model *m = stem_model(s, s->stem_kind);
    if (!m) {
        os_toast(s, u, "No stem models on the card");
        return;
    }
    // The zone at 44.1 kHz, as the model was trained, in stereo.
    struct sample *zone = sample_new((int32_t)(b - a > 0 ? b - a : 1), src->channels, src->rate), *at44 = 0;
    struct stem_job *j = calloc(1, sizeof *j);
    if (zone) memcpy(zone->data, src->data + a * src->channels, sizeof(float) * (size_t)zone->frames * (size_t)src->channels);
    if (zone && src->rate != ENGINE_RATE) {
        struct rec_format f = {REC_STEREO, ENGINE_RATE, 16, 1, 0, 0, 0, 0};
        at44 = rec_convert(zone, &f);
    }
    const struct sample *use = at44 ? at44 : zone;
    if (j && use) {
        j->n = use->frames;
        j->in = malloc(sizeof(float) * 2 * (size_t)j->n);
        j->out = malloc(sizeof(float) * 2 * (size_t)j->n);
    }
    if (!j || !use || !j->in || !j->out) {
        sample_free(zone), sample_free(at44);
        if (j) free(j->in), free(j->out);
        free(j);
        os_toast(s, u, "Out of memory");
        return;
    }
    for (long i = 0; i < j->n; i++)
        for (int c = 0; c < 2; c++) j->in[i * 2 + c] = use->data[i * use->channels + (use->channels == 2 ? c : 0)];
    sample_free(zone), sample_free(at44);
    static const char *const keep[STEM_KINDS] = {"Drums", "Bass", "Vocals", "Music"};
    static const char *const lose[STEM_KINDS] = {"NoDrums", "NoBass", "NoVocals", "Vocals"};
    snprintf(j->name, sizeof j->name, "%.20s-%s", p->name, s->stem_remove ? lose[s->stem_kind] : keep[s->stem_kind]);
    // Music keeps the mix less the vocals: the vocals model, the other way round.
    j->remove = s->stem_kind == STEM_MUSIC ? !s->stem_remove : s->stem_remove;
    j->mono = src->channels == 1;
    j->m = m;
    j->started = seconds_now();
    if (pthread_create(&j->thread, 0, stem_thread, j) != 0) stem_thread(j), j->thread = 0;
    s->stem_job = j;
}

// The separation finished: the stem as a sound, waiting for a pad or ENTER like a take.
static void stems_finish(struct os *s, struct ui *u) {
    struct stem_job *j = s->stem_job;
    if (j->thread) pthread_join(j->thread, 0);
    s->stem_job = 0;
    struct sample *n = j->rc == 0 ? sample_new((int32_t)j->n, j->mono ? 1 : 2, ENGINE_RATE) : 0;
    if (n)
        for (long i = 0; i < j->n; i++) {
            if (j->mono) n->data[i] = 0.5f * (j->out[i * 2] + j->out[i * 2 + 1]);
            else n->data[i * 2] = j->out[i * 2], n->data[i * 2 + 1] = j->out[i * 2 + 1];
        }
    if (!n) {
        os_toast(s, u, "Out of memory");
    } else {
        s->rec_result = n;
        s->rec_sync = 0;
        s->rec_previewed = 0;
        s->rec_result_pitch = 0;
        s->rec_result_counted = 0;
        snprintf(s->rec_result_name, sizeof s->rec_result_name, "%s", j->name);
        char msg[32];
        snprintf(msg, sizeof msg, "Done in %.1f s", seconds_now() - j->started);
        os_toast(s, u, msg);
    }
    free(j->in), free(j->out), free(j);
}

void record_frame(struct os *s, struct ui *u, const struct input *in) {
    struct recorder *r = &s->rec;
    if (s->stem_job) { // separating: nothing to do but wait, and show how long so far
        if (atomic_load(&s->stem_job->done)) {
            stems_finish(s, u);
        } else {
            char line[LCD_COLS + 8];
            lcd_rule(u, TITLE_ROW, TITLES[3]);
            snprintf(line, sizeof line, "Separating %s...", STEM_NAMES[s->stem_kind]);
            lcd_center(u, 6, line);
            snprintf(line, sizeof line, "%.1f s", seconds_now() - s->stem_job->started);
            lcd_center(u, 8, line);
            lcd_rule(u, BOTTOM_ROW, 0);
            ui_wake(u, 100);
            return;
        }
    }
    if (s->stem_too_long) { // the 20 second limit, and how to get under it
        if (input_down(in, IN_B3) || input_down(in, IN_B1) || in->encoder_press) s->stem_too_long = 0;
        lcd_text(u, 22, KEY_ROW, "<OK>");
        lcd_rule(u, TITLE_ROW, TITLES[3]);
        lcd_inverse(u, 0, 5, " 20 seconds is the maximum");
        lcd_inverse(u, 0, 6, " Tip: Convert it with     ");
        lcd_inverse(u, 0, 7, " Pitch +12 to halve its   ");
        lcd_inverse(u, 0, 8, " length, then separate.   ");
        lcd_rule(u, BOTTOM_ROW, 0);
        return;
    }
    struct rec_settings *set = &s->rec_set[s->record_page];
    recorder_service(r);
    watch_audio_track(s);
    if (s->record_page == 5) {
        looper_frame(s, u, in);
        return;
    }
    int state = atomic_load(&r->state);
    if (state == REC_IDLE && s->record_page <= 1) atomic_store(&r->source, 0); // the meter: the input,
    if (state == REC_IDLE && s->record_page == 4) atomic_store(&r->source, 1); // or on Resample the main output
    if (state == REC_DONE && !s->rec_result && s->lp_state == LP_IDLE) take_done(s, u); // the Live Looper's is its own
    int waiting = s->rec_result != 0;
    if (waiting) { // the prompt: a pad assigns (in record_pad), <Play> previews, ENTER or <Keep> keeps
        if (input_down(in, IN_B1)) { // <Cancel>
            if (s->rec_previewed) engine_retire(s->engine, s->rec_result); // freed once the engine lets go
            else sample_free(s->rec_result);
            s->rec_result = 0;
        } else if (input_down(in, IN_B2)) { // <Play>: as the pad will play it
            struct pad_params p;
            memset(&p, 0, sizeof p);
            p.end = s->rec_result->frames;
            p.volume = 100;
            p.cutoff = 100;
            p.mono = 1; // pressed again, it starts over
            p.semi = -s->rec_result_pitch;
            if (engine_note_on(s->engine, TAKE_PAD, 127, s->rec_result, &p) == 0) s->rec_previewed = 1;
        } else if (input_down(in, IN_B3) || in->encoder_press) {
            os_undo_save(s, "Keep");
            if (os_memory_add(s, s->rec_result, s->rec_result_name) != 0) {
                os_toast(s, u, "Memory full");
            } else {
                s->rec_result = 0;
                s->rec_count += s->rec_result_counted;
                os_toast(s, u, "Kept in memory");
            }
        }
    } else if (state == REC_ARMED || state == REC_RECORDING) {
        if (input_down(in, IN_B3) && s->lp_state != LP_IDLE) { // the Live Looper's take
            looper_abort(s);
        } else if (input_down(in, IN_B3)) { // <Stop>, where <Rec> was: the take, and on Audiotrack Record the sequence
            recorder_stop(r);
            if (s->at_running) seq_stop(&s->seq, 1);
            s->at_running = 0;
        }
    } else {
        if (input_down(in, IN_B2) && s->record_page == 1) { // <Live Loop>
            s->record_page = 5;
            s->cursor_record = 0;
            ui_wake(u, 1);
            return;
        }
        if (input_down(in, IN_B3) && s->lp_state != LP_IDLE) {
            os_toast(s, u, "Live Looper busy");
        } else if (input_down(in, IN_B3) && s->record_page == 3) {
            stems_start(s, u);
            ui_wake(u, 1);
        } else if (input_down(in, IN_B3) && s->record_page == 2) {
            convert_pad(s, u);
        } else if (input_down(in, IN_B3)) { // <Rec>
            int resample = s->record_page == 4;
            if (!resample && !atomic_load(&r->present)) {
                os_toast(s, u, "No audio input");
            } else if (s->record_page == 1) {
                start_audio_track(s);
            } else if (resample && set->what) {
                start_resample_sequence(s);
            } else { // LIVE: the input, or on Resample the main output; at the threshold or at once
                int db = resample && set->start_force ? THRES_OFF : set->thres_db;
                atomic_store(&r->thru_resample, s->rec_set[4].thru_record);
                atomic_store(&r->source, resample);
                atomic_store(&r->threshold, db <= THRES_OFF ? 0 : (int)(32767.0 * pow(10.0, db / 20.0)));
                recorder_arm(r, max_frames(s));
            }
        }
        if (input_down(in, IN_B1)) { // <Exit>
            s->screen = s->record_return;
            ui_wake(u, 1);
            return;
        }
        int list[PAGE_FIELDS], nf = fields_of(s, s->record_page, list);
        if (s->cursor_record >= nf) s->cursor_record = 0;
        if (in->encoder_press) s->cursor_record = (s->cursor_record + (in->shift ? nf - 1 : 1)) % nf;
        int kind = list[s->cursor_record];
        int d = in->encoder + input_down(in, IN_PLUS) - input_down(in, IN_MINUS);
        if (d) change(s, set, kind, d);
        if (in->fader_moved && kind == F_RATE) // the slider jumps between the classic rates
            set->rate = CLASSIC[in->fader * NCLASSIC / 128];
    }
    state = atomic_load(&r->state);
    waiting = s->rec_result != 0;

    if (waiting) {
        lcd_text(u, 0, KEY_ROW, "<Cancel>");
        lcd_text(u, 10, KEY_ROW, "<Play>");
        lcd_text(u, 20, KEY_ROW, "<Keep>");
    } else if (state == REC_ARMED || state == REC_RECORDING) {
        lcd_text(u, 20, KEY_ROW, "<Stop>");
    } else {
        lcd_text(u, 0, KEY_ROW, "<Exit>");
        if (s->record_page == 1) lcd_text(u, 8, KEY_ROW, "<Live Loop>");
        lcd_text(u, s->record_page == 2 ? 17 : s->record_page == 3 ? 19 : 20, KEY_ROW,
                 s->record_page == 2 ? "<Convert>" : s->record_page == 3 ? "<Do It>" : "<Rec>");
    }
    lcd_rule(u, TITLE_ROW, TITLES[s->record_page]);
    int shown[PAGE_FIELDS], nshown = fields_of(s, s->record_page, shown);
    for (int i = 0, row = 3; i < nshown; i++) {
        int kind = shown[i], col = KINDS[kind].col;
        char b[LCD_COLS + 1];
        if (s->record_page == 4 && kind == F_THRES) { // Resample: the level beside Start: THRESHOLD
            row--;
            col = 20;
        } else {
            lcd_text(u, 0, row, KINDS[kind].label);
        }
        value(s, set, kind, b, sizeof b);
        if (i == s->cursor_record && state == REC_IDLE && !waiting) lcd_inverse(u, col, row, b);
        else lcd_text(u, col, row, b);
        row++;
    }
    char line[LCD_COLS + 8];
    if (waiting) {
        const struct sample *t = s->rec_result;
        long tenths = (long)t->frames * 10 / t->rate;
        snprintf(line, sizeof line, "%ld.%lds %d.%02dk %dbit", tenths / 10, tenths % 10, t->rate / 1000,
                 t->rate % 1000 / 10, set->bits);
        lcd_rule(u, METER_ROW, line);
        draw_wave(u, s, t);
        lcd_inverse(u, 0, 5, " Press pad to assign, or  ");
        lcd_inverse(u, 0, 6, " ENTER to keep in memory  ");
    } else if (state == REC_RECORDING && (!s->at_running || s->seq.zero_seen)) {
        long n = atomic_load(&r->frames), tenths = n * 10 / ENGINE_RATE, left = (r->max_frames - n) / ENGINE_RATE;
        snprintf(line, sizeof line, "Rec %ld:%02ld.%ld Left %ld:%02ld", tenths / 600, tenths / 10 % 60, tenths % 10,
                 left / 60, left % 60);
        lcd_rule(u, METER_ROW, line);
        draw_wave(u, s, 0);
        ui_wake(u, 40);
    } else if (s->record_page == 2 || s->record_page == 3) { // the sound to convert or separate
        const struct sample *src = s->pads[s->selected].s;
        if (src) {
            long tenths = (long)src->frames * 10 / src->rate;
            snprintf(line, sizeof line, "%ld.%lds %d.%02dk", tenths / 10, tenths % 10, src->rate / 1000, src->rate % 1000 / 10);
            lcd_rule(u, METER_ROW, line);
            draw_wave(u, s, src);
        } else {
            lcd_rule(u, METER_ROW, "Pad is empty");
        }
    } else {
        int resample = s->record_page == 4;
        lcd_rule(u, METER_ROW, s->at_running ? (resample ? "Starting" : "Count In")
                               : state == REC_ARMED ? "Waiting for threshold" : resample ? "Main Out" : "Meter");
        draw_meter(u, s, s->record_page == 0 || (resample && !set->start_force && !set->what) ? set->thres_db : THRES_OFF);
        ui_wake(u, 40);
    }
}
