/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb/usb.h"
#include <tusb.h>

void usb_init(void)
{
    tusb_init();
}

void usb_task(void)
{
    tud_task();
}
