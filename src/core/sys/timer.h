/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Real time, and distances in it.
 *
 * One clock, the operating system's, which does not stop: not when the 6502
 * stops, not at a breakpoint, not when a host falls behind its display. That
 * is what a network retry, a key repeat and a terminal's reply timeout are
 * all measuring, and none of them is the machine's business.
 *
 * Machine time is the other thing and lives in host/host.h -- host_clock_us,
 * which a machine answers for itself and which pauses with it. A blink is
 * neither: it keeps the screen's time, vga_frame_count().
 *
 * The trampoline is here so core reaches the OS through core. Nothing above
 * this includes osal/os.h to ask what time it is.
 */

#ifndef _CORE_SYS_TIMER_H_
#define _CORE_SYS_TIMER_H_

#include <stdbool.h>
#include <stdint.h>

/* Nanoseconds, monotonic, from whenever this OS counts. 64 bits is 584 years,
 * so the wrap is theoretical -- but the comparison below is written for it
 * anyway, because the one that was not is what this replaces. */
uint64_t timer_ns(void);

/* A moment to wait for. Zero is an ordinary value, not a sentinel: a caller
 * that means "no deadline" needs its own flag. */
typedef uint64_t timer_deadline_t;

timer_deadline_t timer_in_us(uint64_t us);
timer_deadline_t timer_in_ms(uint64_t ms);

/* True once the deadline has arrived. Signed difference rather than a
 * magnitude compare, so a wrap costs one late deadline instead of stranding
 * every armed one for a whole clock period. */
bool timer_passed(timer_deadline_t d);

#endif /* _CORE_SYS_TIMER_H_ */
