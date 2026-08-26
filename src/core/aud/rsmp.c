/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/rsmp.h"

/* src/core/gen/rsmp_coef_gen.py designs and emits these; aud_rsmp.sv reads the
 * same numbers out of the package the same script writes. */
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

/* Two passes of the same filter — once with the row below the phase, once
 * with the row above — and a straight line between the results. That is the
 * same number as interpolating the coefficients and filtering once, because
 * both are linear, and it is the cheaper of the two in fabric: one MAC
 * engine run twice, one coefficient read per tap.
 *
 * The fraction is taken to sixteen bits rather than the twenty-five that
 * are there. The difference of two accumulators reaches 2^38, and 2^38
 * times a Q25 fraction does not fit in the int64 this has to stay inside.
 * A sixteenth of a 128th of a sample is 1.2e-7 of an input sample; the
 * coefficients are quantised far more coarsely than that.
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
    /* Round, do not truncate. An arithmetic shift floors, and a floor on
     * every sample is a systematic half-LSB offset — which measures as a
     * DC term well above everything else the filter does. */
    return (int32_t)((v + (1 << (RSMP_Q - 1))) >> RSMP_Q);
}

int rsmp_push(rsmp_t *r, int32_t x, uint64_t step, int32_t *out, int max_out)
{
    /* A cold filter would ring against twenty-three zeros and put a click
     * at the start of every sound. Start it flat at the first sample. */
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
    /* One input consumed, so the interval moves on by one whether or not it
     * yielded anything. A step above 1.0 simply skips some intervals. */
    if (r->phase >= ((uint64_t)1 << 32))
        r->phase -= (uint64_t)1 << 32;
    return n;
}
