/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The savestate's framing: what a blob has to look like before any driver's
 * bytes are believed.
 *
 * Every case here is about the header, the manifest and the trailer, which is
 * the part that decides whether a blob belongs to this build at all. A blob
 * that fails one of these is refused before a byte of the machine is touched,
 * and that is what the scrub proves: the machine is scribbled on first, and
 * has to still hold the scribble afterwards.
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

/* A whole blob, made fresh, so a case can edit one byte of it and watch the
 * loader refuse. */
static uint8_t blob[1 << 20];

/* The screen has to be painted somewhere for a case to hash it. */
static uint32_t fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

static const char *take(void)
{
    return sst_save(blob, sizeof blob, 0);
}

/* Something in the machine that a refused load must not have moved. */
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
    ASSERT_EQ(sst_size(), first); /* a frontend allocates once */
}

/* The total and the chunk count, written down. Both are sums over the roster
 * taken at compile time, so a row whose slot drifts from what its body writes
 * moves one of these -- and a row whose size is accidentally a sizeof moves
 * them between a 32-bit and a 64-bit build of the same source, where the two
 * would otherwise refuse each other's blobs and call it a version skew. */
UTEST(sst, the_shape_is_the_shape)
{
    ASSERT_EQ(take(), (const char *)NULL);
    ASSERT_EQ(sst_size(), (size_t)223385);
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
    ASSERT_NE(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_format_this_build_does_not_know_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    blob[5] = 99;
    ASSERT_NE(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_roster_that_differs_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    blob[12] ^= 0xFF; /* the manifest sum */
    ASSERT_NE(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_blob_that_does_not_add_up_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    blob[sst_size() - 8] ^= 0xFF; /* the payload sum */
    ASSERT_NE(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_blob_that_ends_early_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    memset(blob + sst_size() - 4, 0, 4); /* the end magic */
    ASSERT_NE(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

/* The length in the header is the blob's, and the caller's is only a bound on
 * how much of it arrived. A frontend hands back the length it recorded when
 * the file was written, which is another build's. */
UTEST(sst, a_truncated_blob_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    ASSERT_NE(sst_load(blob, sst_size() - 1, 0, NULL), (const char *)NULL);
    ASSERT_NE(sst_load(blob, 4, 0, NULL), (const char *)NULL);
    ASSERT_NE(sst_load(blob, 0, 0, NULL), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

UTEST(sst, a_machine_that_could_not_have_existed_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    scrub();
    blob[16] = 1; /* starting: a moment inside sys_commit, never a resting state */
    ASSERT_NE(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
    ASSERT_EQ(take(), (const char *)NULL);
    blob[16] = 0;      /* stopped ... */
    blob[17] &= ~1;    /* ... with the reset line up, which cannot happen */
    ASSERT_NE(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    ASSERT_TRUE(scrub_intact());
}

/* A blob that never reaches a disk is checked by whatever carried it, so the
 * payload sum is not computed. The framing still is. */
UTEST(sst, a_trusted_blob_skips_the_sum_and_keeps_the_frame)
{
    ASSERT_EQ(take(), (const char *)NULL);
    blob[sst_size() - 8] ^= 0xFF;
    ASSERT_EQ(sst_load(blob, sst_size(), SST_TRUSTED, NULL), (const char *)NULL);
    blob[0] = 'X';
    ASSERT_NE(sst_load(blob, sst_size(), SST_TRUSTED, NULL), (const char *)NULL);
}

/* The walk, end to end: what a row wrote down is what the machine gets back.
 * The scrub between the two is the whole of the case -- a load into the
 * machine that made the blob proves nothing, because a row that carries
 * nothing passes that trivially. */
UTEST(sst, what_a_row_carries_comes_back)
{
    for (int i = 0; i < 0x100; i++)
        sram[i] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < 0x100; i++)
        xram[i] = (uint8_t)(i * 13 + 5);
    ASSERT_EQ(take(), (const char *)NULL);

    scrub();
    ASSERT_TRUE(scrub_intact());

    ASSERT_EQ(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    for (int i = 0; i < 0x100; i++)
        ASSERT_EQ(sram[i], (uint8_t)(i * 7 + 1));
    for (int i = 0; i < 0x100; i++)
        ASSERT_EQ(xram[i], (uint8_t)(i * 13 + 5));
}

/* The claim the whole change exists for: a program picks up where it was.
 *
 * Two runs of the same program from the same memory. The first is the
 * reference. The second is saved at the same point, scrubbed to nothing, and
 * restored -- and then both are run the same number of frames and asked how
 * many cycles they took. bus_cycles is exact and a frame CRC is not: a
 * program that stopped would paint the same picture forever. */
UTEST(sst, a_program_picks_up_where_it_was)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    emu_frames(20);
    uint64_t at_save = bus_cycles();
    ASSERT_EQ(take(), (const char *)NULL);
    emu_frames(20);
    uint64_t reference = bus_cycles() - at_save;
    ASSERT_GT(reference, (uint64_t)0); /* or this case proved nothing */

    /* Nothing of the program left: fresh fills, and the 6502 held. */
    sram_init();
    xram_init();
    ASSERT_EQ(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    ASSERT_EQ(bus_cycles(), at_save);

    emu_frames(20);
    ASSERT_EQ(bus_cycles() - at_save, reference);
}

/* A descriptor is not the machine's to remember: it is the host's, and the
 * blob carries only the name it was opened by and where it had got to. This
 * is that round trip, driven through the same seam a program uses. */
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
        ASSERT_EQ((uint8_t)buf[i], (uint8_t)(8 + i)); /* it carried on, not restarted */
    fs_std_close(again, &ignored);
    remove(path);
}

/* The screen is the thing a person notices, and it is also the chunk with
 * the most ways to come back wrong: three pointers rebuilt rather than
 * carried, a scroll remap, and a parser that may be mid-escape. */
UTEST(sst, the_screen_comes_back)
{
    vga_set_framebuffer(fb);
    static const char hello[] = "\33[2Jsavestate\r\n";
    com_stdout_write(hello, sizeof hello - 1);
    emu_frames(2);
    uint32_t before = 0;
    ASSERT_TRUE(vga_frame_crc(&before));
    ASSERT_EQ(take(), (const char *)NULL);

    /* Scribble over the screen and let it paint, so a load that carried
     * nothing would be caught. */
    static const char noise[] = "\33[2JXXXXXXXXXXXXXXXX\r\n";
    com_stdout_write(noise, sizeof noise - 1);
    emu_frames(2);
    uint32_t scribbled = 0;
    ASSERT_TRUE(vga_frame_crc(&scribbled));
    ASSERT_NE(scribbled, before);

    ASSERT_EQ(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    emu_frames(2);
    uint32_t after = 0;
    ASSERT_TRUE(vga_frame_crc(&after));
    ASSERT_EQ(after, before);
}

/* The scanline table is the one thing a blob cannot rebuild by replaying
 * what made it: the table is the fold of a sequence of bookings that
 * overwrite one another, and the parameters that produced them are cleared
 * the moment the mode write consumes them. So it rides whole, named by mode
 * and attribute rather than by the addresses this build happens to use. */
UTEST(sst, a_graphics_mode_comes_back)
{
    vga_set_framebuffer(fb);
    /* A canvas and a bitmap mode over it, booked the way a program books. */
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    /* A mode-3 bitmap over the whole canvas. The parameters are staged one
     * register at a time and the mode write consumes the lot, which is also
     * why nothing but the table itself remembers them afterwards. */
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
    ASSERT_TRUE(xreg1(0, 1, 3));      /* MODE: consumes what came before it */
    emu_frames(2);
    uint32_t painted = 0;
    ASSERT_TRUE(vga_frame_crc(&painted));
    ASSERT_EQ(take(), (const char *)NULL);

    /* Forget every booking, which is what a canvas change does. */
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    emu_frames(2);
    uint32_t blank = 0;
    ASSERT_TRUE(vga_frame_crc(&blank));

    ASSERT_EQ(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    emu_frames(2);
    uint32_t again = 0;
    ASSERT_TRUE(vga_frame_crc(&again));
    ASSERT_EQ(again, painted);
    (void)blank;
}

/* Sound is a chunk's hardest case: an engine holds a phase, an envelope and
 * two noise words that no register can be read back to reconstruct, so a
 * blob that restarted it instead of carrying it would click on every rewound
 * frame -- and rewind serializes once a frame.
 *
 * The oracle is the samples themselves. One machine plays on; another is
 * saved, scrubbed to a chip nobody has written, and restored. The two have
 * to make the same bytes. */
/* What the RIA's write engine does when a program stores into the tracked
 * page: the byte lands, and the pair is queued for the sampler to drain.
 * Both engines take their notes this way -- the PSG watches the queue for a
 * gate change, and the OPL takes every register write from it. */
static void aud_poke(uint16_t at, uint8_t val)
{
    xram[at] = val;
    uint8_t next = (uint8_t)(xram_queue_head + 1);
    xram_queue[next][0] = (uint8_t)at;
    xram_queue[next][1] = val;
    xram_queue_head = next;
}

static void aud_note(uint16_t at)
{
    /* A PSG channel block: frequency, an open duty, full attack volume, and
     * the gate on. Written where the pointer will be aimed. */
    for (int i = 0; i < 8 * 8; i++)
        xram[at + i] = 0;
    xram[at + 0] = 0x00;
    xram[at + 1] = 0x04; /* 1024 Hz */
    xram[at + 2] = 0x80; /* duty */
    xram[at + 3] = 0x04; /* full volume (the table descends), attack rate 4 */
    xram[at + 4] = 0x28; /* sustain two steps down, decay rate 8 */
    xram[at + 5] = 0x08; /* sine, release rate 8 */
    xram[at + 6] = 0x00; /* the gate goes on through the queue, below */
}

static void aud_pull(float *dst, int frames)
{
    for (int i = 0; i < frames; i++)
        aud_render(dst + i * 2 * 64, 64);
}

/* Two silences compare equal, so every case here has to say the engine was
 * actually sounding. */
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

    /* Take the engine, sound a note, and let it get somewhere into its
     * envelope before anything is written down. */
    aud_note(0x3000);
    ASSERT_TRUE(psg_xreg(0x3000));
    ASSERT_EQ(aud_device(), aud_dev_psg);
    aud_poke(0x3006, 0x01); /* centre, gate on: the engine watches for this */
    aud_pull(went, 8);
    ASSERT_EQ(take(), (const char *)NULL);

    /* What the saved machine would have gone on to make. */
    aud_pull(went, 32);

    /* Now scrub the engine to a chip nobody has written: a different pointer,
     * a reset, and a run long enough that no phase survives. */
    ASSERT_TRUE(opl_xreg(0x4000));
    ASSERT_EQ(aud_device(), aud_dev_opl);
    ASSERT_EQ(psg_xaddr_get(), (uint16_t)0xFFFF); /* the two park each other */
    aud_pull(came, 16);

    ASSERT_EQ(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    ASSERT_EQ(aud_device(), aud_dev_psg);
    ASSERT_EQ(psg_xaddr_get(), (uint16_t)0x3000);
    ASSERT_EQ(opl_xaddr_get(), (uint16_t)0xFFFF);
    aud_pull(came, 32);
    ASSERT_TRUE(aud_sounded(went, sizeof went / sizeof *went));
    ASSERT_EQ(memcmp(went, came, sizeof went), 0);
}

/* The OPL is the other engine, and its chip is a great deal more state than
 * the PSG's eight channels. Same oracle. */
UTEST(sst, an_opl_chord_comes_back)
{
    static float went[64 * 2 * 32], came[64 * 2 * 32];
    aud_set_enabled(true);
    ASSERT_TRUE(opl_xreg(0x4000));
    /* A note through the register queue the sampler drains: the OPL takes
     * its writes from XRAM rather than from a call. */
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

    /* Hand the mix to the other engine and let the chip be reset under it. */
    aud_note(0x3000);
    ASSERT_TRUE(psg_xreg(0x3000));
    aud_poke(0x3006, 0x01);
    ASSERT_EQ(opl_xaddr_get(), (uint16_t)0xFFFF);
    aud_pull(came, 16);

    ASSERT_EQ(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    ASSERT_EQ(aud_device(), aud_dev_opl);
    aud_pull(came, 32);
    ASSERT_TRUE(aud_sounded(went, sizeof went / sizeof *went));
    ASSERT_EQ(memcmp(went, came, sizeof went), 0);
}

UTEST(sst, a_round_trip_leaves_the_machine_standing)
{
    ASSERT_EQ(take(), (const char *)NULL);
    ASSERT_EQ(sst_load(blob, sst_size(), 0, NULL), (const char *)NULL);
    emu_frames(2); /* and it still runs afterwards */
}

UTEST_MAIN_EMU()
