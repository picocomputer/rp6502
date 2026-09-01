/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What only the RIA says about itself. The VGA firmware compiles none of
 * core/api, so nothing in it asks for a seed.
 */

#include "host/host.h"
#include "osal/os.h"

uint32_t host_random_seed(void)
{
    /* Nothing overrides a board: no command line, no fixture. */
    return os_random_seed();
}
