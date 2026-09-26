/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/path.h"
#include <string.h>

const char *path_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (path_is_sep(*p))
            base = p + 1;
    return base;
}

bool path_fat_ok(const char *path, bool after_drive, api_errno *err)
{
    if (!after_drive && path[strcspn(path, ":/\\")] == ':')
    {
        *err = API_ENODEV;
        return false;
    }
    for (const char *p = path; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F || strchr("\"*:<>?|", c))
        {
            *err = API_EINVAL;
            return false;
        }
    }
    return true;
}
