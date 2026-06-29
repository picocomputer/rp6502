/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for <pico.h>. The vendored FatFs ffunicode.c (compiled for its
 * ff_uni2oem code-page tables) is the only emulator unit that includes it, and
 * only for the flash-placement attribute on those tables — a no-op on the host.
 */

#ifndef _EMU_SHIM_PICO_H_
#define _EMU_SHIM_PICO_H_

#include <stddef.h>
#include <stdint.h>

#define __in_flash(...)
#define __not_in_flash(...)
#define __not_in_flash_func(func) func
#define __in_flash_func(func) func

#endif /* _EMU_SHIM_PICO_H_ */
