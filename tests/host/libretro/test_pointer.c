/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pointer, absolute and relative.
 *
 * The machine has two pointing devices and a program maps whichever it
 * wants: the tablet is absolute and hovers, the mouse is relative and
 * counts. libretro has one abstraction for each, and the claims here are
 * that each reaches the block a program reads — which is what the frames
 * show, since both fixtures draw where they are pointed.
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

/* Somewhere on the canvas, in the [-0x7FFF, 0x7FFF] the pointer speaks. */
static void point_at(float fx, float fy, bool pressed)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_COUNT] = 1;
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_X] = (int16_t)((fx * 2.0f - 1.0f) * 0x7FFF);
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_Y] = (int16_t)((fy * 2.0f - 1.0f) * 0x7FFF);
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_PRESSED] = pressed ? 1 : 0;
}

/* Where pointer.rp6502 maps each block; tests/gen/pointer_rom_gen.py is where
 * these are chosen. The tablet's four-byte header comes before contact 0,
 * whose flags are its first byte. */
#define MOUSE_XREG 0xFF00
#define TABLET_XREG 0xFF10
#define TABLET_CONTACT0 (TABLET_XREG + 4)

static const uint8_t *xram_at(unsigned addr)
{
    const uint8_t *xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    return xram ? xram + addr : NULL;
}

/* pointer.rp6502 draws nothing, so what a move changed is read out of the
 * block the device wrote: six bytes of a contact, five of the mouse. */
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

/* A pointer that moved is a contact record that changed: the coordinate
 * windows are rewritten where the program can read them. */
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

/* Nothing pressed is nothing pointed at, rather than a stale contact left
 * behind. The contacts are touches and no host cursor is claimed, which is
 * why a program has to draw its own: libretro gives a core no way to ask a
 * frontend for a cursor, and a program that hid its own on our word would be
 * left with neither. */
UTEST(pointer, letting_go_ends_the_contact)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    ASSERT_TRUE(fe_load(TABLET_ROM));
    point_at(0.30f, 0.30f, true);
    fe_run(60);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x01); /* tip down */

    point_at(0.30f, 0.30f, false); /* lifted */
    fe_run(20);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x00); /* no contact at all */

    /* A lifted pointer that moves is still not a contact. */
    point_at(0.80f, 0.75f, false);
    fe_run(20);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x00);
    fe.unload_game();
}

/* pointer.rp6502 reads the relative counters under a timer interrupt, so
 * motion is counters that moved and stillness is counters that did not. */
UTEST(pointer, a_relative_pointer_reaches_the_mouse)
{
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(fe_load(MOUSE_ROM));
    fe_run(60);
    block_copy(settled, MOUSE_XREG, MOUSE_BYTES);

    fe_run(20);
    ASSERT_FALSE(block_differs(settled, MOUSE_XREG, MOUSE_BYTES)); /* held still */

    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 80;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_Y] = 60;
    fe_run(2);
    memset(fe.mouse, 0, sizeof fe.mouse); /* one poll's worth of motion */
    fe_run(20);
    ASSERT_TRUE(block_differs(settled, MOUSE_XREG, MOUSE_BYTES));
    fe.unload_game();
}

/* One index of the pointer, as a frontend that walks rather than counts
 * reports it: nothing else in the array is touched. */
static void finger_at(int i, float fx, float fy, bool pressed)
{
    fe.pointer[i][RETRO_DEVICE_ID_POINTER_X] = (int16_t)((fx * 2.0f - 1.0f) * 0x7FFF);
    fe.pointer[i][RETRO_DEVICE_ID_POINTER_Y] = (int16_t)((fy * 2.0f - 1.0f) * 0x7FFF);
    fe.pointer[i][RETRO_DEVICE_ID_POINTER_PRESSED] = pressed ? 1 : 0;
}

/* The fixture booted far enough to map its block: the latch below is learned
 * only while a program is asking, and XRAM before that is the fill. */
static bool rom_loaded(const char *rom)
{
    if (!fe_load(rom))
        return false;
    fe_run(60);
    return true;
}

/* A mouse the frontend has reported once: one poll of motion, then still. */
static void a_mouse_has_moved(void)
{
    memset(fe.mouse, 0, sizeof fe.mouse);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 1;
    fe_run(1);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 0;
}

/* RetroArch's desktop drivers never implement POINTER_COUNT; the header's
 * rule is to walk the index until PRESSED is 0, and a frontend that has shown
 * no mouse gets exactly that. This was the defect: with the count read first,
 * the walk never ran and the tablet never moved on any desktop. */
