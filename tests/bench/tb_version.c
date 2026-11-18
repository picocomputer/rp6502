/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/version.h"

/* A test's machine has no build to be stamped with. */

const char *version_string(void)
{
    return "Version 0.0";
}

const char *version_bare(void)
{
    return "0.0";
}
