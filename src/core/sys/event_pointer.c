/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The pointer's events: the host's tablet and its mouse (core/sys/event.h).
 * The machine's tablet takes both, and the mouse's block the mouse. */

#include "core/sys/event.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "machine.h"

void HOST_IN_RAM_FUNC event_tablet(const event_tablet_t *t)
{
    tablet_host_contacts(t->at, t->n, t->host_cursor);
}

void HOST_IN_RAM_FUNC event_wheel(const event_wheel_t *w)
{
    tablet_host_wheel(w->wheel, w->pan);
}

void HOST_IN_RAM_FUNC event_mouse(const event_mouse_t *m)
{
    if (m->gone)
    {
        mouse_host_buttons(0);
        tablet_host_mouse_gone();
        return;
    }
    mouse_host_move(m->dx, m->dy);
    mouse_host_buttons(m->buttons);
    mouse_host_wheel(m->wheel, m->pan);
    tablet_host_mouse(m->dx, m->dy, m->buttons);
    tablet_host_wheel(m->wheel, m->pan);
}
