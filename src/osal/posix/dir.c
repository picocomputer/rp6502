/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drive, as osal/dir.h asks for it: the directory syscalls
 * answered over a POSIX filesystem.
 *
 * The 6502 asks in FAT's vocabulary -- attribute bits and a 1980-epoch date,
 * which is what the API has always spoken. A POSIX host has none of that
 * natively, so this is where struct stat becomes an f_stat_t, in a single
 * step: the host says what it knows, in the terms the answer is wanted in.
 *
 * Paths cross spelled the way the 6502 spells them and in its OEM code page.
 * The drive prefix comes off with path_to_native() and the code page with
 * oem_to_utf8() (core/str/oem.h) before every libc call; returned names go
 * back with oem_from_utf8().
 */

#include "osal/dir.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include "osal/os.h"
#include "osal/posix/errmap.h"
#include <dirent.h>
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

#define DIR_NAME_MAX 256 /* an entry's name, not a path */

/* A path arrives spelled the way the 6502 spells it. This drive is one
 * directory of a real filesystem, so the drive prefix comes off here and what
 * is left is the native path -- and then the code page comes off too.
 *
 * Allocated to fit rather than capped: path_to_native never grows a path, and
 * oem_to_utf8 answers how much room it wants, so both lengths are known
 * instead of guessed. The caller frees. */
char *path_to_utf8(const char *path, api_errno *err)
{
    size_t nsz = strlen(path) + 1;
    char *native = malloc(nsz);
    if (!native)
    {
        *err = API_ENOMEM;
        return NULL;
    }
    if (!path_to_native(path, native, nsz))
    {
        free(native);
        *err = API_EINVAL;
        return NULL;
    }
    size_t usz = oem_to_utf8(native, NULL, 0) + 1;
    char *u8 = malloc(usz);
    if (u8)
        oem_to_utf8(native, u8, usz);
    else
        *err = API_ENOMEM;
    free(native);
    return u8;
}

/* And back: what the OS answered, spelled for the 6502. One length answers
 * for both steps -- oem_from_utf8 contracts and path_from_native prepends at
 * most a six-byte drive prefix. */
char *path_from_utf8(const char *u8, api_errno *err)
{
    size_t sz = strlen(u8) + 7;
    char *native = malloc(sz), *out = malloc(sz);
    if (native && out)
    {
        oem_from_utf8(u8, native, sz);
        if (!path_from_native(native, out, sz))
        {
            free(out);
            out = NULL;
            *err = API_EINVAL; /* a name too long, as errno_to_api spells it */
        }
    }
    else
    {
        free(out);
        out = NULL;
        *err = API_ENOMEM;
    }
    free(native);
    return out;
}

/* Absolute, in the 6502's spelling -- what argv[0] needs to survive a chdir.
 * Resolved against the cwd drive_getcwd answers for, which is why it is here.
 * No error channel: a caller has a path or it has nothing. */
char *os_dir_realpath(const char *path)
{
    api_errno ignored;
    char *u8 = path_to_utf8(path, &ignored);
    if (!u8)
        return NULL;
    char *r = realpath(u8, NULL);
    free(u8);
    if (!r)
        return NULL;
    char *out = path_from_utf8(r, &ignored);
    free(r);
    return out;
}

char *os_dir_path_hold(const char *path)
{
    return strdup(path);
}

void os_dir_path_drop(char *path)
{
    free(path);
}

/* Whatever the last call set, in the API's words. */
static bool posix_ok(bool ok, api_errno *err)
{
    if (!ok)
        *err = errno_to_api(errno);
    return ok;
}

/* ---- f_stat_t, from what POSIX keeps ------------------------------------- */

