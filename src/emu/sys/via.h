/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_VIA_H_
#define _EMU_VIA_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Reset the VIA (system reset / program (re)start). */
void via_reset(void);

/* One PHI2 tick: counts the timers and, when the CPU addresses $FFD0-$FFDF,
 * performs the register access. pins is the CPU pin mask; the returned mask
 * carries read data and the IRQ line (shared bit with M6502_IRQ). */
uint64_t via_tick(uint64_t pins);

/* The live chip instance (m6522_t*), for the debugger UI + DAP register access. */
void *via_chip(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_VIA_H_ */
