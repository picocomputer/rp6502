/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What pcm.c takes from the rest of core, and its engine for the test.
 * pcm.c is included for its pcm_sums. */

#include "core/aud/pcm.c"
#include "pcm_shim.h"

const char STR_PCM_COLON[] = "PCM:";

static pcm_t shim;

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

uint64_t shim_pcm_step(uint32_t rate) { return pcm_step(rate); }

void shim_pcm_reset(uint32_t rate, uint8_t bits, uint8_t channels)
{
    const pcm_fmt_t fmt = {rate, bits, channels};
    pcm_reset(&shim, &fmt);
}

void shim_pcm_open(uint32_t rate, uint8_t bits, uint8_t channels)
{
    const pcm_fmt_t fmt = {rate, bits, channels};
    pcm_open(&shim, &fmt);
}

uint32_t shim_pcm_put(const uint8_t *src, uint32_t n) { return pcm_put(&shim, src, n); }
void shim_pcm_clear(void) { pcm_clear(&shim); }
uint16_t shim_pcm_level(void) { return pcm_level(&shim); }
uint32_t shim_pcm_underruns(void) { return shim.underruns; }
void shim_pcm_set_underruns(uint32_t n) { shim.underruns = n; }
void shim_pcm_sample(int32_t *l, int32_t *r) { pcm_sample(&shim, l, r); }

void shim_pcm_v(int64_t *vl, int64_t *vr)
{
    const uint32_t mu = (uint32_t)(shim.phase - shim.step);
    const unsigned p = mu >> (32 - RSMP_PHASE_BITS);
    const int64_t f = (mu >> (16 - RSMP_PHASE_BITS)) & 0xFFFF;
    const bool back = p > RSMP_PHASES / 2;
    int64_t d[4];
    pcm_sums(pcm_coef[back ? RSMP_PHASES - 1 - p : p], &shim.hist[0][shim.at], back, d);
    const int a = back ? 2 : 0;
    *vl = d[a] + (((d[2 - a] - d[a]) * f) >> 16);
    *vr = d[a + 1] + (((d[3 - a] - d[a + 1]) * f) >> 16);
}
