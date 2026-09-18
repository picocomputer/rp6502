/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

    typedef struct
    {
        uint64_t cycle;
        bool irq, nmi, rdy, res;
    } lockstep_ev_t;

    typedef struct
    {
        uint64_t cycles;
        char detail[256];
    } lockstep_result_t;

    bool lockstep_run(const dut_t *ref, const dut_t *dut,
                      const uint8_t *image,
                      const lockstep_ev_t *evs, size_t n_evs,
                      uint64_t cycles, lockstep_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_CPU_LOCKSTEP_H_ */
