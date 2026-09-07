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

#include "core/sys/driver.h"
#include "core/sys/sst.h"
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

/* The stream itself, which is the machine's and not the session's. A blob
 * taken before the first draw would otherwise carry an unseeded stream, and
 * the machine that loaded it would seed from its own entropy: two peers
 * handed the same blob would then disagree at the first rand(). So a save
 * seeds first, and seeding is the one thing it may change about the machine.
 *
 * Seeding is separate from drawing here for exactly that reason: sys_random
 * does both in one call, and a save that used it would advance the stream and
 * make two saves of one unchanged machine differ. */
void sys_random_seed(void);

#define RANDOM_SST_SIZE 4
void random_sst_save(sst_cursor_t *c, unsigned flags);
bool random_sst_load(sst_cursor_t *c, unsigned flags);

#define RANDOM_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(RAND, 1, RANDOM_SST_SIZE, random_sst_save, random_sst_load))

#endif /* _CORE_SYS_RANDOM_H_ */
