/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Random (rand.c): the host entropy source the vendored atr.c lrand API reaches
 * through sys/rand.h. The emulator exposes only the seed control point.
 */

#ifndef _EMU_RAND_H_
#define _EMU_RAND_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Force a fixed lrand seed for reproducible runs — the --seed CLI option and
 * the tests. With no seed set, get_rand_64 defaults to host entropy. */
void emu_set_random_seed(uint64_t seed);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_RAND_H_ */
