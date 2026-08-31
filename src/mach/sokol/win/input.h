/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _MACH_SOKOL_WIN_INPUT_H_
#define _MACH_SOKOL_WIN_INPUT_H_

struct sapp_event;

/* Translate one host (sokol) input event into emulated keyboard/mouse input. */
void input_event(const struct sapp_event *e);

#endif /* _MACH_SOKOL_WIN_INPUT_H_ */
