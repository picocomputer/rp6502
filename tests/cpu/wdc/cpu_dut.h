/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Whichever 6502 this tree built.
 *
 * dut.h is the interface both implementations answer; this is the one the
 * suites name, so a corpus is written once and run against the emulator's CPU
 * or the fabric's depending on which tree is asking. cpu_dut_chips.c and
 * cpu_dut_rtl.cpp are the two bindings.
 *
 * Both implementations keep their own names as well — test_lockstep runs them
 * against each other and has to be able to say which is which.
 */

#ifndef _TESTS_CPU_WDC_CPU_DUT_H_
#define _TESTS_CPU_WDC_CPU_DUT_H_

#include "dut.h"

#ifdef __cplusplus
extern "C"
{
#endif

    extern const dut_t *const cpu_dut;

    /* Stand the CPU up, and take it down. Once per process: a verilated model
     * is too expensive to build per case, and the C one has nothing to do. */
    void cpu_dut_init(int argc, const char *const argv[]);
    void cpu_dut_free(void);

#ifdef __cplusplus
}
#endif

/* Replaces UTEST_MAIN(), so a suite says nothing about which CPU it got. */
#define CPU_DUT_MAIN()                               \
    UTEST_STATE();                                   \
    int main(int argc, const char *const argv[])     \
    {                                                \
        cpu_dut_init(argc, argv);                    \
        int rc = utest_main(argc, argv);             \
        cpu_dut_free();                              \
        return rc;                                   \
    }

#endif /* _TESTS_CPU_WDC_CPU_DUT_H_ */
