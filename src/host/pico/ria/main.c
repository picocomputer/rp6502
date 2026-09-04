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
    sys_break_request();
    return true;
}

/* Everything is a device driver, and this is where they are notified of init,
 * task, run, stop and break. The walk comes from this machine's drivers.h; the
 * two task columns are pumped separately, because only one of them is safe to
 * call during blocking file IO. */
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
