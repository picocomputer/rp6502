/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/event.h"
#include "core/hid/gamepad.h"
#include "core/hid/hostkey.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/paste_host.h"
#include "core/hid/tablet.h"
#include "core/hid/vtkey.h"
#include "core/sys/proc.h"
#include "core/sys/sst.h"
#include "core/term/vterm_mouse.h"
#include "core/vga/mode/mode6.h"
#include <string.h>

static struct
{
    /* A key that typed with no character from the host: the host's own
     * character for it, which comes next, is not typed again. */
    bool suppress_char;
    /* A chord acted, until its key comes up. */
    bool chord_held;
    bool pads_mapped;
    /* Which host controller each player is, 0 for none. Keyed on the
     * backend's id rather than its position, so unplugging one controller
     * does not renumber the remaining players' slots. */
    uint64_t player[GAMEPAD_PLAYERS];
} event;

#define EVENT_SST(X) \
    X(SKIP, event, "the host's input as the host has it, like key.c's chord")
SST_TABLE(event, EVENT_SST)

/* ---------------- keys ---------------- */

void event_key(const event_key_t *k)
{
    /* The host repeats a held key and its character; the terminal says
     * whether they are typed (DECARM). */
    if (k->repeat && !vtkey_repeats())
        return;
    if (k->usage)
        keyboard_set_key(k->usage, k->down);
    if (!k->down)
    {
        event.chord_held = false;
        return;
    }
    event.suppress_char = false;
    if (!k->acts)
        return;
    const hostkey_result_t result = hostkey_press(k->usage, k->modifier, k->repeat, k->guess);
    if (result == HOSTKEY_CHORD)
        event.chord_held = true;
    /* A key that typed with no character from here must not type the
     * system's character too. */
    else if (result == HOSTKEY_TYPED && !k->guess)
        event.suppress_char = true;
}

void event_char(const event_char_t *c)
{
    if (c->repeat && !vtkey_repeats())
        return;
    if (event.suppress_char)
        event.suppress_char = false;
    else
        hostkey_char(c->codepoint);
}

/* ---------------- the pointer ---------------- */

/* Because nothing is captured while the tablet has the pointer, a mouse the
 * program mapped is fed from it as well: the one physical pointer drives
 * both blocks, as it would on hardware. */
void event_pointer(const event_pointer_t *p)
{
    if (p->left || !p->inside)
        tablet_host_clear();
    else
        tablet_host_pointer(p->x, p->y, p->buttons, true);
    if (p->left || !mouse_is_mapped())
        return;
    mouse_host_buttons(p->buttons);
    if (p->moved)
        mouse_host_move(p->dx, p->dy);
}

void event_touch(const event_touch_t *t)
{
    tablet_point_t pts[EVENT_TOUCHES];
    for (int i = 0; i < t->n; i++)
        pts[i] = (tablet_point_t){.x = t->at[i].x, .y = t->at[i].y};
    tablet_host_touch(pts, t->n);
}

void event_wheel(const event_wheel_t *w)
{
    if (w->tablet)
        tablet_host_wheel(w->wheel, w->pan);
    if (!w->tablet || mouse_is_mapped())
        mouse_host_wheel(w->wheel, w->pan);
}

void event_mouse(const event_mouse_t *m)
{
    if (m->moved)
        mouse_host_move(m->dx, m->dy);
    else
        mouse_host_buttons(m->buttons);
}

void event_locks(uint8_t locks)
{
    keyboard_set_locks(locks);
}

void event_focus(bool focused)
{
    vterm_focus(focused);
}

void event_paste(const char *utf8)
{
    paste_start(utf8);
}

bool event_chord_held(void)
{
    return event.chord_held;
}

/* ---------------- controllers ---------------- */

/* Every player blanked. */
static void event_unplug(void)
{
    for (int player = 0; player < GAMEPAD_PLAYERS; player++)
        if (event.player[player])
        {
            gamepad_connect(player, false, GAMEPAD_TYPE_UNKNOWN, false);
            event.player[player] = 0;
        }
}

void event_gamepads(const event_gamepads_t *g)
{
    if (!g->opened || !gamepad_is_mapped())
    {
        event_unplug();
        return;
    }
    for (int player = 0; player < GAMEPAD_PLAYERS; player++)
    {
        if (!event.player[player])
            continue;
        bool still_here = false;
        for (int i = 0; i < g->n; i++)
            if (g->pad[i].id == event.player[player])
                still_here = true;
        if (!still_here)
        {
            gamepad_connect(player, false, GAMEPAD_TYPE_UNKNOWN, false);
            event.player[player] = 0;
        }
    }
    for (int i = 0; i < g->n; i++)
    {
        const gamepad_host_t *pad = &g->pad[i];
        if (!pad->id)
            continue;
        int player = -1;
        for (int p = 0; p < GAMEPAD_PLAYERS; p++)
            if (event.player[p] == pad->id)
                player = p;
        if (player < 0)
            for (int p = 0; p < GAMEPAD_PLAYERS && player < 0; p++)
                if (!event.player[p])
                {
                    event.player[p] = pad->id;
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

/* ---------------- the status ---------------- */

event_status_t event_status(void)
{
    const bool mouse = mouse_is_mapped();
    const bool to_tablet = tablet_is_mapped() || (tablet_wanted() && !mouse);
    tablet_host_give(to_tablet);
    const bool pads = gamepad_is_mapped();
    if (event.pads_mapped && !pads)
        event_unplug();
    event.pads_mapped = pads;
    /* The VGA draws the program's own pointers, and a shape the system has
     * none of is no pointer. */
    uint8_t cursor = tablet_shape();
    if (cursor >= TABLET_CURSOR_COUNT || mode6_custom())
        cursor = TABLET_CURSOR_OFF;
    return (event_status_t){
        .flags = (uint8_t)((mouse ? EVENT_MOUSE_MAPPED : 0) |
                           (to_tablet ? EVENT_TO_TABLET : 0) |
                           (to_tablet && tablet_wheel_wanted() ? EVENT_WHEEL_TO_TABLET : 0) |
                           (proc_exited() ? EVENT_EXITED : 0) |
                           (pads ? EVENT_PADS_MAPPED : 0)),
        .cursor = cursor,
    };
}

void event_reset(void)
{
    memset(&event, 0, sizeof event);
}
