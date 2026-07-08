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

// Monotonic microsecond virtual clock derived from the master clock.
uint64_t cpu_now_us(void);

typedef uint64_t absolute_time_t;

static inline absolute_time_t make_timeout_time_us(int64_t us)
{
    return cpu_now_us() + (us < 0 ? 0 : (uint64_t)us);
}

static inline absolute_time_t make_timeout_time_ms(int64_t ms)
{
    return cpu_now_us() + (ms < 0 ? 0 : (uint64_t)ms * 1000);
}

static inline bool time_reached(absolute_time_t t)
{
    return cpu_now_us() >= t;
}

/* Firmware stdout/stdin routing. On the Pico, putchar/printf reach the
 * configured stdio driver (the terminal) through the pico_stdio layer; the
 * shim's pico/stdio.c is that layer here, so a reused firmware source's echo
 * and ANSI handshake land in term.c rather than the developer's real stdout. */
#include <stdio.h>
#include "pico/stdio.h"

#undef putchar
#undef printf
#define putchar(c) stdio_putchar(c)
#define printf stdio_printf

#endif /* _EMU_SHIM_PICO_STDLIB_H_ */
