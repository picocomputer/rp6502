/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RTL_SW_SHIM_PICO_STDIO_H_
#define _RTL_SW_SHIM_PICO_STDIO_H_

#include "ria/sys/com.h"
#include <stdint.h>

#define PICO_ERROR_TIMEOUT (-1)

static inline int stdio_getchar_timeout_us(uint32_t timeout_us)
{
    (void)timeout_us;
    com_source_t src = COM_SOURCE_ANY;
    return com_getchar(&src);
}

#endif
