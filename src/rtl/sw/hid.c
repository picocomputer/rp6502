/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "font.h"

#include "ria/api/oem.h"
#include "ria/ble/ble.h"
#include "ria/usb/usb.h"

bool usb_boot_enumerating(void)
{
    return false;
}

void usb_set_hid_leds(uint8_t leds)
{
    (void)leds;
}

void ble_set_hid_leds(uint8_t leds)
{
    (void)leds;
}

uint16_t oem_get_code_page_run(void)
{
    return font_get_code_page();
}
