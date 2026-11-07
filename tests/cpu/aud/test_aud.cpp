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
#include <cstdlib>
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
static long g_lr_differ;

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
        if (dut->wiring_aud_l != dut->wiring_aud_r)
            g_lr_differ++;
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
    g_lr_differ = 0;
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

/* The block is programmed, gate and all, before xreg. A pointer write
 * starts the PSG's registers at zero, so the gate struck about five frames
 * after xreg plays no tone, and the tone starts when the program writes the
 * frequency and duty about five frames after that. */
UTEST(aud, a_psg_block_programmed_before_its_pointer)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PSG_PRE));
    /* The PSG and the OPL have no reset input, so a note from the previous
     * test continues through tb_reset until the firmware writes their
     * pointer registers. */
    run_frame();
    for (int i = 0; i < 3; i++)
    {
        g_peak = 0;
        run_frame();
        ASSERT_EQ(g_peak, 0);
    }
    g_cross = 0;
    for (int i = 0; i < 5; i++)
        run_frame();
    ASSERT_EQ(g_cross, (long)0);
    int at = -1;
    for (int i = 0; i < 12 && at < 0; i++)
    {
        g_cross = 0;
        g_peak = 0;
        run_frame();
        if (g_cross > 10)
            at = i;
    }
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
UTEST(aud, opl_overruns_count_the_writes_the_queue_could_not_take)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL_OVERRUNS));
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

/* 10 ms of PCM run dry within a frame. The attribute returns the one
 * underrun and clears it, and refuses a write. */
UTEST(aud, pcm_underruns_counts_the_buffer_running_dry)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PCM_DRY));
    auto &ram = dut->rootp->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem;
    const uint16_t got = 0x0210;
    ram[got + 7] = 0;
    for (int frames = 0; frames < 12 && !ram[got + 7]; frames++)
        run_frame();
    ASSERT_EQ(ram[got + 7], 1);
    ASSERT_EQ(ram[got] | ram[got + 1] << 8, 1);
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

/* aud.py's pcm ROM: 1050 Hz at 22050 Hz, 16-bit mono, peaking at 16000,
 * fed once a frame. Two frames start it, and the next ten, 8000 samples,
 * hold 175 cycles, 350 swings through the gate, the same on both sides. */
UTEST(aud, pcm_plays_the_tone_it_is_given)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PCM));
    run_frame();
    run_frame();
    g_valids = 0;
    g_peak = 0;
    g_cross = 0;
    g_lr_differ = 0;
    for (int i = 0; i < 10; i++)
        run_frame();
    fprintf(stderr, "  %ld samples, %ld swings, peak %d\n", g_valids, g_cross, g_peak);
    ASSERT_EQ(g_valids, 8000L);
    ASSERT_GE(g_cross, 348L);
    ASSERT_LE(g_cross, 352L);
    ASSERT_GT(g_peak, 16000 * 98 / 100);
    ASSERT_LT(g_peak, 16000 * 102 / 100);
    ASSERT_EQ(g_lr_differ, 0L);
}

/* aud.py's pcm_rules ROM, whose answers script.pcm_rules checks on the
 * emulator: a word each from RULES, in this order. */
enum
{
    R_OPEN_48001, R_ERRNO_48001, R_OPEN_48000, R_OPEN_BUSY, R_ERRNO_BUSY,
    R_END, R_WRITE, R_CUR0, R_CUR1, R_CUR2, R_SYNC, R_CUR_SYNCED, R_REFILL,
    R_SET, R_CUR_SET, R_CLOSE, R_REOPEN, R_WRITE_800, R_VSYNC0, R_CLOSE_800,
    R_VSYNC1, R_WRITE_EXIT, R_COUNT
};
#define RULES 0x0220
#define RULES_DONE (RULES + 2 * R_COUNT)
#define RULES_EBUSY 16
#define RULES_EINVAL 22
/* What 48000 Hz 16-bit stereo plays in the ROM's delay(25). */
#define RULES_CUR_FALL 771

/* The fabric takes each open's step and format, and each close comes only
 * once the buffer has played. The exit with half a second waiting takes it
 * along: the 6502 is in reset within two frames, and silent after. The tone
 * test leaves PCM open, so the first open is seen only if aud_init closed
 * it. */
