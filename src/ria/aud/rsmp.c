/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rsmp.h"

/* src/fpga/codegen/rsmp_coef_gen.py designs and re-checks these;
 * aud_rsmp.sv reads the same numbers. */
const int16_t rsmp_matrix[RSMP_ORDER][RSMP_TAPS] = {
    {0, 0, 0, 0, 16384, 0, 0, 0, 0, 0},
    {133, -894, 3513, -12473, -2217, 16425, -6285, 2373, -683, 108},
    {-37, 339, -2021, 13844, -24348, 14142, -2377, 560, -116, 14},
    {-236, 1496, -4962, 5793, 2348, -10351, 8789, -3887, 1211, -201},
    {162, -1118, 4309, -9509, 11721, -7720, 2218, 115, -236, 58},
    {-21, 176, -839, 2345, -3888, 3888, -2345, 839, -176, 21},
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

/* Horner over the polynomial, each coefficient a six-tap filter of the
 * history. Everything stays in Q14 until the last shift, so the RTL has one
 * accumulator width and one rounding rule to copy. */
static int32_t rsmp_at(const int32_t *h, uint32_t mu)
{
    int64_t acc = 0;
    for (int k = RSMP_ORDER - 1; k >= 0; k--)
    {
        int64_t c = 0;
        for (int i = 0; i < RSMP_TAPS; i++)
            c += (int64_t)rsmp_matrix[k][i] * h[i];
        acc = ((acc * mu) >> RSMP_Q) + c;
    }
    return (int32_t)(acc >> RSMP_Q);
}

int rsmp_push(rsmp_t *r, int32_t x, uint64_t step, int32_t *out, int max_out)
{
    /* A cold filter would ring against five zeros and put a click at the
     * start of every sound. Start it flat at the first sample instead. */
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
        out[n++] = rsmp_at(r->hist, (uint32_t)(r->phase >> (32 - RSMP_Q)));
        r->phase += step;
    }
    /* One input consumed, so the interval moves on by one whether or not it
     * yielded anything. A step above 1.0 simply skips some intervals. */
    if (r->phase >= ((uint64_t)1 << 32))
        r->phase -= (uint64_t)1 << 32;
    return n;
}
