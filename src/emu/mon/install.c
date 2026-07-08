/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/mon/install.h"
#include "emu/host/msc.h"
#include <errno.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <sys/stat.h>

#define INSTALL_MAX 16
#define INSTALL_NAME_MAX 64

typedef struct
{
    bool used;
    char name[INSTALL_NAME_MAX]; /* basename, e.g. "adventure.rp6502" (the text after ":") */
    char host[MSC_MAX_PATH]; /* the backing file */
    size_t size;
} install_t;
static install_t installs[INSTALL_MAX];

/* Install a .rp6502 on the null drive, keyed by its host-path basename. */
bool install_rom(const char *hostpath)
{
    const char *base = strrchr(hostpath, '/');
    base = base ? base + 1 : hostpath;
    if (!*base || strlen(base) >= INSTALL_NAME_MAX || strlen(hostpath) >= MSC_MAX_PATH)
        return false;
    struct stat st;
    if (stat(hostpath, &st) != 0) /* must exist; size for the whole-file window */
        return false;
    for (int i = 0; i < INSTALL_MAX; i++)
        if (!installs[i].used)
        {
            installs[i].used = true;
            strcpy(installs[i].name, base);
            strcpy(installs[i].host, hostpath);
            installs[i].size = (size_t)st.st_size;
            return true;
        }
    return false;
}

/* Find an installed ROM by name (the text after ":"), case-insensitively to match
 * the firmware's installed-name handling and the sibling ROM: asset driver. */
static install_t *install_find(const char *name)
{
    for (int i = 0; i < INSTALL_MAX; i++)
        if (installs[i].used && strcasecmp(installs[i].name, name) == 0)
            return &installs[i];
    return NULL;
}

/* Resolve a boot/exec ROM path to the host file to open: an installed ":name" ->
 * its backing file, a drive path -> msc_to_host, else the bare path (the native
 * CLI / tests, against the process cwd). The loader (rom.c) then opens it. */
bool install_resolve(const char *path, char *out, size_t outsz)
{
    if (path[0] == ':') /* null drive: an installed ROM, or nothing */
    {
        install_t *in = install_find(path + 1);
        if (!in)
        {
            errno = ENOENT;
            return false;
        }
        if (strlen(in->host) >= outsz)
            return false;
        strcpy(out, in->host);
        return true;
    }
    if (msc_has_drive_prefix(path))
        return msc_to_host(path, out, outsz);
    if (strlen(path) >= outsz)
        return false;
    strcpy(out, path);
    return true;
}
