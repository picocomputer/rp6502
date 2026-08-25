/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The cell blink, which is this machine's alone to be asked about.
 *
 * A blinking cell alternates between its glyph and its background on a timer
 * — TERM_BLINK_TICK_US, 166 ms a phase. Neither machine can be run that long
 * in simulation, and the two would not agree anyway: the phase advances off
 * wall clock in the emulator and off mtime in the soft CPU's firmware. So
 * this holds the fabric's phase register still by hand and asks what the
 * beam paints, which is a question only a machine with that register has.
 *
 * It boots the scripted session from tests/cpu/vid because it wants what that
 * script scrolled into place: a real cell to take a foreground from, a
 * background word, and a row the printer has already left alone. The picture
 * that session settles to is checked over there, on both machines.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "session_prog.h"
#include "tb_machine.h"
#include "tb_rom.h"
#include "tb_term.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <vector>

static Vrp6502 *dut;

/* The firmware keeps advancing its own phase, so a phase under test has to be
 * held down for the whole frame rather than merely written before it. */
static int pinned_blink = -1;

static void capture_frame(uint32_t *fb)
{
    auto ck = [] {
        if (pinned_blink >= 0)
            dut->rootp->rp6502__DOT__vid_mode0__DOT__blink_shadow =
                (uint8_t)pinned_blink;
        tb_clock(dut);
    };
    while (dut->rp6502_scanline != 524)
        ck();
    while (dut->rp6502_scanline != 0)
        ck();
    size_t at = 0;
    while (at < 640 * 480)
    {
        ck();
        if (dut->rp6502_vid_de)
            fb[at++] = tb_rgba8(dut->rp6502_vid_pixel);
    }
}

UTEST(blink, off_phase_blanks_the_glyph)
{
    std::vector<uint8_t> rom;
    ASSERT_TRUE(session_rom(rom));
    ASSERT_TRUE(tb_boot(dut, rom));

    /* Write a blinking glyph into a blank row, taking its colours from cells
     * the session left: {fg from a real cell, ATTR_BLINK, 'B'} over the same
     * background. */
    auto *r = dut->rootp;
    uint32_t base = r->rp6502__DOT__vid_mode0__DOT__row_shadow[25];
    uint32_t seed = term_cell(
        r, r->rp6502__DOT__vid_mode0__DOT__row_shadow[0] >> 2);
    uint32_t bgw = term_cell(
        r, (r->rp6502__DOT__vid_mode0__DOT__row_shadow[0] >> 2) + 1);
    term_cell_set(r, base >> 2, (seed & 0xFFFF0000u) | 0x0200u | 'B');
    term_cell_set(r, (base >> 2) + 1, bgw);

    static uint32_t on[640 * 480], off[640 * 480];
    pinned_blink = 0;
    capture_frame(on); /* latch */
    capture_frame(on);
    pinned_blink = 2;
    capture_frame(off); /* latch */
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
    dut = new Vrp6502;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
