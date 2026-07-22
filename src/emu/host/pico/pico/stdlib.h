/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for <pico/stdlib.h>: provides just enough of the Pico SDK
 * surface that the vendored vga/term/{term,font,color}.c sources need to
 * compile unmodified in the desktop emulator. Real Pico timing/flash
 * placement is collapsed onto a host virtual clock.
 */

#ifndef _EMU_SHIM_PICO_STDLIB_H_
#define _EMU_SHIM_PICO_STDLIB_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

// Pico base types/macros the SDK pulls in via pico/types.h and pico/platform.h.
typedef unsigned int uint;
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// Pico SDK memory-placement attributes are no-ops on the host.
#define __in_flash(...)
#define __not_in_flash(...)
#define __not_in_flash_func(func) func
#define __in_flash_func(func) func
#define __time_critical_func(func) func
#define __uninitialized_ram(name) name
#define __scratch_x(...)
#define __scratch_y(...)

// IRQ plumbing the audio drivers reference (the bodies run as a plain call on
// the host; __isr is a section attribute with no host meaning).
#define __isr
typedef void (*irq_handler_t)(void);

#include "pico/time.h"
#include "pico/stdio.h"

#endif /* _EMU_SHIM_PICO_STDLIB_H_ */
