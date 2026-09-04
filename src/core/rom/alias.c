/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The null drive as a map: an installed ":name" aliasing the host file that
 * backs it. This is the store on the machines whose ROMs stay where they
 * are -- the emulator family's --rom -- as littlefs is the store on a Pico.
 * A host asks for it by defining ROM_ALIAS_MAX; without that this whole
 * file is a pair of refusals the linker drops.
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
    char *name; /* basename, e.g. "adventure.rp6502" (the text after ":") */
    char *host; /* the backing file, and what marks the slot used */
} alias_t;
static alias_t aliases[ROM_ALIAS_MAX];

/* Install a .rp6502 on the null drive, keyed by its host-path basename. */
bool rom_alias_insert(const char *hostpath)
{
    const char *base = path_basename(hostpath);
    if (!*base)
        return false;
    /* Must exist. Asked through the driver, because that is the machine's
     * answer for what a file is. */
    api_errno err;
    int fd = fs_std_open(hostpath, FS_RD, &err);
    if (fd < 0)
        return false;
    fs_std_close(fd, &err);
    for (int i = 0; i < ROM_ALIAS_MAX; i++)
        if (!aliases[i].host)
        {
            char *name = strdup(base), *host = strdup(hostpath);
            if (name && host)
            {
                aliases[i].name = name;
                aliases[i].host = host; /* last: it is what marks the slot used */
                return true;
            }
            free(name), free(host);
            return false;
        }
    return false;
}

/* Resolve ":name" to the file it aliases, case-insensitively to match the
 * firmware's installed-name handling. Everything else -- including a colon
 * name no alias claims -- passes through verbatim: this is a map, not a
 * gate, and whether an unaliased name opens is the store's answer, not the
 * list's. A machine whose store is real needs the pass-through.
 *
 * Borrowed, not copied: an install outlives every load that reads it, and a
 * path that resolves to itself has nowhere better to live than where it
 * already is. */
const char *rom_alias_resolve(const char *path)
{
    if (path[0] == ':')
        for (int i = 0; i < ROM_ALIAS_MAX; i++)
            if (aliases[i].host && strcasecmp(aliases[i].name, path + 1) == 0)
                return aliases[i].host;
    return path;
}

#else /* !ROM_ALIAS_MAX: no aliases; every name is the store's to answer */

bool rom_alias_insert(const char *hostpath)
{
    (void)hostpath;
    return false;
}

const char *rom_alias_resolve(const char *path)
{
    return path;
}

#endif /* ROM_ALIAS_MAX */
