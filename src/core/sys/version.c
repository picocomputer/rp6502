/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/sys/version.h"
#include "rp6502_version.h"

/* Alone in its own unit so the generated header has exactly one object to
 * invalidate. Three callers read it — the no-ROM screen, --credits and the
 * debugger's about box — and none of them should drag a rebuild stamp along. */
const char *version_string(void)
{
    return RP6502_VERSION;
}
