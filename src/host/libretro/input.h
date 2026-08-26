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

/* Forget what this frontend said, for a core being taken down. */
void input_reset(void);

#endif /* _HOST_LIBRETRO_INPUT_H_ */
