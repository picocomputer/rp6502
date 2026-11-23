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

/* The mouse and tablet programs that tests/cpu/hid/pointer.py builds map
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

#define CONTACT_BYTES 4
#define MOUSE_BYTES 5

/* One buffer holds either block, so it is sized for the larger. */
static uint8_t settled[MOUSE_BYTES > CONTACT_BYTES ? MOUSE_BYTES : CONTACT_BYTES];

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
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x00);

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
    ASSERT_EQ(c0[4], 0x01);
    ASSERT_EQ(c0[8], 0x00);
    ASSERT_NE(c0[5] | c0[6] << 8 | c0[7] << 16, c0[1] | c0[2] << 8 | c0[3] << 16);
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
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[4], 0x00);

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
    ASSERT_EQ(xram_at(TABLET_CONTACT0)[0], 0x00);

    fe.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    memset(fe.lightgun, 0, sizeof fe.lightgun);
    fe.unload_game();
}

/* The console's mouse, as a save state holds it. Its chunk, VMOU, ends with
 * contact 0's flags as the console last took them and whether it took that
 * change. A chunk is its id, a two-byte version and a four-byte length, then
 * what was saved. */
static bool console_mouse(uint8_t *flags, bool *took)
{
    const size_t n = fe.serialize_size();
    uint8_t *blob = malloc(n);
    bool found = false;
    if (blob && fe.serialize(blob, n))
        for (size_t at = 0; at + 10 <= n && !found; at++)
        {
            if (memcmp(blob + at, "VMOU", 4))
                continue;
            const uint32_t used = (uint32_t)blob[at + 6] << 24 | (uint32_t)blob[at + 7] << 16 |
                                  (uint32_t)blob[at + 8] << 8 | blob[at + 9];
            if (used < 2 || at + 10 + used > n)
                continue;
            *flags = blob[at + 10 + used - 2];
            *took = blob[at + 10 + used - 1] != 0;
            found = true;
        }
    free(blob);
    return found;
}

/* The console program leaves the console for a canvas with no mode 0 when
 * this byte of its RAM is not 0. */
#define CONSOLE_LEAVE 0x0200

/* With nothing mapped, the console's terminal claims the tablet while mode 0
 * shows, so the pointer is read and reaches the console's mouse. */
UTEST(pointer, the_console_reads_the_pointer_with_nothing_mapped)
{
    fe_close();
    fe_open();
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(rom_loaded(CONSOLE_ROM));
    a_mouse_has_moved();
    finger_at(0, 0.25f, 0.25f, false);
    fe_run(2);
    uint8_t flags = 0;
    bool took = false;
    ASSERT_TRUE(console_mouse(&flags, &took));
    ASSERT_EQ(flags, 0x80); /* hover */
    ASSERT_TRUE(took);
    fe.unload_game();
}

/* A frontend polls once a frame, so a click is a press in one frame and a
 * release in the next; the console's mouse takes each. */
UTEST(pointer, edges_in_consecutive_frames_are_each_delivered)
{
    fe_close();
    fe_open();
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(rom_loaded(CONSOLE_ROM));
    a_mouse_has_moved();
    finger_at(0, 0.25f, 0.25f, false);
    fe_run(2);
    uint8_t flags = 0;
    bool took = false;
    for (int i = 0; i < 2; i++)
    {
        fe.mouse[RETRO_DEVICE_ID_MOUSE_LEFT] = 1;
        fe_run(1);
        ASSERT_TRUE(console_mouse(&flags, &took));
        ASSERT_EQ(flags, 0x81); /* hover + LEFT */
        ASSERT_TRUE(took);
        fe.mouse[RETRO_DEVICE_ID_MOUSE_LEFT] = 0;
        fe_run(1);
        ASSERT_TRUE(console_mouse(&flags, &took));
        ASSERT_EQ(flags, 0x80);
        ASSERT_TRUE(took);
    }
    fe.unload_game();
}

