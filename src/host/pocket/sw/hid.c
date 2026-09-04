/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the shared HID drivers reach for and this machine does not have.
 * The APF bus has no boot enumeration and no lock LEDs, the font asset is
 * the only code page there is, and the layout database lives behind a
 * staging window rather than in memory a pointer can address.
 */

#include "font.h"
#include "mmio.h"

#include "core/hid/hid.h"
#include "core/hid/layout.h"
#include "apf.h"
#include "core/str/oem.h"

/* The keyboard layouts out of the staging store, one word at a time. Same
 * trade as uni.c: twenty kilobytes has no room in the TCM, and the window
 * cannot fetch anything wider than a byte, so every access comes here. */
uint16_t layout_word(uint32_t index)
{
    uint32_t at = index * 2;
    return (uint16_t)KBDLAY[at] | ((uint16_t)KBDLAY[at + 1] << 8);
}

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

/* The locale picks a default code page, and on this machine the code page IS
 * the font's. There is no override to resolve against -- one locale, no SET --
 * so the locale's choice simply takes effect. Defined because core/str/str.c
 * calls it from str_apply_locale; without it that chain is unlinkable and only
 * --gc-sections has been hiding it. */
void oem_locale_changed(uint16_t cp)
{
    oem_set_code_page_run(cp);
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

/* The dock sends a report only when something moves, and the registers it
 * fills are levels, so a control standing still would leave the blank the
 * mapping just wrote. */
void hid_remapped(void)
{
    apf_refresh();
}
