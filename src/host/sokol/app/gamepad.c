/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/hid/gamepad.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/gamepad.h"
#include <string.h>

/* Frames between attempts to open the host's controllers. The web shell
 * watches its own gate every 250ms for the same reason: a program that has
 * mapped the block but plugged nothing in should not have us scanning the
 * host's devices every frame. */
#define GAMEPAD_INPUT_RETRY 15

static bool gamepad_input_opened;
static int gamepad_input_retry;

/* Which host controller each player is, 0 for none. Keyed on the backend's id
 * rather than its position so that unplugging player one leaves player two
 * where they were sitting. */
static uint64_t gamepad_input_player[GAMEPAD_PLAYERS];

static void gamepad_input_release(void)
{
    if (gamepad_input_opened)
    {
        host_gamepad_close();
        gamepad_input_opened = false;
    }
    for (int player = 0; player < GAMEPAD_PLAYERS; player++)
        if (gamepad_input_player[player])
        {
            gamepad_connect(player, false, GAMEPAD_TYPE_UNKNOWN, false);
            gamepad_input_player[player] = 0;
        }
    gamepad_input_retry = 0;
}

void gamepad_input_stop(void)
{
    gamepad_input_release();
}

void gamepad_input_task(void)
{
    /* Nothing reads a controller until a program asks for one, and the moment
     * it stops asking we let go of them again. */
    if (!gamepad_is_mapped())
    {
        if (gamepad_input_opened)
            gamepad_input_release();
        return;
    }

    if (!gamepad_input_opened)
    {
        if (gamepad_input_retry > 0)
        {
            gamepad_input_retry--;
            return;
        }
        gamepad_input_retry = GAMEPAD_INPUT_RETRY;
        if (!host_gamepad_open())
            return;
        gamepad_input_opened = true;
    }

    gamepad_host_t gamepads[GAMEPAD_PLAYERS];
    int count = host_gamepad_poll(gamepads, GAMEPAD_PLAYERS);

    /* Whoever left. Their record blanks; everyone else keeps their number. */
    for (int player = 0; player < GAMEPAD_PLAYERS; player++)
    {
        if (!gamepad_input_player[player])
            continue;
        bool still_here = false;
        for (int i = 0; i < count; i++)
            if (gamepads[i].id == gamepad_input_player[player])
                still_here = true;
        if (!still_here)
        {
            gamepad_connect(player, false, GAMEPAD_TYPE_UNKNOWN, false);
            gamepad_input_player[player] = 0;
        }
    }

    for (int i = 0; i < count; i++)
    {
        const gamepad_host_t *gamepad = &gamepads[i];
        if (!gamepad->id)
            continue;
        int player = -1;
        for (int p = 0; p < GAMEPAD_PLAYERS; p++)
            if (gamepad_input_player[p] == gamepad->id)
                player = p;
        if (player < 0)
            for (int p = 0; p < GAMEPAD_PLAYERS && player < 0; p++)
                if (!gamepad_input_player[p])
                {
                    gamepad_input_player[p] = gamepad->id;
                    player = p;
                }
        if (player < 0)
            continue; /* more controllers than the machine has players */
        /* A backend may only be sure of the labels after a poll or two,
         * so what it claims is restated with every report. */
        gamepad_connect(player, true, gamepad->type, gamepad->sticks);
        gamepad_host_report(player, gamepad->dpad, gamepad->button0, gamepad->button1,
                            gamepad->lx, gamepad->ly, gamepad->rx, gamepad->ry, gamepad->lt, gamepad->rt);
    }
}
