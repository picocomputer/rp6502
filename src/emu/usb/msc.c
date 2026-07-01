/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's FatFs backend for MSC0:. It stands in for the firmware's USB
 * mass-storage drive: a RAM block device (the diskio FatFs calls) plus the glue
 * that lets --tmpdrive present a real, ephemeral FatFs. When the backend is
 * active the 6502's file/dir syscalls run the SHARED ria/api/fat.c driver (files)
 * and FatFs f_* (directories) over the RAM disk — the same code as on hardware,
 * so the tmpdrive exercises real FatFs. The RAM is malloc'd only when a tmpdrive
 * is actually used; the default (host-backed) run allocates nothing.
 */

#include "emu/usb/msc.h"
#include "api/fat.h" /* fat_std_* (shared driver); pulls api/api.h + api/std.h */
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RAM_SECTOR_SIZE 512u
#define RAM_SECTOR_COUNT 1024u /* 512 KiB — enough for a scratch FAT volume */
#define RAM_BYTES ((size_t)RAM_SECTOR_COUNT * RAM_SECTOR_SIZE)

static uint8_t *g_ram; /* the RAM disk, malloc'd on first use (else NULL) */
static bool g_ram_init;

/* FatFs volume ID strings (FF_STR_VOLUME_ID==2, app-provided; matches firmware). */
const char *VolumeStr[FF_VOLUMES] = {"MSC0", "MSC1", "MSC2", "MSC3", "MSC4",
                                     "MSC5", "MSC6", "MSC7", "MSC8", "MSC9"};

static bool ram_alloc(void)
{
    if (!g_ram)
        g_ram = malloc(RAM_BYTES);
    return g_ram != NULL;
}

void emu_ramdisk_reset(void)
{
    if (ram_alloc())
        memset(g_ram, 0, RAM_BYTES);
    g_ram_init = false;
}

/* ---- FatFs diskio (the RAM block device) --------------------------------- */

DSTATUS disk_initialize(BYTE pdrv)
{
    (void)pdrv;
    if (!ram_alloc())
        return STA_NOINIT;
    g_ram_init = true;
    return 0;
}

DSTATUS disk_status(BYTE pdrv)
{
    (void)pdrv;
    return g_ram_init ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!g_ram || sector + count > RAM_SECTOR_COUNT)
        return RES_PARERR;
    memcpy(buff, g_ram + (size_t)sector * RAM_SECTOR_SIZE, (size_t)count * RAM_SECTOR_SIZE);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!g_ram || sector + count > RAM_SECTOR_COUNT)
        return RES_PARERR;
    memcpy(g_ram + (size_t)sector * RAM_SECTOR_SIZE, buff, (size_t)count * RAM_SECTOR_SIZE);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd)
    {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = RAM_SECTOR_COUNT;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = RAM_SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1; /* erase-block size unknown -> 1 */
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/* FatFs timestamp callback (the firmware sources this from its RTC). */
DWORD get_fattime(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year + 1900 < 1980)
        return ((DWORD)1 << 16) | ((DWORD)1 << 21); /* 1980-01-01 */
    return ((DWORD)(tm.tm_year - 80) << 25) | ((DWORD)(tm.tm_mon + 1) << 21) |
           ((DWORD)tm.tm_mday << 16) | ((DWORD)tm.tm_hour << 11) |
           ((DWORD)tm.tm_min << 5) | ((DWORD)(tm.tm_sec / 2));
}

/* f_mkfs preview hook (see ff.c): the firmware inspects a format without writing;
 * the emulator always performs the real format, so proceed (return 0). */
int dsk_mkfs_capture(BYTE fsty, DWORD au_sectors)
{
    (void)fsty, (void)au_sectors;
    return 0;
}

/* Map a FatFs FRESULT to an api_errno. The firmware supplies this from api.c; the
 * emulator builds its own api.c without it, so the shared fat.c gets it here. */
