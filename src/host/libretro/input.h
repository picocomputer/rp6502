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

/* Ask the frontend what it can do, once, before anything is read. */
void input_init(retro_environment_t environ_cb);

/* One key going down or up, as the frontend's keyboard callback delivers it.
 * character is what the frontend's layout composed, or 0 where it has none. */
void input_keyboard_event(bool down, unsigned keycode, uint32_t character,
                          uint16_t key_modifiers);

/* Read the pads and the pointer once, for this frame. Called from retro_run
 * after the frontend's poll, which is the only moment the state callback is
 * defined to answer. */
void input_poll(retro_input_state_t state);

/* What the frontend says is plugged into a port; RETRO_DEVICE_NONE unplugs. */
void input_set_port_device(unsigned port, unsigned device);

/* A savestate has just been put back, so what this host believes about the
 * frontend's devices and what the machine believes about them have to be
 * made to agree again.
 *
 * Every port is marked live, which is not a claim that a pad is plugged in:
 * it is what makes the next poll announce a disconnect for every port this
 * frontend does not have. Without it a blob saved with four pads loads into
 * a session with none and the machine waits on players forever, because the
 * poll only announces a disconnect for a port it thinks was connected.
 *
 * The keyboard is released for the other half of the same problem. Keys
 * arrive as events rather than being polled, so the release that would have
 * cleared a restored key belongs to a session the machine is no longer in.
 *
 * Not called for a load the machine made itself. Runahead and rewind stay
 * inside one session with the frontend's devices unchanged, and a netplay
 * rollback must not have this peer's idea of who is plugged in injected into
 * a state that came from the other one. */
void input_state_restored(void);

/* Forget what this frontend said, for a core being taken down. */
void input_reset(void);

#endif /* _HOST_LIBRETRO_INPUT_H_ */
