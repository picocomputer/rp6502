/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The run loop every machine has. Each keeps the rest of its own -- the task
 * pump, reclocking, XREG dispatch -- in its main.h. */

#ifndef _CORE_MAIN_H_
#define _CORE_MAIN_H_

#include <stdbool.h>
#include <stddef.h>

#include "core/api/std.h"

// This is true when the 6502 is running or there's a pending
// request to start it.
bool main_active(void);

// Request to "start the 6502".
// It will safely do nothing if the 6502 is already running.
void main_run(void);

// Request to "stop the 6502".
// It will safely do nothing if the 6502 is already stopped.
void main_stop(void);

/* Perform a start or stop that was asked for. A machine calls this from its
 * loop, at a point where it can afford the fan-out. */
void main_commit(void);

/* The fan-outs themselves, which are the machine's: what it has to bring up
 * for a program to run, and what it has to put away afterwards. The ordering
 * within them is the whole content, so they stay where the reasons are. */
void main_on_run(void);
void main_on_stop(void);

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

/* PIX XREG register dispatch: device 0 (the RIA's own HID and audio), device 1
 * (the video device). Device 0 never crosses a bus, so every machine answers
 * it locally. Device 1 is answered here only by a machine that is its own
 * video; one with a real bus sends it and the far end answers. */
bool main_xreg_0(uint8_t channel, uint8_t address, uint16_t word);
bool main_xreg_1(uint8_t channel, uint8_t address, uint16_t word);

/* The bus between the 6502 and the machine. A machine with no such transfer
 * answers false and never latches. */

// True while a memory transfer to or from the 6502 is in flight.
bool ria_active(void);

// Returns true once per latched SIGINT, then clears.
bool ria_get_sigint(void);
void ria_trigger_sigint(void);

#endif /* _CORE_MAIN_H_ */
