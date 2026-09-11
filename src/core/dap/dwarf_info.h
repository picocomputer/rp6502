/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A reader for the .debug_info of an llvm-mos ELF: which C variables are
 * visible at an address, where they live, and what their types are. It
 * understands only 32-bit DWARF5, which is what the llvm-mos debug fork emits,
 * and the addresses it reports are 6502 addresses.
 *
 * Two location forms appear in practice. A global or a static is DW_OP_addr with
 * an absolute address, or DW_OP_addrx with an index into .debug_addr. A local is
 * DW_OP_fbreg with an offset from the frame base, which llvm-mos gives as
 * DW_OP_regx RS0, the soft stack pointer held in the register pair rc0 and rc1;
 * reading its zero-page bytes is what the readmem callback below is for.
 */

#ifndef _CORE_DAP_DWARF_INFO_H_
#define _CORE_DAP_DWARF_INFO_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct dwarf_info dwarf_info_t;
typedef struct dtype dtype_t; /* owned by dwarf_info_t */

dwarf_info_t *dwarf_info_load(const char *elf_path);
void dwarf_info_free(dwarf_info_t *di);

typedef struct
{
    const char *name; /* owned by di */
    uint16_t addr;
    const dtype_t *type; /* NULL when the DIE carries no DW_AT_type */
    /* False when the location could not be resolved. A global may have been
     * optimized out or live in a register; a local is more often unresolvable
     * because its frame base could not be reconstructed. */
    bool addr_ok;
} dwarf_var_t;

/* Every compilation unit's variables, which are the globals and the file
 * statics. */
int dwarf_info_globals(const dwarf_info_t *di, dwarf_var_t *out, int max);

/* The locals and parameters whose function and whose lexical block both cover
 * pc. Pass base_ok false when the frame base could not be reconstructed, as it
 * cannot be for a caller frame: the locals held at an offset from it are then
 * reported unresolvable, while the statics among them still resolve. */
int dwarf_info_locals(const dwarf_info_t *di, uint16_t pc, uint16_t frame_base,
                      bool base_ok, dwarf_var_t *out, int max);

/* The live frame base of the function at pc, which is only the top frame's. */
bool dwarf_info_frame_base(const dwarf_info_t *di, uint16_t pc,
                           uint8_t (*readmem)(uint16_t addr), uint16_t *out);

typedef enum
{
    DW_KIND_VOID,
    DW_KIND_BASE,    /* see dwarf_type_encoding for which base type */
    DW_KIND_POINTER,
    DW_KIND_ARRAY,
    DW_KIND_STRUCT,
    DW_KIND_UNION,
    DW_KIND_ENUM,
    DW_KIND_FUNC,
    DW_KIND_UNKNOWN,
} dw_kind_t;

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
uint32_t dwarf_type_size(const dtype_t *t);    /* in bytes, 0 when unknown */
const char *dwarf_type_name(const dtype_t *t); /* built for display, such as "char [256]" */
int dwarf_type_encoding(const dtype_t *t);     /* a DW_ATE_ for DW_KIND_BASE, else 0 */

/* DW_KIND_POINTER, with NULL for a pointer to void. */
const dtype_t *dwarf_type_pointee(const dtype_t *t);

/* DW_KIND_ARRAY. */
const dtype_t *dwarf_type_element(const dtype_t *t, uint32_t *count);

/* DW_KIND_STRUCT and DW_KIND_UNION. */
int dwarf_type_member_count(const dtype_t *t);
bool dwarf_type_member(const dtype_t *t, int i, const char **name,
                       uint32_t *offset, const dtype_t **type);

/* DW_KIND_ENUM: the name of the enumerator with this value, or NULL. */
const char *dwarf_type_enum_name(const dtype_t *t, int64_t value);

#endif /* _CORE_DAP_DWARF_INFO_H_ */
