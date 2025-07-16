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

// Check if device is a known Xbox controller
bool xinput_is_xbox_controller(uint16_t vendor_id, uint16_t product_id);

#endif /* _XINPUT_H_ */
