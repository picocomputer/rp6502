/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_WDC_VIA_H_
#define _CORE_WDC_VIA_H_

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

/* A4-A15 are decoded off-chip into CS1, leaving A0-A3 to select one of the
 * 16 registers (os.rst). */
#define VIA_MMAP_LO 0xFFD0
#define VIA_MMAP_HI 0xFFDF

/* This part shares the 6502's RESB. */
void via_reset(void);

/* data is in and out. Returns the VIA's IRQ line. */
bool via_tick(uint16_t addr, bool read, uint8_t *data);

void *via_chip(void); /* m6522_t* */

/* Every field of the vendored model, because the delay pipelines inside the
 * timers and the interrupt logic cannot be re-derived from the registers: a
 * timer one cycle from underflowing looks like any other. */
#define VIA_SST_SIZE 50
void via_sst_save(sst_cursor_t *c, unsigned flags);
bool via_sst_load(sst_cursor_t *c, unsigned flags);

#define VIA_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(VIA_, 1, VIA_SST_SIZE, via_sst_save, via_sst_load))

#endif /* _CORE_WDC_VIA_H_ */
