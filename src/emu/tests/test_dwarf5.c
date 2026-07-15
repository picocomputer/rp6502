/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * DWARF5 coverage for the .debug_info + .debug_line readers, against
 * tests/roms/dwtest.elf — an llvm-mos (johnwbyrd debug fork) -O0 -g build of
 * tests/roms/dwtest.c. This exercises the v5 paths the readers must handle:
 * strx1 strings, DW_OP_addrx globals via .debug_addr, the v5 unit/line headers,
 * enum/pointer types, and the MOS DWARF register numbering (frame_base =
 * DW_OP_regx RS0 = reg 0x30000). The fixture is committed; no toolchain needed.
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

/* Synthetic soft-SP-lowering prologue idiom at area's entry ($0635): the fork
 * emits a register save before this idiom, so the real bytes don't sit at lo;
 * feed the canonical clc/lda $00/adc #lo/sta $00/lda $01/adc #hi/sta $01 that
 * dwarf_info_frame_size matches, here lowering rc0:rc1 by 11. */
static uint8_t fake_prologue_mem(uint16_t a)
{
    static const uint8_t pro[13] = {0x18, 0xA5, 0x00, 0x69, 0xF5, 0x85, 0x00,
                                    0xA5, 0x01, 0x69, 0xFF, 0x85, 0x01};
    if (a >= 0x0635 && a < 0x0635 + 13)
        return pro[a - 0x0635];
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
    ASSERT_EQ((int)gi8->addr, 0x08f5);
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
    ASSERT_EQ((int)grect->addr, 0x0903);
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

/* Enum type: enumerator names resolve by value; an unlisted value is NULL. */
UTEST(dwarf5, enum_values)
{
    dwarf_info_t *di = dwarf_info_load(DW5_ELF);
    ASSERT_TRUE(di != NULL);
    dwarf_var_t g[64];
    int n = dwarf_info_globals(di, g, 64);

    const dwarf_var_t *col = find(g, n, "g_color");
    ASSERT_TRUE(col != NULL);
    ASSERT_EQ((int)dwarf_type_kind(col->type), (int)DW_KIND_ENUM);
    const char *e0 = dwarf_type_enum_name(col->type, 0);
    const char *e1 = dwarf_type_enum_name(col->type, 1);
    const char *e7 = dwarf_type_enum_name(col->type, 7);
    ASSERT_TRUE(e0 && strcmp(e0, "RED") == 0);
    ASSERT_TRUE(e1 && strcmp(e1, "GREEN") == 0);
    ASSERT_TRUE(e7 && strcmp(e7, "BLUE") == 0);
    ASSERT_TRUE(dwarf_type_enum_name(col->type, 3) == NULL);
    dwarf_info_free(di);
}

/* Pointer types: char * pointee is a 1-byte base; Rect * pointee is a struct. */
UTEST(dwarf5, pointer_type)
{
    dwarf_info_t *di = dwarf_info_load(DW5_ELF);
    ASSERT_TRUE(di != NULL);
    dwarf_var_t g[64];
    int n = dwarf_info_globals(di, g, 64);

    const dwarf_var_t *ptr = find(g, n, "g_ptr");
    ASSERT_TRUE(ptr != NULL);
    ASSERT_EQ((int)dwarf_type_kind(ptr->type), (int)DW_KIND_POINTER);
    const dtype_t *pointee = dwarf_type_pointee(ptr->type);
    ASSERT_TRUE(pointee != NULL);
    ASSERT_EQ(dwarf_type_size(pointee), 1u); /* char * */

    const dwarf_var_t *rectp = find(g, n, "g_rectp");
    ASSERT_TRUE(rectp != NULL);
    ASSERT_EQ((int)dwarf_type_kind(rectp->type), (int)DW_KIND_POINTER);
    ASSERT_EQ((int)dwarf_type_kind(dwarf_type_pointee(rectp->type)), (int)DW_KIND_STRUCT);
    dwarf_info_free(di);
}

/* frame_base = DW_OP_regx RS0 (MOS DWARF reg 0x30000) -> rc0:rc1 soft SP; the
 * fbreg locals of `area` resolve to frame_base + offset. */
UTEST(dwarf5, frame_base_and_locals)
{
    dwarf_info_t *di = dwarf_info_load(DW5_ELF);
    ASSERT_TRUE(di != NULL);
    uint16_t fb = 0;
    ASSERT_TRUE(dwarf_info_frame_base(di, 0x0660, fake_mem, &fb)); /* inside area */
    ASSERT_EQ((int)fb, 0x9000);

    dwarf_var_t v[64];
    int n = dwarf_info_locals(di, 0x0660, fb, true, v, 64);
    const dwarf_var_t *s = find(v, n, "s");
    ASSERT_TRUE(s != NULL && s->addr_ok);
    ASSERT_EQ((int)s->addr, 0x9000); /* DW_OP_fbreg +0 */
    const dwarf_var_t *a = find(v, n, "a");
    ASSERT_TRUE(a != NULL && a->addr_ok);
    ASSERT_EQ((int)a->addr, 0x900a); /* DW_OP_fbreg +10 */

    /* base unavailable (a caller whose frame base couldn't be recovered) */
    n = dwarf_info_locals(di, 0x0660, fb, false, v, 64);
    ASSERT_FALSE(find(v, n, "s")->addr_ok);
    dwarf_info_free(di);
}

/* The soft-stack frame size comes from the prologue idiom, not the DWARF local
 * extents. dwarf_info_frame_size reads the idiom at the function's entry. */
UTEST(dwarf5, frame_size_from_prologue)
{
    dwarf_info_t *di = dwarf_info_load(DW5_ELF);
    ASSERT_TRUE(di != NULL);
    uint16_t alloc = 0xffff;
    ASSERT_TRUE(dwarf_info_frame_size(di, 0x0660, fake_prologue_mem, &alloc)); /* in area */
    ASSERT_EQ((int)alloc, 11);
    /* pc outside any known function -> can't size (fail-closed) */
    ASSERT_FALSE(dwarf_info_frame_size(di, 0x0010, fake_prologue_mem, &alloc));
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
    ASSERT_EQ(line, 81); /* main's opening brace */

    const char *fn = dwarf_line_addr_to_func(dl, 0x0660);
    ASSERT_TRUE(fn != NULL && strcmp(fn, "area") == 0);

    uint16_t addr = 0;
    ASSERT_TRUE(dwarf_line_func_addr(dl, "main", &addr));
    ASSERT_EQ((int)addr, 0x02ae);

    int bl = 0;
    ASSERT_TRUE(dwarf_line_src_to_addr(dl, "dwtest.c", 55, &addr, &bl));
    ASSERT_TRUE(addr >= 0x0635 && addr < 0x076d); /* binds inside area */
    dwarf_line_free(dl);
}

UTEST_MAIN()
