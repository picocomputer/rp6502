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

#ifndef _EMU_DBG_CC65DBG_H_
#define _EMU_DBG_CC65DBG_H_

#include <stdbool.h>
#include <stdint.h>

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

/* A function's entry address by name (the '_'-stripped label). False if absent. */
bool cc65dbg_func_addr(const cc65dbg_t *db, const char *name, uint16_t *addr);

/* ---- best-effort variable inspection ----
 * cc65's .dbg carries no C type info (every csym has type=0), so `size` is the
 * scalar byte width inferred from memory layout (the gap to the next symbol);
 * 0 means unknown (aggregate, or an unbounded leftmost parameter) — read a
 * 16-bit word there. Auto locals are addressed relative to the C stack pointer
 * c_sp, which is only at the frame base at statement boundaries — reliable at a
 * breakpoint stop, not mid-expression. */
typedef struct
{
    const char *name; /* stable pointer owned by db */
    uint16_t addr;
    bool addr_ok;
    uint8_t size; /* scalar byte width 1/2/4; 0 = unknown -> 16-bit word */
} cc65var_t;

/* The auto locals/parameters in scope at pc (csym sc=auto whose lexical scope
 * covers pc), addressed at frame_base + offs. base_ok false marks each as
 * unresolvable (a caller frame whose base couldn't be reconstructed). Returns
 * count. Register-passed parameters (no stack address) are omitted. */
int cc65dbg_locals(const cc65dbg_t *db, uint16_t pc, uint16_t frame_base,
                   bool base_ok, cc65var_t *out, int max);

/* The entry-sp frame base (= live c_sp + frame_size) at pc, for the top frame.
 * False if the .dbg carries no c_sp. */
bool cc65dbg_frame_base(const cc65dbg_t *db, uint16_t pc,
                        uint8_t (*readmem)(uint16_t addr), uint16_t *out);

/* The frame size at pc (bytes the prologue lowers c_sp for locals). */
int32_t cc65dbg_frame_size(const cc65dbg_t *db, uint16_t pc);

/* The byte size of the argument region above the function's frame base, for
 * chaining a caller's frame base. False when it can't be determined exactly
 * (any parameter -> fail-closed), so the caller frame is left unresolvable. */
bool cc65dbg_arg_size(const cc65dbg_t *db, uint16_t pc, uint16_t *out);

/* The C globals (the "_name" data-segment labels), at their fixed addresses.
 * Returns count. */
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

#endif /* _EMU_DBG_CC65DBG_H_ */
