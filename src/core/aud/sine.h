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

void sine_init(void);

#endif /* _CORE_AUD_SINE_H_ */