api_errno api_errno_from_fatfs(unsigned fresult)
{
    switch ((FRESULT)fresult)
    {
    case FR_DISK_ERR:
    case FR_INT_ERR:
    case FR_MKFS_ABORTED:
        return API_EIO;
    case FR_NOT_READY:
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:
        return API_ENODEV;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return API_ENOENT;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
        return API_EINVAL;
    case FR_DENIED:
    case FR_WRITE_PROTECTED:
        return API_EACCES;
    case FR_EXIST:
        return API_EEXIST;
    case FR_INVALID_OBJECT:
        return API_EBADF;
    case FR_TIMEOUT:
        return API_EAGAIN;
    case FR_LOCKED:
        return API_EBUSY;
    case FR_NOT_ENOUGH_CORE:
        return API_ENOMEM;
    case FR_TOO_MANY_OPEN_FILES:
        return API_EMFILE;
    default:
        return API_EIO;
    }
}

/* ---- Drive lifecycle ----------------------------------------------------- */

static FATFS g_ramfs;
static BYTE g_mkfs_work[4096]; /* f_mkfs work area */
static bool g_active;

bool emu_fat_active(void) { return g_active; }

/* --tmpdrive: format a fresh RAM FatFs and make it the active MSC0: backend. */
bool emu_ramdrive_mount(void)
{
    emu_ramdisk_reset();
    if (f_mkfs("MSC0:", 0, g_mkfs_work, sizeof(g_mkfs_work)) != FR_OK)
        return false;
    if (f_mount(&g_ramfs, "MSC0:", 1) != FR_OK)
        return false;
    g_active = true;
    return true;
}

void emu_ramdrive_unmount(void)
{
    f_unmount("MSC0:");
    g_active = false;
}

/* Map the shared driver's api_errno onto a POSIX errno, so the emu std/dir layer
 * (which runs api_errno_from_host over errno) reports the same 6502 errno. */
static void set_errno_from_api(api_errno e)
{
    switch (e)
    {
    case API_EIO: errno = EIO; break;
    case API_ENODEV: errno = ENODEV; break;
    case API_ENOENT: errno = ENOENT; break;
    case API_EINVAL: errno = EINVAL; break;
    case API_EACCES: errno = EACCES; break;
    case API_EEXIST: errno = EEXIST; break;
    case API_EBADF: errno = EBADF; break;
    case API_EAGAIN: errno = EAGAIN; break;
    case API_EBUSY: errno = EBUSY; break;
    case API_ENOMEM: errno = ENOMEM; break;
    case API_EMFILE: errno = EMFILE; break;
    case API_ERANGE: errno = ERANGE; break;
    default: errno = EIO; break;
    }
}

/* ---- File driver: the shared fat_std_* in the emu std.c driver-table shape --
 * The emu passes an opaque void* descriptor; the shared driver uses a small int
 * FIL index, so box it as (index + 1) to keep a valid descriptor non-NULL. */

bool fat_std_handles_(const char *path) { return fat_std_handles(path); }

void *fat_std_open_(const char *path, uint8_t flags)
{
    api_errno err = 0;
    int d = fat_std_open(path, flags, &err);
    if (d < 0)
    {
        set_errno_from_api(err);
        return NULL;
    }
    return (void *)(intptr_t)(d + 1);
}

void fat_std_close_(void *desc)
{
    api_errno err = 0;
    fat_std_close((int)(intptr_t)desc - 1, &err);
}

io_result fat_std_read_(void *desc, void *buf, size_t n, size_t *got)
{
    api_errno err = 0;
    uint32_t br = 0;
    std_rw_result r = fat_std_read((int)(intptr_t)desc - 1, buf, (uint32_t)n, &br, &err);
    *got = br;
    if (r != STD_OK)
    {
        set_errno_from_api(err);
        return IO_ERROR;
    }
    return IO_OK;
}

