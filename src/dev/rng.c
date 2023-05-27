/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rng.h"
#include "api.h"
#include "pico/rand.h"

void rng_api_rand()
{
    return api_return_axsreg(get_rand_32());
}
