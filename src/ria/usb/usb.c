/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb/hid.h"
#include "usb/msc.h"
#include "usb/usb.h"
#include "usb/xin.h"
#include <tusb.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_USB)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

void usb_init(void)
{
    tuh_init(TUH_OPT_RHPORT);
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
}

void usb_task(void)
{
    tuh_task();
}

void usb_print_status(void)
{
    int count_gamepad = hid_pad_count() + xin_pad_count();
    printf("USB : ");
    hid_print_status();
    printf(", %d gamepad%s", count_gamepad, count_gamepad == 1 ? "" : "s");
    msc_print_status();
}
