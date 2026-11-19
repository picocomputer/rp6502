/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/div64.h"
#include "core/aud/pcm.h"
#include "core/aud/mix.h"
#include "machine.h"
#include "core/str/str.h"
#include "osal/fs.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h> /* SEEK_SET */
#include <string.h>
#include <strings.h>

/* A frame is taken at most once a sample, so the step is one at most. */
static_assert(PCM_RATE_MAX <= AUD_NATIVE_RATE, "PCM must not downsample");

/* Rows 0 to PCM_PHASES/2. Row PCM_PHASES-p is row p reversed, which pcm.py
 * checks. */
#include "pcm_coef.h"
static_assert(PCM_COEF_TAPS == PCM_TAPS && PCM_COEF_PHASES == PCM_PHASES,
              "pcm_coef.h is not the filter pcm.h declares");

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

uint64_t HOST_IN_RAM_FUNC pcm_step(uint32_t rate)
{
    return div64((uint64_t)rate << 32, AUD_NATIVE_RATE, NULL);
}

void HOST_IN_RAM_FUNC pcm_reset(pcm_t *s, const pcm_fmt_t *fmt)
{
    s->fmt = *fmt;
    s->frame = (uint8_t)(fmt->bits / 8 * fmt->channels);
    s->step = pcm_step(fmt->rate);
    /* The first sample takes the first frame. */
    s->phase = (uint64_t)1 << 32;
    s->at = 0;
    memset(s->hist, 0, sizeof s->hist);
    s->underruns = 0;
    RING_RESET(&s->buf);
}

uint32_t HOST_IN_RAM_FUNC pcm_put(pcm_t *s, const uint8_t *src, uint32_t n)
{
    return RING_IN(&s->buf, src, n);
}

void HOST_IN_RAM_FUNC pcm_clear(pcm_t *s)
{
    RING_RESET(&s->buf);
}

uint64_t pcm_takes(uint64_t *phase, uint64_t step, uint64_t n)
{
    if (!n)
        return 0;
    const uint64_t takes = (*phase + (n - 1) * step) >> 32;
    *phase = *phase + n * step - (takes << 32);
    return takes;
}

/* The player is in the Pico's audio interrupt, every sample. */
#pragma GCC push_options
#pragma GCC optimize("O3")

/* The sample i bytes after the oldest. */
static int32_t HOST_IN_RAM_FUNC pcm_decode(pcm_t *s, uint32_t i)
{
    if (s->fmt.bits == 8)
        return ((int32_t)*RING_AT(&s->buf, i) - 128) * 256;
    return (int16_t)(*RING_AT(&s->buf, i) | *RING_AT(&s->buf, i + 1) << 8);
}

static void HOST_IN_RAM_FUNC pcm_take(pcm_t *s)
{
    const unsigned at = s->at;
    int32_t l, r;
    const uint32_t level = RING_LEN(&s->buf);
    if (level >= s->frame)
    {
        l = pcm_decode(s, 0);
        r = s->fmt.channels == 2 ? pcm_decode(s, s->fmt.bits / 8) : l;
        RING_SKIP(&s->buf, s->frame);
        if (level < 2u * s->frame && s->underruns < 0x7FFFFFFF)
            s->underruns++;
    }
    else
    {
        /* The newest sample, written last time at at - 1, so its copy is
         * at at + PCM_TAPS - 1. Division truncates toward zero, so it
         * reaches zero exactly. */
        l = s->hist[0][at + PCM_TAPS - 1] * 63 / 64;
        r = s->hist[1][at + PCM_TAPS - 1] * 63 / 64;
    }
    s->hist[0][at] = s->hist[0][at + PCM_TAPS] = l;
    s->hist[1][at] = s->hist[1][at + PCM_TAPS] = r;
    s->at = (uint8_t)(at + 1 == PCM_TAPS ? 0 : at + 1);
}

/* Rows p and p + 1 blended by f, a 16-bit fraction, once for both
 * channels. A row past the middle is a stored row read backwards. */
static void HOST_IN_RAM_FUNC pcm_row(unsigned p, int32_t f, int32_t *c)
{
    enum { H = PCM_PHASES / 2, T = PCM_TAPS };
    const unsigned q = p + 1;
    const int32_t *a = pcm_coef[p <= H ? p : PCM_PHASES - p];
    const int32_t *b = pcm_coef[q <= H ? q : PCM_PHASES - q];
    if (q <= H)
        for (int i = 0; i < T; i++)
            c[i] = a[i] + (((b[i] - a[i]) * f + 0x8000) >> 16);
    else if (p > H)
        for (int i = 0; i < T; i++)
            c[i] = a[T - 1 - i] + (((b[T - 1 - i] - a[T - 1 - i]) * f + 0x8000) >> 16);
    else
        for (int i = 0; i < T; i++)
            c[i] = a[i] + (((b[T - 1 - i] - a[i]) * f + 0x8000) >> 16);
}

