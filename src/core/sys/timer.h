/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Deadlines on two clocks: the operating system's real time, and the
 * machine's own, which a software machine derives from its video beam.
 */

#ifndef _CORE_SYS_TIMER_H_
#define _CORE_SYS_TIMER_H_

#include <stdbool.h>
#include <stdint.h>

/* Monotonic nanoseconds from an origin only the OS knows, so only a
 * difference of two readings means anything. */
uint64_t timer_ns(void);

typedef uint64_t timer_deadline_t;

timer_deadline_t timer_in_us(uint64_t us);
timer_deadline_t timer_in_ms(uint64_t ms);

/* True once the deadline has arrived. The comparison is a signed difference
 * rather than a magnitude compare, so a wrap costs one late deadline instead
 * of stranding every armed one for a whole clock period. At 64 bits of
 * nanoseconds that wrap is 584 years away. */
bool timer_passed(timer_deadline_t d);

/* The machine's own clock, in microseconds. host_clock_us is real time on a
 * board, but a software machine counts scanlines with it: it drifts from real
 * time whenever the host cannot hold the frame rate, a debugger holding the
 * beam holds it, and a savestate carries it, so a deadline measured against it
 * means the same thing in a machine restored from a savestate as it did in the
 * machine that made it. Every deadline the 6502 can observe belongs here; a
 * network retry or a key repeat belongs on the clock above, which none of that
 * touches. */
typedef uint64_t timer_mach_t;

timer_mach_t timer_mach_in_us(uint64_t us);
timer_mach_t timer_mach_in_ms(uint64_t ms);
bool timer_mach_passed(timer_mach_t d);

#endif /* _CORE_SYS_TIMER_H_ */
