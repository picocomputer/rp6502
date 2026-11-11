/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/ria.h"
#include "core/aud/mix.h"
#include "core/api/api.h"
#include "core/api/attr.h"
#include "core/api/clk.h"
#include "core/str/oem.h"
#include "core/api/proc.h"
#include "core/api/std.h"
#include "core/com/rln.h"
#include "core/com/tio.h"
#include "core/com/com.h"
#include "core/wdc/phi2.h"
#include "core/sys/driver.h"
#include "core/sys/random.h"
#include "machine.h"
#include <string.h>

#define ATTR_ERRNO_OPT 0x00
#define ATTR_PHI2_KHZ 0x01
#define ATTR_CODE_PAGE 0x02
#define ATTR_RLN_LENGTH 0x03
#define ATTR_LRAND 0x04
#define ATTR_BEL 0x05
#define ATTR_LAUNCHER 0x06
#define ATTR_EXIT_CODE 0x07
#define ATTR_SIGINT 0x08
#define ATTR_RLN_CAPS 0x09
#define ATTR_RLN_WIDTH 0x0A
#define ATTR_RLN_HEIGHT 0x0B
#define ATTR_RLN_SUPPRESS_NL 0x0C
#define ATTR_PCM_UNDERRUNS 0x0E
#define ATTR_OPL_OVERRUNS 0x0F
#define ATTR_CLK_RUN_MS 0x10
#define ATTR_CLK_RUN_CS 0x11
#define ATTR_CLK_RUN_DS 0x12
#define ATTR_CLK_RUN_S 0x13

/* One function per attribute's work, so each is placed on its own. A getter
 * returns the value and a setter returns false to refuse it. */

