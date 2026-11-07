/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What pcm.c takes from the rest of core, and its engine for the test.
 * pcm.c is included for its pcm_dot. */

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

uint32_t shim_pcm_put(const uint8_t *src, uint32_t n) { return pcm_put(&shim, src, n); }
void shim_pcm_clear(void) { pcm_clear(&shim); }
uint16_t shim_pcm_level(void) { return pcm_level(&shim); }
uint32_t shim_pcm_underruns(void) { return shim.underruns; }
void shim_pcm_set_underruns(uint32_t n) { shim.underruns = n; }
void shim_pcm_sample(int32_t *l, int32_t *r) { pcm_sample(&shim, l, r); }

void shim_pcm_v(int64_t *vl, int64_t *vr)
{
    const uint32_t mu = (uint32_t)(shim.phase - shim.step);
    const int64_t f = (mu >> 9) & 0xFFFF;
    const int32_t *hl = &shim.hist[0][shim.at];
    const int32_t *hr = &shim.hist[1][shim.at];
    int64_t al, ar, bl, br;
    pcm_dot(mu >> 25, hl, hr, &al, &ar);
    pcm_dot((mu >> 25) + 1, hl, hr, &bl, &br);
    *vl = al + (((bl - al) * f) >> 16);
    *vr = ar + (((br - ar) * f) >> 16);
}
