/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The run loop every machine has. Each keeps the rest of its own -- the task
 * pump, reclocking -- in its own main.h. */

#ifndef _CORE_LIFECYCLE_H_
#define _CORE_LIFECYCLE_H_

#include <stdbool.h>
#include <stddef.h>


// This is true when the 6502 is running or there's a pending
// request to start it.
bool lifecycle_active(void);

// Request to "start the 6502".
// It will safely do nothing if the 6502 is already running.
void lifecycle_run(void);

// Request to "stop the 6502".
// It will safely do nothing if the 6502 is already stopped.
void lifecycle_stop(void);

/* Perform a start or stop that was asked for. A machine calls this from its
 * loop, at a point where it can afford the fan-out. */
void lifecycle_commit(void);

/* The fan-outs themselves, which are the machine's: what it has to bring up
 * for a program to run, and what it has to put away afterwards. The ordering
 * within them is the whole content, so they stay where the reasons are. */
void lifecycle_on_run(void);
void lifecycle_on_stop(void);

// Request to "break the system".
// A break is triggered by CTRL-ALT-DEL or UART breaks.
// If the 6502 is running, stop events will be called first.
// False when this platform has nowhere to break to, which is a machine
// with no monitor; the key that asked is then an ordinary key.
bool lifecycle_break(void);

// Like lifecycle_break, but keeps the launcher/exec chain so the launcher
// re-runs instead of dropping to the monitor. Triggered by Alt-F4.
// False when there is nowhere to go: from the launcher itself on any
// platform, and with none registered on a platform that has no monitor
// to fall back to. A RIA with none registered breaks to the monitor.
bool lifecycle_break_to_launcher(void);

#endif /* _CORE_LIFECYCLE_H_ */
