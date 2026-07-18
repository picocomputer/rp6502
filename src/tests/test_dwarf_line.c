/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * DWARF .debug_line reader (dwarf_line.c): address<->source-line mapping and the
 * enclosing function (.symtab STT_FUNC). Exercised against the committed fixture
 * tests/roms/vars.elf (an llvm-mos -O0 -gdwarf-4 build of vars.c) — no toolchain
 * needed. Known rows in that build: $02C4->20 $02F3->21 $02FB->22 $0328->23
 * $033D->24 $03DB->25; main spans [$02C4,$03EF).
 */

#include "emu/dbg/dwarf_line.h"
#include "utest.h"

#include <string.h>

#ifndef VARS_ELF
#define VARS_ELF "vars.elf"
#endif

UTEST(dwarf_line, loads)
{
    dwarf_line_t *dl = dwarf_line_load(VARS_ELF);
    ASSERT_TRUE(dl != NULL);
    dwarf_line_free(dl);
}

UTEST(dwarf_line, addr_to_src)
{
    dwarf_line_t *dl = dwarf_line_load(VARS_ELF);
    ASSERT_TRUE(dl != NULL);
    const char *file = NULL;
    int line = 0;
    ASSERT_TRUE(dwarf_line_addr_to_src(dl, 0x02F3, &file, &line));
    ASSERT_TRUE(strstr(file, "vars.c") != NULL);
    ASSERT_EQ(line, 21);
    /* an address inside line 21's row [0x02F3,0x02FB) maps back to line 21 */
    ASSERT_TRUE(dwarf_line_addr_to_src(dl, 0x02F8, &file, &line));
    ASSERT_EQ(line, 21);
    ASSERT_TRUE(dwarf_line_addr_to_src(dl, 0x0328, &file, &line));
    ASSERT_EQ(line, 23);
    /* well past end_sequence -> no mapping */
    ASSERT_FALSE(dwarf_line_addr_to_src(dl, 0xF000, &file, &line));
    dwarf_line_free(dl);
}

UTEST(dwarf_line, src_to_addr)
{
    dwarf_line_t *dl = dwarf_line_load(VARS_ELF);
    uint16_t addr = 0;
    int bound = 0;
    ASSERT_TRUE(dwarf_line_src_to_addr(dl, "vars.c", 20, &addr, &bound));
    ASSERT_EQ((int)addr, 0x02C4);
    ASSERT_EQ(bound, 20);
    ASSERT_TRUE(dwarf_line_src_to_addr(dl, "vars.c", 23, &addr, &bound));
    ASSERT_EQ((int)addr, 0x0328);
    /* matched by basename + forward-binds a blank line to the next code line */
    ASSERT_TRUE(dwarf_line_src_to_addr(dl, "/abs/path/vars.c", 19, &addr, &bound));
    ASSERT_EQ((int)addr, 0x02C4);
    ASSERT_EQ(bound, 20);
    dwarf_line_free(dl);
}

UTEST(dwarf_line, addr_to_func)
{
    dwarf_line_t *dl = dwarf_line_load(VARS_ELF);
    const char *fn = dwarf_line_addr_to_func(dl, 0x0300);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(strcmp(fn, "main") == 0);
    dwarf_line_free(dl);
}

UTEST_MAIN()
