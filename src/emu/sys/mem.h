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

/* One PHI2 tick of the SRAM. data is in/out. */
void mem_tick(uint16_t addr, bool read, uint8_t *data);

#endif /* _EMU_SYS_MEM_H_ */
