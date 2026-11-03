/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sst.h"
#include "core/sys/xram.h"
#include "core/wdc/bus.h"
#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/aud/pcm.h"
#include "core/aud/psg.h"
#include "core/wdc/sram.h"
#include "core/term/term.h"

#include "emu_boot.h"

#include <string.h>

static uint8_t blob[1 << 20];

static const char *take(void)
{
    return sst_save(blob, sizeof blob, 0);
}

static void scrub(void)
{
    memset(sram, 0xEE, 0x100);
    xram[0] = 0xEE;
}

static bool scrub_intact(void)
{
    for (int i = 0; i < 0x100; i++)
        if (sram[i] != 0xEE)
            return false;
    return xram[0] == 0xEE;
}

UTEST(sst, a_size_is_fixed_and_answered_before_anything_runs)
{
    size_t first = sst_size();
    ASSERT_GT(first, (size_t)0);
    emu_frames(2);
    ASSERT_EQ(sst_size(), first);
}

UTEST(sst, the_shape_is_the_shape)
{
    ASSERT_EQ(take(), (const char *)NULL);
    ASSERT_EQ(sst_size(), (size_t)222890);
    ASSERT_EQ((unsigned)((blob[6] << 8) | blob[7]), 28u);
}

UTEST(sst, a_blob_is_framed)
{
    ASSERT_EQ(take(), (const char *)NULL);
    ASSERT_EQ(memcmp(blob, "RP65", 4), 0);
    ASSERT_EQ(memcmp(blob + sst_size() - 4, "56PR", 4), 0);
}

/* The bytes past the end of the short buffer are the guard. */
UTEST(sst, a_short_buffer_is_refused_rather_than_overrun)
{
    size_t len = sst_size() - 1;
    memset(blob + len, 0x5A, 16);
    ASSERT_NE(sst_save(blob, len, 0), (const char *)NULL);
    for (size_t i = 0; i < 16; i++)
        ASSERT_EQ(blob[len + i], 0x5A);
}

UTEST(sst, two_saves_of_one_unchanged_machine_are_the_same_bytes)
{
    static uint8_t again[sizeof blob];
    ASSERT_EQ(take(), (const char *)NULL);
    memcpy(again, blob, sst_size());
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    ASSERT_EQ(memcmp(again, blob, sst_size()), 0);
}

