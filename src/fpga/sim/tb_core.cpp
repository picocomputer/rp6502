/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tb_core.h"

#include "Vrp6502.h"
#include "verilated.h"
#include "verilated_fst_c.h"

#include <cstdlib>

static Vrp6502 *tb_core_dut;
static VerilatedFstC *tb_core_trace;
static uint64_t tb_core_time;

/* Verilator drives $time from this. */
double sc_time_stamp() { return (double)tb_core_time; }

static void tb_core_edge(int level)
{
    tb_core_dut->clk_sys = level;
    tb_core_dut->eval();
    if (tb_core_trace)
        tb_core_trace->dump(tb_core_time);
    tb_core_time++;
}

void tb_core_init()
{
    tb_core_dut = new Vrp6502;
    tb_core_time = 0;

    if (const char *path = getenv("RP6502_FPGA_TRACE"))
    {
        Verilated::traceEverOn(true);
        tb_core_trace = new VerilatedFstC;
        tb_core_dut->trace(tb_core_trace, 99);
        tb_core_trace->open(path);
    }

    tb_core_dut->clk_sys = 0;
    tb_core_dut->rst_n = 0;
    tb_core_dut->eval();
}

void tb_core_free()
{
    if (tb_core_trace)
    {
        tb_core_trace->close();
        delete tb_core_trace;
        tb_core_trace = nullptr;
    }
    tb_core_dut->final();
    delete tb_core_dut;
    tb_core_dut = nullptr;
}

void tb_core_reset()
{
    tb_core_dut->rst_n = 0;
    tb_core_clocks(4);
    tb_core_dut->rst_n = 1;
    tb_core_dut->eval();
}

void tb_core_clocks(int count)
{
    for (int i = 0; i < count; i++)
    {
        tb_core_edge(1);
        tb_core_edge(0);
    }
}

uint16_t tb_core_scanline()
{
    return tb_core_dut->rp6502_scanline;
}
