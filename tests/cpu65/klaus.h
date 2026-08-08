/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Klaus Dormann's functional tests: a self-checking 64 KB image that runs for
 * tens of millions of cycles and ends by jumping to itself. Where the
 * SingleStepTests vectors check one instruction per case, this checks long
 * sequences, so the two suites fail in different ways and both are worth
 * running.
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
        bool trapped;      /* execution stopped moving */
        bool timed_out;    /* budget exhausted, still running */
        uint16_t trap_pc;  /* where it stopped; success_pc means it passed */
        uint64_t cycles;
    } klaus_result_t;

    /* Load a raw 64 KB image, reset into it, and run until execution traps.
     * Pass and fail both end in a jump to self, so the caller compares trap_pc
     * against the image's success address. */
    bool klaus_run(const char *path, const dut_t *cpu, uint64_t max_cycles,
                   klaus_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_CPU_KLAUS_H_ */
