// The sequencer's screens: the main screen (Play/Record and Track Data pages) and
// Timing Correct, plus the transport buttons, which work from any screen.
#pragma once
#include "os.h"

void seq_go_main(struct os *s); // SEQ: opens the main screen, or flips its page
void seq_screen_frame(struct os *s, struct ui *u, const struct input *in);
void seq_tc_open(struct os *s);
void seq_tc_frame(struct os *s, struct ui *u, const struct input *in);
void seq_tc_pad(struct os *s, int note); // a pad hit picks the notes Move Existing works on
void seq_metro_frame(struct os *s, struct ui *u, const struct input *in);
void seq_repeat_open(struct os *s); // Shift + NOTE REPEAT: the Note Repeat page
void seq_repeat_frame(struct os *s, struct ui *u, const struct input *in);
void seq_program_frame(struct os *s, struct ui *u, const struct input *in);
int seq_program_pad(struct os *s, struct ui *u, int slot); // 1 when the hit finished a Copy or Swap
void seq_erase_open(struct os *s);
void seq_erase_frame(struct os *s, struct ui *u, const struct input *in);
void seq_erase_pad(struct os *s, int note); // a pad hit picks the notes to erase
void seq_copy_open(struct os *s);
void seq_copy_frame(struct os *s, struct ui *u, const struct input *in);
void seq_copy_pad(struct os *s, int pad); // a pad picks the sequence to copy to
void seq_copyev_open(struct os *s); // SHIFT + ERASE: Copy Events
void seq_copyev_frame(struct os *s, struct ui *u, const struct input *in);
void seq_copyev_pad(struct os *s, int note); // a pad hit picks the notes copied
void seq_copybars_open(struct os *s);
void seq_copybars_frame(struct os *s, struct ui *u, const struct input *in);
void seq_mixer_open(struct os *s);
void seq_mixer_frame(struct os *s, struct ui *u, const struct input *in);
void seq_mixer_pad(struct os *s, int pad); // a pad hit picks the channel: pad bank * 16 + pad
void seq_transport(struct os *s, struct ui *u, const struct input *in);
int seq_shift_pad(struct os *s, struct ui *u, int pad); // 1 when the pad is a sequencer shortcut
void seq_lights(struct ui *u, struct os *s);
