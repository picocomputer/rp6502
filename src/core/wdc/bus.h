/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_WDC_BUS_H_
#define _CORE_WDC_BUS_H_

#include "core/sys/sst.h"
#include <stdint.h>

/* 6502 cycles this machine has run, ever. Nothing in the machine reads this;
 * it is here so that a test can say how many cycles a frame was worth, which
 * is otherwise unobservable from outside. */
uint64_t bus_cycles(void);

void bus_reset(void);

/* Run the bus, and every device on it, up to where the beam has reached. */
void bus_task(void);

#define BUS_SST_SIZE 30
void bus_sst_save(sst_cursor_t *c, unsigned flags);
bool bus_sst_load(sst_cursor_t *c, unsigned flags);

#define BUS_DRIVER DRIVER(nul_init, bus_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(BUS_, 1, BUS_SST_SIZE, bus_sst_save, bus_sst_load))

#endif /* _CORE_WDC_BUS_H_ */
