/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The pointer's events: the tablet's and the mouse's (core/sys/event.h). */

#include "core/sys/event.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/sys/sst.h"
#include "machine.h"

/* Whether the host shares its pointer between the tablet and the mouse,
 * which a host says once, as it starts. */
static bool event_shared;

#define EVENT_POINTER_SST(X) \
    X(SKIP, event_shared, "how the host runs, which it says as it starts")
SST_TABLE(event_pointer, EVENT_POINTER_SST)

void event_set_shared(bool shared)
{
    event_shared = shared;
}

/* Whether the tablet has the host's pointer. */
static bool event_to_tablet(void)
{
    if (event_shared)
        return tablet_wanted();
    return tablet_is_mapped() || (tablet_wanted() && !mouse_is_mapped());
}

/* Because nothing is captured while the tablet has the pointer, a mouse the
 * program mapped is fed from it as well, where the host captures: the one
 * physical pointer drives both blocks, as it would on hardware. */
void HOST_IN_RAM_FUNC event_pointer(const event_pointer_t *p)
{
    if (!event_to_tablet())
        return;
    if (p->left || !p->inside)
        tablet_host_clear();
    else
        tablet_host_pointer(p->x, p->y, p->buttons, p->host_cursor);
    if (event_shared || p->left || !mouse_is_mapped())
        return;
    mouse_host_buttons(p->buttons);
    if (p->moved)
        mouse_host_move(p->dx, p->dy);
}

void HOST_IN_RAM_FUNC event_touch(const event_touch_t *t)
{
    tablet_point_t pts[EVENT_TOUCHES];
    for (int i = 0; i < t->n; i++)
        pts[i] = (tablet_point_t){.x = t->at[i].x, .y = t->at[i].y};
    tablet_host_touch(pts, t->n);
}

void HOST_IN_RAM_FUNC event_wheel(const event_wheel_t *w)
{
    if (w->tablet)
        tablet_host_wheel(w->wheel, w->pan);
    if (!w->tablet || mouse_is_mapped())
        mouse_host_wheel(w->wheel, w->pan);
}

void HOST_IN_RAM_FUNC event_mouse(const event_mouse_t *m)
{
    if (m->moved)
        mouse_host_move(m->dx, m->dy);
    else
        mouse_host_buttons(m->buttons);
}

bool event_has_pointer(void)
{
    return event_to_tablet();
}
