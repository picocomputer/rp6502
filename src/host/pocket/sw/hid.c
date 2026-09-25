/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "font.h"
#include "mmio.h"

#include "core/hid/hid.h"
#include "core/hid/layout.h"
#include "apf.h"
#include "core/str/oem.h"

/* A read of the staging window returns one byte, repeated on all four
 * lanes, so a layout word is built from two byte reads. */
uint16_t layout_word(uint32_t index)
{
    uint32_t at = index * 2;
    return (uint16_t)KBDLAY[at] | ((uint16_t)KBDLAY[at + 1] << 8);
}

uint16_t oem_get_code_page_run(void)
{
    return font_get_code_page();
}

/* The locale's code page, which is selected in place of a requested page
 * that the font does not have. */
static uint16_t oem_system_cp;

void oem_set_code_page_run(uint16_t cp)
{
    font_set_code_page(font_has_code_page(cp) ? cp : oem_system_cp);
}

void oem_locale_changed(uint16_t cp)
{
    oem_system_cp = cp;
    oem_set_code_page_run(cp);
}

void hid_set_leds(uint8_t leds)
{
    (void)leds;
}

bool hid_boot_enumerating(void)
{
    return false;
}

void hid_remapped(void)
{
    apf_refresh();
}
