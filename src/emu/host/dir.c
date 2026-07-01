/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSC0: on the native host filesystem: address translation and the directory +
 * file-metadata ops (stat, the opendir/readdir family, free space, unlink/rename/
 * mkdir/chmod/utime/label, the cwd). Host timestamps and modes are mapped into the
 * FatFs FILINFO field set the 6502 reads, so host_dir_ops is one interchangeable
 * backend the emulator plugs into its dir vtable (fat_dir_ops is the other).
 * "MSC0:" maps straight onto the OS filesystem (absolute from the root, relative
 * from the process cwd). ROM:/overlays are not enumerable, so they never appear.
 */

#include "emu/api/api.h" /* FS_HOST_MAX_PATH */
#include "emu/host/dir.h"
#include "emu/host/posixdir.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

/* ---- Address translation: MSC0: <-> host path ---------------------------- */

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

/* Map a drive-stripped MSC0: path to a host path. "//C/..." names a Windows
 * drive; everything else is the native path verbatim — absolute "/x" from the OS
 * root, relative "x" from the process cwd. The OS resolves "." and "..". */
static bool msc_to_host(const char *rest, char *host, size_t hsz)
{
    int w;
    if (rest[0] == '/' && rest[1] == '/' &&
        isalpha((unsigned char)rest[2]) && rest[3] == '/')
        w = snprintf(host, hsz, "%c:/%s", rest[2], rest + 4);
    else
        w = snprintf(host, hsz, "%s", rest[0] ? rest : ".");
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

/* Render a host path as an MSC0: path (the inverse for absolutes): a Windows
 * "C:/x" -> "MSC0://C/x", else the path tacked under MSC0:. Returns its length,
 * or 0 if it did not fit (the caller must treat 0 as a failure, never a short
 * path — f_getcwd is full-path-or-error). Used for argv[0] and getcwd. */
size_t fs_host_to_msc(const char *hostpath, char *out, size_t outsz)
{
    int w;
    if (isalpha((unsigned char)hostpath[0]) && hostpath[1] == ':')
        w = snprintf(out, outsz, "MSC0://%c%s", hostpath[0], hostpath + 2);
    else
        w = snprintf(out, outsz, "MSC0:%s", hostpath);
    if (w < 0 || (size_t)w >= outsz)
        return 0;
    return (size_t)w;
}

/* ---- FILINFO synthesis from host metadata -------------------------------- */

/* FAT attribute bits (FatFs AM_*), as the 6502 sees them in FILINFO.fattrib. */
#define FS_AM_RDO 0x01
#define FS_AM_HID 0x02
#define FS_AM_SYS 0x04
#define FS_AM_DIR 0x10
#define FS_AM_ARC 0x20

/* Pack a host time into the FatFs 16-bit date/time the 6502 expects (local
 * time, FAT epoch 1980). Times before 1980 clamp to the epoch. */
static void fat_pack_time(time_t t, uint16_t *fdate, uint16_t *ftime)
{
    struct tm tm;
    localtime_r(&t, &tm);
    int year = tm.tm_year + 1900;
    if (year < 1980)
    {
        *fdate = (1 << 5) | 1; /* 1980-01-01 */
        *ftime = 0;
        return;
    }
    *fdate = (uint16_t)(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    *ftime = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
}

/* Synthesize FatFs attributes from a host mode + name (no FAT bits on the host,
 * so: directory, archive on files, read-only when unwritable, hidden dotfiles). */
static uint8_t fat_attrib(const struct stat *st, const char *name)
{
    uint8_t a = S_ISDIR(st->st_mode) ? FS_AM_DIR : FS_AM_ARC;
    if (!(st->st_mode & S_IWUSR))
        a |= FS_AM_RDO;
    if (name[0] == '.')
        a |= FS_AM_HID;
    return a;
}

static void info_from_stat(FILINFO *fno, const struct stat *st, const char *name)
{
    snprintf(fno->fname, sizeof(fno->fname), "%s", name);
    fno->altname[0] = 0; /* host has no 8.3 short name */
    fno->fsize = st->st_size > 0xFFFFFFFF ? 0xFFFFFFFF : (FSIZE_t)st->st_size;
    fno->fattrib = fat_attrib(st, name);
    fat_pack_time(st->st_mtime, &fno->fdate, &fno->ftime);
    fat_pack_time(st->st_ctime, &fno->crdate, &fno->crtime);
}

/* Set *err from the current errno and return the driver's -1 sentinel. */
static int host_fail(api_errno *err)
{
    *err = api_errno_from_host(errno);
    return -1;
}

/* ---- Directory pool ------------------------------------------------------ */

#define HOST_MAX_DIR 8

/* Open directory handles: a POSIX stream (opaque) plus the opened path (to stat
 * each entry) and an entry-index counter (mirrors the firmware tells[]). */
struct host_dir
{
    bool used;
    void *dp;
    char host[FS_HOST_MAX_PATH];
    long pos;
};
static struct host_dir dirs[HOST_MAX_DIR];

void host_dir_stop(void)
{
    for (int i = 0; i < HOST_MAX_DIR; i++)
    {
        if (dirs[i].used && dirs[i].dp)
            host_closedir_posix(dirs[i].dp);
        dirs[i].used = false;
        dirs[i].dp = NULL;
    }
}

static struct host_dir *dir_slot(int des, api_errno *err)
{
    if (des < 0 || des >= HOST_MAX_DIR)
    {
        *err = API_EINVAL;
        return NULL;
    }
    if (!dirs[des].used)
    {
        *err = API_EBADF;
        return NULL;
    }
    return &dirs[des];
}

/* ---- The host backend ops (host_dir_ops) --------------------------------- */

int host_stat(const char *path, FILINFO *fno, api_errno *err)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return host_fail(err);
    struct stat st;
    if (stat(host, &st) != 0)
        return host_fail(err);
    /* stat names a single entry; report its basename, not the whole path. */
    const char *base = strrchr(host, '/');
    info_from_stat(fno, &st, base ? base + 1 : host);
    return 0;
}

int host_opendir(const char *path, api_errno *err)
{
    int des = 0;
    for (; des < HOST_MAX_DIR; des++)
        if (!dirs[des].used)
            break;
    if (des == HOST_MAX_DIR)
    {
        *err = API_EMFILE;
        return -1;
    }
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return host_fail(err);
    void *dp = host_opendir_posix(host);
    if (!dp)
        return host_fail(err);
    dirs[des].used = true;
    dirs[des].dp = dp;
    dirs[des].pos = 0;
    snprintf(dirs[des].host, sizeof(dirs[des].host), "%s", host);
    return des;
}

int host_readdir(int des, FILINFO *fno, api_errno *err)
{
    struct host_dir *d = dir_slot(des, err);
    if (!d)
        return -1;
    char name[256]; /* a directory entry name (<= NAME_MAX), not a full path */
    bool is_dir;
    int r;
    do
    {
        r = host_readdir_posix(d->dp, name, sizeof(name), &is_dir);
        if (r < 0)
            return host_fail(err);
        if (r == 0)
        {
            memset(fno, 0, sizeof(*fno)); /* fname[0]==0 signals EOF */
            return 0;
        }
    } while (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
    d->pos++;
    char entry[FS_HOST_MAX_PATH];
    struct stat st;
    if (snprintf(entry, sizeof(entry), "%s/%s", d->host, name) < (int)sizeof(entry) &&
        stat(entry, &st) == 0)
        info_from_stat(fno, &st, name);
    else
    {
        memset(fno, 0, sizeof(*fno)); /* unstattable entry: name + dir guess */
        snprintf(fno->fname, sizeof(fno->fname), "%s", name);
        fno->fattrib = is_dir ? FS_AM_DIR : FS_AM_ARC;
    }
    return 0;
}

int host_closedir(int des, api_errno *err)
{
    struct host_dir *d = dir_slot(des, err);
    if (!d)
        return -1;
    host_closedir_posix(d->dp);
    d->used = false;
    d->dp = NULL;
    return 0;
}

int host_telldir(int des, int32_t *pos, api_errno *err)
{
    struct host_dir *d = dir_slot(des, err);
    if (!d)
        return -1;
    *pos = (int32_t)d->pos;
    return 0;
}

int host_rewinddir(int des, api_errno *err)
{
    struct host_dir *d = dir_slot(des, err);
    if (!d)
        return -1;
    host_rewinddir_posix(d->dp);
    d->pos = 0;
    return 0;
}

/* Seek by entry index, mirroring the firmware: rewind if going back, then read
 * forward to the target. Fails (EINVAL) if the target is past the end. */
int host_seekdir(int des, int32_t offs, api_errno *err)
{
    struct host_dir *d = dir_slot(des, err);
    if (!d)
        return -1;
    if (offs < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    if (d->pos > offs)
    {
        host_rewinddir_posix(d->dp);
        d->pos = 0;
    }
    while (d->pos < offs)
    {
        FILINFO fno;
        if (host_readdir(des, &fno, err) != 0)
            return -1;
        if (!fno.fname[0]) /* hit EOF before reaching offs */
        {
            *err = API_EINVAL;
            return -1;
        }
    }
    return 0;
}

int host_unlink(const char *path, api_errno *err)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return host_fail(err);
    if (remove(host) != 0)
        return host_fail(err);
    return 0;
}

int host_rename(const char *oldp, const char *newp, api_errno *err)
{
    char ho[FS_HOST_MAX_PATH], hn[FS_HOST_MAX_PATH];
    if (!fs_to_host(oldp, ho, sizeof(ho)) || !fs_to_host(newp, hn, sizeof(hn)))
        return host_fail(err);
    if (rename(ho, hn) != 0)
        return host_fail(err);
    return 0;
}

int host_mkdir(const char *path, api_errno *err)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return host_fail(err);
    if (mkdir(host, 0777) != 0)
        return host_fail(err);
    return 0;
}

int host_getfree(const char *path, uint32_t *free_sectors, uint32_t *total_sectors, api_errno *err)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return host_fail(err);
    struct statvfs vfs;
    if (statvfs(host, &vfs) != 0)
        return host_fail(err);
    uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    uint64_t tot = (uint64_t)vfs.f_blocks * unit / 512;
    uint64_t fre = (uint64_t)vfs.f_bavail * unit / 512;
    *total_sectors = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    *free_sectors = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    return 0;
}

