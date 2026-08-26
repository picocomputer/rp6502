/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The generator every machine that needs a reproducible one runs: an LCG step
 * feeding a Murmur3 fmix64 finalizer. One step, written once, so two machines
 * asked for the same seed answer with the same stream -- which is what lets a
 * test pin an oracle across them.
 *
 * The state is the caller's. Who seeds it, from what, and whether a run can be
 * pinned are each machine's policy: a Pico has a hardware RNG, the emulator
 * takes host entropy or a --seed, a Pocket has only the wall clock. A stream
 * that must not disturb the 6502's rand() keeps a state of its own. */

#ifndef _CORE_RAND_H_
#define _CORE_RAND_H_

#include <stdint.h>

uint64_t rand_step(uint64_t *state);

#endif /* _CORE_RAND_H_ */
