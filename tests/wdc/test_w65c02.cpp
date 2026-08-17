/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The suites in tests/wdc against the verilated w65c02. The emulator's CPU
 * answers the same dut_t in tests/wdc/chips_dut.c, so a divergence between the
 * two implementations fails here with the opcode and cycle named.
 */

#include "klaus.h"
#include "utest.h"
#include "vec.h"
#include "w65c02_dut.h"

#include <cstdio>

UTEST(w65c02, singlestep_vectors)
{
    vec_result_t r;
    ASSERT_TRUE(vec_run(VECTORS, &w65c02_dut, -1, &r));
    if (r.failed)
        printf("%s\n", r.detail);
    ASSERT_EQ(r.failed, (size_t)0);
    ASSERT_GT(r.passed, (size_t)0);
}

UTEST(w65c02, klaus_functional)
{
    klaus_result_t r;
    ASSERT_TRUE(klaus_run(KLAUS_6502, &w65c02_dut, 500000000ull, &r));
    if (!r.trapped || r.trap_pc != 0x3469)
        printf("trapped=%d timed_out=%d pc=$%04X after %llu cycles\n",
               r.trapped, r.timed_out, r.trap_pc,
               (unsigned long long)r.cycles);
    ASSERT_FALSE(r.timed_out);
    ASSERT_EQ(r.trap_pc, 0x3469);
}

UTEST(w65c02, klaus_extended_opcodes)
{
    klaus_result_t r;
    ASSERT_TRUE(klaus_run(KLAUS_65C02, &w65c02_dut, 500000000ull, &r));
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
    w65c02_dut_init(argc, argv);
    int rc = utest_main(argc, argv);
    w65c02_dut_free();
    return rc;
}
