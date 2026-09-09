/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_WDC_SRAM_H_
#define _CORE_WDC_SRAM_H_

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

#define SRAM_MMAP_LO 0x0000
#define SRAM_MMAP_HI 0xFEFF

extern uint8_t sram[0x10000];

/* Hardware zeroes nothing -- the 6502's SRAM keeps whatever was last in it --
 * so a random fill is the default and a program that reads a byte it never
 * wrote fails here too, not only on a Pico. Set this before sram_init. */
void sram_set_fill(bool random, uint8_t value, uint32_t seed);

/* proc_boot calls this before rom_load on a PROC_REFILL boot, so what the ROM
 * writes lands on top of the fill. It is also SRAM_DRIVER's init slot. */
void sram_init(void);

/* data is in and out. */
void sram_tick(uint16_t addr, bool read, uint8_t *data);

#define SRAM_SST_SIZE 0x10000
void sram_sst_save(sst_cursor_t *c, unsigned flags);
bool sram_sst_load(sst_cursor_t *c, unsigned flags);

#define SRAM_DRIVER DRIVER(sram_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(SRAM, 1, SRAM_SST_SIZE, sram_sst_save, sram_sst_load))

#endif /* _CORE_WDC_SRAM_H_ */
