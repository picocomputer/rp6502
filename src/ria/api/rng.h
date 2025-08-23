/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RNG_H_
#define _RNG_H_

/* Random Number Generator. Automatically seeds with
 * the RP2350 true random number generator (TRNG).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// API returns a random number.
bool rng_api_lrand(void);

#endif /* _RNG_H_ */
