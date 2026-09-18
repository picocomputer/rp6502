/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_CPU_DUT_H_
#define _TESTS_CPU_DUT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        uint16_t pc;
        uint8_t s, a, x, y, p;
    } dut_regs_t;

    typedef struct
    {
        const char *name;

        void (*reset)(void);

        void (*begin)(const dut_regs_t *regs);

        void (*bus)(uint16_t *addr, bool *read, bool *sync);

        void (*tick)(uint8_t *data);

        void (*end)(dut_regs_t *regs);

        void (*pins)(bool irq, bool nmi, bool rdy, bool res);
    } dut_t;

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_CPU_DUT_H_ */