UTEST(sst, nothing_but_a_savestate_is_believed)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    blob[0] = 'X';
    ASSERT_NE(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_format_this_build_does_not_know_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    blob[5] = 99;
    ASSERT_NE(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_roster_that_differs_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    blob[12] ^= 0xFF; /* the manifest sum */
    ASSERT_NE(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_blob_that_does_not_add_up_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    blob[sst_size() - 8] ^= 0xFF; /* the payload sum */
    ASSERT_NE(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_blob_that_ends_early_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    memset(blob + sst_size() - 4, 0, 4); /* the end magic */
    ASSERT_NE(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_truncated_blob_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    ASSERT_NE(sst_load(blob, sst_size() - 1, 0), (const char *)NULL);
    ASSERT_NE(sst_load(blob, 4, 0), (const char *)NULL);
    ASSERT_NE(sst_load(blob, 0, 0), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_machine_that_could_not_have_existed_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    blob[16] = 1; /* starting, which sst_save never writes */
    ASSERT_NE(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
    ASSERT_EQ(take(), (const char *)NULL);
    blob[16] = 0;      /* stopped ... */
    blob[17] &= ~1;    /* ... but not held, which sys_latch_get never reports */
    ASSERT_NE(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_trusted_blob_skips_the_sum_and_keeps_the_frame)
{
    ASSERT_EQ(take(), (const char *)NULL);
    blob[sst_size() - 8] ^= 0xFF;
    ASSERT_EQ(sst_load(blob, sst_size(), SST_TRUSTED), (const char *)NULL);
    blob[0] = 'X';
    ASSERT_NE(sst_load(blob, sst_size(), SST_TRUSTED), (const char *)NULL);
}

UTEST(sst, what_a_row_carries_comes_back)
{
    for (int i = 0; i < 0x100; i++)
        sram[i] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < 0x100; i++)
        xram[i] = (uint8_t)(i * 13 + 5);
    ASSERT_EQ(take(), (const char *)NULL);

    scrub();
    ASSERT_TRUE(scrub_intact());

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    for (int i = 0; i < 0x100; i++)
        ASSERT_EQ(sram[i], (uint8_t)(i * 7 + 1));
    for (int i = 0; i < 0x100; i++)
        ASSERT_EQ(xram[i], (uint8_t)(i * 13 + 5));
}

UTEST(sst, a_program_picks_up_where_it_was)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    emu_frames(20);
    uint64_t at_save = bus_cycles();
    ASSERT_EQ(take(), (const char *)NULL);
    emu_frames(20);
    uint64_t reference = bus_cycles() - at_save;
    ASSERT_GT(reference, (uint64_t)0);

    sram_init();
    xram_init();
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(bus_cycles(), at_save);

    emu_frames(20);
    ASSERT_EQ(bus_cycles() - at_save, reference);
}

/* What rw_write does, without a 6502 to do it. Every caller writes the block
 * the sounding engine was given, so there is no page to test. */
static void aud_poke(uint16_t at, uint8_t val)
{
    xram[at] = val;
    aud_xram_write(at, val);
}

static void aud_note(uint16_t at)
{
    for (int i = 0; i < 8 * 8; i++)
        xram[at + i] = 0;
    xram[at + 0] = 0x00;
    xram[at + 1] = 0x04; /* 0x0400 thirds of a hertz, about 341 Hz */
    xram[at + 2] = 0x80; /* duty */
    xram[at + 3] = 0x04; /* full volume (the table descends), attack rate 4 */
    xram[at + 4] = 0x28; /* sustain two steps down, decay rate 8 */
    xram[at + 5] = 0x08; /* sine, release rate 8 */
    xram[at + 6] = 0x00; /* psg_sample starts a note only from a reported write */
}

static void aud_pull(float *dst, int frames)
{
    for (int i = 0; i < frames; i++)
        aud_render(dst + i * 2 * 64, 64);
}

static bool aud_sounded(const float *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (buf[i] != 0.0f)
            return true;
    return false;
}

UTEST(sst, a_note_in_flight_comes_back)
{
    static float went[64 * 2 * 32], came[64 * 2 * 32];
    aud_set_enabled(true);

    aud_note(0x3000);
    ASSERT_TRUE(psg_xreg(0x3000));
    ASSERT_EQ(aud_device(), aud_dev_psg);
    aud_poke(0x3006, 0x01); /* pan centre, gate on */
    aud_pull(went, 8);
    ASSERT_EQ(take(), (const char *)NULL);

    aud_pull(went, 32);

    ASSERT_TRUE(opl_xreg(0x4000));
    ASSERT_EQ(aud_device(), aud_dev_opl);
    ASSERT_EQ(psg_xaddr_get(), (uint16_t)0xFFFF);
    aud_pull(came, 16);

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(aud_device(), aud_dev_psg);
    ASSERT_EQ(psg_xaddr_get(), (uint16_t)0x3000);
    ASSERT_EQ(opl_xaddr_get(), (uint16_t)0xFFFF);
    aud_pull(came, 32);
    ASSERT_TRUE(aud_sounded(went, sizeof went / sizeof *went));
    ASSERT_EQ(memcmp(went, came, sizeof went), 0);
}

UTEST(sst, an_opl_chord_comes_back)
{
    static float went[64 * 2 * 32], came[64 * 2 * 32];
    aud_set_enabled(true);
    ASSERT_TRUE(opl_xreg(0x4000));
    static const uint8_t regs[][2] = {
        {0x20, 0x21}, {0x23, 0x21}, {0x40, 0x1A}, {0x43, 0x00},
        {0x60, 0xF0}, {0x63, 0xF0}, {0x80, 0x77}, {0x83, 0x77},
        {0xA0, 0x98}, {0xB0, 0x31},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        aud_poke(0x4000 + regs[i][0], regs[i][1]);
    aud_pull(went, 8);
    ASSERT_EQ(take(), (const char *)NULL);
    aud_pull(went, 32);

    aud_note(0x3000);
    ASSERT_TRUE(psg_xreg(0x3000));
    aud_poke(0x3006, 0x01);
    ASSERT_EQ(opl_xaddr_get(), (uint16_t)0xFFFF);
    aud_pull(came, 16);

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(aud_device(), aud_dev_opl);
    aud_pull(came, 32);
    ASSERT_TRUE(aud_sounded(went, sizeof went / sizeof *went));
    ASSERT_EQ(memcmp(went, came, sizeof went), 0);
}

UTEST(sst, pcm_sounds_with_the_opl_and_comes_back)
{
    static float went[64 * 2 * 32], came[64 * 2 * 32];
    aud_set_enabled(true);
    ASSERT_TRUE(opl_xreg(0x4000));
    static const uint8_t regs[][2] = {
        {0x20, 0x21}, {0x23, 0x21}, {0x40, 0x1A}, {0x43, 0x00},
        {0x60, 0xF0}, {0x63, 0xF0}, {0x80, 0x77}, {0x83, 0x77},
        {0xA0, 0x98}, {0xB0, 0x31},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        aud_poke(0x4000 + regs[i][0], regs[i][1]);

    /* Mono signed 8-bit at 8 kHz in a 1 KB ring, all but one byte full,
     * which is more than every pull below consumes. */
    for (int i = 0; i < 1024; i++)
        xram[0x5008 + i] = (uint8_t)(i * 37);
    xram[0x5000] = 0xFF;
    xram[0x5001] = 0x03;
    xram[0x5002] = 0x03;
    xram[0x5003] = 10;
    xram[0x5004] = 0x40;
    xram[0x5005] = 0x1F;
    ASSERT_TRUE(pcm_xreg(0x5000));
    ASSERT_EQ(aud_device(), aud_dev_opl);
    ASSERT_EQ(opl_xaddr_get(), (uint16_t)0x4000);

    aud_pull(went, 8);
    ASSERT_EQ(take(), (const char *)NULL);
    aud_pull(went, 32);

    ASSERT_TRUE(pcm_xreg(0xFFFF));
    ASSERT_EQ(aud_device(), aud_dev_opl);
    ASSERT_TRUE(opl_xreg(0xFFFF));
    aud_pull(came, 16);

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(aud_device(), aud_dev_opl);
    aud_pull(came, 32);
    ASSERT_TRUE(aud_sounded(went, sizeof went / sizeof *went));
    ASSERT_EQ(memcmp(went, came, sizeof went), 0);

    /* Giving up the OPL leaves PCM playing, and stopping PCM then leaves
     * nothing. */
    ASSERT_TRUE(opl_xreg(0xFFFF));
    ASSERT_EQ(aud_device(), aud_dev_none);
    aud_pull(came, 2);
    aud_pull(came, 8);
    ASSERT_TRUE(aud_sounded(came, 64 * 2 * 8));
    ASSERT_TRUE(pcm_xreg(0xFFFF));
    aud_pull(came, 2);
    aud_pull(came, 8);
    ASSERT_FALSE(aud_sounded(came, 64 * 2 * 8));
}

/* Whether the machine is on the console canvas, the console is w by h, and
 * the terminal on it has rows. */
static bool on_console(int w, int h, int rows)
{
    int cw, ch, vw, vh;
    vga_console_size(&cw, &ch);
    vga_canvas_size(&vw, &vh);
    term_view_t tv;
    term_view(&tv);
    return vga_canvas_is_console() && cw == w && ch == h &&
           vw == w && vh == h && tv.height == rows;
}

/* The host chooses the console's size, and a savestate made under one size
 * brings it to a session the host started at another. It stays for the rest
 * of the session, so the reset and the stop after the load put the console
 * back at the savestate's size and not the host's. Each is handed a program
 * left on another canvas, so the console it leaves is one it laid out. */
UTEST(sst, a_console_size_comes_back_and_stays_for_the_session)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    vga_set_console_size(640, 400);
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
    emu_frames(2);
    ASSERT_TRUE(on_console(640, 400, 25));
    ASSERT_EQ(take(), (const char *)NULL);

    vga_set_console_size(640, 480);
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
    ASSERT_TRUE(on_console(640, 480, 30));

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(on_console(640, 400, 25));

    ASSERT_TRUE(vga_canvas_select(vga_canvas_320_240));
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    emu_frames(2);
    ASSERT_TRUE(on_console(640, 400, 25));

    ASSERT_TRUE(vga_canvas_select(vga_canvas_320_180));
    sys_stop();
    emu_frames(2);
    ASSERT_TRUE(on_console(640, 400, 25));

    vga_set_console_size(640, 480);
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
    ASSERT_TRUE(on_console(640, 480, 30));
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* The VGA row is loaded on its own here, where no scratch copy puts back what
 * a refused row set, so a console that survives a refusal survives by the
 * row's own doing. The row's canvas is at byte 10 and the console's height at
 * 12. The last case is a console a host could give on a canvas no build has,
 * which the row has to refuse before it sets the console. */
UTEST(sst, a_console_no_host_gives_is_refused_before_anything_is_set)
{
    static const struct
    {
        uint16_t canvas, h;
    } bad[] = {
        {vga_canvas_console, 512}, /* the Pico's console on SXGA */
        {vga_canvas_console, 240},
        {VGA_CANVAS_COUNT, 400},
    };
    static uint8_t row[VGA_SST_SIZE], bent[VGA_SST_SIZE];
    ASSERT_TRUE(on_console(640, 480, 30));
    sst_cursor_t c = {row, row + sizeof row, false};
    vga_sst_save(&c, 0);
    ASSERT_TRUE(sst_ok(&c));
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    {
        memcpy(bent, row, sizeof row);
        put_u16(bent + 10, bad[i].canvas);
        put_u16(bent + 12, bad[i].h);
        c = (sst_cursor_t){bent, bent + sizeof bent, false};
        ASSERT_FALSE(vga_sst_load(&c, 0));
        ASSERT_TRUE(on_console(640, 480, 30));
    }
    c = (sst_cursor_t){row, row + sizeof row, false};
    ASSERT_TRUE(vga_sst_load(&c, 0));
    ASSERT_TRUE(on_console(640, 480, 30));
}

UTEST_MAIN_EMU()
