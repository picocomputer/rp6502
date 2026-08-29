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

#include "core/api/fs.h"
#include "core/rom/rom.h"
#include "core/str/path.h"
#include "host/os.h"
#include <errno.h>
#include <string.h>
#include <strings.h>

#ifndef ROM_ALIAS_MAX
#define ROM_ALIAS_MAX 0
#endif

#if ROM_ALIAS_MAX

#define ALIAS_NAME_MAX 64

typedef struct
{
    bool used;
    char name[ALIAS_NAME_MAX]; /* basename, e.g. "adventure.rp6502" (the text after ":") */
    char host[HOST_MAX_PATH];  /* the backing file */
} alias_t;
static alias_t aliases[ROM_ALIAS_MAX];

/* Install a .rp6502 on the null drive, keyed by its host-path basename. */
bool rom_install(const char *hostpath)
{
    const char *base = path_basename(hostpath);
    if (!*base || strlen(base) >= ALIAS_NAME_MAX || strlen(hostpath) >= HOST_MAX_PATH)
        return false;
    /* Must exist. Asked through the driver, because that is the machine's
     * answer for what a file is. */
    api_errno err;
    int fd = fs_std_open(hostpath, FS_RD, &err);
    if (fd < 0)
        return false;
    fs_std_close(fd, &err);
    for (int i = 0; i < ROM_ALIAS_MAX; i++)
        if (!aliases[i].used)
        {
            aliases[i].used = true;
            strcpy(aliases[i].name, base);
            strcpy(aliases[i].host, hostpath);
            return true;
        }
    return false;
}

/* Resolve ":name" to the file it aliases, case-insensitively to match the
 * firmware's installed-name handling. Anything else passes through verbatim:
 * a drive path and a bare host path are both spellings the filesystem seam
 * accepts, so neither is rewritten here. */
bool rom_resolve(const char *path, char *out, size_t outsz)
{
    if (path[0] == ':') /* null drive: an installed ROM, or nothing */
    {
        for (int i = 0; i < ROM_ALIAS_MAX; i++)
            if (aliases[i].used && strcasecmp(aliases[i].name, path + 1) == 0)
            {
                if (strlen(aliases[i].host) >= outsz)
                    return false;
                strcpy(out, aliases[i].host);
                return true;
            }
        errno = ENOENT;
        return false;
    }
    if (strlen(path) >= outsz)
        return false;
    strcpy(out, path);
    return true;
}

#else /* !ROM_ALIAS_MAX: no null drive on this host */

bool rom_install(const char *hostpath)
{
    (void)hostpath;
    return false;
}

bool rom_resolve(const char *path, char *out, size_t outsz)
{
    if (path[0] == ':')
    {
        errno = ENOENT;
        return false;
    }
    if (strlen(path) >= outsz)
        return false;
    strcpy(out, path);
    return true;
}

#endif /* ROM_ALIAS_MAX */
