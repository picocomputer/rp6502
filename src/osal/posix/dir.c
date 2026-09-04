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
 * Paths cross in the 6502's OEM code page, and may carry this drive's name.
 * strip_drive() takes the name off and oem_to_utf8() (core/str/oem.h) the code
 * page, before every libc call; returned names go back with oem_from_utf8().
 * Nothing puts a name back on: a POSIX path has no device in it, so what this
 * drive answers with is what getcwd(3) said.
 */

#include "osal/dir.h"
#include "core/str/oem.h"
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
#include <fcntl.h>
#include <unistd.h>

#define DIR_NAME_MAX 256 /* an entry's name, not a path */

/* Past this drive's name, if the path carries one. There is one filesystem
 * here and FS: is what it answers to; a path without it is already native,
 * which is what lets a host path from a command line go straight through. */
static const char *strip_drive(const char *path)
{
    return strncasecmp(path, "FS:", 3) == 0 ? path + 3 : path;
}

/* A path arrives spelled the way the 6502 spells it. This drive is a real
 * filesystem, so the name comes off here and what is left is the native path
 * -- and then the code page comes off too.
 *
 * Allocated to fit rather than capped: oem_to_utf8 answers how much room it
 * wants, so the length is known instead of guessed. The caller frees. */
char *path_to_utf8(const char *path, api_errno *err)
{
    const char *native = strip_drive(path);
    /* A leading ":" is the null drive, where installed ROMs live. It has no
     * native spelling at all, so neither ":name" nor "FS::name" can be made to
     * land on a real file; the ROM loader reaches installs its own way. Asked
     * after the strip, which is what makes the second spelling fail too. */
    if (native[0] == ':')
    {
        *err = API_ENODEV; /* FR_INVALID_DRIVE, as the Pico spells it */
        return NULL;
    }
    /* A byte the code page cannot spell would be substituted, and a
     * substituted name is a different name. FatFs refuses it; so does this.
     * The length goes with it: API_PATH_MAX is what the machine this API was
     * written for holds, and taking more here was the only way the hosts
     * differed. */
    if (strlen(native) > API_PATH_MAX || !oem_maps_oem(native))
    {
        *err = API_EINVAL; /* FR_INVALID_NAME, as the Pico spells it */
        return NULL;
    }
    size_t usz = oem_to_utf8(native, NULL, 0) + 1;
    char *u8 = malloc(usz);
    if (u8)
        oem_to_utf8(native, u8, usz);
    else
        *err = API_ENOMEM;
    return u8;
}

/* And back: what the OS answered, in the 6502's code page. Nothing is
 * prepended -- a POSIX path names no device -- and oem_from_utf8 only ever
 * contracts, so the host's own length bounds the answer. */
static char *path_from_utf8(const char *u8)
{
    size_t sz = strlen(u8) + 1;
    char *out = malloc(sz);
    if (out)
        oem_from_utf8(u8, out, sz);
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
    char *out = path_from_utf8(r);
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
 * time, FAT epoch 1980). The field holds 1980 to 2107 and nothing else, so
 * both ends clamp: a year past the top would otherwise carry out of the seven
 * bits and come back as a date in the 1980s, which reads as true. */
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
    if (year > 2107)
    {
        *fdate = (127 << 9) | (12 << 5) | 31; /* 2107-12-31 */
        *ftime = (23 << 11) | (59 << 5) | 29;
        return;
    }
    *fdate = (uint16_t)(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    *ftime = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
}

/* There are no FAT bits on a POSIX filesystem, so they are read off what is
 * there: a directory, archive on anything else, read-only when the owner
 * cannot write, hidden by the leading-dot convention. */
/* The creation time, where the filesystem keeps one. st_ctime is the inode
 * change time and is not a creation time in any sense a program could use, so
 * this asks for the real thing and answers with nothing when there is none --
 * which the API already spells as a zero date.
 *
 * Two platforms, two ways of asking, and neither is in struct stat on the
 * other: BSD and macOS put it there as st_birthtime, and Linux has it only
 * through statx, which wants the directory and name rather than the stat that
 * was already done. So this takes both and uses whichever its host has. */
static bool stat_birthtime(int dirfd, const char *name, const struct stat *st,
                           time_t *out)
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
    (void)dirfd, (void)name;
    *out = st->st_birthtime;
    return true;
