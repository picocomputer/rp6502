/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_WDC_CPU_H_
#define _CORE_WDC_CPU_H_

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

/* w65c02_init returns a pin mask with RES asserted, so the cycles after this
 * run the reset sequence and fetch the vector at $FFFC/$FFFD. */
void cpu_reset(void);

/* Advance one PHI2 cycle. data is in and out: a write cycle drives it, and a
 * read cycle takes what the bus supplies. */
void cpu_tick(uint16_t *addr, bool *read, uint8_t *data, bool irq);

bool cpu_opcode_fetch(uint16_t *pc, uint8_t *sp);

uint64_t cpu_dbg_pins(void);

/* The debug code casts this to w65c02_t*, which is why the chip header does
 * not have to be included here. */
void *cpu_chip(void); /* w65c02_t* */

extern void (*cpu_dbg_cycle_cb)(uint64_t pins);

#define CPU_SST_SIZE 34
void cpu_sst_save(sst_cursor_t *c, unsigned flags);
bool cpu_sst_load(sst_cursor_t *c, unsigned flags);

#define CPU_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(CPU_, 1, CPU_SST_SIZE, cpu_sst_save, cpu_sst_load))

#endif /* _CORE_WDC_CPU_H_ */
