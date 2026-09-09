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

/* Whether the host pointer is over the drawn canvas. The tablet's requested
 * cursor applies only there, and the system cursor shows in the letterbox. */
void input_set_pointer_on_canvas(bool on);

/* Apply the cursor the tablet program asked for, or the debugger's over one of
 * its panels. simgui's own cursor control is disabled, so the debugger's cursor
 * is written here, and this runs once a frame so a program's change or a panel
 * hover is answered promptly. */
void input_update_cursor(void);

#endif /* _HOST_SOKOL_APP_INPUT_H_ */
