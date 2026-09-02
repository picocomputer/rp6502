/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The VIA under test, when it is the fabric's — via.sv verilated.
 *
 * The register interface is the same one the machine drives, so the cycle
 * below is the RTL half of what used to be a lockstep against chips: assert
 * the bus, evaluate to get the read data the cycle presents, then clock.
 */

#include "via_dut.h"

#include "Vvia.h"

static Vvia *dut;

void via_dut_init(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vvia;
}

void via_dut_free(void)
{
    dut->final();
    delete dut;
    dut = nullptr;
}

void via_reset(void)
{
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

void via_step(const via_op_t *op, uint8_t *data, bool *irq)
{
    dut->cs = op->kind != VIA_OP_IDLE;
    dut->we = op->kind == VIA_OP_WRITE;
    dut->rs = op->rs;
    dut->data_i = op->kind == VIA_OP_WRITE ? op->data : 0;
    dut->eval();
    *data = dut->via_data;
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
    *irq = dut->via_irq != 0;
}