void HOST_IN_RAM_FUNC pcm_sample(pcm_t *s, int32_t *left, int32_t *right)
{
    if (s->phase >= ((uint64_t)1 << 32))
    {
        s->phase -= (uint64_t)1 << 32;
        pcm_take(s);
    }
    const uint32_t mu = (uint32_t)s->phase;
    int32_t c[PCM_TAPS];
    pcm_row(mu >> 25, (int32_t)((mu >> 9) & 0xFFFF), c);
    const int32_t *hl = &s->hist[0][s->at];
    const int32_t *hr = &s->hist[1][s->at];
    int64_t al = 0, ar = 0;
    for (int i = 0; i < PCM_TAPS; i++)
    {
        al += (int64_t)c[i] * hl[i];
        ar += (int64_t)c[i] * hr[i];
    }
    /* Q(PCM_COEF_Q) by Q15, rounded. */
    const int sh = PCM_COEF_Q + 15;
    *left = (int32_t)((al * PCM_GAIN + ((int64_t)1 << (sh - 1))) >> sh);
    *right = (int32_t)((ar * PCM_GAIN + ((int64_t)1 << (sh - 1))) >> sh);
    s->phase += s->step;
}

#pragma GCC pop_options

bool HOST_IN_RAM_FUNC pcm_std_handles(const char *path)
{
    return strncasecmp(path, STR_PCM_COLON, STR_PCM_COLON_LEN) == 0;
}

/* The open device's format. */
static pcm_fmt_t pcm_fmt;

static bool pcm_open_fmt(const pcm_fmt_t *fmt, api_errno *err)
{
    if (!aud_pcm_open(fmt))
    {
        *err = API_EBUSY;
        return false;
    }
    pcm_fmt = *fmt;
    return true;
}

int pcm_std_open(const char *path, uint8_t flags, api_errno *err)
{
    pcm_fmt_t fmt;
    if ((flags & FS_RD) || !pcm_parse(path + STR_PCM_COLON_LEN, &fmt))
    {
        *err = API_EINVAL;
        return -1;
    }
    return pcm_open_fmt(&fmt, err) ? 0 : -1;
}

/* A last partial frame never plays, so it does not hold up a close. */
static bool HOST_IN_RAM_FUNC pcm_playing(void)
{
    return aud_pcm_level() >= pcm_fmt.bits / 8 * pcm_fmt.channels;
}

std_rw_result HOST_IN_RAM_FUNC pcm_std_close(int desc, api_errno *err)
{
    (void)desc;
    (void)err;
    if (pcm_playing())
        return STD_PENDING;
    aud_pcm_close();
    return STD_OK;
}

std_rw_result HOST_IN_RAM_FUNC pcm_std_write(int desc, const char *buf, uint32_t count,
                                             uint32_t *bytes_written, api_errno *err)
{
    (void)desc;
    (void)err;
    *bytes_written = aud_pcm_write((const uint8_t *)buf, count);
    return STD_OK;
}

std_rw_result HOST_IN_RAM_FUNC pcm_std_sync(int desc, api_errno *err)
{
    (void)desc;
    (void)err;
    return pcm_playing() ? STD_PENDING : STD_OK;
}

int HOST_IN_RAM_FUNC pcm_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err)
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

bool pcm_std_ident(int desc, sst_cursor_t *c)
{
    (void)desc;
    sst_put_u32(c, pcm_fmt.rate);
    sst_put_u8(c, pcm_fmt.bits);
    sst_put_u8(c, pcm_fmt.channels);
    return sst_ok(c);
}

int pcm_std_reopen(sst_cursor_t *c, uint8_t mode, api_errno *err)
{
    pcm_fmt_t fmt;
    fmt.rate = sst_get_u32(c);
    fmt.bits = sst_get_u8(c);
    fmt.channels = sst_get_u8(c);
    if (!sst_ok(c) || mode != FS_WR || !fmt.rate || fmt.rate > PCM_RATE_MAX ||
        (fmt.bits != 8 && fmt.bits != 16) || (fmt.channels != 1 && fmt.channels != 2))
    {
        *err = API_EINVAL;
        return -1;
    }
    return pcm_open_fmt(&fmt, err) ? 0 : -1;
}
