/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_USB_USB_H_
#define _VGA_USB_USB_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void usb_init(void);
void usb_task(void);

/* Exported serial number for USB descriptor
 */

extern char serno[];

#endif /* _VGA_USB_USB_H_ */
