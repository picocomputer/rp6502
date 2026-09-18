/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu_dut.h"
#include "utest.h"
#include "vec.h"

#include <stdio.h>

UTEST(vectors, all_opcodes)
{
    vec_result_t r;
    ASSERT_TRUE(vec_run(VECTORS, cpu_dut, -1, &r));
    if (r.failed)
        printf("%s\n", r.detail);
    ASSERT_EQ(r.failed, (size_t)0);
    ASSERT_GT(r.passed, (size_t)0);
}

/* Opcodes $0F to $FF in steps of $10 are the 16 bit-test branches, BBR0-7
 * and BBS0-7. Each reads zero page without writing and takes 5 cycles, 6 when
 * the branch is taken and 7 when the taken branch crosses a page. */
UTEST(vectors, bit_branches_never_write)
{
    vec_result_t r;
    for (int op = 0x0F; op <= 0xFF; op += 0x10)
    {
        ASSERT_TRUE(vec_run(VECTORS, cpu_dut, op, &r));
        if (r.failed)
            printf("%s\n", r.detail);
        ASSERT_EQ(r.failed, (size_t)0);
        ASSERT_GT(r.passed, (size_t)0);
    }
}

CPU_DUT_MAIN()
