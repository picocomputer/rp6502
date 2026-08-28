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

#include "core/lifecycle.h"
#include "core/cpu.h"

static enum state
{
    stopped,
    starting,
    running,
    stopping,
} volatile lifecycle_state;

void lifecycle_run(void)
{
    if (lifecycle_state != running)
        lifecycle_state = starting;
}

void lifecycle_stop(void)
{
    cpu_stop(); /* RESB down now; the rest of the fan-out can wait */
    if (lifecycle_state == starting)
        lifecycle_state = stopped; /* never started; nothing to tear down */
    else if (lifecycle_state != stopped)
        lifecycle_state = stopping;
}

bool lifecycle_active(void)
{
    return lifecycle_state != stopped;
}

/* Perform whatever was asked for. The machine's loop calls this where it can
 * afford to, which is what makes the ask cheap everywhere else. */
void lifecycle_commit(void)
{
    if (lifecycle_state == starting)
    {
        lifecycle_on_run();
        lifecycle_state = running;
    }
    if (lifecycle_state == stopping)
    {
        lifecycle_on_stop();
        lifecycle_state = stopped;
    }
}
