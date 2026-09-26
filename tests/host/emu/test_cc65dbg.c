/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/dap/cc65dbg.h"
#include "utest.h"

#include <string.h>

#ifndef TEST_FIXTURE
#define TEST_FIXTURE "cc65.dbg"
#endif
#ifndef TEST_ONCE_FIXTURE
#define TEST_ONCE_FIXTURE "cc65_once.dbg"
#endif

/* In the fixture, c_sp, the C stack pointer, is the zero-page pair $00:$01,
 * so fake_mem makes c_sp read as $0500. */
static uint8_t fake_mem(uint16_t a)
{
    if (a == 0) return 0x00;
    if (a == 1) return 0x05;
    return 0;
}

UTEST(cc65dbg, loads)
{
    cc65dbg_t *db = cc65dbg_load(TEST_FIXTURE);
    ASSERT_TRUE(db != NULL);
    cc65dbg_free(db);
}

UTEST(cc65dbg, addr_to_src)
{
    cc65dbg_t *db = cc65dbg_load(TEST_FIXTURE);
    ASSERT_TRUE(db != NULL);
    const char *file = NULL;
    int line = 0;
    ASSERT_TRUE(cc65dbg_addr_to_src(db, 0x0239, &file, &line));
    ASSERT_TRUE(strcmp(file, "main.c") == 0);
    ASSERT_EQ(line, 10);
    ASSERT_TRUE(cc65dbg_addr_to_src(db, 0x0240, &file, &line));
    ASSERT_EQ(line, 10);
    ASSERT_TRUE(cc65dbg_addr_to_src(db, 0x024D, &file, &line));
    ASSERT_EQ(line, 11);
    ASSERT_FALSE(cc65dbg_addr_to_src(db, 0x0300, &file, &line));
    cc65dbg_free(db);
}

UTEST(cc65dbg, src_to_addr_basename)
{
    cc65dbg_t *db = cc65dbg_load(TEST_FIXTURE);
    uint16_t addr = 0;
    int bound = 0;
    ASSERT_TRUE(cc65dbg_src_to_addr(db, "main.c", 10, &addr, &bound));
    ASSERT_EQ((int)addr, 0x0239);
    ASSERT_EQ(bound, 10);
    ASSERT_TRUE(cc65dbg_src_to_addr(db, "/some/where/main.c", 11, &addr, &bound));
    ASSERT_EQ((int)addr, 0x024D);
    cc65dbg_free(db);
}

UTEST(cc65dbg, addr_to_func)
{
    cc65dbg_t *db = cc65dbg_load(TEST_FIXTURE);
    const char *fn = cc65dbg_addr_to_func(db, 0x0250);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(strcmp(fn, "main") == 0);
    ASSERT_TRUE(cc65dbg_addr_to_func(db, 0x0100) == NULL);
    cc65dbg_free(db);
}

UTEST(cc65dbg, ignores_non_c_lines)
{
    cc65dbg_t *db = cc65dbg_load(TEST_FIXTURE);
    const char *file = NULL;
    int line = 0;
    cc65dbg_addr_to_src(db, 0x0239, &file, &line);
    ASSERT_EQ(line, 10);
    cc65dbg_free(db);
}

/* An auto's address is the frame base plus its offs, and the frame base is
 * the live c_sp plus the frame size. main's deepest auto is j at offs -4, so
 * the frame size is 4 and the frame base is $0500 + 4 = $0504. */
