/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * SingleStepTests conformance for the emulator's 65C02. The FPGA core runs
 * these same vectors against w65c02.sv; holding both to one set of per-cycle
 * bus traces is what keeps the two implementations from drifting apart.
 */

#include "chips_dut.h"
#include "utest.h"
#include "vec.h"

#include <stdio.h>

UTEST(vectors, all_opcodes)
{
    vec_result_t r;
    ASSERT_TRUE(vec_run(VECTORS, &chips_dut, -1, &r));
    if (r.failed)
        printf("%s\n", r.detail);
    ASSERT_EQ(r.failed, (size_t)0);
    ASSERT_GT(r.passed, (size_t)0);
}

/* The 16 bit-test branches are read-only and take 5, 6 or 7 cycles with the
 * branch. vendor/chips had them writing and always taking 6; see
 * vendor/chips_rp6502. */
UTEST(vectors, bit_branches_never_write)
{
    vec_result_t r;
    for (int op = 0x0F; op <= 0xFF; op += 0x10)
    {
        ASSERT_TRUE(vec_run(VECTORS, &chips_dut, op, &r));
        if (r.failed)
            printf("%s\n", r.detail);
        ASSERT_EQ(r.failed, (size_t)0);
        ASSERT_GT(r.passed, (size_t)0);
    }
}

UTEST_MAIN()
