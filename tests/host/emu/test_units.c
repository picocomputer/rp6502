/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/config.h"
#include "core/sys/random.h"
#include "host/host.h"
#include "core/api/xreg.h"
#include "core/api/arg.h"
#include "core/api/proc.h"
#include "core/str/oem.h"
#include "core/term/font.h"
#include "core/str/str.h"
#include "host/sokol/cli/cli.h"
#include "tb_hostos.h"
#include "core/hid/usage.h"
#include "core/hid/hostkey.h"
#include "core/hid/vtkey.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/com/rln.h"
#include "core/term/vterm.h"
#include "core/term/vterm_mouse.h"
#include "tb_screen.h"
#include "core/vga/vga_emu.h"
#include "core/vga/mode/mode0.h"
#include "core/sys/sys.h"
#include "core/rom/rom.h"
#include "core/wdc/sram.h"
#include "core/sys/xram.h"
#include "core/com/com.h"
#include "utest.h"
#include <stdio.h>
#include <string.h>

/* NumLock is the keyboard's, so a test asks for it with a bit beside a key's
 * modifiers. A ch says the host has no text for the press, and is what a US
 * keyboard then types. */
#define NUM 16

static int t_encode(char out[VTKEY_MAX], uint8_t usage, uint8_t flags)
{
    keyboard_set_locks(flags & NUM ? KEYBOARD_LED_NUMLOCK : 0);
    return vtkey_encode(out, usage, flags & VTKEY_MODIFIERS);
}

/* A first press, its VTKEY_ flags held as the left modifiers of a HID
 * modifier byte. */
static hostkey_result_t t_press(uint8_t usage, uint8_t flags, char ch)
{
    keyboard_set_locks(flags & NUM ? KEYBOARD_LED_NUMLOCK : 0);
    uint8_t modifier = (uint8_t)((flags & VTKEY_SHIFT ? KEYBOARD_MODIFIER_LEFTSHIFT : 0) |
                                 (flags & VTKEY_ALT ? KEYBOARD_MODIFIER_LEFTALT : 0) |
                                 (flags & VTKEY_CTRL ? KEYBOARD_MODIFIER_LEFTCTRL : 0));
    return hostkey_press(usage, modifier, false, ch != 0);
}

UTEST(crc32, known_vectors)
{
    /* 0xCBF43926 is the CRC-32/ISO-HDLC (zlib) check value for "123456789". */
    ASSERT_EQ(host_crc32(0, "123456789", 9), (uint32_t)0xCBF43926u);
    ASSERT_EQ(host_crc32(0, "", 0), (uint32_t)0x00000000u);
}

UTEST(rom, loads)
{
    memset(sram, 0, 0x10000);
    ASSERT_TRUE(rom_load(TEST_FIXTURE));
    ASSERT_EQ(sram[0xFFFC], 0x00);
    ASSERT_EQ(sram[0xFFFD], 0x02);
    ASSERT_NE(sram[0x0200], 0x00);
}

UTEST(rom, rejects_missing_file)
{
    ASSERT_FALSE(rom_load("/nonexistent/definitely-not-a.rp6502"));
}

/* A ROM without a reset vector loads its records, then fails with the reason
 * that the monitor of an RP6502-PICO gives. */
UTEST(rom, rejects_a_rom_without_a_reset_vector)
{
    static const char image[] =
        "#!RP6502\n"
        "$00300 $4 $06EE5D17\n" "\xA9\x2A\xDB\xEA";
    char path[TEST_PATH_MAX];
    snprintf(path, sizeof path, "%s/novector.rp6502", TEST_SCRATCH);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(fwrite(image, 1, sizeof image - 1, f), sizeof image - 1);
    fclose(f);

    memset(sram, 0, 0x10000);
    ASSERT_FALSE(rom_load(path));
    ASSERT_EQ(sram[0x0300], 0xA9);
    ASSERT_STREQ(rom_load_error(), "?ROM has no reset vector\n");
}

UTEST(rom, loads_a_headerless_image)
{
    static const char image[] =
        "#!RP6502\n"
        "$00300 $4 $06EE5D17\n" "\xA9\x2A\xDB\xEA"
        "$0FFFC $2 $D8D04345\n" "\x00\x03";
    char path[512];
    snprintf(path, sizeof path, "%s/headerless.rp6502", TEST_SCRATCH);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(fwrite(image, 1, sizeof image - 1, f), sizeof image - 1);
    fclose(f);

    memset(sram, 0, 0x10000);
    ASSERT_TRUE(rom_load(path));
    ASSERT_EQ(sram[0x0300], 0xA9);
    ASSERT_EQ(sram[0x0302], 0xDB);
    ASSERT_EQ(sram[0xFFFC], 0x00);
    ASSERT_EQ(sram[0xFFFD], 0x03);
}

/* Any shebang naming rp6502 heads a ROM; one that names something else does
 * not. */
UTEST(rom, takes_any_shebang_naming_rp6502)
{
    static const char records[] =
        "$00300 $4 $06EE5D17\n" "\xA9\x2A\xDB\xEA"
        "$0FFFC $2 $D8D04345\n" "\x00\x03";
    char path[TEST_PATH_MAX];
    snprintf(path, sizeof path, "%s/shebang.rp6502", TEST_SCRATCH);

    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fputs("#!/usr/bin/env rp6502-emu\n", f);
    ASSERT_EQ(fwrite(records, 1, sizeof records - 1, f), sizeof records - 1);
    fclose(f);
    memset(sram, 0, 0x10000);
    ASSERT_TRUE(rom_load(path));
    ASSERT_EQ(sram[0x0300], 0xA9);

    f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fputs("#!/bin/sh\n", f);
    ASSERT_EQ(fwrite(records, 1, sizeof records - 1, f), sizeof records - 1);
    fclose(f);
    ASSERT_FALSE(rom_load(path));
}

/* ROM_RECORD_MAX caps a record at 1024 bytes, and this one is 1025. */
UTEST(rom, rejects_a_record_over_the_format_cap)
{
    char path[TEST_PATH_MAX];
    snprintf(path, sizeof path, "%s/overcap.rp6502", TEST_SCRATCH);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    static uint8_t big[1025];
    fputs("#!RP6502\n", f);
    fprintf(f, "$00300 $%X $%X\n", (unsigned)sizeof big,
            (unsigned)host_crc32(0, big, sizeof big));
    fwrite(big, 1, sizeof big, f);
    /* The reset vector is written so that the cap is the only reason the load
     * can fail. */
    uint8_t vec[2] = {0x00, 0x03};
    fprintf(f, "$FFFC $2 $%X\n", (unsigned)host_crc32(0, vec, 2));
    fwrite(vec, 1, 2, f);
    fclose(f);
    ASSERT_FALSE(rom_load(path));
}

UTEST(xreg, device_channel_dispatch)
{
    ASSERT_TRUE(xreg0(0, 0, 0)); /* xreg_ria_keyboard(0x0000) */
    ASSERT_TRUE(xreg1(0, 0, 3)); /* VGA canvas 640x480 */
    /* On channel 15, register 1 is CODE_PAGE and register 2 is not handled. */
    ASSERT_TRUE(xreg1(15, 1, 437));
    ASSERT_FALSE(xreg1(15, 2, 0));
    ASSERT_TRUE(xreg1(5, 0, 0)); /* a write to channels 1-14 cannot fail */
}

/* vga_canvas_geometry is the one list of canvases, so a CANVAS write is
 * accepted for exactly the codes it knows, and the canvas that is selected
 * has the size it gives. */
UTEST(vga, the_geometry_lists_exactly_the_canvases_a_canvas_write_takes)
{
    sys_init();
    int w, h;
    for (uint16_t code = 0; code < VGA_CANVAS_COUNT; code++)
    {
        ASSERT_TRUE(vga_canvas_geometry(code, &w, &h));
        ASSERT_TRUE(xreg1(0, 0, code));
        int cw, ch;
        vga_canvas_size(&cw, &ch);
        ASSERT_EQ(cw, w);
        ASSERT_EQ(ch, h);
        ASSERT_EQ((int)vga_get_canvas(), (int)code);
    }
    static const uint16_t unknown[] = {VGA_CANVAS_COUNT, 0xFFFF};
    for (size_t i = 0; i < sizeof unknown / sizeof unknown[0]; i++)
    {
        ASSERT_FALSE(vga_canvas_geometry(unknown[i], &w, &h));
        ASSERT_FALSE(xreg1(0, 0, unknown[i]));
    }
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

UTEST(vga, the_console_canvas_takes_the_size_the_host_gives_it)
{
    sys_init();
    vga_set_console_size(640, 400);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    int w, h;
    vga_canvas_size(&w, &h);
    ASSERT_EQ(w, 640);
    ASSERT_EQ(h, 400);
    tb_screen_t tv;
    tb_screen(&tv);
    ASSERT_EQ(tv.width, 80);
    ASSERT_EQ(tv.height, 25);

    vga_set_console_size(640, 480);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    vga_canvas_size(&w, &h);
    ASSERT_EQ(h, 480);
    tb_screen(&tv);
    ASSERT_EQ(tv.height, 30);
}

/* With BEGIN and END both 0 the terminal is the tallest that fits, centered,
 * and its width follows the canvas's width alone. */
UTEST(vga, mode0_lays_the_terminal_out_by_the_canvas)
{
    static const struct
    {
        int16_t w, h, begin, end;
        uint8_t cols, rows;
    } want[] = {
        {320, 240, 0, 240, 40, 30},
        {320, 180, 2, 178, 40, 22},
        {320, 200, 0, 200, 40, 25},
        {640, 480, 0, 480, 80, 30},
        {640, 360, 4, 356, 80, 22},
        {640, 400, 0, 400, 80, 25},
        {640, 512, 0, 512, 80, 32},
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
    {
        int16_t begin = 0, end = 0;
        uint8_t cols = 0, rows = 0;
        ASSERT_TRUE(mode0_layout(want[i].w, want[i].h, &begin, &end, &cols, &rows));
        ASSERT_EQ(begin, want[i].begin);
        ASSERT_EQ(end, want[i].end);
        ASSERT_EQ(cols, want[i].cols);
        ASSERT_EQ(rows, want[i].rows);
    }

    /* END 0 alone runs to the bottom, and a span that is not whole rows is
     * refused. */
    int16_t begin = 8, end = 0;
    uint8_t cols, rows;
    ASSERT_TRUE(mode0_layout(320, 240, &begin, &end, &cols, &rows));
    ASSERT_EQ(end, 240);
    ASSERT_EQ(rows, 29);
    begin = 0, end = 100;
    ASSERT_FALSE(mode0_layout(640, 480, &begin, &end, &cols, &rows));
}

/* With no pinned size, the size is the screen's showing grid: the canvas's
 * width picks 40 or 80 columns, and mode 0 sets the rows. */
UTEST(rln, the_size_is_the_terminal_mode_0_lays_out_on_the_canvas)
{
    static const struct
    {
        uint16_t canvas, cols, rows;
    } want[] = {
        {vga_canvas_console, 80, 30},
        {vga_canvas_320_240, 40, 30},
        {vga_canvas_320_180, 40, 22},
        {vga_canvas_640_480, 80, 30},
        {vga_canvas_640_360, 80, 22},
        {vga_canvas_320_200, 40, 25},
        {vga_canvas_640_400, 80, 25},
    };
    sys_init();
    rln_set_term_width(0);
    rln_set_term_height(0);
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
    {
        ASSERT_TRUE(xreg1(0, 0, want[i].canvas));
        if (want[i].canvas != vga_canvas_console)
            ASSERT_TRUE(xreg1(0, 1, 0)); /* mode 0, the tallest that fits */
        ASSERT_EQ(rln_get_term_width(), want[i].cols);
        ASSERT_EQ(rln_get_term_height(), want[i].rows);
    }
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* The emulators put the console back themselves. DISPLAY, register 0 of
 * channel 15, tells a Pico VGA what it is plugged into and nothing here
 * answers it. */
UTEST(vga, a_stop_puts_the_console_back_without_the_display_register)
{
    sys_init();
    ASSERT_FALSE(xreg1(15, 0, 1));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_180));
    ASSERT_TRUE(xreg1(0, 2, 1)); /* a mode program left half assembled */
    vga_stop();
    vga_task();
    ASSERT_TRUE(vga_canvas_is_console());
    ASSERT_EQ(vga_canvas_width(), 640);
    ASSERT_EQ(vga_canvas_height(), 480);
    tb_screen_t tv;
    tb_screen(&tv);
    ASSERT_EQ(tv.width, 80);
    ASSERT_EQ(tv.height, 30);
    uint8_t blob[64];
    ASSERT_LE((size_t)xreg_sst.size, sizeof blob);
    sst_cursor_t c = {blob, blob + xreg_sst.size, false, NULL};
    xreg_sst.save(&c, 0);
    for (size_t i = 0; i < xreg_sst.size; i++)
        ASSERT_EQ(blob[i], 0);
}

/* Where mode 0 sits, as the record says, and the height of the terminal it
 * places. */
static bool placed_at(bool console, uint8_t cols, uint8_t rows, int16_t begin)
{
    const vterm_placement_t *p = &vterm_placement;
    const term_grid_t *g = cols == 40 ? vterm.grids[0] : vterm.grids[1];
    return p->console == console && p->placed && p->cols == cols &&
           p->rows == rows && p->begin == begin && p->row_px == (cols == 40 ? 8 : 16) &&
           g->height == rows && vterm_get_shown(&vterm) == g;
}

/* Programs mode 0 on plane 0 of the canvas selected, from BEGIN to END. */
static bool program_mode0(uint16_t begin, uint16_t end)
{
    return xreg1(0, 3, begin) && xreg1(0, 4, end) && xreg1(0, 1, 0);
}

/* The same call again leaves the record as it was. */
static bool record_is(const vterm_placement_t *was)
{
    return !memcmp(was, &vterm_placement, sizeof *was);
}

/* A canvas places nothing until mode 0 is programmed on it, and the size
 * showing is the canvas's from the select. Mode 0 places the terminal where
 * its BEGIN and END put it, and the same program again changes nothing. */
UTEST(vga, mode_0_places_the_terminal_on_a_canvas)
{
    sys_init();
    ASSERT_TRUE(placed_at(true, 80, 30, 0));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    ASSERT_FALSE(vterm_placement.placed);
    ASSERT_FALSE(vterm_placement.console);
    ASSERT_EQ(vterm_placement.cols, 40);
    ASSERT_TRUE(vterm_get_shown(&vterm) == vterm.grids[0]);
    ASSERT_TRUE(program_mode0(16, 224));
    ASSERT_TRUE(placed_at(false, 40, 26, 16));
    vterm_placement_t was = vterm_placement;
    ASSERT_TRUE(program_mode0(16, 224));
    ASSERT_TRUE(record_is(&was));
    ASSERT_TRUE(placed_at(false, 40, 26, 16));
    ASSERT_TRUE(program_mode0(0, 0));
    ASSERT_TRUE(placed_at(false, 40, 30, 0));

    ASSERT_TRUE(xreg1(0, 0, vga_canvas_640_360));
    ASSERT_FALSE(vterm_placement.placed);
    ASSERT_EQ(vterm_placement.cols, 80);
    ASSERT_TRUE(program_mode0(0, 0));
    ASSERT_TRUE(placed_at(false, 80, 22, 4));
    ASSERT_TRUE(program_mode0(40, 360));
    ASSERT_TRUE(placed_at(false, 80, 20, 40));
    was = vterm_placement;
    ASSERT_TRUE(program_mode0(40, 360));
    ASSERT_TRUE(record_is(&was));
    /* A span that is not whole rows places nothing new. */
    ASSERT_FALSE(program_mode0(40, 100));
    ASSERT_TRUE(record_is(&was));

    /* The same canvas again drops what was placed on it. */
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_640_360));
    ASSERT_FALSE(vterm_placement.placed);
    was = vterm_placement;
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_640_360));
    ASSERT_TRUE(record_is(&was));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* The console canvas places the console from another canvas; from itself
 * it keeps the record, and a stop on the console keeps it too. */
