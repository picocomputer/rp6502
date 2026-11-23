/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/xreg.h"
#include "core/ria/ria.h"
#include "core/sys/sst.h"
#include "core/sys/xram.h"
#include "core/wdc/bus.h"
#include "core/aud/bel.h"
#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/wdc/sram.h"
#include "core/com/rln.h"
#include "core/com/com.h"
#include "core/hid/clip.h"
#include "core/hid/paste.h"
#include "core/hid/paste_host.h"
#include "core/hid/typed.h"
#include "core/term/draw.h"
#include "core/term/grid.h"
#include "core/term/vterm.h"
#include "core/vga/canvas.h"
#include "tb_screen.h"
#include "host/host.h"

#include "drivers.h"
#include "emu_boot.h"
#include "stdsys.h"

#include <string.h>

#define O_WR 0x02
#define WHENCE_SET 0
#define WHENCE_CUR 1

static uint8_t blob[1 << 20];
/* Where a load saves the machine it may have to put back. */

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
    ASSERT_EQ(sst_size(), (size_t)251446);
    ASSERT_EQ((unsigned)((blob[6] << 8) | blob[7]), 44u);
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

/* A program's own size is a third grid of the terminal, carried by the RLN
 * chunk; it comes back attached, with its cursor, and is dropped again by a
 * blob made without one. */
UTEST(sst, a_programs_own_size_comes_back)
{
    rln_set_term_width(132);
    term_grid_t *g = vterm.grids[TERM_SCREEN_GRIDS];
    g->screen[0].pos.x = 100;
    ASSERT_EQ(take(), (const char *)NULL);
    rln_set_term_width(0);
    ASSERT_EQ(vterm.grid_count, TERM_SCREEN_GRIDS);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(vterm.grid_count, TERM_SCREEN_GRIDS + 1);
    ASSERT_EQ(vterm.grids[TERM_SCREEN_GRIDS]->width, 132);
    ASSERT_EQ(term_pos(vterm.grids[TERM_SCREEN_GRIDS])->x, 100);
    rln_set_term_width(0);
    ASSERT_EQ(take(), (const char *)NULL);
    rln_set_term_width(60);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(vterm.grid_count, TERM_SCREEN_GRIDS);
}

/* Where mode 0 sits on a canvas comes back with the console's terminal,
 * though the canvas's load programs nothing, and the console placed since
 * goes. */
