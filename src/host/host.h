/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/* What a machine says about itself, and what only a machine can answer.
 *
 * This is the other half of the seam osal/os.h describes. An operating system
 * answers os_*; a machine answers these, and every default below is what a
 * machine that means nothing by them gets. The two machines that do mean
 * something -- see host/pico/machine.h and host/pocket/machine.h -- say so
 * before this is parsed, which their own build arranges.
 */

#ifndef _HOST_HOST_H_
#define _HOST_HOST_H_

#include <stdbool.h>
#include <stdint.h>

/* ---- where a host puts things ---- */
/* A Pico has two memories and cares which one a byte lands in: cold paths
 * and tables belong in flash, an interrupt handler must not. A machine with
 * one memory ignores all of it, which is why these default to nothing and a
 * host that means something by them says so before including this. */
#ifndef HOST_IN_FLASH
#define HOST_IN_FLASH(group)
#endif
#ifndef HOST_NOT_IN_FLASH
#define HOST_NOT_IN_FLASH(group)
#endif
#ifndef HOST_UNINITIALIZED_RAM
#define HOST_UNINITIALIZED_RAM(name) name
#endif
#ifndef HOST_TIME_CRITICAL
#define HOST_TIME_CRITICAL(name) name
#endif
#ifndef HOST_ISR
#define HOST_ISR
#endif

/* The tallest terminal this machine's video can show, in rows of cells.
 * Only a device with an SXGA console reaches 32; every other target tops
 * out at 480 scanlines, where two more rows would be bought and never
 * shown -- and this sizes the largest thing term.c owns. */
#ifndef HOST_TERM_MAX_HEIGHT
#define HOST_TERM_MAX_HEIGHT 30
#endif

/* How long a path this machine keeps for the launcher chain. A host OS path
 * can be long; a Pico holds what its monitor accepts, and a soft CPU counting
 * static RAM holds less. Each machine that is not a host OS says so before
 * including this. */
#ifndef PROC_PATH_MAX
#define PROC_PATH_MAX 4096
#endif

/* Each console ring, in bytes; a power of two. Two of these exist, one for
 * what was typed and one for what the terminal answered, and neither is a
 * buffer anything waits on: a paste drips in behind com_keyboard_free and replies
 * arrive in bounded bursts. */
#ifndef COM_RING_SIZE
#define COM_RING_SIZE 256
#endif

/* The seed for the machine's stream (core/sys/random.h), asked once on first
 * use. Always a valid value: a fixture's fixed seed, a --seed, or whatever
 * os_random_seed answers when the machine has nothing better. There is no
 * sentinel -- a machine that does not care still says what it wants -- and it
 * must answer the same value every time, because a machine reads it for the
 * memory fill and for what it reports as well as for the stream. */
uint32_t host_random_seed(void);

/* ---- the machine's microsecond clock ---- */
/* Microseconds since the machine started: TIMER0 on a Pico, the run loop's own
 * counter in the emulator, the fabric's mtime on a Pocket. Machine time, not
 * the host's: it runs while the 6502 is halted, because a halted CPU is a CPU
 * fetching nothing rather than a stopped clock, and it does not follow the
 * host's wall clock -- an emulator paced against a display deliberately lets
 * the two drift. It is savestate state where a machine has savestates. Wall
 * time is tim_get_time.
 *
 * A machine answers it, but not always its own directory: for the three
 * emulated machines the cycle counter in core/wdc/cpu.c is the machine, so
 * that is where their answer is. Only the two with real hardware under them
 * have one of their own. */
uint64_t host_clock_us(void);

/* Deadlines, from the clock above. Inline rather than a translation unit on
 * five build lists for three adds. */
typedef uint64_t host_deadline_t;
static inline host_deadline_t host_deadline_us(uint64_t us) { return host_clock_us() + us; }
static inline host_deadline_t host_deadline_ms(uint64_t ms) { return host_clock_us() + ms * 1000; }
static inline bool host_deadline_passed(host_deadline_t d) { return host_clock_us() >= d; }

#endif /* _HOST_HOST_H_ */
