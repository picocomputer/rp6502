/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_CPU_WDC_VIA_DUT_H_
#define _TESTS_CPU_WDC_VIA_DUT_H_

#include "via_scen.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    void via_dut_init(int argc, const char *const argv[]);
    void via_dut_free(void);

    void via_reset(void);

    void via_step(const via_op_t *op, uint8_t *data, bool *irq);

#ifdef __cplusplus
}
#endif

#define VIA_DUT_MAIN()                               \
    UTEST_STATE();                                   \
    int main(int argc, const char *const argv[])     \
    {                                                \
        via_dut_init(argc, argv);                    \
        int rc = utest_main(argc, argv);             \
        via_dut_free();                              \
        return rc;                                   \
    }

#endif /* _TESTS_CPU_WDC_VIA_DUT_H_ */
