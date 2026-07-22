/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for <pico/stdio/driver.h>. term.c registers itself as a stdio
 * driver; the emulator captures that driver so the RIA stdout syscall can
 * feed bytes straight into the terminal (collapsing the UART/PIX path).
 */

#ifndef _EMU_SHIM_PICO_STDIO_DRIVER_H_
#define _EMU_SHIM_PICO_STDIO_DRIVER_H_

#include "emu/sys/com.h"
#include <stdbool.h>

typedef struct stdio_driver
{
    void (*out_chars)(const char *buf, int len);
    void (*out_flush)(void);
    int (*in_chars)(char *buf, int len);
    bool crlf_enabled;
} stdio_driver_t;

/* term.c registers its driver here; capture its out_chars as com's terminal
 * wire (the analog of the firmware's UART/PIX fanout target). */
static inline void stdio_set_driver_enabled(stdio_driver_t *driver, bool enabled)
{
    com_set_term_out(enabled && driver ? driver->out_chars : NULL);
}

#endif /* _EMU_SHIM_PICO_STDIO_DRIVER_H_ */
