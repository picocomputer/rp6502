/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/oem.h"
#include "hid/kbd.h"
#include "sys/cfg.h"
#include "sys/pix.h"
#include <fatfs/ff.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_OEM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

uint16_t oem_code_page;

void oem_init(void)
{
    cfg_set_code_page(cfg_get_code_page());
}

static uint16_t oem_find_code_page(uint16_t cp)
{
#if RP6502_CODE_PAGE
    (void)cp;
    return RP6502_CODE_PAGE;
#else
    FRESULT result;
    if (cp)
    {
        result = f_setcp(cp);
        if (result == FR_OK)
            return cp;
    }
    uint16_t cfg_code_page = cfg_get_code_page();
    if (cfg_code_page)
    {
        result = f_setcp(cfg_code_page);
        if (result == FR_OK)
            return cfg_code_page;
    }
    f_setcp(OEM_DEFAULT_CODE_PAGE);
    return OEM_DEFAULT_CODE_PAGE;
#endif
}

uint16_t oem_set_code_page(uint16_t cp)
{
    oem_code_page = oem_find_code_page(cp);
    pix_send_blocking(PIX_DEVICE_VGA, 0xFu, 0x01u, oem_code_page);
    kbd_rebuild_code_page_cache();
    return oem_code_page;
}

uint16_t oem_get_code_page(void)
{
    return oem_code_page;
}

bool oem_api_code_page(void)
{
    uint16_t cp = API_AX;
    if (!cp)
        cp = cfg_get_code_page();
    return api_return_ax(oem_set_code_page(cp));
}

void oem_stop(void)
{
    oem_set_code_page(cfg_get_code_page());
}
