/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "retro_fe.h"
#include "utest.h"

#include <string.h>

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    fe_open();
    int rc = utest_main(argc, argv);
    fe_close();
    return rc;
}

static void point_at(float fx, float fy, bool pressed)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_COUNT] = 1;
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_X] = (int16_t)((fx * 2.0f - 1.0f) * 0x7FFF);
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_Y] = (int16_t)((fy * 2.0f - 1.0f) * 0x7FFF);
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_PRESSED] = pressed ? 1 : 0;
}

/* The mouse and tablet programs that tests/gen/pointer_rom_gen.py builds map
 * their blocks at these addresses. The tablet's four-byte header comes before
 * contact 0, whose flags are its first byte. */
#define MOUSE_XREG 0xFF00
#define TABLET_XREG 0xFF10
#define TABLET_CONTACT0 (TABLET_XREG + 4)

static const uint8_t *xram_at(unsigned addr)
{
    const uint8_t *xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    return xram ? xram + addr : NULL;
}

#define CONTACT_BYTES 6
#define MOUSE_BYTES 5

static uint8_t settled[CONTACT_BYTES];

static void block_copy(uint8_t *dst, unsigned addr, int len)
{
    const uint8_t *src = xram_at(addr);
    if (src)
        memcpy(dst, src, (size_t)len);
}

static bool block_differs(const uint8_t *other, unsigned addr, int len)
{
    const uint8_t *now = xram_at(addr);
    return now && memcmp(other, now, (size_t)len) != 0;
}

UTEST(pointer, an_absolute_pointer_reaches_the_tablet)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    ASSERT_TRUE(fe_load(TABLET_ROM));
    point_at(0.25f, 0.25f, true);
    fe_run(60);
    block_copy(settled, TABLET_CONTACT0, CONTACT_BYTES);

    point_at(0.75f, 0.70f, true);
    fe_run(30);
    ASSERT_TRUE(block_differs(settled, TABLET_CONTACT0, CONTACT_BYTES));
    fe.unload_game();
}

UTEST(pointer, letting_go_ends_the_contact)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    ASSERT_TRUE(fe_load(TABLET_ROM));
    point_at(0.30f, 0.30f, true);
    fe_run(60);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x01); /* tip down */

    point_at(0.30f, 0.30f, false);
    fe_run(20);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x00);

    point_at(0.80f, 0.75f, false);
    fe_run(20);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x00);
    fe.unload_game();
}

UTEST(pointer, a_relative_pointer_reaches_the_mouse)
{
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(fe_load(MOUSE_ROM));
    fe_run(60);
    block_copy(settled, MOUSE_XREG, MOUSE_BYTES);

    fe_run(20);
    ASSERT_FALSE(block_differs(settled, MOUSE_XREG, MOUSE_BYTES));

    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 80;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_Y] = 60;
    fe_run(2);
    memset(fe.mouse, 0, sizeof fe.mouse);
    fe_run(20);
    ASSERT_TRUE(block_differs(settled, MOUSE_XREG, MOUSE_BYTES));
    fe.unload_game();
}

static void finger_at(int i, float fx, float fy, bool pressed)
{
    fe.pointer[i][RETRO_DEVICE_ID_POINTER_X] = (int16_t)((fx * 2.0f - 1.0f) * 0x7FFF);
    fe.pointer[i][RETRO_DEVICE_ID_POINTER_Y] = (int16_t)((fy * 2.0f - 1.0f) * 0x7FFF);
    fe.pointer[i][RETRO_DEVICE_ID_POINTER_PRESSED] = pressed ? 1 : 0;
}

/* The core reads no pointer input until a program maps the mouse or tablet
 * block into XRAM, so until then mouse motion is ignored and the block holds
 * whatever XRAM was filled with at load. rom_loaded runs 60 frames so that
 * the program has mapped its block before a case moves the mouse or reads
 * the block. */
static bool rom_loaded(const char *rom)
{
    if (!fe_load(rom))
        return false;
    fe_run(60);
    return true;
}

static void a_mouse_has_moved(void)
{
    memset(fe.mouse, 0, sizeof fe.mouse);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 1;
    fe_run(1);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 0;
}

UTEST(pointer, a_frontend_that_does_not_count_still_points)
{
    fe_close();
    fe_open();
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(rom_loaded(TABLET_ROM));
    finger_at(0, 0.25f, 0.25f, true);
    fe_run(60);
    block_copy(settled, TABLET_CONTACT0, CONTACT_BYTES);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x01); /* tip down, no hover */

    finger_at(0, 0.75f, 0.70f, true);
    fe_run(30);
    ASSERT_TRUE(block_differs(settled, TABLET_CONTACT0, CONTACT_BYTES));
    fe.unload_game();
}

UTEST(pointer, a_mouse_hovers_before_it_presses)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    ASSERT_TRUE(rom_loaded(TABLET_ROM));
    a_mouse_has_moved();
    finger_at(0, 0.25f, 0.25f, false);
    fe_run(60);
    block_copy(settled, TABLET_CONTACT0, CONTACT_BYTES);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x80); /* hover, nothing pressed */

    finger_at(0, 0.75f, 0.70f, false);
    fe_run(30);
    ASSERT_TRUE(block_differs(settled, TABLET_CONTACT0, CONTACT_BYTES));
    fe.unload_game();
}

UTEST(pointer, a_hovering_mouse_brings_its_own_buttons)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    ASSERT_TRUE(rom_loaded(TABLET_ROM));
    a_mouse_has_moved();
    finger_at(0, 0.50f, 0.50f, false);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_RIGHT] = 1;
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x82); /* hover + RIGHT */

    fe.mouse[RETRO_DEVICE_ID_MOUSE_RIGHT] = 0;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_LEFT] = 1;
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x81); /* hover + LEFT */
    memset(fe.mouse, 0, sizeof fe.mouse);
    fe.unload_game();
}

