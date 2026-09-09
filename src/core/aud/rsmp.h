/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_RSMP_H_
#define _CORE_AUD_RSMP_H_

#include <stdbool.h>
#include <stdint.h>

/* Arbitrary-ratio resampling, between the rate a machine makes samples at
 * and the rate its sink takes them.
 *
 * A polyphase windowed sinc: 128 rows of 24 taps, row p realising a delay
 * of p/128 of an input sample, with a linear interpolation between adjacent
 * rows for the rest. src/core/gen/rsmp_coef_gen.py designs the rows and
 * records why they are what they are; tests/cpu/aud/test_rsmp.c measures
 * the result.
 *
 * The arithmetic is integer throughout because rsmp.sv has to reproduce
 * this file sample for sample, which only works if there is nothing to
 * round differently.
 */

#define RSMP_TAPS 24
#define RSMP_PHASES 128
#define RSMP_Q 17

typedef struct
{
    /* x[n-23] .. x[n], newest last. The interpolation interval is between
     * hist[11] and hist[12], which is where rsmp_coef_gen.py puts it. */
    int32_t hist[RSMP_TAPS];
    /* Q32 position inside that interval. It needs more than 32 bits because
     * the step exceeds 1.0 whenever the source outruns the sink, and
     * rsmp_push adds a whole step before it wraps. */
    uint64_t phase;
    bool primed;
} rsmp_t;

/* Row p is the filter for a delay of p/RSMP_PHASES. There are PHASES+1 of
 * them: the last is the first shifted by one tap, so interpolating between
 * p and p+1 never has to special-case the wrap. */
extern const int32_t rsmp_coef[RSMP_PHASES + 1][RSMP_TAPS];

void rsmp_reset(rsmp_t *r);

/* Input frames per output frame, Q32. */
uint64_t rsmp_step(uint32_t in_rate, uint32_t out_rate);

/* Feed one input sample and take the output samples that fall inside the
 * interval it completed: none when decimating, several when stretching.
 * Returns the count written to out. */
int rsmp_push(rsmp_t *r, int32_t x, uint64_t step, int32_t *out, int max_out);

#endif /* _CORE_AUD_RSMP_H_ */
