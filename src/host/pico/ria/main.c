/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sys.h"
#include "drivers.h"
#include "machine.h"

/*****************************/
/* This is the OS scheduler. */
/*****************************/

int HOST_IN_RAM_FUNC main(void)
{
    sys_init();
    while (true)
    {
        sys_task();
        sys_commit();
    }
}
