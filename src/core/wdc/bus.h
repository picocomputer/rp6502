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

/* Puts the bus back in step with the beam at the running PHI2 rate, with no
 * budget owed and no cycles counted, after a failed savestate load. */
void bus_resync(void);

/* Run the bus, and every device on it, up to where the beam has reached. */
void bus_task(void);

extern const sst_table_t bus_sst;

#define BUS_DRIVER DRIVER(                                  \
    TASK_PUMPED(bus_task)                                   \
    RESET(bus_resync)                                       \
    SST("BUS_", bus))

#endif /* _CORE_WDC_BUS_H_ */
