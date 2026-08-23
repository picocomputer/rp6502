/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Soft-CPU shim for <pico/stdio.h>, shadowing the emulator's shim via
 * include-path precedence. On the Pico, stdio_getchar drains the com
 * driver's merged RX; here com.c's rings are that merge.
 */

#ifndef _RTL_SW_SHIM_PICO_STDIO_H_
#define _RTL_SW_SHIM_PICO_STDIO_H_

#include "core/com.h"
#include <stdint.h>

#define PICO_ERROR_TIMEOUT (-1)

static inline int stdio_getchar_timeout_us(uint32_t timeout_us)
{
    (void)timeout_us;
    com_source_t src = COM_SOURCE_ANY;
    return com_getchar(&src);
}

#endif /* _RTL_SW_SHIM_PICO_STDIO_H_ */
