/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_USB_CDC_H_
#define _VGA_USB_CDC_H_

#include "core/sys/driver.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void cdc_task(void);

bool cdc_is_ready(void);

#define CDC_DRIVER DRIVER(TASK_PUMPED(cdc_task))

#endif /* _VGA_USB_CDC_H_ */
