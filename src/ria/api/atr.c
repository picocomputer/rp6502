/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/atr.h"
#include "api/oem.h"
#include "api/pro.h"
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
#define ATR_RLN_LENGTH 0x03
#define ATR_LRAND 0x04
#define ATR_BEL 0x05
#define ATR_LAUNCHER 0x06
#define ATR_EXIT_CODE 0x07

// long ria_get_attr(uint8_t attr_id);
bool atr_api_get(void)
{
    switch (API_A)
    {
    case ATR_ERRNO_OPT:
        return api_return_axsreg(api_get_errno_opt());
    case ATR_PHI2_KHZ:
        return api_return_axsreg(cpu_get_phi2_khz_run());
    case ATR_CODE_PAGE:
        return api_return_axsreg(oem_get_code_page_run());
    case ATR_RLN_LENGTH:
        return api_return_axsreg(rln_get_max_length());
    case ATR_LRAND:
        return api_return_axsreg(get_rand_64() & 0x7FFFFFFF);
    case ATR_BEL:
        return api_return_axsreg(com_get_bel());
    case ATR_LAUNCHER:
        return api_return_axsreg(pro_has_launcher());
    case ATR_EXIT_CODE:
        return api_return_axsreg((uint16_t)pro_get_exit_code());
    default:
        return api_return_errno(API_EINVAL);
    }
}

// int ria_set_attr(uint32_t attr, uint8_t attr_id);
bool atr_api_set(void)
{
    uint32_t value;
    if (!api_pop_uint32_end(&value))
        return api_return_errno(API_EINVAL);
    if (value > 0x7FFFFFFF)
        return api_return_errno(API_EINVAL);
    switch (API_A)
    {
    case ATR_ERRNO_OPT:
        if (value > UINT8_MAX || !api_set_errno_opt((uint8_t)value))
            return api_return_errno(API_EINVAL);
        break;
    case ATR_PHI2_KHZ:
        if (value > UINT16_MAX)
            return api_return_errno(API_EINVAL);
        cpu_set_phi2_khz_run((uint16_t)value);
        break;
    case ATR_CODE_PAGE:
        if (value > UINT16_MAX)
            return api_return_errno(API_EINVAL);
        oem_set_code_page_run((uint16_t)value);
        break;
    case ATR_RLN_LENGTH:
        if (value > UINT8_MAX)
            return api_return_errno(API_EINVAL);
        rln_set_max_length((uint8_t)value);
        break;
    case ATR_BEL:
        if (value > 1)
            return api_return_errno(API_EINVAL);
        com_set_bel(value);
        break;
    case ATR_LAUNCHER:
        if (value > 1)
            return api_return_errno(API_EINVAL);
        pro_set_launcher(value);
        break;
    case ATR_LRAND:     // Read only
    case ATR_EXIT_CODE: // Read only
    default:
        return api_return_errno(API_EINVAL);
    }
    return api_return_ax(0);
}

/*
 * Legacy single-purpose handlers (opcodes 0x02-0x06).
 * Still dispatched from main.c; also reachable via the unified attribute API.
 */

// int phi2(void) - set/get CPU clock
bool atr_api_phi2(void)
{
    return api_return_ax(cpu_get_phi2_khz_run());
}

// int codepage(unsigned cp) - set/get OEM code page
bool atr_api_code_page(void)
{
    uint16_t cp = API_AX;
    if (cp)
        oem_set_code_page_run(cp);
    return api_return_ax(oem_get_code_page_run());
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
