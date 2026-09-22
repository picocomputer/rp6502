/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The terminal advances its blink phase every TERM_BLINK_TICK_FRAMES, which is
 * 10 frames, and a TERM_ATTR_BLINK cell follows bit 1 of the phase, so the
 * cell changes state every 20 frames. The test forces mode0's blink_shadow to
 * the phase it checks instead of simulating those frames.
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "session_prog.h"
#include "tb_machine.h"
#include "tb_rom.h"
#include "tb_term.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <vector>

static Vwiring *dut;

/* The firmware writes its own blink phase to blink_shadow once a frame, so a
 * pinned phase is written again before every clock rather than once. */
static int pinned_blink = -1;

static void capture_frame(uint32_t *fb)
{
    auto ck = [] {
        if (pinned_blink >= 0)
            dut->rootp->wiring__DOT__mode0__DOT__blink_shadow =
                (uint8_t)pinned_blink;
        tb_clock(dut);
    };
    while (dut->wiring_scanline != 524)
        ck();
    while (dut->wiring_scanline != 0)
        ck();
    size_t at = 0;
    while (at < 640 * 480)
    {
        ck();
        if (dut->wiring_vid_de)
            fb[at++] = tb_rgba8(dut->wiring_vid_pixel);
    }
}

UTEST(blink, off_phase_blanks_the_glyph)
{
    std::vector<uint8_t> rom;
    ASSERT_TRUE(session_rom(rom));
    ASSERT_TRUE(tb_boot(dut, rom));

    /* 0x0200 is TERM_ATTR_BLINK in the attribute byte, bits 15:8 of a
     * cell's first word. */
    auto *r = dut->rootp;
    /* A row's latest word pointer is in the slot it was written to since
     * the last latch, else in the slot the render reads. */
    auto row_word = [r](int i) {
        uint32_t slot = ((r->wiring__DOT__mode0__DOT__row_front
                          ^ r->wiring__DOT__mode0__DOT__row_pend) >> i) & 1;
        return (uint32_t)r->wiring__DOT__mode0__DOT__row_mem[2 * i + slot];
    };
    uint32_t base = row_word(25);
    uint32_t seed = term_cell(r, row_word(0));
    uint32_t bgw = term_cell(r, row_word(0) + 1);
    term_cell_set(r, base, (seed & 0xFFFF0000u) | 0x0200u | 'B');
    term_cell_set(r, base + 1, bgw);

    static uint32_t on[640 * 480], off[640 * 480];
    pinned_blink = 0;
    capture_frame(on);
    capture_frame(on);
    pinned_blink = 2;
    capture_frame(off);
    capture_frame(off);
    pinned_blink = -1;

    bool changed = false;
    for (int y = 25 * 16; y < 26 * 16 && !changed; y++)
        for (int x = 0; x < 8; x++)
            if (on[y * 640 + x] != off[y * 640 + x])
            {
                changed = true;
                break;
            }
    ASSERT_TRUE(changed);
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
