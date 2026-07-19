/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/host/tmp.h"
#include "emu/main.h"
#include "emu/host/host.h"
#include "ria/api/fat.h"
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

void tmp_disk_reset(void)
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
    os_localtime(t, &tm);
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

/* ---- Drive lifecycle ----------------------------------------------------- */

static FATFS g_ramfs;
static bool g_active;

bool tmp_active(void) { return g_active; }

/* The `handles` predicate for the shared fat_std_* file driver (std.c's table):
 * claim MSC0: only while --tmpdrive is mounted; otherwise the host catch-all
 * reclaims it. */
bool tmp_std_handles(const char *path)
{
    (void)path;
    return tmp_active();
}

/* --tmpdrive: format a fresh RAM FatFs and make it the active MSC0: backend. The
 * 6502 dir syscalls run the REAL firmware fat_api_* (ria/api/fat.c) over this RAM
 * FatFs; the file syscalls run the shared fat_std_* driver. */
bool tmp_mount(void)
{
    tmp_disk_reset();
    BYTE work[FF_MAX_SS]; /* f_mkfs scratch: one sector, on the stack — mount is rare */
    if (f_mkfs("MSC0:", 0, work, sizeof(work)) != FR_OK)
        return false;
    if (f_mount(&g_ramfs, "MSC0:", 1) != FR_OK)
        return false;
    fat_run();              /* fresh FatFs directory pool (ria/api/fat.c) */
    main_dir_ops_set(true); /* the 6502 dir syscalls -> firmware fat_api_* */
    g_active = true;        /* std.c's fat driver now claims MSC0: (file syscalls -> FatFs) */
    return true;
}

void tmp_unmount(void)
{
    fat_stop(); /* close open FatFs directories (ria/api/fat.c) */
    f_unmount("MSC0:");
    main_dir_ops_set(false); /* back to the native host handlers */
    g_active = false;        /* std.c's fat driver declines; host reclaims MSC0: */
}
