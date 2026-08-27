/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See drive.h.
 */

#include "core/api/api.h"
#include "core/api/dir.h"
#include "core/api/drive.h"
#include "core/api/fs.h"
#include <errno.h>
#include "core/str/path.h"
#include "host/api/dir.h"
#include "host/api/fs.h"
#include "host/os.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* A host filesystem takes filenames as bytes; there is no page to set. */
void oem_fs_code_page(uint16_t cp)
{
    (void)cp;
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

/* Synthesize FatFs attributes from host metadata (no FAT bits on the host, so:
 * directory, archive on files, read-only when unwritable, hidden per the platform). */
static uint8_t fat_attrib(const struct host_fs_meta *m)
{
    uint8_t a = m->is_dir ? FS_AM_DIR : FS_AM_ARC;
    if (m->is_readonly)
        a |= FS_AM_RDO;
    if (m->is_hidden)
        a |= FS_AM_HID;
    return a;
}

static void info_from_stat(FILINFO *fno, const struct host_fs_meta *m, const char *name)
{
    snprintf(fno->fname, sizeof(fno->fname), "%s", name);
    fno->altname[0] = 0; /* host has no 8.3 short name */
    fno->fsize = m->size > 0xFFFFFFFF ? 0xFFFFFFFF : (FSIZE_t)m->size;
    fno->fattrib = fat_attrib(m);
    fat_pack_time(m->mtime, &fno->fdate, &fno->ftime);
    fat_pack_time(m->crtime, &fno->crdate, &fno->crtime);
}

/* ---- Directory pool ------------------------------------------------------ */

/* A directory entry name as the host hands it back. NAME_MAX is 255 on the
 * hosts that have one, and a name longer than this cannot be reached through
 * a path the API can carry anyway. */
#define FS_MAX_NAME 256

/* An open directory is just the platform's stream: the host reports an
 * entry's metadata along with its name, so nothing has to be looked up
 * through a path afterwards. */
struct host_dir
{
    bool used;
    void *dp;
};
static struct host_dir dirs[DIR_MAX_OPEN];

/* ---- The drive, as core/api/dir.c asks for it ---------------------------- */

/* The metadata half of the host seam still reports by setting errno; this
 * funnel dies with the file, when each host answers dir_backend_t itself. */
static api_errno fs_errno_to_api_errno(int host_errno);

/* A host call reports by setting errno and returning false, so every one of
 * these ends the same way: whatever errno says, in the API's spelling. */
static inline bool host_ok(bool ok, api_errno *err)
{
    if (!ok)
        *err = fs_errno_to_api_errno(errno);
    return ok;
}

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
    struct host_fs_meta meta;
    if (!host_ok(host_fs_stat(path, &meta), err))
        return false;
    /* stat names a single entry; report its basename, not the whole path. */
    info_from_stat(fno, &meta, path_basename(path));
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
    void *dp = host_dir_open(path);
    if (!host_ok(dp != NULL, err))
        return false;
    dirs[i].used = true;
    dirs[i].dp = dp;
    *des = i;
    return true;
}

/* "." and ".." are not entries the 6502 sees. */
static bool drive_readdir(int des, FILINFO *fno, api_errno *err)
{
    struct host_dir *d = &dirs[des];
    char name[FS_MAX_NAME]; /* a directory entry name, not a full path */
    struct host_fs_meta meta;
    int r;
    do
    {
        r = host_dir_read(d->dp, name, sizeof(name), &meta);
        if (!host_ok(r >= 0, err))
            return false;
        if (r == 0)
        {
            memset(fno, 0, sizeof(*fno)); /* fname[0]==0 signals EOF */
            return true;
        }
    } while (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
    info_from_stat(fno, &meta, name);
    return true;
}

static bool drive_closedir(int des, api_errno *err)
{
    (void)err;
    host_dir_close(dirs[des].dp);
    dirs[des].used = false;
    dirs[des].dp = NULL;
    return true;
}

static bool drive_rewinddir(int des, api_errno *err)
{
    (void)err;
    host_dir_rewind(dirs[des].dp);
    return true;
}

static bool drive_unlink(const char *path, api_errno *err)
{
    return host_ok(host_fs_remove(path), err);
}

static bool drive_rename(const char *oldname, const char *newname, api_errno *err)
{
    return host_ok(host_fs_rename(oldname, newname), err);
}

static bool drive_mkdir(const char *path, api_errno *err)
{
    return host_ok(host_fs_mkdir(path), err);
}

static bool drive_chdir(const char *path, api_errno *err)
{
    /* chdir validates existence and dir-ness, and sets errno */
    return host_ok(host_fs_chdir(path), err);
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

/* Best-effort: only the read-only bit maps to the host (write permission).
 * Hidden/system/archive have no host equivalent and are silently dropped --
 * including the path, which is not worth resolving to change nothing. */
static bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    if (!(mask & FS_AM_RDO))
        return true;
    return host_ok(host_fs_set_readonly(path, (attr & FS_AM_RDO) != 0), err);
}

/* Best-effort: set the modification time from the FAT date/time. The creation
 * time the API also carries is not settable on POSIX. */
static bool drive_utime(const char *path, const FILINFO *fno, api_errno *err)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = ((fno->fdate >> 9) & 0x7F) + 1980 - 1900;
    tm.tm_mon = ((fno->fdate >> 5) & 0x0F) - 1;
    tm.tm_mday = fno->fdate & 0x1F;
    tm.tm_hour = (fno->ftime >> 11) & 0x1F;
    tm.tm_min = (fno->ftime >> 5) & 0x3F;
    tm.tm_sec = (fno->ftime & 0x1F) * 2;
    tm.tm_isdst = -1;
    return host_ok(host_fs_set_mtime(path, mktime(&tm)), err);
}

static bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    char cwd[HOST_MAX_PATH];
    if (!host_ok(host_fs_getcwd(cwd, sizeof(cwd)), err))
        return false;
    if (strlen(cwd) >= size) /* did not fit: full-path-or-error */
    {
        *err = API_ENOMEM;
        return false;
    }
    strcpy(buf, cwd);
    return true;
}

/* The host filesystem has no FAT volume label. Report an empty one and accept
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
    uint64_t tot_bytes, fre_bytes;
    if (!host_ok(host_fs_freespace(path, &tot_bytes, &fre_bytes), err))
        return false;
    uint64_t tot = tot_bytes / 512;
    uint64_t fre = fre_bytes / 512;
    *tot_sect = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    *fre_sect = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    return true;
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

static api_errno fs_errno_to_api_errno(int host_errno)
{
    switch (host_errno)
    {
    case ENOENT:
        return API_ENOENT;
    case EACCES:
    case EPERM:
    case EROFS:
        return API_EACCES;
    case EEXIST:
        return API_EEXIST;
    case EINVAL:
    case EISDIR:
    case ENOTDIR:
    case ENOTEMPTY:
    case ENAMETOOLONG:
        return API_EINVAL;
    case ENOSPC:
    case EFBIG:
        return API_ENOSPC;
    case EMFILE:
    case ENFILE:
        return API_EMFILE;
    case EBADF:
        return API_EBADF;
    case EBUSY:
        return API_EBUSY;
    case ENODEV:
    case ENXIO:
        return API_ENODEV;
    case EAGAIN:
        return API_EAGAIN;
    case ENOMEM:
        return API_ENOMEM;
    case ESPIPE:
        return API_ESPIPE;
    case ERANGE:
        return API_ERANGE;
    default:
        return API_EIO;
    }
}
