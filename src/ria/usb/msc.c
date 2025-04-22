/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "tusb.h"
#include "usb/usb.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "pico/aon_timer.h"
#include <math.h>

// Validate essential settings in ffconf.h
static_assert(sizeof(TCHAR) == sizeof(char));
static_assert(FF_CODE_PAGE == RP6502_CODE_PAGE);
static_assert(FF_FS_EXFAT == RP6502_EXFAT);
static_assert(FF_LBA64 == RP6502_EXFAT);
static_assert(FF_USE_STRFUNC == 1);
static_assert(FF_USE_LFN == 1);
static_assert(FF_MAX_LFN == 255);
static_assert(FF_LFN_UNICODE == 0);
static_assert(FF_LFN_BUF == 255);
static_assert(FF_SFN_BUF == 12);
static_assert(FF_FS_RPATH == 2);
static_assert(FF_MULTI_PARTITION == 0);
static_assert(FF_FS_LOCK == 8);
static_assert(FF_FS_NORTC == 0);
static_assert(FF_VOLUMES == 8);
static_assert(FF_STR_VOLUME_ID == 1);
#ifdef FF_VOLUME_STRS
#error FF_VOLUME_STRS must not be defined
#endif

static const char __in_flash("fatfs_vol") VolumeStrUSB0[] = "USB0";
static const char __in_flash("fatfs_vol") VolumeStrUSB1[] = "USB1";
static const char __in_flash("fatfs_vol") VolumeStrUSB2[] = "USB2";
static const char __in_flash("fatfs_vol") VolumeStrUSB3[] = "USB3";
static const char __in_flash("fatfs_vol") VolumeStrUSB4[] = "USB4";
static const char __in_flash("fatfs_vol") VolumeStrUSB5[] = "USB5";
static const char __in_flash("fatfs_vol") VolumeStrUSB6[] = "USB6";
static const char __in_flash("fatfs_vol") VolumeStrUSB7[] = "USB7";
__in_flash("fatfs_vols") const char *VolumeStr[FF_VOLUMES] = {
    VolumeStrUSB0, VolumeStrUSB1, VolumeStrUSB2, VolumeStrUSB3,
    VolumeStrUSB4, VolumeStrUSB5, VolumeStrUSB6, VolumeStrUSB7};

typedef enum
{
    msc_volume_free = 0,
    msc_volume_inquiring,
    msc_volume_mounted,
    msc_volume_inquiry_failed,
    msc_volume_mount_failed,
} msc_volume_status_t;

static msc_volume_status_t msc_volume_status[FF_VOLUMES];
static uint8_t msc_volume_dev_addr[FF_VOLUMES];
static FATFS msc_fatfs_volumes[FF_VOLUMES];
static scsi_inquiry_resp_t msc_inquiry_resp[FF_VOLUMES];
static uint64_t msc_volume_size[FF_VOLUMES];
static FRESULT msc_mount_result[FF_VOLUMES];

static bool msc_tuh_dev_busy[CFG_TUH_DEVICE_MAX];

bool inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data)
{
    uint8_t vol;
    for (vol = 0; vol < FF_VOLUMES; vol++)
        if (msc_volume_status[vol] == msc_volume_inquiring &&
            msc_volume_dev_addr[vol] == dev_addr)
            break;
    if (vol == FF_VOLUMES)
        return false;

    if (cb_data->csw->status != 0)
    {
        msc_volume_status[vol] = msc_volume_inquiry_failed;
        return false;
    }
    const uint32_t block_count = tuh_msc_get_block_count(dev_addr, cb_data->cbw->lun);
    const uint32_t block_size = tuh_msc_get_block_size(dev_addr, cb_data->cbw->lun);
    msc_volume_size[vol] = block_count * block_size;

    TCHAR volstr[6] = "USB0:";
    volstr[3] += vol;
    msc_mount_result[vol] = f_mount(&msc_fatfs_volumes[vol], volstr, 1);
    if (msc_mount_result[vol] == FR_OK)
        msc_volume_status[vol] = msc_volume_mounted;
    else
    {
        msc_volume_status[vol] = msc_volume_mount_failed;
        return false;
    }

    // If current directory invalid, change to root of this drive
    char s[2];
    if (FR_OK != f_getcwd(s, 2))
    {
        f_chdrive(volstr);
        f_chdir("/");
    }

    usb_set_status(dev_addr, "MSC %s %.8s %.16s rev %.4s",
                   VolumeStr[vol],
                   msc_inquiry_resp[vol].vendor_id,
                   msc_inquiry_resp[vol].product_id,
                   msc_inquiry_resp[vol].product_rev);

    return true;
}

