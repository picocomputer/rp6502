/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
    dut->clk = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
