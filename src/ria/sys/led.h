/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_LED_H_
#define _RIA_SYS_LED_H_

/* System LED control
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void led_init(void);
void led_task(void);

// Make it blink
void led_blink(bool on);

#endif /* _RIA_SYS_LED_H_ */
