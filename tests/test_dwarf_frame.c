/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * DWARF .debug_frame CFI unwinder (dwarf_frame.c). Exercised against
 * tests/roms/dwtest.elf (llvm-mos debug fork, -O0 -g). llvm-mos emits a two-stack
 * unwind: the CFA and return address are recovered from the 6502 hardware stack
 * via CFI expressions, and the soft-stack pointer RS0 (each frame's variable
 * base) is recovered as the CFA. The fixture is committed; no toolchain needed.
 */

#include "emu/dbg/dwarf_frame.h"
#include "utest.h"

#include <string.h>

#ifndef DW5_ELF
#define DW5_ELF "dwtest.elf"
#endif

static uint8_t g_mem[0x10000];
static uint8_t rd(uint16_t a) { return g_mem[a]; }

UTEST(dwarf_frame, loads)
{
    dwarf_frame_t *df = dwarf_frame_load(DW5_ELF);
    ASSERT_TRUE(df != NULL);
    ASSERT_TRUE(dwarf_frame_has(df, 0x0640));  /* inside area */
    ASSERT_FALSE(dwarf_frame_has(df, 0x0010)); /* below .text */
    dwarf_frame_free(df);
}

/* Stopped in `area` past its 0x647 prologue. Soft SP 0x9000, 6502 SP 0xF8
 * (s16 = 0x01F8). area's FDE row: CFA = RS0 + 14; caller PC = deref[((S+2) &
 * 0xff) | 0x100] = deref[0x1FA]; caller S = ((S+3) & 0xff) | 0x100; caller RS0 =
 * CFA. Put a call site (0x0400, in measure) at 0x1FA. */
UTEST(dwarf_frame, unwind_area)
{
    dwarf_frame_t *df = dwarf_frame_load(DW5_ELF);
    ASSERT_TRUE(df != NULL);
    memset(g_mem, 0, sizeof g_mem);
    g_mem[0x1FA] = 0x00;
    g_mem[0x1FB] = 0x04;
    dwarf_unwind_t u = dwarf_frame_step(df, 0x0660, 0x01F8, 0x9000, rd);
    ASSERT_TRUE(u.ok);
    ASSERT_EQ((int)u.cfa, 0x900E); /* RS0 + 14 */
    ASSERT_EQ((int)u.pc, 0x0400);  /* return slot -> a call site in measure */
    ASSERT_EQ((int)u.s16, 0x01FB); /* ((0x1F8+3)&0xff)|0x100 */
    ASSERT_EQ((int)u.rs0, 0x900E); /* caller soft-stack base = CFA */
    dwarf_frame_free(df);
}

/* Unwinding a frame whose return slot is empty yields a caller PC outside any
 * function, which stops the walk (never fabricate a frame). */
UTEST(dwarf_frame, unwind_terminates)
{
    dwarf_frame_t *df = dwarf_frame_load(DW5_ELF);
    ASSERT_TRUE(df != NULL);
    memset(g_mem, 0, sizeof g_mem);
    dwarf_unwind_t u = dwarf_frame_step(df, 0x0400, 0x01FB, 0x900E, rd); /* in measure */
    ASSERT_TRUE(u.ok);
    ASSERT_FALSE(dwarf_frame_has(df, u.pc)); /* caller pc has no FDE -> stop */
    dwarf_frame_free(df);
}

UTEST_MAIN()
