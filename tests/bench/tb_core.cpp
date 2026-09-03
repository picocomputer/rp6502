/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tb_core.h"

#include "Vwiring.h"
#include "Vwiring___024root.h"
#include "verilated.h"
#include "verilated_fst_c.h"

#include <cstdlib>

static Vwiring *tb_core_dut;
static VerilatedFstC *tb_core_trace;
static uint64_t tb_core_time;

/* Verilator drives $time from this. */
double sc_time_stamp() { return (double)tb_core_time; }

/* The soft CPU's clock is half the machine's and rises with it, the way
 * the PLL makes it. Driving it as a divider registered off clk_sys
 * would put its edge after the machine's own — which is the bug that
 * kept the soft CPU at full rate, so the model has to be honest. */
static int tb_core_rv_phase;

static void tb_core_edge(int level)
{
    if (level)
        tb_core_rv_phase = !tb_core_rv_phase;
    tb_core_dut->clk_sys = level;
    /* The machine's gated clock. Nothing here ever stops it -- these
     * tests never save -- but the pin exists and a machine with no
     * clock renders nothing. */
    tb_core_dut->clk_mach = level;
    tb_core_dut->clk_rv = level && tb_core_rv_phase;
    tb_core_dut->eval();
    if (tb_core_trace)
        tb_core_trace->dump(tb_core_time);
    tb_core_time++;
}

void tb_core_args(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
}

void tb_core_init()
{
    tb_core_dut = new Vwiring;
    tb_core_time = 0;

    if (const char *path = getenv("RP6502_RTL_TRACE"))
    {
        Verilated::traceEverOn(true);
        tb_core_trace = new VerilatedFstC;
        tb_core_dut->trace(tb_core_trace, 99);
        tb_core_trace->open(path);
    }

    tb_core_rv_phase = 0;
    tb_core_dut->clk_sys = 0;
    tb_core_dut->clk_mach = 0;
    tb_core_dut->clk_rv = 0;
    tb_core_dut->rst_n = 0;
    tb_core_dut->mach_running = 1;
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
    return tb_core_dut->wiring_scanline;
}

uint16_t tb_core_h()
{
    return tb_core_dut->rootp->wiring__DOT__vid_h;
}

bool tb_core_hsync()
{
    return tb_core_dut->rootp->wiring__DOT__vid_hsync;
}

bool tb_core_vsync()
{
    return tb_core_dut->rootp->wiring__DOT__vid_vsync;
}

bool tb_core_de()
{
    return tb_core_dut->rootp->wiring__DOT__vid_de;
}
