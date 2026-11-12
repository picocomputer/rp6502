/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/config.h"
#include "core/sys/random.h"
#include "host/host.h"
#include "core/api/xreg.h"
#include "core/str/oem.h"
#include "core/term/font.h"
#include "core/str/str.h"
#include "host/sokol/cli/cli.h"
#include "tb_hostos.h"
#include "core/hid/usage.h"
#include "core/hid/vtkeys.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/com/tio.h"
#include "core/term/term.h"
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
    term_view_t tv;
    term_view(&tv);
    ASSERT_EQ(tv.width, 80);
    ASSERT_EQ(tv.height, 25);

    vga_set_console_size(640, 480);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    vga_canvas_size(&w, &h);
    ASSERT_EQ(h, 480);
    term_view(&tv);
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

/* With no pinned size, no NAWS and no CPR, the size is that of
 * the terminal the canvas holds. */
UTEST(tio, falls_back_to_the_terminal_the_canvas_holds)
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
    tio_set_term_width(0);
    tio_set_term_height(0);
    tio_set_naws_size(0, 0);
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
    {
        ASSERT_TRUE(xreg1(0, 0, want[i].canvas));
        ASSERT_EQ(tio_get_term_width(), want[i].cols);
        ASSERT_EQ(tio_get_term_height(), want[i].rows);
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
    term_view_t tv;
    term_view(&tv);
    ASSERT_EQ(tv.width, 80);
    ASSERT_EQ(tv.height, 30);
    uint8_t blob[XREG_SST_SIZE];
    sst_cursor_t c = {blob, blob + sizeof blob, false};
    xreg_sst_save(&c, 0);
    for (size_t i = 0; i < sizeof blob; i++)
        ASSERT_EQ(blob[i], 0);
}

/* Channel 0 has eight registers, as on the VGA chip, so a write to $12 sets
 * nothing rather than register 2. */
