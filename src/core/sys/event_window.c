/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The events only a host with a window of its own raises, and what only it
 * needs back: a paste its system handed over, and the cursor to show over
 * the canvas. A machine with a display of its own links event.c alone. */

#include "core/sys/event.h"
#include "core/hid/gamepad.h"
#include "core/hid/mouse.h"
#include "core/hid/paste_host.h"
#include "core/sys/proc.h"
#include "core/term/vterm_mouse.h"
#include "core/hid/tablet.h"
#include "core/vga/mode/mode6.h"

void event_paste(const char *utf8)
{
    paste_start(utf8);
}

/* The VGA draws the program's own pointers, and a shape the system has none
 * of is no pointer. */
uint8_t event_cursor(void)
{
    const uint8_t cursor = tablet_shape();
    if (cursor >= TABLET_CURSOR_COUNT || mode6_custom())
        return TABLET_CURSOR_OFF;
    return cursor;
}

void event_focus(bool focused)
{
    vterm_focus(focused);
}

event_status_t event_status(void)
{
    const bool to_tablet = event_has_pointer();
    tablet_host_give(to_tablet);
    const bool pads = gamepad_is_mapped();
    event_pads_watch(pads);
    return (event_status_t){
        .flags = (uint8_t)((mouse_is_mapped() ? EVENT_MOUSE_MAPPED : 0) |
                           (to_tablet ? EVENT_TO_TABLET : 0) |
                           (to_tablet && tablet_wheel_wanted() ? EVENT_WHEEL_TO_TABLET : 0) |
                           (proc_exited() ? EVENT_EXITED : 0) |
                           (pads ? EVENT_PADS_MAPPED : 0)),
    };
}

void event_reset(void)
{
    event_key_reset();
    event_pads_reset();
    event_set_shared(false);
}
