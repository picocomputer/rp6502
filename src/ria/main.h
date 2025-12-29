/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MAIN_H_
#define _RIA_MAIN_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* This manages the main loop for the operating system.
 * Device drivers (everything is a device driver) are notified of various
 * events like init, task, run, stop, break, pre_reclock, and post_reclock.
 * API and XREG calls are dispatched from here too. Everything follows
 * this pattern so it's worth reading main.c in its entirety.
 */

// This is true when the 6502 is running or there's a pending
// request to start it.
bool main_active(void);

// Request to "start the 6502".
// It will safely do nothing if the 6502 is already running.
void main_run(void);

// Request to "stop the 6502".
// It will safely do nothing if the 6502 is already stopped.
void main_stop(void);

// Request to "break the system".
// A break is triggered by CTRL-ALT-DEL or UART breaks.
// If the 6502 is running, stop events will be called first.
void main_break(void);

/* Special events dispatched from main.c
 */

void main_task(void);
void main_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);
bool main_xreg(uint8_t chan, uint8_t addr, uint16_t word);
bool main_api(uint8_t operation);

#endif /* _RIA_MAIN_H_ */
