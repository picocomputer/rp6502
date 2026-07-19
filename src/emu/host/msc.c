/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "ria/api/api.h"
#include "ria/api/std.h"
#include "emu/host/msc.h"
#include "emu/plat.h"
#include "emu/sys/mem.h"
#include "fatfs/ff.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define HOST_MAX_OPEN 16

/* An open host file: a plain fd, flagged once it is written so the drive is persisted
 * when it closes. The read/write transfer is non-blocking in the fs seam. */
struct host_file
{
    bool used;
    int fd;
    bool wrote;
    bool writable; /* opened for write: lseek past EOF extends (else it clamps) */
};
static struct host_file files[HOST_MAX_OPEN];

static struct host_file *host_fil(int desc)
{
    if (desc < 0 || desc >= HOST_MAX_OPEN || !files[desc].used)
        return NULL;
    return &files[desc];
}

/* flags are the rp6502 SDK open() bits (see ria/usb/msc.c). */
static int flags_to_posix(uint8_t flags)
{
    bool rd = flags & 0x01, wr = flags & 0x02;
    int o = wr ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (flags & 0x10) /* CREAT */
        o |= O_CREAT;
    if ((flags & 0x10) && (flags & 0x80)) /* CREAT + EXCL */
        o |= O_EXCL;
    if ((flags & 0x20) && wr) /* TRUNC, only when opened for write */
        o |= O_TRUNC;
    return o;
}

/* ---- Address translation: MSC0: <-> host path ---------------------------- */

/* Drop a recognized writable-drive prefix. FatFs recognizes only "0:".."9:" and
 * "MSC0:".."MSC9:" (case-insensitive); anything else keeps its prefix and is
 * treated as a relative name (the OS, not us, then rejects a bogus ":"). */
const char *msc_strip_drive(const char *path)
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

bool msc_has_drive_prefix(const char *path)
{
    return msc_strip_drive(path) != path;
}

/* Map a drive-stripped MSC0: path to a host path. "//C/..." names a Windows
 * drive; everything else is the native path verbatim — absolute "/x" from the OS
 * root, relative "x" from the process cwd. The OS resolves "." and "..". */
static bool rest_to_host(const char *rest, char *host, size_t hsz)
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

bool msc_to_host(const char *path, char *host, size_t hsz)
{
    const char *rest = msc_strip_drive(path);
    /* A leading ":" is the null drive (installed ROMs, install.c) — never a host
     * path. Refuse it here so neither ":name" nor "MSC0::name" can map onto a host
     * file; the boot/exec loader reaches installs via rom_resolve instead. */
    if (rest[0] == ':')
    {
        errno = ENOENT;
        return false;
    }
    return rest_to_host(rest, host, hsz);
}

/* Render a host path as an MSC0: path (the inverse for absolutes): a Windows
 * "C:/x" -> "MSC0://C/x", else the path tacked under MSC0:. Returns its length,
 * or 0 if it did not fit (the caller must treat 0 as a failure, never a short
 * path — getcwd is full-path-or-error). */
size_t msc_from_host(const char *hostpath, char *out, size_t outsz)
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

/* The fs backends report failures by setting POSIX errno; translate to the 6502 set. */
api_errno msc_errno_to_api_errno(int host_errno)
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

bool msc_std_handles(const char *path)
{
    (void)path;
    return true; /* catch-all, registered last */
}

int msc_std_open(const char *path, uint8_t flags, api_errno *err)
{
    char host[MSC_MAX_PATH];
    if (!msc_to_host(path, host, sizeof(host)))
    {
        *err = msc_errno_to_api_errno(errno);
        return -1;
    }
    int fd = fs_open(host, flags_to_posix(flags), 0666);
    if (fd < 0)
    {
        *err = msc_errno_to_api_errno(errno);
        return -1;
    }
    int des = 0;
    for (; des < HOST_MAX_OPEN; des++)
        if (!files[des].used)
            break;
    if (des == HOST_MAX_OPEN)
    {
        fs_close(fd);
        *err = API_EMFILE;
        return -1;
    }
    files[des] = (struct host_file){.used = true, .fd = fd, .writable = (flags & 0x02) != 0};
    if (flags & 0x40) /* APPEND: one-time seek to EOF (O_TRUNC already ran) */
        fs_lseek(fd, 0, SEEK_END);
    return des;
}

