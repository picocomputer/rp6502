/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_WDC_SRAM_H_
#define _CORE_WDC_SRAM_H_

/* The 6502's RAM, on the software machine. The rest of the map is unassigned,
 * then the VIA and the RIA (os.rst). sram[] itself spans the whole space;
 * outside this window it is a write-through shadow the debug views and the
 * ROM loader read. */

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

#define SRAM_MMAP_LO 0x0000
#define SRAM_MMAP_HI 0xFEFF

extern uint8_t sram[0x10000];

/* What sram[] holds before anything writes it. Hardware zeroes nothing -- the
 * 6502's SRAM keeps whatever was last in it -- so random is the default and a
 * program reading a byte it never wrote fails here instead of only on a Pico.
 * Config, set before sram_init adopts it; the seed is the run's, so the fill
 * repeats. */
void sram_set_fill(bool random, uint8_t value, uint32_t seed);

/* Fill memory. Before the ROM loader in a machine's drivers, so what it writes
 * lands on top of the fill, which is the order the hardware does it in. */
void sram_init(void);

/* One PHI2 tick of the SRAM. data is in/out. */
void sram_tick(uint16_t addr, bool read, uint8_t *data);

/* The whole 64 KB, which is the machine's memory and nothing else: the fill
 * settings above are a session's and stay behind. */
#define SRAM_SST_SIZE 0x10000
void sram_sst_save(sst_cursor_t *c, unsigned flags);
bool sram_sst_load(sst_cursor_t *c, unsigned flags);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define SRAM_DRIVER DRIVER(sram_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(SRAM, 1, SRAM_SST_SIZE, sram_sst_save, sram_sst_load))

#endif /* _CORE_WDC_SRAM_H_ */
