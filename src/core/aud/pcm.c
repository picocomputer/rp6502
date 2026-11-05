/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/pcm.h"
#include "core/aud/mix.h"
#include "core/str/str.h"
#include "osal/fs.h"
#include <assert.h>
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h> /* SEEK_SET */
#include <string.h>
#include <strings.h>

/* A frame is taken at most once a sample, so the step stays under one. */
static_assert(PCM_RATE_MAX < AUD_NATIVE_RATE, "PCM must upsample");
static_assert((PCM_BUF_SIZE & (PCM_BUF_SIZE - 1)) == 0, "PCM buffer is a power of two");
static_assert(PCM_BUF_SIZE <= 0x8000, "PCM buffer counts in 16 bits");

/* Rows 0 to RSMP_PHASES/2 of rsmp's coefficients. Row RSMP_PHASES-p is row
 * p reversed, which rsmp_coef_gen.py checks. */
static const int32_t pcm_coef[RSMP_PHASES / 2 + 1][RSMP_TAPS] = {
#include "rsmp_half.h"
};

static bool pcm_number(const char **s, uint32_t max, uint32_t *out)
{
    const char *p = *s;
    uint32_t v = 0;
    if (!isdigit((unsigned char)*p))
        return false;
    for (; isdigit((unsigned char)*p); p++)
    {
        v = v * 10 + (uint32_t)(*p - '0');
        if (v > max)
            return false;
    }
    *s = p;
    *out = v;
    return true;
}

bool pcm_parse(const char *args, pcm_fmt_t *fmt)
{
    uint32_t rate, bits, channels;
    if (!pcm_number(&args, PCM_RATE_MAX, &rate) || !rate || *args != ',')
        return false;
    args++;
    if (!pcm_number(&args, 16, &bits) || (bits != 8 && bits != 16) || *args != ',')
        return false;
    args++;
    if (!pcm_number(&args, 2, &channels) || !channels || *args)
        return false;
    fmt->rate = rate;
    fmt->bits = (uint8_t)bits;
    fmt->channels = (uint8_t)channels;
    return true;
}

void pcm_reset(pcm_t *s, const pcm_fmt_t *fmt)
{
    s->fmt = *fmt;
    s->frame = (uint8_t)(fmt->bits / 8 * fmt->channels);
    s->step = (uint32_t)(((uint64_t)fmt->rate << 32) / AUD_NATIVE_RATE);
    /* The first sample takes the first frame. */
    s->phase = (uint64_t)1 << 32;
    s->at = 0;
    memset(s->hist, 0, sizeof s->hist);
    s->head = 0;
    s->tail = 0;
}

uint32_t pcm_put(pcm_t *s, const uint8_t *src, uint32_t n)
{
    const uint32_t room = PCM_BUF_SIZE - pcm_level(s);
    if (n > room)
        n = room;
    const uint32_t at = s->head & (PCM_BUF_SIZE - 1);
    const uint32_t first = n < PCM_BUF_SIZE - at ? n : PCM_BUF_SIZE - at;
    memcpy(&s->buf[at], src, first);
    memcpy(s->buf, src + first, n - first);
    /* The player may run between any two instructions here, so the bytes
     * are in place before head says so. */
    atomic_signal_fence(memory_order_release);
    s->head = (uint16_t)(s->head + n);
    return n;
}

void pcm_clear(pcm_t *s)
{
    s->tail = s->head;
}

uint64_t pcm_takes(uint64_t *phase, uint32_t step, uint64_t n)
{
    if (!n)
        return 0;
    const uint64_t takes = (*phase + (n - 1) * step) >> 32;
    *phase = *phase + n * step - (takes << 32);
    return takes;
}

static int32_t pcm_decode(const pcm_t *s, uint32_t at)
{
    if (s->fmt.bits == 8)
        return ((int32_t)s->buf[at & (PCM_BUF_SIZE - 1)] - 128) * 256;
    return (int16_t)(s->buf[at & (PCM_BUF_SIZE - 1)] |
                     s->buf[(at + 1) & (PCM_BUF_SIZE - 1)] << 8);
}

static void pcm_take(pcm_t *s)
{
    int32_t l = 0, r = 0;
    if (pcm_level(s) >= s->frame)
    {
        const uint16_t tail = s->tail;
        l = pcm_decode(s, tail);
        r = s->fmt.channels == 2 ? pcm_decode(s, tail + s->fmt.bits / 8) : l;
        /* The bytes are read before the writer may reuse them. */
        atomic_signal_fence(memory_order_acquire);
        s->tail = (uint16_t)(tail + s->frame);
    }
    const unsigned at = s->at;
    s->hist[0][at] = s->hist[0][at + RSMP_TAPS] = l;
    s->hist[1][at] = s->hist[1][at + RSMP_TAPS] = r;
    s->at = (uint8_t)(at + 1 == RSMP_TAPS ? 0 : at + 1);
}

