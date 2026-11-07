/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_PCM_H_
#define _CORE_AUD_PCM_H_

#include "core/api/std.h"
#include "core/aud/rsmp.h"
#include "core/sys/ring.h"
#include <stdbool.h>
#include <stdint.h>

/* The PCM: device. A program opens "PCM:rate,bits,channels" for writing and
 * writes frames as a WAV file holds them: interleaved, little-endian, 8-bit
 * unsigned or 16-bit signed, one or two channels. They wait in a buffer of
 * PCM_BUF_SIZE bytes and play at rate, resampled to AUD_NATIVE_RATE with
 * rsmp's filter and summed into the mix.
 *
 * A write takes what fits and returns the count, so the program resends the
 * rest. lseek with an offset of 0 is the only seek: SEEK_SET empties the
 * buffer, SEEK_CUR gives the bytes waiting to play and SEEK_END the buffer's
 * size. close and fsync wait until the buffer has played. */

#define PCM_BUF_SIZE 4096
#define PCM_RATE_MAX 48000

typedef struct
{
    uint32_t rate;
    uint8_t bits;
    uint8_t channels;
} pcm_fmt_t;

/* The format after "PCM:", such as "22050,16,2". */
bool pcm_parse(const char *args, pcm_fmt_t *fmt);

/* The sound itself. The writer puts bytes in buf and the player takes frames
 * out. A machine whose writer and player run in two contexts, the Pico's main
 * loop and audio interrupt, shares one of these between them. */
typedef struct
{
    pcm_fmt_t fmt;
    uint8_t frame;  /* bytes per frame */
    uint8_t at;     /* where hist takes the next sample */
    uint64_t step;  /* frames per native sample, Q32, one at most */
    uint64_t phase; /* Q32; at one or more the next sample takes a frame */
    /* Each channel's last RSMP_TAPS samples, every one written twice, at at
     * and at + RSMP_TAPS, so the newest RSMP_TAPS always lie in a row. */
    int32_t hist[2][2 * RSMP_TAPS];
    /* Takes that left less than a frame, saturating at 0x7FFFFFFF: each
     * time the buffer ran dry. Only the player counts. */
    uint32_t underruns;
    RING(uint8_t, PCM_BUF_SIZE) buf;
} pcm_t;

/* Frames per native sample at rate, Q32. */
uint64_t pcm_step(uint32_t rate);

void pcm_reset(pcm_t *s, const pcm_fmt_t *fmt);

static inline uint16_t pcm_level(pcm_t *s)
{
    return (uint16_t)RING_LEN(&s->buf);
}

/* Copies in what fits and returns the count. */
uint32_t pcm_put(pcm_t *s, const uint8_t *src, uint32_t n);

/* Empties the buffer while the player is held. The filter plays out what it
 * already holds. */
void pcm_clear(pcm_t *s);

/* The next native sample. A frame is taken when the phase reaches one; with
 * none whole in the buffer the filter takes silence instead, so time goes on
 * and an underrun is heard as a gap. */
void pcm_sample(pcm_t *s, int32_t *left, int32_t *right);

/* How many frames pcm_sample takes over the next n samples, with the phase
 * moved on past them, buffer or no buffer. A machine that answers the
 * program in its own time, ahead of the player, counts with this. */
uint64_t pcm_takes(uint64_t *phase, uint64_t step, uint64_t n);

/* The device's rows, which open() reaches by name. Each machine's mixer
 * keeps the sound; core/aud/mix.h declares what it gives this file. */
bool pcm_std_handles(const char *path);
int pcm_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result pcm_std_close(int desc, api_errno *err);
std_rw_result pcm_std_write(int desc, const char *buf, uint32_t count,
                            uint32_t *bytes_written, api_errno *err);
std_rw_result pcm_std_sync(int desc, api_errno *err);
int pcm_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err);
bool pcm_std_ident(int desc, sst_cursor_t *c);
int pcm_std_reopen(sst_cursor_t *c, uint8_t mode, api_errno *err);

/* Sound can always resume, so a savestate carries the device and the mixer's
 * row carries its buffer. */
#define PCM_STD_DRIVER              \
    {                               \
        .handles = pcm_std_handles, \
        .open = pcm_std_open,       \
        .close = pcm_std_close,     \
        .write = pcm_std_write,     \
        .sync = pcm_std_sync,       \
        .lseek = pcm_std_lseek,     \
        STD_SST_COLS(pcm_std_ident, pcm_std_reopen, true) \
    }

#endif /* _CORE_AUD_PCM_H_ */
