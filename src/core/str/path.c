/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See path.h.
 */

#include "core/str/path.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
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

bool path_to_native(const char *path, char *out, size_t outsz)
{
    const char *rest = path_strip_drive(path);
    /* A leading ":" is the null drive, where installed ROMs live. It has no
     * native spelling at all, so neither ":name" nor "MSC0::name" can be made
     * to land on a real file; the ROM loader reaches installs its own way. */
    if (rest[0] == ':')
    {
        errno = ENOENT;
        return false;
    }
    int w;
    if (rest[0] == '/' && rest[1] == '/' &&
        isalpha((unsigned char)rest[2]) && rest[3] == '/')
        w = snprintf(out, outsz, "%c:/%s", rest[2], rest + 4);
    else
        w = snprintf(out, outsz, "%s", rest);
    if (w < 0 || (size_t)w >= outsz)
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

size_t path_from_native(const char *native, char *out, size_t outsz)
{
    int w;
    if (isalpha((unsigned char)native[0]) && native[1] == ':')
        w = snprintf(out, outsz, "MSC0://%c%s", native[0], native + 2);
    else
        w = snprintf(out, outsz, "MSC0:%s", native);
    if (w < 0 || (size_t)w >= outsz)
        return 0;
    return (size_t)w;
}
