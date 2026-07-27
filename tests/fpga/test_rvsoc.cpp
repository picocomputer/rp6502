/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU exists: Hazard3 boots the cross-compiled firmware out of its
 * TCM, talks through the console register, and reports main's return value
 * through the halt register.
 */

#include "Vrvsoc.h"
#include "Vrvsoc___024root.h"

#include "utest.h"

#include <cstdio>
#include <string>

static Vrvsoc *dut;

static bool load_tcm(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    auto &tcm = dut->rootp->rv_soc__DOT__tcm;
    for (size_t i = 0; i < 32768; i++)
        tcm[i] = 0;
    uint8_t buf[4];
    size_t word = 0, n;
    while ((n = fread(buf, 1, 4, f)) > 0 && word < 32768)
    {
        uint32_t v = 0;
        for (size_t i = 0; i < n; i++)
            v |= (uint32_t)buf[i] << (8 * i);
        tcm[word++] = v;
    }
    fclose(f);
    return true;
}

UTEST(rvsoc, boots_and_prints)
{
    ASSERT_TRUE(load_tcm(SW_BIN));

    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
    {
        dut->clk = 1;
        dut->eval();
        dut->clk = 0;
        dut->eval();
    }
    dut->rst_n = 1;

    std::string out;
    bool halted = false;
    for (int i = 0; i < 200000 && !halted; i++)
    {
        dut->clk = 1;
        dut->eval();
        dut->clk = 0;
        dut->eval();
        if (dut->rv_soc_tx_valid)
            out.push_back((char)dut->rv_soc_tx_data);
        halted = dut->rv_soc_halted;
    }
    ASSERT_TRUE(halted);
    ASSERT_EQ(dut->rv_soc_exit_code, 0u);
    ASSERT_STREQ(out.c_str(), "hazard3: hello\n");
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrvsoc;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
