/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Minimal, self-contained DWARF .debug_info reader: the variable-inspection
 * companion to dwarf_line.c. It parses .debug_abbrev + .debug_info + .debug_str
 * out of an llvm-mos ELF and answers "what C variables are visible here, where
 * do they live, and what are their types" for the DAP adapter's Variables view.
 * No LLVM dependency. DWARF5 (32-bit) only, as emitted by the llvm-mos debug
 * fork for the rp6502 target; variable addresses are 6502 addresses directly.
 *
 * Two location forms appear in practice and are resolved here:
 *   - DW_OP_addr <abs> / DW_OP_addrx <idx> : globals / statics (an absolute 6502
 *                          address; addrx indexes .debug_addr)
 *   - DW_OP_fbreg <off>  : locals, relative to the function frame base. llvm-mos
 *                          uses DW_OP_regx RS0 as the frame base; RS0 is the soft
 *                          stack pointer pair rc0:rc1 at zero page $00, so the
 *                          frame base is the 16-bit value there (read via the
 *                          caller's readmem callback).
 */

#ifndef _EMU_DBG_DWARF_INFO_H_
#define _EMU_DBG_DWARF_INFO_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct dwarf_info dwarf_info_t;
typedef struct dtype dtype_t; /* opaque C type descriptor, owned by dwarf_info_t */

/* Load + parse the debug-info sections of an ELF. Returns NULL if the file can't
 * be read or carries no usable (DWARF5) .debug_info. */
dwarf_info_t *dwarf_info_load(const char *elf_path);
void dwarf_info_free(dwarf_info_t *di);

/* A resolved variable: a stable name + its absolute 6502 address + its type.
 * addr_ok is false when the location could not be resolved (e.g. optimized out,
 * or held in a register) — the caller should present it as unavailable. */
typedef struct
{
    const char *name; /* stable pointer owned by di */
    uint16_t addr;
    const dtype_t *type; /* may be NULL (no DW_AT_type) */
    bool addr_ok;
} dwarf_var_t;

/* Enumerate compilation-unit-level (global / file-static) variables. Writes up
 * to max entries; returns the count. */
int dwarf_info_globals(const dwarf_info_t *di, dwarf_var_t *out, int max);

/* Enumerate the locals + parameters in scope at pc: the variables of the
 * function whose [low,high) covers pc, restricted to those whose lexical block
 * also covers pc. DW_OP_fbreg locations resolve to frame_base + off; base_ok
 * false (a caller frame whose base couldn't be reconstructed) marks them
 * unresolvable, while DW_OP_addr statics still resolve. Writes up to max;
 * returns count. Returns 0 when pc is not inside any known function. */
int dwarf_info_locals(const dwarf_info_t *di, uint16_t pc, uint16_t frame_base,
                      bool base_ok, dwarf_var_t *out, int max);

/* The live frame base (soft stack pointer RS0 = rc0:rc1) of the function at pc,
 * for the top frame. False if pc is in no known function or the frame base can't
 * be evaluated. */
bool dwarf_info_frame_base(const dwarf_info_t *di, uint16_t pc,
                           uint8_t (*readmem)(uint16_t addr), uint16_t *out);

/* ---- type introspection ---- */

typedef enum
{
    DW_KIND_VOID,
    DW_KIND_BASE,    /* int/char/float/bool... (see dwarf_type_encoding) */
    DW_KIND_POINTER, /* 16-bit pointer */
    DW_KIND_ARRAY,
    DW_KIND_STRUCT,
    DW_KIND_UNION,
    DW_KIND_ENUM,
    DW_KIND_FUNC,
    DW_KIND_UNKNOWN,
} dw_kind_t;

/* DWARF base-type encodings (DW_ATE_*) the formatter cares about. */
enum
{
    DW_ATE_address = 0x01,
    DW_ATE_boolean = 0x02,
    DW_ATE_float = 0x04,
    DW_ATE_signed = 0x05,
    DW_ATE_signed_char = 0x06,
    DW_ATE_unsigned = 0x07,
    DW_ATE_unsigned_char = 0x08,
};

dw_kind_t dwarf_type_kind(const dtype_t *t);
uint32_t dwarf_type_size(const dtype_t *t);   /* total byte size (0 if unknown) */
const char *dwarf_type_name(const dtype_t *t); /* display name, e.g. "char [256]" */
int dwarf_type_encoding(const dtype_t *t);     /* DW_ATE_* for DW_KIND_BASE, else 0 */

/* DW_KIND_POINTER: the pointed-to type (NULL for void*). */
const dtype_t *dwarf_type_pointee(const dtype_t *t);

/* DW_KIND_ARRAY: element type + element count (*count). */
const dtype_t *dwarf_type_element(const dtype_t *t, uint32_t *count);

/* DW_KIND_STRUCT / DW_KIND_UNION: member access. */
int dwarf_type_member_count(const dtype_t *t);
bool dwarf_type_member(const dtype_t *t, int i, const char **name,
                       uint32_t *offset, const dtype_t **type);

/* DW_KIND_ENUM: the enumerator name for value, or NULL. */
const char *dwarf_type_enum_name(const dtype_t *t, int64_t value);

#endif /* _EMU_DBG_DWARF_INFO_H_ */
