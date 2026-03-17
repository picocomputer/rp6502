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

/*
    THIS IS A WORK IN PROGRESS
    IT IS INCOMPLETE
*/

/*
 * Attribute System - dispatches get/set to actual data sources
 * -------------------------------------------------------------
 * Readline attributes (0x00-0x0F) - state lives in rln.c:
 *   0x01 ATR_SUPPRESS_END_MOVE    - Suppress cursor move to end after input (bool, default 0)
 *   0x02 ATR_SUPPRESS_NEWLINE     - Suppress newline after input (bool, default 0)
 *   0x03 ATR_ENABLE_HISTORY       - Enable input history (bool, default 0)
 *   0x04 ATR_MAX_LENGTH           - Readline length limit 0-255 (uint8, default 254)
 *   0x05 ATR_TIMEOUT              - Timeout in milliseconds (uint32, 0 = disabled)
 *   0x06 ATR_CTRL_BITS            - End readline on ctrl chars (uint32)
 *   0x07 ATR_END_CHAR             - Char that ended readline (uint8, read-only)
 *   0x08 ATR_TIMED_OUT            - True if readline timed out (bool, read-only)
 *   0x09 ATR_CURSOR_POS           - Cursor position (uint8, 0xFF = end of line)
 *
 * System attributes (0x80-0x8F) - deprecated API functions mirrored:
 *   0x80 ATR_PHI2_KHZ             - CPU clock in kHz (uint16, read-only via attr)
 *   0x81 ATR_CODE_PAGE            - OEM code page (uint16)
 *   0x82 ATR_LRAND                - Random number (uint32, read-only)
 *   0x83 ATR_ERRNO_OPT            - Errno option (uint8, read-only via attr)
 */

// Attribute IDs - readline (0x00-0x0F)
#define ATR_MAX_LENGTH 0x04

// Attribute IDs - system (0x80-0x8F) - mirrors deprecated APIs
#define ATR_PHI2_KHZ 0x80
#define ATR_CODE_PAGE 0x81
#define ATR_LRAND 0x82
#define ATR_ERRNO_OPT 0x66

// int ria_get_attr(uint32_t *attr, uint8_t attr_id);
bool atr_api_get(void)
{
    uint8_t attr_id = API_A;
    uint32_t value = 0;

    switch (attr_id)
    {
    // Readline attributes - dispatch to rln.c
    case ATR_MAX_LENGTH:
        value = rln_get_max_length();
        break;

    // System attributes - dispatch to respective modules
    case ATR_PHI2_KHZ:
        value = cpu_get_phi2_khz();
        break;
    case ATR_CODE_PAGE:
        value = oem_get_code_page();
        break;
    case ATR_LRAND:
        value = get_rand_32() & 0x7FFFFFFF;
        break;
    case ATR_ERRNO_OPT:
        value = api_get_errno_opt();
        break;

    default:
        return api_return_errno(API_EINVAL);
    }

    // Push value to xstack for return
    if (!api_push_uint32(&value))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
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
    // Readline attributes - dispatch to rln.c
    case ATR_MAX_LENGTH:
        rln_set_max_length((uint8_t)value);
        break;

    // System attributes
    case ATR_PHI2_KHZ:
        // TODO ephemeral
        cpu_set_phi2_khz((uint16_t)value);
        break;
    case ATR_CODE_PAGE:
        oem_set_code_page_ephemeral((uint16_t)value);
        break;
    case ATR_LRAND:
        // Read-only, ignore silently
        break;
    case ATR_ERRNO_OPT:
        api_set_errno_opt((uint8_t)value);
        break;

    default:
        return api_return_errno(API_EINVAL);
    }

    return api_return_ax(0);
}

/*
 * Deprecated API functions - moved here from their original modules.
 * These are the old API op codes that are now also accessible via attributes.
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
    return api_return_axsreg(get_rand_32() & 0x7FFFFFFF);
}

// int errno_opt(unsigned char opt) - set errno mapping
bool atr_api_errno_opt(void)
{
    uint8_t opt = API_A;
    if (!api_set_errno_opt(opt))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

// stdin_opt is garbage. it's time to make better controls
// for the custom OS readline-like system on stdin/stdout.
// It should not be tcgetattr/tcsetattr. Do something best for this.
// Do not delete these requirements, convert into terse comment documentation that will be expanded later.

// Need to set these, typically only once to init, get optional
//-----------
// bool to disable newline expansion. default off
// bool to supress moving to end of line after input. default off
// bool to supress newline after input. default off
// bool to enable input history. default off
// Limit readline length 0-255 (uint8), default 254
// Timeout in 6.2 seconds (uint8).
// End readline on ctrl_bits  (uint32)

// These must have get, setting would be ignored
// ---------
// ctrl_bits char that ended previous readline (uint8). always 10(\r) if ctrl_bits==0
// bool if previous readline timed out

// These must have get/set, typically used with buffer sets and gets
// ---------
// Cursor position (uint8)

// ria_set_readline is the opposite of get which is already implemented as from read_* functions.
// buffer (uint8[256])

// Setting ria_set_readline tells readline to continue editing as if the text
// was already displayed and the term cursor placed according to "supress moving to end of line".
// Meaning the term cursor will be moved from the end of line to Cursor position if not supressed.

// Invalid cursor position is moved to end of line. e.g. 0xff always means end of line

// Do not change com.c, use putchar_raw in std_out_write to bypass newline expansion

// These deprecated APIs are now implemented above as atr_api_*:
// - atr_api_phi2() was cpu_api_phi2()
// - atr_api_code_page() was oem_api_code_page()
// - atr_api_lrand() was rng_api_lrand()
// - atr_api_stdin_opt() was std_api_stdin_opt()
// - atr_api_errno_opt() was api_api_errno_opt()
