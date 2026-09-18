/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/xreg.h"
#include "core/hid/gamepad.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/gamepad.h"
#include "core/sys/driver.h"
#include "core/sys/xram.h"

#include "utest.h"

#include <string.h>

#define AT_PAD 0xFF00

static bool fake_open_result = true;
static int fake_opens, fake_closes;
static bool fake_is_open;
static int fake_polls_while_closed;
static gamepad_host_t fake_gamepads[GAMEPAD_PLAYERS];
static int fake_count;

bool host_gamepad_open(void)
{
    fake_opens++;
    fake_is_open = fake_open_result;
    return fake_open_result;
}

void host_gamepad_close(void)
{
    fake_closes++;
    fake_is_open = false;
}

int host_gamepad_poll(gamepad_host_t *gamepads, int max)
{
    if (!fake_is_open)
        fake_polls_while_closed++;
    int count = fake_count < max ? fake_count : max;
    memcpy(gamepads, fake_gamepads, (size_t)count * sizeof(gamepad_host_t));
    return count;
}

static void fake_reset(void)
{
    gamepad_input_stop();
    fake_open_result = true;
    fake_opens = fake_closes = 0;
    fake_is_open = false;
    fake_polls_while_closed = 0;
    fake_count = 0;
    memset(fake_gamepads, 0, sizeof(fake_gamepads));
    gamepad_stop();
    memset((uint8_t *)xram, 0, 0x10000);
}

static void fake_plug(int index, uint64_t id)
{
    memset(&fake_gamepads[index], 0, sizeof(fake_gamepads[index]));
    fake_gamepads[index].id = id;
    if (index >= fake_count)
        fake_count = index + 1;
}

static const uint8_t *rec(int player)
{
    return (uint8_t *)&xram[AT_PAD + player * 10];
}

static void run_frames(int frames)
{
    for (int i = 0; i < frames; i++)
        gamepad_input_task();
}

UTEST(gamepad_input, nothing_opens_until_a_program_asks)
{
    fake_reset();

    run_frames(120);
    ASSERT_EQ(fake_opens, 0);
    ASSERT_EQ(fake_polls_while_closed, 0);

    ASSERT_TRUE(xreg0(0, 2, AT_PAD)); /* xreg_ria_gamepad sets this register */
    fake_plug(0, 0x11);
    run_frames(1);
    ASSERT_EQ(fake_opens, 1);
    ASSERT_EQ(rec(0)[0] & 0x80, 0x80);

    ASSERT_TRUE(xreg0(0, 2, 0xFFFF));
    run_frames(1);
    ASSERT_EQ(fake_closes, 1);

    run_frames(120);
    ASSERT_EQ(fake_opens, 1);
    ASSERT_EQ(fake_polls_while_closed, 0);

    ASSERT_TRUE(xreg0(0, 2, AT_PAD));
    run_frames(1);
    ASSERT_EQ(fake_opens, 2);
}

UTEST(gamepad_input, a_host_that_cannot_open_is_retried_slowly)
{
    fake_reset();
    fake_open_result = false;
    ASSERT_TRUE(xreg0(0, 2, AT_PAD));

    run_frames(1);
    ASSERT_EQ(fake_opens, 1);
    run_frames(10);
    ASSERT_EQ(fake_opens, 1);
    run_frames(60);
    ASSERT_GT(fake_opens, 1);
    ASSERT_LT(fake_opens, 10);
}

UTEST(gamepad_input, players_keep_their_number)
{
    fake_reset();
    ASSERT_TRUE(xreg0(0, 2, AT_PAD));

    fake_plug(0, 0xAA);
    fake_plug(1, 0xBB);
    fake_gamepads[1].button0 = 0x10;
    run_frames(1);
    ASSERT_EQ(rec(0)[0] & 0x80, 0x80);
    ASSERT_EQ(rec(1)[2], 0x10);

    fake_gamepads[0] = fake_gamepads[1];
    fake_count = 1;
    run_frames(1);
    ASSERT_EQ(rec(0)[0], 0x00);
    ASSERT_EQ(rec(1)[2], 0x10);

    fake_plug(1, 0xCC);
    fake_gamepads[1].button0 = 0x01;
    run_frames(1);
    ASSERT_EQ(rec(0)[2], 0x01);
    ASSERT_EQ(rec(1)[2], 0x10);
}

UTEST(gamepad_input, the_claim_reaches_xram)
{
    fake_reset();
    ASSERT_TRUE(xreg0(0, 2, AT_PAD));

    fake_plug(0, 0x11);
    fake_gamepads[0].type = GAMEPAD_TYPE_PLAYSTATION;
    fake_gamepads[0].sticks = true;
    fake_gamepads[0].dpad = 0x04;
    fake_gamepads[0].lx = -100;
    run_frames(1);
    ASSERT_EQ(rec(0)[0], 0xF4); /* connected | sticks | playstation | left */
    ASSERT_EQ((int8_t)rec(0)[4], -100);

    fake_gamepads[0].type = GAMEPAD_TYPE_UNKNOWN;
    fake_gamepads[0].sticks = false;
    fake_gamepads[0].dpad = 0;
    run_frames(1);
    ASSERT_EQ(rec(0)[0], 0x80);
}

UTEST(gamepad_input, a_fifth_controller_is_ignored)
{
    fake_reset();
    ASSERT_TRUE(xreg0(0, 2, AT_PAD));

    for (int i = 0; i < GAMEPAD_PLAYERS; i++)
    {
        fake_plug(i, 0x100 + (uint64_t)i);
        fake_gamepads[i].button0 = (uint8_t)(1u << i);
    }
    run_frames(1);
    for (int i = 0; i < GAMEPAD_PLAYERS; i++)
        ASSERT_EQ(rec(i)[2], (uint8_t)(1u << i));

    fake_count = GAMEPAD_PLAYERS;
    run_frames(1);
    for (int i = 0; i < GAMEPAD_PLAYERS; i++)
        ASSERT_EQ(rec(i)[2], (uint8_t)(1u << i));
}

UTEST_MAIN()
