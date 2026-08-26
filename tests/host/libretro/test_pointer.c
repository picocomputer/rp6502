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

    point_at(0.30f, 0.30f, false); /* lifted */
    fe_run(20);
    frame_copy(settled);

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