/* rsmp_at's sum for row p, on both channels at once. A row past the middle
 * is a stored row read backwards. */
static void pcm_dot(unsigned p, const int32_t *hl, const int32_t *hr, int64_t *al, int64_t *ar)
{
    int64_t a = 0, b = 0;
    if (p <= RSMP_PHASES / 2)
    {
        const int32_t *c = pcm_coef[p];
        for (int i = 0; i < RSMP_TAPS; i++)
        {
            a += (int64_t)c[i] * hl[i];
            b += (int64_t)c[i] * hr[i];
        }
    }
    else
    {
        const int32_t *c = pcm_coef[RSMP_PHASES - p];
        for (int i = 0; i < RSMP_TAPS; i++)
        {
            a += (int64_t)c[RSMP_TAPS - 1 - i] * hl[i];
            b += (int64_t)c[RSMP_TAPS - 1 - i] * hr[i];
        }
    }
    *al = a;
    *ar = b;
}

/* rsmp_at's blend and rounding, so the result is rsmp's to the bit. */
static int32_t pcm_blend(int64_t a, int64_t b, int64_t f)
{
    const int64_t v = a + (((b - a) * f) >> 16);
    return (int32_t)((v + (1 << (RSMP_Q - 1))) >> RSMP_Q);
}

void pcm_sample(pcm_t *s, int32_t *left, int32_t *right)
{
    if (s->phase >= ((uint64_t)1 << 32))
    {
        s->phase -= (uint64_t)1 << 32;
        pcm_take(s);
    }
    const uint32_t mu = (uint32_t)s->phase;
    const unsigned p = mu >> 25;
    const int64_t f = (mu >> 9) & 0xFFFF;
    const int32_t *hl = &s->hist[0][s->at];
    const int32_t *hr = &s->hist[1][s->at];
    int64_t al, ar, bl, br;
    pcm_dot(p, hl, hr, &al, &ar);
    pcm_dot(p + 1, hl, hr, &bl, &br);
    *left = pcm_blend(al, bl, f);
    *right = pcm_blend(ar, br, f);
    s->phase += s->step;
}

bool pcm_std_handles(const char *path)
{
    return strncasecmp(path, STR_PCM_COLON, STR_PCM_COLON_LEN) == 0;
}

/* Bytes per frame of the open device. */
static uint8_t pcm_frame;

int pcm_std_open(const char *path, uint8_t flags, api_errno *err)
{
    pcm_fmt_t fmt;
    if ((flags & FS_RD) || !pcm_parse(path + STR_PCM_COLON_LEN, &fmt))
    {
        *err = API_EINVAL;
        return -1;
    }
    if (!aud_pcm_open(&fmt))
    {
        *err = API_EBUSY;
        return -1;
    }
    pcm_frame = (uint8_t)(fmt.bits / 8 * fmt.channels);
    return 0;
}

/* A last partial frame never plays, so it does not hold up a close. */
static bool pcm_playing(void)
{
    return aud_pcm_level() >= pcm_frame;
}

std_rw_result pcm_std_close(int desc, api_errno *err)
{
    (void)desc;
    (void)err;
    if (pcm_playing())
        return STD_PENDING;
    aud_pcm_close();
    return STD_OK;
}

std_rw_result pcm_std_write(int desc, const char *buf, uint32_t count,
                            uint32_t *bytes_written, api_errno *err)
{
    (void)desc;
    (void)err;
    *bytes_written = aud_pcm_write((const uint8_t *)buf, count);
    return STD_OK;
}

std_rw_result pcm_std_sync(int desc, api_errno *err)
{
    (void)desc;
    (void)err;
    return pcm_playing() ? STD_PENDING : STD_OK;
}

int pcm_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err)
{
    (void)desc;
    if (offset)
    {
        *err = API_EINVAL;
        return -1;
    }
    switch (whence)
    {
    case SEEK_SET:
        aud_pcm_clear();
        *pos = 0;
        return 0;
    case SEEK_CUR:
        *pos = aud_pcm_level();
        return 0;
    case SEEK_END:
        *pos = PCM_BUF_SIZE;
        return 0;
    }
    *err = API_EINVAL;
    return -1;
}