UTEST(vga, the_console_canvas_places_the_console)
{
    sys_init();
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_180));
    ASSERT_TRUE(program_mode0(0, 0));
    ASSERT_TRUE(placed_at(false, 40, 22, 2));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    ASSERT_TRUE(placed_at(true, 80, 30, 0));
    vterm_placement_t was = vterm_placement;
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    ASSERT_TRUE(record_is(&was));
    vga_console_reset();
    ASSERT_TRUE(record_is(&was));
    /* The console canvas is not programmable. */
    ASSERT_FALSE(program_mode0(16, 464));
    ASSERT_TRUE(record_is(&was));
}

/* A console of another size, which the host chooses, is placed at the
 * console reset after it, and the same size again changes nothing. */
UTEST(vga, a_host_console_size_places_the_console_again)
{
    sys_init();
    vga_set_console_size(640, 400);
    vga_console_reset();
    ASSERT_TRUE(placed_at(true, 80, 25, 0));
    vterm_placement_t was = vterm_placement;
    vga_console_reset();
    ASSERT_TRUE(record_is(&was));
    vga_set_console_size(640, 360);
    vga_console_reset();
    ASSERT_TRUE(placed_at(true, 80, 22, 4));
    vga_set_console_size(640, 480);
    vga_console_reset();
    ASSERT_TRUE(placed_at(true, 80, 30, 0));
}

/* A selection of the first two rows of the terminal showing. */
static void select_two_rows(void)
{
    term_grid_t *g = vterm_get_shown(&vterm);
    vterm_select(g, 0, screen_mem(g->shown, 0), 0, screen_mem(g->shown, 1), 4);
}

/* Mode 0 programmed again where it is keeps the selection, and programmed
 * somewhere new clears it; so does another canvas, and the console canvas
 * coming back from one. The console canvas from itself, and a stop on it,
 * keep it. */
UTEST(vga, the_selection_outlives_mode_0_programmed_where_it_is)
{
    sys_init();
    select_two_rows();
    ASSERT_TRUE(vterm_selection.shows);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    vga_console_reset();
    ASSERT_TRUE(vterm_selection.shows);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_640_360));
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_TRUE(program_mode0(40, 360));
    select_two_rows();
    ASSERT_TRUE(program_mode0(40, 360));
    ASSERT_TRUE(vterm_selection.shows);
    ASSERT_TRUE(program_mode0(40, 344));
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_TRUE(program_mode0(56, 360));
    select_two_rows();
    ASSERT_TRUE(program_mode0(40, 344));
    ASSERT_FALSE(vterm_selection.shows);
    select_two_rows();
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    ASSERT_FALSE(vterm_selection.shows);
}

/* On a 320-wide canvas the stop sets the 40-column terminal back to its
 * standard height, which drops a selection there at any other height. */
UTEST(vga, a_stop_drops_a_selection_on_the_40_at_another_height)
{
    sys_init();
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    ASSERT_TRUE(program_mode0(16, 224));
    select_two_rows();
    vterm_RIS_no_clear();
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_TRUE(program_mode0(0, 0));
    select_two_rows();
    vterm_RIS_no_clear();
    ASSERT_TRUE(vterm_selection.shows);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* Channel 0 has eight registers, as on the VGA chip, so a write to $12 sets
 * nothing rather than register 2. */
UTEST(vga, channel_0_ignores_registers_it_lacks)
{
    sys_init();
    ASSERT_TRUE(xreg1(0, 0x12, 1));
    uint8_t blob[64];
    ASSERT_LE((size_t)xreg_sst.size, sizeof blob);
    sst_cursor_t c = {blob, blob + xreg_sst.size, false, NULL};
    xreg_sst.save(&c, 0);
    for (size_t i = 0; i < xreg_sst.size; i++)
        ASSERT_EQ(blob[i], 0);
}

UTEST(gamepad, host_report_encoding)
{
    gamepad_stop();
    ASSERT_FALSE(gamepad_is_mapped());

    ASSERT_TRUE(xreg0(0, 2, 0xFF00)); /* xreg_ria_gamepad(0xFF00) */
    ASSERT_TRUE(gamepad_is_mapped());
    ASSERT_EQ(xram[0xFF00], 0x00);

    gamepad_connect(0, true, GAMEPAD_TYPE_UNKNOWN, false);
    gamepad_host_report(0, 0x01, 0x01, 0x00, 0, -127, 0, 0, 0, 0);
    ASSERT_EQ(xram[0xFF00 + 0], 0x81);          /* dpad up | connected */
    ASSERT_EQ(xram[0xFF00 + 1], 0x01);          /* sticks: left=N, right=center */
    ASSERT_EQ(xram[0xFF00 + 2], 0x01);          /* button0: A */
    ASSERT_EQ(xram[0xFF00 + 3], 0x00);          /* button1 */
    ASSERT_EQ(xram[0xFF00 + 5], (uint8_t)-127); /* ly passthrough */

    gamepad_connect(1, true, GAMEPAD_TYPE_PLAYSTATION, true);
    ASSERT_EQ(xram[0xFF00 + 10], 0xF0); /* connected | sticks | playstation */
    gamepad_connect(1, true, GAMEPAD_TYPE_EASTERN, false);
    ASSERT_EQ(xram[0xFF00 + 10], 0xA0); /* connected | eastern, no sticks */
    gamepad_connect(1, true, GAMEPAD_TYPE_WESTERN, true);
    ASSERT_EQ(xram[0xFF00 + 10], 0xD0); /* connected | sticks | western */

    gamepad_connect(2, true, GAMEPAD_TYPE_UNKNOWN, false);
    gamepad_host_report(2, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0);
    ASSERT_EQ(xram[0xFF00 + 20 + 8], 255);  /* lt forced to full */
    ASSERT_EQ(xram[0xFF00 + 20 + 3], 0x01); /* button1 keeps L2 */
    gamepad_connect(3, true, GAMEPAD_TYPE_UNKNOWN, false);
    gamepad_host_report(3, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 200);
    ASSERT_EQ(xram[0xFF00 + 30 + 3], 0x02); /* rt>deadzone asserts R2 */

    gamepad_connect(0, false, GAMEPAD_TYPE_UNKNOWN, false);
    ASSERT_EQ(xram[0xFF00 + 0], 0x00);
    ASSERT_TRUE(xreg0(0, 2, 0xFFFF));
    ASSERT_FALSE(gamepad_is_mapped());
}

UTEST(tablet, host_wheel_encoding)
{
    tablet_stop();
    ASSERT_FALSE(tablet_is_mapped());

    ASSERT_TRUE(xreg0(0, 3, 0xFF00)); /* xreg_ria_tablet(0xFF00) */
    ASSERT_TRUE(tablet_is_mapped());
    /* Counters, read by subtracting the previous value. */
    const uint8_t wheel = xram[0xFF00 + 2], pan = xram[0xFF00 + 3];

    tablet_host_wheel(3, -2);
    ASSERT_EQ(xram[0xFF00 + 2], (uint8_t)(wheel + 3));
    ASSERT_EQ(xram[0xFF00 + 3], (uint8_t)(pan - 2));

    tablet_host_wheel(-4, 5);
    ASSERT_EQ(xram[0xFF00 + 2], (uint8_t)(wheel - 1)); /* 3 + (-4) */
    ASSERT_EQ(xram[0xFF00 + 3], (uint8_t)(pan + 3));   /* -2 + 5 */

    /* Mapping again keeps the counts, as the mouse's are kept. */
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    tablet_host_wheel(1, 1);
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    ASSERT_EQ(xram[0xFF00 + 2], (uint8_t)wheel);
    ASSERT_EQ(xram[0xFF00 + 3], (uint8_t)(pan + 4));

    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    ASSERT_FALSE(tablet_is_mapped());
}

/* The shape a host shows is the program's, and one it does not know is no
 * cursor. With no program on the tablet, the console's terminal claims it
 * while mode 0 shows, and the shape is the arrow; with mode 0 nowhere,
 * nothing wants the tablet. */
UTEST(tablet, shape_clamp)
{
    sys_init();
    ASSERT_TRUE(vterm_placement.placed);
    ASSERT_TRUE(tablet_wanted());
    ASSERT_EQ(tablet_shape(), TABLET_CURSOR_ARROW);
    /* The terminal does nothing with the wheel, so only a program wants it. */
    ASSERT_FALSE(tablet_wheel_wanted());

    ASSERT_TRUE(xreg0(0, 3, 0xFF00)); /* xreg_ria_tablet(0xFF00) */
    ASSERT_TRUE(tablet_wanted());
    ASSERT_TRUE(tablet_wheel_wanted());
    for (int shape = 0; shape < TABLET_CURSOR_COUNT; ++shape)
    {
        xram[0xFF00 + TABLET_OFF_CONTROL] = (uint8_t)shape;
        ASSERT_EQ(tablet_shape(), shape);
    }
    xram[0xFF00 + TABLET_OFF_CONTROL] = TABLET_CURSOR_COUNT;
    ASSERT_EQ(tablet_shape(), TABLET_CURSOR_OFF);
    xram[0xFF00 + TABLET_OFF_CONTROL] = 0xFF;
    ASSERT_EQ(tablet_shape(), TABLET_CURSOR_OFF);

    /* A program's shape holds on a canvas with no mode 0. */
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    xram[0xFF00 + TABLET_OFF_CONTROL] = TABLET_CURSOR_HAND;
    ASSERT_TRUE(tablet_wanted());
    ASSERT_EQ(tablet_shape(), TABLET_CURSOR_HAND);

    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    ASSERT_FALSE(tablet_wanted());
    ASSERT_FALSE(tablet_wheel_wanted());
    ASSERT_EQ(tablet_shape(), TABLET_CURSOR_OFF);

    /* Mode 0 on the canvas: the terminal claims it there too. */
    ASSERT_TRUE(program_mode0(16, 224));
    ASSERT_TRUE(tablet_wanted());
    ASSERT_EQ(tablet_shape(), TABLET_CURSOR_ARROW);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

static int tablet_contact_y(void)
{
    return xram[0xFF00 + TABLET_OFF_CONTACTS + 1] |
           (xram[0xFF00 + TABLET_OFF_CONTACTS + 3] >> 4) << 8;
}

static int tablet_contact_x(void)
{
    return xram[0xFF00 + TABLET_OFF_CONTACTS + 2] |
           (xram[0xFF00 + TABLET_OFF_CONTACTS + 3] & 0x0F) << 8;
}

static uint8_t tablet_contact_flags(void)
{
    return xram[0xFF00 + TABLET_OFF_CONTACTS];
}

/* A relative mouse pushed past the bottom of a widescreen canvas stops there,
 * so the first move back up moves the pointer up. */
UTEST(tablet, a_mouse_leaves_the_bottom_of_a_wide_canvas_at_once)
{
    static const tablet_connection_t mouse = {
        .valid = true,
        .button_offsets = {HID_ABSENT, HID_ABSENT, HID_ABSENT, HID_ABSENT, HID_ABSENT},
        .x_relative = true,
        .x_offset = 0, .x_size = 8,
        .y_offset = 8, .y_size = 8,
        .tip_offset = HID_ABSENT,
        .inrange_offset = HID_ABSENT,
    };
    /* On an SXGA display, 512 lines tall, whose bottom rows a reference
     * space of 480 lines once left out of reach. Its 32-row console is the
     * Pico's alone; this machine's terminal stops at 30 rows. The right edge
     * is checked on every canvas too. */
    static const uint16_t canvases[] = {vga_canvas_640_360, vga_canvas_320_180,
                                        vga_canvas_640_400, vga_canvas_320_200};
    sys_init();
    vga_set_console_size(640, 512);
    ASSERT_TRUE(tablet_mount(7, &mouse));
    ASSERT_TRUE(xreg0(0, 3, 0xFF00)); /* xreg_ria_tablet(0xFF00) */
    for (size_t i = 0; i < sizeof canvases / sizeof canvases[0]; i++)
    {
        ASSERT_TRUE(xreg1(0, 0, canvases[i]));
        int w, h;
        vga_canvas_size(&w, &h);
        /* 126 counts are 63 reference pixels with nothing left over. */
        const uint8_t down[] = {0, 126};
        for (int n = 0; n < 20; n++)
            tablet_report(7, down, sizeof down);
        ASSERT_EQ(tablet_contact_y(), h - 1);
        /* 4 counts are 2 reference pixels, at least a pixel of any of
         * these canvases. */
        const uint8_t up[] = {0, (uint8_t)-4};
        tablet_report(7, up, sizeof up);
        ASSERT_LT(tablet_contact_y(), h - 1);
        const uint8_t right[] = {126, 0};
        for (int n = 0; n < 20; n++)
            tablet_report(7, right, sizeof right);
        ASSERT_EQ(tablet_contact_x(), w - 1);
        const uint8_t left[] = {(uint8_t)-4, 0};
        tablet_report(7, left, sizeof left);
        ASSERT_LT(tablet_contact_x(), w - 1);
    }
    ASSERT_TRUE(tablet_umount(7));
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    vga_set_console_size(640, 480);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* A mouse moved before a program maps the tablet is found hovering where it
 * is, and a new canvas puts it on that canvas, in the same place. */
UTEST(tablet, a_program_finds_the_mouse_where_it_is)
{
    static const tablet_connection_t mouse = {
        .valid = true,
        .button_offsets = {HID_ABSENT, HID_ABSENT, HID_ABSENT, HID_ABSENT, HID_ABSENT},
        .x_relative = true,
        .x_offset = 0, .x_size = 8,
        .y_offset = 8, .y_size = 8,
        .tip_offset = HID_ABSENT,
        .inrange_offset = HID_ABSENT,
    };
    sys_init();
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_640_480));
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    ASSERT_TRUE(tablet_mount(7, &mouse));
    ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_HOVER); /* before it moves */
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    /* Home in the corner, then 200 counts right and 100 down: 100 and 50
     * reference pixels. */
    const uint8_t home[] = {(uint8_t)-128, (uint8_t)-128};
    for (int n = 0; n < 20; n++)
        tablet_report(7, home, sizeof home);
    const uint8_t move[] = {100, 50};
    tablet_report(7, move, sizeof move);
    tablet_report(7, move, sizeof move);
    tablet_task();

    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    ASSERT_EQ(xram[0xFF00 + TABLET_OFF_CONTROL], TABLET_CURSOR_ARROW);
    ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_HOVER);
    ASSERT_EQ(tablet_contact_x(), 100);
    ASSERT_EQ(tablet_contact_y(), 50);

    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    tablet_task();
    ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_HOVER);
    ASSERT_EQ(tablet_contact_x(), 50);
    ASSERT_EQ(tablet_contact_y(), 25);

    /* Low on a 512-line console, held at the bottom of a 480-line canvas,
     * and back where it was on the console. */
    vga_set_console_size(640, 512);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    const uint8_t down[] = {0, 126}; /* 63 reference lines */
    for (int n = 0; n < 7; n++)
        tablet_report(7, down, sizeof down);
    ASSERT_EQ(tablet_contact_y(), 50 + 7 * 63);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_640_480));
    tablet_task();
    ASSERT_EQ(tablet_contact_x(), 100);
    ASSERT_EQ(tablet_contact_y(), 479);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    tablet_task();
    ASSERT_EQ(tablet_contact_y(), 50 + 7 * 63);
    vga_set_console_size(640, 480);

    ASSERT_TRUE(tablet_umount(7));
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* An absolute device: the tip in bit 0, then 16-bit X and Y. */
static void tablet_absolute(int slot, int x, int y, bool tip)
{
    const uint8_t r[] = {tip ? 1 : 0, (uint8_t)x, (uint8_t)(x >> 8),
                         (uint8_t)y, (uint8_t)(y >> 8)};
    tablet_report(slot, r, sizeof r);
}

