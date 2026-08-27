/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See path.h.
 */

#include "core/str/path.h"
#include <string.h>
#include <strings.h>

const char *path_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (path_is_sep(*p))
            base = p + 1;
    return base;
}

const char *path_strip_drive(const char *path)
{
    const char *colon = strchr(path, ':');
    if (!colon || colon == path)
        return path;
    size_t n = (size_t)(colon - path);
    bool is_drive = (n == 1 && path[0] == '0') ||
                    (n == 4 && strncasecmp(path, "MSC0", 4) == 0);
    return is_drive ? colon + 1 : path;
}

bool path_has_drive(const char *path)
{
    return path_strip_drive(path) != path;
}