UTEST(vga, channel_0_ignores_registers_it_lacks)
{
    sys_init();
    ASSERT_TRUE(xreg1(0, 0x12, 1));
    uint8_t blob[XREG_SST_SIZE];
    sst_cursor_t c = {blob, blob + sizeof blob, false};
    xreg_sst_save(&c, 0);
    for (size_t i = 0; i < sizeof blob; i++)
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
    ASSERT_EQ(xram[0xFF00 + 2], 0x00); /* wheel default 0 */
    ASSERT_EQ(xram[0xFF00 + 3], 0x00); /* pan default 0 */

    tablet_host_wheel(3, -2);
    ASSERT_EQ(xram[0xFF00 + 2], (uint8_t)3);
    ASSERT_EQ(xram[0xFF00 + 3], (uint8_t)-2);

    tablet_host_wheel(-4, 5);
    ASSERT_EQ(xram[0xFF00 + 2], (uint8_t)-1); /* 3 + (-4) wraps */
    ASSERT_EQ(xram[0xFF00 + 3], (uint8_t)3);  /* -2 + 5 */

    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
    ASSERT_FALSE(tablet_is_mapped());
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
    vtkeys_key(HID_KEY_ARROW_UP, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33[A", 3));

    com_rx_clear();
    vtkeys_key(HID_KEY_F1, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33OP", 3));

    com_rx_clear();
    vtkeys_key(HID_KEY_F5, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 5);
    ASSERT_EQ(0, memcmp(b, "\33[15~", 5));

    com_rx_clear();
    vtkeys_key(HID_KEY_F12, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 5);
    ASSERT_EQ(0, memcmp(b, "\33[24~", 5));

    com_rx_clear();
    vtkeys_key(HID_KEY_INSERT, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 4);
    ASSERT_EQ(0, memcmp(b, "\33[2~", 4));

    com_rx_clear();
    vtkeys_key(HID_KEY_HOME, false, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\33[H", 3));

    /* The xterm modifier parameter is 1 + shift + alt*2 + ctrl*4. */
    com_rx_clear();
    vtkeys_key(HID_KEY_ARROW_UP, true, false, false); /* ctrl -> 5 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;5A", 6));

    com_rx_clear();
    vtkeys_key(HID_KEY_F1, false, true, false); /* shift -> 2 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;2P", 6));

    com_rx_clear();
    vtkeys_key(HID_KEY_END, false, true, true); /* shift+alt -> 4 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[1;4F", 6));

    com_rx_clear();
    vtkeys_key(HID_KEY_PAGE_UP, true, false, false); /* ctrl -> 5 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\33[5;5~", 6));

    com_rx_clear();
    vtkeys_key(HID_KEY_ENTER, false, false, false);
    vtkeys_key(HID_KEY_BACKSPACE, false, false, false);
    vtkeys_key(HID_KEY_BACKSPACE, true, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\r\x7f\x08", 3));
}

UTEST(keyboard, ctrl_and_alt_on_control_keys)
{
    char b[16];

    com_rx_clear();
    vtkeys_key(HID_KEY_ENTER, true, false, false);
    vtkeys_key(HID_KEY_TAB, true, false, false);
    vtkeys_key(HID_KEY_ESCAPE, true, false, false);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "\r\t\x1b", 3));

    com_rx_clear();
    vtkeys_key(HID_KEY_ENTER, false, false, true);
    vtkeys_key(HID_KEY_TAB, false, false, true);
    vtkeys_key(HID_KEY_ESCAPE, false, false, true);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 6);
    ASSERT_EQ(0, memcmp(b, "\x1b\r\x1b\t\x1b\x1b", 6));

    com_rx_clear();
    vtkeys_key(HID_KEY_BACKSPACE, false, false, true);
    vtkeys_key(HID_KEY_BACKSPACE, true, false, true);
    ASSERT_EQ(keyboard_drain(b, sizeof b), 4);
    ASSERT_EQ(0, memcmp(b, "\x1b\x7f\x1b\x08", 4));
}

UTEST(keyboard, text_to_oem)
{
    char b[32];
    str_init(); /* the default locale, EN, uses code page 437 */

    com_rx_clear();
    vtkeys_text("Hi!");
    ASSERT_EQ(keyboard_drain(b, sizeof b), 3);
    ASSERT_EQ(0, memcmp(b, "Hi!", 3));

    com_rx_clear();
    vtkeys_text("\xC3\xA9"); /* U+00E9 'é' -> cp437 0x82 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 1);
    ASSERT_EQ((unsigned char)b[0], 0x82u);

    com_rx_clear();
    /* The decoder returns DEL for a character the code page lacks, and the
     * line editor reads DEL as a backspace, so vtkeys_text queues '?'. */
    vtkeys_text("\xF0\x9F\x98\x80"); /* U+1F600 */
    ASSERT_EQ(keyboard_drain(b, sizeof b), 1);
    ASSERT_EQ(b[0], '?');
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
    ASSERT_EQ(oem_get_code_page_run(), font_get_code_page());

    ASSERT_TRUE(oem_set_code_page(0)); /* back to auto */
    str_init();
    sys_init();
    ASSERT_EQ(oem_get_code_page_run(), font_get_code_page());
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

UTEST(units, host_text_keeps_control_bytes)
{
    char out[32];
    size_t taken = 0;
    size_t n = oem_from_utf8_run("a\nb\r\nc\r", 7, true, out, sizeof out, &taken);
    ASSERT_EQ(taken, (size_t)7);
    ASSERT_EQ(n, (size_t)7);
    ASSERT_EQ(memcmp(out, "a\nb\r\nc\r", 7), 0);
    n = oem_from_utf8_run("\33[A\3\177", 6, true, out, sizeof out, &taken);
    ASSERT_EQ(n, (size_t)6);
    ASSERT_EQ(memcmp(out, "\33[A\3\177", 6), 0);
}

UTEST(units, host_text_carries_a_sequence_split_across_two_reads)
{
    char out[32];
    size_t taken = 0;
    size_t n = oem_from_utf8_run("h\xc3", 2, false, out, sizeof out, &taken);
    ASSERT_EQ(n, (size_t)1);
    ASSERT_EQ(out[0], 'h');
    ASSERT_EQ(taken, (size_t)1); /* the lead byte is left for the next call */
    n = oem_from_utf8_run("\xc3\xa9!", 3, false, out, sizeof out, &taken);
    ASSERT_EQ(taken, (size_t)3);
    ASSERT_EQ(n, (size_t)2);
    ASSERT_EQ((unsigned char)out[0], 0x82); /* CP437 é */
    ASSERT_EQ(out[1], '!');
}

UTEST(units, host_text_spells_what_the_code_page_cannot_as_a_question_mark)
{
    char out[8];
    size_t taken = 0;
    size_t n = oem_from_utf8_run("\xe4\xb8\xad", 3, true, out, sizeof out, &taken);
    ASSERT_EQ(n, (size_t)1);
    ASSERT_EQ(out[0], '?');
    n = oem_from_utf8_run("abcd", 4, true, out, 2, &taken);
    ASSERT_EQ(n, (size_t)2);
    ASSERT_EQ(taken, (size_t)2);
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
