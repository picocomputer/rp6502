/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_APP_RAND_H_
#define _EMU_APP_RAND_H_

#include <stdint.h>

/* Force a fixed lrand seed for reproducible runs. With no seed set,
 * get_rand_64 defaults to host entropy. */
void rand_set_seed(uint64_t seed);

#endif /* _EMU_APP_RAND_H_ */
