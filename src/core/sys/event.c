/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The keyboard's events (core/sys/event.h). */

#include "core/sys/event.h"
#include "core/hid/hostkey.h"
#include "core/hid/keyboard.h"
#include "core/hid/vtkey.h"
#include "core/sys/sst.h"
#include "machine.h"
#include <string.h>

static struct
{
    /* A key that typed with no character from the host: the host's own
     * character for it, which comes next, is not typed again. */
    bool suppress_char;
    /* A chord acted, until its key comes up. */
    bool chord_held;
} event;

#define EVENT_SST(X) \
    X(SKIP, event, "the host's keys as the host has them, like key.c's chord")
SST_TABLE(event, EVENT_SST)

void HOST_IN_RAM_FUNC event_key(const event_key_t *k)
{
    /* The host repeats a held key and its character; the terminal says
     * whether they are typed (DECARM). */
    if (k->repeat && !vtkey_repeats())
        return;
    if (k->usage && !k->own_bitmap)
        keyboard_set_key(k->usage, k->down);
    if (!k->down)
    {
        event.chord_held = false;
        return;
    }
    event.suppress_char = false;
    if (!k->acts)
        return;
    const hostkey_result_t result = hostkey_type(k->usage, k->modifier, k->repeat, k->ch, k->meta);
    if (result == HOSTKEY_CHORD)
        event.chord_held = true;
    /* A key that typed with no character from here must not type the
     * system's character too. */
    else if (result == HOSTKEY_TYPED && !k->ch)
        event.suppress_char = true;
}

void HOST_IN_RAM_FUNC event_char(const event_char_t *c)
{
    if (c->repeat && !vtkey_repeats())
        return;
    if (event.suppress_char)
        event.suppress_char = false;
    else
        hostkey_char(c->codepoint);
}

void event_locks(uint8_t locks)
{
    keyboard_set_locks(locks);
}

bool event_chord_held(void)
{
    return event.chord_held;
}

void event_key_reset(void)
{
    memset(&event, 0, sizeof event);
}