static tablet_connection_t tablet_absolute_desc(int32_t x_max, int32_t y_max)
{
    tablet_connection_t d = {
        .valid = true,
        .button_offsets = {HID_ABSENT, HID_ABSENT, HID_ABSENT, HID_ABSENT, HID_ABSENT},
        .x_offset = 8, .x_size = 16, .x_min = 0, .x_max = x_max,
        .y_offset = 24, .y_size = 16, .y_min = 0, .y_max = y_max,
        .tip_offset = 0,
        .inrange_offset = HID_ABSENT,
    };
    return d;
}

/* A square traced on a 16:10 pen tablet is a square on a 4:3 canvas. The
 * tablet's height spans the canvas's, and its sides are a spare strip. */
UTEST(tablet, a_pen_tablet_keeps_its_shape)
{
    sys_init();
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    /* By its logical extent, then by a physical extent that says the same
     * shape while the logical extent is square. */
    tablet_connection_t pens[2] = {tablet_absolute_desc(1599, 999),
                                   tablet_absolute_desc(1599, 1599)};
    pens[1].x_phys = 160;
    pens[1].y_phys = 100;
    for (int i = 0; i < 2; i++)
    {
        const int xs = pens[i].x_max, ys = pens[i].y_max;
        const int pw = pens[i].x_phys ? pens[i].x_phys : xs;
        const int ph = pens[i].y_phys ? pens[i].y_phys : ys;
        ASSERT_TRUE(tablet_mount(7, &pens[i]));
        const int mx = (xs + 1) / 2, my = (ys + 1) / 2;
        tablet_absolute(7, mx, my, true);
        ASSERT_EQ(tablet_contact_x(), 160);
        ASSERT_EQ(tablet_contact_y(), 120);
        /* One length across and down, in the tablet's own units. */
        const int d = pw * 3 / 16;
        tablet_absolute(7, mx + d * xs / pw, my + d * ys / ph, true);
        const int dx = tablet_contact_x() - 160, dy = tablet_contact_y() - 120;
        ASSERT_GT(dx, 60);
        ASSERT_LE(dx - dy, 1);
        ASSERT_GE(dx - dy, -1);
        tablet_absolute(7, 0, ys, true);
        ASSERT_EQ(tablet_contact_x(), 0);
        ASSERT_EQ(tablet_contact_y(), 239);
        tablet_absolute(7, xs, 0, true);
        ASSERT_EQ(tablet_contact_x(), 319);
        ASSERT_EQ(tablet_contact_y(), 0);
        ASSERT_TRUE(tablet_umount(7));
    }
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* A touchscreen covers the display, and a 16:9 canvas on a 4:3 display fills
 * its middle 360 of 480 lines. A touch on the bands above and below is no
 * touch, and one on the canvas lands where the canvas is drawn. */
UTEST(tablet, a_touchscreen_maps_through_the_display)
{
    sys_init();
    vga_set_console_size(640, 480);
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_640_360));
    tablet_connection_t touch = tablet_absolute_desc(4799, 4799);
    touch.touchscreen = true;
    ASSERT_TRUE(tablet_mount(7, &touch));
    /* Line 240 of 480 is canvas row 180, and line 70 is row 10. */
    tablet_absolute(7, 2400, 2400, true);
    ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_LEFT);
    ASSERT_EQ(tablet_contact_x(), 320);
    ASSERT_EQ(tablet_contact_y(), 180);
    tablet_absolute(7, 0, 700, true);
    ASSERT_EQ(tablet_contact_x(), 0);
    ASSERT_EQ(tablet_contact_y(), 10);
    /* Line 30 is above the canvas, and line 450 below it. */
    tablet_absolute(7, 2400, 300, true);
    ASSERT_EQ(tablet_contact_flags(), 0);
    ASSERT_EQ(tablet_contact_y(), 10);
    tablet_absolute(7, 2400, 4500, true);
    ASSERT_EQ(tablet_contact_flags(), 0);
    /* On a 4:3 canvas the whole screen is the canvas. */
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    tablet_absolute(7, 4799, 4799, true);
    ASSERT_EQ(tablet_contact_x(), 319);
    ASSERT_EQ(tablet_contact_y(), 239);
    ASSERT_TRUE(tablet_umount(7));
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* A KVM's mouse reports where the pointer is rather than how far it moved.
 * It covers the display as a touchscreen does, and with no tip it always
 * hovers. The same report twice is the same spot. */
UTEST(tablet, an_absolute_mouse_points_and_hovers)
{
    sys_init();
    vga_set_console_size(640, 480);
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    tablet_connection_t kvm = tablet_absolute_desc(32767, 32767);
    kvm.button_offsets[0] = 0;
    kvm.tip_offset = HID_ABSENT;
    kvm.touchscreen = true;
    ASSERT_TRUE(tablet_mount(7, &kvm));
    tablet_absolute(7, 0, 0, false);
    ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_HOVER);
    ASSERT_EQ(tablet_contact_x(), 0);
    ASSERT_EQ(tablet_contact_y(), 0);
    tablet_absolute(7, 32767, 32767, false);
    ASSERT_EQ(tablet_contact_x(), 319);
    ASSERT_EQ(tablet_contact_y(), 239);
    for (int i = 0; i < 2; i++)
    {
        tablet_absolute(7, 16384, 16384, true);
        ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_LEFT | TABLET_FLAG_HOVER);
        ASSERT_EQ(tablet_contact_x(), 160);
        ASSERT_EQ(tablet_contact_y(), 120);
    }
    ASSERT_TRUE(tablet_umount(7));
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* Contact 0 is one pointer that every device moves, so a button held on one
 * mouse stays down while another moves, and is let go when its mouse is
 * unplugged. */
UTEST(tablet, buttons_of_two_mice_merge)
{
    static const tablet_connection_t mouse = {
        .valid = true,
        .button_offsets = {0, 1, 2, HID_ABSENT, HID_ABSENT},
        .x_relative = true,
        .x_offset = 8, .x_size = 8,
        .y_offset = 16, .y_size = 8,
        .tip_offset = HID_ABSENT,
        .inrange_offset = HID_ABSENT,
    };
    sys_init();
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    ASSERT_TRUE(tablet_mount(5, &mouse));
    ASSERT_TRUE(tablet_mount(6, &mouse));
    const uint8_t left[] = {TABLET_FLAG_LEFT, 0, 0};
    const uint8_t move[] = {0, 4, 4};
    tablet_report(5, left, sizeof left);
    ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_LEFT | TABLET_FLAG_HOVER);
    tablet_report(6, move, sizeof move);
    ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_LEFT | TABLET_FLAG_HOVER);
    ASSERT_TRUE(tablet_umount(5));
    ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_HOVER);
    ASSERT_TRUE(tablet_umount(6));
    ASSERT_EQ(tablet_contact_flags(), 0);
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
}

