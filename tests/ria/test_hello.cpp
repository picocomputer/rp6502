/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine boots and a program talks through the RIA's bare UART: hello
 * out of $FFE1 under the $FFE0 ready bit, and an echo pulled in through the
 * $FFE2 latch. Programs load the way the RIA loads them — bytes into RAM,
 * a reset vector into the register cells, then release the reset.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "utest.h"

#include <cstring>
#include <string>

static Vrp6502 *dut;
/* Half clk_sys, rising with it: the PLL's shape, not a divider's. */
static bool rv_phase;

static void clock_cycle()
{
    rv_phase = !rv_phase;
    dut->clk_rv = rv_phase;
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_rv = 0;
    dut->clk_sys = 0;
    dut->eval();
}

static void machine_reset()
{
    dut->rst_n = 0;
    clock_cycle();
    clock_cycle();
    dut->rst_n = 1;
    /* These tests bypass the firmware boot; run the 6502 directly. */
    dut->rootp->rp6502__DOT__cpu_run = 1;
}

/* Load bytes into SRAM and point the reset vector at entry. */
static void load(uint16_t org, const uint8_t *bytes, size_t n, uint16_t entry)
{
    auto *r = dut->rootp;
    for (size_t i = 0; i < 0x10000; i++)
        r->rp6502__DOT__sram__DOT__mem[i] = 0;
    for (size_t i = 0; i < n; i++)
        r->rp6502__DOT__sram__DOT__mem[org + i] = bytes[i];
    r->rp6502__DOT__ria__DOT__regs[0x1C] = entry & 0xFF;
    r->rp6502__DOT__ria__DOT__regs[0x1D] = entry >> 8;
}

/* Run until STP or the budget runs out, collecting TX bytes. */
static std::string run(uint64_t max_clocks)
{
    std::string out;
    for (uint64_t i = 0; i < max_clocks; i++)
    {
        clock_cycle();
        if (dut->rp6502_tx_valid)
            out.push_back((char)dut->rp6502_tx_data);
        if (dut->rootp->rp6502__DOT__cpu__DOT__stop_flag)
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
    ASSERT_TRUE(dut->rootp->rp6502__DOT__cpu__DOT__stop_flag);
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
        clock_cycle();
        if (dut->rp6502_rx_taken)
        {
            taken = true;
            dut->rx_valid = 0;
        }
        if (dut->rp6502_tx_valid)
            out.push_back((char)dut->rp6502_tx_data);
        if (dut->rootp->rp6502__DOT__cpu__DOT__stop_flag)
            break;
    }
    ASSERT_TRUE(taken);
    ASSERT_TRUE(dut->rootp->rp6502__DOT__cpu__DOT__stop_flag);
    ASSERT_STREQ(out.c_str(), "Q");
}

/* Ready may only lie in one direction. When $FFE0 reports the transmitter
 * ready, a byte written to $FFE1 has to leave; that is the whole contract
 * and the only failure that can hurt a program.
 *
 * What it deliberately does not assert is *when* the bit becomes visible.
 * The RIA updates the cell in the handler that runs after the 6502's read,
 * so its first cold read is clear; the fabric computes the flags during the
 * read, so its first read already carries them. Both are honest — neither
 * ever claims ready when it is not — and a test that pinned either shape
 * would be enforcing a platform's quirk rather than finding a problem. An
 * earlier version did exactly that and failed the moment the fabric stopped
 * seeding the cell from reset.
 *
 * That ready does eventually set is worth testing too, and
 * prints_through_the_uart already does it the way a 6502 must: bit $FFE0 /
 * bpl wait before every store, throwing away as many reads as it takes. If
 * the bit never set, that test would spin out its budget and fail. So the
 * pair is safety here and liveness there, and a test that wants to assert
 * ready becomes set should retry rather than read once. */
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
    ASSERT_TRUE(dut->rootp->rp6502__DOT__cpu__DOT__stop_flag);

    uint8_t flags = dut->rootp->rp6502__DOT__sram__DOT__mem[0];
    if (flags & 0x80)
        ASSERT_TRUE(out.find('Z') != std::string::npos);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    /* Verilator seeds its edge detectors from the first eval, so a model
     * first evaluated with a clock already high never sees that edge.
     * clk_rv rises once in the two cycles reset is held, and losing it
     * loses the soft CPU's asynchronous reset with it. */
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
