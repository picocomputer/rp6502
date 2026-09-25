/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The API uses FAT's attribute bits and 1980-epoch dates. A POSIX filesystem
 * has neither, so struct stat is converted to an f_stat_t here.
 *
 * Paths from a program are in the 6502's OEM code page and may start with
 * this drive's name. path_to_utf8 removes the name, checks the rest against
 * the FAT rules and converts the code page before every libc call, and names
 * from the host are converted with oem_from_utf8. drive_getcwd and
 * os_dir_realpath put the name back on, because an absolute path always
 * starts with its drive.
 */

#include "osal/dir.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include "osal/os.h"
#include "osal/posix/dir.h"
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
#ifdef __EMSCRIPTEN__
#include "osal/emscripten/os.h"
#endif

#define DIR_NAME_MAX 256 /* an entry's name, not a path */

/* FS: is the only drive here, so a path without it, such as a host path from
 * the command line, is already a host path. */
static const char *strip_drive(const char *path)
{
    return strncasecmp(path, "FS:", 3) == 0 ? path + 3 : path;
}

char *path_to_utf8(const char *path, api_errno *err)
{
    const char *native = strip_drive(path);
    if (!path_fat_ok(native, native != path, err))
        return NULL;
    /* A byte with no character in the code page would be substituted, and a
     * substituted name is a different name, so the path is refused with the
     * same error FatFs returns for it. */
    if (!oem_maps_oem(native))
    {
        *err = API_EINVAL;
        return NULL;
    }
    size_t usz = oem_to_utf8(native, NULL, 0) + 1;
    char *u8 = malloc(usz);
    if (!u8)
    {
        *err = API_ENOMEM;
        return NULL;
    }
    oem_to_utf8(native, u8, usz);
    /* FatFs separates with either slash, and POSIX with '/' alone. */
    for (char *p = u8; *p; p++)
        if (*p == '\\')
            *p = '/';
    return u8;
}

/* A '\\' inside a host name would be read by path_to_utf8 as a separator.
 * oem_from_utf8 writes one byte per UTF-8 sequence, so its output is never
 * longer than its input, and the source length bounds the allocation. */
char *path_from_host(const char *host)
{
    if (strchr(host, '\\') || !oem_maps_utf8(host))
        return NULL;
    size_t sz = 3 + strlen(host) + 1;
    char *out = malloc(sz);
    if (!out)
        return NULL;
    memcpy(out, "FS:", 3);
    oem_from_utf8(host, out + 3, sz - 3);
    api_errno ignored;
    if (!path_fat_ok(out + 3, true, &ignored))
    {
        free(out);
        return NULL;
    }
    return out;
}

/* A name or path from the host, as a program receives it. Each character that
 * the code page cannot hold or that FAT refuses in a name becomes 127, which
 * no path may contain, so the name is listed but cannot be opened. */
static size_t shown_from_utf8(const char *u8, char *dst, size_t size)
{
    size_t need = oem_from_utf8(u8, dst, size);
    for (char *p = dst; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("\"*:<>?|\\", *p))
            *p = 0x7F;
    return need;
}

