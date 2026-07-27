/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * SingleStepTests conformance: replay per-cycle bus traces against a CPU.
 *
 * The 6502 exists twice in this project, once as C in the emulator and once as
 * RTL in the FPGA core. Both answer the same interface here, so both are held
 * to the same vectors and neither can drift alone.
 */

#ifndef _TESTS_CPU_VEC_H_
#define _TESTS_CPU_VEC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        uint16_t pc;
        uint8_t s, a, x, y, p;
    } vec_regs_t;

    /* A CPU under test. The runner owns the 64 KB and the vectors; an
     * implementation only has to present its bus and step one cycle. */
    typedef struct
    {
        const char *name;

        /* Start an instruction: adopt regs and stand at the opcode fetch. */
        void (*begin)(const vec_regs_t *regs);

        /* What the CPU is driving this cycle, before any data moves. */
        void (*bus)(uint16_t *addr, bool *read);

        /* Advance one cycle. *data is the byte read; on a write it is set to
         * the byte written. */
        void (*tick)(uint8_t *data);

        void (*end)(vec_regs_t *regs);
    } vec_cpu_t;

    typedef struct
    {
        size_t passed;
        size_t failed;
        /* First failure, for the assertion message. */
        char detail[256];
    } vec_result_t;

    /* Replay every test in path. only_opcode >= 0 restricts to one opcode,
     * which is how a bring-up whitelist walks the instruction set. */
    bool vec_run(const char *path, const vec_cpu_t *cpu, int only_opcode,
                 vec_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_CPU_VEC_H_ */
