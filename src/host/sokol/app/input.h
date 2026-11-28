/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The window's side of the input: each host event made into the messages
 * the machine takes (evq.h), in its own units, with everything about the
 * window already worked out: where on the canvas, how far in mouse counts,
 * whether the pointer is captured. Uncaptured, the pointer is the host's
 * tablet (event_tablet_t); captured, it is the host's mouse
 * (event_mouse_t). What the window must decide on the spot
 * it decides from what the machine last told it (EVQ_STATUS): whether a
 * click captures the pointer, whether the tablet has the pointer and the
 * wheel, which cursor shows.
 */

#ifndef _HOST_SOKOL_APP_INPUT_H_
#define _HOST_SOKOL_APP_INPUT_H_

#include "host/sokol/app/evq.h"
#include <stdbool.h>

struct sapp_event;

/* One sokol input event, sent to the machine. */
void input_event(const struct sapp_event *e);

/* What the machine last told the window. */
void input_set_status(const evq_status_t *s);
const evq_status_t *input_status(void);

/* sokol said the clipboard is too big for its buffer (app_log). */
void input_clipboard_too_big(void);

/* Whether the event is a paste key going down (core/hid/key.h), not its
 * repeat. The desktop reads its clipboard then and pastes it in the key's
 * place. A web page reads its clipboard only from the paste event the key
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

/* The three locks as the system has them, sent when they change. A lock key
 * changes nothing here: the system changes the lock and this reads it, once
 * a frame and before each key press, so a key is typed by the locks it went
 * down with. Only a window sends them, so a headless or script run keeps its
 * own locks. */
void input_locks(void);

/* What the events and the cursor task asked of the OS: the pointer captured
 * or let go, and the cursor's shape. */
void input_apply(void);

/* sapp_mouse_locked(), limited on the web to the lock on Module.canvas. */
bool input_mouse_locked(void);

/* Whether the tablet wants the pointer, and the wheel too, as the machine
 * last said, which says what cursor shows over the canvas and what the web
 * page cancels. The tablet is sent the pointer whether or not it wants it,
 * while the pointer is not captured; captured, it is the mouse. A wheel that
 * nothing wants is not cancelled: it is left to the page around the player,
 * which scrolls with it. */
bool input_pointer_to_tablet(void);
bool input_wheel_to_tablet(void);

/* Once a frame: asks to let go of a capture the mouse no longer wants. */
void input_pointer_task(void);

/* Choose the cursor the tablet program asked for, or the debugger's over one
 * of its panels, for input_apply. simgui's own cursor control is disabled, so
 * the debugger's cursor is written here, and this runs once a frame so a
 * program's change or a panel hover is answered promptly. */
void input_cursor_task(void);

/* Back to the start, for tests. */
void input_reset(void);

#endif /* _HOST_SOKOL_APP_INPUT_H_ */