/* FAT attribute bits, as the 6502 sees them in f_stat_t.fattrib. */
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
    os_localtime(t, &tm);
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
static void info_from_stat(f_stat_t *info, const struct stat *st, const char *name)
{
    snprintf(info->fname, sizeof(info->fname), "%s", name);
    info->altname[0] = 0; /* no 8.3 short name here */
    info->fsize = (uint64_t)st->st_size > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)st->st_size;
    uint8_t a = S_ISDIR(st->st_mode) ? FS_AM_DIR : FS_AM_ARC;
    if (!(st->st_mode & S_IWUSR))
        a |= FS_AM_RDO;
    if (name[0] == '.')
        a |= FS_AM_HID;
    info->fattrib = a;
    fat_pack_time(st->st_mtime, &info->fdate, &info->ftime);
    fat_pack_time(st->st_ctime, &info->crdate, &info->crtime); /* no birth time */
}

/* ---- The walk, in POSIX's own terms -------------------------------------- */

/* An entry that cannot be stat'd is still an entry, so st is synthesized from
 * what the directory itself said -- the type, and nothing else true. */
static void *posix_opendir(const char *u8path)
{
    return opendir(u8path);
}

static int posix_readdir(void *d, char *u8name, size_t namesz, struct stat *st)
{
    DIR *dp = (DIR *)d;
    errno = 0;
    struct dirent *de = readdir(dp);
    if (!de)
        return errno ? -1 : 0; /* errno set -> a real error, else end-of-directory */
    snprintf(u8name, namesz, "%s", de->d_name);
    if (fstatat(dirfd(dp), de->d_name, st, 0) != 0)
    {
        memset(st, 0, sizeof(*st));
        st->st_mode = (de->d_type == DT_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    }
    return 1;
}

static void posix_rewinddir(void *d)
{
    rewinddir((DIR *)d);
}

static void posix_closedir(void *d)
{
    closedir((DIR *)d);
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

bool drive_validate(int des, api_errno *err)
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

bool drive_stat(const char *path, f_stat_t *info, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    struct stat st;
    bool ok = posix_ok(stat(u8, &st) == 0, err);
    free(u8);
    if (!ok)
        return false;
    /* stat names a single entry; report its basename, not the whole path. */
    info_from_stat(info, &st, path_basename(path));
    return true;
}

bool drive_opendir(const char *path, int *des, api_errno *err)
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
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    /* a directory of no name is the working directory */
    void *dp = posix_opendir(u8[0] ? u8 : ".");
    bool ok = posix_ok(dp != NULL, err);
    free(u8);
    if (!ok)
        return false;
    dirs[i].used = true;
    dirs[i].dp = dp;
    *des = i;
    return true;
}

/* "." and ".." are not entries the 6502 sees. */
bool drive_readdir(int des, f_stat_t *info, api_errno *err)
{
    char u8name[3 * DIR_NAME_MAX]; /* any name whose OEM form fits below */
    struct stat st;
    int r;
    do
    {
        r = posix_readdir(dirs[des].dp, u8name, sizeof u8name, &st);
        if (!posix_ok(r >= 0, err))
            return false;
        if (r == 0)
        {
            memset(info, 0, sizeof(*info)); /* fname[0]==0 signals EOF */
            return true;
        }
    } while (strcmp(u8name, ".") == 0 || strcmp(u8name, "..") == 0);
    char name[DIR_NAME_MAX];
    oem_from_utf8(u8name, name, sizeof name); /* truncation caps, like snprintf */
    info_from_stat(info, &st, name);
    return true;
}

bool drive_closedir(int des, api_errno *err)
{
    (void)err;
    posix_closedir(dirs[des].dp);
    dirs[des].used = false;
    dirs[des].dp = NULL;
    return true;
}

bool drive_rewinddir(int des, api_errno *err)
{
    (void)err;
    posix_rewinddir(dirs[des].dp);
    return true;
}

bool drive_unlink(const char *path, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    bool ok = posix_ok(remove(u8) == 0, err);
    free(u8);
    return ok; /* a file or an empty directory */
}

bool drive_rename(const char *oldname, const char *newname, api_errno *err)
{
    char *u8old = path_to_utf8(oldname, err);
    if (!u8old)
        return false;
    char *u8new = path_to_utf8(newname, err);
    if (!u8new)
    {
        free(u8old);
        return false;
    }
    bool ok = posix_ok(rename(u8old, u8new) == 0, err); /* replaces an existing target */
    free(u8old), free(u8new);
    return ok;
}

bool drive_mkdir(const char *path, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    bool ok = posix_ok(mkdir(u8, 0777) == 0, err);
    free(u8);
    return ok;
}

bool drive_chdir(const char *path, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    /* chdir validates existence and dir-ness, and sets errno */
    bool ok = posix_ok(chdir(u8) == 0, err);
    free(u8);
    return ok;
}

/* The 6502 sees MSC0: (and the bare current drive); anything else is a
 * missing device. */
bool drive_chdrive(const char *drive, api_errno *err)
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
bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    if (!(mask & FS_AM_RDO))
        return true;
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    struct stat st;
    bool ok = posix_ok(stat(u8, &st) == 0, err);
    if (ok)
    {
        mode_t m = st.st_mode & 07777;
        if (attr & FS_AM_RDO)
            m &= ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH);
        else
            m |= S_IWUSR;
        ok = posix_ok(chmod(u8, m) == 0, err);
    }
    free(u8);
    return ok;
}

