/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "host/host.h"
#include "tb_machine.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstring>
#include <string>
#include <vector>

static Vwiring *dut;

static uint32_t fb[640 * 480];

static bool boot(const char *name, size_t px)
{
    std::vector<uint8_t> rom;
    std::string path = std::string(ROMS_DIR "/") + name + ".rp6502";
    if (!tb_rom_read(path.c_str(), rom) || !tb_boot(dut, rom))
        return false;
    static uint32_t once[640 * 480];
    tb_capture(dut, once, px);
    tb_capture(dut, fb, px);
    return memcmp(once, fb, px * sizeof(uint32_t)) == 0;
}

UTEST(vidregs, console_return_restores_vsync_line)
{
    ASSERT_TRUE(boot("mode0_return", 640 * 480));
    ASSERT_EQ(dut->rootp->wiring__DOT__prog__DOT__vsync_shadow, 480);
}

/* The highest scanline end in prog_bands' programs is 224 on its 320x240
 * canvas, so the vsync line is 224 rather than the canvas height of 240. */
UTEST(vidregs, a_short_program_moves_the_vsync_line)
{
    ASSERT_TRUE(boot("prog_bands", 320 * 240));
    ASSERT_EQ(dut->rootp->wiring__DOT__prog__DOT__vsync_shadow, 224);
}

/* The sprite stage stops drawing a line whose sprites are not finished when
 * the line ends, and the emulator draws every sprite, so this CRC is of the
 * RTL's picture alone. */
UTEST(vidregs, sprite_overrun_counts_lost_races)
{
    ASSERT_TRUE(boot("sprite_overrun", 320 * 240));
    ASSERT_GT(dut->rootp->wiring__DOT__sprite__DOT__sprite_overrun, 0);
    ASSERT_EQ(host_crc32(0, fb, 320 * 240 * sizeof(uint32_t)), 0xA34E970Cu);
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
