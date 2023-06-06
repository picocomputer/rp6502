/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "sys/sys.h"
#include "hardware/watchdog.h"

void sys_mon_reboot(const char *args, size_t len)
{
    (void)(args);
    (void)(len);
    watchdog_reboot(0, 0, 0);
}

void sys_mon_reset(const char *args, size_t len)
{
    (void)(args);
    (void)(len);
    main_run();
}
