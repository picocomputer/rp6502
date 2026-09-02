/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sys.h"
#include "ria/main.h"
#include "drivers.h"
#include "core/api/proc.h"
#include "core/rp2350.h"
#include <hardware/clocks.h>
#include <hardware/vreg.h>
#include <pico/stdlib.h>

// The boost that rate is tested at, which only the board that sets it needs.
#define SYS_RP2350_VREG VREG_VOLTAGE_1_15

/*****************************/
/* This is the OS scheduler. */
/*****************************/

bool sys_break(void)
{
    proc_cancel_launcher();
    sys_break_request();
    return true;
}

bool sys_break_to_launcher(void)
{
    // From the launcher there is nowhere to return to.
    if (proc_is_launcher())
        return false;
    api_set_ax(0xFFFF);
    sys_break_request();
    return true;
}

int main(void)
{
    /* Ahead of the drivers rather than first among them: everything derived
     * from this clock -- the UART's baud, the PIO dividers, the radio's band --
     * is set up by a driver, so it cannot itself be one. */
    vreg_set_voltage(SYS_RP2350_VREG);
    set_sys_clock_khz(SYS_RP2350_KHZ, true);
    sys_init();
    while (true)
    {
        sys_task();
        sys_io_task();
        sys_commit();
    }
}
