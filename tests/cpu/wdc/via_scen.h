/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The VIA scenarios, written once and replayed by test_via against
 * whichever VIA the tree built, with a recorded trace as the reference.
 * They were two suites asking the same question of two implementations;
 * one set of scenarios and one recording is what keeps that one question.
 *
 * Register traffic only — the ports are unwired, exactly as core/wdc/via.c
 * leaves them.
 */

#ifndef _TESTS_WDC_VIA_SCEN_H_
#define _TESTS_WDC_VIA_SCEN_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        VIA_OP_IDLE,
        VIA_OP_READ,
        VIA_OP_WRITE,
    } via_op_kind_t;

    typedef struct
    {
        via_op_kind_t kind;
        uint8_t rs;
        uint8_t data;
        uint16_t repeat; /* extra cycles of the same op; idle mostly */
    } via_op_t;

/* Every directed script, so both suites register the same case names and a
 * new one is added in a single place. */
#define VIA_SCRIPTS(X) \
    X(t1_oneshot)         \
    X(t1_continuous)      \
    X(t1_pb7)             \
    X(t2_oneshot)         \
    X(t2_pb6_quirk)       \
    X(ifr_ier)            \
    X(readback_all)

#define VIA_SCEN_DECL(name)                      \
    extern const via_op_t via_scen_##name[];  \
    extern const size_t via_scen_##name##_n;
    VIA_SCRIPTS(VIA_SCEN_DECL)
#undef VIA_SCEN_DECL

/* The fuzz: register traffic with idle gaps, from a fixed seed so both
 * suites drive the identical sequence. */
#define VIA_FUZZ_SEED 0xBEEF
#define VIA_FUZZ_CYCLES 30000

    /* Advances lfsr and writes the next op. */
    void via_fuzz_next(uint16_t *lfsr, via_op_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_WDC_VIA_SCEN_H_ */
