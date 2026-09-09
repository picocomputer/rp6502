/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A map from an installed ":name" to the host file that backs it, for the
 * machines whose ROMs are left where they are rather than copied into a store
 * of their own. ROM_ALIAS_MAX, the number of slots, comes from emu.cmake.
 */

#include "osal/fs.h"
#include "core/rom/rom.h"
#include "core/str/path.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef ROM_ALIAS_MAX
#define ROM_ALIAS_MAX 0
#endif

#if ROM_ALIAS_MAX

typedef struct
{
    char *name; /* the text after the ":", such as "adventure.rp6502" */
    char *host; /* the backing file, and what marks the slot used */
} alias_t;
static alias_t aliases[ROM_ALIAS_MAX];

bool rom_alias_insert_as(const char *hostpath, const char *name)
{
    if (!name || !*name)
        return false;
    /* Only a check that the file exists now; the load reopens it later through
     * fs_rom_open. */
    api_errno err;
    int fd = fs_std_open(hostpath, FS_RD, &err);
    if (fd < 0)
        return false;
    fs_std_close(fd, &err);
    for (int i = 0; i < ROM_ALIAS_MAX; i++)
        if (!aliases[i].host)
        {
            char *key = strdup(name), *host = strdup(hostpath);
            if (key && host)
            {
                aliases[i].name = key;
                aliases[i].host = host;
                return true;
            }
            free(key), free(host);
            return false;
        }
    return false;
}

bool rom_alias_insert(const char *hostpath)
{
    return rom_alias_insert_as(hostpath, path_basename(hostpath));
}

bool rom_alias_remove(const char *name)
{
    if (!name)
        return false;
    if (*name == ':')
        name++;
    for (int i = 0; i < ROM_ALIAS_MAX; i++)
        if (aliases[i].host && strcasecmp(aliases[i].name, name) == 0)
        {
            char *key = aliases[i].name, *host = aliases[i].host;
            aliases[i].host = NULL;
            aliases[i].name = NULL;
            free(key), free(host);
            return true;
        }
    return false;
}

/* Resolve ":name" to the file it aliases. The comparison ignores case, to match
 * the firmware's handling of installed names. Everything else passes through
 * verbatim, including a name no alias claims. */
const char *rom_alias_resolve(const char *path)
{
    if (path[0] == ':')
        for (int i = 0; i < ROM_ALIAS_MAX; i++)
            if (aliases[i].host && strcasecmp(aliases[i].name, path + 1) == 0)
                return aliases[i].host;
    return path;
}

#else /* ROM_ALIAS_MAX */

bool rom_alias_insert(const char *hostpath)
{
    (void)hostpath;
    return false;
}

bool rom_alias_insert_as(const char *hostpath, const char *name)
{
    (void)hostpath, (void)name;
    return false;
}

bool rom_alias_remove(const char *name)
{
    (void)name;
    return false;
}

const char *rom_alias_resolve(const char *path)
{
    return path;
}

#endif /* ROM_ALIAS_MAX */
