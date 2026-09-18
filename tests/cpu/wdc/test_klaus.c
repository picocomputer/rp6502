/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu_dut.h"
#include "klaus.h"
#include "utest.h"

#include <stdio.h>

#define KLAUS_BUDGET 500000000ull

static void report(const klaus_result_t *r, uint16_t want)
{
    if (!r->trapped || r->trap_pc != want)
        printf("trapped=%d timed_out=%d pc=$%04X want $%04X after %llu cycles\n",
               r->trapped, r->timed_out, r->trap_pc, want,
               (unsigned long long)r->cycles);
}

UTEST(klaus, functional)
{
    klaus_result_t r;
    ASSERT_TRUE(klaus_run(KLAUS_6502, cpu_dut, KLAUS_BUDGET, &r));
    report(&r, 0x3469);
    ASSERT_FALSE(r.timed_out);
    ASSERT_EQ(r.trap_pc, 0x3469);
}

UTEST(klaus, extended_opcodes)
{
    klaus_result_t r;
    ASSERT_TRUE(klaus_run(KLAUS_65C02, cpu_dut, KLAUS_BUDGET, &r));
    report(&r, 0x24F1);
    ASSERT_FALSE(r.timed_out);
    ASSERT_EQ(r.trap_pc, 0x24F1);
}

CPU_DUT_MAIN()
