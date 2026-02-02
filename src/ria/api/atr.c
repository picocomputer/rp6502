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

bool atr_api_get(void)
{
    return api_return_errno(API_ENOSYS);
}

bool atr_api_set(void)
{
    return api_return_errno(API_ENOSYS);
}

// TODO stdin_opt is garbage. it's time to make better controls
// for the custom OS readline-like system on stdin/stdout.
// It should not be tcgetattr/tcsetattr. Do something best for this.
// Do not delete these requirements, convert into terse comment documentation that will be expanded later.

// Need to set these, typically only once to init, get optional
//-----------
// bool to disable newline expansion
// bool to supress moving to end of line after input.
// bool to supress newline after input.
// First page of history (uint8).
// Qty of history (uint8).
// Limit readline length 0-255 (uint8), default 254
// Timeout in 6.2 seconds (uint8).
// End readline on ctrl_bits  (uint32)

// Need to set this regularly, get already implemented as from read_* functions.
// buffer (uint8[256])

// These must have get, setting would be ignored
// ---------
// Crtl char that ended previous readline (uint8).
// bool if previous readline timed out

// These must have get/set, typically used with buffer sets and gets
// ---------
// Cursor position (uint8)

// Setting the buffer tells readline to continue editing as if the text
// was already displayed and the term cursor placed according to "supress moving to end of line".
// Meaning the term cursor will be moved from the end of line to Cursor position if not supressed.

// Invalid cusrsor position is moved to end of line. e.g. 0xff always means end of line

// Do not change com.c, use putchar_raw in std_out_write to bypass newline expansion
// Do not maintain backwards compatibility with stdin_opt, ok to delete.
