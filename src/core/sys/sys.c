/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Starting and stopping the 6502, which is a request and not the doing of it.
 *
 * A stop can be asked for from anywhere -- a syscall, a key, an interrupt on
 * another core -- and almost none of those places can afford to run a fan-out
 * that closes files and parks drivers. So the ask is cheap and idempotent, and
 * the machine's loop performs it at a moment of its own choosing. The one
 * thing that cannot wait is RESB, which goes down inside the ask, because a
 * 6502 left running would keep asking for what is being torn down. That is a
 * concurrency fact of the machines whose CPU runs beside the fan-out -- the
 * Pico's second core, the Pocket's fabric. On a software machine the 6502
 * only ever runs inside the bus task, so there is no race for RESB to win; it
 * goes down early there because a stop is a stop, not because it must.
 *
 * So this file is where the line lives (core/wdc/resb.h): down before the
 * driver walk, down again in every ask, up once the run fan-out has finished.
 * No driver row could be all three.
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

/* A break asked for, not yet performed. Kept apart from the state above
 * because a break is a teardown that outlives the stop it implies: the stop
 * fan-out puts the program away, the break fan-out puts the machine's own
 * state machines back. */
static volatile bool sys_breaking;

/* Cold boot: every driver this machine lists, in the order it lists them.
 * One copy for every machine -- each root puts its own machine directory on
 * the include path, so "drivers.h" above is its own. */
void sys_init(void)
{
    resb_init();
#define DRIVER(i, t, iot, r, s, b, ...) i();
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
}

/* One pass of a machine's drivers: the task column, then the io_task column.
 * They are separate walks because only one of them is safe to call during
 * blocking file IO -- a machine whose file operations block re-enters
 * sys_task while a transfer completes, and sys_io_task is where the tasks
 * that may themselves touch a filesystem go. A machine that never blocks
 * calls the two back to back. */
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

/* The fan-outs behind the latch: what a machine brings up for a program to
 * run, and what it puts away afterwards. Static because sys_commit below is
 * the only thing that may perform them -- asking is everyone's, doing is
 * the loop's. */
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

/* Backward, like stop: a break is a teardown. It is also what puts com_break
 * near the last, where the newline it writes lands after whatever the other
 * breaks printed. */
static void sys_on_break(void)
{
#define DRIVER(i, t, iot, r, s, b, ...) b();
    DRIVERS_REVERSE(RP6502_MACH_DRIVERS)
#undef DRIVER
}

void sys_run(void)
{
    /* Only from stopped. A stop that has been asked for but not performed is
     * a teardown this machine still owes its drivers, and promoting it to a
     * start would skip the fan-out that closes their files. Every caller
     * already asks only when sys_active() is false; this is that rule kept
     * here, where it cannot be forgotten. */
    if (sys_state == stopped)
        sys_state = starting;
}

void sys_stop(void)
{
    resb_assert(); /* the rest of the fan-out can wait; this cannot */
    if (sys_state == starting)
        sys_state = stopped; /* never started; nothing to tear down */
    else if (sys_state != stopped)
        sys_state = stopping;
}

bool sys_active(void)
{
    return sys_state != stopped;
}

/* A break is a stop plus a teardown of what the machine itself was in the
 * middle of. RESB drops here, with the ask, for the same reason every other
 * stop drops it here. */
void sys_break_request(void)
{
    sys_breaking = true;
    sys_stop();
}

/* Put the outgoing program away, on the spot. The one thing a driver inside a
 * walk may perform: a program's RAM is about to be written over, and what ran
 * on it has to be shut down first. Performing a break is the loop's alone. */
void sys_stop_now(void)
{
    sys_stop();
    if (sys_state == stopping)
    {
        sys_on_stop();
        sys_state = stopped;
    }
}

/* Perform whatever was asked for. The machine's loop calls this where it can
 * afford to, which is what makes the ask cheap everywhere else. */
void sys_commit(void)
{
    /* Re-derived from the flag rather than taken on trust from the ask: a
     * break asked for anywhere in a pass has to beat a run armed anywhere in
     * it, and the ask's own stop can be undone -- it maps a machine that
     * never started to stopped, which a later sys_run takes back to starting.
     * Deriving the stop here is what makes the two orders the same. */
    if (sys_breaking)
        sys_stop();
    if (sys_state == starting)
    {
        /* Running before the fan-out, not after: a stop asked for while
         * sys_on_run is still walking is a real teardown of drivers that are
         * already up, and the stopping it lands on is performed just below.
         * Assigning after would discard it. */
        sys_state = running;
        sys_on_run();
        /* Only if the walk did not stop us. The ask lowers RESB from anywhere,
         * including from inside a run hook, and nothing downstream would raise
         * it again -- the stop fan-out does not touch the line. */
        if (sys_state == running)
            resb_release();
    }
    if (sys_state == stopping)
    {
        sys_on_stop();
        sys_state = stopped;
    }
    /* Cleared first: a break asked for by a break hook gets its own pass
     * rather than being swallowed by this one. */
    if (sys_breaking)
    {
        sys_breaking = false;
        sys_on_break();
    }
}
