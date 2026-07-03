/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Minimal reader for cc65's ".dbg" debug-info format (ld65 --dbgfile). cc65 does
 * not emit DWARF, so this is the cc65 analogue of dwarf_line.c: it answers the
 * same address<->source-line + address->function queries for the DAP adapter.
 * The .dbg is a flat text file of "type<TAB>key=val,key=val,..." records; a C
 * source line's address is its segment's load address + its span offset, so the
 * addresses are the 6502 load addresses (the emulator's PC directly).
 */

#ifndef _EMU_CC65DBG_H_
#define _EMU_CC65DBG_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct cc65dbg cc65dbg_t;

/* Load + parse a cc65 .dbg file. NULL if it can't be read or has no usable C
 * line info. */
cc65dbg_t *cc65dbg_load(const char *dbg_path);
void cc65dbg_free(cc65dbg_t *db);

/* addr -> C source location: a stable full-path pointer (owned by db) + 1-based
 * line. False if no C-source span covers addr. */
bool cc65dbg_addr_to_src(const cc65dbg_t *db, uint16_t addr,
                         const char **file, int *line);

/* (source file [matched by basename], 1-based line) -> the lowest code address
 * at that line, or the next C line at/after it. *bound_line returns the line
 * actually bound. */
bool cc65dbg_src_to_addr(const cc65dbg_t *db, const char *file, int line,
                         uint16_t *addr, int *bound_line);

/* The function (cc65 "_name" label) enclosing addr, with the leading '_'
 * stripped, or NULL. Stable pointer owned by db. */
const char *cc65dbg_addr_to_func(const cc65dbg_t *db, uint16_t addr);

/* ---- best-effort, UNTYPED variable inspection ----
 * cc65's .dbg carries no usable C type info (every csym has type=0), so unlike
 * the DWARF path these report only a name + 6502 address; the caller reads a
 * 16-bit word there. Auto locals are addressed relative to the C stack pointer
 * c_sp, which is only at the frame base at statement boundaries — reliable at a
 * breakpoint stop, not mid-expression. */
typedef struct
{
    const char *name; /* stable pointer owned by db */
    uint16_t addr;
    bool addr_ok;
} cc65var_t;

/* The auto locals/parameters in scope at pc (csym sc=auto whose lexical scope
 * covers pc), addressed relative to the live c_sp (read via readmem). Returns
 * count. */
int cc65dbg_locals(const cc65dbg_t *db, uint16_t pc,
                   uint8_t (*readmem)(uint16_t addr), cc65var_t *out, int max);

/* The global (sc=ext/static, non-function) C variables, at their fixed
 * addresses. Returns count. */
int cc65dbg_globals(const cc65dbg_t *db, cc65var_t *out, int max);

/* ---- linker segments (the .dbg "seg" records) ----
 * Each named segment's load address + size, for the memory-map view. */
typedef struct
{
    const char *name; /* stable pointer owned by db */
    uint16_t addr;
    uint32_t size;
} cc65seg_t;

/* The segments that have a name and a non-zero size. Returns count (<= max). */
int cc65dbg_segments(const cc65dbg_t *db, cc65seg_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_CC65DBG_H_ */
