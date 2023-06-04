/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/rng.h"
#include "pico/rand.h"

void rng_api_rand32()
{
    return api_return_axsreg(get_rand_32());
}
