/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * DWARF .debug_info reader (dwarf_info.c): the variable/type model that backs the
 * DAP Variables view. Exercised against tests/roms/vars.elf, an llvm-mos -O0
 * -gdwarf-4 build of tests/roms/vars.c covering base/array/struct/enum/pointer
 * types plus globals and function locals. No toolchain or window needed — the
 * fixture is committed.
 */

#include "emu/dbg/dwarf_info.h"
#include "utest.h"

#include <string.h>

#ifndef VARS_ELF
#define VARS_ELF "vars.elf"
#endif

/* A synthetic frame base: the function frame base is DW_OP_regx RS0 = the soft
 * stack pointer pair rc0:rc1 at zero page $00; report it as $0500 so DW_OP_fbreg
 * locals resolve to frame-relative addresses we can assert on. */
static uint8_t fake_mem(uint16_t a)
{
    if (a == 0) return 0x00;
    if (a == 1) return 0x05;
    return 0;
}

static const dwarf_var_t *find(const dwarf_var_t *v, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(v[i].name, name) == 0)
            return &v[i];
    return NULL;
}

UTEST(dwarf_info, loads)
{
    dwarf_info_t *di = dwarf_info_load(VARS_ELF);
    ASSERT_TRUE(di != NULL);
    dwarf_info_free(di);
}

UTEST(dwarf_info, base_types)
{
    dwarf_info_t *di = dwarf_info_load(VARS_ELF);
    ASSERT_TRUE(di != NULL);
    dwarf_var_t g[64];
    int n = dwarf_info_globals(di, g, 64);

    const dwarf_var_t *gint = find(g, n, "gint");
    ASSERT_TRUE(gint != NULL);
    ASSERT_EQ((int)dwarf_type_kind(gint->type), (int)DW_KIND_BASE);
    ASSERT_EQ(dwarf_type_size(gint->type), 2u);
    ASSERT_EQ(dwarf_type_encoding(gint->type), DW_ATE_signed);

    const dwarf_var_t *guint = find(g, n, "guint");
    ASSERT_TRUE(guint != NULL);
    ASSERT_EQ(dwarf_type_encoding(guint->type), DW_ATE_unsigned);

    const dwarf_var_t *glong = find(g, n, "glong");
    ASSERT_TRUE(glong != NULL);
    ASSERT_EQ(dwarf_type_size(glong->type), 4u);

    const dwarf_var_t *gchar = find(g, n, "gchar");
    ASSERT_TRUE(gchar != NULL);
    ASSERT_EQ(dwarf_type_size(gchar->type), 1u);
    ASSERT_EQ(dwarf_type_encoding(gchar->type), DW_ATE_unsigned_char);

    dwarf_info_free(di);
}

UTEST(dwarf_info, array_type)
{
    dwarf_info_t *di = dwarf_info_load(VARS_ELF);
    dwarf_var_t g[64];
    int n = dwarf_info_globals(di, g, 64);
    const dwarf_var_t *garr = find(g, n, "garr");
    ASSERT_TRUE(garr != NULL);
    ASSERT_EQ((int)dwarf_type_kind(garr->type), (int)DW_KIND_ARRAY);
    uint32_t count = 0;
    const dtype_t *elem = dwarf_type_element(garr->type, &count);
    ASSERT_EQ(count, 4u);
    ASSERT_EQ((int)dwarf_type_kind(elem), (int)DW_KIND_BASE);
    ASSERT_EQ(dwarf_type_size(elem), 2u); /* int[4] */
    ASSERT_EQ(dwarf_type_size(garr->type), 8u);
    dwarf_info_free(di);
}

UTEST(dwarf_info, struct_members)
{
    dwarf_info_t *di = dwarf_info_load(VARS_ELF);
    dwarf_var_t g[64];
    int n = dwarf_info_globals(di, g, 64);
    const dwarf_var_t *gpt = find(g, n, "gpt");
    ASSERT_TRUE(gpt != NULL);
    ASSERT_EQ((int)dwarf_type_kind(gpt->type), (int)DW_KIND_STRUCT);
    ASSERT_EQ(dwarf_type_member_count(gpt->type), 3);

    const char *nm;
    uint32_t off;
    const dtype_t *mt;
    ASSERT_TRUE(dwarf_type_member(gpt->type, 0, &nm, &off, &mt));
    ASSERT_TRUE(strcmp(nm, "x") == 0 && off == 0);
    ASSERT_TRUE(dwarf_type_member(gpt->type, 1, &nm, &off, &mt));
    ASSERT_TRUE(strcmp(nm, "y") == 0 && off == 2);
    ASSERT_TRUE(dwarf_type_member(gpt->type, 2, &nm, &off, &mt));
    ASSERT_TRUE(strcmp(nm, "tag") == 0 && off == 4);
    ASSERT_EQ(dwarf_type_size(mt), 1u); /* char tag */
    dwarf_info_free(di);
}

