/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A DWARF .debug_frame reader and unwinder for the llvm-mos two-stack 6502
 * model, in which a function has both the hardware stack at 0x0100 to 0x01FF and
 * a soft stack addressed through RS0. The call frame information llvm-mos emits
 * recovers the caller's PC, S, and RS0.
 */

#ifndef _CORE_DAP_DWARF_FRAME_H_
#define _CORE_DAP_DWARF_FRAME_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct dwarf_frame dwarf_frame_t;

dwarf_frame_t *dwarf_frame_load(const char *elf_path);
void dwarf_frame_free(dwarf_frame_t *df);

typedef struct
{
    /* JSR pushes the address of its own last operand byte, so this is one below
     * the caller's resume address. */
    uint16_t pc;
    uint16_t s16; /* the caller's S, in the 0x0100 | sp form the CFI expressions use */
    uint16_t rs0; /* the caller's soft stack pointer, which is its frame base */
    uint16_t cfa;
    bool ok;
} dwarf_unwind_t;

/* Unwinds one frame, given the live registers of the frame at pc. readmem reads
 * guest memory, which a return address held in memory needs. */
dwarf_unwind_t dwarf_frame_step(const dwarf_frame_t *df, uint16_t pc,
                                uint16_t s16, uint16_t rs0,
                                uint8_t (*readmem)(uint16_t addr));

/* True when an FDE covers pc, so unwinding from there is possible. */
bool dwarf_frame_has(const dwarf_frame_t *df, uint16_t pc);

#endif /* _CORE_DAP_DWARF_FRAME_H_ */
