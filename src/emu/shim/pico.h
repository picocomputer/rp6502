/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for <pico.h>: the flash-placement attributes (no-ops on the host) and
 * <assert.h> (the real pico.h pulls C11 static_assert). Included by the vendored
 * FatFs ffunicode.c and by the reused firmware ria/api/dir.c.
 */

#ifndef _EMU_SHIM_PICO_H_
#define _EMU_SHIM_PICO_H_

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define __in_flash(...)
#define __not_in_flash(...)
#define __not_in_flash_func(func) func
#define __in_flash_func(func) func

#endif /* _EMU_SHIM_PICO_H_ */
