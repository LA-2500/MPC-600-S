#include "wav.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAV_MAX_BYTES (256u * 1024u * 1024u)

static uint32_t u16le(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t u32le(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static float read_value(const unsigned char *p, int bits, int is_float) {
    if (is_float) {
        float f;
        memcpy(&f, p, sizeof f);
        return f == f ? f : 0.0f;
    }
    switch (bits) {
    case 8:
        return ((float)p[0] - 128.0f) / 128.0f;
    case 16:
        return (float)(int16_t)u16le(p) / 32768.0f;
    case 24:
        return (float)(int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0f;
    default:
        return (float)(int32_t)u32le(p) / 2147483648.0f;
    }
}

struct sample *wav_parse(const unsigned char *buf, size_t len) {
    if (!buf || len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) return 0;
    uint32_t format = 0, channels = 0, rate = 0, block = 0, bits = 0;
    const unsigned char *data = 0;
    size_t data_len = 0;
    for (size_t at = 12; at + 8 <= len;) {
        const unsigned char *id = buf + at;
        size_t size = u32le(buf + at + 4);
        const unsigned char *body = buf + at + 8;
        size_t room = len - at - 8;
        if (memcmp(id, "fmt ", 4) == 0) {
            if (size < 16 || room < size) return 0;
            format = u16le(body);
            channels = u16le(body + 2);
            rate = u32le(body + 4);
            block = u16le(body + 12);
            bits = u16le(body + 14);
            if (format == 0xfffe) {
                if (size < 26) return 0;
                format = u16le(body + 24);
            }
        } else if (memcmp(id, "data", 4) == 0) {
            data = body;
            data_len = size < room ? size : room; // a header that overstates its size is common
            break;
        }
        if (room < size) break;
        size_t step = size + (size & 1);
        if (step > len - at - 8) break;
        at += 8 + step;
    }
    if (!data || (format != 1 && format != 3) || channels < 1 || rate < 1 || rate > 192000) return 0;
    if (format == 3 && bits != 32) return 0;
    if (format == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) return 0;
    uint32_t bytes = bits / 8;
    if (block < bytes * channels) return 0;
    size_t frames = data_len / block;
    if (frames < 1 || frames > INT32_MAX / 2) return 0;
    int out_ch = channels >= 2 ? 2 : 1;
    struct sample *s = sample_new((int32_t)frames, out_ch, (int)rate);
    if (!s) return 0;
    for (size_t i = 0; i < frames; i++) {
        const unsigned char *frame = data + i * block;
        for (int c = 0; c < out_ch; c++)
            s->data[i * (size_t)out_ch + (size_t)c] = read_value(frame + (size_t)c * bytes, (int)bits, format == 3);
    }
    return s;
}

static struct sample *load_file(const char *path, size_t limit, int cut) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    struct sample *s = 0;
    unsigned char *buf = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        size_t want = size > 0 ? (size_t)size : 0;
        if (cut && want > limit) want = limit;
        if (want > 0 && want <= limit && fseek(f, 0, SEEK_SET) == 0) {
            buf = malloc(want);
            if (buf && fread(buf, 1, want, f) == want) s = wav_parse(buf, want);
        }
    }
    free(buf);
    fclose(f);
    return s;
}

struct sample *wav_load(const char *path) {
    return load_file(path, WAV_MAX_BYTES, 0);
}

struct sample *wav_load_start(const char *path, size_t max_bytes) {
    return load_file(path, max_bytes, 1);
}

// Writes a sound as a WAV: bits 32 is float (exact), 24 or 16 PCM (for other gear), at the
// sound's own rate and channels. 0, or -1 when the file could not be written.
int wav_save(const char *path, const struct sample *s, int bits) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bytes = bits / 8, ch = s->channels, fmt = bits == 32 ? 3 : 1;
    uint32_t data = (uint32_t)s->frames * (uint32_t)ch * (uint32_t)bytes;
    unsigned char h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16, memcpy(h + 16, &v, 4);
    uint16_t w = (uint16_t)fmt;
    memcpy(h + 20, &w, 2);
    w = (uint16_t)ch, memcpy(h + 22, &w, 2);
    v = (uint32_t)s->rate, memcpy(h + 24, &v, 4);
    v = (uint32_t)s->rate * ch * bytes, memcpy(h + 28, &v, 4);
    w = (uint16_t)(ch * bytes), memcpy(h + 32, &w, 2);
    w = (uint16_t)bits, memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    int ok = fwrite(h, 1, 44, f) == 44;
    unsigned char buf[4096 * 4];
    long n = (long)s->frames * ch, at = 0;
    while (ok && at < n) {
        int k = 0;
        for (; k < 4096 && at < n; k++, at++) {
            float x = s->data[at];
            if (bits == 32) {
                memcpy(buf + k * 4, &x, 4);
                continue;
            }
            x = x > 1 ? 1 : x < -1 ? -1 : x;
            int32_t q = (int32_t)lrintf(x * (bits == 24 ? 8388607.0f : 32767.0f));
            for (int b = 0; b < bytes; b++) buf[k * bytes + b] = (unsigned char)(q >> (8 * b));
        }
        ok = fwrite(buf, (size_t)bytes, (size_t)k, f) == (size_t)k;
    }
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}