UTEST(cc65dbg, locals_frame_base)
{
    cc65dbg_t *db = cc65dbg_load(TEST_FIXTURE);
    ASSERT_EQ((int)cc65dbg_frame_size(db, 0x0240), 4);
    uint16_t base = 0;
    ASSERT_TRUE(cc65dbg_frame_base(db, 0x0240, fake_mem, &base));
    ASSERT_EQ((int)base, 0x0504);
    uint16_t argsz = 0xffff;
    ASSERT_TRUE(cc65dbg_arg_size(db, 0x0240, &argsz));
    ASSERT_EQ((int)argsz, 0);

    cc65var_t v[16];
    int n = cc65dbg_locals(db, 0x0240, base, true, v, 16);
    ASSERT_EQ(n, 2);
    const cc65var_t *vi = NULL, *vj = NULL;
    for (int k = 0; k < n; k++)
    {
        if (strcmp(v[k].name, "i") == 0) vi = &v[k];
        if (strcmp(v[k].name, "j") == 0) vj = &v[k];
    }
    ASSERT_TRUE(vi && vj);
    ASSERT_TRUE(vi->addr_ok && vj->addr_ok);
    ASSERT_EQ((int)vi->addr, 0x0502);
    ASSERT_EQ((int)vj->addr, 0x0500);
    /* An auto below the frame base is as wide as the gap up to the next auto's
     * offs or the frame base, whichever is nearer, when that gap is 1, 2 or 4
     * bytes, so j at -4 and i at -2 are two bytes each. */
    ASSERT_EQ((int)vi->size, 2);
    ASSERT_EQ((int)vj->size, 2);
    n = cc65dbg_locals(db, 0x0240, base, false, v, 16);
    ASSERT_EQ(n, 2);
    ASSERT_FALSE(v[0].addr_ok);
    ASSERT_EQ(cc65dbg_locals(db, 0x0300, base, true, v, 16), 0);
    cc65dbg_free(db);
}

UTEST(cc65dbg, globals_via_import_chain)
{
    cc65dbg_t *db = cc65dbg_load(TEST_FIXTURE);
    cc65var_t v[16];
    int n = cc65dbg_globals(db, v, 16);
    /* gcounter is recorded twice, as its DATA label and as the extern csym
     * that imports it, and the two share an address so it is listed once. The
     * main csym resolves to a CODE label and is dropped. */
    ASSERT_EQ(n, 1);
    ASSERT_TRUE(strcmp(v[0].name, "gcounter") == 0);
    ASSERT_EQ((int)v[0].addr, 0x0800);
    ASSERT_EQ((int)v[0].size, 2);
    cc65dbg_free(db);
}

/* This fixture is trimmed from a real link with rp6502.cfg, which puts BSS on
 * top of ONCE, so the ONCE labels of the startup code share addresses with the
 * C globals. _errno_opt_constructor lies inside the long d, and initlib lies
 * inside the long e. In the fixture, ONCE has the lower seg id and ends past
 * BSS, so __oserror, the last byte of BSS, is one byte only when it is measured
 * to the end of its segment. __errno is a label with no segment, and it is
 * bounded by RIA_OP, which has none either. */
UTEST(cc65dbg, globals_under_once_overlay)
{
    cc65dbg_t *db = cc65dbg_load(TEST_ONCE_FIXTURE);
    ASSERT_TRUE(db != NULL);
    static const struct
    {
        const char *name;
        int addr;
        int size;
    } want[] = {
        {"a", 0x036D, 1},
        {"x", 0x036E, 2},
        {"b", 0x0370, 4},
        {"c", 0x0374, 2},
        {"d", 0x0376, 4},
        {"h", 0x037A, 4},
        {"s", 0x037E, 4},
        {"e", 0x0382, 4},
        {"__oserror", 0x0386, 1},
        {"__errno", 0xFFED, 2},
    };
    cc65var_t v[16];
    int n = cc65dbg_globals(db, v, 16);
    ASSERT_EQ(n, (int)(sizeof want / sizeof want[0]));
    for (int k = 0; k < n; k++)
    {
        ASSERT_STREQ(v[k].name, want[k].name);
        ASSERT_EQ((int)v[k].addr, want[k].addr);
        ASSERT_EQ((int)v[k].size, want[k].size);
    }
    cc65dbg_free(db);
}

UTEST_MAIN()
