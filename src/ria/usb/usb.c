/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"
#include "usb/usb.h"
#include <stdarg.h>

#define DEV_DESC_SIZE 80
char dev_message[CFG_TUH_DEVICE_MAX][DEV_DESC_SIZE];

// TODO Devices can be "mounted" according to tuh_mounted()
//      but tinyusb doesn't notify us. This status module
//      will need to track fully-mounted status itself.
//      Also, look for a way to know when enumeration is
//      fully complete since that's really what we're
//      interested in here.

void usb_print_status()
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < CFG_TUH_DEVICE_MAX; i++)
        if (tuh_mounted(i + 1))
            count++;

    printf("USB: %d device%s\n", count, count == 1 ? "" : "s");
    for (uint8_t i = 0; i < CFG_TUH_DEVICE_MAX; i++)
        if (tuh_mounted(i + 1))
            puts(dev_message[i]);
}

void usb_set_status(uint8_t dev_addr, const char *format, ...)
{
    assert(dev_addr > 0 && dev_addr <= CFG_TUH_DEVICE_MAX);
    va_list argptr;
    va_start(argptr, format);
    sprintf(dev_message[dev_addr - 1], "%d: ", dev_addr);
    vsprintf(dev_message[dev_addr - 1] + 3, format, argptr);
    assert(strlen(dev_message[dev_addr - 1]) < DEV_DESC_SIZE);
}
