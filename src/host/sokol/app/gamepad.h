/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Which host controller is which player, and when to look for more. Reading the
 * controllers is the host_gamepad_ half of entry.h, one implementation per
 * desktop and one for the web, from the page's Gamepad API (web/entry.c).
 *
 * Sokol has no gamepad API, so these are polled rather than delivered as events
 * the way the rest of input.c is. The machine's side is in evq_machine.c.
 */

#ifndef _HOST_SOKOL_APP_GAMEPAD_H_
#define _HOST_SOKOL_APP_GAMEPAD_H_

/* The host's controllers, read once a frame while a program has the block
 * mapped (EVENT_PADS_MAPPED, as the machine last said) and sent to the machine
 * (EVQ_GAMEPADS), which gives each its player. The OS can take a millisecond
 * for each empty XInput slot, which the window's thread spends, not the
 * machine's. Once no program has the block mapped the controllers are closed. */
void gamepad_input_poll(void);

/* Release the host's controllers. */
void gamepad_input_stop(void);

#endif /* _HOST_SOKOL_APP_GAMEPAD_H_ */
