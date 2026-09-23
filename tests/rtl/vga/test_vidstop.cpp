/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A savestate stops the machine's clock wherever it happens to be, while
 * the memories the machine reads keep theirs. core_top.sv promises that
 * stop is invisible: every machine register misses the same edge and
 * resumes on the same edge. So a still scene stopped many times over a
 * frame, at many points in a line, draws the frame it draws unstopped.
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vwiring *dut;

static uint32_t ref[640 * 480], got[640 * 480];

/* One clk_sys period with the machine's clock held low, as the clock
 * control block holds it, and the soft CPU held too, as the savestate
 * engine halts it. */
static void clock_stopped()
{
    dut->mach_running = 0;
    dut->clk_rv = 0;
    dut->clk_mach = 0;
    dut->clk_sys = 1;
    dut->clk_a2 = 0;
    dut->eval();
    dut->clk_sys = 0;
    dut->clk_ph = 0;
    dut->clk_a2 = 1;
    dut->eval();
    dut->clk_a2 = 0;
    dut->eval();
    dut->clk_ph = 1;
    dut->clk_a2 = 1;
    dut->eval();
}

/* Where in a line the stop lands: every other line near its start, where
 * the slot sweeps and the first fetches are, and the rest spread along
 * it. */
static int stop_h(int line)
{
    return line & 1 ? (line * 37) % 800 : (line >> 1) % 12;
}

static void capture(uint32_t *fb, size_t px, bool stops)
{
    tb_frame_start(dut, [] {});
    int stopped_line = -1;
    size_t at = 0;
    while (at < px)
    {
        int line = dut->wiring_scanline;
        if (stops && line != stopped_line && line >= 2 && line < 520
            && dut->rootp->wiring__DOT__vid_h == stop_h(line)
            && !dut->rootp->wiring__DOT__timing__DOT__tick)
        {
            for (int i = 0; i < 25; i++)
                clock_stopped();
            stopped_line = line;
        }
        tb_clock(dut);
        if (dut->wiring_vid_de)
            fb[at++] = tb_rgba8(dut->wiring_vid_pixel);
    }
}

static bool run(const char *name, int w, int h)
{
    std::vector<uint8_t> rom;
    std::string path = std::string(ROMS_DIR "/") + name + ".rp6502";
    if (!tb_rom_read(path.c_str(), rom) || !tb_boot(dut, rom))
        return false;
    size_t px = (size_t)w * (size_t)h;
    capture(ref, px, false);
    capture(got, px, false);
    if (memcmp(ref, got, px * sizeof(uint32_t)) != 0)
        return false;
    capture(got, px, true);
    int rows = 0;
    for (int y = 0; y < h; y++)
        if (memcmp(ref + (size_t)y * w, got + (size_t)y * w,
                   (size_t)w * sizeof(uint32_t)) != 0)
        {
            if (rows < 8)
                fprintf(stderr, "%s: row %d differs after a stop\n", name, y);
            rows++;
        }
    if (rows)
        fprintf(stderr, "%s: %d rows differ\n", name, rows);
    return rows == 0;
}

UTEST(vidstop, fills_resume_unchanged)
{
    ASSERT_TRUE(run("fill_three640_16bpp_spr", 640, 480));
}

UTEST(vidstop, text_resumes_unchanged)
{
    ASSERT_TRUE(run("text_three640", 640, 480));
}

UTEST(vidstop, sprites_resume_unchanged)
{
    ASSERT_TRUE(run("sprite_stress", 640, 480));
}

UTEST(vidstop, terminal_resumes_unchanged)
{
    ASSERT_TRUE(run("mode0_overlay", 640, 480));
}

/* A 320 wide row spans two lines, and this one's sprites run into the
 * second. */
UTEST(vidstop, a_row_of_two_lines_resumes_unchanged)
{
    ASSERT_TRUE(run("sprite_pair", 320, 240));
}

UTEST(vidstop, bands_resume_unchanged)
{
    ASSERT_TRUE(run("prog_bands", 320, 240));
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
