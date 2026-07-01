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
#include "emu/host/dir.h" /* emu_dir_ops_set (swap the dir slots) */
#include "api/dir.h"      /* dir_run / dir_stop (the firmware FatFs DIR pool) */
#include "api/fat.h"     /* fat_std_* file driver; pulls api/api.h + api/std.h */
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
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
static bool g_active;

bool emu_fat_active(void) { return g_active; }

/* --tmpdrive: format a fresh RAM FatFs and make it the active MSC0: backend. The
 * 6502 dir syscalls run the REAL firmware dir_api_* (ria/api/dir.c) over this RAM
 * FatFs; the file syscalls run the shared fat_std_* driver. */
bool emu_ramdrive_mount(void)
{
    emu_ramdisk_reset();
    BYTE work[FF_MAX_SS]; /* f_mkfs scratch: one sector, on the stack — mount is rare */
    if (f_mkfs("MSC0:", 0, work, sizeof(work)) != FR_OK)
        return false;
    if (f_mount(&g_ramfs, "MSC0:", 1) != FR_OK)
        return false;
    dir_run();             /* fresh FatFs directory pool (ria/api/dir.c) */
    emu_dir_ops_set(true); /* the 6502 dir syscalls -> firmware dir_api_* */
    g_active = true;       /* std.c's fat driver now claims MSC0: (file syscalls -> FatFs) */
    return true;
}

void emu_ramdrive_unmount(void)
{
    dir_stop(); /* close open FatFs directories (ria/api/dir.c) */
    f_unmount("MSC0:");
    emu_dir_ops_set(false); /* back to the native host handlers */
    g_active = false;       /* std.c's fat driver declines; host reclaims MSC0: */
}
