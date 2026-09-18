/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/dap/dwarf_info.h"
#include "core/dap/dwarf_line.h"
#include "utest.h"

#include <string.h>

#ifndef TEST_FIXTURE
#define TEST_FIXTURE "dwtest.elf"
#endif

/* RS0, the soft stack pointer, is the zero-page pair rc0:rc1 at $00:$01, so
 * fake_mem makes RS0 read as $9000. */
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
    dwarf_info_t *di = dwarf_info_load(TEST_FIXTURE);
    ASSERT_TRUE(di != NULL);
    dwarf_info_free(di);
}

UTEST(dwarf5, globals_addrx)
{
    dwarf_info_t *di = dwarf_info_load(TEST_FIXTURE);
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
    dwarf_info_t *di = dwarf_info_load(TEST_FIXTURE);
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
    ASSERT_EQ(dwarf_type_size(mt), 1u);
    dwarf_info_free(di);
}

UTEST(dwarf5, enum_values)
{
    dwarf_info_t *di = dwarf_info_load(TEST_FIXTURE);
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

UTEST(dwarf5, pointer_type)
{
    dwarf_info_t *di = dwarf_info_load(TEST_FIXTURE);
    ASSERT_TRUE(di != NULL);
    dwarf_var_t g[64];
    int n = dwarf_info_globals(di, g, 64);

    const dwarf_var_t *ptr = find(g, n, "g_ptr");
    ASSERT_TRUE(ptr != NULL);
    ASSERT_EQ((int)dwarf_type_kind(ptr->type), (int)DW_KIND_POINTER);
    const dtype_t *pointee = dwarf_type_pointee(ptr->type);
    ASSERT_TRUE(pointee != NULL);
    ASSERT_EQ(dwarf_type_size(pointee), 1u);

    const dwarf_var_t *rectp = find(g, n, "g_rectp");
    ASSERT_TRUE(rectp != NULL);
    ASSERT_EQ((int)dwarf_type_kind(rectp->type), (int)DW_KIND_POINTER);
    ASSERT_EQ((int)dwarf_type_kind(dwarf_type_pointee(rectp->type)), (int)DW_KIND_STRUCT);
    dwarf_info_free(di);
}

UTEST(dwarf5, frame_base_and_locals)
{
    dwarf_info_t *di = dwarf_info_load(TEST_FIXTURE);
    ASSERT_TRUE(di != NULL);
    uint16_t fb = 0;
    ASSERT_TRUE(dwarf_info_frame_base(di, 0x0660, fake_mem, &fb)); /* 0x0660 is in area */
    ASSERT_EQ((int)fb, 0x9000);

    dwarf_var_t v[64];
    int n = dwarf_info_locals(di, 0x0660, fb, true, v, 64);
    const dwarf_var_t *s = find(v, n, "s");
    ASSERT_TRUE(s != NULL && s->addr_ok);
    ASSERT_EQ((int)s->addr, 0x9000); /* s is at DW_OP_fbreg +0 */
    const dwarf_var_t *a = find(v, n, "a");
    ASSERT_TRUE(a != NULL && a->addr_ok);
    ASSERT_EQ((int)a->addr, 0x900a); /* a is at DW_OP_fbreg +10 */

    n = dwarf_info_locals(di, 0x0660, fb, false, v, 64);
    const dwarf_var_t *su = find(v, n, "s");
    ASSERT_TRUE(su != NULL);
    ASSERT_FALSE(su->addr_ok);
    dwarf_info_free(di);
}

UTEST(dwarf5, line_mapping)
{
    dwarf_line_t *dl = dwarf_line_load(TEST_FIXTURE);
    ASSERT_TRUE(dl != NULL);

    const char *file = NULL;
    int line = 0;
    ASSERT_TRUE(dwarf_line_addr_to_src(dl, 0x02ae, &file, &line));
    ASSERT_TRUE(strstr(file, "dwtest.c") != NULL);
    ASSERT_EQ(line, 81); /* dwtest.c line 81 is main's opening brace */

    const char *fn = dwarf_line_addr_to_func(dl, 0x0660);
    ASSERT_TRUE(fn != NULL && strcmp(fn, "area") == 0);

    uint16_t addr = 0;
    ASSERT_TRUE(dwarf_line_func_addr(dl, "main", &addr));
    ASSERT_EQ((int)addr, 0x02ae);

    int bl = 0;
    ASSERT_TRUE(dwarf_line_src_to_addr(dl, "dwtest.c", 55, &addr, &bl));
    ASSERT_TRUE(addr >= 0x0635 && addr < 0x076d); /* area spans these addresses */
    dwarf_line_free(dl);
}

UTEST_MAIN()
