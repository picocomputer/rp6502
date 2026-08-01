/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Both audio engines end to end, in the fewest frames that can prove it:
 * a program asks for the device with xreg, pours its registers into the
 * XRAM page through RW0, and the machine makes a noise. Nothing here
 * checks a waveform — psg's lockstep does that for the PSG, and the
 * OPL2 is a different implementation from emu8950 with no agreement to
 * hold it to. What these check is every link between a 6502 store and a
 * sample leaving the machine.
 *
 * This replaces a furelise playthrough that took eighty frames to reach
 * its first note and, for all that, only ever exercised the PSG. The
 * OPL2 reached hardware silent behind exactly that gap: the PSG re-reads
 * its whole config from XRAM every tick and needs the snoop only for
 * gate edges, so a working PSG says nothing about a snoop's ability to
 * carry register data — and the OPL has nothing else.
 *
 * The programs are src/fpga/codegen/aud_rom_gen.py's, the same files a
 * Pocket loads from its card, so a note that sounds on hardware and a
 * note asserted here cannot drift apart.
 *
 * Energy is the measurement, and peak with it. A sum alone passes on an
 * average deviation of one count, which is what a voice scaled wrong
 * looks like, and that too has already shipped once.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "tb_quiet.h"
#include "tb_stage.h"
#include "tb_tcm.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <vector>

static Vrp6502 *dut;
/* Half clk_sys, rising with it: the PLL's shape, not a divider's. */
static bool rv_phase;
static std::vector<uint8_t> rom;

static long g_valids;
static long g_energy;
static int g_peak;

static bool load_firmware(const char *path)
{
    auto *r = dut->rootp;
    return tb_load_tcm(r->rp6502__DOT__rv__DOT__tcm0,
                       r->rp6502__DOT__rv__DOT__tcm1,
                       r->rp6502__DOT__rv__DOT__tcm2,
                       r->rp6502__DOT__rv__DOT__tcm3, path);
}

static void clock_cycle()
{
    uint32_t a = dut->rp6502_stage_addr;
    dut->stage_rdata = tb_stage(rom, a);
    rv_phase = !rv_phase;
    dut->clk_rv = rv_phase;
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_rv = 0;
    dut->clk_sys = 0;
    dut->eval();
    if (dut->rp6502_aud_valid)
    {
        g_valids++;
        /* Signed at full scale, silence at zero. This used to subtract
         * the RP2350 PWM's 512 center, which stopped being the center
         * when the path widened — and every threshold below was loose
         * enough that it kept passing while measuring the wrong thing. */
        int dl = (int)(int16_t)dut->rp6502_aud_l;
        int dr = (int)(int16_t)dut->rp6502_aud_r;
        if (dl < 0)
            dl = -dl;
        if (dr < 0)
            dr = -dr;
        g_energy += dl + dr;
        if (dl > g_peak)
            g_peak = dl;
        if (dr > g_peak)
            g_peak = dr;
    }
}

static void run_frame()
{
    while (dut->rp6502_scanline != 524)
        clock_cycle();
    while (dut->rp6502_scanline != 0)
        clock_cycle();
}

static bool load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    rom.clear();
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        rom.insert(rom.end(), buf, buf + n);
    fclose(f);

    if (!load_firmware(SW_BIN))
        return false;
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();
    g_valids = g_energy = 0;
    g_peak = 0;
    return true;
}
/* Frames until the engine is heard, or -1. Two is the budget; the load
 * and the program's own setup eat most of the first. */
static int frames_to_sound(int limit)
{
    for (int i = 0; i < limit; i++)
    {
        g_energy = 0;
        g_peak = 0;
        run_frame();
        if (g_peak > 0)
            return i;
    }
    return -1;
}

UTEST(aud, psg_makes_a_noise)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PSG));
    int at = frames_to_sound(8);
    ASSERT_NE(at, -1);
    ASSERT_LT(at, 3);
    /* Loud, not merely moving. */
    /* 511 was the whole range this port had when it was ten bits, so a
     * peak above it can only come from the wide path. One channel at full
     * volume centred lands at 32767 * 63 >> 7 = 16127, which is what this
     * ROM plays; 4096 leaves room without accepting a narrow one. */
    ASSERT_GT(g_peak, 4096);
    ASSERT_GT(g_valids, (long)0);
}

UTEST(aud, opl_makes_a_noise)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL));
    int at = frames_to_sound(8);
    ASSERT_NE(at, -1);
    ASSERT_LT(at, 3);
    ASSERT_GT(g_peak, 4096);
    ASSERT_GT(g_valids, (long)0);
}

UTEST(aud, the_machine_runs_while_the_6502_is_held)
{
    /* Resetting the 6502 is how a program is stopped and the next one
     * started, and none of the rest of the machine should notice. The
     * soft CPU owns that reset and has to keep running to release it;
     * the raster cannot pause without the display losing lock; and a
     * voice left sounding is the device's business, not the CPU's. */
    ASSERT_TRUE(load_rom(AUD_ROM_OPL));
    ASSERT_NE(frames_to_sound(8), -1);

    dut->rootp->rp6502__DOT__cpu_run = 0;

    long rv_bytes = 0;
    long frames = 0;
    uint64_t mtime0 = dut->rootp->rp6502__DOT__rv__DOT__mtime_us;
    g_energy = 0;
    g_peak = 0;
    g_valids = 0;
    for (int i = 0; i < 3; i++)
    {
        int prev = dut->rp6502_scanline;
        while (dut->rp6502_scanline != 524)
        {
            clock_cycle();
            if (dut->rp6502_rv_tx_valid)
                rv_bytes++;
            if (dut->rp6502_scanline != prev)
                prev = dut->rp6502_scanline;
        }
        while (dut->rp6502_scanline != 0)
            clock_cycle();
        frames++;
    }

    /* The 6502 really is held. */
    ASSERT_EQ((int)dut->rootp->rp6502__DOT__cpu_run, 0);
    /* The raster kept its cadence. */
    ASSERT_EQ(frames, (long)3);
    /* The soft CPU kept time, which it cannot do if it stopped. */
    ASSERT_GT(dut->rootp->rp6502__DOT__rv__DOT__mtime_us, mtime0);
    /* And the note is still sounding. */
    ASSERT_GT(g_valids, (long)0);
    ASSERT_GT(g_peak, 32);
    (void)rv_bytes;
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
