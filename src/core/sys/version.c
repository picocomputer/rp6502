/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/sys/version.h"
#include "rp6502_version.h"

const char *version_string(void)
{
    return RP6502_VERSION;
}

const char *version_bare(void)
{
    return RP6502_VERSION_BARE;
}
