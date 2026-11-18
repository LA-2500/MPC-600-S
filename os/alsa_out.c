#include "alsa_out.h"
#include "fx.h"
#include "os.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

#define SOUND_LINK "/dev/snd/by-path/platform-sound"
#define RATE ENGINE_RATE
#define MAX_PERIOD 1024
#define MAX_CHANNELS 4
#define RT_PRIORITY 80

static struct {
    int fd, channels, period;
    int in_fd, in_channels; // the audio input, read a period at a time before each output period
    struct recorder *rec;
    struct master *master;
    struct engine *engine;
    _Atomic float *main_gain;
    pthread_t thread;
    atomic_int stop;
    atomic_uint xruns;
} out = {.fd = -1, .in_fd = -1};

// Whether the built-in speaker is playing, and whether the built-in mic is the input. Input
// Thru plays through the speaker, except from the mic, which would hear it and feed back.
static atomic_int speaker_live = 1, mic_live;

static void mask_only(struct snd_pcm_hw_params *hw, int param, unsigned value) {
    struct snd_mask *m = &hw->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof *m);
    m->bits[value >> 5] = 1u << (value & 31);
}

static struct snd_interval *interval(struct snd_pcm_hw_params *hw, int param) {
    return &hw->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void interval_only(struct snd_pcm_hw_params *hw, int param, unsigned value) {
    struct snd_interval *i = interval(hw, param);
    memset(i, 0, sizeof *i);
    i->min = i->max = value;
    i->integer = 1;
}

static void hw_any(struct snd_pcm_hw_params *hw) {
    memset(hw, 0, sizeof *hw);
    memset(hw->masks, 0xff, sizeof hw->masks);
    for (int p = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; p <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; p++) {
        struct snd_interval *i = interval(hw, p);
        i->min = 0;
        i->max = UINT_MAX;
    }
    hw->rmask = ~0u;
    hw->info = ~0u;
}

static int configure_as(int fd, int channels, int period, int periods, int capture, unsigned format) {
    struct snd_pcm_hw_params hw;
    hw_any(&hw);
    mask_only(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    mask_only(&hw, SNDRV_PCM_HW_PARAM_FORMAT, format);
    mask_only(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, (unsigned)SNDRV_PCM_SUBFORMAT_STD);
    interval_only(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, (unsigned)channels);
    interval_only(&hw, SNDRV_PCM_HW_PARAM_RATE, RATE);
    interval_only(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, (unsigned)period);
    interval_only(&hw, SNDRV_PCM_HW_PARAM_PERIODS, (unsigned)periods);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) != 0) return -1;

    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof sw);
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = (snd_pcm_uframes_t)period;
    sw.xfer_align = 1;
    sw.start_threshold = capture ? 1 : (snd_pcm_uframes_t)period * 2; // the input starts on the first read
    sw.stop_threshold = (snd_pcm_uframes_t)period * (snd_pcm_uframes_t)periods;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) != 0) return -1;
    return ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
}

static int configure(int fd, int channels, int period, int periods, int capture) {
    return configure_as(fd, channels, period, periods, capture, (unsigned)SNDRV_PCM_FORMAT_S32_LE);
}

static int find_card(void) {
    char link[64];
    ssize_t n = readlink(SOUND_LINK, link, sizeof link - 1);
    if (n <= 0) return -1;
    link[n] = 0;
    char *c = strstr(link, "controlC");
    if (!c) return -1;
    int card = 0;
    for (c += 8; *c >= '0' && *c <= '9'; c++) card = card * 10 + (*c - '0');
    return card;
}

static void nap_ms(int ms) {
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&t, 0);
}

// The stock app may still be closing the device when we start.
static int open_pcm(int card, int capture) {
    char path[32];
    snprintf(path, sizeof path, "/dev/snd/pcmC%dD0%c", card, capture ? 'c' : 'p');
    for (int i = 0; i < 50; i++) {
        int fd = open(path, (capture ? O_RDONLY : O_WRONLY) | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL);
            if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
            return fd;
        }
        if (errno != EBUSY) return -1;
        nap_ms(100);
    }
    return -1;
}

static inline int32_t to_s32(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return (int32_t)(x * 2147483392.0f);
}

