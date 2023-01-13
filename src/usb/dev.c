/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "dev.h"
#include "tusb.h"
#include <stdarg.h>

#define MAX_DEV_DESC_LEN 80
char message[CFG_TUH_DEVICE_MAX][MAX_DEV_DESC_LEN];

void dev_print_all()
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < CFG_TUH_DEVICE_MAX; i++)
        if (tuh_mounted(i + 1))
            count++;

    printf("USB : %d device%s\n", count, count == 1 ? "" : "s");
    for (uint8_t i = 0; i < CFG_TUH_DEVICE_MAX; i++)
        if (tuh_mounted(i + 1))
            puts(message[i]);
}

void dev_printf(uint8_t dev_addr, const char *format, ...)
{
    assert(dev_addr > 0 && dev_addr <= CFG_TUH_DEVICE_MAX);
    va_list argptr;
    va_start(argptr, format);
    sprintf(message[dev_addr - 1], "%d: ", dev_addr);
    vsprintf(message[dev_addr - 1] + 3, format, argptr);
    assert(strlen(message[dev_addr - 1]) < MAX_DEV_DESC_LEN);
}
