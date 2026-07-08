/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_CPU_H_
#define _EMU_CPU_H_

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

/* Warm restart (exec): reset the 65C02 core, keeping the clock and PHI2. */
void cpu_reset(void);

/* Run 6502 cycles until the master clock reaches deadline_8, the program
 * halts, or (dbg) an instruction breakpoint stops the machine. Returns true on
 * a breakpoint stop, leaving the clock mid-scanline; otherwise the clock is at
 * deadline_8 or later on return (time flows even while halted). */
bool cpu_run_until(uint64_t deadline_8, bool dbg);

/* Program-halt gate: the CPU stops ticking once halted (the EXIT syscall, a
 * failed exec, or a --dap launch hold set it; ria_reset clears it on restart).
 * cpu_active() — the firmware contract — is its inverse. */
bool cpu_halted(void);
void cpu_set_halted(bool halted);

/* Deterministic virtual microsecond clock — the master clock all timing
 * derives from; the same number of frames always yields the same time. */
uint64_t cpu_now_us(void);

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

#endif /* _EMU_CPU_H_ */
