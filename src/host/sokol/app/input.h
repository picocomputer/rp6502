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

/* Translate one host (sokol) input event into emulated keyboard/mouse input. */
void input_event(const struct sapp_event *e);

/* Tell the input layer whether the host pointer is over the drawn canvas, so
 * the tablet's requested cursor applies only there and the system cursor shows
 * in the letterbox. */
void input_set_pointer_on_canvas(bool on);

/* Apply the tablet ROM's requested host cursor, or the debugger's over a panel.
 * The sole cursor writer -- simgui's own control is disabled -- run once per
 * frame so a ROM's change or a panel hover is reflected promptly. */
void input_update_cursor(void);

#endif /* _HOST_SOKOL_APP_INPUT_H_ */
