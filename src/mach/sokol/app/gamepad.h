/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MACH_SOKOL_APP_GAMEPAD_H_
#define _MACH_SOKOL_APP_GAMEPAD_H_

#include "core/hid/gamepad.h"

/* A button by the name a script or a config file calls it. */
bool gamepad_button_from_name(const char *name, gamepad_button_t *button);

#endif /* _MACH_SOKOL_APP_GAMEPAD_H_ */
