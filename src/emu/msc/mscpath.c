/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSC0: addressing and the host mount. MSC0: is the writable drive: a window
 * onto the host filesystem rooted at a mount directory, with no chroot. The
 * mount is chosen ONCE — the launch dir by default, --fs <dir>, or a throwaway
 * --tmpdrive — and persists for the whole session, exec included; nothing in the
 * ROM-load path resets it. The text after "MSC0:" is a host path resolved under
 * the mount:
 *     MSC0:/sub/file       -> <mount>/sub/file   (absolute = drive root)
 *     MSC0:sub/file        -> <cwd>/sub/file      (relative to the cwd)
 *     MSC0://C/Users/Homey -> C:/Users/Homey      (Windows drive letter)
 * The OS resolves "." and ".." — a program may walk out of the mount. The msc.c
 * (files) and mscdir.c (directories) backends translate through here.
 */

#include "emu/api/api.h"
#include "emu/mon/install.h"
#include "emu/msc/mscpath.h"
#include <ctype.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* MSC0: mount root + current directory, both absolute HOST paths. g_root is the
 * drive root (where MSC0:/ lands); g_cwd is the dir relative paths resolve
 * against. Set by fs_set_cwd / fs_use_tmpdrive, else defaulted to the launch dir
 * on first use. */
static char g_cwd[FS_HOST_MAX_PATH];
static char g_root[FS_HOST_MAX_PATH];

/* --tmpdrive's throwaway directory, removed at exit on native (MEMFS expires on
 * its own on the web). Empty unless a tmpdrive is active. */
static char g_tmpdir[FS_HOST_MAX_PATH];

/* Default the mount to the launch dir if no entry point set one (e.g. a unit
 * test that opens a path before mounting). */
static void ensure_mounted(void)
{
    if (!g_root[0])
        fs_set_cwd(".");
}

/* Drop a recognized writable-drive prefix. FatFs recognizes only "0:".."9:" and
 * "MSC0:".."MSC9:" (case-insensitive); anything else keeps its prefix and is
 * treated as a relative name (the OS, not us, then rejects a bogus ":"). */
const char *fs_strip_drive(const char *path)
{
    const char *colon = strchr(path, ':');
    if (!colon || colon == path)
        return path;
    size_t n = (size_t)(colon - path);
    bool is_drive = (n == 1 && isdigit((unsigned char)path[0])) ||
                    (n == 4 && strncasecmp(path, "MSC", 3) == 0 &&
                     isdigit((unsigned char)path[3]));
    return is_drive ? colon + 1 : path;
}

bool fs_has_drive_prefix(const char *path)
{
    return fs_strip_drive(path) != path;
}

/* Map a drive-relative MSC0 path to a host path under the mount. The Windows
 * "//C/..." form names an explicit drive; an absolute "/..." is the drive root
 * (g_root, the mount); a relative path resolves against the current directory.
 * The OS handles "." and ".."; a program may walk above the mount. */