UTEST(sst, where_mode_0_sits_comes_back)
{
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    ASSERT_TRUE(xreg1(0, 3, 16) && xreg1(0, 4, 224) && xreg1(0, 1, 0));
    const vterm_placement_t was = vterm_placement;
    ASSERT_TRUE(was.placed);
    ASSERT_EQ(was.rows, 26);
    ASSERT_EQ(take(), (const char *)NULL);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    ASSERT_TRUE(vterm_placement.console);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(memcmp(&was, &vterm_placement, sizeof was), 0);
    ASSERT_EQ(vga_get_canvas(), vga_canvas_320_240);
    ASSERT_EQ(vterm.grids[0]->height, 26);
    ASSERT_TRUE(vterm_get_shown(&vterm) == vterm.grids[0]);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* Where mode 0 sits must be where the canvas saved beside it puts it: a
 * record of the 40 on the console canvas, of the console off it, or of rows
 * running past the canvas's foot is refused, though each could stand on
 * another canvas. */
static const char *save_placed(vterm_placement_t p)
{
    const vterm_placement_t was = vterm_placement;
    vterm_placement = p;
    const char *err = take();
    vterm_placement = was;
    return err;
}

UTEST(sst, mode_0_where_its_canvas_does_not_put_it_is_refused)
{
    ASSERT_EQ(save_placed((vterm_placement_t){.cols = 40}), (const char *)NULL);
    ASSERT_NE(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(save_placed((vterm_placement_t){.cols = 80}), (const char *)NULL);
    ASSERT_NE(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    const uint8_t rows = vterm.grids[0]->height;
    ASSERT_EQ(save_placed((vterm_placement_t){.cols = 40, .rows = rows, .row_px = 8,
                                              .begin = (int16_t)(248 - rows * 8),
                                              .placed = true}),
              (const char *)NULL);
    ASSERT_NE(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    ASSERT_EQ(save_placed((vterm_placement_t){.cols = 40, .rows = rows, .row_px = 8,
                                              .begin = (int16_t)(240 - rows * 8),
                                              .placed = true}),
              (const char *)NULL);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* The console's palette comes back with the console's terminal, the
 * special colors with it, and mode 0's faint twins are made from them
 * again. */
UTEST(sst, the_palette_and_its_faint_twins_come_back)
{
    const uint16_t red = SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(200, 100, 40);
    const uint16_t cursor = SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(0, 80, 160);
    const uint16_t paper = SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(240, 230, 200);
    const uint16_t lit = SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 255);
    const uint16_t text = SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 0);
    vterm_set_color(&vterm, 1, red);
    vterm_set_color(&vterm, VTERM_COLOR_CURSOR, cursor);
    vterm_set_color(&vterm, VTERM_COLOR_BG, paper);
    vterm_set_color(&vterm, VTERM_COLOR_HIGHLIGHT_BG, lit);
    vterm_set_color(&vterm, VTERM_COLOR_HIGHLIGHT_FG, text);
    draw_publish();
    ASSERT_EQ(take(), (const char *)NULL);
    vterm_reset_colors(&vterm);
    draw_publish();
    ASSERT_EQ(mode0_color(MODE0_ENTRY(1, true)), mode0_faint(color_256[1]));
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(vterm_palette[1], red);
    ASSERT_EQ(vterm_get_color(&vterm, VTERM_COLOR_CURSOR), cursor);
    ASSERT_EQ(vterm_get_color(&vterm, VTERM_COLOR_BG), paper);
    ASSERT_EQ(vterm_get_color(&vterm, VTERM_COLOR_HIGHLIGHT_BG), lit);
    ASSERT_EQ(vterm_get_color(&vterm, VTERM_COLOR_HIGHLIGHT_FG), text);
    draw_publish();
    ASSERT_EQ(mode0_regs.highlight, MODE0_HIGHLIGHT | lit);
    ASSERT_EQ(mode0_color(MODE0_ENTRY(1, false)), red);
    ASSERT_EQ(mode0_color(MODE0_ENTRY(1, true)), mode0_faint(red));
    ASSERT_EQ(mode0_color(MODE0_ENTRY(MODE0_DEFAULT_BG, false)), paper);
    ASSERT_EQ(mode0_color(MODE0_ENTRY(MODE0_DEFAULT_BG, true)), mode0_faint(paper));
    ASSERT_EQ(mode0_color(MODE0_ENTRY(MODE0_DEFAULT_FG, true)), mode0_faint(color_256[7]));
    vterm_reset_colors(&vterm);
    draw_publish();
}

/* A paste is host text on its way in: a save carries none of it, and a load
 * ends it. */
UTEST(sst, a_load_mid_paste_cancels_it)
{
    char big[TYPED_QUEUE_SIZE * 4];
    memset(big, 'p', sizeof big - 1);
    big[sizeof big - 1] = 0;
    paste_start(big);
    paste_task();
    ASSERT_TRUE(paste_busy());
    ASSERT_EQ(take(), (const char *)NULL);
    ASSERT_TRUE(paste_busy());
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_FALSE(paste_busy());
}

/* All the keyboard row gives, the paste staged as the main loop stages it. */
static size_t keyboard_all(char *out, size_t max)
{
    size_t got = 0;
    for (int i = 0; i < 10000 && got < max; i++)
    {
        paste_task();
        size_t n = com_read(COM_SOURCE_KEYBOARD, out + got, max - got);
        got += n;
        if (!n && !paste_busy())
            break;
    }
    return got;
}

static void say(const char *s)
{
    while (*s)
        term_write(&vterm, (uint8_t)*s++);
}

/* The host's text is left behind, but a program that has read the opening
 * bracket is owed the closing one, which comes after a load. */
UTEST(sst, a_bracketed_host_paste_comes_back_as_its_closing_bracket)
{
    com_rx_clear();
    say("\33[?2004h");
    static char big[TYPED_QUEUE_SIZE * 4];
    memset(big, 'p', sizeof big - 1);
    paste_start(big);
    paste_task();
    char head[8];
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, head, sizeof head), sizeof head);
    ASSERT_EQ(memcmp(head, "\33[200~pp", 8), 0);
    ASSERT_EQ(take(), (const char *)NULL);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    char rest[16] = {0};
    ASSERT_EQ(keyboard_all(rest, sizeof rest - 1), (size_t)6);
    ASSERT_STREQ(rest, "\33[201~");
    ASSERT_FALSE(paste_busy());
    say("\33[?2004l");
}

/* Half an opening bracket read is finished, then closed. */
UTEST(sst, a_half_read_opening_bracket_is_finished_after_a_load)
{
    com_rx_clear();
    say("\33[?2004h");
    paste_start("text");
    paste_task();
    char head[3];
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, head, sizeof head), sizeof head);
    ASSERT_EQ(take(), (const char *)NULL);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    char rest[16] = {0};
    ASSERT_EQ(keyboard_all(rest, sizeof rest - 1), (size_t)9);
    ASSERT_STREQ(rest, "00~\33[201~");
    say("\33[?2004l");
}

