/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What a machine is, to core: a list of drivers and a run state. The list is
 * the machine's own, RP6502_MACH_DRIVERS in its drivers.h, and a row of it is
 * shaped by core/sys/driver.h. Each machine keeps the rest of its loop in its
 * own host directory. */

#ifndef _CORE_SYS_SYS_H_
#define _CORE_SYS_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/sys/driver.h"

void sys_init(void);

/* One pass of this machine's drivers. The task column must be safe to call
 * during blocking file IO, because it is what a host's blocking loops re-enter
 * while a transfer completes; the io_task column may itself perform file IO and
 * is never re-entered. A machine's loop calls both, then sys_commit. */
void sys_task(void);
void sys_io_task(void);

/* False only when the machine is fully stopped. True while a start is armed,
 * while the 6502 runs, and while a stop still owes its drivers their fan-out
 * -- that last one is no longer a running 6502, sys_stop having put RESB down
 * before it moved the state, but it is still a machine nothing may print over
 * or load a program into. */
bool sys_active(void);

void sys_run(void);
void sys_stop(void);

/* Carry out whatever was requested: a start, a stop or a break. Never call it
 * from inside a driver walk, because tearing the machine down there would do
 * so under the driver that is mid-pass. */
void sys_commit(void);

void sys_break_request(void);

/* Perform a stop on the spot, and only a stop. For a driver that must put the
 * outgoing program away from inside a walk, because its RAM is about to be
 * written over. */
void sys_stop_now(void);

/* Break to the monitor. If the 6502 is running, stop events will be called
 * first. False when this platform has nowhere to break to, which is a machine
 * with no monitor; the key that asked is then an ordinary key. */
bool sys_break(void);

/* Like sys_break, but keeps the launcher/exec chain so the launcher re-runs
 * instead of dropping to the monitor. Triggered by Alt-F4. False when there is
 * nowhere to go: from the launcher itself on any platform, and with none
 * registered on a platform that has no monitor to fall back to. A RIA with
 * none registered breaks to the monitor. */
bool sys_break_to_launcher(void);

/* A savestate carries the run state, which is sys.c's own static, and the
 * reset line, which is core/wdc/resb.c's. Neither belongs to a driver, so no
 * driver can answer for them.
 *
 * A load applies both before it loads the drivers, because the only other way
 * to put RESB down is resb_assert, which also resets the 6502, the 6522, the
 * parked bus and the run clock -- four things the blob carries. Applying them
 * fans out to nothing, since each driver that follows gets its own state back
 * and a run or stop would undo that.
 *
 * state is the enum in sys.c, 0 to 3. sys_latch_apply takes only stopped and
 * running, because starting and stopping still owe every driver a call.
 * A stopped machine always holds the line. A running one may or may not,
 * because an exec asserts RESB one pass before proc_exec_task boots. */
typedef struct
{
    uint8_t state;
    bool breaking;
    bool held;
} sys_latch_t;

void sys_latch_get(sys_latch_t *latch);
bool sys_latch_apply(const sys_latch_t *latch); /* false when it is not legal */

#endif /* _CORE_SYS_SYS_H_ */
