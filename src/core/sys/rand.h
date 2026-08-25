/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_APP_RAND_H_
#define _EMU_APP_RAND_H_

#include <stdint.h>

/* Seed material from the host RNG or its clocks, implemented per host in
 * host/<os>/host.c. Only this generator asks for it -- the machine asks for
 * host_rand_64 (host/os.h), which on this host is the generator below. */
uint64_t host_entropy_64(void);

/* Force a fixed lrand seed for reproducible runs. With no seed set,
 * host_rand_64 defaults to host entropy. */
void rand_set_seed(uint64_t seed);

/* The run's seed, taking host entropy now if nothing set one. Anything wanting
 * its own reproducible stream salts this rather than drawing from host_rand_64,
 * which the 6502's rand() syscall reads and which nothing else may disturb. */
uint64_t rand_seed_value(void);

#endif /* _EMU_APP_RAND_H_ */