/* Bit 0 of status is the program's: mapping sets it, a report never
 * writes it, and the program's 0 holds until the tablet is mapped again. */
UTEST(tablet, the_share_bit_is_the_programs)
{
    static const tablet_connection_t mouse = {
        .valid = true,
        .button_offsets = {0, HID_ABSENT, HID_ABSENT, HID_ABSENT, HID_ABSENT},
        .x_relative = true,
        .x_offset = 8, .x_size = 8,
        .y_offset = 16, .y_size = 8,
        .tip_offset = HID_ABSENT,
        .inrange_offset = HID_ABSENT,
    };
    sys_init();
    ASSERT_TRUE(tablet_shared());
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    ASSERT_EQ(xram[0xFF00 + TABLET_OFF_STATUS], TABLET_STATUS_SHARED);
    ASSERT_TRUE(tablet_shared());

    xram[0xFF00 + TABLET_OFF_STATUS] = 0;
    ASSERT_FALSE(tablet_shared());
    ASSERT_TRUE(tablet_mount(7, &mouse));
    const uint8_t press[] = {1, 4, 4};
    tablet_report(7, press, sizeof press);
    tablet_host_pointer(100, 50, TABLET_FLAG_RIGHT, false);
    tablet_host_wheel(1, 0);
    const tablet_point_t pt = {10, 10};
    tablet_host_touch(&pt, 1);
    tablet_host_clear();
    ASSERT_EQ(xram[0xFF00 + TABLET_OFF_STATUS], 0);
    ASSERT_FALSE(tablet_shared());
    ASSERT_TRUE(tablet_umount(7));

    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    ASSERT_EQ(xram[0xFF00 + TABLET_OFF_STATUS], TABLET_STATUS_SHARED);
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    ASSERT_TRUE(tablet_shared());
}

/* A host that stops giving the tablet its pointer lets it go once, at the
 * edge: a contact put there while it does not give it is kept. The edge is
 * saved, so a load from before it lets go again. */
UTEST(tablet, a_host_lets_go_of_its_pointer_at_the_edge)
{
    sys_init();
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    tablet_host_give(true);
    tablet_host_pointer(10, 10, TABLET_FLAG_LEFT, true);
    tablet_host_give(true);
    ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_HOVER | TABLET_FLAG_LEFT);

    uint8_t blob[256];
    ASSERT_LE((size_t)tablet_sst.size, sizeof blob);
    sst_cursor_t c = {blob, blob + tablet_sst.size, false, NULL};
    tablet_sst.save(&c, 0);

    tablet_host_give(false);
    ASSERT_EQ(tablet_contact_flags(), 0);
    tablet_host_pointer(20, 10, TABLET_FLAG_LEFT, true);
    tablet_host_give(false);
    ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_HOVER | TABLET_FLAG_LEFT);

    c = (sst_cursor_t){blob, blob + tablet_sst.size, false, NULL};
    ASSERT_TRUE(tablet_sst.load(&c, 0));
    ASSERT_EQ(tablet_contact_flags(), TABLET_FLAG_HOVER | TABLET_FLAG_LEFT);
    tablet_host_give(false);
    ASSERT_EQ(tablet_contact_flags(), 0);
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
}

/* ---- the console's mouse (core/term/vterm_mouse.c) ---------------------- */

static uint32_t mouse_queued(void)
{
    return RING_LEN(&vterm_mouse_queue);
}

static const vterm_mouse_event_t *mouse_change(uint32_t i)
{
    return RING_AT(&vterm_mouse_queue, i);
}

/* A machine on the console with the pointer gone from it and nothing
 * queued. */
static void mouse_start(void)
{
    sys_init();
    tablet_host_clear();
    vterm_mouse_task();
}

/* A press and a release that come before the task runs are both kept, and
 * motion between them is merged. */
UTEST(vterm_mouse, a_press_and_release_in_one_burst_give_both_edges)
{
    mouse_start();
    tablet_host_pointer(10, 10, 0, true);
    tablet_host_pointer(11, 10, 0, true);
    tablet_host_pointer(12, 10, 0, true);
    tablet_host_pointer(12, 10, TABLET_FLAG_LEFT, true);
    tablet_host_pointer(14, 10, TABLET_FLAG_LEFT, true);
    tablet_host_pointer(16, 10, 0, true);
    ASSERT_EQ(mouse_queued(), 5u);
    const vterm_mouse_event_t *e = mouse_change(0);
    ASSERT_TRUE(e->edge);
    ASSERT_EQ(e->flags, TABLET_FLAG_HOVER);
    ASSERT_EQ(e->x, 10);
    e = mouse_change(1);
    ASSERT_FALSE(e->edge);
    ASSERT_TRUE(e->moved);
    ASSERT_EQ(e->x, 12);
    e = mouse_change(2);
    ASSERT_TRUE(e->edge);
    ASSERT_FALSE(e->moved);
    ASSERT_EQ(e->flags, TABLET_FLAG_HOVER | TABLET_FLAG_LEFT);
    ASSERT_EQ(e->x, 12);
    e = mouse_change(3);
    ASSERT_FALSE(e->edge);
    ASSERT_EQ(e->x, 14);
    e = mouse_change(4);
    ASSERT_TRUE(e->edge);
    ASSERT_EQ(e->flags, TABLET_FLAG_HOVER);
    ASSERT_EQ(e->x, 16);
    vterm_mouse_task();
    ASSERT_EQ(mouse_queued(), 0u);
}

/* The same from a HID mouse's reports. */
UTEST(vterm_mouse, a_press_and_release_in_hid_reports_give_both_edges)
{
    static const tablet_connection_t mouse = {
        .valid = true,
        .button_offsets = {0, HID_ABSENT, HID_ABSENT, HID_ABSENT, HID_ABSENT},
        .x_relative = true,
        .x_offset = 8, .x_size = 8,
        .y_offset = 16, .y_size = 8,
        .tip_offset = HID_ABSENT,
        .inrange_offset = HID_ABSENT,
    };
    mouse_start();
    ASSERT_TRUE(tablet_mount(7, &mouse));
    vterm_mouse_task();
    const uint8_t press[] = {1, 0, 0}, release[] = {0, 0, 0};
    tablet_report(7, press, sizeof press);
    tablet_report(7, release, sizeof release);
    ASSERT_EQ(mouse_queued(), 2u);
    ASSERT_EQ(mouse_change(0)->flags, TABLET_FLAG_HOVER | TABLET_FLAG_LEFT);
    ASSERT_EQ(mouse_change(1)->flags, TABLET_FLAG_HOVER);
    ASSERT_TRUE(mouse_change(1)->edge);
    ASSERT_TRUE(tablet_umount(7));
    vterm_mouse_task();
}

/* A frontend that polls once a frame gives a press in one frame and the
 * release in the next, at the same spot; each is a change. */
UTEST(vterm_mouse, edges_in_consecutive_frames_are_each_delivered)
{
    mouse_start();
    tablet_host_pointer(40, 40, 0, false);
    vterm_mouse_task();
    tablet_host_pointer(40, 40, TABLET_FLAG_LEFT, false);
    tablet_host_pointer(40, 40, TABLET_FLAG_LEFT, false); /* no change */
    ASSERT_EQ(mouse_queued(), 1u);
    ASSERT_TRUE(mouse_change(0)->edge);
    vterm_mouse_task();
    tablet_host_pointer(40, 40, 0, false);
    ASSERT_EQ(mouse_queued(), 1u);
    ASSERT_TRUE(mouse_change(0)->edge);
    ASSERT_EQ(mouse_change(0)->flags, TABLET_FLAG_HOVER);
    vterm_mouse_task();
}

/* The wheel and pan come as how far they moved, not as the counters. */
UTEST(vterm_mouse, the_wheel_is_deltas_not_counters)
{
    mouse_start();
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    tablet_host_wheel(5, 0); /* the counters away from 0 */
    vterm_mouse_task();
    tablet_host_wheel(3, -2);
    tablet_host_wheel(-4, 5);
    ASSERT_EQ(mouse_queued(), 1u);
    ASSERT_EQ(mouse_change(0)->wheel, -1);
    ASSERT_EQ(mouse_change(0)->pan, 3);
    ASSERT_FALSE(mouse_change(0)->moved);
    /* A sum too large for one change starts the next. */
    tablet_host_wheel(100, 0);
    tablet_host_wheel(100, 0);
    ASSERT_EQ(mouse_queued(), 2u);
    ASSERT_EQ(mouse_change(0)->wheel, 99);
    ASSERT_EQ(mouse_change(1)->wheel, 100);
    vterm_mouse_task();
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
}

/* A finger is the left button, with no hover. */
UTEST(vterm_mouse, a_touch_is_the_left_button)
{
    mouse_start();
    const tablet_point_t pt = {30, 20};
    tablet_host_touch(&pt, 1);
    ASSERT_EQ(mouse_queued(), 1u);
    ASSERT_EQ(mouse_change(0)->flags, TABLET_FLAG_LEFT);
    ASSERT_EQ(mouse_change(0)->x, 30);
    ASSERT_EQ(mouse_change(0)->y, 20);
    ASSERT_TRUE(mouse_change(0)->edge);
    tablet_host_clear();
    ASSERT_EQ(mouse_change(1)->flags, 0);
    vterm_mouse_task();
}

/* A HID mouse and a host's pointer at the same place with the same button
 * are the same change. */
UTEST(vterm_mouse, the_hid_and_host_paths_agree)
{
    static const tablet_connection_t mouse = {
        .valid = true,
        .button_offsets = {0, HID_ABSENT, HID_ABSENT, HID_ABSENT, HID_ABSENT},
        .x_relative = true,
        .x_offset = 8, .x_size = 8,
        .y_offset = 16, .y_size = 8,
        .tip_offset = HID_ABSENT,
        .inrange_offset = HID_ABSENT,
    };
    mouse_start();
    ASSERT_TRUE(tablet_mount(7, &mouse));
    /* Home in the corner, then 200 counts right and 100 down: 100 and 50
     * pixels of the 640-wide console. */
    const uint8_t home[] = {0, (uint8_t)-128, (uint8_t)-128};
    for (int n = 0; n < 20; n++)
        tablet_report(7, home, sizeof home);
    const uint8_t move[] = {0, 100, 50};
    tablet_report(7, move, sizeof move);
    tablet_report(7, move, sizeof move);
    const uint8_t press[] = {1, 0, 0};
    tablet_report(7, press, sizeof press);
    const vterm_mouse_event_t hid = *mouse_change(mouse_queued() - 1);
    ASSERT_TRUE(tablet_umount(7));
    tablet_host_clear();
    vterm_mouse_task();

    tablet_host_pointer(100, 50, TABLET_FLAG_LEFT, false);
    const vterm_mouse_event_t host = *mouse_change(mouse_queued() - 1);
    ASSERT_EQ(hid.x, 100);
    ASSERT_EQ(hid.y, 50);
    ASSERT_EQ(host.x, hid.x);
    ASSERT_EQ(host.y, hid.y);
    ASSERT_EQ(host.flags, hid.flags);
    ASSERT_TRUE(host.edge && hid.edge);
    vterm_mouse_task();
}

/* A new canvas puts the pointer on it, which is no motion of the user's. */
UTEST(vterm_mouse, a_canvas_re_place_is_not_user_motion)
{
    mouse_start();
    tablet_host_pointer(300, 200, 0, false);
    vterm_mouse_task();
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    tablet_task();
    ASSERT_EQ(mouse_queued(), 1u);
    ASSERT_EQ(mouse_change(0)->x, 150);
    ASSERT_EQ(mouse_change(0)->y, 100);
    ASSERT_FALSE(mouse_change(0)->moved);
    ASSERT_FALSE(mouse_change(0)->edge);
    vterm_mouse_task();
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    tablet_task();
    vterm_mouse_task();
}

/* A full queue keeps the flags contact 0 ends on. */
UTEST(vterm_mouse, a_full_queue_ends_on_the_last_flags)
{
    mouse_start();
    for (int i = 0; i < VTERM_MOUSE_QUEUE + 3; i++)
        tablet_host_pointer(10, 10, (uint8_t)(i & 1 ? TABLET_FLAG_LEFT : 0), false);
    ASSERT_EQ(mouse_queued(), (uint32_t)VTERM_MOUSE_QUEUE);
    ASSERT_EQ(mouse_change(VTERM_MOUSE_QUEUE - 1)->flags, TABLET_FLAG_HOVER);
    vterm_mouse_task();
}

