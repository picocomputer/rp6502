/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/rng.h"
#include <pico/rand.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_RNG)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

bool rng_api_lrand(void)
{
    // The Pi Pico SDK random is perfect here.
    return api_return_axsreg(get_rand_32() & 0x7FFFFFFF);
}
