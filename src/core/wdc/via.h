/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_WDC_VIA_H_
#define _CORE_WDC_VIA_H_

#include <stdbool.h>
#include <stdint.h>

/* 6502 memory map: 16 registers, A4-A15 decoded off-chip into CS1 (os.rst). */
#define VIA_MMAP_LO 0xFFD0
#define VIA_MMAP_HI 0xFFDF

/* Reset, from resb_assert: this part shares the 6502's RESB. */
void via_reset(void);

/* One PHI2 tick: counts the timers always, and services the register access when
 * the address is in the VIA's window. data is in/out. Returns the VIA's IRQ. */
bool via_tick(uint16_t addr, bool read, uint8_t *data);

/* The live chip instance (m6522_t*), for the debugger UI + DAP register access. */
void *via_chip(void);

#endif /* _CORE_WDC_VIA_H_ */
