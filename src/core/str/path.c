/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See path.h.
 */

#include "core/str/path.h"

const char *path_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            base = p + 1;
    return base;
}
