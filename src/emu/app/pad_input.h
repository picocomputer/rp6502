/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_APP_PAD_INPUT_H_
#define _EMU_APP_PAD_INPUT_H_

#include "emu/hid/pad.h"
#include <stdbool.h>
#include <stdint.h>

/* Host gamepads into the emulated ones. The policy — the privacy gate, which
 * host controller is which player, when to look for more — lives in
 * pad_input.c and is the same everywhere. Reading the controllers is the
 * host_pad_ seam below, one implementation per desktop; web and Android have
 * their own paths into pad.c and build a stub.
 *
 * Sokol has no gamepad API, so this is polled rather than delivered as events
 * like the rest of input.c. */

void pad_input_task(void);

/* Release the host's controllers and blank every player. */
void pad_input_stop(void);

/* One host controller, in the units pad_host_report takes, because scaling
 * belongs where the ranges are known. A backend claims a type only when it is
 * certain of the labels, and sticks only when it found both. */
typedef struct
{
    uint64_t id; /* stable while plugged, so a player keeps its number */
    uint8_t dpad, button0, button1;
    int8_t lx, ly, rx, ry;
    uint8_t lt, rt;
    uint8_t type; /* PAD_TYPE_ */
    bool sticks;
} pad_host_t;

/* Start reading controllers. Called on the first frame a program has the
 * gamepad block mapped, and not before — until then the emulator must not
 * touch an input device. False when the host has nothing to offer, which is
 * ordinary and is retried. */
bool host_pad_open(void);

/* Stop reading, release everything, and expect host_pad_open again. */
void host_pad_close(void);

/* What is connected now, newest state, up to max entries. Returns the count.
 * Called once per presented frame while a program has the block mapped. */
int host_pad_poll(pad_host_t *pads, int max);

#endif /* _EMU_APP_PAD_INPUT_H_ */
