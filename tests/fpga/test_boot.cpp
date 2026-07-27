/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The two processors meet: the soft CPU's firmware loads a 6502 program
 * through its window on the machine, writes the vectors, releases the
 * reset, and the 6502 prints — through the TX ring the firmware drains, so
 * the console output arrives on the OS side the way com.c will see it. No
 * testbench backdoor into the 6502's world — only the firmware image goes
 * in, the way a bitstream carries it.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>

static Vrp6502 *dut;

static bool load_firmware(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    auto &tcm = dut->rootp->rp6502__DOT__rv__DOT__tcm;
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

UTEST(boot, firmware_boots_the_6502)
{
    ASSERT_TRUE(load_firmware(SW_BIN));

    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
    {
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
    }
    dut->rst_n = 1;

    /* Typed input arrives through the platform keyboard slot, one offered
     * byte the firmware's com_task moves into its rings. The first line
     * feeds the program's raw echo loop; the second is typed only after
     * the echo completes, into the genuine std/rln stdin engine. */
    const char typed[] = "HI\r";
    const char typed2[] = "OK\r";
    size_t typed_at = 0, typed2_at = 0;

    std::string rv_out, cpu_out;
    bool stopped = false;
    for (int i = 0; i < 4000000; i++)
    {
        if (!dut->rootp->rp6502__DOT__rv__DOT__mmio_kbd_valid)
        {
            if (typed[typed_at])
            {
                dut->rootp->rp6502__DOT__rv__DOT__mmio_kbd_data = typed[typed_at++];
                dut->rootp->rp6502__DOT__rv__DOT__mmio_kbd_valid = 1;
            }
            else if (typed2[typed2_at] && cpu_out.find("HI\r") != std::string::npos)
            {
                dut->rootp->rp6502__DOT__rv__DOT__mmio_kbd_data = typed2[typed2_at++];
                dut->rootp->rp6502__DOT__rv__DOT__mmio_kbd_valid = 1;
            }
        }
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
        if (dut->rp6502_rv_tx_valid)
            rv_out.push_back((char)dut->rp6502_rv_tx_data);
        if (dut->rp6502_tx_valid)
            cpu_out.push_back((char)dut->rp6502_tx_data);
        stopped = dut->rootp->rp6502__DOT__cpu__DOT__stop_flag != 0;
        if (dut->rp6502_rv_halted && stopped)
            break;
    }

    ASSERT_TRUE(dut->rp6502_rv_halted);
    ASSERT_EQ(dut->rp6502_rv_exit_code, 0u);
    ASSERT_TRUE(stopped);
    /* CA is the machine's first real syscall: api.c answered $4143 through
     * the trampoline, and the 6502 printed A then X. DB is the second, the
     * uint16 $4243 pushed on the xstack coming back incremented. HI\r is
     * the keyboard echoed through com.c's rings and the $FFE2 offer. OK\n
     * is a whole line of stdin through the genuine std.c/rln.c engine,
     * handed back on the xstack. */
    ASSERT_STREQ(cpu_out.c_str(), "HELLO, WORLD!\r\nCADBHI\rOK\n");
    /* The machine's stream reaches the OS console through the manifold —
     * whole up to the stdin phase, where rln's own echo and ANSI cursor
     * traffic join the merge. */
    ASSERT_TRUE(strstr(rv_out.c_str(), "boot: loading") != NULL);
    ASSERT_TRUE(strstr(rv_out.c_str(), "boot: running") != NULL);
    ASSERT_TRUE(strstr(rv_out.c_str(), "HELLO, WORLD!\r\nCADBHI\r") != NULL);
    ASSERT_TRUE(strstr(rv_out.c_str(), "OK") != NULL);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
