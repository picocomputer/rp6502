/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * cc65 .dbg reader (cc65dbg.c): address<->source-line mapping, the enclosing
 * function, and the best-effort UNTYPED variable views (auto locals via the C
 * stack pointer, globals via the import->export chain). Exercised against the
 * committed, hand-authored fixture tests/roms/cc65.dbg — a format-faithful
 * minimal ld65 --dbgfile (no cc65 toolchain needed). Layout encoded there:
 *   CODE @ $0239; line 10 -> [$0239,$024D), line 11 -> [$024D,$0259)
 *   _main @ $0239 size 48; c_sp (zeropage) @ $00; _gcounter (data) @ $0800
 *   auto `local` at c_sp+4; global `gcounter`; the `main` csym is a function.
 */

#include "emu/dbg/cc65dbg.h"
#include "utest.h"

#include <string.h>

#ifndef CC65_DBG
#define CC65_DBG "cc65.dbg"
#endif

/* c_sp ($00) -> $0500, so auto locals resolve to c_sp + offs. */
static uint8_t fake_mem(uint16_t a)
{
    if (a == 0) return 0x00;
    if (a == 1) return 0x05;
    return 0;
}

UTEST(cc65dbg, loads)
{
    cc65dbg_t *db = cc65dbg_load(CC65_DBG);
    ASSERT_TRUE(db != NULL);
    cc65dbg_free(db);
}

UTEST(cc65dbg, addr_to_src)
{
    cc65dbg_t *db = cc65dbg_load(CC65_DBG);
    ASSERT_TRUE(db != NULL);
    const char *file = NULL;
    int line = 0;
    ASSERT_TRUE(cc65dbg_addr_to_src(db, 0x0239, &file, &line));
    ASSERT_TRUE(strcmp(file, "main.c") == 0);
    ASSERT_EQ(line, 10);
    ASSERT_TRUE(cc65dbg_addr_to_src(db, 0x0240, &file, &line)); /* mid-span */
    ASSERT_EQ(line, 10);
    ASSERT_TRUE(cc65dbg_addr_to_src(db, 0x024D, &file, &line));
    ASSERT_EQ(line, 11);
    /* outside any C span */
    ASSERT_FALSE(cc65dbg_addr_to_src(db, 0x0300, &file, &line));
    cc65dbg_free(db);
}

UTEST(cc65dbg, src_to_addr_basename)
{
    cc65dbg_t *db = cc65dbg_load(CC65_DBG);
    uint16_t addr = 0;
    int bound = 0;
    ASSERT_TRUE(cc65dbg_src_to_addr(db, "main.c", 10, &addr, &bound));
    ASSERT_EQ((int)addr, 0x0239);
    ASSERT_EQ(bound, 10);
    /* matched by basename, and a request below a code line binds forward */
    ASSERT_TRUE(cc65dbg_src_to_addr(db, "/some/where/main.c", 11, &addr, &bound));
    ASSERT_EQ((int)addr, 0x024D);
    cc65dbg_free(db);
}

UTEST(cc65dbg, addr_to_func)
{
    cc65dbg_t *db = cc65dbg_load(CC65_DBG);
    const char *fn = cc65dbg_addr_to_func(db, 0x0250);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(strcmp(fn, "main") == 0); /* leading '_' stripped */
    /* below the first function -> none (cc65 funcs carry no size, so a PC past a
     * function maps to the nearest preceding one, like the firmware monitor) */
    ASSERT_TRUE(cc65dbg_addr_to_func(db, 0x0100) == NULL);
    cc65dbg_free(db);
}

/* A type=0 (asm) line on the same span must not shadow the C line. */
UTEST(cc65dbg, ignores_non_c_lines)
{
    cc65dbg_t *db = cc65dbg_load(CC65_DBG);
    const char *file = NULL;
    int line = 0;
    cc65dbg_addr_to_src(db, 0x0239, &file, &line);
    ASSERT_EQ(line, 10); /* the C line, never the type=0 line 99 */
    cc65dbg_free(db);
}

/* cc65 `offs` is relative to the frame base (entry sp); the live sp sits
 * frame_size below it, so the frame base = live sp + frame_size and a local's
 * address = frame_base + offs. The fixture's main has autos i@offs-2 and
 * j@offs-4, so frame_size=4, frame base = $0500 + 4 = $0504. */
UTEST(cc65dbg, locals_frame_base)
{
    cc65dbg_t *db = cc65dbg_load(CC65_DBG);
    ASSERT_EQ((int)cc65dbg_frame_size(db, 0x0240), 4); /* deepest auto j@-4 */
    uint16_t base = 0;
    ASSERT_TRUE(cc65dbg_frame_base(db, 0x0240, fake_mem, &base));
    ASSERT_EQ((int)base, 0x0504); /* live sp($0500) + frame_size(4) */
    /* main takes no parameters -> its argument region is exactly 0 (chainable). */
    uint16_t argsz = 0xffff;
    ASSERT_TRUE(cc65dbg_arg_size(db, 0x0240, &argsz));
    ASSERT_EQ((int)argsz, 0);

    cc65var_t v[16];
    int n = cc65dbg_locals(db, 0x0240, base, true, v, 16);
    ASSERT_EQ(n, 2); /* i + j; the `main` csym is a function, not an auto */
    const cc65var_t *vi = NULL, *vj = NULL;
    for (int k = 0; k < n; k++)
    {
        if (strcmp(v[k].name, "i") == 0) vi = &v[k];
        if (strcmp(v[k].name, "j") == 0) vj = &v[k];
    }
    ASSERT_TRUE(vi && vj);
    ASSERT_TRUE(vi->addr_ok && vj->addr_ok);
    ASSERT_EQ((int)vi->addr, 0x0502); /* base($0504) + offs(-2) */
    ASSERT_EQ((int)vj->addr, 0x0500); /* base($0504) + offs(-4) */
    /* widths from the offset gap: j@-4 -> i@-2 = 2; i@-2 -> frame base 0 = 2 */
    ASSERT_EQ((int)vi->size, 2);
    ASSERT_EQ((int)vj->size, 2);
    /* a caller frame whose base couldn't be reconstructed -> unresolvable */
    n = cc65dbg_locals(db, 0x0240, base, false, v, 16);
    ASSERT_EQ(n, 2);
    ASSERT_FALSE(v[0].addr_ok);
    /* outside the function scope -> no locals */
    ASSERT_EQ(cc65dbg_locals(db, 0x0300, base, true, v, 16), 0);
    cc65dbg_free(db);
}

UTEST(cc65dbg, globals_via_import_chain)
{
    cc65dbg_t *db = cc65dbg_load(CC65_DBG);
    cc65var_t v[16];
    int n = cc65dbg_globals(db, v, 16);
    /* `gcounter` is recorded twice (its DATA label + the extern csym importing
     * it) and deduped to one; the function `main` is excluded (CODE segment). */
    ASSERT_EQ(n, 1);
    ASSERT_TRUE(strcmp(v[0].name, "gcounter") == 0);
    ASSERT_EQ((int)v[0].addr, 0x0800);
    ASSERT_EQ((int)v[0].size, 2); /* the data lab carries an explicit size=2 */
    cc65dbg_free(db);
}

UTEST_MAIN()
