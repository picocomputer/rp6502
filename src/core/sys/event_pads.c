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

/* The player a controller is, or -1. 0 finds the first player with none. */
static int HOST_IN_RAM_FUNC event_find(uint64_t id)
{
    for (int player = 0; player < GAMEPAD_PLAYERS; player++)
        if (event_player[player] == id)
            return player;
    return -1;
}

int HOST_IN_RAM_FUNC event_gamepad_player(uint64_t id)
{
    return id ? event_find(id) : -1;
}

/* A controller the machine has not seen is the first player with none,
 * and keeps that player until it goes. A backend may become sure of the
 * face-button labels only after a poll or two, so what it claims is restated
 * with every report. */
void HOST_IN_RAM_FUNC event_gamepad(const gamepad_host_t *pad)
{
    int player = event_find(pad->id);
    if (player < 0)
    {
        player = event_find(0);
        if (player < 0)
            return; /* more controllers than the machine has players */
        event_player[player] = pad->id;
    }
    gamepad_connect(player, true, pad->type, pad->sticks);
    gamepad_host_report(player, pad->dpad, pad->button0, pad->button1,
                        pad->lx, pad->ly, pad->rx, pad->ry, pad->lt, pad->rt);
}

void event_gamepad_gone(uint64_t id)
{
    const int player = event_gamepad_player(id);
    if (player < 0)
        return;
    gamepad_connect(player, false, GAMEPAD_TYPE_UNKNOWN, false);
    event_player[player] = 0;
}

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
            event_gamepad_gone(event_player[player]);
    }
    for (int i = 0; i < g->n; i++)
        if (g->pad[i].id)
            event_gamepad(&g->pad[i]);
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
