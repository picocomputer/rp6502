/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_SOKOL_PAD_H_
#define _HOST_SOKOL_PAD_H_

#include "core/hid/pad.h"

/* A button by the name a script or a config file calls it. */
bool pad_button_from_name(const char *name, pad_button_t *button);

#endif /* _HOST_SOKOL_PAD_H_ */
