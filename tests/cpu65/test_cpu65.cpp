/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The suites in tests/cpu against the verilated cpu65. The emulator's CPU
 * answers the same dut_t in tests/cpu65/chips_dut.c, so a divergence between the
 * two implementations fails here with the opcode and cycle named.
 */

#include "cpu65_dut.h"
#include "klaus.h"
#include "utest.h"
#include "vec.h"

#include <cstdio>

UTEST(cpu65, singlestep_vectors)
{
    vec_result_t r;
    ASSERT_TRUE(vec_run(VECTORS, &cpu65_dut, -1, &r));
    if (r.failed)
        printf("%s\n", r.detail);
    ASSERT_EQ(r.failed, (size_t)0);
    ASSERT_GT(r.passed, (size_t)0);
}

UTEST(cpu65, klaus_functional)
{
    klaus_result_t r;
    ASSERT_TRUE(klaus_run(KLAUS_6502, &cpu65_dut, 500000000ull, &r));
    if (!r.trapped || r.trap_pc != 0x3469)
        printf("trapped=%d timed_out=%d pc=$%04X after %llu cycles\n",
               r.trapped, r.timed_out, r.trap_pc,
               (unsigned long long)r.cycles);
    ASSERT_FALSE(r.timed_out);
    ASSERT_EQ(r.trap_pc, 0x3469);
}

UTEST(cpu65, klaus_extended_opcodes)
{
    klaus_result_t r;
    ASSERT_TRUE(klaus_run(KLAUS_65C02, &cpu65_dut, 500000000ull, &r));
    if (!r.trapped || r.trap_pc != 0x24F1)
        printf("trapped=%d timed_out=%d pc=$%04X after %llu cycles\n",
               r.trapped, r.timed_out, r.trap_pc,
               (unsigned long long)r.cycles);
    ASSERT_FALSE(r.timed_out);
    ASSERT_EQ(r.trap_pc, 0x24F1);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    cpu65_dut_init(argc, argv);
    int rc = utest_main(argc, argv);
    cpu65_dut_free();
    return rc;
}
