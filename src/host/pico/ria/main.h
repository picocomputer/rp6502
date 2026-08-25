/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MAIN_H_
#define _RIA_MAIN_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/main.h"

/* This manages the main loop for the operating system.
 * Device drivers (everything is a device driver) are notified of various
 * events like init, task, run, stop, break, and reclock.
 * API and XREG calls are dispatched from here too. Everything follows
 * this pattern so it's worth reading main.c in its entirety.
 */

/* Special events dispatched from main.c
 */

void main_task(void);
void main_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);

#endif /* _RIA_MAIN_H_ */
