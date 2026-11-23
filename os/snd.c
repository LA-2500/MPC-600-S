// Akai .SND (as the MPC 2000 / 2000XL write it). The header, little-endian:
//   0     1
//   1     4 (an MPC 2000XL's; an MPC 2000's is 2)
//   2-17  name, 16 characters, padded with spaces
//   18    0
//   19    level 0..200 (100)
//   20    tune, signed, -120..120 (tenths of a semitone)
//   21    0 mono, 1 stereo
//   22    start (4 bytes)
//   26    end (4 bytes)
//   30    frames (4 bytes)
//   34    loop length (4 bytes): the loop starts at end - length
//   38    loop on
//   39    beats in the loop
//   40    sample rate (2 bytes)
//   42    the samples: 16-bit, a stereo sound's left channel and then its right
// The MPC 3000's is the same to byte 33, version 2, and its samples start at 38: it has no rate
// (always 44.1 kHz) and no loop. It writes bytes 34-37 as d0 07 01 00. Checked against an MPC 3000
// (OS 3.12, in MAME): it loads such a file with its length and tune, and saves one like it.
// Which header a file has: the one its length fits, frames x 2 bytes x channels after it.
#include "snd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SND_HEADER 42
#define SND_HEADER_3000 38

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(unsigned char *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }
static int32_t clamp32(int64_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : (int32_t)v; }

struct sample *snd_parse(const unsigned char *b, size_t len, struct snd_info *info) {
    if (len < SND_HEADER_3000 || b[0] != 1 || (b[1] != 4 && b[1] != 2 && b[1] != 3) || b[21] > 1) return NULL;
    int ch = b[21] ? 2 : 1;
    int64_t want = (int64_t)rd32(b + 30) * 2 * ch;
    int64_t n = (int64_t)len;
    int head = n == SND_HEADER_3000 + want ? SND_HEADER_3000 : n == SND_HEADER + want ? SND_HEADER
             : b[1] == 2 ? SND_HEADER_3000 : SND_HEADER; // the length decides; else the version
    if (len < (size_t)head) return NULL;
    int m3000 = head == SND_HEADER_3000;
    int64_t have = (int64_t)(len - head) / (2 * ch), frames = rd32(b + 30);
    if (frames <= 0 || frames > have) frames = have; // what the file holds, whatever the header says
    if (frames <= 0) return NULL;
    int rate = m3000 ? 44100 : b[40] | b[41] << 8;
    if (rate < 4000) rate = 44100;
    struct sample *s = sample_new((int32_t)frames, ch, rate);
    if (!s) return NULL;
    const unsigned char *d = b + head;
    for (int c = 0; c < ch; c++)
        for (int64_t i = 0; i < frames; i++) {
            const unsigned char *p = d + 2 * (c * frames + i);
            s->data[i * ch + c] = (int16_t)(p[0] | p[1] << 8) / 32768.0f;
        }
    if (info) {
        memset(info, 0, sizeof *info);
        memcpy(info->name, b + 2, 16);
        for (int i = 15; i >= 0 && (info->name[i] == ' ' || !info->name[i]); i--) info->name[i] = 0;
        for (int i = 0; info->name[i]; i++)
            if ((unsigned char)info->name[i] < 32 || (unsigned char)info->name[i] > 126) info->name[i] = '_';
        info->level = b[19] <= 200 ? b[19] : 100;
        info->tune = clamp32((int8_t)b[20], -120, 120);
        info->end = clamp32(rd32(b + 26), 1, (int32_t)frames);
        if (rd32(b + 26) == 0) info->end = (int32_t)frames;
        info->start = clamp32(rd32(b + 22), 0, info->end - 1);
        info->mpc3000 = m3000;
        info->loop_len = m3000 ? 0 : clamp32(rd32(b + 34), 0, info->end);
        info->loop_on = !m3000 && b[38] == 1;
        info->beats = !m3000 && b[39] >= 1 && b[39] <= 16 ? b[39] : 4;
    }
    return s;
}

struct sample *snd_load(const char *path, struct snd_info *info) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct sample *s = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        unsigned char *buf = n > 0 ? malloc((size_t)n) : NULL;
        if (buf && fseek(f, 0, SEEK_SET) == 0 && fread(buf, 1, (size_t)n, f) == (size_t)n) s = snd_parse(buf, (size_t)n, info);
        free(buf);
    }
    fclose(f);
    return s;
}

int snd_save(const char *path, const struct sample *s, const struct snd_info *info) {
    if (!s || s->frames <= 0 || s->channels < 1) return -1;
    int ch = s->channels > 1 ? 2 : 1;
    int32_t frames = s->frames;
    int m3000 = info && info->mpc3000;
    unsigned char h[SND_HEADER] = {1, (unsigned char)(m3000 ? 2 : 4)};
    memset(h + 2, ' ', 16);
    const char *nm = info ? info->name : "";
    for (int i = 0; i < 16 && nm[i]; i++) h[2 + i] = (unsigned char)nm[i] >= 32 && (unsigned char)nm[i] <= 126 ? nm[i] : '_';
    int32_t end = info && info->end > 0 ? clamp32(info->end, 1, frames) : frames;
    int32_t start = info ? clamp32(info->start, 0, end - 1) : 0;
    int32_t loop = info ? clamp32(info->loop_len, 0, end) : 0;
    h[19] = info ? (unsigned char)clamp32(info->level, 0, 200) : 100;
    h[20] = (unsigned char)(int8_t)(info ? clamp32(info->tune, -120, 120) : 0);
    h[21] = ch == 2;
    wr32(h + 22, (uint32_t)start), wr32(h + 26, (uint32_t)end), wr32(h + 30, (uint32_t)frames), wr32(h + 34, (uint32_t)loop);
    h[38] = info && info->loop_on;
    h[39] = info && info->beats >= 1 && info->beats <= 16 ? info->beats : 4;
    int rate = s->rate > 0 && s->rate <= 65535 ? s->rate : 44100;
    h[40] = rate & 255, h[41] = rate >> 8;
    if (m3000) h[34] = 0xd0, h[35] = 0x07, h[36] = 1, h[37] = 0; // as an MPC 3000 writes them
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int head = m3000 ? SND_HEADER_3000 : SND_HEADER;
    int ok = fwrite(h, 1, (size_t)head, f) == (size_t)head;
    unsigned char buf[4096];
    for (int c = 0; c < ch && ok; c++) {
        size_t n = 0;
        for (int32_t i = 0; i < frames && ok; i++) {
            float v = s->data[(int64_t)i * s->channels + (c < s->channels ? c : 0)] * 32768.0f;
            int x = v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int)(v < 0 ? v - 0.5f : v + 0.5f);
            buf[n++] = x & 255, buf[n++] = (x >> 8) & 255;
            if (n == sizeof buf) ok = fwrite(buf, 1, n, f) == n, n = 0;
        }
        if (ok && n) ok = fwrite(buf, 1, n, f) == n;
    }
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}