UTEST(pointer, a_pointer_off_the_image_is_no_contact)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    ASSERT_TRUE(rom_loaded(TABLET_ROM));
    a_mouse_has_moved();
    finger_at(0, 0.50f, 0.50f, false);
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x80);

    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 1;
    fe_run(5);
    for (int i = 0; i < 6; i++)
        ASSERT_EQ(xram_at(TABLET_CONTACT0)[i], 0x00);

    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 0;
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x80);
    fe.unload_game();
}

UTEST(pointer, off_the_image_the_mouse_moves_but_presses_nothing)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(rom_loaded(MOUSE_ROM));
    block_copy(settled, MOUSE_XREG, MOUSE_BYTES);

    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 1;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_LEFT] = 1;
    fe_run(5);
    ASSERT_EQ(xram_at(MOUSE_XREG)[0], 0x00);

    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 80;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_Y] = 60;
    fe_run(2);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 0;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_Y] = 0;
    fe_run(20);
    ASSERT_TRUE(block_differs(settled, MOUSE_XREG, MOUSE_BYTES));

    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 0;
    fe_run(5);
    ASSERT_EQ(xram_at(MOUSE_XREG)[0], 0x01);
    memset(fe.mouse, 0, sizeof fe.mouse);
    fe.unload_game();
}

UTEST(pointer, two_fingers_are_two_contacts)
{
    fe_close();
    fe_open();
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(rom_loaded(TABLET_ROM));
    finger_at(0, 0.25f, 0.25f, true);
    finger_at(1, 0.75f, 0.75f, true);
    finger_at(2, 0.50f, 0.50f, false);
    fe_run(5);
    const uint8_t *c0 = xram_at(TABLET_CONTACT0);
    ASSERT_EQ(c0[0], 0x01);
    ASSERT_EQ(c0[6], 0x01);
    ASSERT_EQ(c0[12], 0x00);
    ASSERT_NE(c0[7] | c0[8] << 8 | c0[9] << 16, c0[1] | c0[2] << 8 | c0[3] << 16);
    fe.unload_game();
}

#define MOUSE_BUTTONS 0

/* The two side buttons are BACKWARD and FORWARD in the mouse block, bits 3
 * and 4. */
UTEST(pointer, the_side_buttons_reach_the_machine)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(rom_loaded(MOUSE_ROM));

    fe.mouse[RETRO_DEVICE_ID_MOUSE_BUTTON_4] = 1;
    fe_run(5);
    ASSERT_EQ(xram_at(MOUSE_XREG)[MOUSE_BUTTONS], 0x08);

    fe.mouse[RETRO_DEVICE_ID_MOUSE_BUTTON_4] = 0;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_BUTTON_5] = 1;
    fe_run(5);
    ASSERT_EQ(xram_at(MOUSE_XREG)[MOUSE_BUTTONS], 0x10);

    memset(fe.mouse, 0, sizeof fe.mouse);
    fe_run(5);
    ASSERT_EQ(xram_at(MOUSE_XREG)[MOUSE_BUTTONS], 0x00);
    fe.unload_game();
}

UTEST(pointer, a_finger_takes_the_tablet_from_the_mouse)
{
    fe_close();
    fe_open();
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(rom_loaded(TABLET_ROM));

    a_mouse_has_moved();
    finger_at(0, 0.30f, 0.30f, false);
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x80);

    finger_at(0, 0.60f, 0.60f, true);
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x01);

    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 5;
    fe_run(2);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 0;
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x01);

    finger_at(0, 0.60f, 0.60f, false);
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x00);
    a_mouse_has_moved();
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x80);
    fe.unload_game();
}

UTEST(pointer, a_held_button_is_not_a_finger)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(rom_loaded(TABLET_ROM));
    a_mouse_has_moved();

    finger_at(0, 0.40f, 0.40f, true);
    finger_at(1, 0.40f, 0.40f, true);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_RIGHT] = 1;
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x82); /* hover + RIGHT */
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[6], 0x00);

    memset(fe.mouse, 0, sizeof fe.mouse);
    memset(fe.pointer, 0, sizeof fe.pointer);
    fe.unload_game();
}

UTEST(pointer, a_lightgun_points_at_the_tablet)
{
    fe_close();
    fe_open();
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    memset(fe.lightgun, 0, sizeof fe.lightgun);
    ASSERT_TRUE(rom_loaded(TABLET_ROM));
    fe.set_controller_port_device(0, RETRO_DEVICE_LIGHTGUN);

    fe.lightgun[0][RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X] = (int16_t)(0.25f * 0x7FFF);
    fe.lightgun[0][RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y] = (int16_t)(0.25f * 0x7FFF);
    fe.lightgun[0][RETRO_DEVICE_ID_LIGHTGUN_TRIGGER] = 1;
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x81); /* hover + LEFT */
    block_copy(settled, TABLET_CONTACT0, CONTACT_BYTES);

    fe.lightgun[0][RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X] = (int16_t)(0.70f * 0x7FFF);
    fe_run(20);
    ASSERT_TRUE(block_differs(settled, TABLET_CONTACT0, CONTACT_BYTES));

    /* libretro.h defines -0x8000 on a lightgun axis as out of bounds. */
    fe.lightgun[0][RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X] = -0x8000;
    fe_run(5);
    for (int i = 0; i < 6; i++)
        ASSERT_EQ(xram_at(TABLET_CONTACT0)[i], 0x00);

    fe.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    memset(fe.lightgun, 0, sizeof fe.lightgun);
    fe.unload_game();
}
