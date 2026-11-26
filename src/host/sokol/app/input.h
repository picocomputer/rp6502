/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_SOKOL_APP_INPUT_H_
#define _HOST_SOKOL_APP_INPUT_H_

#include <stdbool.h>

struct sapp_event;

/* Translate one sokol input event into emulated keyboard, mouse or tablet
 * input. */
void input_event(const struct sapp_event *e);

/* Whether the event is a paste key going down (core/hid/key.h), not its
 * repeat. A web page reads its clipboard only from the paste event the key
 * makes, so the web player leaves the key to the browser, and pastes at
 * that event. */
bool input_paste_key(const struct sapp_event *e);

/* Whether the event is a key going down that the browser must not act on:
 * a copy or cut key, which the player acts on itself (a browser may open
 * its developer tools on Ctrl+Shift+C); plain Ctrl+V, which types 0x16;
 * and a held paste key's repeat, as a held chord acts once
 * (core/hid/key.h). The web player cancels it, so every paste event the
 * page gets pastes. */
bool input_key_withheld(const struct sapp_event *e);

/* Take the three locks from the system, which owns them. A lock key changes
 * nothing here: the system changes the lock and this reads it, once a frame
 * and before each key press. input_read_locks asks the system, on the window's
 * thread without the machine's lock, since an X server takes a round trip to
 * answer; input_locks hands what it said to the machine, under the lock. Only
 * a window calls these, so a headless or script run keeps its own locks. */
void input_read_locks(void);
void input_locks(void);

/* What the events and the cursor task asked of the OS: the pointer captured
 * or let go, and the cursor's shape. The window, after it lets go of the
 * machine's lock. */
void input_apply(void);

/* sapp_mouse_locked(), limited on the web to the lock on Module.canvas. */
bool input_mouse_locked(void);

/* Whether this host gives its pointer to the tablet (tablet_wanted) rather
 * than capturing it for the mouse. A program that maps the mouse and not the
 * tablet is not on the tablet, even while the console's terminal claims it:
 * the pointer is the mouse's, its first click only captures, and the terminal
 * has no pointer here. A program that maps both is fed from the tablet's one
 * pointer, uncaptured. */
bool input_pointer_to_tablet(void);

/* Whether the wheel goes to the tablet (tablet_wheel_wanted) while it has
 * the pointer. A wheel that nothing uses is neither fed nor cancelled: it is
 * left to the page around the player, which scrolls with it. */
bool input_wheel_to_tablet(void);

/* Once a frame: asks to let go of a capture the mouse no longer wants, and tells
 * the tablet whether it has this host's pointer (tablet_host_give), so a
 * button held as it stops having it is let go. */
void input_pointer_task(void);

/* Whether the host pointer is over the drawn canvas. The tablet's requested
 * cursor applies only there, and the system cursor shows in the letterbox. */
void input_set_pointer_on_canvas(bool on);

/* Choose the cursor the tablet program asked for, or the debugger's over one
 * of its panels, for input_apply. simgui's own cursor control is disabled, so
 * the debugger's cursor is written here, and this runs once a frame so a
 * program's change or a panel hover is answered promptly. */
void input_cursor_task(void);

#endif /* _HOST_SOKOL_APP_INPUT_H_ */
