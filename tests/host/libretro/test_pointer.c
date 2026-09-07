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

static void frame_copy(uint32_t *dst)
{
    memcpy(dst, fe.frame_copy, (size_t)fe.frame_w * fe.frame_h * sizeof(uint32_t));
}

static bool frame_differs(const uint32_t *other)
{
    return memcmp(other, fe.frame_copy,
                  (size_t)fe.frame_w * fe.frame_h * sizeof(uint32_t)) != 0;
}

static uint32_t settled[640 * 480];

/* paint_tablet.rp6502 decodes contact 0 and draws there, so a pointer that
 * moved is a picture that changed. */
UTEST(pointer, an_absolute_pointer_reaches_the_tablet)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    ASSERT_TRUE(fe_load(FIXTURES_DIR "/paint_tablet.rp6502"));
    point_at(0.25f, 0.25f, true);
    fe_run(60);
    frame_copy(settled);

    point_at(0.75f, 0.70f, true);
    fe_run(30);
    ASSERT_TRUE(frame_differs(settled));
    fe.unload_game();
}

/* The contacts are touches and no host cursor is claimed, so the program
 * draws its own pointer. That matters because there is nothing here to draw
 * one for it: libretro gives a core no way to ask a frontend for a cursor,
 * and a program that hid its own on our word would be left with neither.
 *
 * The proof is that the pointer is visible at all — the case above moves it
 * and the picture follows, which only happens while the program is drawing
 * it. This one holds the other half: nothing pressed is nothing pointed at,
 * rather than a stale contact left behind. */
UTEST(pointer, letting_go_ends_the_contact)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    ASSERT_TRUE(fe_load(FIXTURES_DIR "/paint_tablet.rp6502"));
    point_at(0.30f, 0.30f, true);
    fe_run(60);
    frame_copy(settled);

    point_at(0.30f, 0.30f, false); /* lifted */
    fe_run(20);
    ASSERT_FALSE(frame_differs(settled)); /* the lift itself moves nothing */

    /* Moving a lifted pointer paints nothing and moves nothing. */
    point_at(0.80f, 0.75f, false);
    fe_run(20);
    ASSERT_FALSE(frame_differs(settled));
    fe.unload_game();
}

/* paint_mouse.rp6502 reads the relative counters under a timer interrupt and
 * moves a sprite, so motion is a picture that changed and stillness is one
 * that did not. */
UTEST(pointer, a_relative_pointer_reaches_the_mouse)
{
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(fe_load(FIXTURES_DIR "/paint_mouse.rp6502"));
    fe_run(60);
    frame_copy(settled);

    fe_run(20);
    ASSERT_FALSE(frame_differs(settled)); /* held still */

    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 80;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_Y] = 60;
    fe_run(2);
    memset(fe.mouse, 0, sizeof fe.mouse); /* one poll's worth of motion */
    fe_run(20);
    ASSERT_TRUE(frame_differs(settled));
    fe.unload_game();
}

/* Both paint fixtures map their block at 0xFFA0 (TABLET_INPUT and MOUSE_INPUT
 * in the examples). The tablet's contact 0 is four bytes in, flags first. */
#define PAINT_XREG 0xFFA0
#define TABLET_CONTACT0 (PAINT_XREG + 4)

static const uint8_t *xram_at(unsigned addr)
{
    const uint8_t *xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    return xram ? xram + addr : NULL;
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
static bool paint_loaded(const char *rom)
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
    ASSERT_TRUE(paint_loaded(FIXTURES_DIR "/paint_tablet.rp6502"));
    finger_at(0, 0.25f, 0.25f, true);
    fe_run(60);
    frame_copy(settled);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x01); /* tip down, no hover */

    finger_at(0, 0.75f, 0.70f, true);
    fe_run(30);
    ASSERT_TRUE(frame_differs(settled));
    fe.unload_game();
}

/* A mouse hovers: once the frontend has shown one, its pointer is a contact
 * with nothing pressed, and the program's own cursor follows it. */
UTEST(pointer, a_mouse_hovers_before_it_presses)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    ASSERT_TRUE(paint_loaded(FIXTURES_DIR "/paint_tablet.rp6502"));
    a_mouse_has_moved();
    finger_at(0, 0.25f, 0.25f, false);
    fe_run(60);
    frame_copy(settled);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x80); /* hover, nothing pressed */

    finger_at(0, 0.75f, 0.70f, false);
    fe_run(30);
    ASSERT_TRUE(frame_differs(settled));
    fe.unload_game();
}

/* The pointer has one PRESSED bit; the mouse has three buttons, and they are
 * the ones a hovering mouse reports. A right button is RIGHT, not a tip. */
UTEST(pointer, a_hovering_mouse_brings_its_own_buttons)
{
    memset(fe.pointer, 0, sizeof fe.pointer);
    ASSERT_TRUE(paint_loaded(FIXTURES_DIR "/paint_tablet.rp6502"));
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
    ASSERT_TRUE(paint_loaded(FIXTURES_DIR "/paint_tablet.rp6502"));
    a_mouse_has_moved();
    finger_at(0, 0.50f, 0.50f, false);
    fe_run(5);
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x80);
    frame_copy(settled);

    /* No contact is six zero bytes -- no position, so the program's pointer
     * stays where it was rather than going to (0,0). */
    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 1;
    fe_run(5);
    for (int i = 0; i < 6; i++)
        ASSERT_EQ(xram_at(TABLET_CONTACT0)[i], 0x00);
    ASSERT_FALSE(frame_differs(settled));

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
    ASSERT_TRUE(paint_loaded(FIXTURES_DIR "/paint_mouse.rp6502"));
    frame_copy(settled);

    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 1;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_LEFT] = 1;
    fe_run(5);
    ASSERT_EQ(xram_at(PAINT_XREG)[0], 0x00); /* the button byte: released */

    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 80;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_Y] = 60;
    fe_run(2);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_X] = 0;
    fe.mouse[RETRO_DEVICE_ID_MOUSE_Y] = 0;
    fe_run(20);
    ASSERT_TRUE(frame_differs(settled)); /* it still moved */

    fe.pointer[0][RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN] = 0;
    fe_run(5);
    ASSERT_EQ(xram_at(PAINT_XREG)[0], 0x01); /* back over the image, the button is real */
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
    ASSERT_TRUE(paint_loaded(FIXTURES_DIR "/paint_tablet.rp6502"));
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
