/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

#define AUD_GATE 512
static long g_cross;
static int g_side;

static void aud_clock()
{
    tb_platform_clock(dut, rom, nullptr);
    if (dut->wiring_aud_valid)
    {
        g_valids++;
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
        const int l = (int)(int16_t)dut->wiring_aud_l;
        if (l > AUD_GATE)
        {
            if (g_side < 0)
                g_cross++;
            g_side = 1;
        }
        else if (l < -AUD_GATE)
        {
            if (g_side > 0)
                g_cross++;
            g_side = -1;
        }
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
    g_cross = 0;
    g_side = 0;
    return true;
}
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
    /* This ROM plays one PSG channel at full volume with centred pan,
     * which peaks at about 32767 * 63 / 128 = 16127, so 4096 leaves a wide
     * margin. */
    ASSERT_GT(g_peak, 4096);
    ASSERT_GT(g_valids, (long)0);
    run_frame();
    run_frame();
    run_frame();
    ASSERT_GT(g_cross, (long)10);
}

/* The PSG takes its registers only from the XRAM writes it snoops, so a
 * block written before xreg is loaded into it by the replay in the
 * firmware's psg_xreg. While AUD_PSG_REPLAY is clear, the PSG starts or
 * releases a voice from a gate bit only on a 6502 write. psg_xreg replays
 * the block with AUD_PSG_REPLAY clear, so the replay starts no note, and
 * the note starts on the gate written afterwards. */
UTEST(aud, a_psg_block_programmed_before_its_pointer)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PSG_PRE));
    /* The PSG and the OPL have no reset input, so a note from the previous
     * test continues through tb_reset until the firmware writes their
     * pointer registers. */
    run_frame();
    /* The program writes the second gate about five frames after xreg, at
     * the default 8 MHz. */
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
    /* One OPL2 voice at full volume peaks near 4088, a real YM3812's
     * level. */
    ASSERT_GT(g_peak, 2048);
    ASSERT_GT(g_valids, (long)0);
    run_frame();
    run_frame();
    run_frame();
    ASSERT_GT(g_cross, (long)10);
}

/* The note after the page clear sounds, and the burst after the note, which
 * the queue drops, changes nothing. */
UTEST(aud, opl_init_sounds_after_a_page_clear)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL_INIT));
    int at = frames_to_sound(8);
    ASSERT_NE(at, -1);
    ASSERT_LT(at, 3);
    ASSERT_GT(g_peak, 2048);
}

/* 300 writes to one register, 4 cycles apart at 8 MHz, against a queue of
 * 256. They take about 7.5 samples to arrive and the engine applies some
 * of them meanwhile, so 44 less those are dropped. The attribute returns
 * the count and clears it, and refuses a write. */
UTEST(aud, opl_drops_counts_the_writes_the_queue_could_not_take)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL_DROPS));
    auto *r = dut->rootp;
    auto &ram = r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem;
    const uint16_t got = 0x0210;
    ram[got + 7] = 0;
    /* Each clock's push, drop and pop land on the next edge; a pop frees a
     * place only for the writes after its edge. */
    long pushed = 0, dropped = 0, taken = 0, frames = 0;
    while (!ram[got + 7] && frames < 20)
    {
        const int line = dut->wiring_scanline;
        aud_clock();
        const long seen = pushed + dropped;
        const int push = r->wiring__DOT__opl__DOT__q_push;
        const int drop = r->wiring__DOT__opl__DOT__q_drop;
        if (seen > 0 && seen + push + drop < 300)
            taken += r->wiring__DOT__opl__DOT__q_take;
        pushed += push;
        dropped += drop;
        if (line == 524 && dut->wiring_scanline == 0)
            frames++;
    }
    fprintf(stderr, "  %ld queued, %ld applied during the burst, %ld dropped\n",
            pushed, taken, dropped);
    ASSERT_EQ(ram[got + 7], 1);
    ASSERT_EQ(pushed + dropped, 300L);
    ASSERT_GT(dropped, 0L);
    ASSERT_EQ(dropped, 300 - 256 - taken);
    ASSERT_EQ(ram[got] | ram[got + 1] << 8, (int)dropped);
    ASSERT_EQ(ram[got + 2] | ram[got + 3] << 8, 0);
    ASSERT_EQ(ram[got + 4] | ram[got + 5] << 8, 0xFFFF);
    ASSERT_NE(ram[got + 6], 0);
}

UTEST(aud, the_bell_rings_with_no_program)
{
    ASSERT_TRUE(load_rom(AUD_ROM_BEL));
    const int at = frames_to_sound(4);
    fprintf(stderr, "  bel heard on frame %d, peak %d\n", at, g_peak);
    ASSERT_NE(at, -1);
    ASSERT_GT(g_peak, 32);
    ASSERT_GT(g_valids, (long)0);
}

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

    ASSERT_EQ((int)dut->rootp->wiring__DOT__resb, 0);
    ASSERT_EQ(frames, (long)3);
    ASSERT_GT(dut->rootp->wiring__DOT__soc__DOT__mtime_us, mtime0);
    ASSERT_GT(g_valids, (long)0);
    ASSERT_GT(g_peak, 32);
    (void)rv_bytes;
}

UTEST(aud, a_program_exit_goes_quiet)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL_EXIT));
    int at = frames_to_sound(8);
    ASSERT_NE(at, -1);

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

    run_frame();
    g_energy = 0;
    g_peak = 0;
    g_valids = 0;
    run_frame();
    run_frame();
    ASSERT_GT(g_valids, (long)0);
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
