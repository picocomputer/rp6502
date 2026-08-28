/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/main.h"
#include "drivers.h"
#include "core/api/proc.h"
#include <pico/stdlib.h>

/**************************************/
/* All device drivers register below. */
/**************************************/

/* The task column. These run while FatFs is blocking -- msc_pump, the USB
 * string fetch and the BLE shutdown spin all re-enter this to complete a
 * transfer -- so calling FatFs from one of them will summon a dragon. They
 * must not block either: every driver here is a state machine.
 *
 * A named function and not only a walk, because those three call it by name. */
void main_task(void)
{
#define DRIVER(i, t, iot, r, s, b) t();
    MACH_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
}

/* The io_task column: the tasks that may call FatFs, never re-entered. The
 * tail of this walk is load-bearing -- see the drivers.h exec rule. */
static void task(void)
{
#define DRIVER(i, t, iot, r, s, b) iot();
    MACH_FORWARD(RP6502_MACH_DRIVERS)
#undef DRIVER
}

/* Backward, like stop: a break is a teardown. It is also what puts com_break
 * near the last, where the newline it writes lands after whatever the other
 * breaks printed. */
void mach_break_drivers(void)
{
#define DRIVER(i, t, iot, r, s, b) b();
    MACH_REVERSE(RP6502_MACH_DRIVERS)
#undef DRIVER
}

// Triggered once after init then after every PHI2 change.
void main_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    cpu_reclock();
    ria_reclock(clkdiv_int, clkdiv_frac);
    pix_reclock(clkdiv_int, clkdiv_frac);
}


/*****************************/
/* This is the OS scheduler. */
/*****************************/

static bool is_breaking;

bool mach_break(void)
{
    proc_cancel_launcher();
    is_breaking = true;
    return true;
}

bool mach_break_to_launcher(void)
{
    // From the launcher there is nowhere to return to.
    if (proc_is_launcher())
        return false;
    api_set_ax(0xFFFF);
    is_breaking = true;
    return true;
}

int main(void)
{
    mach_init();
    while (true)
    {
        main_task();
        task();
        if (is_breaking)
            mach_stop();
        mach_commit();
        if (is_breaking)
        {
            mach_break_drivers();
            is_breaking = false;
        }
    }
}