// Returns 0, or -1 for an error the stream cannot recover from.
static int write_frames(const int32_t *buf, int frames) {
    while (frames > 0) {
        struct snd_xferi x = {.buf = (void *)buf, .frames = (snd_pcm_uframes_t)frames};
        if (ioctl(out.fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) {
            int done = (int)x.result;
            if (done <= 0) return 0;
            buf += done * out.channels;
            frames -= done;
            continue;
        }
        if (errno == EINTR || errno == EAGAIN) continue;
        if (errno == EPIPE) {
            atomic_fetch_add(&out.xruns, 1u);
            if (ioctl(out.fd, SNDRV_PCM_IOCTL_PREPARE) != 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static void flush_denormals(void) {
#ifdef __aarch64__
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= 1ull << 24; // FZ
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}

// One period of input, as stereo floats. 0, or -1 when the input has failed for good.
static int read_input(int32_t *raw, float *in) {
    int frames = out.period;
    int32_t *at = raw;
    while (frames > 0) {
        struct snd_xferi x = {.buf = at, .frames = (snd_pcm_uframes_t)frames};
        if (ioctl(out.in_fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x) == 0) {
            int done = (int)x.result;
            if (done <= 0) break;
            at += done * out.in_channels;
            frames -= done;
            continue;
        }
        if (errno == EINTR || errno == EAGAIN) continue;
        if (errno == EPIPE) { // an overrun: start again, the gap reads as silence
            atomic_fetch_add(&out.xruns, 1u);
            if (ioctl(out.in_fd, SNDRV_PCM_IOCTL_PREPARE) != 0) return -1;
            memset(at, 0, (size_t)frames * (size_t)out.in_channels * sizeof *at);
            break;
        }
        return -1;
    }
    for (int i = 0; i < out.period; i++) {
        in[i * 2] = (float)raw[i * out.in_channels] / 2147483648.0f;
        in[i * 2 + 1] = (float)raw[i * out.in_channels + 1] / 2147483648.0f;
    }
    return 0;
}


// ---- USB audio in: the computer's audio, from the USB audio gadget ----
//
// The gadget runs on the computer's clock, not the codec's, so it has its own thread, which
// reads it into a ring. The audio thread takes a period from the ring each time round, reading
// it a hair faster or slower (a few hundred ppm at most, by linear interpolation) to keep the
// ring half full: the two clocks drift apart, and this follows the drift without a click.

#define USB_RING 8192 // frames, a power of two
#define USB_FILL 1024 // the fill the reader steers to: about 23 ms
static struct {
    atomic_int run, live, present;
    int fd, format, bytes;
    pthread_t thread;
    float ring[USB_RING * 2];
    atomic_uint written; // frames written, all told
    double read;         // where the audio thread reads, in frames all told
    double ratio;        // how far it moves for each output frame
} usb = {.fd = -1, .ratio = 1.0};

static int usb_card(void) {
    char link[64];
    ssize_t n = readlink("/proc/asound/UAC2Gadget", link, sizeof link - 1); // "card3"
    if (n <= 0) return -1;
    link[n] = 0;
    return strncmp(link, "card", 4) == 0 ? atoi(link + 4) : -1;
}

static void *usb_thread(void *arg) {
    (void)arg;
    unsigned char raw[256 * 2 * 4];
    while (atomic_load(&usb.run)) {
        struct pollfd p = {.fd = usb.fd, .events = POLLIN};
        if (poll(&p, 1, 100) <= 0) continue; // nothing from the computer: try again, so it can stop
        struct snd_xferi x = {.buf = raw, .frames = 256};
        if (ioctl(usb.fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x) != 0) {
            if (errno == EPIPE) ioctl(usb.fd, SNDRV_PCM_IOCTL_PREPARE);
            else if (errno != EAGAIN && errno != EINTR) nap_ms(10);
            continue;
        }
        unsigned w = atomic_load_explicit(&usb.written, memory_order_relaxed);
        for (int i = 0; i < (int)x.result; i++) {
            float v[2];
            for (int c = 0; c < 2; c++) {
                const unsigned char *s = raw + (i * 2 + c) * usb.bytes;
                int32_t a = usb.bytes == 2 ? (int32_t)(int16_t)(s[0] | s[1] << 8) << 16
                          : usb.bytes == 3 ? (int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24)
                                           : (int32_t)((uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24);
                v[c] = (float)a / 2147483648.0f;
            }
            unsigned at = (w + (unsigned)i) & (USB_RING - 1);
            usb.ring[at * 2] = v[0], usb.ring[at * 2 + 1] = v[1];
        }
        atomic_store_explicit(&usb.written, w + (unsigned)x.result, memory_order_release);
    }
    return 0;
}

static int usb_open(void) {
    if (usb.fd >= 0) return 0;
    int card = usb_card();
    if (card < 0) return -1;
    int fd = open_pcm(card, 1);
    if (fd < 0) return -1;
    static const struct { unsigned format; int bytes; } FORMATS[] = {
        {SNDRV_PCM_FORMAT_S16_LE, 2}, {SNDRV_PCM_FORMAT_S24_3LE, 3}, {SNDRV_PCM_FORMAT_S32_LE, 4},
    };
    int ok = 0;
    for (size_t i = 0; i < sizeof FORMATS / sizeof FORMATS[0] && !ok; i++) {
        if (configure_as(fd, 2, 256, 8, 1, FORMATS[i].format) == 0) usb.bytes = FORMATS[i].bytes, ok = 1;
        else ioctl(fd, SNDRV_PCM_IOCTL_HW_FREE);
    }
    if (!ok) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    usb.fd = fd;
    atomic_store(&usb.written, 0u);
    usb.read = 0, usb.ratio = 1.0;
    atomic_store(&usb.run, 1);
    if (pthread_create(&usb.thread, 0, usb_thread, 0) != 0) {
        close(fd);
        usb.fd = -1;
        return -1;
    }
    ioctl(fd, SNDRV_PCM_IOCTL_START);
    return 0;
}

static void usb_close(void) {
    if (usb.fd < 0) return;
    atomic_store(&usb.live, 0);
    atomic_store(&usb.run, 0);
    pthread_join(usb.thread, 0);
    ioctl(usb.fd, SNDRV_PCM_IOCTL_DROP);
    close(usb.fd);
    usb.fd = -1;
}

// A period from the ring into in (stereo), steering the ring's fill to USB_FILL. Silence while
// the computer sends nothing.
static void usb_take(float *in, int frames) {
    unsigned w = atomic_load_explicit(&usb.written, memory_order_acquire);
    double fill = (double)w - usb.read;
    if (fill < frames + 2 || fill > USB_RING - frames * 2) { // starved or overrun: start again at the fill
        memset(in, 0, (size_t)frames * 2 * sizeof *in);
        if (fill > USB_RING - frames * 2 || (double)w > USB_FILL) usb.read = (double)w - USB_FILL;
        if (usb.read < 0) usb.read = 0;
        usb.ratio = 1.0;
        return;
    }
    double err = (fill - USB_FILL) / USB_FILL; // above: read faster
    double want = 1.0 + (err > 1 ? 1 : err < -1 ? -1 : err) * 0.0005;
    usb.ratio += (want - usb.ratio) * 0.01; // a slow glide: no audible wobble
    for (int i = 0; i < frames; i++) {
        unsigned a = (unsigned)usb.read, b = a + 1;
        float t = (float)(usb.read - floor(usb.read));
        const float *pa = &usb.ring[(a & (USB_RING - 1)) * 2], *pb = &usb.ring[(b & (USB_RING - 1)) * 2];
        in[i * 2] = pa[0] + t * (pb[0] - pa[0]);
        in[i * 2 + 1] = pa[1] + t * (pb[1] - pa[1]);
        usb.read += usb.ratio;
    }
}

// ---- USB audio out: the master mix to the computer, through the same gadget ----
//
// The gadget's playback stream is what the computer records. The audio thread copies each
// finished period (after Master FX, before the main volume: the phones mix) into a ring; this
// thread feeds the gadget from it on the computer's clock, reading a hair faster or slower to
// keep the ring half full, as the input does. With no computer recording, the ring simply
// runs on and nothing waits. The audio thread never touches the gadget.

static struct {
    atomic_int on, run;
    int fd, bytes;
    pthread_t thread;
    int started;
    float ring[USB_RING * 2];
    atomic_uint written; // frames the audio thread has put in, all told
    double read, ratio;
} usbo = {.fd = -1, .ratio = 1.0};

static void usbo_put(const float *mix, int frames) { // the audio thread: a period in, no waiting
    unsigned w = atomic_load_explicit(&usbo.written, memory_order_relaxed);
    for (int i = 0; i < frames; i++) {
        unsigned at = (w + (unsigned)i) & (USB_RING - 1);
        usbo.ring[at * 2] = mix[i * 2], usbo.ring[at * 2 + 1] = mix[i * 2 + 1];
    }
    atomic_store_explicit(&usbo.written, w + (unsigned)frames, memory_order_release);
}

static int usbo_open(void) {
    int card = usb_card();
    if (card < 0) return -1;
    int fd = open_pcm(card, 0);
    if (fd < 0) return -1;
    static const struct { unsigned format; int bytes; } FORMATS[] = {
        {SNDRV_PCM_FORMAT_S16_LE, 2}, {SNDRV_PCM_FORMAT_S24_3LE, 3}, {SNDRV_PCM_FORMAT_S32_LE, 4},
    };
    int ok = 0;
    for (size_t i = 0; i < sizeof FORMATS / sizeof FORMATS[0] && !ok; i++) {
        if (configure_as(fd, 2, 256, 8, 0, FORMATS[i].format) == 0) usbo.bytes = FORMATS[i].bytes, ok = 1;
        else ioctl(fd, SNDRV_PCM_IOCTL_HW_FREE);
    }
    if (!ok) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    usbo.fd = fd;
    unsigned w = atomic_load(&usbo.written);
    usbo.read = (double)w - USB_FILL < 0 ? 0 : (double)w - USB_FILL;
    usbo.ratio = 1.0;
    return 0;
}

static void usbo_close_fd(void) {
    if (usbo.fd < 0) return;
    ioctl(usbo.fd, SNDRV_PCM_IOCTL_DROP);
    close(usbo.fd);
    usbo.fd = -1;
}

// A period from the ring, steering its fill to USB_FILL (the reader here is the computer).
static void usbo_take(float *buf, int frames) {
    unsigned w = atomic_load_explicit(&usbo.written, memory_order_acquire);
    double fill = (double)w - usbo.read;
    if (fill < frames + 2 || fill > USB_RING - frames * 2) { // starved or overrun: start again at the fill
        memset(buf, 0, (size_t)frames * 2 * sizeof *buf);
        usbo.read = (double)w - USB_FILL < 0 ? 0 : (double)w - USB_FILL;
        usbo.ratio = 1.0;
        return;
    }
    double err = (fill - USB_FILL) / USB_FILL; // above: read faster
    double want = 1.0 + (err > 1 ? 1 : err < -1 ? -1 : err) * 0.0005;
    usbo.ratio += (want - usbo.ratio) * 0.01;
    for (int i = 0; i < frames; i++) {
        unsigned a = (unsigned)usbo.read, b = a + 1;
        float t = (float)(usbo.read - floor(usbo.read));
        const float *pa = &usbo.ring[(a & (USB_RING - 1)) * 2], *pb = &usbo.ring[(b & (USB_RING - 1)) * 2];
        buf[i * 2] = pa[0] + t * (pb[0] - pa[0]);
        buf[i * 2 + 1] = pa[1] + t * (pb[1] - pa[1]);
        usbo.read += usbo.ratio;
    }
}

static void *usbo_thread(void *arg) {
    (void)arg;
    float f[256 * 2];
    unsigned char raw[256 * 2 * 4];
    while (atomic_load(&usbo.run)) {
        if (!atomic_load(&usbo.on)) { // switched off: let the gadget go
            usbo_close_fd();
            nap_ms(200);
            continue;
        }
        if (usbo.fd < 0 && usbo_open() != 0) { // no computer, or the gadget away (USB card mode): later
            nap_ms(1000);
            continue;
        }
        struct pollfd p = {.fd = usbo.fd, .events = POLLOUT};
        if (poll(&p, 1, 100) <= 0) { // the computer is not taking any: keep the ring's read up to date
            unsigned w = atomic_load(&usbo.written);
            if ((double)w - usbo.read > USB_RING / 2) usbo.read = (double)w - USB_FILL;
            continue;
        }
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (ioctl(usbo.fd, SNDRV_PCM_IOCTL_PREPARE) != 0) usbo_close_fd();
            continue;
        }
        usbo_take(f, 256);
        for (int i = 0; i < 256 * 2; i++) {
            float x = f[i] > 1.0f ? 1.0f : f[i] < -1.0f ? -1.0f : f[i];
            int32_t a = (int32_t)(x * 2147483392.0f);
            unsigned char *d = raw + i * usbo.bytes;
            if (usbo.bytes == 2) d[0] = (unsigned char)(a >> 16), d[1] = (unsigned char)(a >> 24);
            else if (usbo.bytes == 3) d[0] = (unsigned char)(a >> 8), d[1] = (unsigned char)(a >> 16), d[2] = (unsigned char)(a >> 24);
            else d[0] = (unsigned char)a, d[1] = (unsigned char)(a >> 8), d[2] = (unsigned char)(a >> 16), d[3] = (unsigned char)(a >> 24);
        }
        struct snd_xferi x = {.buf = raw, .frames = 256};
        if (ioctl(usbo.fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) != 0) {
            if (errno == EPIPE) ioctl(usbo.fd, SNDRV_PCM_IOCTL_PREPARE); // the computer fell behind: go on
            else if (errno != EAGAIN && errno != EINTR) usbo_close_fd();  // the gadget went: open it again later
        }
    }
    usbo_close_fd();
    return 0;
}

void alsa_out_set_usb_out(int on) {
    atomic_store(&usbo.on, on != 0);
    if (!usbo.started) { // the thread starts the first time, and then follows the switch
        atomic_store(&usbo.run, 1);
        if (pthread_create(&usbo.thread, 0, usbo_thread, 0) == 0) usbo.started = 1;
    }
}

static void *audio_thread(void *arg) {
    (void)arg;
    flush_denormals();
    float mix[MAX_PERIOD * 2], in[MAX_PERIOD * 2];
    int32_t pcm[MAX_PERIOD * MAX_CHANNELS], raw[MAX_PERIOD * MAX_CHANNELS];
    float gain = atomic_load(out.main_gain);
    while (!atomic_load_explicit(&out.stop, memory_order_relaxed)) {
        int have_input = 0, resample = out.rec && atomic_load_explicit(&out.rec->source, memory_order_relaxed);
        long long clock = atomic_load(&out.engine->clock);
        int from_usb = atomic_load_explicit(&usb.live, memory_order_relaxed);
        if (out.in_fd >= 0) { // the input and output share the codec's clock, so this paces both
            if (read_input(raw, in) == 0) {
                have_input = 1;
                if (from_usb) usb_take(in, out.period); // the computer's audio instead of the jacks
                else if (out.rec->fx && atomic_load_explicit(&out.rec->fx->inst, memory_order_relaxed))
                    fx_instrument(out.rec->fx, in, out.period); // a guitar or bass straight in: mono, levelled
                // Input Effects: the input as heard through Input Thru and as recorded.
                struct fx_chain *fx = out.rec->fx;
                if (fx && atomic_load_explicit(&fx->on, memory_order_relaxed)) fx_chain_process(fx, in, out.period);
                if (!resample) recorder_input(out.rec, in, out.period, clock);
            } else {
                close(out.in_fd);
                out.in_fd = -1;
                atomic_store(&out.rec->present, 0);
            }
        }
        engine_render(out.engine, mix, out.period);
        // Resample: the main output as it leaves the engine (Input Effects already on the input,
        // master FX will come after), without the input passing through.
        // Input Thru: the mic only to headphones or the line out, never the speaker (feedback).
        // While resampling, muted unless Resample's Input Thru says RECORD, when it goes in.
        int state = out.rec ? atomic_load_explicit(&out.rec->state, memory_order_relaxed) : REC_IDLE;
        int taking = resample && (state == REC_ARMED || state == REC_RECORDING);
        if (have_input && atomic_load_explicit(&out.rec->thru, memory_order_relaxed) &&
            !(atomic_load_explicit(&mic_live, memory_order_relaxed) && atomic_load_explicit(&speaker_live, memory_order_relaxed)) &&
            !(taking && !atomic_load_explicit(&out.rec->thru_resample, memory_order_relaxed)))
            for (int i = 0; i < out.period * 2; i++) mix[i] += in[i];
        if (resample) recorder_input(out.rec, mix, out.period, clock);
        if (out.master) master_process(out.master, mix, out.period); // last: never resampled
        if (atomic_load_explicit(&usbo.on, memory_order_relaxed)) usbo_put(mix, out.period); // to the computer, before the volume
        float target = atomic_load_explicit(out.main_gain, memory_order_relaxed);
        float step = (target - gain) / (float)out.period;
        for (int i = 0; i < out.period; i++) {
            gain += step;
            float l = mix[i * 2], r = mix[i * 2 + 1];
            int32_t *f = pcm + i * out.channels;
            f[0] = to_s32(l * gain);
            f[1] = to_s32(r * gain);
            // Channels 3-4 are the phones mix. Their level is not set by the main knob.
            if (out.channels == 4) {
                f[2] = to_s32(l);
                f[3] = to_s32(r);
            }
        }
        gain = target;
        if (write_frames(pcm, out.period) != 0) break;
    }
    return 0;
}

void alsa_out_set_master(struct master *m) {
    out.master = m;
}

static void start_thread(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    struct sched_param param = {.sched_priority = RT_PRIORITY};
    pthread_attr_setschedparam(&attr, &param);
    int rc = pthread_create(&out.thread, &attr, audio_thread, 0);
    pthread_attr_destroy(&attr);
    if (rc != 0) pthread_create(&out.thread, 0, audio_thread, 0);
}

// The speaker amp stays off until something sets this mixer switch. The stock app does it
// at startup, so a program that replaces the stock app has to do it as well.
static struct {
    int fd, speaker;
    int have_speaker, have_headphones, have_line, have_mic, have_trs;
    struct snd_ctl_elem_id speaker_id, headphones_id, line_id, mic_id, trs_id;
} mixer = {.fd = -1, .speaker = -1};

static void mixer_open(int card) {
    char path[32];
    snprintf(path, sizeof path, "/dev/snd/controlC%d", card);
    mixer.fd = open(path, O_RDWR | O_CLOEXEC);
    if (mixer.fd < 0) return;
    struct snd_ctl_elem_list list;
    memset(&list, 0, sizeof list);
    if (ioctl(mixer.fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) != 0 || list.count == 0) return;
    struct snd_ctl_elem_id *ids = calloc(list.count, sizeof *ids);
    if (!ids) return;
    list.space = list.count;
    list.pids = ids;
    if (ioctl(mixer.fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0) {
        for (unsigned i = 0; i < list.used; i++) {
            const char *name = (const char *)ids[i].name;
            if (strcmp(name, "Builtin Speaker Switch") == 0) mixer.speaker_id = ids[i], mixer.have_speaker = 1;
            if (strcmp(name, "Headphones Jack") == 0) mixer.headphones_id = ids[i], mixer.have_headphones = 1;
            if (strcmp(name, "Line Out Jack") == 0) mixer.line_id = ids[i], mixer.have_line = 1;
            if (strcmp(name, "Builtin Microphone Switch") == 0) mixer.mic_id = ids[i], mixer.have_mic = 1;
            if (strcmp(name, "TRS Switch") == 0) mixer.trs_id = ids[i], mixer.have_trs = 1;
        }
    }
    free(ids);
}

static int mixer_read(const struct snd_ctl_elem_id *id) {
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    v.id = *id;
    if (ioctl(mixer.fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) != 0) return 0;
    return v.value.integer.value[0] != 0;
}

static void mixer_write_speaker(int on) {
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    v.id = mixer.speaker_id;
    v.value.integer.value[0] = on;
    if (ioctl(mixer.fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) == 0) {
        mixer.speaker = on;
        atomic_store(&speaker_live, on);
    }
}

static void mixer_write(const struct snd_ctl_elem_id *id, int on) {
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    v.id = *id;
    v.value.integer.value[0] = on;
    ioctl(mixer.fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
}

int alsa_out_set_input(int src) {
    int mic = src == INPUT_MIC, rc = 0; // INST is the line jack, as LINE
    atomic_store(&mic_live, mic);
    if (src == INPUT_USB) { // the computer's audio; the jacks stay on the line input, unheard
        rc = usb_open();
        atomic_store(&usb.live, rc == 0);
    } else {
        usb_close();
    }
    if (mixer.fd < 0) return rc;
    // One or the other, never both: the switch being turned off goes first.
    if (mic) {
        if (mixer.have_trs) mixer_write(&mixer.trs_id, 0);
        if (mixer.have_mic) mixer_write(&mixer.mic_id, 1);
    } else {
        if (mixer.have_mic) mixer_write(&mixer.mic_id, 0);
        if (mixer.have_trs) mixer_write(&mixer.trs_id, 1);
    }
    return rc;
}

void alsa_out_speaker_update(void) {
    if (mixer.fd < 0 || !mixer.have_speaker) return;
    int plugged = (mixer.have_headphones && mixer_read(&mixer.headphones_id))
               || (mixer.have_line && mixer_read(&mixer.line_id));
    if (mixer.speaker != !plugged) mixer_write_speaker(!plugged);
}

int alsa_out_start(struct engine *e, _Atomic float *main_gain, struct recorder *rec, char *status, size_t status_len) {
    int card = find_card();
    if (card < 0) {
        snprintf(status, status_len, "no sound card at " SOUND_LINK);
        return -1;
    }
    out.fd = open_pcm(card, 0);
    if (out.fd < 0) {
        snprintf(status, status_len, "open card %d: %s", card, strerror(errno));
        return -1;
    }
    // The first entry is what the stock app opens on this hardware.
    static const int tries[][3] = {
        {2, 128, 3}, {2, 128, 4}, {2, 256, 3}, {2, 512, 3},
    };
    int ok = 0, err = 0;
    for (size_t i = 0; i < sizeof tries / sizeof tries[0] && !ok; i++) {
        if (configure(out.fd, tries[i][0], tries[i][1], tries[i][2], 0) == 0) {
            out.channels = tries[i][0];
            out.period = tries[i][1];
            snprintf(status, status_len, "card %d, %d Hz, S32, %d ch, %d x %d", card, RATE, tries[i][0], tries[i][1],
                     tries[i][2]);
            ok = 1;
        } else {
            err = errno;
            ioctl(out.fd, SNDRV_PCM_IOCTL_HW_FREE);
        }
    }
    if (!ok) {
        snprintf(status, status_len, "configure card %d: %s", card, strerror(err));
        close(out.fd);
        out.fd = -1;
        return -1;
    }
    mixer_open(card);
    alsa_out_speaker_update();
    // The input, at the output's rate and period. Two channels if it allows, else four.
    out.rec = rec;
    if (rec) {
        out.in_fd = open_pcm(card, 1);
        static const int in_channels[] = {2, 4};
        for (size_t i = 0; i < 2 && out.in_fd >= 0 && !out.in_channels; i++) {
            if (configure(out.in_fd, in_channels[i], out.period, 4, 1) == 0) out.in_channels = in_channels[i];
            else ioctl(out.in_fd, SNDRV_PCM_IOCTL_HW_FREE);
        }
        if (out.in_fd >= 0 && !out.in_channels) {
            close(out.in_fd);
            out.in_fd = -1;
        }
        atomic_store(&rec->present, out.in_fd >= 0);
        // Output to input: the periods queued to play, the period being read, and the converters.
        rec->latency = out.period * 3 + 40;
        size_t used = strlen(status);
        snprintf(status + used, status_len - used, ", input %s", out.in_fd >= 0 ? (out.in_channels == 2 ? "2 ch" : "4 ch") : "none");
    }
    size_t used = strlen(status);
    snprintf(status + used, status_len - used, ", speaker %s",
             !mixer.have_speaker ? "switch missing" : mixer.speaker < 0 ? "failed" : mixer.speaker ? "on" : "off");
    out.engine = e;
    out.main_gain = main_gain;
    atomic_store(&out.stop, 0);
    start_thread();
    return 0;
}

void alsa_out_stop(void) {
    usb_close();
    if (out.fd < 0) return;
    atomic_store(&out.stop, 1);
    pthread_join(out.thread, 0);
    ioctl(out.fd, SNDRV_PCM_IOCTL_DROP);
    close(out.fd);
    out.fd = -1;
    if (out.in_fd >= 0) {
        ioctl(out.in_fd, SNDRV_PCM_IOCTL_DROP);
        close(out.in_fd);
        out.in_fd = -1;
    }
    if (mixer.fd >= 0) {
        if (mixer.have_speaker) mixer_write_speaker(0);
        close(mixer.fd);
        mixer.fd = -1;
    }
}

int alsa_out_speaker_on(void) {
    return mixer.fd >= 0 && mixer.have_speaker ? mixer.speaker : -1;
}

unsigned alsa_out_xruns(void) {
    return atomic_load(&out.xruns);
}
