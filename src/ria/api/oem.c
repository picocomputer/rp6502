/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/oem.h"
#include "sys/cfg.h"
#include "fatfs/ff.h"

void oem_init(void)
{
    cfg_set_codepage(oem_set_codepage(cfg_get_codepage()));
}

uint16_t oem_set_codepage(uint16_t cp)
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
    f_setcp(437);
    return 437;
#endif
}

void oem_api_codepage()
{
    return api_return_ax(cfg_get_codepage());
}
