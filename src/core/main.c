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
 * 6502 left running would keep asking for what is being torn down.
 */

#include "core/main.h"
#include "core/cpu.h"

static enum state
{
    stopped,
    starting,
    running,
    stopping,
} volatile main_state;

void main_run(void)
{
    if (main_state != running)
        main_state = starting;
}

void main_stop(void)
{
    cpu_stop(); /* RESB down now; the rest of the fan-out can wait */
    if (main_state == starting)
        main_state = stopped; /* never started; nothing to tear down */
    else if (main_state != stopped)
        main_state = stopping;
}

bool main_active(void)
{
    return main_state != stopped;
}

/* Perform whatever was asked for. The machine's loop calls this where it can
 * afford to, which is what makes the ask cheap everywhere else. */
void main_commit(void)
{
    if (main_state == starting)
    {
        main_on_run();
        main_state = running;
    }
    if (main_state == stopping)
    {
        main_on_stop();
        main_state = stopped;
    }
}
