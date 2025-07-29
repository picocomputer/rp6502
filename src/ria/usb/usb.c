/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"
#include "usb/hid.h"
#include "usb/msc.h"
#include "usb/usb.h"

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_USB)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

void usb_init(void)
{
    tuh_init(TUH_OPT_RHPORT);
}

void usb_task(void)
{
    tuh_task();
}

void usb_print_status(void)
{
    printf("USB : ");
    hid_print_status();
    msc_print_status();
}
