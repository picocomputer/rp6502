/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A reader for the .debug_line section of an llvm-mos ELF. It understands only
 * 32-bit DWARF5, which is what the llvm-mos debug fork emits. The addresses in
 * the table are 6502 load addresses, so they compare directly against the PC.
 */

#ifndef _CORE_DAP_DWARF_LINE_H_
#define _CORE_DAP_DWARF_LINE_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct dwarf_line dwarf_line_t;

dwarf_line_t *dwarf_line_load(const char *elf_path);
void dwarf_line_free(dwarf_line_t *dl);

/* The file is the DWARF directory entry joined to the file name, so it is
 * relative when that entry is. It is owned by dl, and the line is 1-based. */
bool dwarf_line_addr_to_src(const dwarf_line_t *dl, uint16_t addr,
                            const char **file, int *line);

/* A line with no code binds forward to the next line that has some, so
 * bound_line reports the line actually bound. A whole-component path suffix
 * match wins over a basename-only one, which is what tells a/util.c from
 * b/util.c. */
bool dwarf_line_src_to_addr(const dwarf_line_t *dl, const char *file, int line,
                            uint16_t *addr, int *bound_line);

/* The last .symtab STT_FUNC symbol at or below addr, owned by dl, or NULL. It
 * is accepted when addr falls within the symbol's size, or when that size is
 * zero, because zero means unknown rather than empty. */
const char *dwarf_line_addr_to_func(const dwarf_line_t *dl, uint16_t addr);

bool dwarf_line_func_addr(const dwarf_line_t *dl, const char *name, uint16_t *addr);

/* An allocatable ELF section, for the memory map view. */
typedef struct
{
    const char *name; /* owned by dl */
    uint16_t addr;
    uint32_t size;
} dwarf_section_t;

int dwarf_line_sections(const dwarf_line_t *dl, dwarf_section_t *out, int max);

#endif /* _CORE_DAP_DWARF_LINE_H_ */
