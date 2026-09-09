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

void sys_init(void)
{
    resb_init();
#define DRIVER(i, t, iot, r, s, b, ...) i();
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
}

void sys_task(void)
{
#define DRIVER(i, t, iot, r, s, b, ...) t();
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
}

void sys_io_task(void)
{
#define DRIVER(i, t, iot, r, s, b, ...) iot();
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
}

static void sys_on_run(void)
{
#define DRIVER(i, t, iot, r, s, b, ...) r();
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
}

static void sys_on_stop(void)
{
#define DRIVER(i, t, iot, r, s, b, ...) s();
    DRIVERS_REVERSE(RP6502_MACH_DRIVERS)
#undef DRIVER
}

static void sys_on_break(void)
{
#define DRIVER(i, t, iot, r, s, b, ...) b();
    DRIVERS_REVERSE(RP6502_MACH_DRIVERS)
#undef DRIVER
}

void sys_latch_get(sys_latch_t *latch)
{
    latch->state = (uint8_t)sys_state;
    latch->breaking = sys_breaking;
    latch->held = !resb_running();
}

bool sys_latch_apply(const sys_latch_t *latch)
{
    if (latch->state != stopped && latch->state != running)
        return false;
    if (latch->state == stopped && !latch->held)
        return false;
    sys_state = latch->state;
    sys_breaking = latch->breaking;
    resb_restore(latch->held);
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

void sys_stop(void)
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

void sys_break_request(void)
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

void sys_commit(void)
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
