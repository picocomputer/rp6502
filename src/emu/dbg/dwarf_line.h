/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Minimal, self-contained DWARF .debug_line reader: parses the line-number
 * program out of an llvm-mos ELF and answers address<->source-line queries for
 * the DAP adapter. No LLVM dependency. DWARF 2-4 (32-bit) is supported, which is
 * what `clang -gdwarf-4` emits for the rp6502 target; the addresses in the table
 * are the 6502 load addresses, i.e. the emulator's PC directly.
 */

#ifndef _EMU_DWARF_LINE_H_
#define _EMU_DWARF_LINE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct dwarf_line dwarf_line_t;

/* Load + parse .debug_line from an ELF. Returns NULL if the file can't be read
 * or carries no usable (DWARF 2-4) line table. */
dwarf_line_t *dwarf_line_load(const char *elf_path);
void dwarf_line_free(dwarf_line_t *dl);

/* addr -> the source location covering it: a stable full-path pointer (owned by
 * dl) + 1-based line. False if no row covers addr. */
bool dwarf_line_addr_to_src(const dwarf_line_t *dl, uint16_t addr,
                            const char **file, int *line);

/* (source file, 1-based line) -> the lowest code address at that line, or the
 * next code line at/after it (so a breakpoint on a blank line binds forward).
 * The file is matched by basename. *bound_line returns the line actually bound. */
bool dwarf_line_src_to_addr(const dwarf_line_t *dl, const char *file, int line,
                            uint16_t *addr, int *bound_line);

/* The function symbol (.symtab STT_FUNC) enclosing addr, or NULL. Stable pointer
 * owned by dl. */
const char *dwarf_line_addr_to_func(const dwarf_line_t *dl, uint16_t addr);

/* A function's entry address by name (.symtab STT_FUNC). False if not found. */
bool dwarf_line_func_addr(const dwarf_line_t *dl, const char *name, uint16_t *addr);

/* ---- allocatable ELF sections (.text/.data/.bss/.zp/...) ----
 * Each section's 6502 load address + size, for the memory-map view. */
typedef struct
{
    const char *name; /* stable pointer owned by dl */
    uint16_t addr;
    uint32_t size;
} dwarf_section_t;

/* The SHF_ALLOC sections with a non-zero size. Returns count (<= max). */
int dwarf_line_sections(const dwarf_line_t *dl, dwarf_section_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_DWARF_LINE_H_ */
