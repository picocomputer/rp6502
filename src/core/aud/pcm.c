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

/* Rows 0 to RSMP_PHASES/2 + 1 of rsmp's coefficients. Row RSMP_PHASES-p is
 * row p reversed, which rsmp.py checks, so rows p and p+1 are two rows in a
 * row here: read forwards up to the middle and backwards past it. */
static const int32_t HOST_IN_RAM pcm_coef[RSMP_PHASES / 2 + 2][RSMP_TAPS] = {
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

uint64_t HOST_IN_RAM_FUNC pcm_step(uint32_t rate)
{
    return div64((uint64_t)rate << 32, AUD_NATIVE_RATE, NULL);
}

void HOST_IN_RAM_FUNC pcm_open(pcm_t *s, const pcm_fmt_t *fmt)
{
    s->fmt = *fmt;
    s->frame = (uint8_t)(fmt->bits / 8 * fmt->channels);
    s->step = pcm_step(fmt->rate);
    /* The first sample takes the first frame. */
    s->phase = (uint64_t)1 << 32;
    s->underruns = 0;
}

void HOST_IN_RAM_FUNC pcm_reset(pcm_t *s, const pcm_fmt_t *fmt)
{
    pcm_open(s, fmt);
    RING_RESET(&s->buf);
    s->at = 0;
    memset(s->hist, 0, sizeof s->hist);
}

/* Every value the filter holds is zero and no frame is left to take, so
 * every sample from here on is zero. */
bool HOST_IN_RAM_FUNC pcm_settled(const pcm_t *s)
{
    if (RING_LEN(&s->buf) >= s->frame)
        return false;
    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < RSMP_TAPS; i++)
            if (s->hist[ch][i])
                return false;
    return true;
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

int32_t HOST_IN_RAM_FUNC pcm_fade(int32_t v, uint64_t step)
{
    const uint32_t m = (uint32_t)(step - 1);
    int k = 0;
    for (uint32_t t = 1u << 26; t && m >= t; t <<= 1)
        k++;
    return v - (v > 0 ? (v + (1 << k) - 1) >> k : v >> k);
}

static void HOST_IN_RAM_FUNC pcm_take(pcm_t *s)
{
    int32_t l, r;
    const uint32_t level = RING_LEN(&s->buf);
    if (level < s->frame)
    {
        l = pcm_fade(s->hist[0][s->at + RSMP_TAPS - 1], s->step);
        r = pcm_fade(s->hist[1][s->at + RSMP_TAPS - 1], s->step);
    }
    else
    {
        l = pcm_decode(s, 0);
        r = s->fmt.channels == 2 ? pcm_decode(s, s->fmt.bits / 8) : l;
        RING_SKIP(&s->buf, s->frame);
        if (level < 2u * s->frame && s->underruns < 0x7FFFFFFF)
            s->underruns++;
    }
    const unsigned at = s->at;
    s->hist[0][at] = s->hist[0][at + RSMP_TAPS] = l;
    s->hist[1][at] = s->hist[1][at + RSMP_TAPS] = r;
    s->at = (uint8_t)(at + 1 == RSMP_TAPS ? 0 : at + 1);
}

/* rsmp_at's two sums, for rows p and p+1, on both channels in one pass, so
 * each coefficient and each sample is loaded once. c is the first of two
 * stored rows and h the left channel's oldest tap; the right channel's taps
 * are 2*RSMP_TAPS further on. d gets the sums of the first stored row, left
 * and right, then of the second. Backwards, the taps are read from the
 * newest, which reads both rows reversed. */
#if defined(__GNUC__) && defined(__ARM_ARCH_8M_MAIN__)

/* The Cortex-M33's loop. GCC's spills two of the eight halves of the sums
 * on every tap; here they all stay in registers: the sums in r2:r3, r4:r5,
 * r6:r7 and r8:r9, the coefficient, the left tap and the right tap in r10,
 * r11 and r12, the rows in r0, the taps in r1, and the end of the row in
 * lr. A tap is four loads and four SMLALs, and the loop takes four. */
#define PCM_S(x) #x
#define PCM_X(x) PCM_S(x)
#define PCM_TAP(step)                                \
    "ldr r12, [r1, #(" PCM_X(RSMP_TAPS) " * 8)]\n"   \
    "ldr r11, [r1], #" step "\n"                     \
    "ldr r10, [r0, #(" PCM_X(RSMP_TAPS) " * 4)]\n"   \
    "smlal r6, r7, r10, r11\n"                       \
    "smlal r8, r9, r10, r12\n"                       \
    "ldr r10, [r0], #4\n"                            \
    "smlal r2, r3, r10, r11\n"                       \
    "smlal r4, r5, r10, r12\n"
#define PCM_SUMS(name, step)                                     \
    __attribute__((naked, noinline)) static void HOST_IN_RAM_FUNC \
    name(__attribute__((unused)) const int32_t *c,                \
         __attribute__((unused)) const int32_t *h,                \
         __attribute__((unused)) int64_t d[4])                    \
    {                                                             \
        __asm__ volatile(                                         \
            "push {r2, r4-r11, lr}\n"                             \
            "add lr, r0, #(" PCM_X(RSMP_TAPS) " * 4)\n"           \
            "movs r2, #0\n"                                       \
            "movs r3, #0\n"                                       \
            "movs r4, #0\n"                                       \
            "movs r5, #0\n"                                       \
            "movs r6, #0\n"                                       \
            "movs r7, #0\n"                                       \
            "mov r8, #0\n"                                        \
            "mov r9, #0\n"                                        \
            "1:\n" PCM_TAP(step) PCM_TAP(step)                    \
                PCM_TAP(step) PCM_TAP(step)                       \
            "cmp r0, lr\n"                                        \
            "bne 1b\n"                                            \
            "pop {r10}\n"                                         \
            "strd r2, r3, [r10]\n"                                \
            "strd r4, r5, [r10, #8]\n"                            \
            "strd r6, r7, [r10, #16]\n"                           \
            "strd r8, r9, [r10, #24]\n"                           \
            "pop {r4-r11, pc}\n");                                \
    }
static_assert(RSMP_TAPS % 4 == 0, "the loop takes four taps a turn");
PCM_SUMS(pcm_sums_up, "4")
PCM_SUMS(pcm_sums_down, "-4")

static inline void pcm_sums(const int32_t *c, const int32_t *h, bool back, int64_t d[4])
{
    if (back)
        pcm_sums_down(c, &h[RSMP_TAPS - 1], d);
    else
        pcm_sums_up(c, h, d);
}

#else

static inline __attribute__((always_inline)) void pcm_sums(const int32_t *c, const int32_t *h, bool back, int64_t d[4])
{
    int64_t l0 = 0, r0 = 0, l1 = 0, r1 = 0;
    for (int i = 0; i < RSMP_TAPS; i++)
    {
        const int32_t *t = back ? &h[RSMP_TAPS - 1 - i] : &h[i];
        const int32_t sl = t[0], sr = t[2 * RSMP_TAPS];
        const int32_t c0 = c[i], c1 = c[RSMP_TAPS + i];
        l0 += (int64_t)c0 * sl;
        r0 += (int64_t)c0 * sr;
        l1 += (int64_t)c1 * sl;
        r1 += (int64_t)c1 * sr;
    }
    d[0] = l0;
    d[1] = r0;
    d[2] = l1;
    d[3] = r1;
}

#endif

/* rsmp_at's blend and rounding, so the result is rsmp's to the bit. */
static int32_t HOST_IN_RAM_FUNC pcm_blend(int64_t a, int64_t b, int64_t f)
{
    const int64_t v = a + (((b - a) * f) >> 16);
    return (int32_t)((v + (1 << (RSMP_Q - 1))) >> RSMP_Q);
}

void HOST_IN_RAM_FUNC pcm_sample(pcm_t *s, int32_t *left, int32_t *right)
{
    if (s->phase >= ((uint64_t)1 << 32))
    {
        s->phase -= (uint64_t)1 << 32;
        pcm_take(s);
    }
    const uint32_t mu = (uint32_t)s->phase;
    const unsigned p = mu >> (32 - RSMP_PHASE_BITS);
    const int64_t f = (mu >> (16 - RSMP_PHASE_BITS)) & 0xFFFF;
    const int32_t *h = &s->hist[0][s->at];
    static_assert(sizeof s->hist[0] == 2 * RSMP_TAPS * sizeof(int32_t),
                  "pcm_sums finds the right channel 2*RSMP_TAPS on");
    /* Up to the middle, rows p and p+1 are stored rows p and p+1. Past it,
     * they are stored rows RSMP_PHASES-p and RSMP_PHASES-1-p reversed, so
     * the pair starts at RSMP_PHASES-1-p and row p's sums come second. */
    const bool back = p > RSMP_PHASES / 2;
    int64_t d[4];
    pcm_sums(pcm_coef[back ? RSMP_PHASES - 1 - p : p], h, back, d);
    const int a = back ? 2 : 0;
    *left = pcm_blend(d[a], d[2 - a], f);
    *right = pcm_blend(d[a + 1], d[3 - a], f);
    s->phase += s->step;
}

#pragma GCC pop_options

bool HOST_IN_RAM_FUNC pcm_std_handles(const char *path)
{
    return strncasecmp(path, STR_PCM_COLON, STR_PCM_COLON_LEN) == 0;
}

/* The open device's format. */
static pcm_fmt_t pcm_fmt;

#define PCM_SST(X)                                              \
    X(SKIP, pcm_fmt, "the STD chunk carries it: pcm_std_ident "  \
                     "writes it and pcm_std_reopen sets it")
SST_TABLE(pcm, PCM_SST)

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
