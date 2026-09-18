/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * $FFE3 increments once a frame, which is 840,000 clocks. Writing bit 7 of
 * $FFF0 enables the vsync interrupt, and a read of $FFF0 returns the pending
 * sources and acknowledges all of them.
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "utest.h"

#include <cstdint>
#include <string>
#include <vector>

static Vwiring *dut;

static void machine_reset()
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vwiring;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->eval();
    tb_clock(dut);
    tb_clock(dut);
    dut->rst_n = 1;
    dut->rootp->wiring__DOT__resb = 1;
}

UTEST(vsync_rtl, ffe3_counts_frames_and_fff0_interrupts)
{
    /* ldy #4; ldx $FFE3; w: cpx $FFE3; beq w; ldx $FFE3; stx $FFE1; dey;
     * bne w; lda #$80; sta $FFF0; cli; wai; lda #'Q'; sta $FFE1; stp;
     * irq: pha; lda $FFF0; sta $FFE1; pla; rti */
    static const uint8_t prog[] = {
        0xA0, 0x04,
        0xAE, 0xE3, 0xFF,
        0xEC, 0xE3, 0xFF, /* w at $0205 */
        0xF0, 0xFB,
        0xAE, 0xE3, 0xFF,
        0x8E, 0xE1, 0xFF,
        0x88,
        0xD0, 0xF2,
        0xA9, 0x80,
        0x8D, 0xF0, 0xFF,
        0x58,
        0xCB,
        0xA9, 0x51,
        0x8D, 0xE1, 0xFF,
        0xDB,
        0x48,             /* irq at $0220 */
        0xAD, 0xF0, 0xFF,
        0x8D, 0xE1, 0xFF,
        0x68,
        0x40,
    };
    machine_reset();
    auto *r = dut->rootp;
    for (size_t i = 0; i < 0x10000; i++)
        r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[i] = 0;
    for (size_t i = 0; i < sizeof prog; i++)
        r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[0x0200 + i] = prog[i];
    r->wiring__DOT__ria__DOT__regs[0x1C] = 0x00;
    r->wiring__DOT__ria__DOT__regs[0x1D] = 0x02;
    r->wiring__DOT__ria__DOT__regs[0x1E] = 0x20;
    r->wiring__DOT__ria__DOT__regs[0x1F] = 0x02;

    std::string out;
    std::vector<uint64_t> at;
    for (uint64_t i = 0; i < 16000000; i++)
    {
        tb_clock(dut);
        if (dut->wiring_tx_valid)
        {
            out.push_back((char)dut->wiring_tx_data);
            at.push_back(i);
        }
        if (r->wiring__DOT__cpu__DOT__stop_flag)
            break;
    }

    ASSERT_TRUE(r->wiring__DOT__cpu__DOT__stop_flag);
    ASSERT_EQ(out.size(), (size_t)6);
    for (int i = 1; i < 4; i++)
        ASSERT_EQ((uint8_t)out[i], (uint8_t)((uint8_t)out[i - 1] + 1));
    for (int i = 1; i < 4; i++)
    {
        int64_t delta = (int64_t)(at[i] - at[i - 1]);
        ASSERT_GT(delta, 840000 - 256);
        ASSERT_LT(delta, 840000 + 256);
    }
    ASSERT_EQ((uint8_t)out[4], 0x80);
    ASSERT_EQ((uint8_t)out[5], 'Q');
}

UTEST(vsync_rtl, movable_line_keeps_the_cadence)
{
    static const uint8_t prog[] = {
        0xA0, 0x04,
        0xAE, 0xE3, 0xFF,
        0xEC, 0xE3, 0xFF,
        0xF0, 0xFB,
        0xAE, 0xE3, 0xFF,
        0x8E, 0xE1, 0xFF,
        0x88,
        0xD0, 0xF2,
        0xDB,
    };
    machine_reset();
    auto *r = dut->rootp;
    r->wiring__DOT__prog__DOT__vsync_shadow = 240;
    for (size_t i = 0; i < 0x10000; i++)
        r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[i] = 0;
    for (size_t i = 0; i < sizeof prog; i++)
        r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[0x0200 + i] = prog[i];
    r->wiring__DOT__ria__DOT__regs[0x1C] = 0x00;
    r->wiring__DOT__ria__DOT__regs[0x1D] = 0x02;

    std::vector<uint64_t> at;
    for (uint64_t i = 0; i < 16000000; i++)
    {
        tb_clock(dut);
        if (dut->wiring_tx_valid)
            at.push_back(i);
        if (r->wiring__DOT__cpu__DOT__stop_flag)
            break;
    }
    ASSERT_TRUE(r->wiring__DOT__cpu__DOT__stop_flag);
    ASSERT_EQ(at.size(), (size_t)4);
    /* vsync_shadow is copied to vsync_q only at frame start, so the first
     * interval is short by the 240 lines the vsync line moved up. Only the
     * intervals after it are a full frame. */
    for (int i = 2; i < 4; i++)
    {
        int64_t delta = (int64_t)(at[i] - at[i - 1]);
        ASSERT_GT(delta, 840000 - 256);
        ASSERT_LT(delta, 840000 + 256);
    }
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vwiring;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
