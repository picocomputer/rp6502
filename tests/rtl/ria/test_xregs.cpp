/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "utest.h"
#include "xreg_prog.h"

#include <vector>

static Vwiring *dut;

UTEST(xregs, the_program_reaches_the_devices)
{
    std::vector<uint8_t> rom;
    xreg_rom(rom);

    int strikes = 0;
    bool bel_gate_prev = false;
    ASSERT_TRUE(tb_boot_each(dut, rom, nullptr, [&] {
        const bool bg =
            (dut->rootp->wiring__DOT__psg__DOT__bel_hi >> 16) & 1;
        if (!bel_gate_prev && bg)
            strikes++;
        bel_gate_prev = bg;
    }));

    /* The program sends BEL twice, first with the bell muted and then
     * unmuted, so the bell strikes once. */
    ASSERT_EQ(strikes, 1);

    /* The program points the PSG at 0x8000 and then the OPL at 0xF000.
     * The firmware writes 0xFFFF to one engine's pointer whenever it sets
     * the other's, so the PSG's xaddr reads 0xFFFF. */
    ASSERT_EQ(dut->rootp->wiring__DOT__psg__DOT__xaddr, 0xFFFF);
    ASSERT_TRUE(dut->rootp->wiring__DOT__opl__DOT__enabled);
    ASSERT_EQ(dut->rootp->wiring__DOT__opl__DOT__page, 0xF0);

    /* The scanline program's arrays are indexed by line * 4 + plane. Mode 3
     * is set on plane 0 over the whole of canvas 1, which is 240 lines tall,
     * so lines 0 to 239 hold a fill entry and line 240 holds none. */
    auto *r = dut->rootp;
    ASSERT_EQ(r->wiring__DOT__prog__DOT__canvas_shadow, 1);
    ASSERT_EQ(r->wiring__DOT__prog__DOT__fill_e[0],
              0x80000000u | (3u << 16));
    ASSERT_EQ(r->wiring__DOT__prog__DOT__fill_c[0], 0x1000);
    ASSERT_EQ(r->wiring__DOT__prog__DOT__fill_e[239 * 4],
              0x80000000u | (3u << 16));
    ASSERT_EQ(r->wiring__DOT__prog__DOT__fill_e[240 * 4], 0u);

    /* spr_e keeps the enable in bit 19, the mode in bits 18 to 16 and the
     * attribute in bits 15 to 0. spr_c keeps the descriptor count in bits
     * 31 to 16 and the config address in bits 15 to 0. */
    ASSERT_EQ(r->wiring__DOT__prog__DOT__spr_e[100 * 4 + 1],
              (1u << 19) | (4u << 16));
    ASSERT_EQ(r->wiring__DOT__prog__DOT__spr_c[100 * 4 + 1],
              (3u << 16) | 0x2000u);
    ASSERT_EQ(r->wiring__DOT__prog__DOT__spr_e[100 * 4 + 2],
              (1u << 19) | (5u << 16) | 10u);
    ASSERT_EQ(r->wiring__DOT__prog__DOT__spr_c[100 * 4 + 2],
              (2u << 16) | 0x3000u);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vwiring;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