std_rw_result msc_std_close(int desc, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    if (!f)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    bool wrote = f->wrote;
    int rc = 0;
    if (f->fd >= 0)
        rc = fs_close(f->fd);
    f->used = false;
    if (wrote)
        fs_sync(); /* a saved file just closed: persist the drive (web: IDBFS) */
    if (rc != 0) /* deferred flush failure (ENOSPC/EIO on network/overlay FS) */
    {
        *err = msc_errno_to_api_errno(errno);
        return STD_ERROR;
    }
    return STD_OK;
}

std_rw_result msc_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    if (!f)
    {
        *got = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    std_rw_result r = fs_read(f->fd, buf, count, got);
    if (r == STD_ERROR)
        *err = msc_errno_to_api_errno(errno);
    return r;
}

std_rw_result msc_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    if (!f)
    {
        *put = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    std_rw_result r = fs_write(f->fd, buf, count, put);
    if (r == STD_OK)
        f->wrote = true;
    else if (r == STD_ERROR)
        *err = msc_errno_to_api_errno(errno);
    return r;
}

int msc_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    if (!f)
    {
        *err = API_EBADF;
        return -1;
    }
    /* The position is reported back as a signed 32-bit value (0xFFFFFFFF is the
     * error sentinel), so reject a target past 2GB-1 before moving the pointer,
     * leaving the file pointer where it was rather than at an unreportable spot. */
    int64_t cur = fs_lseek(f->fd, 0, SEEK_CUR);
    if (cur < 0)
    {
        *err = msc_errno_to_api_errno(errno);
        return -1;
    }
    int64_t base;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = cur;
    else if (whence == SEEK_END)
    {
        base = fs_lseek(f->fd, 0, SEEK_END);
        fs_lseek(f->fd, cur, SEEK_SET);
        if (base < 0)
        {
            *err = msc_errno_to_api_errno(errno);
            return -1;
        }
    }
    else
    {
        *err = API_EINVAL;
        return -1;
    }
    int64_t target = base + off;
    if (target < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    if (target > 0x7FFFFFFF)
    {
        *err = API_ERANGE;
        return -1;
    }
    /* Match FatFs f_lseek (firmware fat_std_lseek): a read-only file clamps the
     * pointer to its size; a writable file is extended to the target. Plain POSIX
     * lseek would leave a read pointer past EOF and defer any extension to the
     * next write, diverging from hardware. */
    int64_t size = fs_lseek(f->fd, 0, SEEK_END);
    if (size < 0)
    {
        *err = msc_errno_to_api_errno(errno);
        return -1;
    }
    if (target > size)
    {
        if (!f->writable)
            target = size; /* read mode: clamp to EOF */
        else if (fs_ftruncate(f->fd, target) < 0) /* write mode: extend the file */
        {
            *err = msc_errno_to_api_errno(errno);
            return -1;
        }
    }
    int64_t np = fs_lseek(f->fd, target, SEEK_SET);
    if (np < 0)
    {
        *err = msc_errno_to_api_errno(errno);
        return -1;
    }
    *pos = (int32_t)np;
    return 0;
}

