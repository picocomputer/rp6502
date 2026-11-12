/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Starting and stopping the 6502. sys_run and sys_stop move sys_state, and
 * the machine's loop calls sys_commit, which performs the driver fan-out.
 *
 * sys_stop is called from anywhere -- a syscall, a key, an interrupt on
 * another core -- and most of those places cannot afford a fan-out that closes
 * files and parks drivers, so the call only moves the state. RESB is the
 * exception and goes down inside sys_stop, because on a machine whose 6502
 * runs beside the fan-out, such as the Pico's second core or the Pocket's
 * fabric, a 6502 still running would keep asking for what is being torn down.
 *
 */

#include "core/sys/sys.h"
#include "core/wdc/resb.h"
#include "drivers.h"
#include "machine.h"

static enum state
{
    stopped,
    starting,
    running,
    stopping,
} volatile sys_state;

/* A break asked for, not yet performed. It is a flag beside the state rather
 * than another state because a break outlives the stop it implies: sys_commit
 * performs the stop fan-out first and the break fan-out after it. */
static volatile bool sys_breaking;

/* Set when flashing starts, and never cleared. Only sys_pump reads it. */
static bool sys_flashing;

void HOST_IN_FLASH_FUNC sys_init(void)
{
    resb_init();
#define INIT(f) f();
#include "core/sys/driver_walk.h"
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
}

/* One pass of the list, so each task has a single call site. MSVC has no
 * noinline attribute; osal/windows/msvc/compat.h defines __attribute__ away
 * there. */
__attribute__((noinline)) static void HOST_IN_RAM_FUNC sys_walk(bool fatfs, bool pumped)
{
#define TASK_FATFS(f) \
    if (fatfs)        \
        f();
#define TASK_PUMPED(f) \
    if (pumped)        \
        f();
#define TASK_ALWAYS(f) f();
#include "core/sys/driver_walk.h"
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
}

void HOST_IN_RAM_FUNC sys_task(void)
{
    sys_walk(true, true);
}

void HOST_IN_RAM_FUNC sys_pump(void)
{
    sys_walk(false, !sys_flashing);
}

void HOST_IN_FLASH_FUNC sys_set_flashing(void)
{
    sys_flashing = true;
#define FLASHING(f) f();
#include "core/sys/driver_walk.h"
    DRIVERS_REVERSE(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
}

/* RESB is low through the run, stop and break fan-outs. */
static void HOST_IN_FLASH_FUNC sys_on_run(void)
{
#define RUN(f) f();
#include "core/sys/driver_walk.h"
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
}

static void HOST_IN_FLASH_FUNC sys_on_stop(void)
{
#define STOP(f) f();
#include "core/sys/driver_walk.h"
    DRIVERS_REVERSE(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
}

static void HOST_IN_FLASH_FUNC sys_on_break(void)
{
#define BREAK(f) f();
#include "core/sys/driver_walk.h"
    DRIVERS_REVERSE(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
}

void sys_latch_get(sys_latch_t *latch)
{
    latch->state = (uint8_t)sys_state;
    latch->breaking = sys_breaking;
    latch->held = sys_state != running;
}

bool sys_latch_apply(const sys_latch_t *latch)
{
    if (latch->state != stopped && latch->state != running)
        return false;
    if (latch->state == stopped && !latch->held)
        return false;
    sys_state = latch->state;
    sys_breaking = latch->breaking;
    return true;
}

void sys_run(void)
{
    /* Only from stopped, because a stop that has been asked for but not
     * performed is a teardown the machine still owes its drivers, and
     * promoting it to a start would skip the fan-out that closes their
     * files. */
    if (sys_state == stopped)
        sys_state = starting;
}

void HOST_IN_RAM_FUNC sys_stop(void)
{
    resb_assert();
    if (sys_state == starting)
        sys_state = stopped; /* no run fan-out ran, so there is nothing to undo */
    else if (sys_state != stopped)
        sys_state = stopping;
}

bool sys_active(void)
{
    return sys_state != stopped;
}

bool sys_running(void)
{
    return sys_state == running;
}

void HOST_IN_RAM_FUNC sys_break_request(void)
{
    sys_breaking = true;
    sys_stop();
}

/* The stop is performed here and only the stop, because a driver inside a
 * walk may need the outgoing program shut down before its RAM is written
 * over. A break is left to sys_commit, the only caller of sys_on_break. */
void sys_stop_now(void)
{
    sys_stop();
    if (sys_state == stopping)
    {
        sys_on_stop();
        sys_state = stopped;
    }
}

void HOST_IN_RAM_FUNC sys_commit(void)
{
    /* The stop is derived from the flag again rather than trusted from
     * sys_break_request, because a break asked for anywhere in a pass has to
     * beat a run armed anywhere in the same pass. The stop sys_break_request
     * already did can be undone: it maps a machine that never started to
     * stopped, which a later sys_run takes back to starting. */
    if (sys_breaking)
        sys_stop();
    if (sys_state == starting)
    {
        /* Assigned before sys_on_run, not after, because a stop asked for
         * while sys_on_run is still calling drivers is a real teardown of the
         * drivers already up. The stopping state it leaves behind is handled
         * just below, and assigning after would discard it. */
        sys_state = running;
        sys_on_run();
        /* Only when the run hooks did not stop us. sys_stop lowers RESB from
         * anywhere, including from inside a run hook, and releasing here would
         * raise the line on a machine that is being torn down. */
        if (sys_state == running)
            resb_release();
    }
    if (sys_state == stopping)
    {
        sys_on_stop();
        sys_state = stopped;
    }
    /* The flag is cleared before the break hooks run, so a break asked for by
     * a break hook gets its own pass instead of being swallowed by this one. */
    if (sys_breaking)
    {
        sys_breaking = false;
        sys_on_break();
    }
}
