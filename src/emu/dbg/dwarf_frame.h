/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Minimal, self-contained DWARF .debug_frame (CFI) reader + unwinder for the
 * llvm-mos two-stack 6502 model. No LLVM dependency. llvm-mos emits real CFI
 * (CIE/FDE with expressions) that normalizes the hardware stack (0x0100-0x01FF)
 * and recovers the caller's PC, S, and soft-stack pointer RS0. This replaces the
 * heuristic hardware-stack scan + prologue-size chaining used before CFI existed.
 */

#ifndef _EMU_DBG_DWARF_FRAME_H_
#define _EMU_DBG_DWARF_FRAME_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct dwarf_frame dwarf_frame_t;

/* Load + parse .debug_frame from an ELF. NULL if absent/unusable. */
dwarf_frame_t *dwarf_frame_load(const char *elf_path);
void dwarf_frame_free(dwarf_frame_t *df);

/* The result of unwinding one frame: the caller's register values + this frame's
 * CFA. s16 is the S register in the 0x0100|sp form the CFI expressions use. */
typedef struct
{
    uint16_t pc;   /* caller PC (the return slot value; a call-site address) */
    uint16_t s16;  /* caller S, as 0x0100 | sp */
    uint16_t rs0;  /* caller soft-stack pointer (its frame base) */
    uint16_t cfa;  /* this frame's canonical frame address */
    bool ok;
} dwarf_unwind_t;

/* Unwind one frame at pc, given the current frame's live registers: s16 =
 * 0x0100 | (6502 SP), rs0 = the soft-stack pointer value. readmem reads guest
 * memory (for return-address deref). ok=false if no FDE covers pc or a rule
 * can't be evaluated. */
dwarf_unwind_t dwarf_frame_step(const dwarf_frame_t *df, uint16_t pc,
                                uint16_t s16, uint16_t rs0,
                                uint8_t (*readmem)(uint16_t addr));

/* True if any FDE covers pc (i.e. CFI-based unwinding is available here). */
bool dwarf_frame_has(const dwarf_frame_t *df, uint16_t pc);

#endif /* _EMU_DBG_DWARF_FRAME_H_ */