#if 0
    const char *xb = "MB";
    double size = block_count * block_size / (1024 * 1024);
    if (size >= 1000)
    {
        xb = "GB";
        size /= 1024;
    }
    if (size >= 1000)
    {
        xb = "TB";
        size /= 1024;
    }
    size = ceil(size * 10) / 10;
    usb_set_status(dev_addr, "MSC %.1f %s %.8s %.16s rev %.4s",
                   size, xb,
                   msc_inquiry_resp[dev_addr].vendor_id,
                   msc_inquiry_resp[dev_addr].product_id,
                   msc_inquiry_resp[dev_addr].product_rev);
#endif

void tuh_msc_mount_cb(uint8_t dev_addr)
{
    uint8_t const lun = 0;
    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_volume_status[vol] == msc_volume_free)
        {
            msc_volume_status[vol] = msc_volume_inquiring;
            msc_volume_dev_addr[vol] = dev_addr;
            tuh_msc_inquiry(dev_addr, lun, &msc_inquiry_resp[vol], inquiry_complete_cb, 0);
            break;
        }
    }
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_volume_status[vol] == msc_volume_mounted &&
            msc_volume_dev_addr[vol] == dev_addr)
        {
            msc_volume_status[vol] = msc_volume_free;
            TCHAR volstr[6] = "USB0:";
            volstr[3] += vol;
            f_unmount(volstr);
        }
    }
}

static void wait_for_disk_io(uint8_t dev_addr)
{
    while (msc_tuh_dev_busy[dev_addr - 1])
        main_task();
}

static bool disk_io_complete(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data)
{
    (void)cb_data;
    msc_tuh_dev_busy[dev_addr - 1] = false;
    return true;
}

DWORD get_fattime(void)
{
    struct tm tm;
    if (aon_timer_get_time_calendar(&tm) &&
        tm.tm_year + 1900 >= 1980 &&
        tm.tm_year + 1900 <= 2107)
        return ((DWORD)(tm.tm_year + 1900 - 1980) << 25) |
               ((DWORD)(tm.tm_mon + 1) << 21) |
               ((DWORD)tm.tm_mday << 16) |
               ((WORD)tm.tm_hour << 11) |
               ((WORD)tm.tm_min << 5) |
               ((WORD)(tm.tm_sec >> 1));
    else
        return ((DWORD)0 << 25 | (DWORD)1 << 21 | (DWORD)1 << 16);
}

DSTATUS disk_status(BYTE pdrv)
{
    uint8_t dev_addr = pdrv;
    return tuh_msc_mounted(dev_addr) ? 0 : STA_NODISK;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    (void)(pdrv);
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
    uint8_t const lun = 0;
    msc_tuh_dev_busy[dev_addr - 1] = true;
    tuh_msc_read10(dev_addr, lun, buff, sector, (uint16_t)count, disk_io_complete, 0);
    wait_for_disk_io(dev_addr);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
    uint8_t const lun = 0;
    msc_tuh_dev_busy[dev_addr - 1] = true;
    tuh_msc_write10(dev_addr, lun, buff, sector, (uint16_t)count, disk_io_complete, 0);
    wait_for_disk_io(dev_addr);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
    uint8_t const lun = 0;
    switch (cmd)
    {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *((DWORD *)buff) = (WORD)tuh_msc_get_block_count(dev_addr, lun);
        return RES_OK;
    case GET_SECTOR_SIZE:
        *((WORD *)buff) = (WORD)tuh_msc_get_block_size(dev_addr, lun);
        return RES_OK;
    case GET_BLOCK_SIZE:
        *((DWORD *)buff) = 1; // 1 sector
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