/* A full queue holds a wheel too large to merge at its end, keeping its
 * direction, and still merges the pan beside it. */
UTEST(vterm_mouse, a_full_queue_holds_the_wheel_at_its_end)
{
    mouse_start();
    for (int i = 0; i < VTERM_MOUSE_QUEUE; i++)
        tablet_host_pointer(10, 10, (uint8_t)(i & 1 ? TABLET_FLAG_LEFT : 0), false);
    ASSERT_EQ(mouse_queued(), (uint32_t)VTERM_MOUSE_QUEUE);
    tablet_host_wheel(-100, 2);
    tablet_host_wheel(-100, 3);
    ASSERT_EQ(mouse_queued(), (uint32_t)VTERM_MOUSE_QUEUE);
    ASSERT_EQ(mouse_change(VTERM_MOUSE_QUEUE - 1)->wheel, INT8_MIN);
    ASSERT_EQ(mouse_change(VTERM_MOUSE_QUEUE - 1)->pan, 5);
    vterm_mouse_task();
}

/* The pointer at x, y, buttons held, taken by the terminal at once. */
static bool mouse_acts_at(int x, int y, uint8_t buttons)
{
    tablet_host_pointer(x, y, buttons, false);
    vterm_mouse_task();
    return vterm_mouse_acts();
}

/* With no program on the tablet, the terminal acts over mode 0's cells,
 * wherever mode 0 sits on its canvas, and nowhere on a canvas without it. */
