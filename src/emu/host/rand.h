/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_HOST_RAND_H_
#define _EMU_HOST_RAND_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Force a fixed lrand seed for reproducible runs. With no seed set,
 * get_rand_64 defaults to host entropy. */
void rand_set_seed(uint64_t seed);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_RAND_H_ */
