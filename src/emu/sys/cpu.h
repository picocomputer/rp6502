/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SYS_CPU_H_
#define _EMU_SYS_CPU_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The firmware contract cpu.c implements: cpu_init, cpu_active,
 * cpu_set_phi2_khz_run (clamped to [CPU_PHI2_MIN_KHZ, CPU_PHI2_MAX_KHZ],
 * quantized), cpu_get_phi2_khz_run, and the CPU_RP2350_KHZ / CPU_PHI2_*
 * constants. Wrapped here so C++ consumers get C linkage. */
#include "sys/cpu.h"

/* The master clock unit is 1/8 of a 256 MHz tick (2048 per microsecond), so
 * the PHI2 fractional divider lands on an integer per-cycle step. */

/* Program start: reset the 65C02 core (fetch the vector) and unhalt, keeping the
 * clock and PHI2. Must be last in the run fan-out. */
void cpu_run(void);

/* Program stop: halt the 65C02 (freeze ticking). */
void cpu_stop(void);

uint64_t cpu_tick(void);   /* advance one PHI2 cycle; returns the bus pins */
uint32_t cpu_step_8(void); /* 1/8-tick units advanced per 6502 cycle */

/* True on an opcode fetch (SYNC); out-writes the fetch PC and SP. */
bool cpu_opcode_fetch(uint64_t pins, uint16_t *pc, uint8_t *sp);

/* Program-halt gate: the CPU stops ticking once halted (the EXIT syscall, a
 * failed exec, or a --dap launch hold set it; cpu_run clears it on restart).
 * cpu_active() — the firmware contract — is its inverse. */
bool cpu_halted(void);
void cpu_set_halted(bool halted);

/* The live 65C02 instance, for the debugger UI + DAP register access (the
 * debug code casts to m6502_t*, which includes the chip header, so this need
 * not pull it in). */
void *cpu_chip(void); /* m6502_t* */

/* Optional per-CPU-cycle observer for the debugger UI. Display-only and MUST
 * NOT gate the CPU — dbg.c is the one authoritative engine. NULL when no
 * observer is registered. */
extern void (*cpu_dbg_cycle_cb)(uint64_t pins);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_SYS_CPU_H_ */
