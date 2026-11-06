/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sst.h"
#include "core/sys/xram.h"
#include "core/wdc/bus.h"
#include "core/aud/bel.h"
#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/wdc/sram.h"
#include "core/term/term.h"
#include "host/host.h"

#include "emu_boot.h"
#include "stdsys.h"

#include <string.h>

#define O_WR 0x02
#define WHENCE_SET 0
#define WHENCE_CUR 1

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
    ASSERT_EQ(sst_size(), (size_t)226663);
    ASSERT_EQ((unsigned)((blob[6] << 8) | blob[7]), 27u);
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

/* What rw_write does, without a 6502 to do it: the engine mapped on the
 * page, if one is, then XRAM. */
static void aud_poke(uint16_t at, uint8_t val)
{
    aud_xram_write(at, val);
    xram[at] = val;
}

/* Channel 0's note but its gate, written once the PSG is mapped, as a
 * program writes it. */
static void aud_note(uint16_t at)
{
    aud_poke(at + 0, 0x00);
    aud_poke(at + 1, 0x04); /* 0x0400 thirds of a hertz, about 341 Hz */
    aud_poke(at + 2, 0x80); /* duty */
    aud_poke(at + 3, 0x04); /* full volume (the table descends), attack rate 4 */
    aud_poke(at + 4, 0x28); /* sustain two steps down, decay rate 8 */
    aud_poke(at + 5, 0x08); /* sine, release rate 8 */
}

/* The most sound a frame makes at 48 kHz. */
#define AUD_FRAME_MAX 801
#define AUD_PULL_MAX (AUD_FRAME_MAX * 4)

/* What the machine made since the last take, as a sink on its thread takes
 * it: everything, up to a frame's worth, and older sound is dropped. */
static size_t aud_take(float *dst)
{
    return (size_t)aud_render(dst, AUD_FRAME_MAX);
}

/* Frames run and their sound taken. Returns the frames of sound. */
static size_t aud_pull(float *dst, int frames)
{
    size_t at = 0;
    for (int i = 0; i < frames; i++)
    {
        emu_frames(1);
        at += aud_take(dst + at * 2);
    }
    return at;
}

/* Scanlines run short of a frame, the mix stepped up to them, and their sound
 * taken. */
static size_t aud_pull_lines(float *dst, int lines)
{
    for (int i = 0; i < lines; i++)
    {
        sys_task();
        sys_io_task();
        sys_commit();
    }
    aud_step();
    return aud_take(dst);
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
    static float went[AUD_PULL_MAX * 2], came[AUD_PULL_MAX * 2];
    aud_set_enabled(true);
    aud_stop(); /* an earlier test may have left an engine mapped */

    ASSERT_TRUE(psg_xreg(0x3000));
    aud_note(0x3000);
    aud_poke(0x3006, 0x01); /* pan centre, gate on */
    aud_pull(went, 1);
    /* Still on the ring when the save starts, for the next sample. */
    aud_poke(0x3001, 0x05);
    ASSERT_EQ(take(), (const char *)NULL);

    const size_t n = aud_pull(went, 3);

    /* A map zeroes the registers, so only the PSG's row brings them back. */
    ASSERT_TRUE(psg_xreg(0x3000));
    ASSERT_TRUE(opl_xreg(0x4000));
    ASSERT_EQ(psg_xaddr_get(), (uint16_t)0x3000);
    aud_pull(came, 2);

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(psg_xaddr_get(), (uint16_t)0x3000);
    ASSERT_EQ(opl_xaddr_get(), (uint16_t)0xFFFF);
    ASSERT_EQ(aud_pull(came, 3), n);
    ASSERT_TRUE(aud_sounded(went, n * 2));
    ASSERT_EQ(memcmp(went, came, n * 2 * sizeof *went), 0);
}

