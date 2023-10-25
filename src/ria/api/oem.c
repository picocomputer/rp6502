/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/oem.h"
#include "sys/cfg.h"
#include "sys/pix.h"
#include "fatfs/ff.h"

// Only the code page specified by RP6502_CODE_PAGE is installed to flash.
// To include all code pages, set RP6502_CODE_PAGE to 0 (CMmakeLists.txt).
// This is the default for when RP6502_CODE_PAGE == 0.
#define DEFAULT_CODE_PAGE 850

void oem_init(void)
{
    cfg_set_codepage(oem_set_codepage(cfg_get_codepage()));
}

static uint16_t oem_find_codepage(uint16_t cp)
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
    uint16_t cfg_code_page = cfg_get_codepage();
    if (cfg_code_page)
    {
        result = f_setcp(cfg_code_page);
        if (result == FR_OK)
            return cfg_code_page;
    }
    f_setcp(DEFAULT_CODE_PAGE);
    return DEFAULT_CODE_PAGE;
#endif
}

uint16_t oem_set_codepage(uint16_t cp)
{
    cp = oem_find_codepage(cp);
    pix_send_blocking(PIX_DEVICE_VGA, 0xFu, 0x01u, cp);
    return cp;
}

void oem_api_codepage(void)
{
    return api_return_ax(cfg_get_codepage());
}
