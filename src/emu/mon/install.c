/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Installed ROMs on the NULL DRIVE ":" — the emulator's analog of the firmware's
 * flash-installed ROMs (ria/mon/rom.c rom_open opens a ":name" image for the
 * monitor's boot/exec loader, stripping the leading colon). `--rom <file.rp6502>`
 * installs a .rp6502 by its host basename; the boot/exec loader reaches it as
 * ":basename" through fs_resolve_rom (matched case-insensitively, like the ROM:
 * driver and the firmware). Like the firmware, the null drive is reachable ONLY by
 * the loader — a 6502 open/stat/opendir/chdir of ":name" goes to MSC0:, where a
 * leading ":" is refused (mscpath.c), so it is never the cwd and never enumerated
 * or stat'd. Installs are read-only, referenced by host PATH (no bytes copied), and
 * survive exec so an installed launcher can exec another installed ROM. The browser
 * fetches ROMs into a MEMFS cache and installs them here; the desktop points at disk.
 */

#include "emu/api/api.h"
#include "emu/mon/install.h"
#include "emu/host/dir.h"
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
    char host[FS_HOST_MAX_PATH]; /* the backing file */
    size_t size;
} install_t;
static install_t installs[INSTALL_MAX];

/* Install a .rp6502 on the null drive, keyed by its host-path basename. */
bool fs_install_rom(const char *hostpath)
{
    const char *base = strrchr(hostpath, '/');
    base = base ? base + 1 : hostpath;
    if (!*base || strlen(base) >= INSTALL_NAME_MAX || strlen(hostpath) >= FS_HOST_MAX_PATH)
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
 * its backing file, a drive path -> fs_to_host, else the bare path (the native
 * CLI / tests, against the process cwd). The loader (rom.c) then opens it. */
bool fs_resolve_rom(const char *path, char *out, size_t outsz)
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
    if (fs_has_drive_prefix(path))
        return fs_to_host(path, out, outsz);
    if (strlen(path) >= outsz)
        return false;
    strcpy(out, path);
    return true;
}