/* A bracketed host paste the program has read nothing of goes whole. */
UTEST(sst, an_unread_host_paste_leaves_nothing_after_a_load)
{
    com_rx_clear();
    say("\33[?2004h");
    paste_start("text");
    paste_task();
    ASSERT_EQ(take(), (const char *)NULL);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_FALSE(paste_busy());
    char rest[16];
    ASSERT_EQ(keyboard_all(rest, sizeof rest), (size_t)0);
    say("\33[?2004l");
}

/* A paste of a machine buffer that waits behind host text is saved: after
 * a load the host paste's closing bracket comes, then the whole of it. */
UTEST(sst, a_machine_paste_behind_host_text_comes_back)
{
    com_rx_clear();
    say("\33[?2004h\33[H\33[2Jmine");
    term_job_finish(&vterm);
    clip_copy(CLIP_PRIMARY, vterm_get_shown(&vterm)->shown, 0, 0, 0, 4);
    static char big[TYPED_QUEUE_SIZE * 4];
    memset(big, 'p', sizeof big - 1);
    paste_start(big);
    paste_task();
    char head[8];
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, head, sizeof head), sizeof head);
    clip_paste(CLIP_PRIMARY);
    paste_task();
    ASSERT_EQ(take(), (const char *)NULL);
    clip_reset();
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    char rest[32] = {0};
    ASSERT_EQ(keyboard_all(rest, sizeof rest - 1), (size_t)22);
    ASSERT_STREQ(rest, "\33[201~\33[200~mine\33[201~");
    say("\33[?2004l");
}

/* A paste of the machine's own text is machine state: a load in the middle
 * of one goes on from where the save was, with the buffer it reads. */
UTEST(sst, a_paste_of_the_machine_s_own_text_comes_back)
{
    com_rx_clear();
    for (int y = 0; y < 10; y++)
        for (const char *t = "\r\n0123456789abcdefghijklmnopqrstuvwxyz"; *t; t++)
            term_write(&vterm, (uint8_t)*t);
    term_job_finish(&vterm);
    const term_grid_t *g = vterm_get_shown(&vterm);
    clip_copy(CLIP_PRIMARY, g->shown, 0, 0, (uint8_t)(g->height - 1), g->width);
    clip_paste(CLIP_PRIMARY);
    paste_task();
    char head[100];
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, head, sizeof head), sizeof head);
    ASSERT_EQ(take(), (const char *)NULL);
    static char rest[2048], again[2048];
    const size_t got = keyboard_all(rest, sizeof rest);
    ASSERT_GT(got, (size_t)TYPED_QUEUE_SIZE);
    clip_reset();
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(keyboard_all(again, sizeof again), got);
    ASSERT_EQ(memcmp(rest, again, got), 0);
    ASSERT_FALSE(paste_busy());
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

/* Channel 0's note but its gate, written once the PSG is mapped, as a
 * program writes it. */
