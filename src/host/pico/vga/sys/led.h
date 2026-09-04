/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_LED_H_
#define _VGA_SYS_LED_H_

#include "core/sys/driver.h"

/* Turns on the LED
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void led_init(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define LED_DRIVER DRIVER(led_init, nul_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _VGA_SYS_LED_H_ */
