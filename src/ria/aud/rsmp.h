/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_AUD_RSMP_H_
#define _RIA_AUD_RSMP_H_

#include <stdbool.h>
#include <stdint.h>

/* Arbitrary-ratio resampling, for the one voice whose rate is not ours to
 * choose. The OPL2 runs at 49716 Hz because that is what a YM3812 does —
 * 3579552 divided by exactly 72 — and every sink wants something else: the
 * Pocket's I2S wants 48000, and a PC sound card wants whatever it wants.
 * Everything else this machine makes a noise with is generated at the rate
 * the sink asked for and needs none of this.
 *
 * A Farrow structure rather than a polyphase table, because the two hosts
 * have opposite scarcities and this suits both. The Cyclone V has twenty
 * spare DSP blocks and 1050 clocks between output samples, but only eight
 * spare M10K and a 64 KB TCM already wanting sixteen of them — so a
 * 169-phase coefficient table is the one thing it cannot afford, while a
 * few dozen multiply-accumulates are free. Farrow keeps the coefficients in
 * a 6x6 matrix and computes the phase instead of looking it up.
 *
 * It is also the only structure that suits both ends. The Pocket's ratio is
 * exactly 175/169 — both rates divide the same 50.4 MHz, so the phase
 * sequence is one of 169 exact values and can never drift — while the
 * emulator's is whatever the sound card returned. A polyphase table would
 * need a different design for each; a Farrow evaluates any fraction.
 *
 * The sub-filters are least-squares designed, not Lagrange. Lagrange is the
 * obvious choice and it is maximally flat at DC, which sounds right until
 * it is measured: it spends every degree of freedom at DC, and above the
 * midband its gain starts to depend on the fractional phase — which is
 * heard as roughness, not as a level error. Through the integer path at
 * 49704 into 48000 that costs -57 dB at 8 kHz and -36 dB at 12 kHz. The
 * same structure fitted properly gives -85 and -77. See rsmp_coef_gen.py
 * for the design and for why the weighting matters.
 *
 * Integer throughout, and that is deliberate: aud_rsmp.sv is held to this
 * file sample-for-sample the way aud_psg is held to psg.c, which only works
 * if there is nothing to round differently.
 */

#define RSMP_TAPS 10
#define RSMP_ORDER 6
#define RSMP_Q 14

typedef struct
{
    /* x[n-9] .. x[n], newest last. The interpolation interval is between
     * hist[4] and hist[5]; the samples either side are the context that
     * lets it be a filter rather than a straight line. */
    int32_t hist[RSMP_TAPS];
    /* Q32 position inside that interval. Wider than 32 bits because the
     * step exceeds 1.0 whenever the source outruns the sink, which is the
     * case this exists for. */
    uint64_t phase;
    bool primed;
} rsmp_t;

/* Q14 Farrow matrix. Row k holds the ten-tap filter whose output is the
 * coefficient of u^k. Every row but the first sums to zero and the first
 * sums to one, so the gain is exactly unity at DC for every phase — the
 * one property worth constraining rather than fitting. */
extern const int16_t rsmp_matrix[RSMP_ORDER][RSMP_TAPS];

void rsmp_reset(rsmp_t *r);

/* Input frames per output frame, Q32. */
uint64_t rsmp_step(uint32_t in_rate, uint32_t out_rate);

/* Feed one input sample, take however many output samples fall inside the
 * interval it completed — none when decimating hard, several when
 * stretching. Returns the count written to out. */
int rsmp_push(rsmp_t *r, int32_t x, uint64_t step, int32_t *out, int max_out);

#endif /* _RIA_AUD_RSMP_H_ */
