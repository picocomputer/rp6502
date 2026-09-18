/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_CPU_KLAUS_H_
#define _TESTS_CPU_KLAUS_H_

#include <stdint.h>

#include "dut.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        bool trapped;
        bool timed_out;
        uint16_t trap_pc;
        uint64_t cycles;
    } klaus_result_t;

    bool klaus_run(const char *path, const dut_t *cpu, uint64_t max_cycles,
                   klaus_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_CPU_KLAUS_H_ */