/* Best-effort: only the read-only bit maps to the host (write permission).
 * Hidden/system/archive have no host equivalent and are silently dropped. */
int host_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    if (!(mask & FS_AM_RDO))
        return 0;
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return host_fail(err);
    struct stat st;
    if (stat(host, &st) != 0)
        return host_fail(err);
    mode_t m = st.st_mode & 07777;
    if (attr & FS_AM_RDO)
        m &= ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH);
    else
        m |= S_IWUSR;
    if (chmod(host, m) != 0)
        return host_fail(err);
    return 0;
}

/* Best-effort: set the modification time from the FAT date/time in the FILINFO.
 * The creation time it also carries is not settable on POSIX, so it is ignored. */
int host_utime(const char *path, const FILINFO *fno, api_errno *err)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return host_fail(err);
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = ((fno->fdate >> 9) & 0x7F) + 1980 - 1900;
    tm.tm_mon = ((fno->fdate >> 5) & 0x0F) - 1;
    tm.tm_mday = fno->fdate & 0x1F;
    tm.tm_hour = (fno->ftime >> 11) & 0x1F;
    tm.tm_min = (fno->ftime >> 5) & 0x3F;
    tm.tm_sec = (fno->ftime & 0x1F) * 2;
    tm.tm_isdst = -1;
    struct utimbuf ub;
    ub.actime = ub.modtime = mktime(&tm);
    if (utime(host, &ub) != 0)
        return host_fail(err);
    return 0;
}

