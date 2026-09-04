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
 * and the rate its sink takes them. The OPL2 runs at 49716 Hz because that
 * is what a YM3812 does — 3579552 divided by exactly 72 — and the soft
 * machine adopted that rate for every voice, so on the emulator all of them
 * come through here. Every sink wants something else: the Pocket's I2S wants
 * 48000, and a PC sound card wants whatever it wants.
 *
 * A polyphase windowed sinc: 128 rows of 24 taps, row p realising a delay
 * of p/128 of an input sample, with a linear interpolation between adjacent
 * rows for the rest. One structure serves both ends — the Pocket's ratio is
 * exactly 175/169 and the emulator's is whatever the sound card returned,
 * and a table plus a lerp evaluates either.
 *
 * This replaced a Farrow structure, which computes its coefficients from a
 * polynomial in the phase instead of looking them up. That trade looked
 * right while the Cyclone V was short of block memory and stopped being
 * right when it wasn't: the Farrow measured -35 dB at 16 kHz and -14 dB at
 * 20 kHz through the Pocket's ratio, which made it the last lossy stage on
 * a path that is otherwise sixteen bits end to end. See rsmp_coef_gen.py
 * for the design and tests/cpu/aud/test_rsmp.c for the measurement — that file
 * is the ruler, and the ruler was wrong twice before the filter was.
 *
 * Integer throughout, and that is deliberate: rsmp.sv is held to this
 * file sample-for-sample the way psg is held to psg.c, which only works
 * if there is nothing to round differently.
 *
 * It lives here rather than beside the voices in ria/aud because the RP2350
 * never resamples — its OPL runs at 49716 into a PWM that will carry any
 * rate — so this is emulator and fabric code, and emu_core is the oracle the
 * RTL is held to in any case.
 */

#define RSMP_TAPS 24
#define RSMP_PHASES 128
#define RSMP_Q 17

typedef struct
{
    /* x[n-23] .. x[n], newest last. The interpolation interval is between
     * hist[11] and hist[12]; the samples either side are the context that
     * lets it be a filter rather than a straight line. */
    int32_t hist[RSMP_TAPS];
    /* Q32 position inside that interval. Wider than 32 bits because the
     * step exceeds 1.0 whenever the source outruns the sink, which is the
     * case this exists for. */
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

/* Feed one input sample, take however many output samples fall inside the
 * interval it completed — none when decimating hard, several when
 * stretching. Returns the count written to out. */
int rsmp_push(rsmp_t *r, int32_t x, uint64_t step, int32_t *out, int max_out);

#endif /* _CORE_AUD_RSMP_H_ */
