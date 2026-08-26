/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_SOKOL_GAMEPAD_INPUT_H_
#define _HOST_SOKOL_GAMEPAD_INPUT_H_

#include "core/hid/gamepad.h"
#include <stdbool.h>
#include <stdint.h>

/* Host gamepads into the emulated ones. The policy — the privacy gate, which
 * host controller is which player, when to look for more — lives in
 * gamepad_input.c and is the same everywhere. Reading the controllers is the
 * host_gamepad_ seam below, one implementation per desktop; web and Android have
 * their own paths into gamepad.c and build a stub.
 *
 * Sokol has no gamepad API, so this is polled rather than delivered as events
 * like the rest of input.c. */

void gamepad_input_task(void);

/* Release the host's controllers and blank every player. */
void gamepad_input_stop(void);

/* One host controller, in the units gamepad_host_report takes, because scaling
 * belongs where the ranges are known. A backend claims a type only when it is
 * certain of the labels, and sticks only when it found both. */
typedef struct
{
    uint64_t id; /* stable while plugged, so a player keeps its number */
    uint8_t dpad, button0, button1;
    int8_t lx, ly, rx, ry;
    uint8_t lt, rt;
    uint8_t type; /* GAMEPAD_TYPE_ */
    bool sticks;
} gamepad_host_t;

/* Start reading controllers. Called on the first frame a program has the
 * gamepad block mapped, and not before — until then the emulator must not
 * touch an input device. False when the host has nothing to offer, which is
 * ordinary and is retried. */
bool host_gamepad_open(void);

/* Stop reading, release everything, and expect host_gamepad_open again. */
void host_gamepad_close(void);

/* What is connected now, newest state, up to max entries. Returns the count.
 * Called once per presented frame while a program has the block mapped. */
int host_gamepad_poll(gamepad_host_t *gamepads, int max);

#endif /* _HOST_SOKOL_GAMEPAD_INPUT_H_ */