/* The host filesystem has no FAT volume label; report an empty one and accept
 * (ignore) a set, so label-aware programs run rather than erroring. */
int host_getlabel(const char *path, char *label, api_errno *err)
{
    (void)path, (void)err;
    label[0] = 0;
    return 0;
}

int host_setlabel(const char *path, api_errno *err)
{
    (void)path, (void)err;
    return 0;
}

int host_chdir(const char *path, api_errno *err)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return host_fail(err);
    if (chdir(host) != 0) /* chdir validates existence/dir-ness and sets errno */
        return host_fail(err);
    return 0;
}

/* The 6502 sees MSC0: (and the bare current drive); reject anything else as a
 * missing device. */
int host_chdrive(const char *drive, api_errno *err)
{
    if (drive[0] == ':') /* the null drive (installs) is not a cwd-able drive */
    {
        *err = API_ENODEV;
        return -1;
    }
    char name[16];
    size_t i = 0;
    for (; drive[i] && drive[i] != ':' && i < sizeof(name) - 1; i++)
        name[i] = (char)drive[i];
    name[i] = 0;
    if (name[0] == 0 || strcasecmp(name, "MSC0") == 0)
        return 0;
    *err = API_ENODEV;
    return -1;
}

int host_getcwd(char *buf, size_t size, api_errno *err)
{
    char cwd[FS_HOST_MAX_PATH];
    if (!getcwd(cwd, sizeof(cwd)))
        return host_fail(err);
    if (fs_host_to_msc(cwd, buf, size) == 0) /* did not fit: full-path-or-error */
    {
        *err = API_ENOMEM;
        return -1;
    }
    return 0;
}

const fs_dir_ops host_dir_ops = {
    .stat = host_stat,
    .opendir = host_opendir,
    .readdir = host_readdir,
    .closedir = host_closedir,
    .telldir = host_telldir,
    .seekdir = host_seekdir,
    .rewinddir = host_rewinddir,
    .unlink = host_unlink,
    .rename = host_rename,
    .chmod = host_chmod,
    .utime = host_utime,
    .mkdir = host_mkdir,
    .chdir = host_chdir,
    .chdrive = host_chdrive,
    .getcwd = host_getcwd,
    .getlabel = host_getlabel,
    .setlabel = host_setlabel,
    .getfree = host_getfree,
    .stop = host_dir_stop,
};