UTEST(vterm_mouse, the_terminal_acts_over_mode_0s_cells)
{
    mouse_start();
    ASSERT_TRUE(mouse_acts_at(100, 100, 0));
    ASSERT_TRUE(mouse_acts_at(639, 479, 0));

    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    ASSERT_FALSE(tablet_wanted());
    ASSERT_FALSE(mouse_acts_at(100, 100, 0));
    ASSERT_FALSE(mouse_acts_at(100, 101, TABLET_FLAG_LEFT));
    ASSERT_FALSE(mouse_acts_at(100, 101, 0));

    /* 26 rows of 8 lines from line 16. */
    ASSERT_TRUE(program_mode0(16, 224));
    ASSERT_FALSE(mouse_acts_at(100, 15, 0));
    ASSERT_TRUE(mouse_acts_at(100, 16, 0));
    ASSERT_TRUE(mouse_acts_at(319, 223, 0));
    ASSERT_FALSE(mouse_acts_at(100, 224, 0));
    ASSERT_TRUE(mouse_acts_at(0, 100, 0));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* A press decides, and the decision holds to the release, wherever a drag
 * goes. */
UTEST(vterm_mouse, a_press_decides_to_the_release)
{
    mouse_start();
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    ASSERT_TRUE(program_mode0(16, 224));
    ASSERT_TRUE(mouse_acts_at(100, 100, 0));
    ASSERT_TRUE(mouse_acts_at(100, 100, TABLET_FLAG_LEFT));
    ASSERT_TRUE(mouse_acts_at(100, 230, TABLET_FLAG_LEFT));
    ASSERT_TRUE(mouse_acts_at(100, 230, TABLET_FLAG_LEFT | TABLET_FLAG_RIGHT));
    ASSERT_TRUE(mouse_acts_at(100, 230, 0)); /* the release is the press's */
    ASSERT_FALSE(mouse_acts_at(100, 231, 0));

    ASSERT_FALSE(mouse_acts_at(100, 231, TABLET_FLAG_LEFT));
    ASSERT_FALSE(mouse_acts_at(100, 100, TABLET_FLAG_LEFT));
    ASSERT_FALSE(mouse_acts_at(100, 100, 0));
    ASSERT_TRUE(mouse_acts_at(101, 100, 0));

    /* A press and its release taken in one run of the task. */
    tablet_host_pointer(100, 100, TABLET_FLAG_LEFT, false);
    tablet_host_pointer(100, 230, TABLET_FLAG_LEFT, false);
    tablet_host_pointer(100, 230, 0, false);
    vterm_mouse_task();
    ASSERT_TRUE(vterm_mouse_acts());
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* A program that maps the tablet shares it, and the terminal acts over mode
 * 0 as before. A program that clears the share bit has it to itself; the bit
 * is read at the press and held to the release. */
UTEST(vterm_mouse, the_share_bit_decides_while_mapped)
{
    mouse_start();
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    ASSERT_TRUE(mouse_acts_at(100, 100, 0));
    xram[0xFF00 + TABLET_OFF_STATUS] = 0;
    ASSERT_FALSE(mouse_acts_at(101, 100, 0));
    ASSERT_FALSE(mouse_acts_at(101, 100, TABLET_FLAG_LEFT));
    xram[0xFF00 + TABLET_OFF_STATUS] = TABLET_STATUS_SHARED;
    ASSERT_FALSE(mouse_acts_at(102, 100, TABLET_FLAG_LEFT));
    ASSERT_FALSE(mouse_acts_at(102, 100, 0));
    ASSERT_TRUE(mouse_acts_at(103, 100, 0));
    ASSERT_TRUE(mouse_acts_at(103, 100, TABLET_FLAG_LEFT));
    xram[0xFF00 + TABLET_OFF_STATUS] = 0;
    ASSERT_TRUE(mouse_acts_at(104, 100, TABLET_FLAG_LEFT));
    ASSERT_TRUE(mouse_acts_at(104, 100, 0));
    ASSERT_FALSE(mouse_acts_at(105, 100, 0));
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    ASSERT_TRUE(mouse_acts_at(106, 100, 0));
}

/* The share bit is the one the program had as the press came, not the one it
 * has when the task takes the press. */
UTEST(vterm_mouse, the_share_bit_is_read_as_the_press_comes)
{
    mouse_start();
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    tablet_host_pointer(100, 100, TABLET_FLAG_LEFT, false);
    xram[0xFF00 + TABLET_OFF_STATUS] = 0;
    vterm_mouse_task();
    ASSERT_TRUE(vterm_mouse_acts());
    tablet_host_pointer(100, 100, 0, false);
    tablet_host_pointer(100, 100, TABLET_FLAG_LEFT, false);
    xram[0xFF00 + TABLET_OFF_STATUS] = TABLET_STATUS_SHARED;
    vterm_mouse_task();
    ASSERT_FALSE(vterm_mouse_acts());
    tablet_host_pointer(100, 100, 0, false);
    vterm_mouse_task();
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
}

/* A contact that has gone is no one's: a pointer that left, and the wheel
 * turned with none there. A finger lifted is its press's release. */
UTEST(vterm_mouse, a_contact_that_has_gone_goes_nowhere)
{
    mouse_start();
    ASSERT_TRUE(mouse_acts_at(100, 100, 0));
    tablet_host_clear();
    vterm_mouse_task();
    ASSERT_FALSE(vterm_mouse_acts());
    tablet_host_wheel(1, 0);
    vterm_mouse_task();
    ASSERT_FALSE(vterm_mouse_acts());

    const tablet_point_t pt = {100, 100};
    tablet_host_touch(&pt, 1);
    vterm_mouse_task();
    ASSERT_TRUE(vterm_mouse_acts());
    tablet_host_clear();
    vterm_mouse_task();
    ASSERT_TRUE(vterm_mouse_acts()); /* the release is the press's */
    tablet_host_wheel(1, 0);
    vterm_mouse_task();
    ASSERT_FALSE(vterm_mouse_acts());
}

static int keyboard_drain(char *buf, int max)
{
    int n = 0, c;
    com_source_t src = COM_SOURCE_KEYBOARD;
    while (n < max && (c = com_getchar(&src)) >= 0)
    {
        buf[n++] = (char)c;
        src = COM_SOURCE_KEYBOARD;
    }
    return n;
}

UTEST(keyboard, ansi_sequences)
{
    char b[32];

    com_rx_clear();
    t_press(HID_KEY_ARROW_UP, 0, 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33[A", 3));

    com_rx_clear();
    t_press(HID_KEY_F1, 0, 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33OP", 3));

    com_rx_clear();
    t_press(HID_KEY_F5, 0, 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 5);
    ASSERT_EQ(0, memcmp(b, "\33[15~", 5));

    com_rx_clear();
    t_press(HID_KEY_F12, 0, 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 5);
    ASSERT_EQ(0, memcmp(b, "\33[24~", 5));

    com_rx_clear();
    t_press(HID_KEY_INSERT, 0, 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 4);
    ASSERT_EQ(0, memcmp(b, "\33[2~", 4));

    com_rx_clear();
    t_press(HID_KEY_HOME, 0, 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33[H", 3));

    /* The xterm modifier parameter is 1 + shift + alt*2 + ctrl*4. */
    com_rx_clear();
    t_press(HID_KEY_ARROW_UP, VTKEY_CTRL, 0); /* ctrl -> 5 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;5A", 6));

    com_rx_clear();
    t_press(HID_KEY_F1, VTKEY_SHIFT, 0); /* shift -> 2 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;2P", 6));

    com_rx_clear();
    t_press(HID_KEY_END, VTKEY_SHIFT | VTKEY_ALT, 0); /* shift+alt -> 4 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;4F", 6));

    com_rx_clear();
    t_press(HID_KEY_PAGE_UP, VTKEY_CTRL, 0); /* ctrl -> 5 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[5;5~", 6));

    com_rx_clear();
    t_press(HID_KEY_ENTER, 0, 0);
    t_press(HID_KEY_BACKSPACE, 0, 0);
    t_press(HID_KEY_BACKSPACE, VTKEY_CTRL, 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\r\x7f\x08", 3));
}

UTEST(keyboard, ctrl_and_alt_on_control_keys)
{
    char b[16];

    com_rx_clear();
    t_press(HID_KEY_ENTER, VTKEY_CTRL, 0);
    t_press(HID_KEY_TAB, VTKEY_CTRL, 0);
    t_press(HID_KEY_ESCAPE, VTKEY_CTRL, 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\r\t\x1b", 3));

    com_rx_clear();
    t_press(HID_KEY_ENTER, VTKEY_ALT, 0);
    t_press(HID_KEY_TAB, VTKEY_ALT, 0);
    t_press(HID_KEY_ESCAPE, VTKEY_ALT, 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\x1b\r\x1b\t\x1b\x1b", 6));

    com_rx_clear();
    t_press(HID_KEY_BACKSPACE, VTKEY_ALT, 0);
    t_press(HID_KEY_BACKSPACE, VTKEY_CTRL | VTKEY_ALT, 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 4);
    ASSERT_EQ(0, memcmp(b, "\x1b\x7f\x1b\x08", 4));
}

/* DECBKM: Backspace sends BS, and Ctrl swaps it back to DEL. */
UTEST(keyboard, backarrow_key_mode)
{
    char b[16];

    com_rx_clear();
    vterm.backarrow_bs = true;
    t_press(HID_KEY_BACKSPACE, 0, 0);
    t_press(HID_KEY_BACKSPACE, VTKEY_CTRL, 0);
    t_press(HID_KEY_BACKSPACE, VTKEY_ALT, 0);
    vterm.backarrow_bs = false;
    ASSERT_EQ(keyboard_drain(b, sizeof b), 4);
    ASSERT_EQ(0, memcmp(b, "\x08\x7f\x1b\x08", 4));
}

UTEST(keyboard, ctrl_and_alt_on_characters)
{
    char b[16];

    com_rx_clear();
    ASSERT_EQ(t_press(0x06, 0, 'c'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(0x04, VTKEY_CTRL, 'a'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(0x04, VTKEY_ALT | VTKEY_SHIFT, 'A'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(0x04, VTKEY_ALT | VTKEY_CTRL, 'a'), HOSTKEY_TYPED);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "c\x01\x1b" "A\x1b\x01", 6));

    /* Ctrl has no byte for '1', so it is typed as it is. Ctrl-? is DEL. No
     * character sends nothing. */
    ASSERT_EQ(t_press(0x1E, VTKEY_CTRL, '1'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(0x04, 0, 0), HOSTKEY_NONE);
    ASSERT_EQ(t_press(0x1E, VTKEY_ALT | VTKEY_CTRL, '1'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(0x38, VTKEY_CTRL | VTKEY_SHIFT, '?'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(0x25, VTKEY_CTRL, '8'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(0x1F, VTKEY_CTRL, '2'), HOSTKEY_TYPED);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "1\x1b" "1\x7f\x7f\0", 6));

    /* A keypad key's character is typed whatever Ctrl and Alt say. */
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_DIVIDE, VTKEY_CTRL, '/'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_8, NUM | VTKEY_ALT, '8'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_ENTER, VTKEY_ALT, 0), HOSTKEY_TYPED);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "/8\r", 3));
}

/* The terminal modes a key's bytes depend on. */
#define M_CKM 1  /* DECCKM, application cursor keys */
#define M_KPAM 2 /* DECKPAM, application keypad */
#define M_LNM 4  /* new line mode */
#define M_VT52 8

static void key_modes(unsigned modes)
{
    vterm.app_cursor = modes & M_CKM;
    vterm.app_keypad = modes & M_KPAM;
    vterm.newline_mode = modes & M_LNM;
    vterm.vt52 = modes & M_VT52;
}

#define SH VTKEY_SHIFT
#define CHR NULL /* VTKEY_CHAR */

/* What vtkey_encode answers for a key class in the modes that matter to it:
 * none set, then DECCKM, DECKPAM, LNM and VT52 alone, then VT52 with all of
 * the other three. */
static const struct
{
    uint8_t usage;
    uint8_t flags;
    const char *want[6];
} key_table[] = {
    /* Cursor keys, Home and End */
    {HID_KEY_ARROW_UP, 0, {"\33[A", "\33OA", "\33[A", "\33[A", "\33A", "\33A"}},
    {HID_KEY_ARROW_DOWN, 0, {"\33[B", "\33OB", "\33[B", "\33[B", "\33B", "\33B"}},
    {HID_KEY_ARROW_RIGHT, 0, {"\33[C", "\33OC", "\33[C", "\33[C", "\33C", "\33C"}},
    {HID_KEY_ARROW_LEFT, 0, {"\33[D", "\33OD", "\33[D", "\33[D", "\33D", "\33D"}},
    {HID_KEY_HOME, 0, {"\33[H", "\33OH", "\33[H", "\33[H", "\33H", "\33H"}},
    {HID_KEY_END, 0, {"\33[F", "\33OF", "\33[F", "\33[F", "\33F", "\33F"}},
    {HID_KEY_ARROW_UP, VTKEY_CTRL, {"\33[1;5A", "\33[1;5A", "\33[1;5A", "\33[1;5A", "\33A", "\33A"}},
    {HID_KEY_END, SH | VTKEY_ALT, {"\33[1;4F", "\33[1;4F", "\33[1;4F", "\33[1;4F", "\33F", "\33F"}},
    /* F1 to F4 */
    {HID_KEY_F1, 0, {"\33OP", "\33OP", "\33OP", "\33OP", "\33P", "\33P"}},
    {HID_KEY_F4, 0, {"\33OS", "\33OS", "\33OS", "\33OS", "\33S", "\33S"}},
    {HID_KEY_F1, SH, {"\33[1;2P", "\33[1;2P", "\33[1;2P", "\33[1;2P", "\33P", "\33P"}},
    /* The keys a VT52 lacks send what they always do, without modifiers */
    {HID_KEY_F5, 0, {"\33[15~", "\33[15~", "\33[15~", "\33[15~", "\33[15~", "\33[15~"}},
    {HID_KEY_F12, 0, {"\33[24~", "\33[24~", "\33[24~", "\33[24~", "\33[24~", "\33[24~"}},
    {HID_KEY_F12, 7, {"\33[24;8~", "\33[24;8~", "\33[24;8~", "\33[24;8~", "\33[24~", "\33[24~"}},
    {HID_KEY_INSERT, 0, {"\33[2~", "\33[2~", "\33[2~", "\33[2~", "\33[2~", "\33[2~"}},
    {HID_KEY_DELETE, 0, {"\33[3~", "\33[3~", "\33[3~", "\33[3~", "\33[3~", "\33[3~"}},
    {HID_KEY_PAGE_UP, VTKEY_CTRL, {"\33[5;5~", "\33[5;5~", "\33[5;5~", "\33[5;5~", "\33[5~", "\33[5~"}},
    {HID_KEY_PAGE_DOWN, 0, {"\33[6~", "\33[6~", "\33[6~", "\33[6~", "\33[6~", "\33[6~"}},
    /* Enter, Tab, Escape and Backspace */
    {HID_KEY_ENTER, 0, {"\r", "\r", "\r", "\r\n", "\r", "\r\n"}},
    {HID_KEY_ENTER, VTKEY_CTRL, {"\r", "\r", "\r", "\r\n", "\r", "\r\n"}},
    {HID_KEY_ENTER, VTKEY_ALT, {"\33\r", "\33\r", "\33\r", "\33\r\n", "\33\r", "\33\r\n"}},
    {HID_KEY_TAB, 0, {"\t", "\t", "\t", "\t", "\t", "\t"}},
    {HID_KEY_TAB, VTKEY_ALT, {"\33\t", "\33\t", "\33\t", "\33\t", "\33\t", "\33\t"}},
    {HID_KEY_TAB, SH, {"\33[Z", "\33[Z", "\33[Z", "\33[Z", "\33[Z", "\33[Z"}},
    {HID_KEY_TAB, SH | VTKEY_ALT, {"\33[Z", "\33[Z", "\33[Z", "\33[Z", "\33[Z", "\33[Z"}},
    {HID_KEY_ESCAPE, 0, {"\33", "\33", "\33", "\33", "\33", "\33"}},
    {HID_KEY_ESCAPE, VTKEY_ALT, {"\33\33", "\33\33", "\33\33", "\33\33", "\33\33", "\33\33"}},
    {HID_KEY_BACKSPACE, 0, {"\x7f", "\x7f", "\x7f", "\x7f", "\x7f", "\x7f"}},
    {HID_KEY_BACKSPACE, VTKEY_CTRL, {"\b", "\b", "\b", "\b", "\b", "\b"}},
    {HID_KEY_BACKSPACE, VTKEY_CTRL | VTKEY_ALT, {"\33\b", "\33\b", "\33\b", "\33\b", "\33\b", "\33\b"}},
    /* The keypad with NumLock on: characters whatever the keypad mode. */
    {HID_KEY_KEYPAD_8, NUM, {CHR, CHR, CHR, CHR, CHR, CHR}},
    {HID_KEY_KEYPAD_5, NUM, {CHR, CHR, CHR, CHR, CHR, CHR}},
    {HID_KEY_KEYPAD_DECIMAL, NUM, {CHR, CHR, CHR, CHR, CHR, CHR}},
    {HID_KEY_KEYPAD_DIVIDE, NUM, {CHR, CHR, CHR, CHR, CHR, CHR}},
    {HID_KEY_KEYPAD_ADD, NUM | VTKEY_CTRL, {CHR, CHR, CHR, CHR, CHR, CHR}},
    {HID_KEY_KEYPAD_EQUAL, NUM, {CHR, CHR, CHR, CHR, CHR, CHR}},
    {HID_KEY_KEYPAD_COMMA, NUM, {",", ",", ",", ",", ",", ","}},
    {HID_KEY_KEYPAD_ENTER, NUM, {"\r", "\r", "\r", "\r\n", "\r", "\r\n"}},
    {HID_KEY_KEYPAD_ENTER, NUM | VTKEY_ALT, {"\r", "\r", "\r", "\r\n", "\r", "\r\n"}},
    /* Shift with NumLock on is NumLock off, with the Shift used up. */
    {HID_KEY_KEYPAD_8, NUM | SH, {"\33[A", "\33OA", "\33Ox", "\33[A", "\33A", "\33?x"}},
    {HID_KEY_KEYPAD_8, NUM | SH | VTKEY_CTRL, {"\33[1;5A", "\33[1;5A", "\33O5x", "\33[1;5A", "\33A", "\33?x"}},
    {HID_KEY_KEYPAD_0, NUM | SH, {"\33[2~", "\33[2~", "\33Op", "\33[2~", "\33[2~", "\33?p"}},
    {HID_KEY_KEYPAD_5, NUM | SH, {"\33[E", "\33OE", "\33Ou", "\33[E", "\33E", "\33?u"}},
    {HID_KEY_KEYPAD_ADD, NUM | SH, {CHR, CHR, "\33Ok", CHR, CHR, "\33?k"}},
    /* The keypad with NumLock off: the navigation keys, or a VT100's keypad
     * when the program asked for it, with plus as xterm has it. A modifier
     * goes before the letter, and a VT52 has none. */
    {HID_KEY_KEYPAD_8, 0, {"\33[A", "\33OA", "\33Ox", "\33[A", "\33A", "\33?x"}},
    {HID_KEY_KEYPAD_8, SH, {"\33[1;2A", "\33[1;2A", "\33O2x", "\33[1;2A", "\33A", "\33?x"}},
    {HID_KEY_KEYPAD_1, 0, {"\33[F", "\33OF", "\33Oq", "\33[F", "\33F", "\33?q"}},
    {HID_KEY_KEYPAD_7, 0, {"\33[H", "\33OH", "\33Ow", "\33[H", "\33H", "\33?w"}},
    {HID_KEY_KEYPAD_9, 0, {"\33[5~", "\33[5~", "\33Oy", "\33[5~", "\33[5~", "\33?y"}},
    {HID_KEY_KEYPAD_0, 0, {"\33[2~", "\33[2~", "\33Op", "\33[2~", "\33[2~", "\33?p"}},
    {HID_KEY_KEYPAD_DECIMAL, 0, {"\33[3~", "\33[3~", "\33On", "\33[3~", "\33[3~", "\33?n"}},
    {HID_KEY_KEYPAD_5, 0, {"\33[E", "\33OE", "\33Ou", "\33[E", "\33E", "\33?u"}},
    {HID_KEY_KEYPAD_5, VTKEY_CTRL, {"\33[1;5E", "\33[1;5E", "\33O5u", "\33[1;5E", "\33E", "\33?u"}},
    {HID_KEY_KEYPAD_DIVIDE, 0, {CHR, CHR, "\33Oo", CHR, CHR, "\33?o"}},
    {HID_KEY_KEYPAD_MULTIPLY, 0, {CHR, CHR, "\33Oj", CHR, CHR, "\33?j"}},
    {HID_KEY_KEYPAD_SUBTRACT, 0, {CHR, CHR, "\33Om", CHR, CHR, "\33?m"}},
    {HID_KEY_KEYPAD_ADD, 0, {CHR, CHR, "\33Ok", CHR, CHR, "\33?k"}},
    {HID_KEY_KEYPAD_ADD, VTKEY_CTRL, {CHR, CHR, "\33O5k", CHR, CHR, "\33?k"}},
    {HID_KEY_KEYPAD_COMMA, 0, {",", ",", "\33Ol", ",", ",", "\33?l"}},
    {HID_KEY_KEYPAD_EQUAL, 0, {CHR, CHR, "\33OX", CHR, CHR, "\33?X"}},
    {HID_KEY_KEYPAD_ENTER, 0, {"\r", "\r", "\33OM", "\r\n", "\r", "\33?M"}},
    {HID_KEY_KEYPAD_ENTER, VTKEY_ALT, {"\r", "\r", "\33O3M", "\r\n", "\r", "\33?M"}},
    /* Keys with a character, and keys that send nothing */
    {0x04, 0, {CHR, CHR, CHR, CHR, CHR, CHR}},
    {0x04, VTKEY_CTRL | VTKEY_ALT, {CHR, CHR, CHR, CHR, CHR, CHR}},
    {HID_KEY_SPACE, 0, {CHR, CHR, CHR, CHR, CHR, CHR}},
    {HID_KEY_NONE, 0, {CHR, CHR, CHR, CHR, CHR, CHR}},
    {0x46, 0, {"", "", "", "", "", ""}},
    {HID_KEY_SCROLL_LOCK, 0, {"", "", "", "", "", ""}},
    {0x48, 0, {"", "", "", "", "", ""}},
};

UTEST(keyboard, every_key_class_in_every_mode)
{
    static const unsigned modes[6] = {
        0, M_CKM, M_KPAM, M_LNM, M_VT52, M_VT52 | M_CKM | M_KPAM | M_LNM};
    for (size_t i = 0; i < sizeof key_table / sizeof key_table[0]; i++)
        for (int m = 0; m < 6; m++)
        {
            char out[VTKEY_MAX];
            const char *want = key_table[i].want[m];
            key_modes(modes[m]);
            int n = t_encode(out, key_table[i].usage, key_table[i].flags);
            key_modes(0);
            int want_n = want ? (int)strlen(want) : VTKEY_CHAR;
            if (n != want_n || (n > 0 && memcmp(out, want, (size_t)n)))
            {
                printf("key_table[%d] usage 0x%02X flags %d mode %d\n",
                       (int)i, key_table[i].usage, key_table[i].flags, m);
                ASSERT_TRUE(false);
            }
        }
}

/* A mode touches only its own keys, so every combination of modes agrees
 * with the modes taken one at a time, and nothing outruns the buffer. */
UTEST(keyboard, the_modes_do_not_cross)
{
    for (unsigned modes = 0; modes < 16; modes++)
        for (unsigned usage = 0; usage < 256; usage++)
            for (unsigned flags = 0; flags < 32; flags++)
            {
                char out[VTKEY_MAX + 1], alone[VTKEY_MAX + 1];
                bool pad = (usage >= HID_KEY_KEYPAD_DIVIDE &&
                            usage <= HID_KEY_KEYPAD_DECIMAL) ||
                           usage == HID_KEY_KEYPAD_EQUAL ||
                           usage == HID_KEY_KEYPAD_COMMA;
                bool off = !(flags & NUM) || (flags & SH);
                bool nav = pad && usage >= HID_KEY_KEYPAD_1 &&
                           usage <= HID_KEY_KEYPAD_DECIMAL && off;
                bool app = pad && off && (modes & M_KPAM);
                bool enter = !app && (usage == HID_KEY_ENTER ||
                                      usage == HID_KEY_KEYPAD_ENTER);
                unsigned own = M_VT52;
                if (app)
                    own |= M_KPAM;
                else if (enter)
                    own |= M_LNM;
                else if (!pad || nav)
                    own |= M_CKM;
                out[VTKEY_MAX] = alone[VTKEY_MAX] = 0x55;
                key_modes(modes);
                int n = t_encode(out, (uint8_t)usage, (uint8_t)flags);
                key_modes(modes & own);
                int n_alone = t_encode(alone, (uint8_t)usage, (uint8_t)flags);
                key_modes(0);
                ASSERT_GE(n, VTKEY_CHAR);
                ASSERT_LE(n, VTKEY_MAX);
                ASSERT_EQ(out[VTKEY_MAX], 0x55);
                ASSERT_EQ(n, n_alone);
                if (n > 0)
                    ASSERT_EQ(0, memcmp(out, alone, (size_t)n));
            }
}

/* xterm's modifyOtherKeys: what a key sends at levels 0 to 3. A key with a
 * character goes through vtkey_char, the others through vtkey_encode. In a
 * wanted string "\200" stands for a NUL. */
static const struct
{
    uint8_t usage;
    char ch;
    uint8_t flags;
    const char *want[4];
} other_table[] = {
    {0x04, 'a', 0, {"a", "a", "a", "\33[27;1;97~"}},
    {0x04, 'A', SH, {"A", "A", "\33[27;2;65~", "\33[27;2;65~"}},
    {0x04, 'a', VTKEY_CTRL, {"\1", "\1", "\33[27;5;97~", "\33[27;5;97~"}},
    {0x04, 'A', VTKEY_CTRL | SH, {"\1", "\33[27;6;65~", "\33[27;6;65~", "\33[27;6;65~"}},
    {0x04, 'a', VTKEY_ALT, {"\33a", "\33a", "\33[27;3;97~", "\33[27;3;97~"}},
    {0x04, 'a', VTKEY_ALT | VTKEY_CTRL, {"\33\1", "\33\1", "\33[27;7;97~", "\33[27;7;97~"}},
    {0x04, 'A', 7, {"\33\1", "\33\33[27;6;65~", "\33[27;8;65~", "\33[27;8;65~"}},
    {0x1E, '1', 0, {"1", "1", "1", "\33[27;1;49~"}},
    {0x1E, '!', SH, {"!", "!", "!", "\33[27;2;33~"}},
    {0x1E, '1', VTKEY_CTRL, {"1", "\33[27;5;49~", "\33[27;5;49~", "\33[27;5;49~"}},
    {0x1E, '1', VTKEY_ALT, {"\0331", "\0331", "\33[27;3;49~", "\33[27;3;49~"}},
    {0x1F, '2', VTKEY_CTRL, {"\200", "\200", "\33[27;5;50~", "\33[27;5;50~"}},
    {0x1F, '@', VTKEY_CTRL | SH, {"\200", "\33[27;6;64~", "\33[27;6;64~", "\33[27;6;64~"}},
    {HID_KEY_SPACE, ' ', SH, {" ", " ", "\33[27;2;32~", "\33[27;2;32~"}},
    {HID_KEY_SPACE, ' ', VTKEY_CTRL, {"\200", "\200", "\33[27;5;32~", "\33[27;5;32~"}},
    {0x38, '/', VTKEY_CTRL, {"\37", "\37", "\33[27;5;47~", "\33[27;5;47~"}},
    {0x38, '?', VTKEY_CTRL | SH, {"\177", "\33[27;6;63~", "\33[27;6;63~", "\33[27;6;63~"}},
    {0x04, '\351', 0, {"\351", "\351", "\351", "\33[27;1;233~"}},
    {HID_KEY_ENTER, 0, 0, {"\r", "\r", "\r", "\33[27;1;13~"}},
    {HID_KEY_ENTER, 0, SH, {"\r", "\33[27;2;13~", "\33[27;2;13~", "\33[27;2;13~"}},
    {HID_KEY_ENTER, 0, VTKEY_CTRL, {"\r", "\33[27;5;13~", "\33[27;5;13~", "\33[27;5;13~"}},
    {HID_KEY_ENTER, 0, VTKEY_ALT, {"\33\r", "\33\r", "\33[27;3;13~", "\33[27;3;13~"}},
    {HID_KEY_ENTER, 0, VTKEY_ALT | SH, {"\33\r", "\33\33[27;2;13~", "\33[27;4;13~", "\33[27;4;13~"}},
    {HID_KEY_TAB, 0, 0, {"\t", "\t", "\t", "\33[27;1;9~"}},
    {HID_KEY_TAB, 0, SH, {"\33[Z", "\33[Z", "\33[27;2;9~", "\33[27;2;9~"}},
    {HID_KEY_TAB, 0, VTKEY_CTRL, {"\t", "\33[27;5;9~", "\33[27;5;9~", "\33[27;5;9~"}},
    {HID_KEY_TAB, 0, VTKEY_CTRL | SH, {"\33[Z", "\33[27;6;9~", "\33[27;6;9~", "\33[27;6;9~"}},
    {HID_KEY_ESCAPE, 0, 0, {"\33", "\33", "\33", "\33[27;1;27~"}},
    {HID_KEY_ESCAPE, 0, SH, {"\33", "\33", "\33[27;2;27~", "\33[27;2;27~"}},
    {HID_KEY_BACKSPACE, 0, 0, {"\177", "\177", "\177", "\33[27;1;127~"}},
    {HID_KEY_BACKSPACE, 0, SH, {"\177", "\177", "\33[27;2;127~", "\33[27;2;127~"}},
    {HID_KEY_BACKSPACE, 0, VTKEY_CTRL, {"\b", "\b", "\b", "\33[27;1;8~"}},
    /* The keys with sequences of their own, and the keypad, are as ever. */
    {HID_KEY_ARROW_UP, 0, VTKEY_CTRL, {"\33[1;5A", "\33[1;5A", "\33[1;5A", "\33[1;5A"}},
    {HID_KEY_F5, 0, SH, {"\33[15;2~", "\33[15;2~", "\33[15;2~", "\33[15;2~"}},
    {HID_KEY_DELETE, 0, 0, {"\33[3~", "\33[3~", "\33[3~", "\33[3~"}},
    {HID_KEY_KEYPAD_ENTER, 0, SH, {"\r", "\r", "\r", "\r"}},
    {HID_KEY_KEYPAD_ADD, '+', NUM | VTKEY_CTRL, {"+", "+", "+", "+"}},
};

static int other_send(char *out, size_t i)
{
    int n = t_encode(out, other_table[i].usage, other_table[i].flags);
    if (n == VTKEY_CHAR)
        n = vtkey_char(out, other_table[i].usage, other_table[i].ch,
                       other_table[i].flags & VTKEY_MODIFIERS);
    return n;
}

UTEST(keyboard, modify_other_keys_at_every_level)
{
    for (size_t i = 0; i < sizeof other_table / sizeof other_table[0]; i++)
        for (unsigned level = 0; level < 4; level++)
        {
            char out[VTKEY_MAX + 1], want[VTKEY_MAX + 1];
            strcpy(want, other_table[i].want[level]);
            const int want_n = (int)strlen(want);
            for (int k = 0; k < want_n; k++)
                if (want[k] == '\200')
                    want[k] = 0;
            vterm.modify_other_keys = (uint8_t)level;
            const int n = other_send(out, i);
            vterm.modify_other_keys = 0;
            if (n != want_n || memcmp(out, want, (size_t)n))
            {
                printf("other_table[%d] level %u\n", (int)i, level);
                ASSERT_TRUE(false);
            }
        }
}

/* formatOtherKeys writes the same keys the shorter way, and VT52 mode has
 * neither. */
UTEST(keyboard, format_other_keys_and_vt52)
{
    char out[VTKEY_MAX];
    vterm.modify_other_keys = 2;
    vterm.format_other_keys = true;
    ASSERT_EQ(vtkey_char(out, 0x04, 'a', VTKEY_CTRL), 7);
    ASSERT_EQ(0, memcmp(out, "\33[97;5u", 7));
    ASSERT_EQ(t_encode(out, HID_KEY_ENTER, SH), 7);
    ASSERT_EQ(0, memcmp(out, "\33[13;2u", 7));
    vterm.format_other_keys = false;
    vterm.vt52 = true;
    ASSERT_EQ(vtkey_char(out, 0x04, 'a', VTKEY_CTRL), 1);
    ASSERT_EQ(out[0], 1);
    ASSERT_EQ(t_encode(out, HID_KEY_ENTER, SH), 1);
    vterm.vt52 = false;
    vterm.modify_other_keys = 0;
}

UTEST(keyboard, hostkey_types_what_the_encoder_says)
{
    char b[16];

    /* A keypad digit is the host's character with NumLock on and the
     * navigation key with it off or with Shift. KP5 with NumLock off is
     * Begin, even when the host has a character for it. */
    com_rx_clear();
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_8, NUM, '8'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_8, 0, '8'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_8, NUM | SH, '8'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_5, 0, '5'), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_8, NUM, 0), HOSTKEY_NONE);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 10);
    ASSERT_EQ(0, memcmp(b, "8\33[A\33[A\33[E", 10));

    /* An operator is the host's character until the application keypad
     * takes it, where plus is a VT100's comma. */
    key_modes(M_KPAM);
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_ADD, 0, 0), HOSTKEY_TYPED);
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_ADD, NUM, 0), HOSTKEY_NONE);
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_ADD, NUM, '+'), HOSTKEY_TYPED);
    key_modes(0);
    ASSERT_EQ(t_press(HID_KEY_KEYPAD_ADD, 0, 0), HOSTKEY_NONE);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 4);
    ASSERT_EQ(0, memcmp(b, "\33Ok+", 4));

    /* The modes reach a host's keys. */
    key_modes(M_CKM | M_LNM);
    t_press(HID_KEY_ARROW_LEFT, 0, 0);
    t_press(HID_KEY_ENTER, 0, 0);
    key_modes(M_VT52);
    t_press(HID_KEY_ARROW_LEFT, VTKEY_CTRL, 0);
    ASSERT_EQ(t_press(HID_KEY_F5, VTKEY_CTRL, 0), HOSTKEY_TYPED);
    key_modes(0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 12);
    ASSERT_EQ(0, memcmp(b, "\33OD\r\n\33D\33[15~", 12));

    /* Alt puts ESC in front of a character or Enter, in VT52 mode too. */
    t_press(0x38, VTKEY_ALT | SH, '?');
    t_press(HID_KEY_ENTER, VTKEY_ALT, 0);
    key_modes(M_VT52);
    t_press(0x38, VTKEY_ALT | SH, '?');
    t_press(HID_KEY_ENTER, VTKEY_ALT, 0);
    key_modes(0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 8);
    ASSERT_EQ(0, memcmp(b, "\33?\33\r\33?\33\r", 8));
}

