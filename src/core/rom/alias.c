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
#include "core/str/oem.h"
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
    char *name; /* the text after the ":" in the code page, such as "adventure.rp6502" */
    char *host; /* the absolute host path, and what marks the slot used */
} alias_t;
static alias_t aliases[ROM_ALIAS_MAX];

/* The comparison ignores case, to match the firmware's handling of installed
 * names. */
static alias_t *alias_find(const char *name)
{
    for (int i = 0; i < ROM_ALIAS_MAX; i++)
        if (aliases[i].host && strcasecmp(aliases[i].name, name) == 0)
            return &aliases[i];
    return NULL;
}

/* The host path is kept absolute, because a program may chdir before the
 * install is loaded, and as a host path, because the code page may not hold
 * it. */
const char *rom_alias_insert_as(const char *host, const char *name)
{
    if (!*name)
        return NULL;
    char *abs = fs_host_realpath(host);
    if (!abs)
        return NULL;
    alias_t *slot = alias_find(name);
    for (int i = 0; !slot && i < ROM_ALIAS_MAX; i++)
        if (!aliases[i].host)
            slot = &aliases[i];
    char *key = slot ? strdup(name) : NULL;
    if (!key)
    {
        free(abs);
        return NULL;
    }
    free(slot->name), free(slot->host);
    slot->name = key;
    slot->host = abs;
    return key;
}

/* The name is the last part of the path as given, so a symlink is installed
 * under the symlink's name, not the target's. */
const char *rom_alias_insert(const char *host)
{
    char name[API_PATH_MAX]; /* with its ":", a name has to fit in a path */
    if (oem_from_utf8(path_basename(host), name, sizeof name) >= sizeof name)
        return NULL;
    return rom_alias_insert_as(host, name);
}

bool rom_alias_remove(const char *name)
{
    if (!name)
        return false;
    if (*name == ':')
        name++;
    alias_t *a = alias_find(name);
    if (!a)
        return false;
    free(a->name), free(a->host);
    a->name = a->host = NULL;
    return true;
}

const char *rom_alias_resolve(const char *path)
{
    alias_t *a = path[0] == ':' ? alias_find(path + 1) : NULL;
    return a ? a->host : NULL;
}

int rom_alias_open(const char *path, api_errno *err)
{
    const char *host = rom_alias_resolve(path);
    return host ? fs_rom_open_host(host, err) : fs_rom_open(path, FS_RD, err);
}

#else /* ROM_ALIAS_MAX */

const char *rom_alias_insert(const char *host)
{
    (void)host;
    return NULL;
}

const char *rom_alias_insert_as(const char *host, const char *name)
{
    (void)host, (void)name;
    return NULL;
}

bool rom_alias_remove(const char *name)
{
    (void)name;
    return false;
}

const char *rom_alias_resolve(const char *path)
{
    (void)path;
    return NULL;
}

int rom_alias_open(const char *path, api_errno *err)
{
    return fs_rom_open(path, FS_RD, err);
}

#endif /* ROM_ALIAS_MAX */