static bool msc_to_host(const char *rest, char *host, size_t hsz)
{
    ensure_mounted();
    int w;
    if (rest[0] == '/' && rest[1] == '/' &&
        isalpha((unsigned char)rest[2]) && rest[3] == '/')
        w = snprintf(host, hsz, "%c:/%s", rest[2], rest + 4);
    else if (rest[0] == '/')
        w = snprintf(host, hsz, "%s%s", g_root, rest); /* absolute = drive root */
    else
        w = snprintf(host, hsz, "%s/%s", g_cwd, rest);
    if (w < 0 || (size_t)w >= hsz)
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

bool fs_to_host(const char *path, char *host, size_t hsz)
{
    const char *rest = fs_strip_drive(path);
    /* A leading ":" is the null drive (installed ROMs, install.c) — never a host
     * path. Refuse it here so neither ":name" nor "MSC0::name" can map onto a host
     * file; the boot/exec loader reaches installs via fs_resolve_rom instead. */
    if (rest[0] == ':')
    {
        errno = ENOENT;
        return false;
    }
    return msc_to_host(rest, host, hsz);
}

/* Render an absolute host path as an MSC0: path (the inverse for absolutes):
 * a path under the mount -> "MSC0:/rel"; Windows "C:/x" -> "MSC0://C/x"; else
 * the bare path tacked under MSC0:. Returns its length, or 0 if it did not fit
 * (the caller must treat 0 as a failure, never a short path — f_getcwd is
 * full-path-or-error). Used for argv[0] and fs_getcwd. */
size_t fs_host_to_msc(const char *hostpath, char *out, size_t outsz)
{
    ensure_mounted();
    int w;
    /* hide the mount: a path inside it shows as MSC0:/... */
    size_t rl = strlen(g_root);
    if (strncmp(hostpath, g_root, rl) == 0 &&
        (hostpath[rl] == 0 || hostpath[rl] == '/'))
    {
        const char *rel = hostpath + rl;
        w = snprintf(out, outsz, "MSC0:%s", rel[0] ? rel : "/");
        if (w < 0 || (size_t)w >= outsz)
            return 0;
        return (size_t)w;
    }
    if (isalpha((unsigned char)hostpath[0]) && hostpath[1] == ':')
        w = snprintf(out, outsz, "MSC0://%c%s", hostpath[0], hostpath + 2);
    else
        w = snprintf(out, outsz, "MSC0:%s", hostpath);
    if (w < 0 || (size_t)w >= outsz)
        return 0;
    return (size_t)w;
}

/* Mount MSC0: at a host directory: it becomes the drive root and the current
 * dir. Used by --fs (and the boot default ".") to choose where the drive lives. */
bool fs_set_cwd(const char *hostdir)
{
    char resolved[FS_HOST_MAX_PATH];
    if (!realpath(hostdir, resolved))
        return false;
    if (strlen(resolved) >= sizeof(g_root))
        return false;
    strcpy(g_root, resolved);
    strcpy(g_cwd, resolved);
    return true;
}

/* Recursively delete the tmpdrive at exit (native). nftw walks depth-first so
 * remove() sees empty dirs. */
static int rm_entry(const char *p, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st, (void)flag, (void)ftw;
    remove(p);
    return 0;
}
static void tmpdrive_cleanup(void)
{
    if (g_tmpdir[0])
        nftw(g_tmpdir, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
}

/* --tmpdrive: back MSC0: with a fresh throwaway directory so a ROM runs with an
 * isolated, ephemeral store. mkdtemp gives a private dir under /tmp (a real
 * tmpfs/dir on native; MEMFS on the web, which never persists to IDBFS and
 * vanishes when the session ends). Removed at process exit on native. */
bool fs_use_tmpdrive(void)
{
    char tmpl[] = "/tmp/rp6502-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    if (!dir || strlen(dir) >= sizeof(g_tmpdir))
        return false;
    strcpy(g_tmpdir, dir);
    strcpy(g_root, dir);
    strcpy(g_cwd, dir);
    static bool registered;
    if (!registered)
    {
        atexit(tmpdrive_cleanup);
        registered = true;
    }
    return true;
}

int fs_chdir(const char *path)
{
    char host[FS_HOST_MAX_PATH], resolved[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return -1;
    if (!realpath(host, resolved)) /* canonical, resolving .. and symlinks */
        return -1;                 /* errno set by realpath */
    struct stat st;
    if (stat(resolved, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode))
    {
        errno = ENOTDIR;
        return -1;
    }
    if (strlen(resolved) >= sizeof(g_cwd))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(g_cwd, resolved); /* no chroot: the cwd may sit outside the mount */
    return 0;
}

/* The 6502 sees MSC0: (and the bare current drive); reject anything else as a
 * missing device. */
int fs_chdrive(const char *drive)
{
    if (drive[0] == ':') /* the null drive (installs) is not a cwd-able drive */
    {
        errno = ENODEV;
        return -1;
    }
    char name[16];
    size_t i = 0;
    for (; drive[i] && drive[i] != ':' && i < sizeof(name) - 1; i++)
        name[i] = (char)drive[i];
    name[i] = 0;
    if (name[0] == 0 || strcasecmp(name, "MSC0") == 0)
        return 0;
    errno = ENODEV;
    return -1;
}

size_t fs_getcwd(char *out, size_t outsz)
{
    ensure_mounted();
    return fs_host_to_msc(g_cwd, out, outsz);
}
