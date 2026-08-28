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
 * only ever runs inside cpu_task, so there is no race for RESB to win; it
 * goes down early there because a stop is a stop, not because it must.
 */

#include "core/mach.h"
#include "core/cpu.h"
#include "drivers.h"

static enum state
{
    stopped,
    starting,
    running,
    stopping,
} volatile mach_state;

/* Cold boot: every driver this machine lists, in the order it lists them.
 * One copy for every machine -- each root puts its own mach directory first
 * on the include path, so "drivers.h" above is its own. */
void mach_init(void)
{
#define DRIVER(i, t, iot, r, s, b) i();
    MACH_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
}

/* The fan-outs behind the latch: what a machine brings up for a program to
 * run, and what it puts away afterwards. Static because mach_commit below is
 * the only thing that may perform them -- asking is everyone's, doing is
 * the loop's. */
static void mach_on_run(void)
{
#define DRIVER(i, t, iot, r, s, b) r();
    MACH_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
}

static void mach_on_stop(void)
{
#define DRIVER(i, t, iot, r, s, b) s();
    MACH_REVERSE(RP6502_MACH_DRIVERS)
#undef DRIVER
}

void mach_run(void)
{
    if (mach_state != running)
        mach_state = starting;
}

void mach_stop(void)
{
    cpu_stop(); /* RESB down now; the rest of the fan-out can wait */
    if (mach_state == starting)
        mach_state = stopped; /* never started; nothing to tear down */
    else if (mach_state != stopped)
        mach_state = stopping;
}

bool mach_active(void)
{
    return mach_state != stopped;
}

/* Perform whatever was asked for. The machine's loop calls this where it can
 * afford to, which is what makes the ask cheap everywhere else. */
void mach_commit(void)
{
    if (mach_state == starting)
    {
        mach_on_run();
        mach_state = running;
    }
    if (mach_state == stopping)
    {
        mach_on_stop();
        mach_state = stopped;
    }
}
