/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * This is the main kernel event loop.
 */

// Request to "start the 6502".
// It will safely do nothing if the 6502 is already running.
void main_run();

// Request to "stop the 6502".
// It will safely do nothing if the 6502 is already stopped.
void main_stop();

// Request to "break the kernel".
// A break is triggered by CTRL-ALT-DEL and UART breaks.
// If the 6502 is running, stop events will be called first.
// Kernel modules should reset to a state similar to after
// init() was first run.
void main_break();

// This is true when the 6502 is running or there's a pending
// request to start it.
bool main_active();

/*
 * See main.c for information about the events below.
 */

void main_task();
void main_reclock(uint32_t sys_clk_khz, uint16_t clkdiv_int, uint8_t clkdiv_frac);
bool main_pix(uint8_t ch, uint8_t addr, uint16_t word);
bool main_api(uint8_t operation);

#endif /* _MAIN_H_ */