/* Alt-F4 in a launcher's child quits it, and its key held repeats as
 * nothing: it neither quits again nor types. A new press asks again, and
 * with the program stopping Alt-F4 is an ordinary key, which repeats. */
UTEST(keyboard, a_held_chord_acts_once)
{
    const uint8_t alt = KEYBOARD_MODIFIER_LEFTALT;
    char b[16];
    com_rx_clear();
    sys_run();
    sys_commit();
    arg_clear();
    arg_append("launcher.rp6502");
    proc_run();
    proc_set_launcher(true);
    arg_clear();
    arg_append("child.rp6502");
    proc_run();
    proc_set_exit_code(0);

    ASSERT_EQ(hostkey_press(HID_KEY_F4, alt, false, true), HOSTKEY_CHORD);
    ASSERT_EQ(proc_get_exit_code(), -1);
    proc_set_exit_code(0);
    ASSERT_EQ(hostkey_press(HID_KEY_F4, alt, true, true), HOSTKEY_CHORD);
    ASSERT_EQ(hostkey_press(HID_KEY_F4, alt, true, true), HOSTKEY_CHORD);
    ASSERT_EQ(proc_get_exit_code(), 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 0);

    ASSERT_EQ(hostkey_press(HID_KEY_F4, alt, false, true), HOSTKEY_TYPED);
    ASSERT_EQ(hostkey_press(HID_KEY_F4, alt, true, true), HOSTKEY_TYPED);
    ASSERT_EQ(proc_get_exit_code(), 0);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 12);
    ASSERT_EQ(0, memcmp(b, "\33[1;3S\33[1;3S", 12));

    /* The launcher goes first, so the stop relaunches nothing. */
    proc_chain_reset();
    sys_commit();
    ASSERT_FALSE(sys_active());
}

