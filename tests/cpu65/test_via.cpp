/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * via.sv against chips/m6522.h in lockstep, driven exactly as the machine
 * drives its VIA: register reads and writes with the ports unwired. Every
 * cycle's read data and IRQ line must match, through the timer pipelines,
 * the IFR/IER rules and the PB7 toggle.
 *
 * The scenarios are via_scen.c, shared with test_via_chips, which replays
 * them against chips alone when there is no simulator to disagree with.
 */

#define CHIPS_IMPL
#include "chips/chips/m6522.h"

#include "via_scen.h"

#include "Vvia.h"
#include "utest.h"

#include <cstdio>

static m6522_t ref;
static Vvia *dut;

static void reset_both()
{
    m6522_init(&ref);
    dut->rst_n = 0;
    dut->en = 1;
    dut->cs = 0;
    dut->we = 0;
    dut->rs = 0;
    dut->data_i = 0;
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
    dut->rst_n = 1;
}

/* One cycle on both. Returns false on divergence, with detail filled. */
static bool cycle(const via_op_t *op, uint64_t n, char *detail, size_t cap)
{
    /* The reference, wired the way emu/emu/via.c wires it. */
    uint64_t pins = 0;
    if (op->kind != VIA_OP_IDLE)
    {
        pins |= op->rs & M6522_RS_PINS;
        pins |= M6522_CS1;
        if (op->kind == VIA_OP_READ)
            pins |= M6522_RW;
        else
            M6522_SET_DATA(pins, op->data);
    }
    pins = m6522_tick(&ref, pins);
    uint8_t ref_data = M6522_GET_DATA(pins);
    bool ref_irq = (pins & M6522_IRQ) != 0;

    /* The RTL. */
    dut->cs = op->kind != VIA_OP_IDLE;
    dut->we = op->kind == VIA_OP_WRITE;
    dut->rs = op->rs;
    dut->data_i = op->kind == VIA_OP_WRITE ? op->data : 0;
    dut->eval();
    uint8_t dut_data = dut->via_data;
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
    bool dut_irq = dut->via_irq != 0;

    if (op->kind == VIA_OP_READ && ref_data != dut_data)
    {
        snprintf(detail, cap, "cycle %llu: read $%X was %02X, ref %02X",
                 (unsigned long long)n, op->rs, dut_data, ref_data);
        return false;
    }
    if (ref_irq != dut_irq)
    {
        snprintf(detail, cap, "cycle %llu: irq was %d, ref %d",
                 (unsigned long long)n, dut_irq, ref_irq);
        return false;
    }
    return true;
}

static bool run_script(const via_op_t *ops, size_t n_ops, char *detail, size_t cap)
{
    reset_both();
    uint64_t n = 0;
    for (size_t i = 0; i < n_ops; i++)
        for (uint16_t r = 0; r <= ops[i].repeat; r++)
            if (!cycle(&ops[i], n++, detail, cap))
                return false;
    return true;
}

#define VIA_LOCKSTEP(name)                                          \
    UTEST(via, name)                                                \
    {                                                               \
        char detail[128] = "";                                      \
        bool ok = run_script(via_scen_##name, via_scen_##name##_n,  \
                             detail, sizeof detail);                \
        if (!ok)                                                    \
            printf("%s\n", detail);                                 \
        ASSERT_TRUE(ok);                                            \
    }

VIA_SCRIPTS(VIA_LOCKSTEP)

/* Deterministic fuzz: random register traffic with idle gaps. */
UTEST(via, fuzz)
{
    reset_both();
    char detail[128] = "";
    uint16_t lfsr = VIA_FUZZ_SEED;
    uint64_t n = 0;
    for (int i = 0; i < VIA_FUZZ_CYCLES; i++)
    {
        via_op_t op;
        via_fuzz_next(&lfsr, &op);
        if (!cycle(&op, n++, detail, sizeof detail))
        {
            printf("%s\n", detail);
            ASSERT_TRUE(false);
        }
    }
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vvia;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
