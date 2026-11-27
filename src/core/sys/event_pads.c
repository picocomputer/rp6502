/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The controllers' events (core/sys/event.h). */

#include "core/sys/event.h"
#include "core/hid/gamepad.h"
#include "core/sys/sst.h"
#include "machine.h"
#include <string.h>

/* Which host controller each player is, 0 for none. Keyed on the backend's
 * id rather than its position, so unplugging one controller does not
 * renumber the remaining players' slots. */
static uint64_t event_player[GAMEPAD_PLAYERS];
static bool event_pads_mapped;

#define EVENT_PADS_SST(X)                                                  \
    X(SKIP, event_player, "the host's controllers as the host has them")   \
    X(SKIP, event_pads_mapped, "read again from the block at every status")
SST_TABLE(event_pads, EVENT_PADS_SST)

/* Every player blanked. */
static void event_unplug(void)
{
    for (int player = 0; player < GAMEPAD_PLAYERS; player++)
        if (event_player[player])
        {
            gamepad_connect(player, false, GAMEPAD_TYPE_UNKNOWN, false);
            event_player[player] = 0;
        }
}

void HOST_IN_RAM_FUNC event_gamepads(const event_gamepads_t *g)
{
    if (g->fixed)
    {
        /* Each port is its player, and an empty one is no player. */
        for (int player = 0; player < GAMEPAD_PLAYERS; player++)
        {
            const gamepad_host_t *pad = &g->pad[player];
            const uint64_t id = g->opened ? pad->id : 0;
            const bool had = event_player[player] != 0;
            event_player[player] = id;
            if (!id)
            {
                if (had)
                    gamepad_connect(player, false, GAMEPAD_TYPE_UNKNOWN, false);
                continue;
            }
            gamepad_connect(player, true, pad->type, pad->sticks);
            gamepad_host_report(player, pad->dpad, pad->button0, pad->button1,
                                pad->lx, pad->ly, pad->rx, pad->ry, pad->lt, pad->rt);
        }
        return;
    }
    if (!g->opened || !gamepad_is_mapped())
    {
        event_unplug();
        return;
    }
    for (int player = 0; player < GAMEPAD_PLAYERS; player++)
    {
        if (!event_player[player])
            continue;
        bool still_here = false;
        for (int i = 0; i < g->n; i++)
            if (g->pad[i].id == event_player[player])
                still_here = true;
        if (!still_here)
        {
            gamepad_connect(player, false, GAMEPAD_TYPE_UNKNOWN, false);
            event_player[player] = 0;
        }
    }
    for (int i = 0; i < g->n; i++)
    {
        const gamepad_host_t *pad = &g->pad[i];
        if (!pad->id)
            continue;
        int player = -1;
        for (int p = 0; p < GAMEPAD_PLAYERS; p++)
            if (event_player[p] == pad->id)
                player = p;
        if (player < 0)
            for (int p = 0; p < GAMEPAD_PLAYERS && player < 0; p++)
                if (!event_player[p])
                {
                    event_player[p] = pad->id;
                    player = p;
                }
        if (player < 0)
            continue; /* more controllers than the machine has players */
        /* A backend may become sure of the face-button labels only after a
         * poll or two, so what it claims is restated with every report. */
        gamepad_connect(player, true, pad->type, pad->sticks);
        gamepad_host_report(player, pad->dpad, pad->button0, pad->button1,
                            pad->lx, pad->ly, pad->rx, pad->ry, pad->lt, pad->rt);
    }
}

/* A state load brought back the players the saved machine had, so every
 * player is told again, or unplugged, by the host's next controllers. */
void event_state_loaded(void)
{
    for (int player = 0; player < GAMEPAD_PLAYERS; player++)
        event_player[player] = UINT64_MAX;
}

void event_pads_watch(bool mapped)
{
    if (event_pads_mapped && !mapped)
        event_unplug();
    event_pads_mapped = mapped;
}

void event_pads_reset(void)
{
    memset(event_player, 0, sizeof event_player);
    event_pads_mapped = false;
}