UTEST(pointer, a_frontend_that_does_not_count_still_points)
{
    fe_close(); /* a session of its own: no mouse has been seen */
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

/* A mouse hovers: once the frontend has shown one, its pointer is a contact
 * with nothing pressed, and the program's own cursor follows it. */
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

/* The pointer has one PRESSED bit; the mouse has three buttons, and they are
 * the ones a hovering mouse reports. A right button is RIGHT, not a tip. */
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

/* Off the game image the cursor is over the rest of the frontend: the tablet
 * has no contact there. */
UTEST(pointer, a_pointer_off_the_image_is_no_contact)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    ASSERT_TRUE(rom_loaded(TABLET_ROM));
    a_mouse_has_moved();
    finger_at(0, 0.50f, 0.50f, false);
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x80);

    /* No contact is six zero bytes: no flags and no position, which is what
     * lets a program leave its pointer where it was rather than sending it
     * to (0,0). */
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 1;
    fe_run(5);
    for (int i = 0; i < 6; i++)
        ASSERT_EQ(xram_at(TABLET_CONTACT0)[i], 0x00);

    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 0;
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x80);
    fe.unload_game();
}

/* The mouse is relative and its OS cursor legitimately parks off the image
 * -- a letterbox is off the image -- so its motion always flows. Only its
 * buttons are muted there, as a release. */
UTEST(pointer, off_the_image_the_mouse_moves_but_presses_nothing)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(rom_loaded(MOUSE_ROM));
    block_copy(settled, MOUSE_XREG, MOUSE_BYTES);

    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 1;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_LEFT] = 1;
    fe_run(5);
    ASSERT_EQ(xram_at(MOUSE_XREG)[0], 0x00); /* the button byte: released */

    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 80;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_Y] = 60;
    fe_run(2);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 0;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_Y] = 0;
    fe_run(20);
    ASSERT_TRUE(block_differs(settled, MOUSE_XREG, MOUSE_BYTES)); /* it still moved */

    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 0;
    fe_run(5);
    ASSERT_EQ(xram_at(MOUSE_XREG)[0], 0x01); /* back over the image, the button is real */
    memset(fe.mouse, 0, sizeof fe.mouse);
    fe.unload_game();
}

/* Two fingers are two contacts, walked in index order until one is not
 * pressed. Nothing exercised the second slot before this. */
UTEST(pointer, two_fingers_are_two_contacts)
{
    fe_close(); /* a session of its own: no mouse has been seen */
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
    ASSERT_EQ(c0[6], 0x01);  /* contact 1, six bytes on */
    ASSERT_EQ(c0[12], 0x00); /* contact 2: not a finger */
    /* Contact 1 is the second position, not a copy of the first. */
    ASSERT_NE(c0[7] | c0[8] << 8 | c0[9] << 16, c0[1] | c0[2] << 8 | c0[3] << 16);
    fe.unload_game();
}

/* The block's button byte, which the mouse fixture maps at PAINT_XREG. */
#define MOUSE_BUTTONS 0

/* Five buttons reach the machine, not three. The two side buttons are
 * BACKWARD and FORWARD in the mouse block, bits 3 and 4. */
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

/* Touch and the mouse take turns: a finger takes the tablet, keeps it while
 * the mouse moves under it, and gives it back once the touch ends and the
 * mouse moves again. */
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
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x80); /* the mouse, hovering */

    /* A finger arrives: tip down, no hover, and it owns the block. */
    finger_at(0, 0.60f, 0.60f, true);
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x01);

    /* The mouse moving does not take it back while the touch is live. */
    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 5;
    fe_run(2);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 0;
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x01);

    /* The touch ends and nothing is pointed at until the mouse moves. */
    finger_at(0, 0.60f, 0.60f, false);
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x00);
    a_mouse_has_moved();
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x80);
    fe.unload_game();
}

/* x11 and udev report a contact per held mouse button, at the mouse's own
 * position. Those are not fingers: a right-click is RIGHT on a hovering
 * pointer, not a tip-down touch that would paint. */
UTEST(pointer, a_held_button_is_not_a_finger)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(rom_loaded(TABLET_ROM));
    a_mouse_has_moved();

    /* What those drivers send for a right-click: index 0 and index 1 both
     * pressed, both at the cursor. */
    finger_at(0, 0.40f, 0.40f, true);
    finger_at(1, 0.40f, 0.40f, true);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_RIGHT] = 1;
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x82);      /* hover + RIGHT */
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[6], 0x00);      /* no second finger */

    memset(fe.mouse, 0, sizeof fe.mouse);
    memset(fe.pointer, 0, sizeof fe.pointer);
    fe.unload_game();
}

/* A lightgun is an absolute pointer with its own buttons, and -0x8000 on an
 * axis is the frontend saying it is off the screen. */
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
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x81); /* hover + tip */
    block_copy(settled, TABLET_CONTACT0, CONTACT_BYTES);

    fe.lightgun[0][RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X] = (int16_t)(0.70f * 0x7FFF);
    fe_run(20);
    ASSERT_TRUE(block_differs(settled, TABLET_CONTACT0, CONTACT_BYTES));

    /* Off the screen: no contact, and the program's pointer stays put. */
    fe.lightgun[0][RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X] = -0x8000;
    fe_run(5);
    for (int i = 0; i < 6; i++)
        ASSERT_EQ(xram_at(TABLET_CONTACT0)[i], 0x00);

    fe.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    memset(fe.lightgun, 0, sizeof fe.lightgun);
    fe.unload_game();
}
