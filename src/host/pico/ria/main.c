/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sys.h"
#include "drivers.h"
#include "core/api/proc.h"

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
    // Stop as exit() does, so proc_stop can queue the launcher.
    // A break would run rom_break after it in the same sys_commit,
    // and ROM_IDLE would cancel that load.
    sys_stop();
    return true;
}

int main(void)
{
    sys_init();
    while (true)
    {
        sys_task();
        sys_io_task();
        sys_commit();
    }
}