static void aud_note(uint16_t at)
{
    ria_xram_write(at + 0, 0x00);
    ria_xram_write(at + 1, 0x04); /* 0x0400 thirds of a hertz, about 341 Hz */
    ria_xram_write(at + 2, 0x80); /* duty */
    ria_xram_write(at + 3, 0x04); /* full volume (the table descends), attack rate 4 */
    ria_xram_write(at + 4, 0x28); /* sustain two steps down, decay rate 8 */
    ria_xram_write(at + 5, 0x08); /* sine, release rate 8 */
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

    ASSERT_TRUE(xreg0(1, 0, 0x3000));
    aud_note(0x3000);
    ria_xram_write(0x3006, 0x01); /* pan centre, gate on */
    aud_pull(went, 1);
    /* Still on the ring when the save starts, for the next sample. */
    ria_xram_write(0x3001, 0x05);
    ASSERT_EQ(take(), (const char *)NULL);

    const size_t n = aud_pull(went, 3);

    /* A map zeroes the registers, so only the PSG's row brings them back. */
    ASSERT_TRUE(xreg0(1, 0, 0x3000));
    ASSERT_TRUE(xreg0(1, 1, 0x4000));
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
    ASSERT_TRUE(xreg0(1, 1, 0x4000));
    static const uint8_t regs[][2] = {
        {0x20, 0x21}, {0x23, 0x21}, {0x40, 0x1A}, {0x43, 0x00},
        {0x60, 0xF0}, {0x63, 0xF0}, {0x80, 0x77}, {0x83, 0x77},
        {0xA0, 0x98}, {0xB0, 0x31},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        ria_xram_write(0x4000 + regs[i][0], regs[i][1]);
    aud_pull(went, 1);
    ASSERT_EQ(take(), (const char *)NULL);
    const size_t n = aud_pull(went, 3);

    ASSERT_TRUE(xreg0(1, 0, 0x3000));
    aud_note(0x3000);
    ria_xram_write(0x3006, 0x01);
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
    ASSERT_TRUE(xreg0(1, 1, 0x4000));
    static const uint8_t regs[][2] = {
        {0x20, 0x21}, {0x23, 0x21}, {0x40, 0x1A}, {0x43, 0x00},
        {0x60, 0xF0}, {0x63, 0xF0}, {0x80, 0x77}, {0x83, 0x77},
        {0xA0, 0x98}, {0xB0, 0x31},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        ria_xram_write(0x4000 + regs[i][0], regs[i][1]);
    for (int k = 0; k < 300; k++)
        ria_xram_write((uint16_t)(0x4040 + k % 6), (uint8_t)(k & 0x3F));
    aud_pull_lines(went, 40);
    ASSERT_GT(RING_LEN(&opl_queue.ring), 0);
    ASSERT_EQ(take(), (const char *)NULL);
    for (int k = 0; k < 300; k++)
        ria_xram_write((uint16_t)(0x4060 + k % 6), (uint8_t)(0xF0 | (k & 15)));
    const size_t n = aud_pull(went, 3);
    const uint32_t drops = aud_opl_overruns();
    ASSERT_GT(drops, 0u);

    ASSERT_TRUE(xreg0(1, 1, 0x4000));
    aud_pull(came, 1);

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    for (int k = 0; k < 300; k++)
        ria_xram_write((uint16_t)(0x4060 + k % 6), (uint8_t)(0xF0 | (k & 15)));
    ASSERT_EQ(aud_pull(came, 3), n);
    ASSERT_TRUE(aud_sounded(went, n * 2));
    ASSERT_EQ(memcmp(went, came, n * 2 * sizeof *went), 0);
    ASSERT_EQ(aud_opl_overruns(), drops);
}

/* A blob whose OPL chunk is version 1, as older builds wrote it, with its sum
 * made good again: the load refuses it and leaves the machine as it was. */
UTEST(sst, an_old_opl_chunk_is_refused)
{
    ASSERT_EQ(take(), (const char *)NULL);
    const size_t total = sst_size();
    size_t at = 0;
    while (at + 6 <= total && memcmp(blob + at, "OPL_", 4))
        at++;
    ASSERT_LT(at + 6, total);
    blob[at + 5] ^= 1; /* the chunk's version */
    const uint32_t sum = host_crc32(0, blob, total - 8);
    for (int i = 0; i < 4; i++)
        blob[total - 8 + i] = (uint8_t)(sum >> (24 - 8 * i));
    ASSERT_NE(sst_load(blob, total, 0), (const char *)NULL);
    ASSERT_FALSE(sys_active());
}

UTEST(sst, both_engines_come_back)
{
    static float out[AUD_FRAME_MAX * 2];
    aud_set_enabled(true);
    ASSERT_TRUE(xreg0(1, 0, 0x3000));
    ASSERT_TRUE(xreg0(1, 1, 0x4000));
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

    ASSERT_TRUE(xreg0(1, 0, 0x3000));
    aud_note(0x3000);
    ria_xram_write(0x3003, 0x64); /* six steps down, attack rate 4 */
    ria_xram_write(0x3006, 0x01);

    ASSERT_TRUE(xreg0(1, 1, 0x4000));
    static const uint8_t regs[][2] = {
        {0x20, 0xE1}, {0x23, 0xE1}, {0x40, 0x1A}, {0x43, 0x08},
        {0x60, 0xF0}, {0x63, 0xF0}, {0x80, 0x77}, {0x83, 0x77},
        {0xBD, 0xC0}, {0xA0, 0x98}, {0xB0, 0x31},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        ria_xram_write(0x4000 + regs[i][0], regs[i][1]);

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
    tb_screen_t tv;
    tb_screen(&tv);
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

/* The CANV row is loaded on its own here. Its console height is the I16 at
 * byte 0 and its canvas the U16 at byte 2. The last case is a console a host
 * could give on a canvas no build has. The row refuses each, and the reset
 * that follows a refused load leaves a console a host gives: 640x480 in
 * place of one no host gives, and the 640x400 the row wrote, which a host
 * does give. */
UTEST(sst, a_console_no_host_gives_is_refused)
{
    static const struct
    {
        uint16_t canvas, h;
        int after;
    } bad[] = {
        {vga_canvas_console, 512, 480}, /* the Pico's console on SXGA */
        {vga_canvas_console, 240, 480},
        {VGA_CANVAS_COUNT, 400, 400},
    };
    static uint8_t row[64], bent[64];
    ASSERT_LE((size_t)canvas_sst.size, sizeof row);
    ASSERT_TRUE(on_console(640, 480, 30));
    sst_cursor_t c = {row, row + canvas_sst.size, false, NULL};
    canvas_sst.save(&c, 0);
    ASSERT_TRUE(sst_ok(&c));
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    {
        memcpy(bent, row, canvas_sst.size);
        put_u16(bent + 0, bad[i].h);
        put_u16(bent + 2, bad[i].canvas);
        c = (sst_cursor_t){bent, bent + canvas_sst.size, false, NULL};
        ASSERT_FALSE(canvas_sst.load(&c, 0));
        sys_reset();
        ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
        emu_frames(2);
        ASSERT_TRUE(on_console(640, bad[i].after, bad[i].after / 16));
        vga_set_console_size(640, 480);
        ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
    }
    c = (sst_cursor_t){row, row + canvas_sst.size, false, NULL};
    ASSERT_TRUE(canvas_sst.load(&c, 0));
    ASSERT_TRUE(on_console(640, 480, 30));
}

/* Every row's id and slot size, in the order its chunk lies in the blob, from
 * the driver list core/sys/sst.c walks. A chunk's header gives its id,
 * version and the bytes it used but not its slot, so a walk from one chunk to
 * the next needs the slot from the row. */
#define ROWS_MAX 64
static char row_id[ROWS_MAX][5];
static uint32_t row_size[ROWS_MAX];

static void row_add(unsigned *n, const sst_row_t *r)
{
    if (*n < ROWS_MAX)
    {
        memcpy(row_id[*n], r->id, sizeof r->id);
        row_size[*n] = r->table->size;
    }
    ++*n;
}

/* A row is a compound literal inside the call, whose parentheses keep its
 * commas from splitting the driver list, as in core/sys/sst.c. */
static unsigned rows(void)
{
    unsigned n = 0;
#define SST(id, name) row_add(&n, &(const sst_row_t){id, &name##_sst});
#include "core/sys/driver_walk.h"
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
    return n;
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           p[3];
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* What a boot of the fixture printed: the rows from the one under the fence
 * that boot_fixture writes before it starts the program down to the row the
 * cursor is on, each row's glyphs as text with its trailing blanks cut. */
#define FENCE "-- sst fence --"
typedef struct
{
    int rows;
    char row[TERM_MAX_HEIGHT][TERM_MAX_WIDTH + 1];
} boot_text_t;

static void boot_row(char *dst, uint8_t y, uint8_t width)
{
    const mode0_cell_t *cell = tb_screen_row(y);
    int end = 0;
    for (int x = 0; x < width; x++)
    {
        const uint16_t g = cell[x].glyph;
        dst[x] = g == 0 ? ' ' : g < 0x80 ? (char)g : '?';
        if (dst[x] != ' ')
            end = x + 1;
    }
    dst[end] = '\0';
}

/* False until the program has printed its first question under the last
 * fence on the screen. */
static bool boot_text(boot_text_t *t)
{
    static char line[TERM_MAX_WIDTH + 1];
    draw_task();
    tb_screen_t v;
    tb_screen(&v);
    int fence = -1, asked = -1;
    for (int y = 0; y < v.height; y++)
    {
        boot_row(line, (uint8_t)y, v.width);
        if (strstr(line, FENCE))
        {
            fence = y;
            asked = -1;
        }
        else if (fence >= 0 && strstr(line, "Would you like instructions?"))
            asked = y;
    }
    if (asked < 0 || v.cursor_y < asked)
        return false;
    t->rows = v.cursor_y - fence;
    for (int i = 0; i < t->rows; i++)
        boot_row(t->row[i], (uint8_t)(fence + 1 + i), v.width);
    return true;
}

/* A fence written to the terminal, then the fixture booted and run until it
 * asks its first question, and what it printed under the fence. The fence
 * tells this boot's output from an earlier one's, which reads the same. */
static bool boot_fixture(boot_text_t *t)
{
    static const char fence[] = "\r\n" FENCE "\r\n";
    for (const char *s = fence; *s; s++)
        term_write(&vterm, (uint8_t)*s);
    if (!emu_restart(TEST_FIXTURE))
        return false;
    for (int i = 0; i < 100; i++)
    {
        emu_frames(10);
        if (boot_text(t))
        {
            /* Whatever it prints after the question, it has printed. */
            emu_frames(10);
            return boot_text(t) && sys_running();
        }
    }
    return false;
}

/* A row whose load fails partway leaves every field it wrote before the
 * failure holding what the blob gave it, the rows before it loaded and the
 * rows after it as they were. The load resets the machine, and the next
 * boot of a program has to run as it does on a fresh machine.
 *
 * Each chunk in turn is cut to half the bytes it used, so its row runs out
 * partway, once as it was saved and once with what is left before the cut
 * overwritten with 0xA5, so the fields the row writes before it fails hold
 * values no machine had. SST_TRUSTED skips the payload sum, which would
 * otherwise refuse the blob before any row runs. A row that takes the short
 * chunk is put back with the whole blob and skipped, but most rows must
 * refuse. */
UTEST(sst, a_row_refused_partway_leaves_a_machine_that_boots_again)
{
    static uint8_t clean[sizeof blob];
    static boot_text_t ref, got;
    static char label[64];
    ASSERT_TRUE(boot_fixture(&ref));
    ASSERT_GT(ref.rows, 1);
    ASSERT_EQ(take(), (const char *)NULL);
    const size_t total = sst_size();
    memcpy(clean, blob, total);

    const unsigned n = rows();
    ASSERT_LE(n, (unsigned)ROWS_MAX);
    ASSERT_EQ((unsigned)((clean[6] << 8) | clean[7]), n);
    unsigned tried = 0, refused = 0;
    size_t at = 18; /* the blob's header */
    for (unsigned k = 0; k < n; k++)
    {
        ASSERT_EQ(memcmp(clean + at, row_id[k], 4), 0);
        const uint32_t used = get_u32(clean + at + 6); /* id 4, version 2 */
        for (int garbage = 0; used && garbage < 2; garbage++)
        {
            snprintf(label, sizeof label, "%s cut to %u of %u%s", row_id[k],
                     (unsigned)(used / 2), (unsigned)used,
                     garbage ? ", filled with 0xA5" : "");
            memcpy(blob, clean, total);
            put_u32(blob + at + 6, used / 2);
            if (garbage)
                memset(blob + at + 10, 0xA5, used / 2);
            tried++;
            if (!sst_load(blob, total, SST_TRUSTED))
            {
                ASSERT_EQ_MSG(sst_load(clean, total, SST_TRUSTED),
                              (const char *)NULL, label);
                continue;
            }
            refused++;
            ASSERT_FALSE_MSG(sys_active(), label);
            ASSERT_TRUE_MSG(boot_fixture(&got), label);
            ASSERT_EQ_MSG(got.rows, ref.rows, label);
            for (int i = 0; i < ref.rows; i++)
                ASSERT_STREQ_MSG(got.row[i], ref.row[i], label);
        }
        at += 10 + row_size[k];
    }
    ASSERT_EQ(at, total - 8); /* the trailer */
    ASSERT_GT(refused * 2, tried);
}

UTEST_MAIN_EMU()