#elif defined(__linux__) && defined(STATX_BTIME)
    (void)st;
    struct statx stx;
    /* Older kernels and some filesystems have no birth time; statx says so by
     * leaving the bit out of stx_mask rather than by failing. */
    if (statx(dirfd, name, AT_SYMLINK_NOFOLLOW * 0, STATX_BTIME, &stx) != 0 ||
        !(stx.stx_mask & STATX_BTIME))
        return false;
    *out = (time_t)stx.stx_btime.tv_sec;
    return true;
#else
    (void)dirfd, (void)name, (void)st, (void)out;
    return false;
#endif
}

static void info_from_stat(f_stat_t *info, const struct stat *st, const char *name,
                           int dirfd, const char *at)
{
    snprintf(info->fname, sizeof(info->fname), "%s", name);
    info->altname[0] = 0; /* no 8.3 short name here */
    /* A directory's size is 0 on FAT and on Win32; a POSIX host's own number
     * for one is how big the directory file is, which is not the same thing
     * and not what a program reading this field is asking. */
    info->fsize = S_ISDIR(st->st_mode)              ? 0
                  : (uint64_t)st->st_size > 0xFFFFFFFF ? 0xFFFFFFFF
                                                    : (uint32_t)st->st_size;
    uint8_t a = S_ISDIR(st->st_mode) ? FS_AM_DIR : FS_AM_ARC;
    if (!(st->st_mode & S_IWUSR))
        a |= FS_AM_RDO;
    if (name[0] == '.')
        a |= FS_AM_HID;
    info->fattrib = a;
    fat_pack_time(st->st_mtime, &info->fdate, &info->ftime);
    time_t birth;
    if (stat_birthtime(dirfd, at, st, &birth))
        fat_pack_time(birth, &info->crdate, &info->crtime);
    else
        info->crdate = info->crtime = 0;
}

/* ---- The walk, in POSIX's own terms -------------------------------------- */

static void *posix_opendir(const char *u8path)
{
    return opendir(u8path);
}

/* An entry whose metadata cannot be read is a failure, not an entry: every
 * field the 6502 reads would otherwise be made up here -- a size of zero, a
 * writable file, the FAT epoch -- and none of it distinguishable from an
 * entry that really is those things. */
static int posix_readdir(void *d, char *u8name, size_t namesz, struct stat *st,
                         int *dirfd_out)
{
    DIR *dp = (DIR *)d;
    errno = 0;
    struct dirent *de = readdir(dp);
    if (!de)
        return errno ? -1 : 0; /* errno set -> a real error, else end-of-directory */
    snprintf(u8name, namesz, "%s", de->d_name);
    *dirfd_out = dirfd(dp);
    if (fstatat(*dirfd_out, de->d_name, st, 0) != 0)
        return -1;
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
    if (ok)
    {
        /* stat names a single entry, and the name it reports is the one the
         * host resolved -- taken off the native path, not off the caller's
         * text, which still carries whatever drive name it was written with
         * and would answer "" for anything ending in a separator. That empty
         * name is what readdir uses to say end-of-directory. */
        size_t n = strlen(u8);
        while (n > 1 && u8[n - 1] == '/')
            u8[--n] = 0;
        const char *slash = strrchr(u8, '/');
        const char *base = slash && slash[1] ? slash + 1 : (slash ? slash : u8);
        char name[DIR_NAME_MAX];
        oem_from_utf8(base, name, sizeof name);
        info_from_stat(info, &st, name, AT_FDCWD, u8);
    }
    free(u8);
    return ok;
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
    int r, fd;
    do
    {
        r = posix_readdir(dirs[des].dp, u8name, sizeof u8name, &st, &fd);
        if (!posix_ok(r >= 0, err))
            return false;
        if (r == 0)
        {
            memset(info, 0, sizeof(*info)); /* fname[0]==0 signals EOF */
            return true;
        }
    } while (strcmp(u8name, ".") == 0 || strcmp(u8name, "..") == 0);
    /* A name the code page cannot spell has no name here, and reporting a
     * substituted one would let two entries arrive under one name and let a
     * program hand back a name that opens neither. */
    if (!oem_maps_utf8(u8name))
    {
        *err = API_EINVAL; /* FR_INVALID_NAME, as the Pico spells it */
        return false;
    }
    char name[DIR_NAME_MAX];
    oem_from_utf8(u8name, name, sizeof name); /* truncation caps, like snprintf */
    info_from_stat(info, &st, name, fd, u8name);
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
    /* A path of no name is the drive in use, the way f_chdir("0:") is a no-op
     * on FatFs. chdir validates existence and dir-ness, and sets errno. */
    bool ok = posix_ok(chdir(u8[0] ? u8 : ".") == 0, err);
    free(u8);
    return ok;
}

