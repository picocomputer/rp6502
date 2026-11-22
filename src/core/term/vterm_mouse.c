/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/vterm_mouse.h"
#include "core/hid/tablet.h"
#include "core/term/vterm.h"
#include "machine.h"

vterm_mouse_queue_t vterm_mouse_queue;

/* Contact 0's flags as the task last took them, and whether the terminal
 * took the last change: decided at a press and held to the release, and
 * otherwise at every change. */
static uint8_t vterm_mouse_flags;
static bool vterm_mouse_acting;

/* The buttons, which make a press: hover alone is none. */
#define VTERM_MOUSE_BUTTONS (TABLET_FLAG_LEFT | TABLET_FLAG_RIGHT | TABLET_FLAG_MIDDLE | \
                             TABLET_FLAG_BTN4 | TABLET_FLAG_BTN5)

/* A contact is there while it hovers or presses; one with neither has gone,
 * as a pointer that left or a finger lifted. */
#define VTERM_MOUSE_PRESENT (VTERM_MOUSE_BUTTONS | TABLET_FLAG_HOVER)

/* The counters may be anything that holds no more than the queue. */
static bool vterm_mouse_sst_valid(sst_cursor_t *c)
{
    if (RING_LEN(&vterm_mouse_queue) > VTERM_MOUSE_QUEUE)
        return sst_refuse(c, "more mouse changes queued than the queue holds");
    return true;
}

#define VTERM_MOUSE_EVENT_ROWS(Y, e)                         \
    Y(I16, e.x) Y(I16, e.y) Y(U8, e.flags) Y(U8, e.wheel)    \
    Y(U8, e.pan) Y(BOOL, e.edge) Y(BOOL, e.moved) Y(BOOL, e.shared)
#define VTERM_MOUSE_SST(X)                                         \
    X(U32, vterm_mouse_queue.idx.wr)                               \
    X(U32, vterm_mouse_queue.idx.rd)                               \
    X(STRUCTS, vterm_mouse_queue.buf, VTERM_MOUSE_EVENT_ROWS)      \
    X(VALIDATE, vterm_mouse_sst_valid)                             \
    X(U8, vterm_mouse_flags)                                       \
    X(BOOL, vterm_mouse_acting)
SST_TABLE(vterm_mouse, VTERM_MOUSE_SST)

/* A failed load can leave the counters holding more than the queue. */
void vterm_mouse_reset(void)
{
    RING_RESET(&vterm_mouse_queue);
    vterm_mouse_flags = 0;
    vterm_mouse_acting = false;
}

bool tablet_claimed(void)
{
    return vterm_placement.placed;
}

/* The terminal has no scrollback, so the wheel does nothing here. */
bool tablet_wheel_claimed(void)
{
    return false;
}

/* A sum of two deltas, held at the ends of an int8_t. *fits is cleared when
 * it was held. */
static int8_t HOST_IN_RAM_FUNC vterm_mouse_add(int8_t a, int8_t b, bool *fits)
{
    const int sum = a + b;
    if (sum < INT8_MIN || sum > INT8_MAX)
    {
        *fits = false;
        return sum < 0 ? INT8_MIN : INT8_MAX;
    }
    return (int8_t)sum;
}

void HOST_IN_RAM_FUNC tablet_on_contact(uint8_t old_flags, uint8_t flags, int16_t x, int16_t y,
                                        int8_t dwheel, int8_t dpan, bool moved_by_user)
{
    const uint32_t len = RING_LEN(&vterm_mouse_queue);
    const bool edge = flags != old_flags;
    const bool shared = tablet_shared();
    if (len)
    {
        vterm_mouse_event_t *e = RING_AT(&vterm_mouse_queue, len - 1);
        const bool full = len == VTERM_MOUSE_QUEUE;
        if (full || (!edge && !e->edge))
        {
            /* A merge that does not fit takes a new change, or with a full
             * queue holds the counters at their ends, so the direction is
             * kept. */
            bool fits = true;
            const int8_t wheel = vterm_mouse_add(e->wheel, dwheel, &fits);
            const int8_t pan = vterm_mouse_add(e->pan, dpan, &fits);
            if (fits || full)
            {
                e->x = x;
                e->y = y;
                e->flags = flags;
                e->wheel = wheel;
                e->pan = pan;
                e->edge |= edge;
                e->moved |= moved_by_user;
                e->shared = shared;
                return;
            }
        }
    }
    const vterm_mouse_event_t e = {
        .x = x,
        .y = y,
        .flags = flags,
        .wheel = dwheel,
        .pan = dpan,
        .edge = edge,
        .moved = moved_by_user,
        .shared = shared,
    };
    RING_PUT(&vterm_mouse_queue, e);
}

/* Mode 0's cells: its rows down from begin, and the columns across from the
 * canvas's left edge, 8 pixels each. Nothing is over them while mode 0 is
 * not placed. */
static bool vterm_mouse_over(int x, int y)
{
    const vterm_placement_t *p = &vterm_placement;
    return p->placed && x >= 0 && x < p->cols * 8 &&
           y >= p->begin && y < p->begin + p->rows * p->row_px;
}

/* Whether the terminal takes a change that no held button has decided: the
 * contact is there, the program shared the tablet as the change came, and
 * the contact is over mode 0's cells. */
static bool vterm_mouse_takes(const vterm_mouse_event_t *e)
{
    return (e->flags & VTERM_MOUSE_PRESENT) && e->shared && vterm_mouse_over(e->x, e->y);
}

bool vterm_mouse_acts(void)
{
    return vterm_mouse_acting;
}

void vterm_mouse_task(void)
{
    vterm_mouse_event_t e;
    while (RING_GET(&vterm_mouse_queue, &e))
    {
        /* With no button held, this change decides, a press included; a
         * release goes where its press went. */
        if (!(vterm_mouse_flags & VTERM_MOUSE_BUTTONS))
            vterm_mouse_acting = vterm_mouse_takes(&e);
        vterm_mouse_flags = e.flags;
    }
}
