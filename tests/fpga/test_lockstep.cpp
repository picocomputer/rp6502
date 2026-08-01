/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Interrupts, reset, RDY and the stalls, proven by lockstep: the C emulation
 * and the RTL run the same program under the same scripted pin activity, and
 * every cycle's bus must match. The SingleStepTests vectors leave all of this
 * uncovered, and Klaus covers only a quiet-pin reset.
 */

#include "chips_dut.h"
#include "cpu65_dut.h"
#include "lockstep.h"
#include "utest.h"

#include <cstdio>
#include <cstring>

static uint8_t image[0x10000];

/* Hand-assembled scenario entries; the reset vector selects one per test. */
static void build_image()
{
    memset(image, 0, sizeof image);
    uint8_t code[] = {
        /* $0200: CLI; loop: INX; JMP loop — interrupts welcome */
        0x58, 0xE8, 0x4C, 0x01, 0x02,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* $0210: SEI; loop: INX; JMP loop — interrupts masked */
        0x78, 0xE8, 0x4C, 0x11, 0x02,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* $0220: CLI; WAI; INY; JMP $0221 — wait, then service */
        0x58, 0xCB, 0xC8, 0x4C, 0x21, 0x02,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* $0230: SEI; WAI; INY; JMP $0231 — wait, then continue */
        0x78, 0xCB, 0xC8, 0x4C, 0x31, 0x02,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* $0240: STP — only reset restarts */
        0xDB,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* $0250: CLI; loop: INC $40; BRA loop — RMW + taken branch */
        0x58, 0xE6, 0x40, 0x80, 0xFC,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* $0260: LDX #4; loop: DEX; BNE loop; JMP $0260 */
        0xA2, 0x04, 0xCA, 0xD0, 0xFD, 0x4C, 0x60, 0x02,
    };
    memcpy(&image[0x0200], code, sizeof code);

    /* Handlers: INY; RTI */
    image[0x0300] = 0xC8;
    image[0x0301] = 0x40;
    image[0x0308] = 0xC8;
    image[0x0309] = 0x40;

    image[0xFFFA] = 0x08;  /* NMI  -> $0308 */
    image[0xFFFB] = 0x03;
    image[0xFFFE] = 0x00;  /* IRQ  -> $0300 */
    image[0xFFFF] = 0x03;
}

static bool run(uint16_t entry, const lockstep_ev_t *evs, size_t n_evs,
                uint64_t cycles, lockstep_result_t *r)
{
    build_image();
    image[0xFFFC] = entry & 0xFF;
    image[0xFFFD] = entry >> 8;
    bool ok = lockstep_run(&chips_dut, &cpu65_dut, image, evs, n_evs, cycles, r);
    if (!ok)
        printf("%s\n", r->detail);
    return ok;
}

#define LOCKSTEP(name, entry, cycles, ...)                        \
    UTEST(lockstep, name)                                         \
    {                                                             \
        static const lockstep_ev_t evs[] = __VA_ARGS__;           \
        lockstep_result_t r;                                      \
        ASSERT_TRUE(run(entry, evs, sizeof evs / sizeof *evs,     \
                        cycles, &r));                             \
    }

LOCKSTEP(reset_only, 0x0200, 300, {{0, 0, 0, 0, 0}})

LOCKSTEP(irq_pulse, 0x0200, 400,
         {{20, 1, 0, 0, 0}, {24, 0, 0, 0, 0}})

/* Level-triggered: held IRQ re-enters the handler after every RTI. */
LOCKSTEP(irq_level_held, 0x0200, 600, {{20, 1, 0, 0, 0}})

LOCKSTEP(irq_masked, 0x0210, 400, {{20, 1, 0, 0, 0}})

LOCKSTEP(nmi_edge, 0x0200, 400,
         {{25, 0, 1, 0, 0}, {29, 0, 0, 0, 0}})

/* Edge-triggered: a second rise fires again, a held level does not. */
LOCKSTEP(nmi_two_edges, 0x0200, 600,
         {{25, 0, 1, 0, 0}, {29, 0, 0, 0, 0},
          {200, 0, 1, 0, 0}, {204, 0, 0, 0, 0}})

/* NMI is not gated by I. */
LOCKSTEP(nmi_while_masked, 0x0210, 400,
         {{25, 0, 1, 0, 0}, {29, 0, 0, 0, 0}})

LOCKSTEP(wai_irq, 0x0220, 400,
         {{40, 1, 0, 0, 0}, {60, 0, 0, 0, 0}})

LOCKSTEP(wai_irq_masked_continues, 0x0230, 400,
         {{40, 1, 0, 0, 0}, {60, 0, 0, 0, 0}})

LOCKSTEP(wai_nmi, 0x0220, 400,
         {{40, 0, 1, 0, 0}, {44, 0, 0, 0, 0}})

LOCKSTEP(stp_then_res, 0x0240, 300,
         {{30, 0, 0, 0, 1}, {33, 0, 0, 0, 0}})

LOCKSTEP(rdy_stretches, 0x0200, 400,
         {{15, 0, 0, 1, 0}, {21, 0, 0, 0, 0},
          {40, 0, 0, 1, 0}, {42, 0, 0, 0, 0}})

LOCKSTEP(res_mid_rmw, 0x0250, 300,
         {{22, 0, 0, 0, 1}, {24, 0, 0, 0, 0}})

/* The taken same-page branch delays interrupt recognition by one cycle. */
LOCKSTEP(branch_pip_irq, 0x0250, 400, {{30, 1, 0, 0, 0}})

LOCKSTEP(bne_loop_irq, 0x0260, 400, {{35, 1, 0, 0, 0}})

/* Deterministic pin fuzz: a small LFSR toggles IRQ, NMI and RDY over the
 * open-interrupt loop. Coverage of alignments no directed case picks. */
UTEST(lockstep, pin_fuzz)
{
    build_image();
    image[0xFFFC] = 0x00;
    image[0xFFFD] = 0x02;

    static lockstep_ev_t evs[512];
    uint16_t lfsr = 0xACE1;
    for (size_t i = 0; i < 512; i++)
    {
        lfsr = (uint16_t)((lfsr >> 1)
                          ^ (-(int)(lfsr & 1) & 0xB400));
        evs[i].cycle = 10 + i * 9;
        evs[i].irq = (lfsr >> 0) & 1;
        evs[i].nmi = (lfsr >> 3) & 1;
        evs[i].rdy = ((lfsr >> 7) & 3) == 3;  /* mostly low */
        evs[i].res = 0;
    }

    lockstep_result_t r;
    bool ok = lockstep_run(&chips_dut, &cpu65_dut, image,
                           evs, 512, 5000, &r);
    if (!ok)
        printf("%s\n", r.detail);
    ASSERT_TRUE(ok);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    cpu65_dut_init(argc, argv);
    int rc = utest_main(argc, argv);
    cpu65_dut_free();
    return rc;
}