/* The 6502 sees FS: (and the bare current drive); anything else is a missing
 * device. Whatever the name leaves behind has to be nothing -- "FS:junk" names
 * no drive, and neither does the null drive. */
bool drive_chdrive(const char *drive, api_errno *err)
{
    const char *rest = strip_drive(drive);
    if (rest[0] == 0 && drive[0] != ':')
        return true;
    *err = API_ENODEV;
    return false;
}

/* Only the read-only bit maps to a POSIX filesystem (write permission).
 * Hidden/system/archive have no equivalent and are dropped -- but the path is
 * resolved either way, because a chmod of something that is not there is an
 * error on every other machine and answering otherwise would make the mask
 * decide whether a missing file exists. */
bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    struct stat st;
    bool ok = posix_ok(stat(u8, &st) == 0, err);
    if (ok && (mask & FS_AM_RDO))
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

/* Set the modification time from the FAT date/time. A date of 0 is invalid
 * and leaves the stamp alone, which is what the API promises and what f_utime
 * does -- and it is why this is utimensat rather than utime: UTIME_OMIT says
 * exactly that, and it also stops the access time being rewritten by a call
 * that was never about it. The creation time the API also carries is not
 * settable on POSIX; a machine that can set it does. */
bool drive_utime(const char *path, const f_stat_t *info, api_errno *err)
{
    struct timespec ts[2];
    ts[0].tv_sec = ts[1].tv_sec = 0;
    ts[0].tv_nsec = UTIME_OMIT; /* access time: never this call's business */
    ts[1].tv_nsec = UTIME_OMIT;
    if (info->fdate)
    {
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        tm.tm_year = ((info->fdate >> 9) & 0x7F) + 1980 - 1900;
        tm.tm_mon = ((info->fdate >> 5) & 0x0F) - 1;
        tm.tm_mday = info->fdate & 0x1F;
        tm.tm_hour = (info->ftime >> 11) & 0x1F;
        tm.tm_min = (info->ftime >> 5) & 0x3F;
        tm.tm_sec = (info->ftime & 0x1F) * 2;
        tm.tm_isdst = -1;
        ts[1].tv_sec = mktime(&tm);
        ts[1].tv_nsec = 0;
    }
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    /* Still resolved and still reported on, even when both stamps are left
     * alone: the API answers for the path, not only for the stamps. */
    bool ok = posix_ok(utimensat(AT_FDCWD, u8, ts, 0) == 0, err);
    free(u8);
    return ok;
}

/* What getcwd(3) said, in the guest's code page. No drive name goes in front
 * of it: this host's paths do not carry one, and answering as if they did is
 * the emulation this drive no longer does. */
bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    char *u8 = getcwd(NULL, 0); /* the OS says how long its own answer is */
    if (!posix_ok(u8 != NULL, err))
        return false;
    bool ok = oem_from_utf8(u8, buf, size) < size; /* full-path-or-error */
    if (!ok)
        *err = API_ENOMEM;
    free(u8);
    return ok;
}

/* A POSIX filesystem has no volume label. An empty one is the true answer and
 * FatFs's own word for unlabeled, so getlabel says that; there is no portable
 * name to say instead -- struct statfs carries an id and no name, and reaching
 * the real thing means walking the mount table to a device and asking a
 * library about it.
 *
 * Setting one is a different matter: there is nothing to set, and reporting
 * success for that is the kind of answer this drive no longer gives. */
bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err)
{
    (void)path, (void)size, (void)err;
    label[0] = 0;
    return true;
}

bool drive_setlabel(const char *path, api_errno *err)
{
    (void)path;
    *err = API_ENOSYS;
    return false;
}

bool drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect,
                          api_errno *err)
{
    /* A drive query names a drive, and no name is the one in use -- the same
     * rule opendir follows, and what f_getfree does with "". Asked after the
     * conversion, so that "FS:" is a drive query and not an empty path. */
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    struct statvfs vfs;
    bool ok = posix_ok(statvfs(u8[0] ? u8 : ".", &vfs) == 0, err);
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

