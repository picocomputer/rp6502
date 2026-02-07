/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/oem.h"
#include "hid/kbd.h"
#include "mon/mon.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "sys/pix.h"
#include <fatfs/ff.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_OEM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Only the code page specified by RP6502_CODE_PAGE is installed to flash.
// To include all code pages, set RP6502_CODE_PAGE to 0 (CMmakeLists.txt).
// This is the default for when RP6502_CODE_PAGE == 0.
#define OEM_DEFAULT_CODE_PAGE 437

uint16_t oem_code_page_setting;
uint16_t oem_code_page;

static void oem_request_code_page(uint16_t cp)
{
    uint16_t old_code_page = oem_code_page;
#if RP6502_CODE_PAGE
    (void)cp;
    oem_code_page = RP6502_CODE_PAGE;
#else
    if (f_setcp(cp) == FR_OK)
        oem_code_page = cp;
    else if (oem_code_page == 0)
    {
        if (f_setcp(OEM_DEFAULT_CODE_PAGE) != FR_OK)
            mon_add_response_str(STR_ERR_INTERNAL_ERROR);
        oem_code_page = OEM_DEFAULT_CODE_PAGE;
    }
#endif
    if (old_code_page != oem_code_page)
    {
        pix_send_blocking(PIX_DEVICE_VGA, 0xFu, 0x01u, oem_code_page);
        kbd_rebuild_code_page_cache();
    }
}

void oem_init(void)
{
    if (!oem_code_page)
    {
        oem_request_code_page(OEM_DEFAULT_CODE_PAGE);
        oem_code_page_setting = oem_code_page;
    }
}

void oem_stop(void)
{
    if (oem_code_page != oem_code_page_setting)
        oem_request_code_page(oem_code_page_setting);
}

void oem_set_code_page_ephemeral(uint16_t cp)
{
    oem_request_code_page(cp);
}

bool oem_set_code_page(uint32_t cp)
{
    oem_request_code_page(cp);
    if (cp != oem_code_page)
        return false;
    if (oem_code_page_setting != oem_code_page)
    {
        oem_code_page_setting = oem_code_page;
        cfg_save();
    }
    return true;
}

uint16_t oem_get_code_page(void)
{
    return oem_code_page;
}

void oem_load_code_page(const char *str, size_t len)
{
    uint16_t cp;
    str_parse_uint16(&str, &len, &cp);
    oem_request_code_page(cp);
    oem_code_page_setting = oem_code_page;
}