/* Best-effort: set the modification time from the FAT date/time. The creation
 * time the API also carries is not settable on POSIX. */
bool drive_utime(const char *path, const f_stat_t *info, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = ((info->fdate >> 9) & 0x7F) + 1980 - 1900;
    tm.tm_mon = ((info->fdate >> 5) & 0x0F) - 1;
    tm.tm_mday = info->fdate & 0x1F;
    tm.tm_hour = (info->ftime >> 11) & 0x1F;
    tm.tm_min = (info->ftime >> 5) & 0x3F;
    tm.tm_sec = (info->ftime & 0x1F) * 2;
    tm.tm_isdst = -1;
    struct utimbuf ub;
    ub.actime = ub.modtime = mktime(&tm);
    bool ok = posix_ok(utime(u8, &ub) == 0, err);
    free(u8);
    return ok;
}

bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    char *u8 = getcwd(NULL, 0); /* the OS says how long its own answer is */
    if (!posix_ok(u8 != NULL, err))
        return false;
    /* oem_from_utf8 contracts and path_from_native prepends at most a
     * six-byte drive prefix, so one length answers for both steps. */
    size_t sz = strlen(u8) + 7;
    char *native = malloc(sz), *cwd = malloc(sz);
    bool ok = native && cwd;
    if (ok)
    {
        oem_from_utf8(u8, native, sz);
        ok = path_from_native(native, cwd, sz) &&
             strlen(cwd) < size; /* did not fit: full-path-or-error */
    }
    if (ok)
        strcpy(buf, cwd);
    else
        *err = API_ENOMEM;
    free(u8), free(native), free(cwd);
    return ok;
}

/* A POSIX filesystem has no FAT volume label. Report an empty one and accept
 * (ignore) a set, so label-aware programs run rather than erroring -- these
 * are answers, not missing calls, which is why neither slot is left NULL. */
bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err)
{
    (void)path, (void)size, (void)err;
    label[0] = 0;
    return true;
}

bool drive_setlabel(const char *path, api_errno *err)
{
    (void)path, (void)err;
    return true;
}

bool drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect,
                          api_errno *err)
{
    /* A drive query names a drive, and no name is the one in use -- the same
     * rule opendir follows, and what f_getfree does with "". */
    char *u8 = path_to_utf8(path[0] ? path : ".", err);
    if (!u8)
        return false;
    struct statvfs vfs;
    bool ok = posix_ok(statvfs(u8, &vfs) == 0, err);
    free(u8);
    if (!ok)
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

