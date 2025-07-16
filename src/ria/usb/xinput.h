/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _XINPUT_H_
#define _XINPUT_H_

#include <stdint.h>
#include <stdbool.h>

/* Xbox One XInput protocol support
 *
 * Uses device-level mount callbacks to detect Xbox controllers
 * and handle them through a virtual HID interface
 */

/* Kernel events
 */

void xinput_init(void);

int xinput_xbox_controller_type(uint8_t dev_addr);

#endif /* _XINPUT_H_ */