static uint32_t HOST_IN_FLASH_FUNC attr_get_errno_opt(void)
{
    return api_get_errno_opt();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_phi2_khz(void)
{
    return phi2_get_khz_run();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_code_page(void)
{
    return oem_get_code_page_run();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_rln_length(void)
{
    return rln_get_max_length();
}

static uint32_t HOST_IN_RAM_FUNC attr_get_lrand(void)
{
    return sys_random() & 0x7FFFFFFF;
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_bel(void)
{
    return com_get_bel();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_launcher(void)
{
    return proc_has_launcher();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_exit_code(void)
{
    return (uint16_t)proc_get_exit_code();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_sigint(void)
{
    return ria_get_sigint();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_rln_caps(void)
{
    return rln_get_caps();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_rln_width(void)
{
    return tio_get_term_width();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_rln_height(void)
{
    return tio_get_term_height();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_rln_suppress_nl(void)
{
    return rln_get_suppress_nl();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_pcm_underruns(void)
{
    return aud_pcm_underruns();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_opl_overruns(void)
{
    return aud_opl_overruns();
}

static uint32_t HOST_IN_FLASH_FUNC attr_get_clk_run(uint32_t us_per_tick)
{
    return clk_get_run(us_per_tick) & 0x7FFFFFFF;
}

static bool HOST_IN_FLASH_FUNC attr_set_errno_opt(uint32_t value)
{
    return value <= UINT8_MAX && api_set_errno_opt((uint8_t)value);
}

static bool HOST_IN_FLASH_FUNC attr_set_phi2_khz(uint32_t value)
{
    if (value < PHI2_MIN_KHZ || value > PHI2_MAX_KHZ)
        return false;
    phi2_set_khz_run((uint16_t)value);
    return true;
}

static bool HOST_IN_FLASH_FUNC attr_set_code_page(uint32_t value)
{
    return value <= UINT16_MAX && oem_set_code_page_run((uint16_t)value);
}

static bool HOST_IN_FLASH_FUNC attr_set_rln_length(uint32_t value)
{
    if (value > UINT8_MAX)
        return false;
    rln_set_max_length((uint8_t)value);
    return true;
}

static bool HOST_IN_FLASH_FUNC attr_set_bel(uint32_t value)
{
    if (value > 1)
        return false;
    com_set_bel(value);
    return true;
}

static bool HOST_IN_FLASH_FUNC attr_set_launcher(uint32_t value)
{
    if (value > 1)
        return false;
    proc_set_launcher(value);
    return true;
}

static bool HOST_IN_FLASH_FUNC attr_set_rln_caps(uint32_t value)
{
    if (value > 2)
        return false;
    rln_set_caps((uint8_t)value);
    return true;
}

static bool HOST_IN_FLASH_FUNC attr_set_rln_width(uint32_t value)
{
    if (value > UINT16_MAX)
        return false;
    tio_set_term_width((uint16_t)value);
    return true;
}

static bool HOST_IN_FLASH_FUNC attr_set_rln_height(uint32_t value)
{
    if (value > UINT16_MAX)
        return false;
    tio_set_term_height((uint16_t)value);
    return true;
}

static bool HOST_IN_FLASH_FUNC attr_set_rln_suppress_nl(uint32_t value)
{
    if (value > 1)
        return false;
    rln_set_suppress_nl((uint8_t)value);
    return true;
}

// long ria_attr_get(uint8_t attr_id);
bool HOST_IN_RAM_FUNC attr_api_get(void)
{
    switch (API_A)
    {
    case ATTR_ERRNO_OPT:
        return api_return_axsreg(attr_get_errno_opt());
    case ATTR_PHI2_KHZ:
        return api_return_axsreg(attr_get_phi2_khz());
    case ATTR_CODE_PAGE:
        return api_return_axsreg(attr_get_code_page());
    case ATTR_RLN_LENGTH:
        return api_return_axsreg(attr_get_rln_length());
    case ATTR_LRAND:
        return api_return_axsreg(attr_get_lrand());
    case ATTR_BEL:
        return api_return_axsreg(attr_get_bel());
    case ATTR_LAUNCHER:
        return api_return_axsreg(attr_get_launcher());
    case ATTR_EXIT_CODE:
        return api_return_axsreg(attr_get_exit_code());
    case ATTR_SIGINT:
        return api_return_axsreg(attr_get_sigint());
    case ATTR_RLN_CAPS:
        return api_return_axsreg(attr_get_rln_caps());
    case ATTR_RLN_WIDTH:
        return api_return_axsreg(attr_get_rln_width());
    case ATTR_RLN_HEIGHT:
        return api_return_axsreg(attr_get_rln_height());
    case ATTR_RLN_SUPPRESS_NL:
        return api_return_axsreg(attr_get_rln_suppress_nl());
    case ATTR_PCM_UNDERRUNS:
        return api_return_axsreg(attr_get_pcm_underruns());
    case ATTR_OPL_OVERRUNS:
        return api_return_axsreg(attr_get_opl_overruns());
    case ATTR_CLK_RUN_MS:
        return api_return_axsreg(attr_get_clk_run(1000));
    case ATTR_CLK_RUN_CS:
        return api_return_axsreg(attr_get_clk_run(10000));
    case ATTR_CLK_RUN_DS:
        return api_return_axsreg(attr_get_clk_run(100000));
    case ATTR_CLK_RUN_S:
        return api_return_axsreg(attr_get_clk_run(1000000));
    default:
        return api_return_errno(API_EINVAL);
    }
}

// int ria_attr_set(long val, unsigned char id);
bool HOST_IN_RAM_FUNC attr_api_set(void)
{
    uint32_t value;
    if (!api_pop_uint32_end(&value))
        return api_return_errno(API_EINVAL);
    if (value > 0x7FFFFFFF)
        return api_return_errno(API_EINVAL);
    bool ok;
    switch (API_A)
    {
    case ATTR_ERRNO_OPT:
        ok = attr_set_errno_opt(value);
        break;
    case ATTR_PHI2_KHZ:
        ok = attr_set_phi2_khz(value);
        break;
    case ATTR_CODE_PAGE:
        ok = attr_set_code_page(value);
        break;
    case ATTR_RLN_LENGTH:
        ok = attr_set_rln_length(value);
        break;
    case ATTR_BEL:
        ok = attr_set_bel(value);
        break;
    case ATTR_LAUNCHER:
        ok = attr_set_launcher(value);
        break;
    case ATTR_RLN_CAPS:
        ok = attr_set_rln_caps(value);
        break;
    case ATTR_RLN_WIDTH:
        ok = attr_set_rln_width(value);
        break;
    case ATTR_RLN_HEIGHT:
        ok = attr_set_rln_height(value);
        break;
    case ATTR_RLN_SUPPRESS_NL:
        ok = attr_set_rln_suppress_nl(value);
        break;
    case ATTR_LRAND:
    case ATTR_EXIT_CODE:
    case ATTR_SIGINT:
    case ATTR_PCM_UNDERRUNS:
    case ATTR_OPL_OVERRUNS:
    case ATTR_CLK_RUN_MS:
    case ATTR_CLK_RUN_CS:
    case ATTR_CLK_RUN_DS:
    case ATTR_CLK_RUN_S:
    default:
        ok = false;
        break;
    }
    if (!ok)
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

// int phi2(void)
bool HOST_IN_FLASH_FUNC attr_api_phi2(void)
{
    return api_return_ax(phi2_get_khz_run());
}

// int code_page(int cp)
bool HOST_IN_FLASH_FUNC attr_api_code_page(void)
{
    oem_set_code_page_run(API_AX);
    return api_return_ax(oem_get_code_page_run());
}

// long lrand(void)
bool HOST_IN_RAM_FUNC attr_api_lrand(void)
{
    return api_return_axsreg(attr_get_lrand());
}

// RIA_OP_ERRNO_OPT 0x06 is retained for binaries built with older SDKs.
bool HOST_IN_FLASH_FUNC attr_api_errno_opt(void)
{
    uint8_t opt = API_A;
    if (!api_set_errno_opt(opt))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}
