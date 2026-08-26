/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The desktop gamepad policy, against a host that does as it is told.
 *
 * Reading a real controller is three files CI has no hardware for, so what
 * is checked here is everything above that seam: that nothing is opened
 * until a program asks and everything is let go when it stops asking, and
 * that a player keeps their number while they stay plugged in. The last one
 * is the reason the seam reports an id at all — renumbering the survivors
 * when someone else's battery dies is the bug this is here to prevent.
 */

#include "host/sokol/gamepad_input.h"
#include "core/sys/main.h"
#include "core/mem/mem.h"

#include "utest.h"

#include <string.h>

#define AT_PAD 0xFF00

/* The fake host. */
static bool fake_open_result = true;
static int fake_opens, fake_closes;
static bool fake_is_open;
static int fake_polls_while_closed; /* the gate leaking would show up here */
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

/* Enough frames for the retry counter between open attempts to run out. */
static void run_frames(int frames)
{
    for (int i = 0; i < frames; i++)
        gamepad_input_task();
}

/* The web shell's contract, kept on the desktop: no device is touched until a
 * program maps the block, and all of them are released when it stops. */
UTEST(gamepad_input, nothing_opens_until_a_program_asks)
{
    fake_reset();

    run_frames(120);
    ASSERT_EQ(fake_opens, 0);
    ASSERT_EQ(fake_polls_while_closed, 0);

    ASSERT_TRUE(main_xreg_0(0, 2, AT_PAD)); /* xreg_ria_gamepad */
    fake_plug(0, 0x11);
    run_frames(1);
    ASSERT_EQ(fake_opens, 1);
    ASSERT_EQ(rec(0)[0] & 0x80, 0x80);

    /* Released: the devices go. The bytes stay as they last were, because the
     * program has that memory back and blanking it would be writing to it. */
    ASSERT_TRUE(main_xreg_0(0, 2, 0xFFFF));
    run_frames(1);
    ASSERT_EQ(fake_closes, 1);

    run_frames(120);
    ASSERT_EQ(fake_opens, 1);
    ASSERT_EQ(fake_polls_while_closed, 0);

    /* And asking again opens again. */
    ASSERT_TRUE(main_xreg_0(0, 2, AT_PAD));
    run_frames(1);
    ASSERT_EQ(fake_opens, 2);
}

/* A host with nothing plugged in is ordinary, and is not asked again every
 * frame — a scan of the host's input devices is not a per-frame cost. */
UTEST(gamepad_input, a_host_that_cannot_open_is_retried_slowly)
{
    fake_reset();
    fake_open_result = false;
    ASSERT_TRUE(main_xreg_0(0, 2, AT_PAD));

    run_frames(1);
    ASSERT_EQ(fake_opens, 1);
    run_frames(10);
    ASSERT_EQ(fake_opens, 1); /* still waiting */
    run_frames(60);
    ASSERT_GT(fake_opens, 1);
    ASSERT_LT(fake_opens, 10); /* but not once a frame */
}

/* Player two stays player two when player one's battery dies. */
UTEST(gamepad_input, players_keep_their_number)
{
    fake_reset();
    ASSERT_TRUE(main_xreg_0(0, 2, AT_PAD));

    fake_plug(0, 0xAA);
    fake_plug(1, 0xBB);
    fake_gamepads[1].button0 = 0x10; /* Y, so player two is identifiable */
    run_frames(1);
    ASSERT_EQ(rec(0)[0] & 0x80, 0x80);
    ASSERT_EQ(rec(1)[2], 0x10);

    /* The first one leaves. The list closes up, but the players do not. */
    fake_gamepads[0] = fake_gamepads[1];
    fake_count = 1;
    run_frames(1);
    ASSERT_EQ(rec(0)[0], 0x00); /* player one unplugged */
    ASSERT_EQ(rec(1)[2], 0x10); /* player two untouched */

    /* A new controller takes the free slot rather than shuffling anyone. */
    fake_plug(1, 0xCC);
    fake_gamepads[1].button0 = 0x01;
    run_frames(1);
    ASSERT_EQ(rec(0)[2], 0x01);
    ASSERT_EQ(rec(1)[2], 0x10);
}

/* What a backend claims is what a program reads. */
UTEST(gamepad_input, the_claim_reaches_xram)
{
    fake_reset();
    ASSERT_TRUE(main_xreg_0(0, 2, AT_PAD));

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

/* More controllers than the machine has players: the extras are ignored, and
 * nobody already playing is disturbed by them. */
UTEST(gamepad_input, a_fifth_controller_is_ignored)
{
    fake_reset();
    ASSERT_TRUE(main_xreg_0(0, 2, AT_PAD));

    for (int i = 0; i < GAMEPAD_PLAYERS; i++)
    {
        fake_plug(i, 0x100 + (uint64_t)i);
        fake_gamepads[i].button0 = (uint8_t)(1u << i);
    }
    run_frames(1);
    for (int i = 0; i < GAMEPAD_PLAYERS; i++)
        ASSERT_EQ(rec(i)[2], (uint8_t)(1u << i));

    /* host_gamepad_poll is asked for at most GAMEPAD_PLAYERS, so a fifth never even
     * reaches us — but the policy must survive being handed one anyway. */
    fake_count = GAMEPAD_PLAYERS;
    run_frames(1);
    for (int i = 0; i < GAMEPAD_PLAYERS; i++)
        ASSERT_EQ(rec(i)[2], (uint8_t)(1u << i));
}

UTEST_MAIN()
