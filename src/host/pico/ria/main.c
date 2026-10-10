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
    // With a launcher, stop as an exit does, as the pocket host does. A break
    // would run the break hooks after proc_stop has queued the relaunch, and
    // rom_break would set ROM_IDLE over the load rom_exec just started.
    if (proc_has_launcher())
        sys_stop();
    else
        sys_break_request();
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
