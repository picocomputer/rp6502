/*
 * Copyright (c) 2026 Rumbledethumps
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
#include "host/usbh.h"
#include "host/usbh_pvt.h"

/* Main events
 */

// For monitor status command.
int xin_pad_count(void);

// Class driver for app-driver registration.
const usbh_class_driver_t *xin_get_class_driver(void);

#endif /* _RIA_USB_XIN_H_ */
