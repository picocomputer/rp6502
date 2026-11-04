/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_WDC_BUS_H_
#define _CORE_WDC_BUS_H_

#include "core/sys/sst.h"
#include <stdint.h>

/* 6502 cycles this machine has run, ever, counted as each one runs. */
uint64_t bus_cycles(void);

/* The machine's present as a count of whole periods of an hz clock that
 * started with scanline 0: the period the cycle now running starts in, or
 * between scanlines the one the next cycle will start in. */
uint64_t bus_now(uint32_t hz);

void bus_reset(void);

/* Run the bus, and every device on it, up to where the beam has reached. */
void bus_task(void);

#define BUS_SST_SIZE 30
void bus_sst_save(sst_cursor_t *c, unsigned flags);
bool bus_sst_load(sst_cursor_t *c, unsigned flags);

#define BUS_DRIVER DRIVER(nul_init, bus_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(BUS_, 1, BUS_SST_SIZE, bus_sst_save, bus_sst_load))

#endif /* _CORE_WDC_BUS_H_ */
