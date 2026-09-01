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

#include <stdint.h>

/* The machine's stream, the one the 6502 reads. Seeded on first use by asking
 * the machine (host_random_seed), which is where a fixture or a --seed gets
 * its say. */
uint32_t sys_random(void);

/* A stream of one's own, for something that must not disturb the above. The
 * state is the caller's; core/mem/mem.c keeps one so a 128 KB wipe cannot move
 * what a seeded program sees. */
uint32_t sys_random_step(uint32_t *state);

#endif /* _CORE_SYS_RANDOM_H_ */
