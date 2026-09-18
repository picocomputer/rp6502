/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/dap/dwarf_frame.h"
#include "utest.h"

#include <string.h>

#ifndef TEST_FIXTURE
#define TEST_FIXTURE "dwtest.elf"
#endif

static uint8_t g_mem[0x10000];
static uint8_t rd(uint16_t a) { return g_mem[a]; }

UTEST(dwarf_frame, loads)
{
    dwarf_frame_t *df = dwarf_frame_load(TEST_FIXTURE);
    ASSERT_TRUE(df != NULL);
    ASSERT_TRUE(dwarf_frame_has(df, 0x0640));  /* 0x0640 is in area */
    ASSERT_FALSE(dwarf_frame_has(df, 0x0010)); /* 0x0010 is below .text */
    dwarf_frame_free(df);
}

/* From 0x647 on, area's FDE row has these rules: the CFA is RS0 + 14, the
 * caller's PC is the word at ((S + 2) & 0xff) | 0x100, the caller's S is
 * ((S + 3) & 0xff) | 0x100, and the caller's RS0 is the CFA. With RS0 at
 * 0x9000 and S at 0x01F8, the return slot is 0x1FA, where the test puts
 * 0x0400, an address in measure. */
UTEST(dwarf_frame, unwind_area)
{
    dwarf_frame_t *df = dwarf_frame_load(TEST_FIXTURE);
    ASSERT_TRUE(df != NULL);
    memset(g_mem, 0, sizeof g_mem);
    g_mem[0x1FA] = 0x00;
    g_mem[0x1FB] = 0x04;
    dwarf_unwind_t u = dwarf_frame_step(df, 0x0660, 0x01F8, 0x9000, rd);
    ASSERT_TRUE(u.ok);
    ASSERT_EQ((int)u.cfa, 0x900E);
    ASSERT_EQ((int)u.pc, 0x0400);
    ASSERT_EQ((int)u.s16, 0x01FB);
    ASSERT_EQ((int)u.rs0, 0x900E);
    dwarf_frame_free(df);
}

/* With memory zeroed, measure's return slot holds 0x0000, which no FDE covers,
 * so the unwind stops there. */
UTEST(dwarf_frame, unwind_terminates)
{
    dwarf_frame_t *df = dwarf_frame_load(TEST_FIXTURE);
    ASSERT_TRUE(df != NULL);
    memset(g_mem, 0, sizeof g_mem);
    dwarf_unwind_t u = dwarf_frame_step(df, 0x0400, 0x01FB, 0x900E, rd);
    ASSERT_TRUE(u.ok);
    ASSERT_FALSE(dwarf_frame_has(df, u.pc));
    dwarf_frame_free(df);
}

UTEST_MAIN()
