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
 * size. close and fsync wait until the buffer holds less than a frame,
 * which a close then drops and fsync leaves for the next write to finish.
 * After a close the filter plays out what it holds, fading to silence. */

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

/* An open: the format, and the first sample taking the first frame. The
 * buffer is empty already, because a close leaves it so. The filter's
 * history carries on from what played before, so a tail still in it plays
 * out behind the new frames rather than being cut off. pcm_reset is an
 * open on an emptied buffer and a filter cleared to silence. */
void pcm_open(pcm_t *s, const pcm_fmt_t *fmt);
void pcm_reset(pcm_t *s, const pcm_fmt_t *fmt);

/* Nothing left to play: a player that stops calling pcm_sample now cuts
 * nothing off. A closed device plays on until it is. */
bool pcm_settled(const pcm_t *s);

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
 * none whole in the buffer the filter takes each channel's newest sample
 * moved toward zero instead, so time goes on and an underrun is heard as a
 * gap that the sound fades into rather than drops into. */
void pcm_sample(pcm_t *s, int32_t *left, int32_t *right);

/* What an underrun takes in place of a frame: v, a channel's newest sample,
 * moved toward zero by 2^-k of itself, and by one at least, so it gets
 * there. k is the place of the top bit of step - 1 above bit 25, which
 * grows with the rate and makes 2^k frames about 1.3 to 2.7 ms at any rate.
 * At a 64th of the native rate and below, a frame is that long by itself,
 * k is 0 and the first silent frame is zero. */
int32_t pcm_fade(int32_t v, uint64_t step);

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
#define PCM_DRIVER DRIVER(            \
    STD(.handles = pcm_std_handles,   \
        .open = pcm_std_open,         \
        .close = pcm_std_close,       \
        .write = pcm_std_write,       \
        .sync = pcm_std_sync,         \
        .lseek = pcm_std_lseek)       \
    STD_SST(.ident = pcm_std_ident,   \
            .reopen = pcm_std_reopen, \
            .resumes_writes = true))


#endif /* _CORE_AUD_PCM_H_ */
