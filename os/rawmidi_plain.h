// A MIDI port read as plain bytes. The kernel keeps a rawmidi input's "framing" setting on the
// port itself, not on whoever opened it (sound/core/rawmidi.c sets substream->framing only in
// the PARAMS ioctl, never on open or close). The stock app turns on timestamp framing (32-byte
// frames: type, length, 2 reserved, nanoseconds, seconds, 16 data bytes); when it gets to open
// the panel before our start script stops it, the port stays framed for us, and the timestamps
// read as random notes, buttons and wheel steps (seen in a tester's mpc-os.log, 2026-11-27).
// So every input port is set back to no framing when it is opened.
#ifndef RAWMIDI_PLAIN_H
#define RAWMIDI_PLAIN_H
#include <sound/asound.h>
#include <string.h>
#include <sys/ioctl.h>

// 0 when the port now gives plain bytes; -1 when the kernel would not take the setting.
static inline int rawmidi_plain(int fd) {
    struct snd_rawmidi_params p;
    memset(&p, 0, sizeof p);
    p.stream = SNDRV_RAWMIDI_STREAM_INPUT;
    p.buffer_size = 4096; // the kernel's own default
    p.avail_min = 1;
    p.mode = SNDRV_RAWMIDI_MODE_FRAMING_NONE | SNDRV_RAWMIDI_MODE_CLOCK_NONE;
    return fd >= 0 && ioctl(fd, SNDRV_RAWMIDI_IOCTL_PARAMS, &p) == 0 ? 0 : -1;
}
#endif
