/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RTL_SW_SHIM_PICO_STDIO_DRIVER_H_
#define _RTL_SW_SHIM_PICO_STDIO_DRIVER_H_

#include <stdbool.h>

typedef struct stdio_driver
{
    void (*out_chars)(const char *buf, int len);
    void (*out_flush)(void);
    int (*in_chars)(char *buf, int len);
    bool crlf_enabled;
} stdio_driver_t;

void com_set_term_out(void (*out_chars)(const char *buf, int len));

static inline void stdio_set_driver_enabled(stdio_driver_t *driver, bool enabled)
{
    com_set_term_out(enabled && driver ? driver->out_chars : 0);
}

#endif
