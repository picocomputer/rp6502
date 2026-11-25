/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Which host controller is which player, and when to look for more. Reading the
 * controllers is the host_gamepad_ half of entry.h, one implementation per
 * desktop; only the desktop emulator builds this file, because the web reaches
 * core/hid/gamepad.h by its own path.
 *
 * Sokol has no gamepad API, so these are polled rather than delivered as events
 * the way the rest of input.c is.
 */

#ifndef _HOST_SOKOL_APP_GAMEPAD_H_
#define _HOST_SOKOL_APP_GAMEPAD_H_

/* The host's controllers are read once a frame in two halves. The poll asks
 * the OS, which can take a millisecond for each empty XInput slot, so the
 * window does it without the machine lock; the report hands what it found to
 * the machine, under the lock, before the frames due. A poll goes by whether
 * a program had the block mapped when the machine was last asked, and closes
 * the controllers once none has; gamepad_input_watch asks, under the lock,
 * after the frames, so a program's mapping is seen at the next poll. */
void gamepad_input_poll(void);
void gamepad_input_report(void);
void gamepad_input_watch(void);

/* Release the host's controllers and blank every player, once the machine
 * has stopped. */
void gamepad_input_stop(void);

#endif /* _HOST_SOKOL_APP_GAMEPAD_H_ */
