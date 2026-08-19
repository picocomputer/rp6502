/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SYS_MEM_H_
#define _EMU_SYS_MEM_H_

#include "ria/sys/mem.h"

/* 6502 RAM. The rest of the map is unassigned, then the VIA and the RIA (os.rst).
 * ram[] itself spans the whole space; outside this window it is a write-through
 * shadow the debug views and the ROM loader read. */
#define MEM_MMAP_LO 0x0000
#define MEM_MMAP_HI 0xFEFF

extern uint8_t ram[0x10000];

/* What ram[] and xram[] hold before anything writes them. Hardware zeroes
 * neither — the 6502's SRAM keeps whatever was last in it, and both firmwares
 * declare xram __uninitialized_ram() — so random is the default and a program
 * reading a byte it never wrote fails here instead of only on a Pico. Config,
 * set before mem_init adopts it; the seed is the run's, so the fill repeats. */
void mem_set_fill(bool random, uint8_t value, uint64_t seed);

/* Fill memory. First of the drivers, so what the ROM loader writes lands on top
 * of the fill, which is the order the hardware does it in. */
void mem_init(void);

/* One PHI2 tick of the SRAM. data is in/out. */
void mem_tick(uint16_t addr, bool read, uint8_t *data);

#endif /* _EMU_SYS_MEM_H_ */
