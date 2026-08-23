/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_USB_CDC_H_
#define _VGA_USB_CDC_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void cdc_task(void);

// True when a host terminal is ready and actively communicating
bool cdc_is_ready(void);

#endif /* _VGA_USB_CDC_H_ */
