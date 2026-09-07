/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_WDC_VIA_H_
#define _CORE_WDC_VIA_H_

#include "core/sys/sst.h"
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

/* Every field of the vendored model. Its two ports and two timers are the
 * whole of it, and the delay pipelines inside them are why a savestate cannot
 * simply re-derive a VIA from its registers: a timer that is one cycle from
 * underflowing looks like any other. */
#define VIA_SST_SIZE 50
void via_sst_save(sst_cursor_t *c, unsigned flags);
bool via_sst_load(sst_cursor_t *c, unsigned flags);

#define VIA_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(VIA_, 1, VIA_SST_SIZE, via_sst_save, via_sst_load))

#endif /* _CORE_WDC_VIA_H_ */
