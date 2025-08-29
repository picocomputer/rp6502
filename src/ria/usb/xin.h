/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_XIN_H_
#define _RIA_USB_XIN_H_

/* USB XInput driver for XBox gamepads.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void xin_task(void);

// For monitor status command.
int xin_pad_count(void);

#endif /* _RIA_USB_XIN_H_ */
