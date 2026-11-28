/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/oem.h"
#include "core/str/unicode.h"
#include "core/term/vterm.h"
#include "core/vga/vga.h"

uint16_t oem_get_code_page_run(void)
{
    return vterm_get_code_page();
}

bool oem_set_code_page_run(uint16_t cp)
{
    if (!unicode_has_page(cp))
        return false;
    vga_set_code_page(cp);
    return true;
}

void oem_restore_code_page(void)
{
    oem_set_code_page_run(437);
}

void oem_locale_changed(uint16_t cp)
{
    oem_set_code_page_run(cp);
}
