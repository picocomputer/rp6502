/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What a frontend's devices become on the way to the machine's.
 *
 * The pad record's bytes are the same ones tests/cpu/hid pins through the
 * script channel; the claim here is not what they mean, which is the
 * machine's business, but that a RetroPad reaches them at all — and that the
 * face buttons arrive under the thumbs that pressed them rather than under
 * the letters printed on them.
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

#define PAD_XREG 0xFF00 /* where gamepad.rp6502 points the pad block */
#define PAD_RECORD 10   /* bytes per player */

static const uint8_t *pad_record(int player)
{
    const uint8_t *xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    return xram ? xram + PAD_XREG + player * PAD_RECORD : NULL;
}

/* A button held. The bench answers both ways a core may read a pad from
 * this, so a case says what is pressed and not how it is fetched. */
static void press(unsigned port, unsigned id, bool down)
{
    fe.input[port][0][id] = down ? 1 : 0;
}

static void start_pad_program(int *utest_result)
{
    memset(fe.input, 0, sizeof fe.input);
    memset(fe.analog, 0, sizeof fe.analog);
    ASSERT_TRUE(fe_load(ROMS_DIR "/gamepad.rp6502"));
    fe_run(40);
}

/* A pad the frontend offers is a pad the machine has: the connected bit is
 * the gate a program reads before anything else. */
UTEST(input, a_port_becomes_a_player)
{
    start_pad_program(utest_result);
    const uint8_t *rec = pad_record(0);
    ASSERT_TRUE(rec != NULL);
    ASSERT_EQ(rec[0] & 0x80, 0x80);
    fe.unload_game();
}

/* Start lands in the byte the machine keeps it in — the same $08 in
 * button1 that tests/cpu/hid pins from the other side. */
UTEST(input, start_reaches_the_record)
{
    start_pad_program(utest_result);
    press(0, RETRO_DEVICE_ID_JOYPAD_START, true);
    fe_run(20);
    const uint8_t *rec = pad_record(0);
    /* Connected, western layout, two sticks — what a RetroPad is. */
    ASSERT_EQ(rec[0], 0xD0);
    ASSERT_EQ(rec[1], 0x00);
    ASSERT_EQ(rec[2], 0x00);
    ASSERT_EQ(rec[3], 0x08);
    fe.unload_game();
}

/* The dpad and the south button land in their own bytes, and the south
 * button is RetroPad B — positional, not by name. */
UTEST(input, the_south_button_is_the_machines_a)
{
    start_pad_program(utest_result);
    press(0, RETRO_DEVICE_ID_JOYPAD_UP, true);
    press(0, RETRO_DEVICE_ID_JOYPAD_B, true);
    fe_run(20);
    const uint8_t *rec = pad_record(0);
    ASSERT_EQ(rec[0], 0xD1); /* connected pad, dpad up */
    ASSERT_EQ(rec[2], 0x01); /* button0 bit 0 is A */
    fe.unload_game();
}

/* The stick derives the digital sticks byte the way the firmware does. */
UTEST(input, a_stick_derives_its_digital_reading)
{
    start_pad_program(utest_result);
    fe.analog[0][RETRO_DEVICE_INDEX_ANALOG_LEFT][RETRO_DEVICE_ID_ANALOG_Y] = -0x7F00;
    fe_run(20);
    const uint8_t *rec = pad_record(0);
    ASSERT_EQ(rec[0], 0xD0);
    ASSERT_EQ(rec[1], 0x01); /* left stick full north */
    /* A stick is not a button: deflecting one presses nothing. */
    ASSERT_EQ(rec[2], 0x00);
    ASSERT_EQ(rec[3], 0x00);
    fe.unload_game();
}

/* A port the frontend says is empty is a player the machine does not have. */
UTEST(input, an_empty_port_blanks_the_record)
{
    start_pad_program(utest_result);
    press(0, RETRO_DEVICE_ID_JOYPAD_START, true);
    fe_run(20);
    ASSERT_EQ(pad_record(0)[0] & 0x80, 0x80);

    fe.set_controller_port_device(0, RETRO_DEVICE_NONE);
    fe_run(20);
    const uint8_t *rec = pad_record(0);
    for (int i = 0; i < PAD_RECORD; i++)
        ASSERT_EQ(rec[i], 0x00);

    fe.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    fe.unload_game();
}

/* Four ports, four players, each its own record. */
UTEST(input, the_second_port_is_the_second_player)
{
    start_pad_program(utest_result);
    press(1, RETRO_DEVICE_ID_JOYPAD_START, true);
    fe_run(20);
    ASSERT_EQ(pad_record(1)[3], 0x08);
    ASSERT_EQ(pad_record(0)[3], 0x00); /* player one pressed nothing */
    fe.unload_game();
}

/* The keyboard reaches the machine through the callback the core registered
 * — the one a frontend only offers if the core asked for it. */
UTEST(input, the_core_asked_for_a_keyboard)
{
    ASSERT_TRUE(fe.keyboard_set);
    ASSERT_TRUE(fe.keyboard.callback != NULL);
}

/* mode2.rp6502 polls the HID bitmap and leaves each of its two loops on a
 * press and release of any key, so reaching the end is the proof the program
 * saw the bits — the same proof tests/cpu/hid takes through the script
 * channel, taken here through the frontend's keyboard callback.
 *
 * And a program that ends is a core that is finished, so this is where the
 * machine stopping reaches the frontend as well. */
UTEST(input, keys_reach_the_program_and_its_end_reaches_the_frontend)
{
    memset(fe.input, 0, sizeof fe.input);
    ASSERT_TRUE(fe_load(ROMS_DIR "/mode2.rp6502"));
    fe_run(20);
    ASSERT_TRUE(fe.keyboard.callback != NULL);
    ASSERT_FALSE(fe.shutdown); /* still running */

    for (int loop = 0; loop < 2; loop++)
    {
        fe.keyboard.callback(true, RETROK_SPACE, ' ', 0);
        fe_run(5);
        fe.keyboard.callback(false, RETROK_SPACE, ' ', 0);
        fe_run(10);
    }

    fe_run(10);
    ASSERT_TRUE(fe.shutdown);
    fe.unload_game();
}

/* A frontend's remapper and its on-screen pad have nothing to print unless
 * the core says what its buttons do. */
UTEST(input, the_buttons_are_labelled_for_the_frontend)
{
    ASSERT_TRUE(fe.input_descriptors_set);
    ASSERT_TRUE(fe.controller_info_set);
}