UTEST(sst, an_opl_chord_comes_back)
{
    static float went[AUD_PULL_MAX * 2], came[AUD_PULL_MAX * 2];
    aud_set_enabled(true);
    aud_stop(); /* an earlier test may have left an engine mapped */
    ASSERT_TRUE(opl_xreg(0x4000));
    static const uint8_t regs[][2] = {
        {0x20, 0x21}, {0x23, 0x21}, {0x40, 0x1A}, {0x43, 0x00},
        {0x60, 0xF0}, {0x63, 0xF0}, {0x80, 0x77}, {0x83, 0x77},
        {0xA0, 0x98}, {0xB0, 0x31},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        aud_poke(0x4000 + regs[i][0], regs[i][1]);
    aud_pull(went, 1);
    ASSERT_EQ(take(), (const char *)NULL);
    const size_t n = aud_pull(went, 3);

    ASSERT_TRUE(psg_xreg(0x3000));
    aud_note(0x3000);
    aud_poke(0x3006, 0x01);
    ASSERT_EQ(opl_xaddr_get(), (uint16_t)0x4000);
    aud_pull(came, 2);

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(opl_xaddr_get(), (uint16_t)0x4000);
    ASSERT_EQ(psg_xaddr_get(), (uint16_t)0xFFFF);
    ASSERT_EQ(aud_pull(came, 3), n);
    ASSERT_TRUE(aud_sounded(went, n * 2));
    ASSERT_EQ(memcmp(went, came, n * 2 * sizeof *went), 0);
}

/* A burst too big for the OPL2's queue, saved while the queue drains: the
 * load brings back the writes still waiting and the drop count, so a second
 * burst after it plays and drops as it did after the save. */
UTEST(sst, an_opl_burst_comes_back_mid_flight)
{
    static float went[AUD_PULL_MAX * 2], came[AUD_PULL_MAX * 2];
    aud_set_enabled(true);
    aud_stop(); /* an earlier test may have left an engine mapped */
    ASSERT_TRUE(opl_xreg(0x4000));
    static const uint8_t regs[][2] = {
        {0x20, 0x21}, {0x23, 0x21}, {0x40, 0x1A}, {0x43, 0x00},
        {0x60, 0xF0}, {0x63, 0xF0}, {0x80, 0x77}, {0x83, 0x77},
        {0xA0, 0x98}, {0xB0, 0x31},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        aud_poke(0x4000 + regs[i][0], regs[i][1]);
    for (int k = 0; k < 300; k++)
        aud_poke((uint16_t)(0x4040 + k % 6), (uint8_t)(k & 0x3F));
    aud_pull_lines(went, 40);
    ASSERT_GT(opl_queue.count, 0);
    ASSERT_EQ(take(), (const char *)NULL);
    for (int k = 0; k < 300; k++)
        aud_poke((uint16_t)(0x4060 + k % 6), (uint8_t)(0xF0 | (k & 15)));
    const size_t n = aud_pull(went, 3);
    const uint32_t drops = aud_opl_drops();
    ASSERT_GT(drops, 0u);

    ASSERT_TRUE(opl_xreg(0x4000));
    aud_pull(came, 1);

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    for (int k = 0; k < 300; k++)
        aud_poke((uint16_t)(0x4060 + k % 6), (uint8_t)(0xF0 | (k & 15)));
    ASSERT_EQ(aud_pull(came, 3), n);
    ASSERT_TRUE(aud_sounded(went, n * 2));
    ASSERT_EQ(memcmp(went, came, n * 2 * sizeof *went), 0);
    ASSERT_EQ(aud_opl_drops(), drops);
}

/* A blob whose OPL chunk is version 1, as older builds wrote it, with its sum
 * made good again: the load refuses it and leaves the machine as it was. */
UTEST(sst, an_old_opl_chunk_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    const size_t total = sst_size();
    size_t at = 0;
    while (at + 6 <= total && memcmp(blob + at, "OPL_\x00\x02", 6))
        at++;
    ASSERT_LT(at + 6, total);
    blob[at + 5] = 1;
    const uint32_t sum = host_crc32(0, blob, total - 8);
    for (int i = 0; i < 4; i++)
        blob[total - 8 + i] = (uint8_t)(sum >> (24 - 8 * i));
    ASSERT_NE(sst_load(blob, total, 0), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, both_engines_come_back)
{
    static float out[AUD_FRAME_MAX * 2];
    aud_set_enabled(true);
    ASSERT_TRUE(psg_xreg(0x3000));
    ASSERT_TRUE(opl_xreg(0x4000));
    aud_take(out);
    ASSERT_EQ(take(), (const char *)NULL);
    aud_stop();
    ASSERT_EQ(psg_xaddr_get(), (uint16_t)0xFFFF);
    ASSERT_EQ(opl_xaddr_get(), (uint16_t)0xFFFF);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(psg_xaddr_get(), (uint16_t)0x3000);
    ASSERT_EQ(opl_xaddr_get(), (uint16_t)0x4000);
}

/* Frames run and their sound taken and let go. */
static void aud_pass(int frames)
{
    static float out[AUD_FRAME_MAX * 2];
    for (int i = 0; i < frames; i++)
        aud_pull(out, 1);
}

/* Whether the bell is silent once the teletype's restrike has had its
 * 800 ms. */
static bool aud_rung_out(void)
{
    static float out[AUD_FRAME_MAX * 2];
    aud_pass(56);
    return !aud_sounded(out, aud_pull(out, 1) * 2);
}

/* Whether a bell an earlier case left sounding has rung out, so a case
 * that strikes its own starts from silence. */
static bool aud_bell_quiet(void)
{
    bool quiet = false;
    for (int i = 0; i < 8 && !quiet; i++)
        quiet = aud_rung_out();
    return quiet;
}

/* The bell struck twice, as a held BEL strikes it, and saved as the first
 * strike releases: the load brings back the tone, its envelope and the
 * restrike waiting in the queue. */
UTEST(sst, a_bell_mid_tone_comes_back)
{
    static float went[AUD_PULL_MAX * 2], came[AUD_PULL_MAX * 2];
    aud_set_enabled(true);
    aud_stop(); /* an earlier test may have left an engine mapped */
    ASSERT_TRUE(aud_bell_quiet());
    bel_add(&bel_teletype);
    bel_add(&bel_teletype);
    aud_pass(5); /* past the release at 20 ms, short of the restrike at 100 */
    ASSERT_EQ(take(), (const char *)NULL);
    const size_t n = aud_pull(went, 3);

    bel_add(&bel_nfc_fail);
    aud_pull(came, 2);

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(aud_pull(came, 3), n);
    ASSERT_TRUE(aud_sounded(went, n * 2));
    ASSERT_EQ(memcmp(went, came, n * 2 * sizeof *went), 0);
    ASSERT_TRUE(aud_rung_out());
}

/* A PCM: stream from a table at $1000 of XRAM, looped: 16-bit stereo, a
 * triangle 50 frames long on the left and the same inverted at half the
 * size on the right. */
#define AUD_TABLE 0x1000
#define AUD_TABLE_LEN (40 * 50 * 4)
static uint16_t aud_table_at;

static void aud_table(void)
{
    for (int i = 0; i < AUD_TABLE_LEN / 4; i++)
    {
        const int t = i % 50;
        const int16_t l = (int16_t)((t < 25 ? t : 50 - t) * 1000 - 12500);
        const int16_t r = (int16_t)(-l / 2);
        const uint16_t at = (uint16_t)(AUD_TABLE + i * 4);
        xram[at] = (uint8_t)l;
        xram[at + 1] = (uint8_t)((uint16_t)l >> 8);
        xram[at + 2] = (uint8_t)r;
        xram[at + 3] = (uint8_t)((uint16_t)r >> 8);
    }
    aud_table_at = 0;
}

/* As much of the looped table as the device takes, from where it left
 * off. */
static void aud_top_up(int fd)
{
    for (;;)
    {
        const int n = ssys_write_xram(fd, (uint16_t)(AUD_TABLE + aud_table_at),
                                      (uint16_t)(AUD_TABLE_LEN - aud_table_at));
        if (n <= 0)
            return;
        aud_table_at = (uint16_t)((aud_table_at + n) % AUD_TABLE_LEN);
        if (aud_table_at)
            return;
    }
}

/* Frames run and their sound taken, the stream topped up after each. */
static size_t aud_pull_pcm(int fd, float *dst, int frames)
{
    size_t at = 0;
    for (int i = 0; i < frames; i++)
    {
        at += aud_pull(dst + at * 2, 1);
        aud_top_up(fd);
    }
    return at;
}

/* The PSG's note, the OPL2's note with vibrato and tremolo, the bell and a
 * PCM: stream, all playing and saved mid-note: the load brings every engine
 * back, so the mix after it is the mix of the run that never saved. */
UTEST(sst, every_engine_comes_back_at_once)
{
    static float went[AUD_PULL_MAX * 2], came[AUD_PULL_MAX * 2];
    aud_set_enabled(true);
    /* The stream's calls are made here as a 6502 makes them, so no program
     * is left running to make its own. */
    sys_stop_now();
    aud_stop(); /* an earlier test may have left an engine mapped */
    ASSERT_TRUE(aud_bell_quiet());

    ASSERT_TRUE(psg_xreg(0x3000));
    aud_note(0x3000);
    aud_poke(0x3003, 0x64); /* six steps down, attack rate 4 */
    aud_poke(0x3006, 0x01);

    ASSERT_TRUE(opl_xreg(0x4000));
    static const uint8_t regs[][2] = {
        {0x20, 0xE1}, {0x23, 0xE1}, {0x40, 0x1A}, {0x43, 0x08},
        {0x60, 0xF0}, {0x63, 0xF0}, {0x80, 0x77}, {0x83, 0x77},
        {0xBD, 0xC0}, {0xA0, 0x98}, {0xB0, 0x31},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        aud_poke(0x4000 + regs[i][0], regs[i][1]);

    aud_table();
    const int fd = ssys_open("PCM:22050,16,2", O_WR);
    ASSERT_TRUE(fd >= 0);
    aud_top_up(fd);

    bel_add(&bel_teletype);
    bel_add(&bel_teletype);
    aud_pull_pcm(fd, went, 3); /* the restrike at 100 ms is still to come */
    const uint16_t saved_at = aud_table_at;
    const int32_t level = ssys_lseek(fd, 0, WHENCE_CUR);
    ASSERT_GT(level, 0);
    ASSERT_EQ(take(), (const char *)NULL);
    const size_t n = aud_pull_pcm(fd, went, 4);

    aud_stop();
    bel_add(&bel_nfc_fail);
    aud_pull_pcm(fd, came, 1);

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    aud_table_at = saved_at;
    ASSERT_EQ(psg_xaddr_get(), (uint16_t)0x3000);
    ASSERT_EQ(opl_xaddr_get(), (uint16_t)0x4000);
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_CUR), level);
    ASSERT_EQ(aud_pull_pcm(fd, came, 4), n);
    ASSERT_TRUE(aud_sounded(went, n * 2));
    ASSERT_EQ(memcmp(went, came, n * 2 * sizeof *went), 0);

    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_SET), 0);
    ASSERT_EQ(ssys_close(fd), 0);
    aud_stop();
    ASSERT_TRUE(aud_rung_out());
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
