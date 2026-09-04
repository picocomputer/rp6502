/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What a machine is, to core: a list of drivers and a run state.
 *
 * The list is the machine's own -- RP6502_MACH_DRIVERS in its drivers.h -- and
 * what a row of it looks like is core/sys/driver.h. This file is what a machine
 * does with the list: bring it up, pump it, and the latch that turns "stop the
 * 6502" from an ask into a doing. Each machine keeps the rest of its own loop
 * in its own main.c. */

#ifndef _CORE_SYS_SYS_H_
#define _CORE_SYS_SYS_H_

#include <stdbool.h>
#include <stddef.h>

/* The row shape and the walks this file's fan-outs are built out of. */
#include "core/sys/driver.h"


/* Cold boot: this machine's drivers, walked forward. Every machine answers
 * it, because every machine has drivers to bring up. */
void sys_init(void);

/* One pass of this machine's drivers. The task column must be safe to call
 * during blocking file IO -- it is what a host's blocking loops re-enter while
 * a transfer completes -- and the io_task column, which may itself perform
 * file IO, is never re-entered. A machine's loop calls both, then sys_commit.
 */
void sys_task(void);
void sys_io_task(void);

// This is true when the 6502 is running or there's a pending
// request to start it.
bool sys_active(void);

// Request to "start the 6502".
// It will safely do nothing if the 6502 is already running.
void sys_run(void);

// Request to "stop the 6502".
// It will safely do nothing if the 6502 is already stopped.
void sys_stop(void);

/* Perform whatever was asked for -- a start, a stop, a break. A machine calls
 * this from its loop, at a point where it can afford the fan-out, and nowhere
 * else: performing a break from inside a walk would tear down the machine
 * under the driver that is mid-pass. */
void sys_commit(void);

/* Ask for a break and let sys_commit perform it, after the stop it implies. */
void sys_break_request(void);

/* Perform a stop on the spot, and only a stop. For a driver that must put the
 * outgoing program away from inside a walk, because its RAM is about to be
 * written over. */
void sys_stop_now(void);

// Request to "break the system".
// A break is triggered by CTRL-ALT-DEL or UART breaks.
// If the 6502 is running, stop events will be called first.
// False when this platform has nowhere to break to, which is a machine
// with no monitor; the key that asked is then an ordinary key.
bool sys_break(void);

// Like sys_break, but keeps the launcher/exec chain so the launcher
// re-runs instead of dropping to the monitor. Triggered by Alt-F4.
// False when there is nowhere to go: from the launcher itself on any
// platform, and with none registered on a platform that has no monitor
// to fall back to. A RIA with none registered breaks to the monitor.
bool sys_break_to_launcher(void);

#endif /* _CORE_SYS_SYS_H_ */
