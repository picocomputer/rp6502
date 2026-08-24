/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * SingleStepTests conformance for whichever 65C02 this tree built — the
 * emulator's C model, or w65c02.sv verilated. One suite, one corpus, and
 * neither implementation with a standard of its own is what keeps the two
 * from drifting apart.
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

/* The 16 bit-test branches are read-only and take 5, 6 or 7 cycles with the
 * branch. */
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
