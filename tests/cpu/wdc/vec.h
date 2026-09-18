/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_CPU_VEC_H_
#define _TESTS_CPU_VEC_H_

#include <stddef.h>

#include "dut.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        size_t passed;
        size_t failed;
        char detail[256];
    } vec_result_t;

    bool vec_run(const char *path, const dut_t *cpu, int only_opcode,
                 vec_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_CPU_VEC_H_ */
