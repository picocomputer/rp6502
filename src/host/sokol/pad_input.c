/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/pad_input.h"
#include <string.h>

/* Frames between attempts to open the host's controllers. The web shell
 * watches its own gate every 250ms for the same reason: a program that has
 * mapped the block but plugged nothing in should not have us scanning the
 * host's devices every frame. */
#define PAD_INPUT_RETRY 15

static bool pad_input_opened;
static int pad_input_retry;

/* Which host controller each player is, 0 for none. Keyed on the backend's id
 * rather than its position so that unplugging player one leaves player two
 * where they were sitting. */
static uint64_t pad_input_player[PAD_PLAYERS];

static void pad_input_release(void)
{
    if (pad_input_opened)
    {
        host_pad_close();
        pad_input_opened = false;
    }
    for (int player = 0; player < PAD_PLAYERS; player++)
        if (pad_input_player[player])
        {
            pad_connect(player, false, PAD_TYPE_UNKNOWN, false);
            pad_input_player[player] = 0;
        }
    pad_input_retry = 0;
}

void pad_input_stop(void)
{
    pad_input_release();
}

void pad_input_task(void)
{
    /* Nothing reads a controller until a program asks for one, and the moment
     * it stops asking we let go of them again. */
    if (!pad_is_mapped())
    {
        if (pad_input_opened)
            pad_input_release();
        return;
    }

    if (!pad_input_opened)
    {
        if (pad_input_retry > 0)
        {
            pad_input_retry--;
            return;
        }
        pad_input_retry = PAD_INPUT_RETRY;
        if (!host_pad_open())
            return;
        pad_input_opened = true;
    }

    pad_host_t pads[PAD_PLAYERS];
    int count = host_pad_poll(pads, PAD_PLAYERS);

    /* Whoever left. Their record blanks; everyone else keeps their number. */
    for (int player = 0; player < PAD_PLAYERS; player++)
    {
        if (!pad_input_player[player])
            continue;
        bool still_here = false;
        for (int i = 0; i < count; i++)
            if (pads[i].id == pad_input_player[player])
                still_here = true;
        if (!still_here)
        {
            pad_connect(player, false, PAD_TYPE_UNKNOWN, false);
            pad_input_player[player] = 0;
        }
    }

    for (int i = 0; i < count; i++)
    {
        const pad_host_t *pad = &pads[i];
        if (!pad->id)
            continue;
        int player = -1;
        for (int p = 0; p < PAD_PLAYERS; p++)
            if (pad_input_player[p] == pad->id)
                player = p;
        if (player < 0)
            for (int p = 0; p < PAD_PLAYERS && player < 0; p++)
                if (!pad_input_player[p])
                {
                    pad_input_player[p] = pad->id;
                    player = p;
                }
        if (player < 0)
            continue; /* more controllers than the machine has players */
        /* A backend may only be sure of the labels after a poll or two,
         * so what it claims is restated with every report. */
        pad_connect(player, true, pad->type, pad->sticks);
        pad_host_report(player, pad->dpad, pad->button0, pad->button1,
                        pad->lx, pad->ly, pad->rx, pad->ry, pad->lt, pad->rt);
    }
}
