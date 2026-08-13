/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pin scenarios, once, for the two suites that replay them: test_lockstep
 * runs them with the C and the RTL side by side, and test_w65c02_chips runs
 * them against chips alone with a recorded trace as the reference. The image
 * and the event scripts live here so the RTL comparison and the drift check
 * cannot come to be asking different questions.
 *
 * Neither the SingleStepTests corpus nor Klaus reaches any of this — they
 * leave the pins quiet — which is also why nothing here needs either of them.
 */

#ifndef _TESTS_WDC_LOCKSTEP_SCEN_H_
#define _TESTS_WDC_LOCKSTEP_SCEN_H_

#include "lockstep.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* The hand-assembled 64K image, with entry written into $FFFC. */
    void lockstep_scen_image(uint8_t *image, uint16_t entry);

    typedef struct
    {
        const char *name;
        uint16_t entry;
        uint64_t cycles;
        const lockstep_ev_t *evs;
        size_t n_evs;
    } lockstep_scen_t;

/* Every directed case, so both suites register the same names and a new one
 * is added in a single place. */
#define LOCKSTEP_SCENARIOS(X)   \
    X(reset_only)               \
    X(irq_pulse)                \
    X(irq_level_held)           \
    X(irq_masked)               \
    X(nmi_edge)                 \
    X(nmi_two_edges)            \
    X(nmi_while_masked)         \
    X(wai_irq)                  \
    X(wai_irq_masked_continues) \
    X(wai_nmi)                  \
    X(stp_then_res)             \
    X(rdy_stretches)            \
    X(res_mid_rmw)              \
    X(branch_pip_irq)           \
    X(bne_loop_irq)

#define LOCKSTEP_SCEN_DECL(name) extern const lockstep_scen_t lockstep_scen_##name;
    LOCKSTEP_SCENARIOS(LOCKSTEP_SCEN_DECL)
#undef LOCKSTEP_SCEN_DECL

/* The pin fuzz: IRQ, NMI and RDY toggled over the open-interrupt loop, from
 * a fixed seed so both suites drive the identical sequence. */
#define LOCKSTEP_FUZZ_SEED 0xACE1
#define LOCKSTEP_FUZZ_EVENTS 512
#define LOCKSTEP_FUZZ_CYCLES 5000
#define LOCKSTEP_FUZZ_ENTRY 0x0200

    /* Fills evs[LOCKSTEP_FUZZ_EVENTS]. */
    void lockstep_scen_fuzz(lockstep_ev_t *evs);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_WDC_LOCKSTEP_SCEN_H_ */
