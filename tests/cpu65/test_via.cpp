/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * via.sv against chips/m6522.h in lockstep, driven exactly as the machine
 * drives its VIA: register reads and writes with the ports unwired. Every
 * cycle's read data and IRQ line must match, through the timer pipelines,
 * the IFR/IER rules and the PB7 toggle.
 */

#define CHIPS_IMPL
#include "chips/chips/m6522.h"

#include "Vvia.h"
#include "utest.h"

#include <cstdio>

static m6522_t ref;
static Vvia *dut;

typedef enum
{
    OP_IDLE,
    OP_READ,
    OP_WRITE,
} op_kind_t;

typedef struct
{
    op_kind_t kind;
    uint8_t rs;
    uint8_t data;
    uint16_t repeat; /* extra cycles of the same op; idle mostly */
} op_t;

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
static bool cycle(const op_t *op, uint64_t n, char *detail, size_t cap)
{
    /* The reference, wired the way emu/emu/via.c wires it. */
    uint64_t pins = 0;
    if (op->kind != OP_IDLE)
    {
        pins |= op->rs & M6522_RS_PINS;
        pins |= M6522_CS1;
        if (op->kind == OP_READ)
            pins |= M6522_RW;
        else
            M6522_SET_DATA(pins, op->data);
    }
    pins = m6522_tick(&ref, pins);
    uint8_t ref_data = M6522_GET_DATA(pins);
    bool ref_irq = (pins & M6522_IRQ) != 0;

    /* The RTL. */
    dut->cs = op->kind != OP_IDLE;
    dut->we = op->kind == OP_WRITE;
    dut->rs = op->rs;
    dut->data_i = op->kind == OP_WRITE ? op->data : 0;
    dut->eval();
    uint8_t dut_data = dut->via_data;
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
    bool dut_irq = dut->via_irq != 0;

    if (op->kind == OP_READ && ref_data != dut_data)
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

static bool run_script(const op_t *ops, size_t n_ops, char *detail, size_t cap)
{
    reset_both();
    uint64_t n = 0;
    for (size_t i = 0; i < n_ops; i++)
        for (uint16_t r = 0; r <= ops[i].repeat; r++)
            if (!cycle(&ops[i], n++, detail, cap))
                return false;
    return true;
}

#define VIA_LOCKSTEP(name, ...)                                     \
    UTEST(via, name)                                                \
    {                                                               \
        static const op_t ops[] = __VA_ARGS__;                      \
        char detail[128] = "";                                      \
        bool ok = run_script(ops, sizeof ops / sizeof *ops,         \
                             detail, sizeof detail);                \
        if (!ok)                                                    \
            printf("%s\n", detail);                                 \
        ASSERT_TRUE(ok);                                            \
    }

/* T1 one-shot: interrupt once, cleared by reading T1CL. */
VIA_LOCKSTEP(t1_oneshot, {
    {OP_WRITE, 0xE, 0xC0, 0},  /* IER: enable T1 */
    {OP_WRITE, 0x4, 10, 0},    /* T1 latch low */
    {OP_WRITE, 0x5, 0, 0},     /* T1 go */
    {OP_IDLE, 0, 0, 40},
    {OP_READ, 0xD, 0, 0},      /* IFR */
    {OP_READ, 0x4, 0, 0},      /* T1CL: clears */
    {OP_IDLE, 0, 0, 40},
    {OP_READ, 0xD, 0, 0},
})

/* T1 continuous: interrupt on every underflow. */
VIA_LOCKSTEP(t1_continuous, {
    {OP_WRITE, 0xB, 0x40, 0},  /* ACR: continuous */
    {OP_WRITE, 0xE, 0xC0, 0},
    {OP_WRITE, 0x4, 6, 0},
    {OP_WRITE, 0x5, 0, 0},
    {OP_IDLE, 0, 0, 30},
    {OP_READ, 0x4, 0, 0},
    {OP_IDLE, 0, 0, 30},
    {OP_READ, 0xD, 0, 0},
})

/* T1 drives PB7 when ACR bit 7 is set. */
VIA_LOCKSTEP(t1_pb7, {
    {OP_WRITE, 0x2, 0x80, 0},  /* DDRB: PB7 out */
    {OP_WRITE, 0xB, 0xC0, 0},  /* ACR: continuous + PB7 */
    {OP_WRITE, 0x4, 4, 0},
    {OP_WRITE, 0x5, 0, 0},
    {OP_IDLE, 0, 0, 3},
    {OP_READ, 0x0, 0, 0},
    {OP_IDLE, 0, 0, 3},
    {OP_READ, 0x0, 0, 0},
    {OP_IDLE, 0, 0, 3},
    {OP_READ, 0x0, 0, 0},
    {OP_IDLE, 0, 0, 3},
    {OP_READ, 0x0, 0, 0},
})

/* T2 one-shot, cleared by reading T2CL; no reload from latch. */
VIA_LOCKSTEP(t2_oneshot, {
    {OP_WRITE, 0xE, 0xA0, 0},  /* IER: enable T2 */
    {OP_WRITE, 0x8, 8, 0},
    {OP_WRITE, 0x9, 0, 0},
    {OP_IDLE, 0, 0, 40},
    {OP_READ, 0xD, 0, 0},
    {OP_READ, 0x8, 0, 0},
    {OP_IDLE, 0, 0, 40},
    {OP_READ, 0xD, 0, 0},
})

/* The PB6 count-mode quirk: with PB6 driven high, the reference sees a
 * falling edge against its zeroed port inputs every cycle. */
VIA_LOCKSTEP(t2_pb6_quirk, {
    {OP_WRITE, 0x2, 0x40, 0},  /* DDRB: PB6 out */
    {OP_WRITE, 0x0, 0x40, 0},  /* ORB: PB6 high */
    {OP_WRITE, 0xB, 0x20, 0},  /* ACR: T2 counts PB6 */
    {OP_WRITE, 0xE, 0xA0, 0},
    {OP_WRITE, 0x8, 5, 0},
    {OP_WRITE, 0x9, 0, 0},
    {OP_IDLE, 0, 0, 20},
    {OP_READ, 0xD, 0, 0},
    {OP_READ, 0x8, 0, 0},
})

/* IER set/clear addressing, IFR write-to-clear, bit 7 readback. */
VIA_LOCKSTEP(ifr_ier, {
    {OP_WRITE, 0xE, 0xE0, 0},  /* set T1+T2 enables */
    {OP_READ, 0xE, 0, 0},
    {OP_WRITE, 0xE, 0x40, 0},  /* clear T1 enable */
    {OP_READ, 0xE, 0, 0},
    {OP_WRITE, 0x4, 4, 0},
    {OP_WRITE, 0x5, 0, 0},
    {OP_IDLE, 0, 0, 20},
    {OP_READ, 0xD, 0, 0},      /* T1 flag, no master (masked) */
    {OP_WRITE, 0xD, 0x7F, 0},  /* clear all flags */
    {OP_READ, 0xD, 0, 0},
})

VIA_LOCKSTEP(readback_all, {
    {OP_WRITE, 0x0, 0xAA, 0}, {OP_WRITE, 0x1, 0x55, 0},
    {OP_WRITE, 0x2, 0xF0, 0}, {OP_WRITE, 0x3, 0x0F, 0},
    {OP_WRITE, 0xB, 0x00, 0}, {OP_WRITE, 0xC, 0x21, 0},
    {OP_READ, 0x0, 0, 0}, {OP_READ, 0x1, 0, 0},
    {OP_READ, 0x2, 0, 0}, {OP_READ, 0x3, 0, 0},
    {OP_READ, 0x6, 0, 0}, {OP_READ, 0x7, 0, 0},
    {OP_READ, 0xA, 0, 0}, {OP_READ, 0xB, 0, 0},
    {OP_READ, 0xC, 0, 0}, {OP_READ, 0xF, 0, 0},
})

/* Deterministic fuzz: random register traffic with idle gaps. */
UTEST(via, fuzz)
{
    reset_both();
    char detail[128] = "";
    uint16_t lfsr = 0xBEEF;
    uint64_t n = 0;
    for (int i = 0; i < 30000; i++)
    {
        lfsr = (uint16_t)((lfsr >> 1) ^ (-(int)(lfsr & 1) & 0xB400));
        op_t op = {OP_IDLE, 0, 0, 0};
        if ((lfsr & 0x0F) < 3)
            op = (op_t){OP_WRITE, (uint8_t)((lfsr >> 4) & 0x0F),
                        (uint8_t)(lfsr >> 8), 0};
        else if ((lfsr & 0x0F) < 6)
            op = (op_t){OP_READ, (uint8_t)((lfsr >> 4) & 0x0F), 0, 0};
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
