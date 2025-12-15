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

uint16_t oem_code_page;

void oem_init(void)
{
    cfg_set_code_page(cfg_get_code_page());
}

uint16_t oem_set_code_page(uint16_t cp)
{
#if RP6502_CODE_PAGE
    (void)cp;
    oem_code_page = RP6502_CODE_PAGE;
#else
    if (f_setcp(cp) == FR_OK)
        oem_code_page = cp;
    else
    {
        uint16_t cfg_code_page = cfg_get_code_page();
        if (f_setcp(cfg_code_page) == FR_OK)
            oem_code_page = cfg_code_page;
        else
        {
            oem_code_page = OEM_DEFAULT_CODE_PAGE;
            if (f_setcp(OEM_DEFAULT_CODE_PAGE) != FR_OK)
                mon_add_response_str(STR_ERR_INTERNAL_ERROR);
        }
    }
#endif
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
