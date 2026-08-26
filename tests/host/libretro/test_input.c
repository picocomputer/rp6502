/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What a frontend's devices become on the way to the machine's.
 *
 * The gamepad record's bytes are the same ones tests/cpu/hid pins through the
 * script channel; the claim here is not what they mean, which is the
 * machine's business, but that a RetroPad reaches them at all — and that the
 * face buttons arrive under the thumbs that pressed them rather than under
 * the letters printed on them.
 */

#include "retro_fe.h"
#include "utest.h"

#include <stdio.h>
#include <string.h>

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    fe_open();
    int rc = utest_main(argc, argv);
    fe_close();
    return rc;
}

#define GAMEPAD_XREG 0xFF00 /* where gamepad.rp6502 points the gamepad block */
#define GAMEPAD_RECORD 10   /* bytes per player */

static const uint8_t *gamepad_record(int player)
{
    const uint8_t *xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    return xram ? xram + GAMEPAD_XREG + player * GAMEPAD_RECORD : NULL;
}

/* A button held. The bench answers both ways a core may read a gamepad from
 * this, so a case says what is pressed and not how it is fetched. */
static void press(unsigned port, unsigned id, bool down)
{
    fe.input[port][0][id] = down ? 1 : 0;
}

static void start_gamepad_program(int *utest_result)
{
    memset(fe.input, 0, sizeof fe.input);
    memset(fe.analog, 0, sizeof fe.analog);
    ASSERT_TRUE(fe_load(ROMS_DIR "/gamepad.rp6502"));
    fe_run(40);
}

/* A gamepad the frontend offers is a gamepad the machine has: the connected bit is
 * the gate a program reads before anything else. */
UTEST(input, a_port_becomes_a_player)
{
    start_gamepad_program(utest_result);
    const uint8_t *rec = gamepad_record(0);
    ASSERT_TRUE(rec != NULL);
    ASSERT_EQ(rec[0] & 0x80, 0x80);
    fe.unload_game();
}

/* Start lands in the byte the machine keeps it in — the same $08 in
 * button1 that tests/cpu/hid pins from the other side. */
UTEST(input, start_reaches_the_record)
{
    start_gamepad_program(utest_result);
    press(0, RETRO_DEVICE_ID_JOYPAD_START, true);
    fe_run(20);
    const uint8_t *rec = gamepad_record(0);
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
    start_gamepad_program(utest_result);
    press(0, RETRO_DEVICE_ID_JOYPAD_UP, true);
    press(0, RETRO_DEVICE_ID_JOYPAD_B, true);
    fe_run(20);
    const uint8_t *rec = gamepad_record(0);
    ASSERT_EQ(rec[0], 0xD1); /* connected gamepad, dpad up */
    ASSERT_EQ(rec[2], 0x01); /* button0 bit 0 is A */
    fe.unload_game();
}

/* The stick derives the digital sticks byte the way the firmware does. */
UTEST(input, a_stick_derives_its_digital_reading)
{
    start_gamepad_program(utest_result);
    fe.analog[0][RETRO_DEVICE_INDEX_ANALOG_LEFT][RETRO_DEVICE_ID_ANALOG_Y] = -0x7F00;
    fe_run(20);
    const uint8_t *rec = gamepad_record(0);
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
    start_gamepad_program(utest_result);
    press(0, RETRO_DEVICE_ID_JOYPAD_START, true);
    fe_run(20);
    ASSERT_EQ(gamepad_record(0)[0] & 0x80, 0x80);

    fe.set_controller_port_device(0, RETRO_DEVICE_NONE);
    fe_run(20);
    const uint8_t *rec = gamepad_record(0);
    for (int i = 0; i < GAMEPAD_RECORD; i++)
        ASSERT_EQ(rec[i], 0x00);

    fe.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    fe.unload_game();
}