UTEST(dwarf_info, enum_values)
{
    dwarf_info_t *di = dwarf_info_load(VARS_ELF);
    dwarf_var_t g[64];
    int n = dwarf_info_globals(di, g, 64);
    const dwarf_var_t *gcol = find(g, n, "gcol");
    ASSERT_TRUE(gcol != NULL);
    ASSERT_EQ((int)dwarf_type_kind(gcol->type), (int)DW_KIND_ENUM);
    const char *e0 = dwarf_type_enum_name(gcol->type, 0);
    const char *e1 = dwarf_type_enum_name(gcol->type, 1);
    const char *e7 = dwarf_type_enum_name(gcol->type, 7);
    ASSERT_TRUE(e0 && strcmp(e0, "RED") == 0);
    ASSERT_TRUE(e1 && strcmp(e1, "GREEN") == 0);
    ASSERT_TRUE(e7 && strcmp(e7, "BLUE") == 0);
    ASSERT_TRUE(dwarf_type_enum_name(gcol->type, 3) == NULL);
    dwarf_info_free(di);
}

UTEST(dwarf_info, pointer_type)
{
    dwarf_info_t *di = dwarf_info_load(VARS_ELF);
    dwarf_var_t g[64];
    int n = dwarf_info_globals(di, g, 64);
    const dwarf_var_t *gptr = find(g, n, "gptr");
    ASSERT_TRUE(gptr != NULL);
    ASSERT_EQ((int)dwarf_type_kind(gptr->type), (int)DW_KIND_POINTER);
    const dtype_t *pointee = dwarf_type_pointee(gptr->type);
    ASSERT_TRUE(pointee != NULL);
    ASSERT_EQ(dwarf_type_size(pointee), 1u); /* char * */

    const dwarf_var_t *gpp = find(g, n, "gpp");
    ASSERT_TRUE(gpp != NULL);
    ASSERT_EQ((int)dwarf_type_kind(dwarf_type_pointee(gpp->type)), (int)DW_KIND_STRUCT);
    dwarf_info_free(di);
}

/* A used global keeps a real address (DW_OP_addr); an unused one is GC'd by the
 * linker and reports addr_ok == false (its type is still known). */
UTEST(dwarf_info, global_addresses)
{
    dwarf_info_t *di = dwarf_info_load(VARS_ELF);
    dwarf_var_t g[64];
    int n = dwarf_info_globals(di, g, 64);
    const dwarf_var_t *gstr = find(g, n, "gstr");
    ASSERT_TRUE(gstr != NULL);
    ASSERT_TRUE(gstr->addr_ok); /* referenced by gptr/lpc -> kept */
    const dwarf_var_t *sink = find(g, n, "sink");
    ASSERT_TRUE(sink != NULL);
    ASSERT_TRUE(sink->addr_ok);
    dwarf_info_free(di);
}

/* Locals in main resolve via the frame base (soft stack pointer rc0:rc1). */
UTEST(dwarf_info, locals_in_scope)
{
    dwarf_info_t *di = dwarf_info_load(VARS_ELF);
    dwarf_var_t v[64];
    int n = dwarf_info_locals(di, 0x300, fake_mem, v, 64); /* inside main [0x2c4,0x3ef) */
    ASSERT_TRUE(n >= 3);

    const dwarf_var_t *li = find(v, n, "local_i");
    ASSERT_TRUE(li != NULL);
    ASSERT_EQ((int)dwarf_type_kind(li->type), (int)DW_KIND_BASE);
    ASSERT_TRUE(li->addr_ok);
    ASSERT_TRUE(li->addr >= 0x0500); /* frame base $0500 + positive offset */

    const dwarf_var_t *lp = find(v, n, "lp");
    ASSERT_TRUE(lp != NULL);
    ASSERT_EQ((int)dwarf_type_kind(lp->type), (int)DW_KIND_STRUCT);

    const dwarf_var_t *lpc = find(v, n, "lpc");
    ASSERT_TRUE(lpc != NULL);
    ASSERT_EQ((int)dwarf_type_kind(lpc->type), (int)DW_KIND_POINTER);

    /* outside any function -> no locals */
    ASSERT_EQ(dwarf_info_locals(di, 0x0010, fake_mem, v, 64), 0);
    dwarf_info_free(di);
}

UTEST_MAIN()
