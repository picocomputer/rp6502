/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_LIBRETRO_INPUT_H_
#define _HOST_LIBRETRO_INPUT_H_

#include <stdbool.h>
#include <stdint.h>

#include "libretro.h"

void input_init(retro_environment_t environ_cb);

/* One key going down or up. character is what the frontend's own keyboard
 * layout composed, or 0 where the frontend supplies none. */
void input_keyboard_event(bool down, unsigned keycode, uint32_t character,
                          uint16_t key_modifiers);

void input_poll(retro_input_state_t state);

/* What the frontend says is plugged into a port; RETRO_DEVICE_NONE unplugs. */
void input_set_port_device(unsigned port, unsigned device);

/* A savestate has just been put back, so what this host believes about the
 * frontend's devices has to be made to agree with the machine again.
 *
 * Marking every port live is not a claim that a pad is plugged in: it is what
 * makes the next input_poll announce a disconnect for every port this
 * frontend does not have, because that poll only announces a disconnect for a
 * port it already thought was connected. Without it, a blob saved with four
 * pads loads into a session with none and a program waits on players forever.
 *
 * The keyboard is released because keys arrive as events rather than being
 * polled, so the key-up that would have cleared a restored key belongs to a
 * session the machine is no longer in.
 *
 * Called only for a load with neither flag: runahead and rewind stay inside
 * one session with the frontend's devices unchanged, and a netplay rollback
 * must not have this peer's idea of who is plugged in injected into a state
 * that came from the other peer. */
void input_state_restored(void);

void input_reset(void);

#endif /* _HOST_LIBRETRO_INPUT_H_ */
