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

#define LED_DRIVER DRIVER(INIT(led_init))

#endif /* _VGA_SYS_LED_H_ */
