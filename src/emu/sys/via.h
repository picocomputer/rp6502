/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SYS_VIA_H_
#define _EMU_SYS_VIA_H_

#include <stdbool.h>
#include <stdint.h>

/* Program start: reset the VIA (it shares the 6502 RESB). */
void via_run(void);

/* One PHI2 tick: counts the timers and, when selected, performs the register
 * access. The board decodes $FFD0-$FFDF and passes the result as selected. Returns
 * the VIA's own pin mask — read data (valid only when selected) and M6522_IRQ. */
uint64_t via_tick(uint64_t pins, bool selected);

/* The live chip instance (m6522_t*), for the debugger UI + DAP register access. */
void *via_chip(void);

#endif /* _EMU_SYS_VIA_H_ */
