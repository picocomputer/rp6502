/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/api/hostfs.h"
#include "emu/host/msc.h"
#include "emu/plat.h"
#include "emu/sys/mem.h"
#include "api/api.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

void hostfs_stop(void)
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

bool hostfs_api_stat(void)
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

bool hostfs_api_opendir(void)
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

bool hostfs_api_readdir(void)
{
    FILINFO fno;
    api_errno err;
    if (host_next_entry(API_A, &fno, &err) < 0)
        return api_return_errno(err);
    if (!dir_push_filinfo(&fno))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool hostfs_api_closedir(void)
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

bool hostfs_api_telldir(void)
{
    api_errno err;
    struct host_dir *d = dir_slot(API_A, &err);
    if (!d)
        return api_return_errno(err);
    return api_return_axsreg((uint32_t)d->pos);
}

bool hostfs_api_rewinddir(void)
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
bool hostfs_api_seekdir(void)
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

bool hostfs_api_unlink(void)
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

bool hostfs_api_rename(void)
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
bool hostfs_api_chmod(void)
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
bool hostfs_api_utime(void)
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

bool hostfs_api_mkdir(void)
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

bool hostfs_api_chdir(void)
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
bool hostfs_api_chdrive(void)
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
    if (name[0] == 0 || os_strcasecmp(name, "MSC0") == 0)
        return api_return_ax(0);
    return api_return_errno(API_ENODEV);
}

bool hostfs_api_getcwd(void)
{
    /* Write "MSC0:<cwd>" at the bottom of the xstack, then relocate it to the top
     * so it pops in order — matching firmware dir_api_getcwd. */
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
bool hostfs_api_getlabel(void)
{
    xstack_ptr = XSTACK_SIZE; /* consume the path; no label to push */
    return api_return_ax(1);  /* empty string -> length 0 + terminator */
}

bool hostfs_api_setlabel(void)
{
    xstack_ptr = XSTACK_SIZE;
    return api_return_ax(0);
}

bool hostfs_api_getfree(void)
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
