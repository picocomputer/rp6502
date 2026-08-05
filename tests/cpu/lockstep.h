/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Two CPUs, one pin script, cycle-for-cycle comparison. The SingleStepTests
 * vectors never raise IRQ, NMI, RES or RDY, so the interrupt machinery is
 * proven by running the C emulation and the RTL side by side through directed
 * scenarios and demanding identical bus activity every cycle.
 */

#ifndef _TESTS_CPU_LOCKSTEP_H_
#define _TESTS_CPU_LOCKSTEP_H_

#include <stddef.h>
#include <stdint.h>

#include "dut.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Pin levels take effect at a cycle and hold until the next event. */
    typedef struct
    {
        uint64_t cycle;
        bool irq, nmi, rdy, res;
    } lockstep_ev_t;

    typedef struct
    {
        uint64_t cycles;
        /* First divergence, for the assertion message. */
        char detail[256];
    } lockstep_result_t;

    /* Reset both CPUs into copies of the same 64 KB image and run them under
     * the script. Returns false on the first cycle where address, direction,
     * sync or data differ. */
    bool lockstep_run(const dut_t *ref, const dut_t *dut,
                      const uint8_t *image,
                      const lockstep_ev_t *evs, size_t n_evs,
                      uint64_t cycles, lockstep_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_CPU_LOCKSTEP_H_ */
