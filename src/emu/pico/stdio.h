/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for <pico/stdio.h>: emulates the pico_stdio surface for the
 * vendored/shared sources. Output takes the SDK-layer CRLF translation
 * (stdio.c here) into com's terminal wire; input drains the com module's
 * merged RX.
 */

#ifndef _EMU_SHIM_PICO_STDIO_H_
#define _EMU_SHIM_PICO_STDIO_H_

#include "emu/sys/com.h"
#include <stdint.h>

int stdio_putchar(int c);
int stdio_printf(const char *fmt, ...);

#define PICO_ERROR_TIMEOUT (-1)

/* Firmware stdin routing (std_tty_read). On the Pico, stdio_getchar drains
 * the com driver's merged RX; here the com module's rings are that merge. */
static inline int stdio_getchar_timeout_us(uint32_t timeout_us)
{
    (void)timeout_us;
    com_source_t src = COM_SOURCE_ANY;
    return com_getchar(&src);
}

#endif /* _EMU_SHIM_PICO_STDIO_H_ */