io_result fat_std_write_(void *desc, const void *buf, size_t n, size_t *put)
{
    api_errno err = 0;
    uint32_t bw = 0;
    std_rw_result r = fat_std_write((int)(intptr_t)desc - 1, buf, (uint32_t)n, &bw, &err);
    *put = bw;
    if (r != STD_OK)
    {
        set_errno_from_api(err);
        return IO_ERROR;
    }
    return IO_OK;
}

void fat_std_sync_(void *desc)
{
    api_errno err = 0;
    fat_std_sync((int)(intptr_t)desc - 1, &err);
}

long fat_std_lseek_(void *desc, long off, int whence)
{
    api_errno err = 0;
    int32_t pos = 0;
    if (fat_std_lseek((int)(intptr_t)desc - 1, (int8_t)whence, (int32_t)off, &pos, &err) != 0)
    {
        set_errno_from_api(err);
        return -1;
    }
    return pos;
}

/* ---- Directory / metadata ops over FatFs (the host/dir.c fs_* shape) ------ */

#define FAT_MAX_DIR 8
static struct
{
    bool used;
    DIR dp;
    long pos; /* entries read so far, for telldir/seekdir */
} fat_dirs[FAT_MAX_DIR];

void fat_dir_reset(void) /* close open directories (machine reset) */
{
    for (int i = 0; i < FAT_MAX_DIR; i++)
    {
        if (fat_dirs[i].used)
            f_closedir(&fat_dirs[i].dp);
        fat_dirs[i].used = false;
    }
}

/* FatFs FILINFO already carries FAT-packed dates + attribute bits, so this is a
 * straight copy (unlike the host, which synthesizes them from stat). */
static void info_from_filinfo(fs_info_t *info, const FILINFO *fno)
{
    info->size = (uint32_t)fno->fsize;
    info->mdate = fno->fdate;
    info->mtime = fno->ftime;
    info->cdate = 0; /* FILINFO has no creation time */
    info->ctime = 0;
    info->attrib = fno->fattrib;
    snprintf(info->altname, sizeof(info->altname), "%s", fno->altname);
    snprintf(info->name, sizeof(info->name), "%s", fno->fname);
}

static int fat_fail(FRESULT fr)
{
    set_errno_from_api(api_errno_from_fatfs(fr));
    return -1;
}

int fat_stat(const char *path, fs_info_t *info)
{
    FILINFO fno;
    FRESULT fr = f_stat(path, &fno);
    if (fr != FR_OK)
        return fat_fail(fr);
    info_from_filinfo(info, &fno);
    return 0;
}

int fat_opendir(const char *path)
{
    int des = 0;
    for (; des < FAT_MAX_DIR; des++)
        if (!fat_dirs[des].used)
            break;
    if (des == FAT_MAX_DIR)
    {
        errno = EMFILE;
        return -1;
    }
    FRESULT fr = f_opendir(&fat_dirs[des].dp, path);
    if (fr != FR_OK)
        return fat_fail(fr);
    fat_dirs[des].used = true;
    fat_dirs[des].pos = 0;
    return des;
}

static bool fat_dir_bad(int des) /* sets errno like the host dir_slot */
{
    if (des < 0 || des >= FAT_MAX_DIR)
    {
        errno = EINVAL;
        return true;
    }
    if (!fat_dirs[des].used)
    {
        errno = EBADF;
        return true;
    }
    return false;
}

int fat_readdir(int des, fs_info_t *info)
{
    if (fat_dir_bad(des))
        return -1;
    FILINFO fno;
    FRESULT fr = f_readdir(&fat_dirs[des].dp, &fno);
    if (fr != FR_OK)
        return fat_fail(fr);
    if (!fno.fname[0]) /* end of directory */
    {
        memset(info, 0, sizeof(*info));
        return 0;
    }
    fat_dirs[des].pos++;
    info_from_filinfo(info, &fno);
    return 0;
}

int fat_closedir(int des)
{
    if (fat_dir_bad(des))
        return -1;
    FRESULT fr = f_closedir(&fat_dirs[des].dp);
    fat_dirs[des].used = false;
    if (fr != FR_OK)
        return fat_fail(fr);
    return 0;
}

