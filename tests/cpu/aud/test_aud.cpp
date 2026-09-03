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
 * The programs are tests/gen/aud_rom_gen.py's, the same files a
 * Pocket loads from its card, so a note that sounds on hardware and a
 * note asserted here cannot drift apart.
 *
 * Energy is the measurement, and peak with it. A sum alone passes on an
 * average deviation of one count, which is what a voice scaled wrong
 * looks like, and that too has already shipped once.
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <vector>

static Vwiring *dut;
static std::vector<uint8_t> rom;

static long g_valids;
static long g_energy;
static int g_peak;

/* The platform clock with the codec tapped: every sample the machine
 * puts out is counted, so a frame's worth of them is what "heard" means
 * below. */
static void aud_clock()
{
    tb_platform_clock(dut, rom, nullptr);
    if (dut->wiring_aud_valid)
    {
        g_valids++;
        /* Signed at full scale, silence at zero. This used to subtract
         * the RP2350 PWM's 512 center, which stopped being the center
         * when the path widened — and every threshold below was loose
         * enough that it kept passing while measuring the wrong thing. */
        int dl = (int)(int16_t)dut->wiring_aud_l;
        int dr = (int)(int16_t)dut->wiring_aud_r;
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
    while (dut->wiring_scanline != 524)
        aud_clock();
    while (dut->wiring_scanline != 0)
        aud_clock();
}

static bool load_rom(const char *path)
{
    rom.clear();
    if (!tb_rom_read(path, rom))
        return false;
    if (!tb_firmware(dut, SW_BIN))
        return false;
    tb_reset(dut);
    dut->rootp->wiring__DOT__soc__DOT__mmio_slot_len = (uint32_t)rom.size();
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

/* The same note with the block written before the pointer. An engine
 * that only hears writes would have nothing to play, so this is the
 * import's end of the plumbing: the whole structure has to arrive, and
 * the gate written among it has to not sound, which is what the machine
 * that reads XRAM does with its queue. The note starts on the gate
 * written afterwards, so silence here would be an import that struck it
 * early rather than one that never ran. */
UTEST(aud, a_psg_block_programmed_before_its_pointer)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PSG_PRE));
    /* One frame for the last program's note to die under the firmware's
     * park: these engines free-run and take no reset. */
    run_frame();
    /* The program holds its own gate off for about five frames. */
    for (int i = 0; i < 3; i++)
    {
        g_energy = 0;
        g_peak = 0;
        run_frame();
        ASSERT_EQ(g_peak, 0);
    }
    int at = frames_to_sound(16);
    ASSERT_NE(at, -1);
    ASSERT_GT(g_peak, 4096);
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

/* The bell is the soft CPU's, and it has to sound with no program holding
 * an engine at all — the console's own case, and the one the fabric used
 * to serve with a module of its own. */
UTEST(aud, the_bell_rings_with_no_program)
{
    ASSERT_TRUE(load_rom(AUD_ROM_BEL));
    const int at = frames_to_sound(4);
    fprintf(stderr, "  bel heard on frame %d, peak %d\n", at, g_peak);
    ASSERT_NE(at, -1);
    ASSERT_GT(g_peak, 32);
    ASSERT_GT(g_valids, (long)0);
}

/* Nothing gates the mix. An OPL program holds an engine and the bell
 * sounds over it, both reaching the codec on the one tick. */
UTEST(aud, the_bell_rings_over_the_opl)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL_BEL));
    const int at = frames_to_sound(4);
    fprintf(stderr, "  opl+bel heard on frame %d, peak %d\n", at, g_peak);
    ASSERT_NE(at, -1);
    ASSERT_GT(g_peak, 32);
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

    dut->rootp->wiring__DOT__resb = 0;

    long rv_bytes = 0;
    long frames = 0;
    uint64_t mtime0 = dut->rootp->wiring__DOT__soc__DOT__mtime_us;
    g_energy = 0;
    g_peak = 0;
    g_valids = 0;
    for (int i = 0; i < 3; i++)
    {
        int prev = dut->wiring_scanline;
        while (dut->wiring_scanline != 524)
        {
            aud_clock();
            if (dut->wiring_rv_tx_valid)
                rv_bytes++;
            if (dut->wiring_scanline != prev)
                prev = dut->wiring_scanline;
        }
        while (dut->wiring_scanline != 0)
            aud_clock();
        frames++;
    }

    /* The 6502 really is held. */
    ASSERT_EQ((int)dut->rootp->wiring__DOT__resb, 0);
    /* The raster kept its cadence. */
    ASSERT_EQ(frames, (long)3);
    /* The soft CPU kept time, which it cannot do if it stopped. */
    ASSERT_GT(dut->rootp->wiring__DOT__soc__DOT__mtime_us, mtime0);
    /* And the note is still sounding. */
    ASSERT_GT(g_valids, (long)0);
    ASSERT_GT(g_peak, 32);
    (void)rv_bytes;
}

UTEST(aud, a_program_exit_goes_quiet)
{
    /* The distinction the test above draws is the one this enforces from
     * the other side: HOLDING the 6502 leaves a voice sounding, because a
     * voice is the device's business — but EXITING a program parks both
     * engines, the way the RP2350's aud_stop hands its interrupt back.
     * The engines here free-run, so until the firmware's exit path parked
     * the pointers, a stopped program's last chord played forever. It had
     * from first power-on to now, with nothing looking. */
    ASSERT_TRUE(load_rom(AUD_ROM_OPL_EXIT));
    int at = frames_to_sound(8);
    ASSERT_NE(at, -1);

    /* The ROM delays a moment and exits; give the firmware frames enough
     * to see the API op and park the engines. */
    int stopped = -1;
    for (int i = 0; i < 20; i++)
    {
        run_frame();
        if (!dut->rootp->wiring__DOT__resb)
        {
            stopped = i;
            break;
        }
    }
    ASSERT_NE(stopped, -1);

    /* One frame for the release already in flight, then silence — exactly
     * zero, not merely quiet: parked engines answer zero and no bell has
     * been rung. */
    run_frame();
    g_energy = 0;
    g_peak = 0;
    g_valids = 0;
    run_frame();
    run_frame();
    ASSERT_GT(g_valids, (long)0); /* the sample tick survives the stop */
    ASSERT_EQ(g_peak, 0);
    ASSERT_EQ(g_energy, (long)0);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vwiring;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