std_rw_result msc_std_sync(int desc, api_errno *err)
{
    (void)desc, (void)err;
    fs_sync();
    return STD_OK;
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

/* Synthesize FatFs attributes from host metadata (no FAT bits on the host, so:
 * directory, archive on files, read-only when unwritable, hidden per the platform). */
static uint8_t fat_attrib(const struct fs_meta *m)
{
    uint8_t a = m->is_dir ? FS_AM_DIR : FS_AM_ARC;
    if (m->is_readonly)
        a |= FS_AM_RDO;
    if (m->is_hidden)
        a |= FS_AM_HID;
    return a;
}

static void info_from_stat(FILINFO *fno, const struct fs_meta *m, const char *name)
{
    snprintf(fno->fname, sizeof(fno->fname), "%s", name);
    fno->altname[0] = 0; /* host has no 8.3 short name */
    fno->fsize = m->size > 0xFFFFFFFF ? 0xFFFFFFFF : (FSIZE_t)m->size;
    fno->fattrib = fat_attrib(m);
    fat_pack_time(m->mtime, &fno->fdate, &fno->ftime);
    fat_pack_time(m->crtime, &fno->crdate, &fno->crtime);
}

/* Push one entry in the firmware FILINFO byte order: fields reversed so they land
 * forward in the 6502-visible struct (mirrors firmware dir_push_filinfo). */
static bool dir_push_filinfo(FILINFO *fno)
{
    bool ok = true;
    for (int i = FF_LFN_BUF; i >= 0; i--)
        ok &= api_push_char(&fno->fname[i]);
    for (int i = FF_SFN_BUF; i >= 0; i--)
        ok &= api_push_char(&fno->altname[i]);
    ok &= api_push_uint8(&fno->fattrib);
    ok &= api_push_uint16(&fno->crtime);
    ok &= api_push_uint16(&fno->crdate);
    ok &= api_push_uint16(&fno->ftime);
    ok &= api_push_uint16(&fno->fdate);
    uint32_t fsize = fno->fsize;
    if (fno->fsize > 0xFFFFFFFF)
        fsize = 0xFFFFFFFF;
    ok &= api_push_uint32(&fsize);
    return ok;
}

/* Fail the syscall from the current errno (mapped to an api_errno). */
static bool host_err(void)
{
    return api_return_errno(msc_errno_to_api_errno(errno));
}

/* ---- Directory pool ------------------------------------------------------ */

#define HOST_MAX_DIR 8

/* Open directory handles: an opaque platform stream plus the opened path (to stat
 * each entry) and an entry-index counter (mirrors the firmware tells[]). */
struct host_dir
{
    bool used;
    void *dp;
    char host[MSC_MAX_PATH];
    long pos;
};
static struct host_dir dirs[HOST_MAX_DIR];

void msc_stop(void)
{
    for (int i = 0; i < HOST_MAX_DIR; i++)
    {
        if (dirs[i].used && dirs[i].dp)
            dir_close(dirs[i].dp);
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

/* Read the next real directory entry (skipping "." / ".."), filling a FILINFO from
 * a stat of it (fname[0]==0 at end-of-directory). */
static int host_next_entry(int des, FILINFO *fno, api_errno *err)
{
    struct host_dir *d = dir_slot(des, err);
    if (!d)
        return -1;
    char name[256]; /* a directory entry name (<= NAME_MAX), not a full path */
    bool is_dir;
    int r;
    do
    {
        r = dir_read(d->dp, name, sizeof(name), &is_dir);
        if (r < 0)
        {
            *err = msc_errno_to_api_errno(errno);
            return -1;
        }
        if (r == 0)
        {
            memset(fno, 0, sizeof(*fno)); /* fname[0]==0 signals EOF */
            return 0;
        }
    } while (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
    d->pos++;
    char entry[MSC_MAX_PATH];
    struct fs_meta meta;
    if (snprintf(entry, sizeof(entry), "%s/%s", d->host, name) < (int)sizeof(entry) &&
        fs_stat(entry, &meta))
        info_from_stat(fno, &meta, name);
    else
    {
        memset(fno, 0, sizeof(*fno)); /* unstattable entry: name + dir guess */
        snprintf(fno->fname, sizeof(fno->fname), "%s", name);
        fno->fattrib = is_dir ? FS_AM_DIR : FS_AM_ARC;
    }
    return 0;
}

/* ---- The host dir syscall handlers (installed in the OP array) ------------ */

bool msc_api_stat(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    char host[MSC_MAX_PATH];
    if (!msc_to_host(path, host, sizeof(host)))
        return host_err();
    struct fs_meta meta;
    if (!fs_stat(host, &meta))
        return host_err();
    /* stat names a single entry; report its basename, not the whole path. */
    const char *base = strrchr(host, '/');
    FILINFO fno;
    info_from_stat(&fno, &meta, base ? base + 1 : host);
    if (!dir_push_filinfo(&fno))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool msc_api_opendir(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    int des = 0;
    for (; des < HOST_MAX_DIR; des++)
        if (!dirs[des].used)
            break;
    if (des == HOST_MAX_DIR)
        return api_return_errno(API_EMFILE);
    char host[MSC_MAX_PATH];
    if (!msc_to_host(path, host, sizeof(host)))
        return host_err();
    void *dp = dir_open(host);
    if (!dp)
        return host_err();
    dirs[des].used = true;
    dirs[des].dp = dp;
    dirs[des].pos = 0;
    snprintf(dirs[des].host, sizeof(dirs[des].host), "%s", host);
    return api_return_ax((uint16_t)des);
}

bool msc_api_readdir(void)
{
    FILINFO fno;
    api_errno err;
    if (host_next_entry(API_A, &fno, &err) < 0)
        return api_return_errno(err);
    if (!dir_push_filinfo(&fno))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool msc_api_closedir(void)
{
    api_errno err;
    struct host_dir *d = dir_slot(API_A, &err);
    if (!d)
        return api_return_errno(err);
    dir_close(d->dp);
    d->used = false;
    d->dp = NULL;
    return api_return_ax(0);
}

bool msc_api_telldir(void)
{
    api_errno err;
    struct host_dir *d = dir_slot(API_A, &err);
    if (!d)
        return api_return_errno(err);
    return api_return_axsreg((uint32_t)d->pos);
}

bool msc_api_rewinddir(void)
{
    api_errno err;
    struct host_dir *d = dir_slot(API_A, &err);
    if (!d)
        return api_return_errno(err);
    dir_rewind(d->dp);
    d->pos = 0;
    return api_return_ax(0);
}

/* Seek by entry index, mirroring the firmware: rewind if going back, then read
 * forward to the target. Fails (EINVAL) if the target is past the end. */
bool msc_api_seekdir(void)
{
    int des = API_A;
    int32_t offs;
    if (!api_pop_int32_end(&offs))
        return api_return_errno(API_EINVAL);
    api_errno err;
    struct host_dir *d = dir_slot(des, &err);
    if (!d)
        return api_return_errno(err);
    if (offs < 0)
        return api_return_errno(API_EINVAL);
    if (d->pos > offs)
    {
        dir_rewind(d->dp);
        d->pos = 0;
    }
    while (d->pos < offs)
    {
        FILINFO fno;
        if (host_next_entry(des, &fno, &err) < 0)
            return api_return_errno(err);
        if (!fno.fname[0]) /* hit EOF before reaching offs */
            return api_return_errno(API_EINVAL);
    }
    return api_return_ax(0);
}

bool msc_api_unlink(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    char host[MSC_MAX_PATH];
    if (!msc_to_host(path, host, sizeof(host)))
        return host_err();
    if (!fs_remove(host))
        return host_err();
    return api_return_ax(0);
}

bool msc_api_rename(void)
{
    /* xstack holds newname\0oldname (firmware order); rename(old, new). */
    char *newname = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    char *oldname = newname;
    while (*oldname)
        oldname++;
    if (oldname == (char *)&xstack[XSTACK_SIZE])
        return api_return_errno(API_EINVAL);
    oldname++;
    char ho[MSC_MAX_PATH], hn[MSC_MAX_PATH];
    if (!msc_to_host(oldname, ho, sizeof(ho)) || !msc_to_host(newname, hn, sizeof(hn)))
        return host_err();
    if (!fs_rename(ho, hn))
        return host_err();
    return api_return_ax(0);
}

/* Best-effort: only the read-only bit maps to the host (write permission).
 * Hidden/system/archive have no host equivalent and are silently dropped. */
bool msc_api_chmod(void)
{
    uint8_t mask = API_A;
    uint8_t attr;
    if (!api_pop_uint8(&attr))
        return api_return_errno(API_EINVAL);
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (!(mask & FS_AM_RDO))
        return api_return_ax(0);
    char host[MSC_MAX_PATH];
    if (!msc_to_host(path, host, sizeof(host)))
        return host_err();
    if (!fs_set_readonly(host, (attr & FS_AM_RDO) != 0))
        return host_err();
    return api_return_ax(0);
}

/* Best-effort: set the modification time from the FAT date/time. The creation
 * time the API also carries (crtime in AX, crdate) is not settable on POSIX. */
bool msc_api_utime(void)
{
    uint16_t crdate, ftime, fdate;
    if (!api_pop_uint16(&crdate) || !api_pop_uint16(&ftime) || !api_pop_uint16(&fdate))
        return api_return_errno(API_EINVAL);
    (void)crdate;
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    char host[MSC_MAX_PATH];
    if (!msc_to_host(path, host, sizeof(host)))
        return host_err();
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = ((fdate >> 9) & 0x7F) + 1980 - 1900;
    tm.tm_mon = ((fdate >> 5) & 0x0F) - 1;
    tm.tm_mday = fdate & 0x1F;
    tm.tm_hour = (ftime >> 11) & 0x1F;
    tm.tm_min = (ftime >> 5) & 0x3F;
    tm.tm_sec = (ftime & 0x1F) * 2;
    tm.tm_isdst = -1;
    if (!fs_set_mtime(host, mktime(&tm)))
        return host_err();
    return api_return_ax(0);
}

bool msc_api_mkdir(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    char host[MSC_MAX_PATH];
    if (!msc_to_host(path, host, sizeof(host)))
        return host_err();
    if (!fs_mkdir(host))
        return host_err();
    return api_return_ax(0);
}

bool msc_api_chdir(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    char host[MSC_MAX_PATH];
    if (!msc_to_host(path, host, sizeof(host)))
        return host_err();
    if (!fs_chdir(host)) /* chdir validates existence/dir-ness and sets errno */
        return host_err();
    return api_return_ax(0);
}

/* The 6502 sees MSC0: (and the bare current drive); reject anything else as a
 * missing device. */
bool msc_api_chdrive(void)
{
    const char *drive = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (drive[0] == ':') /* the null drive (installs) is not a cwd-able drive */
        return api_return_errno(API_ENODEV);
    char name[16];
    size_t i = 0;
    for (; drive[i] && drive[i] != ':' && i < sizeof(name) - 1; i++)
        name[i] = (char)drive[i];
    name[i] = 0;
    if (name[0] == 0 || strcasecmp(name, "MSC0") == 0)
        return api_return_ax(0);
    return api_return_errno(API_ENODEV);
}

bool msc_api_getcwd(void)
{
    /* Write "MSC0:<cwd>" at the bottom of the xstack, then relocate it to the top
     * so it pops in order — matching firmware fat_api_getcwd. */
    char cwd[MSC_MAX_PATH];
    if (!fs_getcwd(cwd, sizeof(cwd)))
    {
        xstack_ptr = XSTACK_SIZE;
        return host_err();
    }
    size_t len = msc_from_host(cwd, (char *)xstack, XSTACK_SIZE);
    if (len == 0) /* did not fit: full-path-or-error */
    {
        xstack_ptr = XSTACK_SIZE;
        return api_return_errno(API_ENOMEM);
    }
    xstack_ptr = XSTACK_SIZE;
    for (size_t i = len; i;)
        xstack[--xstack_ptr] = xstack[--i];
    return api_return_ax((uint16_t)(len + 1));
}

/* The host filesystem has no FAT volume label; report an empty one and accept
 * (ignore) a set, so label-aware programs run rather than erroring. */
bool msc_api_getlabel(void)
{
    xstack_ptr = XSTACK_SIZE; /* consume the path; no label to push */
    return api_return_ax(1);  /* empty string -> length 0 + terminator */
}

bool msc_api_setlabel(void)
{
    xstack_ptr = XSTACK_SIZE;
    return api_return_ax(0);
}

bool msc_api_getfree(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    char host[MSC_MAX_PATH];
    if (!msc_to_host(path, host, sizeof(host)))
        return host_err();
    uint64_t tot_bytes, fre_bytes;
    if (!fs_freespace(host, &tot_bytes, &fre_bytes))
        return host_err();
    uint64_t tot64 = tot_bytes / 512;
    uint64_t fre64 = fre_bytes / 512;
    uint32_t tot = tot64 > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot64;
    uint32_t fre = fre64 > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre64;
    if (!api_push_uint32(&tot) || !api_push_uint32(&fre))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}
