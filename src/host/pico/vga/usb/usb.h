/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_USB_USB_H_
#define _VGA_USB_USB_H_

#include "core/sys/driver.h"

/* USB device driver
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void usb_init(void);
void usb_task(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define USB_DRIVER DRIVER(usb_init, usb_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _VGA_USB_USB_H_ */
