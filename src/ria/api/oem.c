/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/oem.h"
#include "hid/kbd.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "sys/vga.h"
#include <fatfs/ff.h>
#include <pico.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_OEM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static uint16_t oem_code_page_set;
static uint16_t oem_code_page_run;
static uint16_t oem_auto_cp; // Locale default code page (0 until a locale is applied)

// Resolve the code page to apply: the override if set, else the locale auto.
static uint16_t oem_resolve(void)
{
    return oem_code_page_set ? oem_code_page_set : oem_auto_cp;
}

static void oem_request_code_page(uint16_t cp)
{
    uint16_t old_code_page = oem_code_page_run;
    // cp >= 900 are DBCS; allow SBCS only
    if (cp < 900 && f_setcp(cp) == FR_OK)
        oem_code_page_run = cp;
    if (old_code_page != oem_code_page_run)
    {
        vga_set_code_page(oem_code_page_run);
        kbd_rebuild_code_page_cache();
    }
}

void __in_flash("oem_init") oem_init(void)
{
    // Nothing loaded from config (no CONFIG.SYS): default to auto.
    if (!oem_code_page_run)
    {
        oem_code_page_set = 0;
        oem_request_code_page(oem_resolve());
    }
}

void oem_stop(void)
{
    if (oem_code_page_run != oem_resolve())
        oem_request_code_page(oem_resolve());
}

void oem_set_code_page_run(uint16_t cp)
{
    oem_request_code_page(cp);
}

bool oem_set_code_page(uint32_t cp)
{
    if (cp > UINT16_MAX)
        return false;
    if (cp == 0)
    {
        // Auto: track the locale's default code page.
        if (oem_code_page_set != 0)
        {
            oem_code_page_set = 0;
            oem_request_code_page(oem_resolve());
            cfg_save();
        }
        return true;
    }
    oem_request_code_page(cp);
    if (cp != oem_code_page_run)
        return false;
    if (oem_code_page_set != cp)
    {
        oem_code_page_set = cp;
        cfg_save();
    }
    return true;
}

uint16_t oem_get_code_page(void)
{
    return oem_code_page_set;
}

bool oem_is_auto(void)
{
    return oem_code_page_set == 0;
}

uint16_t oem_get_code_page_run(void)
{
    return oem_code_page_run;
}

void oem_locale_changed(uint16_t cp)
{
    oem_auto_cp = cp;
    if (oem_code_page_set == 0)
        oem_request_code_page(oem_resolve());
}

void oem_load_code_page(const char *str)
{
    uint16_t cp;
    if (!str_parse_uint16(&str, &cp))
        return;
    oem_code_page_set = cp; // 0 = auto; legacy non-zero = hard override
    oem_request_code_page(oem_resolve());
}
