/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The VIA under test, when it is the fabric's — w65c22.sv verilated.
 *
 * The register interface is the same one the machine drives, so the cycle
 * below is the RTL half of what used to be a lockstep against chips: assert
 * the bus, evaluate to get the read data the cycle presents, then clock.
 */

#include "via_dut.h"

#include "Vw65c22.h"

static Vw65c22 *dut;

void via_dut_init(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vw65c22;
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

void via_step(const w65c22_op_t *op, uint8_t *data, bool *irq)
{
    dut->cs = op->kind != W65C22_OP_IDLE;
    dut->we = op->kind == W65C22_OP_WRITE;
    dut->rs = op->rs;
    dut->data_i = op->kind == W65C22_OP_WRITE ? op->data : 0;
    dut->eval();
    *data = dut->w65c22_data;
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
    *irq = dut->w65c22_irq != 0;
}
