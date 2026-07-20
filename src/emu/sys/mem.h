/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SYS_MEM_H_
#define _EMU_SYS_MEM_H_

#include "ria/sys/mem.h"

/* RAM is $0000-$FEFF (os.rst); ram[] itself spans the whole space as a write-through
 * shadow the debug views and the ROM loader read. */
#define MEM_RAM_HI 0xFEFF

extern uint8_t ram[0x10000];

/* One PHI2 tick of the SRAM. data is in/out. */
void mem_tick(uint16_t addr, bool read, uint8_t *data);

#endif /* _EMU_SYS_MEM_H_ */
