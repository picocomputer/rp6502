/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The merged OPL2 LUT ROM against the generated tables, both ports on
 * every cycle. The generator parses the vendor's case arms and checks
 * them against their documented formulas, so agreement here is
 * agreement with the submodule: what this proves is the ROM module
 * itself — the halves land where the ports look, the exp port truncates
 * to ten bits, and both reads register on the same edge.
 */

#include "Vopl2_lut_rom.h"
#include "verilated.h"

#include "opl2_lut_tables.h"
#include "utest.h"

UTEST_STATE();

static Vopl2_lut_rom *dut;

static void clock_cycle()
{
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
}

UTEST(oplrom, every_word_on_both_ports)
{
    /* Walk the halves against each other: one port ascending, the
     * other descending, so every cycle reads two unrelated words. */
    for (int i = 0; i < 256; i++)
    {
        dut->theta = i;
        dut->exp_in = 255 - i;
        clock_cycle();
        ASSERT_EQ(dut->log_sin_out, OPL2_LUT[i]);
        ASSERT_EQ(dut->exp_out, OPL2_LUT[256 + (255 - i)] & 0x3FF);
    }
}

UTEST(oplrom, same_index_both_ports)
{
    /* The same 8-bit index names different words per port. */
    for (int i = 0; i < 256; i++)
    {
        dut->theta = i;
        dut->exp_in = i;
        clock_cycle();
        ASSERT_EQ(dut->log_sin_out, OPL2_LUT[i]);
        ASSERT_EQ(dut->exp_out, OPL2_LUT[256 + i] & 0x3FF);
    }
}

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vopl2_lut_rom;
    /* Settle time zero first, so the initial load is not sharing an
     * eval with the first read's edge. */
    dut->clk = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
