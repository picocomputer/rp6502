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

// True when a host terminal is ready and actively communicating
bool cdc_is_ready(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define CDC_DRIVER DRIVER(nul_init, cdc_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _VGA_USB_CDC_H_ */
