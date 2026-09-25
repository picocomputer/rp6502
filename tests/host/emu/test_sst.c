/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sst.h"
#include "core/sys/sys.h"
#include "core/sys/xram.h"
#include "core/wdc/bus.h"
#include "core/com/com.h"
#include "core/api/xreg.h"
#include "core/ria/regs.h"
#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/vga/vga.h"
#include "core/vga/vga_emu.h"
#include "osal/fs.h"
#include <stdio.h>
#include "core/wdc/sram.h"

#include "emu_boot.h"

#include <string.h>

static uint8_t blob[1 << 20];

static uint32_t fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

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
    ASSERT_EQ(sst_size(), (size_t)222870);
    ASSERT_EQ((unsigned)((blob[6] << 8) | blob[7]), 27u);
}

UTEST(sst, a_blob_is_framed)
{
    ASSERT_EQ(take(), (const char *)NULL);
    ASSERT_EQ(memcmp(blob, "RP65", 4), 0);
    ASSERT_EQ(memcmp(blob + sst_size() - 4, "56PR", 4), 0);
}

UTEST(sst, a_short_buffer_is_refused_rather_than_overrun)
{
    uint8_t guard[16];
    memset(guard, 0x5A, sizeof guard);
    ASSERT_NE(sst_save(blob, sst_size() - 1, 0), (const char *)NULL);
    for (unsigned i = 0; i < sizeof guard; i++)
        ASSERT_EQ(guard[i], 0x5A);
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

UTEST(sst, an_open_file_comes_back_where_it_was)
{
    char path[] = TEST_SCRATCH "/sst_open.txt";
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    for (int i = 0; i < 64; i++)
        fputc(i, f);
    fclose(f);

    api_errno err = API_EIO;
    int fd = fs_std_open(path, FS_RD, &err);
    ASSERT_GE(fd, 0);
    char buf[8];
    uint32_t got = 0;
    while (fs_std_read(fd, buf, 8, &got, &err) == STD_PENDING)
        ;
    ASSERT_EQ(got, (uint32_t)8);

    sst_cursor_t c = {(uint8_t *)blob, (uint8_t *)blob + sizeof blob, false};
    ASSERT_TRUE(fs_std_ident(fd, &c));
    api_errno ignored;
    fs_std_close(fd, &ignored);

    sst_cursor_t r = {(uint8_t *)blob, c.at, false};
    int again = fs_std_reopen(&r, &err);
    ASSERT_GE(again, 0);
    while (fs_std_read(again, buf, 8, &got, &err) == STD_PENDING)
        ;
    ASSERT_EQ(got, (uint32_t)8);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ((uint8_t)buf[i], (uint8_t)(8 + i));
    fs_std_close(again, &ignored);
    remove(path);
}

UTEST(sst, the_screen_comes_back)
{
    vga_set_framebuffer(fb);
    static const char hello[] = "\33[2Jsavestate\r\n";
    com_stdout_write(hello, sizeof hello - 1);
    emu_frames(2);
    uint32_t before = 0;
    ASSERT_TRUE(vga_frame_crc(&before));
    ASSERT_EQ(take(), (const char *)NULL);

    static const char noise[] = "\33[2JXXXXXXXXXXXXXXXX\r\n";
    com_stdout_write(noise, sizeof noise - 1);
    emu_frames(2);
    uint32_t scribbled = 0;
    ASSERT_TRUE(vga_frame_crc(&scribbled));
    ASSERT_NE(scribbled, before);

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    emu_frames(2);
    uint32_t after = 0;
    ASSERT_TRUE(vga_frame_crc(&after));
    ASSERT_EQ(after, before);
}

UTEST(sst, a_graphics_mode_comes_back)
{
    vga_set_framebuffer(fb);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    for (int i = 0; i < 0x4000; i++)
        xram[0x2000 + i] = (uint8_t)(i * 31 + 7);
    static const uint16_t cfg[] = {320, 240, 0x2000, 0x1000};
    for (int i = 0; i < 4; i++)
    {
        xram[0x1000 + i * 2] = (uint8_t)cfg[i];
        xram[0x1000 + i * 2 + 1] = (uint8_t)(cfg[i] >> 8);
    }
    ASSERT_TRUE(xreg1(0, 2, 3));      /* 8bpp */
    ASSERT_TRUE(xreg1(0, 3, 0x1000)); /* the config struct */
    ASSERT_TRUE(xreg1(0, 4, 0));      /* plane */
    ASSERT_TRUE(xreg1(0, 5, 0));
    ASSERT_TRUE(xreg1(0, 6, 0));
    ASSERT_TRUE(xreg1(0, 1, 3));      /* MODE reads 2-6, so it goes last */
    emu_frames(2);
    uint32_t painted = 0;
    ASSERT_TRUE(vga_frame_crc(&painted));
    ASSERT_EQ(take(), (const char *)NULL);

    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    emu_frames(2);
    uint32_t blank = 0;
    ASSERT_TRUE(vga_frame_crc(&blank));

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    emu_frames(2);
    uint32_t again = 0;
    ASSERT_TRUE(vga_frame_crc(&again));
    ASSERT_EQ(again, painted);
    (void)blank;
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

UTEST(sst, a_round_trip_leaves_the_machine_standing)
{
    ASSERT_EQ(take(), (const char *)NULL);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    emu_frames(2);
}

UTEST_MAIN_EMU()
