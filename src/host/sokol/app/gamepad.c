/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/hid/gamepad.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/gamepad.h"
#include "host/sokol/app/input.h"
#include "host/sokol/app/seam.h"

/* Frames between attempts to open the host's controllers. The counter is
 * reloaded on the frame that attempts an open and decremented on each of the
 * next fifteen, so attempts are sixteen frames apart, about 267 ms at 60 Hz.
 * That is close to the 250 ms interval the web shell watches its own mapping
 * on. A program that has mapped the block with nothing plugged in should not
 * have the emulator scanning the host's devices every frame. */
#define GAMEPAD_INPUT_RETRY 15

static bool gamepad_input_opened;
static int gamepad_input_retry;

void gamepad_input_stop(void)
{
    if (gamepad_input_opened)
    {
        host_gamepad_close();
        gamepad_input_opened = false;
    }
    gamepad_input_retry = 0;
}

void gamepad_input_poll(void)
{
    if (!(input_status()->flags & SEAM_PADS_MAPPED))
    {
        gamepad_input_stop();
        return;
    }
    seam_gamepads_t g = {0};
    if (!gamepad_input_opened && gamepad_input_retry > 0)
        gamepad_input_retry--;
    else if (!gamepad_input_opened)
    {
        gamepad_input_retry = GAMEPAD_INPUT_RETRY;
        gamepad_input_opened = host_gamepad_open();
    }
    if (gamepad_input_opened)
    {
        g.opened = true;
        g.n = (uint8_t)host_gamepad_poll(g.pad, GAMEPAD_PLAYERS);
    }
    seam_to_machine(SEAM_GAMEPADS, &g, sizeof g);
}
