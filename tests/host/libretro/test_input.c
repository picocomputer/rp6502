/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

#define GAMEPAD_XREG 0xFF00 /* gamepad.rp6502 maps the gamepad block here */
#define GAMEPAD_RECORD 10

static const uint8_t *gamepad_record(int player)
{
    const uint8_t *xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    return xram ? xram + GAMEPAD_XREG + player * GAMEPAD_RECORD : NULL;
}

static void press(unsigned port, unsigned id, bool down)
{
    fe.input[port][0][id] = down ? 1 : 0;
}

static void start_gamepad_program(int *utest_result)
{
    memset(fe.input, 0, sizeof fe.input);
    memset(fe.analog, 0, sizeof fe.analog);
    ASSERT_TRUE(fe_load(GAMEPAD_ROM));
    fe_run(40);
}

UTEST(input, a_port_becomes_a_player)
{
    start_gamepad_program(utest_result);
    const uint8_t *rec = gamepad_record(0);
    ASSERT_TRUE(rec != NULL);
    ASSERT_EQ(rec[0] & 0x80, 0x80);
    fe.unload_game();
}

/* Start is $08 in button1, the record's fourth byte. */
UTEST(input, start_reaches_the_record)
{
    start_gamepad_program(utest_result);
    press(0, RETRO_DEVICE_ID_JOYPAD_START, true);
    fe_run(20);
    const uint8_t *rec = gamepad_record(0);
    /* 0xC0 holds the connected and two-stick bits, which the core sets for
     * every RetroPad. */
    ASSERT_EQ(rec[0], 0xC0);
    ASSERT_EQ(rec[1], 0x00);
    ASSERT_EQ(rec[2], 0x00);
    ASSERT_EQ(rec[3], 0x08);
    fe.unload_game();
}

UTEST(input, the_south_button_is_the_machines_a)
{
    start_gamepad_program(utest_result);
    press(0, RETRO_DEVICE_ID_JOYPAD_UP, true);
    press(0, RETRO_DEVICE_ID_JOYPAD_B, true);
    fe_run(20);
    const uint8_t *rec = gamepad_record(0);
    ASSERT_EQ(rec[0], 0xC1); /* 0xC0 + dpad up */
    ASSERT_EQ(rec[2], 0x01); /* button0 bit 0 is A */
    fe.unload_game();
}

UTEST(input, a_stick_derives_its_digital_reading)
{
    start_gamepad_program(utest_result);
    fe.analog[0][RETRO_DEVICE_INDEX_ANALOG_LEFT][RETRO_DEVICE_ID_ANALOG_Y] = -0x7F00;
    fe_run(20);
    const uint8_t *rec = gamepad_record(0);
    ASSERT_EQ(rec[0], 0xC0);
    ASSERT_EQ(rec[1], 0x01); /* left stick up */
    ASSERT_EQ(rec[2], 0x00);
    ASSERT_EQ(rec[3], 0x00);
    fe.unload_game();
}

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

UTEST(input, the_second_port_is_the_second_player)
{
    start_gamepad_program(utest_result);
    press(1, RETRO_DEVICE_ID_JOYPAD_START, true);
    fe_run(20);
    ASSERT_EQ(gamepad_record(1)[3], 0x08);
    ASSERT_EQ(gamepad_record(0)[3], 0x00);
    fe.unload_game();
}

UTEST(input, the_core_asked_for_a_keyboard)
{
    ASSERT_TRUE(fe.keyboard_set);
    ASSERT_TRUE(fe.keyboard.callback != NULL);
}

/* keyboard.rp6502 polls the HID bitmap and exits once a key is pressed and
 * released, so fe.shutdown is set only after the program has read the key. */
UTEST(input, keys_reach_the_program_and_its_end_reaches_the_frontend)
{
    memset(fe.input, 0, sizeof fe.input);
    ASSERT_TRUE(fe_load(KEYBOARD_ROM));
    fe_run(20);
    ASSERT_TRUE(fe.keyboard.callback != NULL);
    ASSERT_FALSE(fe.shutdown);

    fe.keyboard.callback(true, RETROK_SPACE, ' ', 0);
    fe_run(5);
    fe.keyboard.callback(false, RETROK_SPACE, ' ', 0);
    fe_run(10);
    ASSERT_TRUE(fe.shutdown);
    fe.unload_game();
}

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
    ASSERT_TRUE(fe_load(ADVENTURE_ROM));
    fe_run(120);
    ASSERT_TRUE(fe.keyboard.callback != NULL);

    const size_t px = (size_t)fe.frame_w * fe.frame_h;

    memcpy(settled, fe.frame_copy, px * sizeof(uint32_t));
    fe_run(70);
    size_t idle = pixels_differing(settled, fe.frame_copy, px);

    memcpy(settled, fe.frame_copy, px * sizeof(uint32_t));
    fe.keyboard.callback(true, RETROK_n, 'n', 0);
    fe.keyboard.callback(false, RETROK_n, 'n', 0);
    fe_run(10);
    fe.keyboard.callback(true, RETROK_RETURN, '\r', 0);
    fe.keyboard.callback(false, RETROK_RETURN, '\r', 0);
    fe_run(60);
    size_t typed = pixels_differing(settled, fe.frame_copy, px);

    fprintf(stderr, "  idle %zu px, typed %zu px\n", idle, typed);
    ASSERT_TRUE(idle < 2000);
    ASSERT_TRUE(typed > 5000);
    fe.unload_game();
}

