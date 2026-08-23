/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The run loop, as the machine sees it. Every machine has one -- ria/main.c on
 * the Pico, emu/main.c, rtl/sw/main.c on the Pocket -- and it owns the same
 * three jobs everywhere: dispatch an API call, name the stdio drivers this
 * platform has, and answer whether there is anywhere to break to.
 *
 * What each machine adds to its own main.h is the part core never asks for:
 * starting and stopping the 6502, the task pump, reclocking, XREG dispatch. */

#ifndef _CORE_MAIN_H_
#define _CORE_MAIN_H_

#include <stdbool.h>
#include <stddef.h>

#include "core/api/std.h"

// API calls are dispatched here.
bool main_api(uint8_t operation);

// This platform's stdio driver table (built in its main.c).
const std_driver_t *main_std_drivers(size_t *count);

// Request to "break the system".
// A break is triggered by CTRL-ALT-DEL or UART breaks.
// If the 6502 is running, stop events will be called first.
// False when this platform has nowhere to break to, which is a machine
// with no monitor; the key that asked is then an ordinary key.
bool main_break(void);

// Like main_break, but keeps the launcher/exec chain so the launcher
// re-runs instead of dropping to the monitor. Triggered by Alt-F4.
// False when there is nowhere to go: from the launcher itself on any
// platform, and with none registered on a platform that has no monitor
// to fall back to. A RIA with none registered breaks to the monitor.
bool main_break_to_launcher(void);

/* The bus between the 6502 and the machine. Named for the Pico's RIA chip
 * because that is where it was written, but these three are lifecycle rather
 * than chip: a machine with no such transfer answers false and never latches. */

// True while a memory transfer to or from the 6502 is in flight.
bool ria_active(void);

// Returns true once per latched SIGINT, then clears.
bool ria_get_sigint(void);
void ria_trigger_sigint(void);

#endif /* _CORE_MAIN_H_ */