char *os_dir_realpath(const char *path)
{
    api_errno ignored;
    char *u8 = path_to_utf8(path, &ignored);
    if (!u8)
        return NULL;
    char *r = realpath(u8, NULL);
    free(u8);
    char *out = r ? path_from_host(r) : NULL;
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

static bool posix_ok(bool ok, api_errno *err)
{
    if (!ok)
        *err = errno_to_api(errno);
    return ok;
}

#define FS_AM_RDO 0x01
#define FS_AM_HID 0x02
#define FS_AM_SYS 0x04
#define FS_AM_DIR 0x10
#define FS_AM_ARC 0x20

/* Pack a host time into the FatFs 16-bit date and time: local time, seven
 * bits of year from a 1980 epoch, and seconds in units of two. A year past
 * 2107 carries out of those seven bits and comes back as a believable date in
 * the 1980s, and one before 1980 goes negative, so both ends clamp to the
 * dates the field can hold. */
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

/* Whether this libc declares statx(). The kernel headers define STATX_BTIME
 * wherever they are new enough, which is not the same question: bionic has
 * the constant from API 28 and the function only from API 30, so a build
 * against the lower one fails at the call rather than falling back. */
#if defined(__linux__) && defined(STATX_BTIME)
#if defined(__ANDROID__)
#define OSAL_HAVE_STATX (__ANDROID_API__ >= 30)
#elif defined(__GLIBC__)
#define OSAL_HAVE_STATX \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 28))
#else
#define OSAL_HAVE_STATX 0
#endif
#else
#define OSAL_HAVE_STATX 0
#endif

/* The creation time, where the filesystem keeps one. st_ctime is the inode
 * change time and is not a creation time, so it is not used; the API already
 * uses a zero date for an unknown creation time.
 *
 * BSD and macOS keep it in struct stat as st_birthtime, while Linux has it
 * only through statx, which takes a directory descriptor and a name rather
 * than a stat already done. This takes both and uses whichever the host has. */
static bool stat_birthtime(int dirfd, const char *name, const struct stat *st,
                           time_t *out)
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
    (void)dirfd, (void)name;
    *out = st->st_birthtime;
    return true;
#elif OSAL_HAVE_STATX
    (void)st;
    struct statx stx;
    /* Older kernels and some filesystems have no birth time, and statx says
     * so by leaving the bit out of stx_mask rather than by failing. */
    if (statx(dirfd, name, 0, STATX_BTIME, &stx) != 0 ||
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
    info->altname[0] = 0;
    /* FAT and Win32 both report a directory's size as 0. A POSIX host reports
     * how large the directory file is, which is not what a program reading
     * this field is asking for. */
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

static void *posix_opendir(const char *u8path)
{
    return opendir(u8path);
}

/* An entry whose metadata cannot be read is a failure rather than an entry,
 * because every field would otherwise be invented here and would read as an
 * entry that really is empty, writable and stamped with the FAT epoch. */
static int posix_readdir(void *d, char *u8name, size_t namesz, struct stat *st,
                         int *dirfd_out)
{
    DIR *dp = (DIR *)d;
    errno = 0;
    struct dirent *de = readdir(dp);
    if (!de)
        return errno ? -1 : 0; /* readdir leaves errno alone at end of directory */
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

static struct
{
    bool used;
    void *dp;
    char path[API_PATH_MAX + 1];
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
    if (!u8[0]) /* an empty path or a bare drive name */
    {
        free(u8);
        *err = API_EINVAL;
        return false;
    }
    struct stat st, root;
    bool ok = posix_ok(stat(u8, &st) == 0, err);
    if (ok && stat("/", &root) == 0 && st.st_dev == root.st_dev &&
        st.st_ino == root.st_ino)
        f_stat_root(info);
    else if (ok)
    {
        /* The reported name is taken off the native path rather than off the
         * caller's text, which still carries whatever drive name it was
         * written with and would end up empty for a path ending in a
         * separator. An empty name is how readdir says end of directory. */
        size_t n = strlen(u8);
        while (n > 1 && u8[n - 1] == '/')
            u8[--n] = 0;
        const char *slash = strrchr(u8, '/');
        char name[DIR_NAME_MAX];
        oem_from_utf8(slash ? slash + 1 : u8, name, sizeof name);
        info_from_stat(info, &st, name, AT_FDCWD, u8);
    }
    free(u8);
    return ok;
}

static bool dir_open_into(int i, const char *path, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    bool here = !u8[0]; /* a directory of no name is the working directory */
    void *dp = posix_opendir(here ? "." : u8);
    bool ok = posix_ok(dp != NULL, err);
    free(u8);
    if (!ok)
        return false;
    dirs[i].used = true;
    dirs[i].dp = dp;
    dirs[i].path[0] = 0;
    char *abs = os_dir_realpath(here ? "." : path);
    if (abs)
    {
        if (strlen(abs) <= API_PATH_MAX)
            strcpy(dirs[i].path, abs);
        free(abs);
    }
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
    if (!dir_open_into(i, path, err))
        return false;
    *des = i;
    return true;
}

bool drive_dir_path(int des, char *buf, size_t size)
{
    if (des < 0 || des >= DIR_MAX_OPEN || !dirs[des].used ||
        !dirs[des].path[0] || strlen(dirs[des].path) >= size)
        return false;
    strcpy(buf, dirs[des].path);
    return true;
}

bool drive_reopendir(int des, const char *path, api_errno *err)
{
    if (des < 0 || des >= DIR_MAX_OPEN)
    {
        *err = API_EINVAL;
        return false;
    }
    if (dirs[des].used)
    {
        posix_closedir(dirs[des].dp);
        dirs[des].used = false;
        dirs[des].dp = NULL;
    }
    return dir_open_into(des, path, err);
}

bool drive_readdir(int des, f_stat_t *info, api_errno *err)
{
    /* One OEM byte becomes at most three UTF-8 bytes, so this holds any name
     * whose converted form fits DIR_NAME_MAX below. */
    char u8name[3 * DIR_NAME_MAX];
    struct stat st;
    int r, fd;
    do
    {
        r = posix_readdir(dirs[des].dp, u8name, sizeof u8name, &st, &fd);
        if (!posix_ok(r >= 0, err))
            return false;
        if (r == 0)
        {
            memset(info, 0, sizeof(*info)); /* an empty fname is end of directory */
            return true;
        }
    } while (strcmp(u8name, ".") == 0 || strcmp(u8name, "..") == 0);
    char name[DIR_NAME_MAX];
    shown_from_utf8(u8name, name, sizeof name);
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

/* POSIX removes a file regardless of its permissions, while f_unlink refuses
 * one marked read-only, which here is one without S_IWUSR. remove(3) removes
 * a file or an empty directory, as f_unlink does. */
bool drive_unlink(const char *path, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    struct stat st;
    bool ok = posix_ok(stat(u8, &st) == 0, err);
    if (ok && !(st.st_mode & S_IWUSR))
    {
        *err = API_EACCES;
        ok = false;
    }
    ok = ok && posix_ok(remove(u8) == 0, err);
    free(u8);
#ifdef __EMSCRIPTEN__
    if (ok)
        os_estimate_stale();
#endif
    return ok;
}

/* A file replaces only a file. rename(2) would also put a folder in place of
 * an empty one, and fails with other errors for the other combinations, so
 * those are refused first. The same file under a second name, such as a
 * change of case where the host ignores case, does not count as an existing
 * target, and a target on another mount is left for rename(2) to refuse. */
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
    struct stat so, sn;
    bool ok = posix_ok(stat(u8old, &so) == 0, err);
    if (ok && stat(u8new, &sn) == 0 && so.st_dev == sn.st_dev &&
        so.st_ino != sn.st_ino && (S_ISDIR(so.st_mode) || S_ISDIR(sn.st_mode)))
    {
        *err = API_EEXIST;
        ok = false;
    }
    ok = ok && posix_ok(rename(u8old, u8new) == 0, err);
    free(u8old), free(u8new);
#ifdef __EMSCRIPTEN__
    if (ok)
        os_estimate_stale();
#endif
    return ok;
}

bool drive_mkdir(const char *path, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    bool ok = posix_ok(mkdir(u8, 0777) == 0, err);
    free(u8);
#ifdef __EMSCRIPTEN__
    if (ok)
        os_estimate_stale();
#endif
    return ok;
}

bool drive_chdir(const char *path, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return false;
    /* A path of no name is the drive in use, the way f_chdir("0:") is a no-op
     * on FatFs. */
    bool ok = posix_ok(chdir(u8[0] ? u8 : ".") == 0, err);
    free(u8);
    return ok;
}

/* FS: and the bare current drive are the only drives, and anything else is a
 * missing device. The name must leave nothing behind it, so "FS:junk" names no
 * drive, and neither does the null drive. */
bool drive_chdrive(const char *drive, api_errno *err)
{
    if (!strip_drive(drive)[0])
        return true;
    *err = API_ENODEV;
    return false;
}

/* Only the read-only bit has a POSIX equivalent, which is write permission;
 * hidden, system and archive are dropped. The path is resolved whatever the
 * mask holds, because a chmod of something that is not there is an error on
 * every other machine, and skipping the lookup would let the mask decide
 * whether a missing file exists. */
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

/* Set the modification time from the FAT date and time. A date of 0 leaves
 * the stamp alone, which is what the API promises and what f_utime does, and
 * it is why this is utimensat rather than utime: UTIME_OMIT says exactly
 * that, and it also keeps the access time from being rewritten by a call that
 * was never about it. POSIX cannot set the creation time the API carries. */
bool drive_utime(const char *path, const f_stat_t *info, api_errno *err)
{
    struct timespec ts[2];
    ts[0].tv_sec = ts[1].tv_sec = 0;
    ts[0].tv_nsec = UTIME_OMIT;
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
    bool ok = posix_ok(utimensat(AT_FDCWD, u8, ts, 0) == 0, err);
    free(u8);
    return ok;
}

bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    char *u8 = getcwd(NULL, 0); /* getcwd allocates an answer of its own length */
    if (!posix_ok(u8 != NULL, err))
        return false;
    /* shown_from_utf8 returns the untruncated length, so a short buffer is
     * an error here rather than a path the caller cannot use. */
    bool ok = size > 3 && shown_from_utf8(u8, buf + 3, size - 3) < size - 3;
    if (ok)
        memcpy(buf, "FS:", 3);
    else
        *err = API_ENOMEM;
    free(u8);
    return ok;
}

/* Only the Pico has volume labels. The emulator neither reads nor renames the
 * host's, so both label calls refuse. */
bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err)
{
    (void)path, (void)label, (void)size;
    *err = API_EACCES;
    return false;
}

bool drive_setlabel(const char *path, api_errno *err)
{
    (void)path;
    *err = API_EACCES;
    return false;
}

std_rw_result drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect,
                            api_errno *err)
{
    /* A query of no name asks about the drive in use, which is what f_getfree
     * does with "". Asked after the conversion, so that "FS:" is a drive query
     * and not an empty path. */
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return STD_ERROR;
    struct statvfs vfs;
    bool ok = posix_ok(statvfs(u8[0] ? u8 : ".", &vfs) == 0, err);
    free(u8);
    if (!ok)
        return STD_ERROR;
#ifdef __EMSCRIPTEN__
    /* The browser's filesystem reports made-up sizes. What the page may store
     * is the quota the browser gives it, less what it already uses. */
    uint64_t quota, usage;
    os_estimate_start();
    std_rw_result r = os_estimate_poll(&quota, &usage, err);
    if (r != STD_OK)
        return r;
    uint64_t tot = quota / 512;
    uint64_t fre = (usage < quota ? quota - usage : 0) / 512;
#else
    uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    uint64_t tot = ((uint64_t)vfs.f_blocks * unit) / 512;
    uint64_t fre = ((uint64_t)vfs.f_bavail * unit) / 512;
#endif
    *tot_sect = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    *fre_sect = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    return STD_OK;
}

/* A POSIX filesystem takes filenames as bytes; there is no page to set. */
void oem_fs_code_page(uint16_t cp)
{
    (void)cp;
}

