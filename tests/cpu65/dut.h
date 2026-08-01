/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502 under test. The 6502 exists twice in this project, once as C in the
 * emulator and once as RTL in the FPGA core; both answer this, so both are held
 * to the same suites and neither can drift alone.
 *
 * Runners own the memory and the fixtures. A CPU only has to start, present its
 * bus, and step a cycle — which a pin-based C model and a clocked RTL model can
 * both do.
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

        /* Power on and run the reset sequence, taking PC from $FFFC. */
        void (*reset)(void);

        /* Adopt registers and stand at an opcode fetch, for fixtures that start
         * mid-stream instead of at reset. */
        void (*begin)(const dut_regs_t *regs);

        /* What the CPU drives this cycle, before any data moves. */
        void (*bus)(uint16_t *addr, bool *read, bool *sync);

        /* Advance one cycle. *data is the byte read; on a write it is set to
         * the byte written. */
        void (*tick)(uint8_t *data);

        void (*end)(dut_regs_t *regs);

        /* Drive the interrupt-side pins for the coming cycles. Suites that
         * never raise them leave this unused. */
        void (*pins)(bool irq, bool nmi, bool rdy, bool res);
    } dut_t;

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_CPU_DUT_H_ */
