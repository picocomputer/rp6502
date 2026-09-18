/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_CPU_WDC_CPU_DUT_H_
#define _TESTS_CPU_WDC_CPU_DUT_H_

#include "dut.h"

#ifdef __cplusplus
extern "C"
{
#endif

    extern const dut_t *const cpu_dut;

    void cpu_dut_init(int argc, const char *const argv[]);
    void cpu_dut_free(void);

#ifdef __cplusplus
}
#endif

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