/* A machine asks this before it types a repeat. */
UTEST(keyboard, repeat_follows_the_terminal)
{
    vterm.autorepeat = false;
    ASSERT_FALSE(vtkey_repeats());
    vterm.autorepeat = true;
    ASSERT_TRUE(vtkey_repeats());
}

UTEST(keyboard, char_to_oem)
{
    char b[32];
    str_init(); /* the default locale, EN, uses code page 437 */

    com_rx_clear();
    hostkey_char('H');
    hostkey_char(0xE9); /* 'é' -> cp437 0x82 */
    /* DEL stands for a character the code page lacks, and the line editor
     * reads DEL as a backspace, so '?' is queued. */
    hostkey_char(0x1F600);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "H\x82?", 3));
}

/* font_init rebuilds the glyph store for code page 437, and oem_init then
 * loads the active code page's glyphs into it, so OEM_DRIVER has to come
 * after FONT_DRIVER in the driver list. The case calls sys_init because it
 * checks that order. */
UTEST(oem, glyph_store_follows_the_run_page)
{
    ASSERT_TRUE(oem_set_code_page(850));
    sys_init();
    ASSERT_EQ((uint16_t)850, oem_get_code_page_run());
    ASSERT_EQ(oem_get_code_page_run(), vterm_get_code_page());

    ASSERT_TRUE(oem_set_code_page(0)); /* back to auto */
    str_init();
    sys_init();
    ASSERT_EQ(oem_get_code_page_run(), vterm_get_code_page());
}

/* The console's code page is the terminal's: a change resets it, the same
 * page again does not, and a load, as a savestate makes, never does. A mode
 * the reset clears shows which happened. */
UTEST(font, a_page_change_resets_the_terminal_and_a_load_does_not)
{
    font_load_code_page(437);
    vterm.app_cursor = true;
    font_set_code_page(437);
    ASSERT_TRUE(vterm.app_cursor);

    font_set_code_page(850);
    ASSERT_EQ((uint16_t)850, vterm_get_code_page());
    ASSERT_FALSE(vterm.app_cursor);

    vterm.app_cursor = true;
    font_load_code_page(437);
    ASSERT_EQ((uint16_t)437, vterm_get_code_page());
    ASSERT_TRUE(vterm.app_cursor);

    /* A page the fonts lack is 0, ASCII alone, and a change too. */
    font_set_code_page(1252);
    ASSERT_EQ((uint16_t)0, vterm_get_code_page());
    ASSERT_FALSE(vterm.app_cursor);

    vterm.app_cursor = false;
    font_load_code_page(oem_get_code_page_run());
}

UTEST(oem, utf8_string_roundtrip)
{
    str_init(); /* the default locale, EN, uses code page 437 */

    char oem[16], u8[16];
    ASSERT_EQ(oem_from_utf8("caf\xC3\xA9", oem, sizeof oem), (size_t)4);
    ASSERT_STREQ(oem, "caf\x82"); /* CP437 'é' */
    ASSERT_EQ(oem_to_utf8(oem, u8, sizeof u8), (size_t)5);
    ASSERT_STREQ(u8, "caf\xC3\xA9");

    /* U+1F600 is not in code page 437, and 0xFF is not a UTF-8 lead byte, so
     * each converts to 0x7F. */
    ASSERT_EQ(oem_from_utf8("\xF0\x9F\x98\x80", oem, sizeof oem), (size_t)1);
    ASSERT_EQ((unsigned char)oem[0], 0x7Fu);
    ASSERT_EQ(oem_from_utf8("\xFF", oem, sizeof oem), (size_t)1);
    ASSERT_EQ((unsigned char)oem[0], 0x7Fu);

    /* 0xC0 0xAF is an overlong '/', which decodes to 0x7F */
    ASSERT_EQ(oem_from_utf8("A\xC0\xAF", oem, sizeof oem), (size_t)2);
    ASSERT_EQ(oem[0], 'A');
    ASSERT_EQ((unsigned char)oem[1], 0x7Fu);

    /* As with snprintf, the return is the untruncated length. A sequence is
     * never split, so a 2-byte dst, which cannot hold 'é' (two UTF-8 bytes)
     * and the NUL, gets none of it. */
    ASSERT_EQ(oem_to_utf8("\x82", u8, 2), (size_t)2);
    ASSERT_EQ(u8[0], 0);
    ASSERT_EQ(oem_from_utf8("caf\xC3\xA9", oem, 3), (size_t)4);
    ASSERT_STREQ(oem, "ca");

    uint16_t w[3] = {'a', 0x00E9, 0x2603}; /* 'a' 'é' snowman */
    ASSERT_EQ(oem_from_wide_n(w, 3, oem, sizeof oem), (size_t)3);
    ASSERT_EQ(oem[0], 'a');
    ASSERT_EQ((unsigned char)oem[1], 0x82u);
    ASSERT_EQ((unsigned char)oem[2], 0x7Fu);

    /* U+1F600 is a surrogate pair, one character, so one 0x7F */
    uint16_t pair[3] = {'a', 0xD83D, 0xDE00};
    ASSERT_EQ(oem_from_wide_n(pair, 3, oem, sizeof oem), (size_t)2);
    ASSERT_EQ(oem[0], 'a');
    ASSERT_EQ((unsigned char)oem[1], 0x7Fu);
    ASSERT_EQ(oem[2], 0);
    ASSERT_EQ(oem_from_wide_n(w, 3, oem, 2), (size_t)3);
    ASSERT_STREQ(oem, "a");

    /* 'ã' is 0xC6 in CP850 and is absent from CP437 */
    oem_set_code_page_run(850);
    ASSERT_EQ(oem_from_utf8("\xC3\xA3", oem, sizeof oem), (size_t)1);
    ASSERT_EQ((unsigned char)oem[0], 0xC6u);
    str_init();
}

UTEST(cli, rom_args_after_separator)
{
    cli_options o;
    cli_options_init(&o);
    char *argv[] = {"emu", "rom.rp6502", "--", "--looks-like-an-option", "b"};
    ASSERT_EQ(cli_parse_args(5, argv, &o), 0);
    ASSERT_STREQ(o.rom, "rom.rp6502");
    ASSERT_EQ(o.n_rom_args, 2);
    ASSERT_STREQ(o.rom_args[0], "--looks-like-an-option");
    ASSERT_STREQ(o.rom_args[1], "b");
}

UTEST(cli, rom_args_with_install_form)
{
    cli_options o;
    cli_options_init(&o);
    char *argv[] = {"emu", "--install", "x.rp6502", "--", "a"};
    ASSERT_EQ(cli_parse_args(5, argv, &o), 0);
    ASSERT_EQ(o.n_installs, 1);
    ASSERT_TRUE(o.rom == NULL);
    ASSERT_EQ(o.n_rom_args, 1);
    ASSERT_STREQ(o.rom_args[0], "a");
}

UTEST(cli, rom_args_bare_separator_and_passes)
{
    cli_options o;
    cli_options_init(&o);
    char *asset[] = {"emulator", "--mute", "--", "x"};
    ASSERT_EQ(cli_parse_args(4, asset, &o), 0);
    ASSERT_EQ(o.n_rom_args, 1);
    ASSERT_STREQ(o.rom_args[0], "x");

    char *cli[] = {"emu", "rom.rp6502", "--"};
    ASSERT_EQ(cli_parse_args(3, cli, &o), 0);
    ASSERT_TRUE(o.rom_args != NULL);
    ASSERT_EQ(o.n_rom_args, 0);
    ASSERT_STREQ(o.rom, "rom.rp6502");

    char *plain[] = {"emu", "--mute"};
    ASSERT_EQ(cli_parse_args(2, plain, &o), 0);
    ASSERT_TRUE(o.rom_args != NULL);
    ASSERT_EQ(o.n_rom_args, 0);
}

UTEST(cli, no_separator_no_rom_args)
{
    cli_options o;
    cli_options_init(&o);
    char *argv[] = {"emu", "rom.rp6502"};
    ASSERT_EQ(cli_parse_args(2, argv, &o), 0);
    ASSERT_TRUE(o.rom_args == NULL);
    ASSERT_EQ(o.n_rom_args, 0);
    ASSERT_STREQ(o.rom, "rom.rp6502");
}

UTEST(cli, save_dir_names_the_folder_behind_save)
{
    cli_options o;
    cli_options_init(&o);
    char *plain[] = {"emu", "rom.rp6502"};
    ASSERT_EQ(cli_parse_args(2, plain, &o), 0);
    ASSERT_TRUE(o.save_dir == NULL);

    cli_options_init(&o);
    char *argv[] = {"emu", "--save-dir", "saves", "rom.rp6502"};
    ASSERT_EQ(cli_parse_args(4, argv, &o), 0);
    ASSERT_STREQ(o.save_dir, "saves");
    ASSERT_STREQ(o.rom, "rom.rp6502");
}

UTEST(cli, batch_headless_and_unpaced)
{
    cli_options o;
    cli_options_init(&o);
    char *argv[] = {"emu", "--crc", "--headless", "--phi2", "0", "rom.rp6502"};
    ASSERT_EQ(cli_parse_args(6, argv, &o), 0);
    ASSERT_TRUE(o.crc);
    ASSERT_TRUE(o.headless);
    ASSERT_TRUE(o.unpaced);
    ASSERT_EQ(o.phi2_khz, 0);

    cli_options_init(&o);
    char *khz[] = {"emu", "--phi2", "4000", "rom.rp6502"};
    ASSERT_EQ(cli_parse_args(4, khz, &o), 0);
    ASSERT_FALSE(o.unpaced);
    ASSERT_EQ(o.phi2_khz, 4000);
}

/* Each aspect is a console 640 wide that fixes the frame, and without one the
 * frame follows the canvas. */
UTEST(cli, aspect_names_the_console_and_fixes_the_frame)
{
    static const struct
    {
        char *aspect;
        int h;
    } cases[] = {{"4:3", 480}, {"16:9", 360}, {"16:10", 400}};
    cli_options o;
    cli_options_init(&o);
    char *plain[] = {"emu", "rom.rp6502"};
    ASSERT_EQ(cli_parse_args(2, plain, &o), 0);
    ASSERT_FALSE(o.fixed_aspect);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
    {
        cli_options_init(&o);
        char *argv[] = {"emu", "--aspect", cases[i].aspect, "rom.rp6502"};
        ASSERT_EQ(cli_parse_args(4, argv, &o), 0);
        ASSERT_TRUE(o.fixed_aspect);
        ASSERT_EQ(o.console_w, 640);
        ASSERT_EQ(o.console_h, cases[i].h);
        ASSERT_STREQ(o.rom, "rom.rp6502");
    }
}

UTEST_MAIN();

UTEST(random, the_machines_seed_does_not_move)
{
    uint32_t first = host_seed();
    ASSERT_EQ(first, host_seed());
    ASSERT_EQ(first, host_seed());
}

UTEST(random, the_same_state_gives_the_same_stream)
{
    uint32_t a = 0x6502C0DE, b = 0x6502C0DE;
    for (int i = 0; i < 16; i++)
        ASSERT_EQ(sys_random_step(&a), sys_random_step(&b));
    ASSERT_EQ(a, b);
}

UTEST(random, zero_is_an_ordinary_state)
{
    uint32_t z = 0;
    uint32_t seen[4];
    for (int i = 0; i < 4; i++)
        seen[i] = sys_random_step(&z);
    ASSERT_NE(z, 0u);
    for (int i = 1; i < 4; i++)
        ASSERT_NE(seen[i], seen[0]);
}

UTEST(random, a_private_stream_does_not_touch_the_machines)
{
    uint32_t mine = 1;
    uint32_t before = sys_random();
    for (int i = 0; i < 1000; i++)
        sys_random_step(&mine);
    uint32_t after = sys_random();
    ASSERT_NE(before, after);
}
