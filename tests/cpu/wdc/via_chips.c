/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The VIA under test, when it is the emulator's — chips/chips/m6522.h, which
 * is someone else's code this machine depends on.
 *
 * The wiring is core/wdc/via.c's, which is what this reproduces: CS1 asserted,
 * ports unwired, RW high to read.
 */

#include "via_dut.h"

#define CHIPS_IMPL
#include "chips/chips/m6522.h"

static m6522_t via;

void via_dut_init(int argc, const char *const argv[])
{
    (void)argc;
    (void)argv;
}

void via_dut_free(void)
{
}

void via_reset(void)
{
    m6522_init(&via);
}

void via_step(const via_op_t *op, uint8_t *data, bool *irq)
{
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
    pins = m6522_tick(&via, pins);
    *data = M6522_GET_DATA(pins);
    *irq = (pins & M6522_IRQ) != 0;
}
