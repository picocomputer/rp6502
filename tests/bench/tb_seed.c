/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/random.h"
#include "host/host.h"

#include <stdint.h>

uint32_t host_seed(void)
{
    return 0x6502C0DE;
}

const char *host_save_dir(void)
{
    return NULL;
}
