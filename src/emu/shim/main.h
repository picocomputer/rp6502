/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for ria/main.h. The emulator has no firmware main loop: ops
 * dispatch synchronously from the $FFEF write (emu/sys/ria.c), so the shared
 * api.c's task latch never fires and its dispatcher is a no-op, like the pix
 * sends in sys/pix.h.
 */

#ifndef _EMU_SHIM_MAIN_H_
#define _EMU_SHIM_MAIN_H_

#include <stdint.h>
#include <stdbool.h>

static inline bool main_api(uint8_t operation)
{
    (void)operation;
    return false;
}

#endif /* _EMU_SHIM_MAIN_H_ */
