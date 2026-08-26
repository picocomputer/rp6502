/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_WDC_CPU_H_
#define _CORE_WDC_CPU_H_

#include <stdbool.h>
#include <stdint.h>

/* The machine's 6502 contract this file completes: cpu_active, the config and
 * run PHI2 pairs, and the CPU_PHI2_* range. */
#include "core/cpu.h"

/* Cold boot: the clock, the pins and the core, from nothing. */
void cpu_init(void);

/* Program start: reset the 65C02 core (fetch the vector) and unhalt, keeping the
 * clock and PHI2. Must be last in the run fan-out. */
void cpu_run(void);

/* Program stop: halt the 65C02 (freeze ticking). */
void cpu_stop(void);

/* Advance the 6502 one PHI2 cycle. irq is the interrupt line as the devices left it
 * last cycle; data is in/out — the value the bus settled on, then the value the CPU
 * drives. The w65c02 pin mask stays inside cpu.c; the board speaks decoded signals. */
void cpu_tick(uint16_t *addr, bool *read, uint8_t *data, bool irq);

uint32_t cpu_cycle_ticks(void); /* system-clock ticks per 6502 cycle */

/* True on an opcode fetch (SYNC); out-writes the fetch PC and SP. */
bool cpu_opcode_fetch(uint16_t *pc, uint8_t *sp);

/* The raw w65c02 pin mask, for the debugger's per-cycle observer only. */
uint64_t cpu_dbg_pins(void);

/* Program-halt gate: the CPU stops ticking once halted (the EXIT syscall, a
 * failed exec, or a --dap launch hold set it; cpu_run clears it on restart).
 * cpu_active() — the firmware contract — is its inverse. */
bool cpu_halted(void);
void cpu_set_halted(bool halted);

/* The live 65C02 instance, for the debugger UI + DAP register access (the
 * debug code casts to w65c02_t*, which includes the chip header, so this need
 * not pull it in). */
void *cpu_chip(void); /* w65c02_t* */

/* Optional per-CPU-cycle observer for the debugger UI. Display-only and MUST
 * NOT gate the CPU — dbg.c is the one authoritative engine. NULL when no
 * observer is registered. */
extern void (*cpu_dbg_cycle_cb)(uint64_t pins);

#endif /* _CORE_WDC_CPU_H_ */
