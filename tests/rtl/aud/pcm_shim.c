/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What pcm.c takes from the rest of core, and the engine pcm.sv implements
 * for the test. That engine is rsmp's to the bit: rsmp's 24-tap rows, the
 * upper half read backwards, two dots blended, silence on an underrun and no
 * gain. pcm.c has moved on to its own filter; this copy stays with pcm.sv.
 * pcm.c is included for pcm_step and its rows in std. */

#include "core/aud/pcm.c"
#include "core/aud/rsmp.h"
#include "pcm_shim.h"

const char STR_PCM_COLON[] = "PCM:";

static const int32_t ref_coef[RSMP_PHASES / 2 + 1][RSMP_TAPS] = {
#include "rsmp_half.h"
};

/* pcm.sv's FIFO, whatever PCM_BUF_SIZE the other machines have. */
#define SHIM_FIFO_SIZE 4096

static struct
{
    pcm_fmt_t fmt;
    uint8_t frame;
    uint8_t at;
    uint64_t step;
    uint64_t phase;
    int32_t hist[2][2 * RSMP_TAPS];
    uint32_t underruns;
    RING(uint8_t, SHIM_FIFO_SIZE) buf;
} shim;

bool aud_pcm_open(const pcm_fmt_t *fmt)
{
    (void)fmt;
    return false;
}
void aud_pcm_close(void) {}
uint32_t aud_pcm_write(const uint8_t *buf, uint32_t n)
{
    (void)buf;
    (void)n;
    return 0;
}
uint16_t aud_pcm_level(void) { return 0; }
void aud_pcm_clear(void) {}

static int32_t ref_decode(uint32_t i)
{
    if (shim.fmt.bits == 8)
        return ((int32_t)*RING_AT(&shim.buf, i) - 128) * 256;
    return (int16_t)(*RING_AT(&shim.buf, i) | *RING_AT(&shim.buf, i + 1) << 8);
}

static void ref_take(void)
{
    int32_t l = 0, r = 0;
    const uint32_t level = RING_LEN(&shim.buf);
    if (level >= shim.frame)
    {
        l = ref_decode(0);
        r = shim.fmt.channels == 2 ? ref_decode(shim.fmt.bits / 8) : l;
        RING_SKIP(&shim.buf, shim.frame);
        if (level < 2u * shim.frame && shim.underruns < 0x7FFFFFFF)
            shim.underruns++;
    }
    const unsigned at = shim.at;
    shim.hist[0][at] = shim.hist[0][at + RSMP_TAPS] = l;
    shim.hist[1][at] = shim.hist[1][at + RSMP_TAPS] = r;
    shim.at = (uint8_t)(at + 1 == RSMP_TAPS ? 0 : at + 1);
}

static void ref_dot(unsigned p, const int32_t *hl, const int32_t *hr, int64_t *al, int64_t *ar)
{
    int64_t a = 0, b = 0;
    if (p <= RSMP_PHASES / 2)
    {
        const int32_t *c = ref_coef[p];
        for (int i = 0; i < RSMP_TAPS; i++)
        {
            a += (int64_t)c[i] * hl[i];
            b += (int64_t)c[i] * hr[i];
        }
    }
    else
    {
        const int32_t *c = ref_coef[RSMP_PHASES - p];
        for (int i = 0; i < RSMP_TAPS; i++)
        {
            a += (int64_t)c[RSMP_TAPS - 1 - i] * hl[i];
            b += (int64_t)c[RSMP_TAPS - 1 - i] * hr[i];
        }
    }
    *al = a;
    *ar = b;
}

static int64_t ref_blend(int64_t a, int64_t b, int64_t f)
{
    return a + (((b - a) * f) >> 16);
}

static int32_t ref_round(int64_t v)
{
    return (int32_t)((v + (1 << (RSMP_Q - 1))) >> RSMP_Q);
}

uint64_t shim_pcm_step(uint32_t rate) { return pcm_step(rate); }

void shim_pcm_reset(uint32_t rate, uint8_t bits, uint8_t channels)
{
    shim.fmt = (pcm_fmt_t){rate, bits, channels};
    shim.frame = (uint8_t)(bits / 8 * channels);
    shim.step = pcm_step(rate);
    shim.phase = (uint64_t)1 << 32;
    shim.at = 0;
    memset(shim.hist, 0, sizeof shim.hist);
    shim.underruns = 0;
    RING_RESET(&shim.buf);
}

uint32_t shim_pcm_put(const uint8_t *src, uint32_t n) { return RING_IN(&shim.buf, src, n); }
void shim_pcm_clear(void) { RING_RESET(&shim.buf); }
uint16_t shim_pcm_level(void) { return (uint16_t)RING_LEN(&shim.buf); }
uint32_t shim_pcm_underruns(void) { return shim.underruns; }
void shim_pcm_set_underruns(uint32_t n) { shim.underruns = n; }

void shim_pcm_sample(int32_t *l, int32_t *r)
{
    if (shim.phase >= ((uint64_t)1 << 32))
    {
        shim.phase -= (uint64_t)1 << 32;
        ref_take();
    }
    const uint32_t mu = (uint32_t)shim.phase;
    const int64_t f = (mu >> 9) & 0xFFFF;
    const int32_t *hl = &shim.hist[0][shim.at];
    const int32_t *hr = &shim.hist[1][shim.at];
    int64_t al, ar, bl, br;
    ref_dot(mu >> 25, hl, hr, &al, &ar);
    ref_dot((mu >> 25) + 1, hl, hr, &bl, &br);
    *l = ref_round(ref_blend(al, bl, f));
    *r = ref_round(ref_blend(ar, br, f));
    shim.phase += shim.step;
}

void shim_pcm_v(int64_t *vl, int64_t *vr)
{
    const uint32_t mu = (uint32_t)(shim.phase - shim.step);
    const int64_t f = (mu >> 9) & 0xFFFF;
    const int32_t *hl = &shim.hist[0][shim.at];
    const int32_t *hr = &shim.hist[1][shim.at];
    int64_t al, ar, bl, br;
    ref_dot(mu >> 25, hl, hr, &al, &ar);
    ref_dot((mu >> 25) + 1, hl, hr, &bl, &br);
    *vl = ref_blend(al, bl, f);
    *vr = ref_blend(ar, br, f);
}
