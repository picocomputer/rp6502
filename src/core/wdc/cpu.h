/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502 as software, beside the same part as fabric in cpu.sv.
 *
 * These keep the cpu_ prefix: the vendored model owns w65c02_, and this is
 * the board's side of it. (via.h has the room to say via_ because the model
 * beneath it is m6522_.)
 */

#ifndef _CORE_WDC_CPU_H_
#define _CORE_WDC_CPU_H_

#include <stdbool.h>
#include <stdint.h>

/* Reset, from resb_assert: this part is what the line resets. The pin mask
 * comes back with RES asserted, so the cycles after the line rises run the
 * reset sequence and fetch the vector at $FFFC/$FFFD. */
void cpu_reset(void);

/* Advance one PHI2 cycle. irq is the interrupt line as the devices left it
 * last cycle; data is in/out -- the value the bus settled on, then the value
 * the CPU drives. The w65c02 pin mask stays inside w65c02.c; the board speaks
 * decoded signals. */
void cpu_tick(uint16_t *addr, bool *read, uint8_t *data, bool irq);

/* True on an opcode fetch (SYNC); out-writes the fetch PC and SP. */
bool cpu_opcode_fetch(uint16_t *pc, uint8_t *sp);

/* The raw w65c02 pin mask, for the debugger's per-cycle observer only. */
uint64_t cpu_dbg_pins(void);

/* The live 65C02 instance, for the debugger UI + DAP register access (the
 * debug code casts to w65c02_t*, which includes the chip header, so this need
 * not pull it in). */
void *cpu_chip(void); /* w65c02_t* */

/* Optional per-CPU-cycle observer for the debugger UI. Display-only and MUST
 * NOT gate the CPU -- dbg.c is the one authoritative engine. NULL when no
 * observer is registered. */
extern void (*cpu_dbg_cycle_cb)(uint64_t pins);

#endif /* _CORE_WDC_CPU_H_ */
