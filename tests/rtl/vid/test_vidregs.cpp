/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Two registers inside the video path that nothing outside it can read.
 *
 * mode0_return's picture is checked in tests/cpu/vid on both machines; what
 * is left over is the scanline the vsync shadow went back to when the mode-0
 * slot returned the console, which no frame can show — a picture that is
 * right for the wrong reason looks exactly like one that is right.
 *
 * sprite_overrun is wholly this machine's. It puts more sprites on a line
 * than the stage can serve, and what it proves is that the stage drops them
 * and says so. The emulator has no beam to lose a race against and draws them
 * all, so the two machines' pictures differ here by design — which is why
 * this fixture never had a cross-machine expectation, and why its frame is
 * asserted here rather than in the shared suite.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "crc32.h"
#include "tb_machine.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;

static uint32_t fb[640 * 480];

/* Boot a corpus ROM and stand at a settled frame, left in fb. */
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

/* The console canvas is 640x480, and the vsync line goes back to its last
 * scanline when the slot that borrowed it returns. */
UTEST(vidregs, console_return_restores_vsync_line)
{
    ASSERT_TRUE(boot("mode0_return", 640 * 480));
    ASSERT_EQ(dut->rootp->rp6502__DOT__vid_prog__DOT__vsync_shadow, 480);
}

/* sprite_overrun is built on canvas 1 — 320x240, vidmodes.py. The picture is
 * still deterministic, so it is held to a CRC of its own; it is simply not
 * the emulator's. */
UTEST(vidregs, sprite_overrun_counts_lost_races)
{
    ASSERT_TRUE(boot("sprite_overrun", 320 * 240));
    ASSERT_GT(dut->rootp->rp6502__DOT__vid_sprite__DOT__vid_sprite_overrun, 0);
    ASSERT_EQ(bench_crc32(fb, 320 * 240 * sizeof(uint32_t)), 0xA34E970Cu);
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
