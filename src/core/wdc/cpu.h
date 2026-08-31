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
#include "core/rp2350.h" /* SYS_RP2350_KHZ */

/* The RIA's PIO clock divider is 16.8 fixed point, so a PHI2 period is
 * 32*int + frac/8 system ticks -- not an integer. Counting the system clock in
 * eighths makes every achievable PHI2 exact: 2048 MHz, no accumulated rounding.
 * This is the unit cpu_cycle_ticks and host_clock_us are denominated in. */
#define SYS_OVERSAMPLE 8
#define SYS_TICKS_PER_US (SYS_RP2350_KHZ * SYS_OVERSAMPLE / 1000) /* 2048 */

/* Cold boot: the clock, the pins and the core, from nothing. */
void cpu_init(void);
bool cpu_check_phi2_khz(uint16_t *v);

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

/* Run the 6502 up to the beam -- and with it every device on the bus, because
 * this chip is the only master. */
void cpu_task(void);

/* This driver's row in a machine's driver list; see core/driver.h. A row lives with the
 * implementation, not the contract: which hooks a machine's CPU has is the
 * implementation's answer, and three of them differ. */
/* A software machine quantizes at init, so there is nothing to apply when
 * the setting moves. */
#define CPU_CONFIG_PHI2 CONFIG_INT(P, cpu, phi2_khz, uint16_t, CPU_PHI2_DEFAULT, \
    cpu_check_phi2_khz, nul_apply, STR_PHI2, cpu_phi2_response, STR_HELP_SET_PHI2, NULL)
#define CPU_DRIVER DRIVER(cpu_init, cpu_task, nul_task, cpu_run, cpu_stop, nul_break, \
    CPU_CONFIG_PHI2, nul_config)

#endif /* _CORE_WDC_CPU_H_ */
