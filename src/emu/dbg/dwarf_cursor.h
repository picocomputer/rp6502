/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Byte cursor + DWARF LEB128 primitives shared by the .debug_info and
 * .debug_line readers. A short read sets ok=false and yields 0/""; readers
 * check ok and abort the current unit.
 */

#ifndef _EMU_DBG_DWARF_CURSOR_H_
#define _EMU_DBG_DWARF_CURSOR_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    const uint8_t *p, *end;
    bool ok;
} dwarf_cur;

uint8_t dwarf_u8(dwarf_cur *c);
uint16_t dwarf_u16(dwarf_cur *c);
uint32_t dwarf_u32(dwarf_cur *c);
uint64_t dwarf_u64(dwarf_cur *c);
uint64_t dwarf_uleb(dwarf_cur *c);
int64_t dwarf_sleb(dwarf_cur *c);
const char *dwarf_cstr(dwarf_cur *c);

#endif /* _EMU_DBG_DWARF_CURSOR_H_ */
