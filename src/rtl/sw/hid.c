/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the shared HID drivers reach for and this machine does not have.
 *
 * kbd.c speaks to the transports that carry a keyboard: it asks USB
 * whether an enumeration is the boot one, so a vendor quirk applies
 * only then, and it pushes the lock LEDs back out to whoever has a
 * light to light. The Pocket's keyboard arrives over the APF bus, which
 * has neither, so the answers are no and nothing — and the transport
 * that does exist stays a transport rather than growing a special case
 * inside the driver.
 *
 * The code page is the other one. The RIA's oem.c owns a persisted
 * page, an automatic one and a running one; here the font asset is the
 * only page there is, so the question has one answer.
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
