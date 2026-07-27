/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The frame counter and the vsync interrupt, emulator semantics: $FFE3
 * increments once per frame, exactly 420,000 clocks apart; $FFF0 bit 7
 * enables the vsync IRQ, a read returns the pending sources and acks them
 * all. The program counts four frames by polling, then sleeps in WAI until
 * the interrupt delivers the fifth.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "utest.h"

#include <cstdint>
#include <string>
#include <vector>

static Vrp6502 *dut;

static void clock_cycle()
{
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_sys = 0;
    dut->eval();
}

static void machine_reset()
{
    dut->rst_n = 0;
    clock_cycle();
    clock_cycle();
    dut->rst_n = 1;
    dut->rootp->rp6502__DOT__cpu_run = 1;
}

UTEST(vsync, ffe3_counts_frames_and_fff0_interrupts)
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
    auto *r = dut->rootp;
    machine_reset();
    for (size_t i = 0; i < 0x10000; i++)
        r->rp6502__DOT__sram__DOT__mem[i] = 0;
    for (size_t i = 0; i < sizeof prog; i++)
        r->rp6502__DOT__sram__DOT__mem[0x0200 + i] = prog[i];
    r->rp6502__DOT__ria__DOT__regs[0x1C] = 0x00;
    r->rp6502__DOT__ria__DOT__regs[0x1D] = 0x02;
    r->rp6502__DOT__ria__DOT__regs[0x1E] = 0x20;
    r->rp6502__DOT__ria__DOT__regs[0x1F] = 0x02;

    std::string out;
    std::vector<uint64_t> at;
    for (uint64_t i = 0; i < 3000000; i++)
    {
        clock_cycle();
        if (dut->rp6502_tx_valid)
        {
            out.push_back((char)dut->rp6502_tx_data);
            at.push_back(i);
        }
        if (r->rp6502__DOT__cpu__DOT__stop_flag)
            break;
    }

    ASSERT_TRUE(r->rp6502__DOT__cpu__DOT__stop_flag);
    ASSERT_EQ(out.size(), (size_t)6);
    /* Four polled frames: consecutive counter values... */
    for (int i = 1; i < 4; i++)
        ASSERT_EQ((uint8_t)out[i], (uint8_t)((uint8_t)out[i - 1] + 1));
    /* ...exactly one frame apart, within the poll loop's jitter. */
    for (int i = 1; i < 4; i++)
    {
        int64_t delta = (int64_t)(at[i] - at[i - 1]);
        ASSERT_GT(delta, 420000 - 64);
        ASSERT_LT(delta, 420000 + 64);
    }
    /* The interrupt: the handler read $FFF0 pending (bit 7) and acked. */
    ASSERT_EQ((uint8_t)out[4], 0x80);
    ASSERT_EQ((uint8_t)out[5], 'Q');
}

UTEST(vsync, movable_line_keeps_the_cadence)
{
    /* The vsync line is the highest programmed scanline once a mode
     * shrinks the picture; moving it shifts the phase, never the rate. */
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
    auto *r = dut->rootp;
    machine_reset();
    r->rp6502__DOT__vid_prog__DOT__vsync_shadow = 240;
    for (size_t i = 0; i < 0x10000; i++)
        r->rp6502__DOT__sram__DOT__mem[i] = 0;
    for (size_t i = 0; i < sizeof prog; i++)
        r->rp6502__DOT__sram__DOT__mem[0x0200 + i] = prog[i];
    r->rp6502__DOT__ria__DOT__regs[0x1C] = 0x00;
    r->rp6502__DOT__ria__DOT__regs[0x1D] = 0x02;

    std::vector<uint64_t> at;
    for (uint64_t i = 0; i < 3000000; i++)
    {
        clock_cycle();
        if (dut->rp6502_tx_valid)
            at.push_back(i);
        if (r->rp6502__DOT__cpu__DOT__stop_flag)
            break;
    }
    ASSERT_TRUE(r->rp6502__DOT__cpu__DOT__stop_flag);
    ASSERT_EQ(at.size(), (size_t)4);
    for (int i = 1; i < 4; i++)
    {
        int64_t delta = (int64_t)(at[i] - at[i - 1]);
        ASSERT_GT(delta, 420000 - 64);
        ASSERT_LT(delta, 420000 + 64);
    }
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
