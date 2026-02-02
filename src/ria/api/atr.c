/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/atr.h"

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_ATR)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

void atr_run(void)
{
}

// int ria_get_attr(uint32_t &attr, uint8_t attr_id);
bool atr_api_get(void)
{
    return api_return_errno(API_ENOSYS);
}

// int ria_set_attr(uint32_t attr, uint8_t attr_id);
bool atr_api_set(void)
{
    return api_return_errno(API_ENOSYS);
}

// int ria_set_readline(char *buf);
bool atr_api_set_readline(void)
{
    return api_return_errno(API_ENOSYS);
}

// TODO stdin_opt is garbage. it's time to make better controls
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

// Invalid cusrsor position is moved to end of line. e.g. 0xff always means end of line

// Do not change com.c, use putchar_raw in std_out_write to bypass newline expansion

// These will be deprecated, don't delete, but we will mirror their funtionality in attributes
// return cpu_api_phi2();
// return oem_api_code_page();
// return rng_api_lrand();
// return std_api_stdin_opt();
// return api_api_errno_opt();
