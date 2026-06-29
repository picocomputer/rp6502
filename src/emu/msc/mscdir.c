/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSC0: directory + metadata: stat, the opendir/readdir family, free space, and
 * the path mutations (unlink/rename/mkdir/chmod/utime/label). The firmware routes
 * these straight to FatFs, so there is one backing — the host filesystem — and no
 * driver table; dir.c calls in here. Paths translate through mscpath.c; host
 * timestamps and modes are mapped to the FatFs FILINFO field set the 6502 reads.
 * ROM:/overlays are not enumerable, so they never appear here.
 */

#include "emu/api/api.h"
#include "emu/msc/mscdir.h"
#include "emu/msc/mscpath.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

/* FAT attribute bits (FatFs AM_*), as the 6502 sees them in fs_info_t.attrib. */
#define FS_AM_RDO 0x01
#define FS_AM_HID 0x02
#define FS_AM_SYS 0x04
#define FS_AM_DIR 0x10
#define FS_AM_ARC 0x20

#define MSC_MAX_DIR 8

/* Open directory handles: a host DIR* plus an entry-index counter (mirrors the
 * firmware tells[]) so telldir/seekdir work. */
struct msc_dir
{
    bool used;
    DIR *dp;
    char host[FS_HOST_MAX_PATH]; /* the opened path, to stat each entry + rewind */
    long pos;                    /* entries read so far */
};
static struct msc_dir dirs[MSC_MAX_DIR];

/* Close every open directory (machine reset). */
void mscdir_reset(void)
{
    for (int i = 0; i < MSC_MAX_DIR; i++)
    {
        if (dirs[i].used && dirs[i].dp)
            closedir(dirs[i].dp);
        dirs[i].used = false;
        dirs[i].dp = NULL;
    }
}

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

static void info_from_stat(fs_info_t *info, const struct stat *st, const char *name)
{
    snprintf(info->name, sizeof(info->name), "%s", name);
    info->altname[0] = 0; /* host has no 8.3 short name */
    info->size = st->st_size > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)st->st_size;
    info->attrib = fat_attrib(st, name);
    fat_pack_time(st->st_mtime, &info->mdate, &info->mtime);
    fat_pack_time(st->st_ctime, &info->cdate, &info->ctime);
}

int fs_stat(const char *path, fs_info_t *info)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return -1;
    struct stat st;
    if (stat(host, &st) != 0)
        return -1;
    /* f_stat names a single entry; report its basename, not the whole path. */
    const char *base = strrchr(host, '/');
    info_from_stat(info, &st, base ? base + 1 : host);
    return 0;
}

int fs_opendir(const char *path)
{
    int des = 0;
    for (; des < MSC_MAX_DIR; des++)
        if (!dirs[des].used)
            break;
    if (des == MSC_MAX_DIR)
    {
        errno = EMFILE;
        return -1;
    }
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return -1;
    DIR *dp = opendir(host);
    if (!dp)
        return -1;
    dirs[des].used = true;
    dirs[des].dp = dp;
    dirs[des].pos = 0;
    snprintf(dirs[des].host, sizeof(dirs[des].host), "%s", host);
    return des;
}

static struct msc_dir *dir_slot(int des)
{
    if (des < 0 || des >= MSC_MAX_DIR)
    {
        errno = EINVAL;
        return NULL;
    }
    if (!dirs[des].used)
    {
        errno = EBADF;
        return NULL;
    }
    return &dirs[des];
}

int fs_readdir(int des, fs_info_t *info)
{
    struct msc_dir *d = dir_slot(des);
    if (!d)
        return -1;
    struct dirent *de;
    do
    {
        errno = 0;
        de = readdir(d->dp);
        if (!de)
        {
            if (errno)
                return -1;                  /* a real error, not end-of-dir */
            memset(info, 0, sizeof(*info)); /* name[0]==0 signals EOF */
            return 0;
        }
    } while (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0);
    /* Host entries only: overlays are not real files here, so they never list. */
    d->pos++;
    char entry[FS_HOST_MAX_PATH];
    struct stat st;
    if (snprintf(entry, sizeof(entry), "%s/%s", d->host, de->d_name) < (int)sizeof(entry) &&
        stat(entry, &st) == 0)
        info_from_stat(info, &st, de->d_name);
    else
    {
        memset(info, 0, sizeof(*info)); /* unstattable entry: name + dir guess */
        snprintf(info->name, sizeof(info->name), "%s", de->d_name);
        info->attrib = (de->d_type == DT_DIR) ? FS_AM_DIR : FS_AM_ARC;
    }
    return 0;
}

