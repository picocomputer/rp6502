/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host gamepads into the emulated ones: the privacy gate, which host controller
 * is which player, and when to look for more. The same policy everywhere.
 * Reading the controllers is the host_gamepad_ seam in entry.h, one
 * implementation per desktop; web and Android reach core/hid/gamepad.h by their
 * own paths and build none of this.
 *
 * The gamepad_ prefix is core/hid's, so these keep gamepad_input_.
 *
 * Sokol has no gamepad API, so this is polled rather than delivered as events
 * like the rest of input.c.
 */

#ifndef _HOST_SOKOL_APP_GAMEPAD_H_
#define _HOST_SOKOL_APP_GAMEPAD_H_

void gamepad_input_task(void);

/* Release the host's controllers and blank every player. */
void gamepad_input_stop(void);

#endif /* _HOST_SOKOL_APP_GAMEPAD_H_ */