/* A canvas with no mode 0 takes the pointer off the tablet, and a button
 * held as it went is let go there, so the console's mouse is not left
 * pressed. */
UTEST(pointer, a_pointer_taken_off_the_tablet_lets_go)
{
    fe_close();
    fe_open();
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    ASSERT_TRUE(rom_loaded(CONSOLE_ROM));
    a_mouse_has_moved();
    finger_at(0, 0.25f, 0.25f, false);
    fe.mouse[RETRO_DEVICE_ID_MOUSE_LEFT] = 1;
    fe_run(2);
    uint8_t flags = 0;
    bool took = false;
    ASSERT_TRUE(console_mouse(&flags, &took));
    ASSERT_EQ(flags, 0x81);

    uint8_t *ram = fe.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    ASSERT_TRUE(ram != NULL);
    ram[CONSOLE_LEAVE] = 1;
    fe_run(5);
    ASSERT_EQ(fe.frame_w, 320u);
    ASSERT_TRUE(console_mouse(&flags, &took));
    ASSERT_EQ(flags, 0x00);
    memset(fe.mouse, 0, sizeof fe.mouse);
    fe.unload_game();
}

/* Frames of a frontend's input for the console's mouse: the pointer at fx,
 * fy of the image, the buttons held (CLICK_), for that many frames. */
#define CLICK_LEFT 0x01
#define CLICK_MIDDLE 0x02
typedef struct
{
    float fx, fy;
    uint8_t buttons;
    int frames;
} click_step_t;

/* A double click on "these" of the console program's words, a pause past
 * the clicking time, and a triple click there, with the pointer resting
 * between. */
static const click_step_t click_steps[] = {
    {0.12f, 0.02f, 0, 2},          {0.12f, 0.02f, CLICK_LEFT, 1}, {0.12f, 0.02f, 0, 1},
    {0.12f, 0.02f, CLICK_LEFT, 1}, {0.12f, 0.02f, 0, 1},          {0.12f, 0.02f, 0, 30},
    {0.12f, 0.02f, CLICK_LEFT, 1}, {0.12f, 0.02f, 0, 1},          {0.12f, 0.02f, CLICK_LEFT, 1},
    {0.12f, 0.02f, 0, 1},          {0.12f, 0.02f, CLICK_LEFT, 1}, {0.12f, 0.02f, 0, 5},
};

/* A double click on "these", a pause past the clicking time, and a click
 * of the middle button lower down, which pastes the primary to the
 * program, which writes it back after its words. */
static const click_step_t paste_steps[] = {
    {0.12f, 0.02f, 0, 2},          {0.12f, 0.02f, CLICK_LEFT, 1},   {0.12f, 0.02f, 0, 1},
    {0.12f, 0.02f, CLICK_LEFT, 1}, {0.12f, 0.02f, 0, 1},            {0.12f, 0.02f, 0, 30},
    {0.50f, 0.50f, 0, 1},          {0.50f, 0.50f, CLICK_MIDDLE, 1}, {0.50f, 0.50f, 0, 20},
};

/* The image after step mid and after the last step of a run of rom, each
 * frame run as runahead runs it when ahead is set: run, save, run the
 * frame shown, and load back to the first. Only the buttons in held are
 * pressed; the pointer makes the same moves without the others. */
