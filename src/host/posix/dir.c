/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drive, as core/api/dir.h asks for it: the directory syscalls
 * answered over a POSIX filesystem.
 *
 * The 6502 asks in FatFs's vocabulary -- a FILINFO, with FAT attribute bits
 * and a 1980-epoch date -- because that is what the API has always spoken. A
 * POSIX host has none of that natively, so this is where struct stat becomes
 * one, in a single step: the host says what it knows, in the terms the answer
 * is wanted in.
 *
 * The directory walk itself is next door in dirent.c, because <dirent.h> and
 * ff.h both define a type called DIR and only one of them can be here.
 *
 * Paths cross spelled the way the 6502 spells them and in its OEM code page.
 * The drive prefix comes off with path_to_native() and the code page with
 * oem_to_utf8() (core/str/oem.h) before every libc call; returned names go
 * back with oem_from_utf8().
 */

#include "core/api/dir.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include "host/os.h"
#include "host/posix/dirent.h"
#include "host/posix/errno.h"
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

#define DIR_UPATH_MAX (3 * 4096) /* worst case: every OEM byte -> 3 UTF-8 bytes */
#define DIR_NAME_MAX 256         /* an entry's name, not a path */

/* A path arrives spelled the way the 6502 spells it. This drive is one
 * directory of a real filesystem, so the drive prefix comes off here and what
 * is left is the native path -- and then the code page comes off too. */
static bool path_to_utf8(const char *path, char *u8 /* [DIR_UPATH_MAX] */, api_errno *err)
{
    char native[HOST_MAX_PATH];
    if (!path_to_native(path, native, sizeof native) ||
        oem_to_utf8(native, u8, DIR_UPATH_MAX) >= DIR_UPATH_MAX)
    {
        *err = API_EINVAL;
        return false;
    }
    return true;
}

/* Whatever the last call set, in the API's words. */
static bool posix_ok(bool ok, api_errno *err)
{
    if (!ok)
        *err = errno_to_api(errno);
    return ok;
}

/* ---- FILINFO, from what POSIX keeps -------------------------------------- */

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
    host_localtime(t, &tm);
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

/* There are no FAT bits on a POSIX filesystem, so they are read off what is
 * there: a directory, archive on anything else, read-only when the owner
 * cannot write, hidden by the leading-dot convention. */
static void info_from_stat(FILINFO *fno, const struct stat *st, const char *name)
{
    snprintf(fno->fname, sizeof(fno->fname), "%s", name);
    fno->altname[0] = 0; /* no 8.3 short name here */
    fno->fsize = (uint64_t)st->st_size > 0xFFFFFFFF ? 0xFFFFFFFF : (FSIZE_t)st->st_size;
    uint8_t a = S_ISDIR(st->st_mode) ? FS_AM_DIR : FS_AM_ARC;
    if (!(st->st_mode & S_IWUSR))
        a |= FS_AM_RDO;
    if (name[0] == '.')
        a |= FS_AM_HID;
    fno->fattrib = a;
    fat_pack_time(st->st_mtime, &fno->fdate, &fno->ftime);
    fat_pack_time(st->st_ctime, &fno->crdate, &fno->crtime); /* no birth time */
}

/* ---- The drive, as core/api/dir.c asks for it ---------------------------- */

/* An open directory is just the platform's stream: an entry's metadata comes
 * back with its name, so nothing has to be rebuilt into a path and looked up
 * again. */
static struct
{
    bool used;
    void *dp;
} dirs[DIR_MAX_OPEN];

static bool drive_validate(int des, api_errno *err)
{
    if (des < 0 || des >= DIR_MAX_OPEN)
    {
        *err = API_EINVAL;
        return false;
    }
    if (!dirs[des].used)
    {
        *err = API_EBADF;
        return false;
    }
    return true;
}

static bool drive_stat(const char *path, FILINFO *fno, api_errno *err)
{
    char u8[DIR_UPATH_MAX];
    if (!path_to_utf8(path, u8, err))
        return false;
    struct stat st;
    if (!posix_ok(stat(u8, &st) == 0, err))
        return false;
    /* stat names a single entry; report its basename, not the whole path. */
    info_from_stat(fno, &st, path_basename(path));
    return true;
}

static bool drive_opendir(const char *path, int *des, api_errno *err)
{
    int i = 0;
    for (; i < DIR_MAX_OPEN; i++)
        if (!dirs[i].used)
            break;
    if (i == DIR_MAX_OPEN)
    {
        *err = API_EMFILE;
        return false;
    }
    char u8[DIR_UPATH_MAX];
    if (!path_to_utf8(path, u8, err))
        return false;
    if (!u8[0]) /* a directory of no name is the working directory */
        u8[0] = '.', u8[1] = 0;
    void *dp = posix_opendir(u8);
    if (!posix_ok(dp != NULL, err))
        return false;
    dirs[i].used = true;
    dirs[i].dp = dp;
    *des = i;
    return true;
}

/* "." and ".." are not entries the 6502 sees. */
static bool drive_readdir(int des, FILINFO *fno, api_errno *err)
{
    char u8name[DIR_UPATH_MAX];
    struct stat st;
    int r;
    do
    {
        r = posix_readdir(dirs[des].dp, u8name, sizeof u8name, &st);
        if (!posix_ok(r >= 0, err))
            return false;
        if (r == 0)
        {
            memset(fno, 0, sizeof(*fno)); /* fname[0]==0 signals EOF */
            return true;
        }
    } while (strcmp(u8name, ".") == 0 || strcmp(u8name, "..") == 0);
    char name[DIR_NAME_MAX];
    oem_from_utf8(u8name, name, sizeof name); /* truncation caps, like snprintf */
    info_from_stat(fno, &st, name);
    return true;
}

