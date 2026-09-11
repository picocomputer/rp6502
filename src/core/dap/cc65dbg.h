/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A reader for the .dbg file that ld65 --dbgfile writes. cc65 emits no DWARF, so
 * this answers for a cc65 program what dwarf_line.c answers for an llvm-mos one.
 * The address of a C source line is the load address of its segment plus its
 * span offset, so the addresses here are 6502 addresses and compare directly
 * against the PC.
 */

#ifndef _CORE_DAP_CC65DBG_H_
#define _CORE_DAP_CC65DBG_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct cc65dbg cc65dbg_t;

cc65dbg_t *cc65dbg_load(const char *dbg_path);
void cc65dbg_free(cc65dbg_t *db);

/* The file is the path the .dbg gives, owned by db, and the line is 1-based. */
bool cc65dbg_addr_to_src(const cc65dbg_t *db, uint16_t addr,
                         const char **file, int *line);

/* A line with no code binds forward to the next C line that has some, so
 * bound_line reports the line actually bound. A whole-component path suffix
 * match wins over a basename-only one, which is what tells a/util.c from
 * b/util.c. */
bool cc65dbg_src_to_addr(const cc65dbg_t *db, const char *file, int line,
                         uint16_t *addr, int *bound_line);

/* The nearest function label at or below addr, with cc65's leading underscore
 * stripped and owned by db. No function length is kept, so an address past the
 * end of a function still names it. */
const char *cc65dbg_addr_to_func(const cc65dbg_t *db, uint16_t addr);

/* The name is the label with its underscore stripped, as reported above. */
bool cc65dbg_func_addr(const cc65dbg_t *db, const char *name, uint16_t *addr);

/* Variable inspection is best effort, because a .dbg carries no C types: every
 * csym has type=0. A symbol whose record carries no size gets one inferred
 * from the gap to the next symbol in memory. An auto local is addressed from
 * the frame base, which is the live C stack pointer c_sp plus the frame size.
 * That holds only at a statement boundary, because mid-expression cc65 has
 * pushed intermediates and c_sp sits lower still, so these addresses are sound
 * at a breakpoint stop and not in the middle of an expression. */
typedef struct
{
    const char *name; /* owned by db */
    uint16_t addr;
    bool addr_ok;
    uint8_t size; /* 1, 2 or 4 bytes; 0 is unknown, to be read as a 16-bit word */
} cc65var_t;

/* The autos whose lexical scope covers pc, addressed at frame_base + offs. Pass
 * base_ok false when the frame base could not be reconstructed, as it cannot be
 * for a caller frame, and each is reported unresolvable. A parameter passed in a
 * register has no stack address and is left out. */
int cc65dbg_locals(const cc65dbg_t *db, uint16_t pc, uint16_t frame_base,
                   bool base_ok, cc65var_t *out, int max);

/* The frame base at pc, which is the live c_sp plus the frame size, for the top
 * frame only. False when the .dbg records no c_sp. */
bool cc65dbg_frame_base(const cc65dbg_t *db, uint16_t pc,
                        uint8_t (*readmem)(uint16_t addr), uint16_t *out);

/* The bytes the prologue lowers c_sp by to make room for locals. */
int32_t cc65dbg_frame_size(const cc65dbg_t *db, uint16_t pc);

/* The argument region above the function's frame base, which chaining to the
 * caller's frame base needs. It cannot be sized exactly when the function takes
 * any parameter, and the false this returns then leaves the caller's frame
 * unresolved. */
bool cc65dbg_arg_size(const cc65dbg_t *db, uint16_t pc, uint16_t *out);

int cc65dbg_globals(const cc65dbg_t *db, cc65var_t *out, int max);

/* A linker segment, from the seg records, for the memory map view. */
typedef struct
{
    const char *name; /* owned by db */
    uint16_t addr;
    uint32_t size;
} cc65seg_t;

/* Only the segments that have a name and a non-zero size. */
int cc65dbg_segments(const cc65dbg_t *db, cc65seg_t *out, int max);

#endif /* _CORE_DAP_CC65DBG_H_ */
