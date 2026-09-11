/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The one generator every machine runs, written once so two machines given the
 * same seed answer with the same stream. Thirty-two bits because that is all
 * anyone reads: the 6502's lrand syscall masks the result to 31 bits.
 */

#ifndef _CORE_SYS_RANDOM_H_
#define _CORE_SYS_RANDOM_H_

#include "core/sys/driver.h"
#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>

uint32_t sys_random(void);

/* A stream of its own, for something that must not disturb the one above. The
 * memory fills use these, so a 64 KB wipe cannot move what a seeded program
 * sees. */
uint32_t sys_random_step(uint32_t *state);

/* len bytes of that stream, a word at a time. len must be a multiple of four,
 * because the last word is written whole. */
void sys_random_fill(void *dst, size_t len, uint32_t *state);

void sys_random_seed(void);

#define RANDOM_SST_SIZE 4
void random_sst_save(sst_cursor_t *c, unsigned flags);
bool random_sst_load(sst_cursor_t *c, unsigned flags);

#define RANDOM_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(RAND, 1, RANDOM_SST_SIZE, random_sst_save, random_sst_load))

#endif /* _CORE_SYS_RANDOM_H_ */
