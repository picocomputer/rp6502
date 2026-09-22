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

double sc_time_stamp() { return (double)tb_core_time; }

/* clk_rv is half the rate of clk_sys and rises in the same eval, as the PLL
 * in pocket_pll.v makes it. A clk_rv registered off clk_sys would rise after
 * the clk_sys registers had changed, so the soft CPU would capture their new
 * values instead of the ones they held before the edge. */
static int tb_core_rv_phase;

static void tb_core_edge(int level)
{
    if (level)
        tb_core_rv_phase = !tb_core_rv_phase;
    tb_core_dut->clk_sys = level;
    tb_core_dut->clk_mach = level;
    tb_core_dut->clk_rv = level && tb_core_rv_phase;
    tb_core_dut->clk_a2 = 0;
    tb_core_dut->eval();
    if (tb_core_trace)
        tb_core_trace->dump(tb_core_time);
    tb_core_time++;
    /* XRAM's port clock rises twice in a machine clock, after each of
     * its edges; clk_ph tells the port which rise is which. */
    if (!level)
        for (int ph = 0; ph < 2; ph++)
        {
            tb_core_dut->clk_ph = ph;
            tb_core_dut->clk_a2 = 1;
            tb_core_dut->eval();
            if (tb_core_trace)
                tb_core_trace->dump(tb_core_time);
            tb_core_time++;
            tb_core_dut->clk_a2 = 0;
            tb_core_dut->eval();
        }
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
    tb_core_dut->clk_a2 = 0;
    tb_core_dut->clk_ph = 0;
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
