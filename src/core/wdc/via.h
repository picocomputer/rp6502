/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_WDC_VIA_H_
#define _CORE_WDC_VIA_H_

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

/* A4-A15 are decoded off-chip into CS1, leaving A0-A3 to select one of the
 * 16 registers (os.rst). */
#define VIA_MMAP_LO 0xFFD0
#define VIA_MMAP_HI 0xFFDF

/* This part shares the 6502's RESB. */
void via_reset(void);

/* data is in and out. Returns the VIA's IRQ line. */
bool via_tick(uint16_t addr, bool read, uint8_t *data);

void *via_chip(void); /* m6522_t* */

/* A register as a read finds it, and set the same way, with none of what a
 * read or a write on the bus does. For a script. */
uint8_t via_reg_peek(uint16_t addr);
void via_reg_poke(uint16_t addr, uint8_t data);

/* A write as the 6502 makes it, without the cycle a bus tick would add. */
void via_reg_write(uint16_t addr, uint8_t data);

extern const sst_table_t via_sst;

#define VIA_DRIVER DRIVER(                                  \
    SST("VIA_", via))

#endif /* _CORE_WDC_VIA_H_ */
