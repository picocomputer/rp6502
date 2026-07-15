/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * DWARF5 coverage for the .debug_info + .debug_line readers, against
 * tests/roms/dwtest.elf — an llvm-mos (johnwbyrd debug fork) -O0 -g build of
 * tests/roms/dwtest.c. Unlike vars.elf (DWARF 4), this exercises the v5 paths:
 * strx1 strings, DW_OP_addrx globals via .debug_addr, the v5 unit/line headers,
 * and the new MOS DWARF register numbering (frame_base = DW_OP_regx RS0 = reg
 * 0x30000). The fixture is committed; no toolchain or window needed.
 */

#include "emu/dbg/dwarf_info.h"
#include "emu/dbg/dwarf_line.h"
#include "utest.h"

#include <string.h>

#ifndef DW5_ELF
#define DW5_ELF "dwtest.elf"
#endif

/* soft stack pointer RS0 = rc0:rc1 at $00:$01; report $9000 so DW_OP_fbreg
 * locals resolve to frame-relative addresses we can assert on. */
static uint8_t fake_mem(uint16_t a)
{
    if (a == 0) return 0x00;
    if (a == 1) return 0x90;
    return 0;
}

static const dwarf_var_t *find(const dwarf_var_t *v, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(v[i].name, name) == 0)
            return &v[i];
    return NULL;
}

UTEST(dwarf5, info_loads)
{
    dwarf_info_t *di = dwarf_info_load(DW5_ELF);
    ASSERT_TRUE(di != NULL);
    dwarf_info_free(di);
}

/* Globals use DW_OP_addrx into .debug_addr; verify the exact resolved 6502
 * addresses (matching the linker's placement) and the strx-named types. */
UTEST(dwarf5, globals_addrx)
{
    dwarf_info_t *di = dwarf_info_load(DW5_ELF);
    ASSERT_TRUE(di != NULL);
    dwarf_var_t g[64];
    int n = dwarf_info_globals(di, g, 64);

    const dwarf_var_t *gi8 = find(g, n, "g_i8");
    ASSERT_TRUE(gi8 != NULL);
    ASSERT_TRUE(gi8->addr_ok);
    ASSERT_EQ((int)gi8->addr, 0x08dc);
    ASSERT_EQ(dwarf_type_size(gi8->type), 1u);
    ASSERT_EQ(dwarf_type_encoding(gi8->type), DW_ATE_signed_char);

    const dwarf_var_t *gi16 = find(g, n, "g_i16");
    ASSERT_TRUE(gi16 != NULL);
    ASSERT_TRUE(gi16->addr_ok);
    ASSERT_EQ(dwarf_type_size(gi16->type), 2u);
    ASSERT_EQ(dwarf_type_encoding(gi16->type), DW_ATE_signed);

    const dwarf_var_t *gu16 = find(g, n, "g_u16");
    ASSERT_TRUE(gu16 != NULL && gu16->type != NULL);
    ASSERT_EQ(dwarf_type_encoding(gu16->type), DW_ATE_unsigned);

    const dwarf_var_t *grect = find(g, n, "g_rect");
    ASSERT_TRUE(grect != NULL);
    ASSERT_TRUE(grect->addr_ok);
    ASSERT_EQ((int)grect->addr, 0x08ea);
    dwarf_info_free(di);
}

UTEST(dwarf5, array_and_struct)
{
    dwarf_info_t *di = dwarf_info_load(DW5_ELF);
    ASSERT_TRUE(di != NULL);
    dwarf_var_t g[64];
    int n = dwarf_info_globals(di, g, 64);

    const dwarf_var_t *msg = find(g, n, "g_msg");
    ASSERT_TRUE(msg != NULL);
    ASSERT_EQ((int)dwarf_type_kind(msg->type), (int)DW_KIND_ARRAY);
    uint32_t count = 0;
    const dtype_t *elem = dwarf_type_element(msg->type, &count);
    ASSERT_EQ(count, 8u);
    ASSERT_EQ(dwarf_type_size(elem), 1u);

    const dwarf_var_t *rect = find(g, n, "g_rect");
    ASSERT_TRUE(rect != NULL);
    ASSERT_EQ((int)dwarf_type_kind(rect->type), (int)DW_KIND_STRUCT);
    ASSERT_EQ(dwarf_type_size(rect->type), 9u);
    ASSERT_EQ(dwarf_type_member_count(rect->type), 4);
    const char *nm;
    uint32_t off;
    const dtype_t *mt;
    ASSERT_TRUE(dwarf_type_member(rect->type, 0, &nm, &off, &mt));
    ASSERT_TRUE(strcmp(nm, "origin") == 0 && off == 0);
    ASSERT_TRUE(dwarf_type_member(rect->type, 3, &nm, &off, &mt));
    ASSERT_TRUE(strcmp(nm, "tag") == 0 && off == 8);
    ASSERT_EQ(dwarf_type_size(mt), 1u); /* char tag */
    dwarf_info_free(di);
}

/* frame_base = DW_OP_regx RS0 (MOS DWARF reg 0x30000) -> rc0:rc1 soft SP; the
 * fbreg locals of `area` resolve to frame_base + offset. */
UTEST(dwarf5, frame_base_and_locals)
{
    dwarf_info_t *di = dwarf_info_load(DW5_ELF);
    ASSERT_TRUE(di != NULL);
    uint16_t fb = 0;
    ASSERT_TRUE(dwarf_info_frame_base(di, 0x0640, fake_mem, &fb)); /* inside area */
    ASSERT_EQ((int)fb, 0x9000);

    dwarf_var_t v[64];
    int n = dwarf_info_locals(di, 0x0640, fb, true, v, 64);
    const dwarf_var_t *s = find(v, n, "s");
    ASSERT_TRUE(s != NULL && s->addr_ok);
    ASSERT_EQ((int)s->addr, 0x9000); /* DW_OP_fbreg +0 */
    const dwarf_var_t *a = find(v, n, "a");
    ASSERT_TRUE(a != NULL && a->addr_ok);
    ASSERT_EQ((int)a->addr, 0x900a); /* DW_OP_fbreg +10 */

    /* base unavailable (a caller whose frame base couldn't be recovered) */
    n = dwarf_info_locals(di, 0x0640, fb, false, v, 64);
    ASSERT_FALSE(find(v, n, "s")->addr_ok);
    dwarf_info_free(di);
}

/* v5 line program: address<->source, function names, breakpoint binding. */
UTEST(dwarf5, line_mapping)
{
    dwarf_line_t *dl = dwarf_line_load(DW5_ELF);
    ASSERT_TRUE(dl != NULL);

    const char *file = NULL;
    int line = 0;
    ASSERT_TRUE(dwarf_line_addr_to_src(dl, 0x02ae, &file, &line));
    ASSERT_TRUE(strstr(file, "dwtest.c") != NULL);
    ASSERT_EQ(line, 66); /* main's opening brace */

    const char *fn = dwarf_line_addr_to_func(dl, 0x061c);
    ASSERT_TRUE(fn != NULL && strcmp(fn, "area") == 0);

    uint16_t addr = 0;
    ASSERT_TRUE(dwarf_line_func_addr(dl, "main", &addr));
    ASSERT_EQ((int)addr, 0x02ae);

    int bl = 0;
    ASSERT_TRUE(dwarf_line_src_to_addr(dl, "dwtest.c", 39, &addr, &bl));
    ASSERT_TRUE(addr >= 0x061c && addr < 0x0754); /* binds inside area */
    dwarf_line_free(dl);
}

UTEST_MAIN()
