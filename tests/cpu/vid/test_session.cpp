/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A scripted terminal session, on whichever machine this tree built: forty
 * lines of scrolling past the screen, a DECSTBM region scroll that permutes
 * the row map, lazy EL clears, the SGR set with an SGR 58 underline colour,
 * DEC Special Graphics, italic, an alt-screen round trip whose content must
 * not survive the return, and a steady block cursor parked mid-screen.
 *
 * The picture it settles to is held to the CRC below. It used to be held to
 * the emulator rendering the same script inside this test.
 *
 * The blink phase is not here. It runs off wall clock on one machine and off
 * mtime on the other, so it is not a thing the two can be asked together;
 * tests/rtl/vid makes that claim against the machine that has a phase
 * register to hold still.
 */

#include "crc32.h"
#include "mut.h"
#include "session_prog.h"
#include "utest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

UTEST(session, scripted_terminal_frame_640x480)
{
    /* The console canvas, which this script never changes. */
    const int w = 640, h = 480;
    const size_t px = (size_t)w * (size_t)h;

    std::vector<uint8_t> rom;
    ASSERT_TRUE(session_rom(rom));
    const char *path = TEST_SCRATCH "/test_session.rp6502";
    ASSERT_TRUE(tb_rom_write(path, rom));
    ASSERT_TRUE(mut_boot(path));

    static uint32_t settled[640 * 480];
    memcpy(settled, mut_frame(w, h), px * sizeof(uint32_t));
    ASSERT_EQ(memcmp(settled, mut_frame(w, h), px * sizeof(uint32_t)), 0);

    uint32_t got = bench_crc32(settled, px * sizeof(uint32_t));
    if (getenv("RP6502_BLESS_CRC"))
        fprintf(stderr, "session frame 0x%08X\n", got);
    else
        ASSERT_EQ(got, 0x1059759Eu);
}

MUT_MAIN()