static void click_through(const char *rom, const click_step_t *steps, size_t count, size_t mid,
                          bool ahead, uint8_t held, uint32_t *at_mid, uint32_t *at_end)
{
    static uint8_t blob[1 << 20];
    memset(fe.pointer, 0, sizeof fe.pointer);
    memset(fe.mouse, 0, sizeof fe.mouse);
    fe_close();
    fe_open();
    if (!rom_loaded(rom))
        return;
    a_mouse_has_moved();
    const size_t n = fe.serialize_size();
    for (size_t i = 0; i < count; i++)
    {
        const click_step_t *c = &steps[i];
        const uint8_t pressed = c->buttons & held;
        finger_at(0, c->fx, c->fy, false);
        fe.mouse[RETRO_DEVICE_ID_MOUSE_LEFT] = (pressed & CLICK_LEFT) != 0;
        fe.mouse[RETRO_DEVICE_ID_MOUSE_MIDDLE] = (pressed & CLICK_MIDDLE) != 0;
        for (int f = 0; f < c->frames; f++)
        {
            fe.run();
            if (ahead && n <= sizeof blob && fe.serialize(blob, n))
            {
                fe.run();
                fe.unserialize(blob, n);
            }
        }
        if (i == mid)
            memcpy(at_mid, fe.frame_copy, 640 * 480 * sizeof *fe.frame_copy);
    }
    fe.run();
    memcpy(at_end, fe.frame_copy, 640 * 480 * sizeof *fe.frame_copy);
    memset(fe.mouse, 0, sizeof fe.mouse);
    fe.unload_game();
}

#define CLICKS(steps) steps, sizeof steps / sizeof *steps

/* Runahead runs each frame twice and puts the machine back between, so a
 * double and a triple click count as they do in a straight run, and the
 * highlight each makes is the same. */
UTEST(pointer, runahead_counts_double_and_triple_clicks)
{
    static uint32_t straight[2][640 * 480], ahead[2][640 * 480], none[2][640 * 480];
    click_through(CONSOLE_ROM, CLICKS(click_steps), 5, false, CLICK_LEFT, straight[0], straight[1]);
    click_through(CONSOLE_ROM, CLICKS(click_steps), 5, true, CLICK_LEFT, ahead[0], ahead[1]);
    click_through(CONSOLE_ROM, CLICKS(click_steps), 5, false, 0, none[0], none[1]);
    ASSERT_EQ(memcmp(straight[0], ahead[0], sizeof straight[0]), 0);
    ASSERT_EQ(memcmp(straight[1], ahead[1], sizeof straight[1]), 0);
    ASSERT_NE(memcmp(straight[0], none[0], sizeof straight[0]), 0);
    ASSERT_NE(memcmp(straight[1], none[1], sizeof straight[1]), 0);
    ASSERT_NE(memcmp(straight[0], straight[1], sizeof straight[0]), 0);
}

/* Under runahead a double click selects and a paste of the primary reaches
 * the program as in a straight run: the highlight and the text written
 * back are the same, and the text is there only for the middle button. */
static void paste_through(int *utest_result, const char *rom)
{
    static uint32_t straight[2][640 * 480], ahead[2][640 * 480], unpasted[2][640 * 480];
    const uint8_t both = CLICK_LEFT | CLICK_MIDDLE;
    click_through(rom, CLICKS(paste_steps), 5, false, both, straight[0], straight[1]);
    click_through(rom, CLICKS(paste_steps), 5, true, both, ahead[0], ahead[1]);
    click_through(rom, CLICKS(paste_steps), 5, false, CLICK_LEFT, unpasted[0], unpasted[1]);
    ASSERT_EQ(memcmp(straight[0], ahead[0], sizeof straight[0]), 0);
    ASSERT_EQ(memcmp(straight[1], ahead[1], sizeof straight[1]), 0);
    ASSERT_EQ(memcmp(straight[0], unpasted[0], sizeof straight[0]), 0);
    ASSERT_NE(memcmp(straight[1], unpasted[1], sizeof straight[1]), 0);
}

UTEST(pointer, runahead_keeps_a_paste_and_a_double_click)
{
    paste_through(utest_result, CONSOLE_ROM);
}

/* The same with the console slowed by DECSCS, so the words and the paste
 * written back reach the screen across the frames runahead goes back
 * over. */
UTEST(pointer, runahead_keeps_a_paste_and_a_double_click_when_slowed)
{
    paste_through(utest_result, CONSOLE_PACED_ROM);
}
