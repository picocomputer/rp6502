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

void lifecycle_init(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) i();
    LIFECYCLE_FORWARD(RP6502_MACH_DRIVERS)
#undef LIFECYCLE
}

/* The task column. These run while FatFs is blocking -- msc_pump, the USB
 * string fetch and the BLE shutdown spin all re-enter this to complete a
 * transfer -- so calling FatFs from one of them will summon a dragon. They
 * must not block either: every driver here is a state machine.
 *
 * A named function and not only a walk, because those three call it by name. */
void main_task(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) t();
    LIFECYCLE_FORWARD(RP6502_MACH_DRIVERS)
#undef LIFECYCLE
}

/* The io_task column: the tasks that may call FatFs, never re-entered. The
 * tail of this walk is load-bearing -- see the drivers.h exec rule. */
static void task(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) iot();
    LIFECYCLE_FORWARD(RP6502_MACH_DRIVERS)
#undef LIFECYCLE
}

void lifecycle_on_run(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) r();
    LIFECYCLE_FORWARD(RP6502_MACH_DRIVERS)
#undef LIFECYCLE
}

void lifecycle_on_stop(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) s();
    LIFECYCLE_REVERSE(RP6502_MACH_DRIVERS)
#undef LIFECYCLE
}

/* Backward, like stop: a break is a teardown. It is also what puts com_break
 * near the last, where the newline it writes lands after whatever the other
 * breaks printed. */
void lifecycle_break_drivers(void)
{
#define LIFECYCLE(i, t, iot, r, s, b) b();
    LIFECYCLE_REVERSE(RP6502_MACH_DRIVERS)
#undef LIFECYCLE
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

bool lifecycle_break(void)
{
    proc_cancel_launcher();
    is_breaking = true;
    return true;
}

bool lifecycle_break_to_launcher(void)
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
    lifecycle_init();
    while (true)
    {
        main_task();
        task();
        if (is_breaking)
            lifecycle_stop();
        lifecycle_commit();
        if (is_breaking)
        {
            lifecycle_break_drivers();
            is_breaking = false;
        }
    }
}
