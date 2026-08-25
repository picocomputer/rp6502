/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the shared HID drivers reach for and this machine does not have.
 * The APF bus has no boot enumeration and no lock LEDs, and the font
 * asset is the only code page there is.
 */

#include "font.h"

#include "core/hid/hid.h"
#include "core/api/oem.h"

uint16_t oem_get_code_page_run(void)
{
    return font_get_code_page();
}

/* A page this machine does not carry leaves the one in force; the get that
 * follows says which that is. */
void oem_set_code_page_run(uint16_t cp)
{
    if (font_has_code_page(cp))
        font_set_code_page(cp);
}

/* The dock's controllers have no lamps and nothing enumerates. */
void hid_set_leds(uint8_t leds)
{
    (void)leds;
}

bool hid_boot_enumerating(void)
{
    return false;
}