int fs_closedir(int des)
{
    struct msc_dir *d = dir_slot(des);
    if (!d)
        return -1;
    closedir(d->dp);
    d->used = false;
    d->dp = NULL;
    return 0;
}

long fs_telldir(int des)
{
    struct msc_dir *d = dir_slot(des);
    if (!d)
        return -1;
    return d->pos;
}

int fs_rewinddir(int des)
{
    struct msc_dir *d = dir_slot(des);
    if (!d)
        return -1;
    rewinddir(d->dp);
    d->pos = 0;
    return 0;
}

/* Seek by entry index, mirroring the firmware: rewind if going back, then read
 * forward to the target. Fails (EINVAL) if the target is past the end. */
int fs_seekdir(int des, long off)
{
    struct msc_dir *d = dir_slot(des);
    if (!d)
        return -1;
    if (off < 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (d->pos > off)
    {
        rewinddir(d->dp);
        d->pos = 0;
    }
    while (d->pos < off)
    {
        fs_info_t info;
        if (fs_readdir(des, &info) != 0)
            return -1;
        if (!info.name[0]) /* hit EOF before reaching off */
        {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

int fs_unlink(const char *path)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return -1;
    return remove(host);
}

int fs_rename(const char *oldp, const char *newp)
{
    char ho[FS_HOST_MAX_PATH], hn[FS_HOST_MAX_PATH];
    if (!fs_to_host(oldp, ho, sizeof(ho)) || !fs_to_host(newp, hn, sizeof(hn)))
        return -1;
    return rename(ho, hn);
}

int fs_mkdir(const char *path)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return -1;
    return mkdir(host, 0777);
}

int fs_getfree(const char *path, uint32_t *free_sectors, uint32_t *total_sectors)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return -1;
    struct statvfs vfs;
    if (statvfs(host, &vfs) != 0)
        return -1;
    uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    uint64_t tot = (uint64_t)vfs.f_blocks * unit / 512;
    uint64_t fre = (uint64_t)vfs.f_bavail * unit / 512;
    *total_sectors = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    *free_sectors = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    return 0;
}

/* Best-effort: only the read-only bit maps to the host (write permission).
 * Hidden/system/archive have no host equivalent and are silently dropped. */
int fs_chmod(const char *path, uint8_t attr, uint8_t mask)
{
    if (!(mask & FS_AM_RDO))
        return 0;
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return -1;
    struct stat st;
    if (stat(host, &st) != 0)
        return -1;
    mode_t m = st.st_mode & 07777;
    if (attr & FS_AM_RDO)
        m &= ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH);
    else
        m |= S_IWUSR;
    return chmod(host, m);
}

/* Best-effort: set the modification time from the FAT date/time. The creation
 * time the API also carries is not settable on POSIX, so it is ignored. */
int fs_utime(const char *path, uint16_t fdate, uint16_t ftime)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return -1;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = ((fdate >> 9) & 0x7F) + 1980 - 1900;
    tm.tm_mon = ((fdate >> 5) & 0x0F) - 1;
    tm.tm_mday = fdate & 0x1F;
    tm.tm_hour = (ftime >> 11) & 0x1F;
    tm.tm_min = (ftime >> 5) & 0x3F;
    tm.tm_sec = (ftime & 0x1F) * 2;
    tm.tm_isdst = -1;
    struct utimbuf ub;
    ub.actime = ub.modtime = mktime(&tm);
    return utime(host, &ub);
}

/* The host filesystem has no FAT volume label; report an empty one and accept
 * (ignore) a set, so label-aware programs run rather than erroring. */
int fs_getlabel(const char *path, char *label, size_t sz)
{
    (void)path;
    if (sz)
        label[0] = 0;
    return 0;
}

int fs_setlabel(const char *path)
{
    (void)path;
    return 0;
}
