/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See save.h.
 */

#include "core/api/save.h"
#include "core/str/path.h"
#include "core/str/str.h"
#include "osal/fs.h"
#include <string.h>
#include <strings.h>

bool save_std_handles(const char *path)
{
    return strncasecmp(path, STR_SAVE_COLON, STR_SAVE_COLON_LEN) == 0;
}

static bool save_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}

/* Each list holds three-letter names back to back. */
static bool save_listed(const char *name, const char *list)
{
    for (; *list; list += 3)
        if (strncasecmp(name, list, 3) == 0)
            return true;
    return false;
}

/* Windows cannot store a file under a device name, whatever its extension. */
static bool save_reserved(const char *name)
{
    size_t stem = strcspn(name, ".");
    if (stem == 3)
        return save_listed(name, STR_SAVE_DEVICES);
    return stem == 4 && name[3] >= '1' && name[3] <= '9' &&
           save_listed(name, STR_SAVE_PORTS);
}

static bool save_name_ok(const char *name)
{
    size_t len = strlen(name);
    if (!len || len > SAVE_NAME_MAX || name[len - 1] == '.')
        return false;
    for (const char *p = name; *p; p++)
        if (!save_char_ok(*p))
            return false;
    return !save_reserved(name);
}

int save_std_open(const char *path, uint8_t flags, api_errno *err)
{
    const char *name = path + STR_SAVE_COLON_LEN;
    if (path_is_sep(*name))
        name++;
    if (!save_name_ok(name))
    {
        *err = API_EINVAL;
        return -1;
    }
    return fs_save_open(name, flags, err);
}
