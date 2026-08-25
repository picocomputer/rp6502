/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the xreg program leaves behind in the fabric.
 *
 * It boots the same probe program tests/cpu/ria runs, where the console it
 * prints is checked on both machines. Everything asserted here is a register
 * no console can carry and no emulator has: the bell that struck once, the
 * pointers the two audio devices took through the soft CPU's validation, and
 * the scanline program the video path is holding.
 *
 * Reaching those registers is the whole path — the xreg dispatch, the soft
 * CPU's check, the MMIO write and the machine's decode of it — so a break
 * anywhere along it lands here rather than in a picture that happens to
 * still look right.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "tb_machine.h"
#include "utest.h"
#include "xreg_prog.h"

#include <vector>

static Vrp6502 *dut;

UTEST(xregs, the_program_reaches_the_devices)
{
    std::vector<uint8_t> rom;
    xreg_rom(rom);

    int strikes = 0;
    bool bel_gate_prev = false;
    ASSERT_TRUE(tb_boot_each(dut, rom, nullptr, [&] {
        /* The bell is a voice of the PSG now and the soft CPU rings it by
         * writing the voice's gate, so a strike is that bit going up. */
        const bool bg =
            (dut->rootp->rp6502__DOT__aud_psg__DOT__bel_hi >> 16) & 1;
        if (!bel_gate_prev && bg)
            strikes++;
        bel_gate_prev = bg;
    }));

    /* The muted BEL never struck; the unmuted one did. */
    ASSERT_EQ(strikes, 1);

    /* The PSG took 0x8000 and the OPL took 0xF000 after it. Setting up
     * either engine resets the other, so the PSG's pointer is parked. */
    ASSERT_EQ(dut->rootp->rp6502__DOT__aud_psg__DOT__xaddr, 0xFFFF);
    ASSERT_TRUE(dut->rootp->rp6502__DOT__aud_opl__DOT__enabled);
    ASSERT_EQ(dut->rootp->rp6502__DOT__aud_opl__DOT__page, 0xF0);

    /* Canvas 1, mode 3 entries across [0, 240) on plane 0 with the config
     * pointer, nothing at 240. */
    auto *r = dut->rootp;
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__canvas_shadow, 1);
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__fill_e[0],
              0x80000000u | (3u << 16));
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__fill_c[0], 0x1000);
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__fill_e[239 * 4],
              0x80000000u | (3u << 16));
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__fill_e[240 * 4], 0u);

    /* The sprite slots: mode 4 plane 1, mode 5 plane 2, count over config in
     * the second word. spr_e keeps only the live bits —
     * {enable, mode[2:0], attr[15:0]} — because the twelve dead ones cost
     * three M10K to store. */
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__spr_e[100 * 4 + 1],
              (1u << 19) | (4u << 16));
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__spr_c[100 * 4 + 1],
              (3u << 16) | 0x2000u);
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__spr_e[100 * 4 + 2],
              (1u << 19) | (5u << 16) | 10u);
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__spr_c[100 * 4 + 2],
              (2u << 16) | 0x3000u);
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