UTEST(aud, pcm_keeps_the_rules)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PCM_RULES));
    auto *r = dut->rootp;
    auto &ram = r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem;
    ram[RULES_DONE] = 0;
    uint32_t step[2] = {0, 0};
    int fmt[2] = {0, 0}, level[2] = {-1, -1};
    int opens = 0, closes = 0;
    bool was = r->wiring__DOT__pcm__DOT__open;
    long frames = 0;
    while (!ram[RULES_DONE] && frames < 40)
    {
        const int line = dut->wiring_scanline;
        aud_clock();
        const bool open = r->wiring__DOT__pcm__DOT__open;
        if (open && !was && opens < 2)
        {
            step[opens] = r->wiring__DOT__pcm__DOT__step_m1;
            fmt[opens++] = r->wiring__DOT__pcm__DOT__b16 | r->wiring__DOT__pcm__DOT__st << 1;
        }
        if (!open && was && opens && closes < 2)
            level[closes++] = (r->wiring__DOT__pcm__DOT__wr - r->wiring__DOT__pcm__DOT__rd) & 0x1FFF;
        was = open;
        if (line == 524 && dut->wiring_scanline == 0)
            frames++;
    }
    ASSERT_EQ(ram[RULES_DONE], 1);
    uint16_t w[R_COUNT];
    fprintf(stderr, " ");
    for (int i = 0; i < R_COUNT; i++)
    {
        w[i] = (uint16_t)(ram[RULES + 2 * i] | ram[RULES + 2 * i + 1] << 8);
        fprintf(stderr, " %u", w[i]);
    }
    fprintf(stderr, "\n  %ld frames\n", frames);
    ASSERT_EQ(w[R_OPEN_48001], 0xFFFF);
    ASSERT_EQ(w[R_ERRNO_48001], RULES_EINVAL);
    ASSERT_LT(w[R_OPEN_48000], 0x8000);
    ASSERT_EQ(w[R_OPEN_BUSY], 0xFFFF);
    ASSERT_EQ(w[R_ERRNO_BUSY], RULES_EBUSY);
    ASSERT_EQ(w[R_END], 4096);
    ASSERT_EQ(w[R_WRITE], 4096);
    ASSERT_LE(w[R_CUR0], 4096);
    for (int i = R_CUR0; i < R_CUR2; i++)
    {
        const int fall = w[i] - w[i + 1];
        ASSERT_LT(abs(fall - RULES_CUR_FALL), RULES_CUR_FALL / 8);
    }
    for (int i = R_CUR0; i <= R_CUR2; i++)
    {
        ASSERT_GT(w[i], 0);
        ASSERT_EQ(w[i] % 4, 0);
    }
    ASSERT_EQ(w[R_SYNC], 0);
    ASSERT_EQ(w[R_CUR_SYNCED], 0);
    ASSERT_EQ(w[R_REFILL], 4096);
    ASSERT_EQ(w[R_SET], 0);
    ASSERT_EQ(w[R_CUR_SET], 0);
    ASSERT_EQ(w[R_CLOSE], 0);
    ASSERT_LT(w[R_REOPEN], 0x8000);
    ASSERT_EQ(w[R_WRITE_800], 800);
    ASSERT_EQ(w[R_CLOSE_800], 0);
    const int waited = (w[R_VSYNC1] - w[R_VSYNC0]) & 0xFF;
    ASSERT_GE(waited, 5);
    ASSERT_LE(waited, 7);

    ASSERT_EQ(opens, 2);
    ASSERT_EQ(step[0], 0xFFFFFFFFu);
    ASSERT_EQ(fmt[0], 3);
    ASSERT_EQ(step[1], (uint32_t)((8000ull << 32) / 48000 - 1));
    ASSERT_EQ(fmt[1], 0);
    ASSERT_EQ(closes, 2);
    ASSERT_EQ(level[0], 0);
    ASSERT_EQ(level[1], 0);

    ASSERT_EQ(w[R_WRITE_EXIT], 4096);
    int stopped = -1;
    for (int i = 0; i < 40 && stopped < 0; i++)
    {
        run_frame();
        if (!r->wiring__DOT__resb)
            stopped = i;
    }
    fprintf(stderr, "  stopped in frame %d\n", stopped);
    ASSERT_NE(stopped, -1);
    ASSERT_LT(stopped, 2);
    run_frame();
    g_peak = 0;
    run_frame();
    ASSERT_EQ(g_peak, 0);
    ASSERT_FALSE(r->wiring__DOT__pcm__DOT__open);
}

/* aud.py's pcm_order ROM: the sizes of its writes, from XRAM and then from
 * the xstack. The count each returns is a word from ORDER. */
static const int order_sizes[] = {1, 3, 2, 5, 7, 4, 9, 6, 11, 13, 8, 1, 1, 1, 17, 30, 2, 3,
                                  1, 2, 3, 5, 7, 4, 6, 1, 3};
#define ORDER_N (int)(sizeof order_sizes / sizeof order_sizes[0])
#define ORDER 0x0250
#define ORDER_DONE (ORDER + 2 * ORDER_N)

/* FIFO byte i is bank i & 3, row i >> 2. */
static uint8_t pcm_fifo(int i)
{
    auto *r = dut->rootp;
    const int row = i >> 2;
    switch (i & 3)
    {
    case 0:
        return r->wiring__DOT__pcm__DOT__bank0[row];
    case 1:
        return r->wiring__DOT__pcm__DOT__bank1[row];
    case 2:
        return r->wiring__DOT__pcm__DOT__bank2[row];
    }
    return r->wiring__DOT__pcm__DOT__bank3[row];
}

/* At PCM:1,8,1 the FIFO still holds every byte written. Each write, of 1
 * to 30 bytes from any alignment, returns its size, and the bytes land in
 * order: byte i is i * 7 + 3. */
UTEST(aud, pcm_writes_land_in_order)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PCM_ORDER));
    auto *r = dut->rootp;
    auto &ram = r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem;
    ram[ORDER_DONE] = 0;
    for (int i = 0; i < 40 && !ram[ORDER_DONE]; i++)
        run_frame();
    ASSERT_EQ(ram[ORDER_DONE], 1);
    int total = 0;
    for (int i = 0; i < ORDER_N; i++)
    {
        ASSERT_EQ(ram[ORDER + 2 * i] | ram[ORDER + 2 * i + 1] << 8, order_sizes[i]);
        total += order_sizes[i];
    }
    ASSERT_EQ((int)r->wiring__DOT__pcm__DOT__wr, total);
    int bad = 0;
    for (int i = 0; i < total; i++)
        if (pcm_fifo(i) != (uint8_t)(i * 7 + 3))
        {
            if (bad++ < 4)
                fprintf(stderr, "  byte %d is %02X\n", i, pcm_fifo(i));
        }
    ASSERT_EQ(bad, 0);
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
