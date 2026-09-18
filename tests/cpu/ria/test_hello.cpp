/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "utest.h"

#include <cstring>
#include <string>

static Vwiring *dut;

/* rst_n does not reset resb, which drives the 6502's reset, so each case
 * builds a new model instead of reusing the last one. */
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

/* Register cells 0x1C and 0x1D are the reset vector at $FFFC. */
static void load(uint16_t org, const uint8_t *bytes, size_t n, uint16_t entry)
{
    auto *r = dut->rootp;
    for (size_t i = 0; i < 0x10000; i++)
        r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[i] = 0;
    for (size_t i = 0; i < n; i++)
        r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[org + i] = bytes[i];
    r->wiring__DOT__ria__DOT__regs[0x1C] = entry & 0xFF;
    r->wiring__DOT__ria__DOT__regs[0x1D] = entry >> 8;
}

static std::string run(uint64_t max_clocks)
{
    std::string out;
    for (uint64_t i = 0; i < max_clocks; i++)
    {
        tb_clock(dut);
        if (dut->wiring_tx_valid)
            out.push_back((char)dut->wiring_tx_data);
        if (dut->rootp->wiring__DOT__cpu__DOT__stop_flag)
            break;
    }
    return out;
}

UTEST(hello, prints_through_the_uart)
{
    /* ldx #0; loop: lda msg,x; beq done; wait: bit $FFE0; bpl wait;
     * sta $FFE1; inx; bne loop; nop; done: stp; msg: "HELLO, WORLD!\r\n" */
    static const uint8_t prog[] = {
        0xA2, 0x00,
        0xBD, 0x14, 0x02,
        0xF0, 0x0C,
        0x2C, 0xE0, 0xFF,
        0x10, 0xFB,
        0x8D, 0xE1, 0xFF,
        0xE8,
        0xD0, 0xF0,
        0xEA,
        0xDB,
        'H', 'E', 'L', 'L', 'O', ',', ' ',
        'W', 'O', 'R', 'L', 'D', '!', '\r', '\n', 0,
    };
    machine_reset();
    load(0x0200, prog, sizeof prog, 0x0200);
    std::string out = run(100000);
    ASSERT_TRUE(dut->rootp->wiring__DOT__cpu__DOT__stop_flag);
    ASSERT_STREQ(out.c_str(), "HELLO, WORLD!\r\n");
}

UTEST(hello, echoes_through_the_latch)
{
    /* wait: bit $FFE0; bvc wait; lda $FFE2; sta $FFE1; stp */
    static const uint8_t prog[] = {
        0x2C, 0xE0, 0xFF,
        0x50, 0xFB,
        0xAD, 0xE2, 0xFF,
        0x8D, 0xE1, 0xFF,
        0xDB,
    };
    machine_reset();
    load(0x0200, prog, sizeof prog, 0x0200);

    dut->rx_valid = 1;
    dut->rx_data = 'Q';
    std::string out;
    bool taken = false;
    for (int i = 0; i < 100000; i++)
    {
        tb_clock(dut);
        if (dut->wiring_rx_taken)
        {
            taken = true;
            dut->rx_valid = 0;
        }
        if (dut->wiring_tx_valid)
            out.push_back((char)dut->wiring_tx_data);
        if (dut->rootp->wiring__DOT__cpu__DOT__stop_flag)
            break;
    }
    ASSERT_TRUE(taken);
    ASSERT_TRUE(dut->rootp->wiring__DOT__cpu__DOT__stop_flag);
    ASSERT_STREQ(out.c_str(), "Q");
}

/* The first read of $FFE0 is not required to show the transmitter ready,
 * because the Pico RIA updates that bit after the read has returned, while
 * the FPGA computes it during the read. */
UTEST(hello, ready_never_claims_more_than_it_can_do)
{
    /* lda $FFE0; sta $00; lda #'Z'; sta $FFE1; stp */
    static const uint8_t prog[] = {
        0xAD, 0xE0, 0xFF,
        0x85, 0x00,
        0xA9, 0x5A,
        0x8D, 0xE1, 0xFF,
        0xDB,
    };
    machine_reset();
    load(0x0200, prog, sizeof prog, 0x0200);
    std::string out = run(100000);
    ASSERT_TRUE(dut->rootp->wiring__DOT__cpu__DOT__stop_flag);

    uint8_t flags = dut->rootp->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[0];
    if (flags & 0x80)
        ASSERT_TRUE(out.find('Z') != std::string::npos);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vwiring;
    /* Verilator seeds its edge detectors from the first eval, so a clock that
     * is already high at the first eval has no rising edge. machine_reset
     * builds each case's model with the same first eval, and clk_rv rises
     * once in the two cycles that machine_reset holds rst_n low. Without that
     * edge the soft CPU's asynchronous reset is never applied, because rst_n
     * is already low at the first eval and so has no falling edge. */
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
