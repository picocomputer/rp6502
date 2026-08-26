/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Whichever 6522 this tree built.
 *
 * The VIA exists twice for the same reason the 6502 does — chips/m6522.h in
 * the emulator, w65c22.sv in the fabric — and the machine drives both the same
 * way: chip select asserted, ports unwired, a register read or written per
 * cycle. That is the whole of this interface, which is why one suite can hold
 * both to one recorded trace and to the same documented behaviour.
 *
 * via_chips.c and via_rtl.cpp are the two bindings.
 */

#ifndef _TESTS_CPU_WDC_VIA_DUT_H_
#define _TESTS_CPU_WDC_VIA_DUT_H_

#include "w65c22_scen.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Stand the VIA up, and take it down. Once per process. */
    void via_dut_init(int argc, const char *const argv[]);
    void via_dut_free(void);

    /* Power-on reset. Every scenario starts from one. */
    void via_reset(void);

    /* One cycle, the way core/wdc/via.c wires it: *data is what a read
     * returned (meaningless on a write or an idle), *irq the IRQ line as this
     * cycle left it. */
    void via_step(const w65c22_op_t *op, uint8_t *data, bool *irq);

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
