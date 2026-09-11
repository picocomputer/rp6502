/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_SYS_XRAM_H_
#define _CORE_SYS_XRAM_H_

/* 64 KB extended RAM, every machine's. Volatile because something else writes
 * it while the machine reads: the 6502 through the RIA on a Pico, DMA off the
 * PIX bus on the VGA. On a Pocket it is not an array at all but a fixed
 * address in the fabric's memory map. */

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

extern volatile uint8_t *const xram;

/* What xram holds before anything writes it, on the software machine, where
 * random is the default because the firmwares leave their own uninitialized.
 * It must be set before xram_init, which is what reads it. */
void xram_set_fill(bool random, uint8_t value, uint32_t seed);
void xram_init(void);

#define XRAM_SST_SIZE 0x10000
void xram_sst_save(sst_cursor_t *c, unsigned flags);
bool xram_sst_load(sst_cursor_t *c, unsigned flags);

/* Only the software machines list this row, because they are the ones that
 * fill xram at boot and carry it in a savestate. The Pico's RIA and VGA
 * compile xram.c too, but nothing there calls xram_init. */
#define XRAM_DRIVER DRIVER(xram_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(XRAM, 1, XRAM_SST_SIZE, xram_sst_save, xram_sst_load))

#endif /* _CORE_SYS_XRAM_H_ */