/* Four ports, four players, each its own record. */
UTEST(input, the_second_port_is_the_second_player)
{
    start_gamepad_program(utest_result);
    press(1, RETRO_DEVICE_ID_JOYPAD_START, true);
    fe_run(20);
    ASSERT_EQ(gamepad_record(1)[3], 0x08);
    ASSERT_EQ(gamepad_record(0)[3], 0x00); /* player one pressed nothing */
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

/* Typing, all the way to a program reading the console.
 *
 * The case above proves a key reaches the HID bitmap, which is a program
 * polling for a keypress. This is the other kind of keyboard: adventure
 * reads cooked lines from the console, so what has to arrive is characters
 * and then an Enter that ends the line.
 *
 * The picture is the observable, and the terminal blinks its cursor, so
 * this measures rather than compares: an idle stretch says what the blink
 * costs, and the answer to a typed line has to be bigger than that by more
 * than a character cell's worth of pixels. */
static size_t pixels_differing(const uint32_t *a, const uint32_t *b, size_t n)
{
    size_t d = 0;
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            d++;
    return d;
}

UTEST(input, typing_reaches_a_program_reading_the_console)
{
    static uint32_t settled[640 * 480];
    memset(fe.input, 0, sizeof fe.input);
    ASSERT_TRUE(fe_load(ROMS_DIR "/adventure.rp6502"));
    fe_run(120); /* to the prompt */
    ASSERT_TRUE(fe.keyboard.callback != NULL);

    const size_t px = (size_t)fe.frame_w * fe.frame_h;

    /* What the terminal does on its own over the same stretch. */
    memcpy(settled, fe.frame_copy, px * sizeof(uint32_t));
    fe_run(70);
    size_t idle = pixels_differing(settled, fe.frame_copy, px);

    /* And what it does when a line is typed at it. */
    memcpy(settled, fe.frame_copy, px * sizeof(uint32_t));
    fe.keyboard.callback(true, RETROK_n, 'n', 0);
    fe.keyboard.callback(false, RETROK_n, 'n', 0);
    fe_run(10);
    fe.keyboard.callback(true, RETROK_RETURN, '\r', 0);
    fe.keyboard.callback(false, RETROK_RETURN, '\r', 0);
    fe_run(60);
    size_t typed = pixels_differing(settled, fe.frame_copy, px);

    fprintf(stderr, "  idle %zu px, typed %zu px\n", idle, typed);
    /* A whole answer, not a cursor: one character cell is 128 pixels and
     * the response to this line is measured in tens of thousands. */
    ASSERT_TRUE(idle < 2000);
    ASSERT_TRUE(typed > 5000);
    fe.unload_game();
}

/* Four ports is what a frontend offers, not what a player has. RetroArch
 * reports every port as a joypad whether or not anything is plugged into
 * it, so a machine that connected all four would show four players to a
 * program counting them. */
UTEST(input, only_the_players_the_frontend_has)
{
    fe.max_users = 2;
    start_gamepad_program(utest_result);
    ASSERT_EQ(gamepad_record(0)[0] & 0x80, 0x80);
    ASSERT_EQ(gamepad_record(1)[0] & 0x80, 0x80);
    ASSERT_EQ(gamepad_record(2)[0], 0x00);
    ASSERT_EQ(gamepad_record(3)[0], 0x00);

    /* A controller plugged in while the program runs arrives. */
    fe.max_users = 3;
    fe_run(20);
    ASSERT_EQ(gamepad_record(2)[0] & 0x80, 0x80);

    /* And one unplugged leaves. */
    fe.max_users = 1;
    fe_run(20);
    for (int i = 0; i < GAMEPAD_RECORD; i++)
        ASSERT_EQ(gamepad_record(1)[i], 0x00);

    fe.max_users = -1;
    fe.unload_game();
}

/* A frontend that will not say how many players it has gets the machine's
 * four, which is where this started and is still the only honest answer to
 * a question nobody will answer. */
UTEST(input, a_silent_frontend_gets_all_four)
{
    fe.max_users = -1;
    start_gamepad_program(utest_result);
    for (int p = 0; p < 4; p++)
        ASSERT_EQ_MSG(gamepad_record(p)[0] & 0x80, 0x80, "every port connected");
    fe.unload_game();
}

/* The keyboard is bound to the frontend's own gamepad and hotkeys until a player
 * turns that off, so a computer's keyboard looks broken on first launch. The
 * core cannot turn it off and has no way to ask, so it says so — once, on
 * the first program of a session, not once per program. */
UTEST(input, the_core_says_how_to_type_once)
{
    /* A session of its own: the hint is per-session and earlier cases in
     * this binary have already had theirs. */
    fe_close();
    fe_open();

    ASSERT_EQ(fe.message_count, 0); /* nothing before content */
    ASSERT_TRUE(fe_load(ROMS_DIR "/gamepad.rp6502"));
    ASSERT_EQ(fe.message_count, 1);
    ASSERT_TRUE(strstr(fe.message, "Game Focus") != NULL);

    /* A second program is not a second lecture. */
    fe.unload_game();
    ASSERT_TRUE(fe_load(ROMS_DIR "/mode2.rp6502"));
    ASSERT_EQ(fe.message_count, 1);
    fe.unload_game();
}

/* A frontend too old for the message interface still hears it, through the
 * call that counts in frames. */
UTEST(input, an_old_frontend_is_told_the_old_way)
{
    fe_close();
    fe_open_as(2, true);
    fe.message_version = 0; /* only SET_MESSAGE */

    ASSERT_TRUE(fe_load(ROMS_DIR "/gamepad.rp6502"));
    ASSERT_EQ(fe.message_count, 1);
    ASSERT_TRUE(strstr(fe.message, "Game Focus") != NULL);
    fe.unload_game();

    fe_close();
    fe_open(); /* leave the fixture as the other cases expect it */
}

/* A frontend's remapper and its on-screen gamepad have nothing to print unless
 * the core says what its buttons do. The ports are declared when the core
 * is asked what it is; the button labels when content loads, which is the
 * first moment a frontend has a remapper to fill in. */
UTEST(input, the_buttons_are_labelled_for_the_frontend)
{
    ASSERT_TRUE(fe.controller_info_set);
    ASSERT_TRUE(fe_load(ROMS_DIR "/gamepad.rp6502"));
    ASSERT_TRUE(fe.input_descriptors_set);
    fe.unload_game();
}
