/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "mach/version.h"
#include "rp6502_version.h"

/* Alone in its own unit so the generated header has exactly one object to
 * invalidate: everything else this program is made of is what the stamp is
 * taken against, and none of it should be dragged into a rebuild by carrying
 * the answer. Every machine that has a version to give compiles this file. */
const char *version_string(void)
{
    return RP6502_VERSION;
}

const char *version_bare(void)
{
    return RP6502_VERSION_BARE;
}
