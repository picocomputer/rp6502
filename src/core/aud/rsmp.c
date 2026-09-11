/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/rsmp.h"

const int32_t rsmp_coef[RSMP_PHASES + 1][RSMP_TAPS] = {
#include "rsmp_coef.h"
};

void rsmp_reset(rsmp_t *r)
{
    for (int i = 0; i < RSMP_TAPS; i++)
        r->hist[i] = 0;
    r->phase = 0;
    r->primed = false;
}

uint64_t rsmp_step(uint32_t in_rate, uint32_t out_rate)
{
    if (!out_rate)
        return (uint64_t)1 << 32;
    return ((uint64_t)in_rate << 32) / out_rate;
}

/* The fraction between the rows is taken to sixteen bits of the twenty-five
 * that are there, which is what rsmp.sv takes. A 65,536th of a 128th of an
 * input sample is 1.2e-7 of one, and the coefficients are quantised far more
 * coarsely than that.
 */
static int32_t rsmp_at(const int32_t *h, uint32_t mu)
{
    const unsigned p = mu >> 25;               /* 0 .. RSMP_PHASES-1 */
    const int64_t f = (mu >> 9) & 0xFFFF;      /* Q16 between p and p+1 */
    int64_t a = 0, b = 0;
    for (int i = 0; i < RSMP_TAPS; i++)
    {
        a += (int64_t)rsmp_coef[p][i] * h[i];
        b += (int64_t)rsmp_coef[p + 1][i] * h[i];
    }
    const int64_t v = a + (((b - a) * f) >> 16);
    /* Round rather than truncate: an arithmetic shift floors, and a floor
     * on every sample is a systematic half-LSB offset, which is DC rather
     * than noise. */
    return (int32_t)((v + (1 << (RSMP_Q - 1))) >> RSMP_Q);
}

int rsmp_push(rsmp_t *r, int32_t x, uint64_t step, int32_t *out, int max_out)
{
    /* A cold filter would ring against twenty-three zeros and click at the
     * start of every sound, so it starts flat at the first sample. */
    if (!r->primed)
    {
        for (int i = 0; i < RSMP_TAPS; i++)
            r->hist[i] = x;
        r->primed = true;
    }
    else
    {
        for (int i = 0; i < RSMP_TAPS - 1; i++)
            r->hist[i] = r->hist[i + 1];
        r->hist[RSMP_TAPS - 1] = x;
    }

    int n = 0;
    while (r->phase < ((uint64_t)1 << 32))
    {
        if (n == max_out)
            break;
        out[n++] = rsmp_at(r->hist, (uint32_t)r->phase);
        r->phase += step;
    }
    /* The interval moves on with the input that was consumed, except on the
     * max_out break, which leaves the phase inside it. A step above 1.0
     * skips intervals. */
    if (r->phase >= ((uint64_t)1 << 32))
        r->phase -= (uint64_t)1 << 32;
    return n;
}