long fat_telldir(int des)
{
    if (fat_dir_bad(des))
        return -1;
    return fat_dirs[des].pos;
}

int fat_rewinddir(int des)
{
    if (fat_dir_bad(des))
        return -1;
    FRESULT fr = f_readdir(&fat_dirs[des].dp, NULL); /* NULL rewinds */
    if (fr != FR_OK)
        return fat_fail(fr);
    fat_dirs[des].pos = 0;
    return 0;
}

int fat_seekdir(int des, long off)
{
    if (fat_dir_bad(des))
        return -1;
    if (off < 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (fat_dirs[des].pos > off && fat_rewinddir(des) != 0)
        return -1;
    while (fat_dirs[des].pos < off)
    {
        fs_info_t info;
        if (fat_readdir(des, &info) != 0)
            return -1;
        if (!info.name[0]) /* hit EOF before reaching off */
        {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

int fat_getfree(const char *path, uint32_t *free_sectors, uint32_t *total_sectors)
{
    DWORD nclust = 0;
    FATFS *fs = NULL;
    FRESULT fr = f_getfree(path, &nclust, &fs);
    if (fr != FR_OK)
        return fat_fail(fr);
    uint64_t tot = (uint64_t)(fs->n_fatent - 2) * fs->csize; /* 512-byte sectors */
    uint64_t fre = (uint64_t)nclust * fs->csize;
    *total_sectors = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    *free_sectors = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    return 0;
}

int fat_chmod(const char *path, uint8_t attr, uint8_t mask)
{
    FRESULT fr = f_chmod(path, attr, mask); /* FAT AM_* bits pass straight through */
    if (fr != FR_OK)
        return fat_fail(fr);
    return 0;
}

int fat_utime(const char *path, uint16_t fdate, uint16_t ftime)
{
    FILINFO fno;
    memset(&fno, 0, sizeof(fno));
    fno.fdate = fdate;
    fno.ftime = ftime;
    FRESULT fr = f_utime(path, &fno);
    if (fr != FR_OK)
        return fat_fail(fr);
    return 0;
}

int fat_getlabel(const char *path, char *label, size_t sz)
{
    (void)sz;
    DWORD vsn;
    FRESULT fr = f_getlabel(path, label, &vsn);
    if (fr != FR_OK)
        return fat_fail(fr);
    return 0;
}

int fat_setlabel(const char *path)
{
    FRESULT fr = f_setlabel(path);
    if (fr != FR_OK)
        return fat_fail(fr);
    return 0;
}

int fat_unlink(const char *path)
{
    FRESULT fr = f_unlink(path);
    if (fr != FR_OK)
        return fat_fail(fr);
    return 0;
}

int fat_rename(const char *oldp, const char *newp)
{
    FRESULT fr = f_rename(oldp, newp);
    if (fr != FR_OK)
        return fat_fail(fr);
    return 0;
}

int fat_mkdir(const char *path)
{
    FRESULT fr = f_mkdir(path);
    if (fr != FR_OK)
        return fat_fail(fr);
    return 0;
}

int fat_chdir(const char *path)
{
    FRESULT fr = f_chdir(path);
    if (fr != FR_OK)
        return fat_fail(fr);
    return 0;
}

int fat_chdrive(const char *drive)
{
    if (drive[0] == ':') /* the null drive (installs) is not a cwd-able drive */
    {
        errno = ENODEV;
        return -1;
    }
    FRESULT fr = f_chdrive(drive);
    if (fr != FR_OK)
        return fat_fail(fr);
    return 0;
}

size_t fat_getcwd(char *out, size_t outsz)
{
    FRESULT fr = f_getcwd(out, (UINT)outsz);
    if (fr != FR_OK)
    {
        set_errno_from_api(api_errno_from_fatfs(fr));
        return 0;
    }
    return strlen(out);
}
