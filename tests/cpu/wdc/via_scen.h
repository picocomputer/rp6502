/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
        uint16_t repeat;
    } via_op_t;

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

#define VIA_FUZZ_SEED 0xBEEF
#define VIA_FUZZ_CYCLES 30000

    void via_fuzz_next(uint16_t *lfsr, via_op_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_WDC_VIA_SCEN_H_ */
