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

#define VIA_WINDOW_LO 0xFFD0
#define VIA_WINDOW_HI 0xFFDF /* inclusive */

/* Program start: reset the VIA (it shares the 6502 RESB). */
void via_run(void);

/* One PHI2 tick: counts the timers always, and services the register access when
 * the address is in the VIA's window. data is in/out. Returns the VIA's IRQ. */
bool via_tick(uint16_t addr, bool read, uint8_t *data);

/* The live chip instance (m6522_t*), for the debugger UI + DAP register access. */
void *via_chip(void);

#endif /* _EMU_SYS_VIA_H_ */
