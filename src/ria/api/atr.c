/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/atr.h"
#include "api/oem.h"
#include "api/std.h"
#include "str/rln.h"
#include "sys/com.h"
#include "sys/cpu.h"
#include <pico/rand.h>
#include <stdio.h>
#include <string.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_ATR)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Attribute IDs
#define ATR_ERRNO_OPT 0x00
#define ATR_PHI2_KHZ 0x01
#define ATR_CODE_PAGE 0x02
#define ATR_MAX_LENGTH 0x03
#define ATR_LRAND 0x04

// long ria_get_attr(uint8_t attr_id);
bool atr_api_get(void)
{
    switch (API_A)
    {
    case ATR_ERRNO_OPT:
        return api_return_axsreg(api_get_errno_opt());
    case ATR_PHI2_KHZ:
        return api_return_axsreg(cpu_get_phi2_khz());
    case ATR_CODE_PAGE:
        return api_return_axsreg(oem_get_code_page());
    case ATR_MAX_LENGTH:
        return api_return_axsreg(rln_get_max_length());
    case ATR_LRAND:
        return api_return_axsreg(get_rand_64() & 0x7FFFFFFF);
    default:
        return api_return_errno(API_EINVAL);
    }
}

// int ria_set_attr(uint32_t attr, uint8_t attr_id);
bool atr_api_set(void)
{
    uint8_t attr_id = API_A;
    uint32_t value;
    if (!api_pop_uint32_end(&value))
        return api_return_errno(API_EINVAL);
    switch (attr_id)
    {
    case ATR_ERRNO_OPT:
        api_set_errno_opt((uint8_t)value);
        break;
    case ATR_PHI2_KHZ:
        // TODO ephemeral
        cpu_set_phi2_khz((uint16_t)value);
        break;
    case ATR_CODE_PAGE:
        oem_set_code_page_ephemeral((uint16_t)value);
        break;
    case ATR_MAX_LENGTH:
        rln_set_max_length((uint8_t)value);
        break;
    case ATR_LRAND: // Read only
    default:
        return api_return_errno(API_EINVAL);
    }
    return api_return_ax(0);
}

/*
 * Deprecated API functions - moved here from their original modules.
 * These are the old API op codes that are now accessible via attributes.
 */

// int phi2(unsigned khz) - set/get CPU clock
bool atr_api_phi2(void)
{
    uint16_t khz = API_AX;
    if (khz)
        cpu_set_phi2_khz(khz);
    return api_return_ax(cpu_get_phi2_khz());
}

// int codepage(unsigned cp) - set/get OEM code page
bool atr_api_code_page(void)
{
    uint16_t cp = API_AX;
    if (cp)
        oem_set_code_page_ephemeral(cp);
    return api_return_ax(oem_get_code_page());
}

// long lrand(void) - get random number
bool atr_api_lrand(void)
{
    return api_return_axsreg(get_rand_64() & 0x7FFFFFFF);
}

// int errno_opt(unsigned char opt) - set errno mapping
bool atr_api_errno_opt(void)
{
    uint8_t opt = API_A;
    if (!api_set_errno_opt(opt))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}
