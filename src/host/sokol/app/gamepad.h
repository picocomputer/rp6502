/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Which host controller is which player, and when to look for more. Reading the
 * controllers is the host_gamepad_ half of entry.h, one implementation per
 * desktop; only the desktop emulator builds this file, because web and Android
 * reach core/hid/gamepad.h by their own paths.
 *
 * Sokol has no gamepad API, so these are polled rather than delivered as events
 * the way the rest of input.c is.
 */

#ifndef _HOST_SOKOL_APP_GAMEPAD_H_
#define _HOST_SOKOL_APP_GAMEPAD_H_

void gamepad_input_task(void);

/* Release the host's controllers and blank every player. */
void gamepad_input_stop(void);

#endif /* _HOST_SOKOL_APP_GAMEPAD_H_ */
