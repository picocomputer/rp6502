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
uint64_t emu_now_us(void);

typedef uint64_t absolute_time_t;

static inline absolute_time_t make_timeout_time_us(int64_t us)
{
    return emu_now_us() + (us < 0 ? 0 : (uint64_t)us);
}

static inline absolute_time_t make_timeout_time_ms(int64_t ms)
{
    return emu_now_us() + (ms < 0 ? 0 : (uint64_t)ms * 1000);
}

static inline bool time_reached(absolute_time_t t)
{
    return emu_now_us() >= t;
}

/* Firmware stdout routing. On the Pico, putchar/printf reach the configured
 * stdio driver (the terminal). Here we send them to the same sink the 6502's
 * stdout syscall uses, so a reused firmware source's echo and ANSI handshake
 * land in term.c rather than the developer's real stdout. */
#include <stdio.h>

int com_term_putchar(int c);
int com_term_printf(const char *fmt, ...);

#undef putchar
#undef printf
#define putchar(c) com_term_putchar(c)
#define printf com_term_printf

/* Firmware stdin routing (std_tty_read). On the Pico, stdio_getchar drains
 * the com driver's merged RX; here the com module's rings are that merge. */
#include "emu/sys/com.h"

#define PICO_ERROR_TIMEOUT (-1)

static inline int stdio_getchar_timeout_us(uint32_t timeout_us)
{
    (void)timeout_us;
    com_source_t src = COM_SOURCE_ANY;
    return com_getchar(&src);
}

#endif /* _EMU_SHIM_PICO_STDLIB_H_ */