static bool drive_closedir(int des, api_errno *err)
{
    (void)err;
    posix_closedir(dirs[des].dp);
    dirs[des].used = false;
    dirs[des].dp = NULL;
    return true;
}

static bool drive_rewinddir(int des, api_errno *err)
{
    (void)err;
    posix_rewinddir(dirs[des].dp);
    return true;
}

static bool drive_unlink(const char *path, api_errno *err)
{
    char u8[DIR_UPATH_MAX];
    if (!path_to_utf8(path, u8, err))
        return false;
    return posix_ok(remove(u8) == 0, err); /* a file or an empty directory */
}

static bool drive_rename(const char *oldname, const char *newname, api_errno *err)
{
    char u8old[DIR_UPATH_MAX], u8new[DIR_UPATH_MAX];
    if (!path_to_utf8(oldname, u8old, err) || !path_to_utf8(newname, u8new, err))
        return false;
    return posix_ok(rename(u8old, u8new) == 0, err); /* replaces an existing target */
}

static bool drive_mkdir(const char *path, api_errno *err)
{
    char u8[DIR_UPATH_MAX];
    if (!path_to_utf8(path, u8, err))
        return false;
    return posix_ok(mkdir(u8, 0777) == 0, err);
}

static bool drive_chdir(const char *path, api_errno *err)
{
    char u8[DIR_UPATH_MAX];
    if (!path_to_utf8(path, u8, err))
        return false;
    /* chdir validates existence and dir-ness, and sets errno */
    return posix_ok(chdir(u8) == 0, err);
}

/* The 6502 sees MSC0: (and the bare current drive); anything else is a
 * missing device. */
static bool drive_chdrive(const char *drive, api_errno *err)
{
    if (drive[0] != ':') /* the null drive (installs) is not a cwd-able drive */
    {
        char name[16];
        size_t i = 0;
        for (; drive[i] && drive[i] != ':' && i < sizeof(name) - 1; i++)
            name[i] = drive[i];
        name[i] = 0;
        if (name[0] == 0 || strcasecmp(name, "MSC0") == 0)
            return true;
    }
    *err = API_ENODEV;
    return false;
}

/* Best-effort: only the read-only bit maps to a POSIX filesystem (write
 * permission). Hidden/system/archive have no equivalent and are silently
 * dropped -- including the path, which is not worth resolving to change
 * nothing. */
static bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    if (!(mask & FS_AM_RDO))
        return true;
    char u8[DIR_UPATH_MAX];
    if (!path_to_utf8(path, u8, err))
        return false;
    struct stat st;
    if (!posix_ok(stat(u8, &st) == 0, err))
        return false;
    mode_t m = st.st_mode & 07777;
    if (attr & FS_AM_RDO)
        m &= ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH);
    else
        m |= S_IWUSR;
    return posix_ok(chmod(u8, m) == 0, err);
}

/* Best-effort: set the modification time from the FAT date/time. The creation
 * time the API also carries is not settable on POSIX. */
static bool drive_utime(const char *path, const FILINFO *fno, api_errno *err)
{
    char u8[DIR_UPATH_MAX];
    if (!path_to_utf8(path, u8, err))
        return false;
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
    return posix_ok(utime(u8, &ub) == 0, err);
}

static bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    char u8[DIR_UPATH_MAX], native[HOST_MAX_PATH], cwd[HOST_MAX_PATH];
    if (!posix_ok(getcwd(u8, sizeof u8) != NULL, err))
        return false;
    if (oem_from_utf8(u8, native, sizeof native) >= sizeof native ||
        !path_from_native(native, cwd, sizeof cwd) ||
        strlen(cwd) >= size) /* did not fit: full-path-or-error */
    {
        *err = API_ENOMEM;
        return false;
    }
    strcpy(buf, cwd);
    return true;
}

/* A POSIX filesystem has no FAT volume label. Report an empty one and accept
 * (ignore) a set, so label-aware programs run rather than erroring -- these
 * are answers, not missing calls, which is why neither slot is left NULL. */
static bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err)
{
    (void)path, (void)size, (void)err;
    label[0] = 0;
    return true;
}

static bool drive_setlabel(const char *path, api_errno *err)
{
    (void)path, (void)err;
    return true;
}

static bool drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect,
                          api_errno *err)
{
    /* A drive query names a drive, and no name is the one in use -- the same
     * rule opendir follows, and what f_getfree does with "". */
    char u8[DIR_UPATH_MAX];
    if (!path_to_utf8(path[0] ? path : ".", u8, err))
        return false;
    struct statvfs vfs;
    if (!posix_ok(statvfs(u8, &vfs) == 0, err))
        return false;
    uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    uint64_t tot = ((uint64_t)vfs.f_blocks * unit) / 512;
    uint64_t fre = ((uint64_t)vfs.f_bavail * unit) / 512;
    *tot_sect = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    *fre_sect = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    return true;
}

/* A POSIX filesystem takes filenames as bytes; there is no page to set. */
void oem_fs_code_page(uint16_t cp)
{
    (void)cp;
}

const dir_backend_t drive_backend = {
    .stat = drive_stat,
    .unlink = drive_unlink,
    .rename = drive_rename,
    .mkdir = drive_mkdir,
    .chdir = drive_chdir,
    .chdrive = drive_chdrive,
    .chmod = drive_chmod,
    .utime = drive_utime,
    .getfree = drive_getfree,
    .getcwd = drive_getcwd,
    .getlabel = drive_getlabel,
    .setlabel = drive_setlabel,
    .opendir = drive_opendir,
    .readdir = drive_readdir,
    .closedir = drive_closedir,
    .rewinddir = drive_rewinddir,
    .validate = drive_validate,
};
