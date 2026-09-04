/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The one generator every machine runs: an LCG step feeding a 32-bit
 * finalizer. Written once, so two machines given the same seed answer with the
 * same stream -- which is what lets a test pin an oracle across them.
 *
 * Thirty-two bits because that is all anyone reads: the 6502's rand() syscall
 * masks to 31, and a soft CPU counting its 96 KB should not run 64-bit
 * multiplies to throw half the result away.
 */

#ifndef _CORE_SYS_RANDOM_H_
#define _CORE_SYS_RANDOM_H_

#include <stddef.h>
#include <stdint.h>

/* The machine's stream, the one the 6502 reads. Seeded on first use by asking
 * the machine (host_seed), which is where a fixture or a --seed gets
 * its say. */
uint32_t sys_random(void);

/* A stream of one's own, for something that must not disturb the above. The
 * state is the caller's; the memory fills each keep one so a 64 KB wipe cannot
 * move what a seeded program sees. */
uint32_t sys_random_step(uint32_t *state);

/* len bytes of that stream, a word at a time; len is a multiple of four. */
void sys_random_fill(void *dst, size_t len, uint32_t *state);

#endif /* _CORE_SYS_RANDOM_H_ */