UTEST(input, only_the_players_the_frontend_has)
{
    fe.max_users = 2;
    start_gamepad_program(utest_result);
    ASSERT_EQ(gamepad_record(0)[0] & 0x80, 0x80);
    ASSERT_EQ(gamepad_record(1)[0] & 0x80, 0x80);
    ASSERT_EQ(gamepad_record(2)[0], 0x00);
    ASSERT_EQ(gamepad_record(3)[0], 0x00);

    fe.max_users = 3;
    fe_run(20);
    ASSERT_EQ(gamepad_record(2)[0] & 0x80, 0x80);

    fe.max_users = 1;
    fe_run(20);
    for (int i = 0; i < GAMEPAD_RECORD; i++)
        ASSERT_EQ(gamepad_record(1)[i], 0x00);

    fe.max_users = -1;
    fe.unload_game();
}

UTEST(input, a_silent_frontend_gets_all_four)
{
    fe.max_users = -1;
    start_gamepad_program(utest_result);
    for (int p = 0; p < 4; p++)
        ASSERT_EQ_MSG(gamepad_record(p)[0] & 0x80, 0x80, "every port connected");
    fe.unload_game();
}

UTEST(input, the_core_says_how_to_type_once)
{
    /* The hint is shown once per session and an earlier case may already
     * have shown it, so this case starts a new session. */
    fe_close();
    fe_open();

    ASSERT_EQ(fe.message_count, 0);
    ASSERT_TRUE(fe_load(ADVENTURE_ROM));
    ASSERT_EQ(fe.message_count, 0);
    fe_run(120);
    ASSERT_EQ(fe.message_count, 1);
    ASSERT_TRUE(strstr(fe.message, "Game Focus") != NULL);

    fe.unload_game();
    ASSERT_TRUE(fe_load(ADVENTURE_ROM));
    fe_run(120);
    ASSERT_EQ(fe.message_count, 1);
    fe.unload_game();
}

UTEST(input, a_program_that_never_asks_is_never_told)
{
    fe_close();
    fe_open();
    ASSERT_TRUE(fe_load(GAMEPAD_ROM));
    fe_run(120);
    ASSERT_EQ(fe.message_count, 0);
    fe.unload_game();
}

UTEST(input, a_program_that_wants_only_the_tablet_is_never_told)
{
    fe_close();
    fe_open();
    ASSERT_TRUE(fe_load(TABLET_ROM));
    fe_run(120);
    ASSERT_EQ(fe.message_count, 0);
    fe.unload_game();
}

UTEST(input, a_program_that_wants_the_mouse_is_told)
{
    fe_close();
    fe_open();
    ASSERT_TRUE(fe_load(MOUSE_ROM));
    fe_run(120);
    ASSERT_EQ(fe.message_count, 1);
    ASSERT_TRUE(strstr(fe.message, "Game Focus") != NULL);
    fe.unload_game();
}

UTEST(input, an_old_frontend_is_told_the_old_way)
{
    fe_close();
    fe_open_as(2, true);
    fe.message_version = 0;

    ASSERT_TRUE(fe_load(ADVENTURE_ROM));
    fe_run(120);
    ASSERT_EQ(fe.message_count, 1);
    ASSERT_TRUE(strstr(fe.message, "Game Focus") != NULL);
    fe.unload_game();

    fe_close();
    fe_open();
}

UTEST(input, the_buttons_are_labelled_for_the_frontend)
{
    ASSERT_TRUE(fe.controller_info_set);
    ASSERT_TRUE(fe_load(GAMEPAD_ROM));
    ASSERT_TRUE(fe.input_descriptors_set);
    fe.unload_game();
}

UTEST(input, a_shifted_symbol_still_types)
{
    static uint32_t settled[640 * 480];
    memset(fe.input, 0, sizeof fe.input);
    ASSERT_TRUE(fe_load(ADVENTURE_ROM));
    fe_run(120);
    ASSERT_TRUE(fe.keyboard.callback != NULL);

    const size_t px = (size_t)fe.frame_w * fe.frame_h;
    memcpy(settled, fe.frame_copy, px * sizeof(uint32_t));

    /* The composed character is 0, so the '?' can only come from the
     * keycode. */
    fe.keyboard.callback(true, RETROK_QUESTION, 0, 0);
    fe.keyboard.callback(false, RETROK_QUESTION, 0, 0);
    fe_run(30);
    ASSERT_TRUE(pixels_differing(settled, fe.frame_copy, px) > 0);
    fe.unload_game();
}
