/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_SINE_H_
#define _CORE_AUD_SINE_H_

#include <stdint.h>

/* Phase 0 is the trough and phase 128 the peak, because psg.c and bel.c
 * index this with the top byte of a phase accumulator and open their duty
 * gates around 128. */
extern int16_t sine_table[256];

/* The constant-power pan law, out of 4096: a voice at pan p weighs
 * sine_pan[63 - p] on the left and sine_pan[63 + p] on the right. The
 * centre, 1024, puts a full sine at one OPL2 channel's level. */
extern uint16_t sine_pan[127];

void sine_init(void);

#endif /* _CORE_AUD_SINE_H_ */
