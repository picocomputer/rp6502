/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _LED_H_
#define _LED_H_

#include <stdbool.h>

/* Kernel events
 */

void led_init(void);
void led_task(void);

void led_blink(bool on);

#endif /* _LED_H_ */
