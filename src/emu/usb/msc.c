/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's block device: a RAM disk backing a FatFs volume. It stands in
 * for the firmware's USB mass-storage block device (ria/usb/msc.c's diskio), so
 * the SHARED FatFs driver (ria/api/fat.c) runs unchanged in the emulator over an
 * ephemeral in-RAM disk. Used by --tmpdrive and the FatFs tests. Only the diskio
 * contract lives here; formatting/mounting is the caller's (f_mkfs/f_mount).
 */

#include "emu/usb/msc.h"
#include "api/api.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include <string.h>
#include <time.h>

#define RAM_SECTOR_SIZE 512u
#define RAM_SECTOR_COUNT 4096u /* 2 MiB — enough for a scratch FAT volume */

static uint8_t g_ram[RAM_SECTOR_COUNT * RAM_SECTOR_SIZE];
static bool g_ram_init;

/* FatFs volume ID strings (FF_STR_VOLUME_ID==2, app-provided; matches firmware). */
const char *VolumeStr[FF_VOLUMES] = {"MSC0", "MSC1", "MSC2", "MSC3", "MSC4",
                                     "MSC5", "MSC6", "MSC7", "MSC8", "MSC9"};

void emu_ramdisk_reset(void)
{
    memset(g_ram, 0, sizeof(g_ram));
    g_ram_init = false;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    (void)pdrv;
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
    if (sector + count > RAM_SECTOR_COUNT)
        return RES_PARERR;
    memcpy(buff, g_ram + (size_t)sector * RAM_SECTOR_SIZE, (size_t)count * RAM_SECTOR_SIZE);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (sector + count > RAM_SECTOR_COUNT)
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
