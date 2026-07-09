/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See dwarf_cursor.h.
 */

#include "emu/dbg/dwarf_cursor.h"

uint8_t dwarf_u8(dwarf_cur *c)
{
    if (c->p >= c->end) { c->ok = false; return 0; }
    return *c->p++;
}
uint16_t dwarf_u16(dwarf_cur *c)
{
    uint16_t a = dwarf_u8(c), b = dwarf_u8(c);
    return (uint16_t)(a | (b << 8));
}
uint32_t dwarf_u32(dwarf_cur *c)
{
    uint32_t a = dwarf_u16(c), b = dwarf_u16(c);
    return a | (b << 16);
}
uint64_t dwarf_u64(dwarf_cur *c)
{
    uint64_t a = dwarf_u32(c), b = dwarf_u32(c);
    return a | (b << 32);
}
uint64_t dwarf_uleb(dwarf_cur *c)
{
    uint64_t v = 0;
    int shift = 0;
    for (;;)
    {
        uint8_t b = dwarf_u8(c);
        if (!c->ok) break;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 63) { c->ok = false; break; }
    }
    return v;
}
int64_t dwarf_sleb(dwarf_cur *c)
{
    int64_t v = 0;
    int shift = 0;
    uint8_t b = 0;
    do
    {
        b = dwarf_u8(c);
        if (!c->ok) break;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (shift > 63) break; /* malformed: guard the next <<shift (UB at >=64) */
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        v |= -((int64_t)1 << shift);
    return v;
}
const char *dwarf_cstr(dwarf_cur *c)
{
    const char *s = (const char *)c->p;
    while (c->p < c->end && *c->p)
        c->p++;
    if (c->p >= c->end) { c->ok = false; return ""; }
    c->p++; /* skip NUL */
    return s;
}
