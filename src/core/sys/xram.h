/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_SYS_XRAM_H_
#define _CORE_SYS_XRAM_H_

/* 64 KB extended RAM, every machine's. Volatile because something else writes
 * it while the machine reads: the 6502 through the RIA on a Pico, DMA off the
 * PIX bus on the VGA, the fabric on a Pocket. */

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

extern volatile uint8_t *const xram;

/* What xram holds before anything writes it, on the software machine. The
 * firmwares declare it uninitialized, so random is the default and a program
 * reading a byte it never wrote fails here instead of only on a Pico. Config,
 * set before xram_init adopts it; the seed is the run's, so the fill repeats. */
void xram_set_fill(bool random, uint8_t value, uint32_t seed);
void xram_init(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. The
 * software machine's: a machine whose xram is real RAM lists no row. */
#define XRAM_SST_SIZE 0x10000
void xram_sst_save(sst_cursor_t *c, unsigned flags);
bool xram_sst_load(sst_cursor_t *c, unsigned flags);

#define XRAM_DRIVER DRIVER(xram_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(XRAM, 1, XRAM_SST_SIZE, xram_sst_save, xram_sst_load))

#endif /* _CORE_SYS_XRAM_H_ */
