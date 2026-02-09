/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "tusb.h"
#include "str/str.h"
#include "usb/msc.h"
#include "api/api.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "pico/aon_timer.h"
#include <stdio.h>
#include <math.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_MSC)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// File descriptor pool for open files
#define MSC_STD_FIL_MAX 8
static FIL msc_std_fil_pool[MSC_STD_FIL_MAX];

// Validate essential settings from ffconf.h
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

// Place volume strings in flash
static const char __in_flash("fatfs_vol") VolumeStrMSC0[] = "MSC0";
static const char __in_flash("fatfs_vol") VolumeStrMSC1[] = "MSC1";
static const char __in_flash("fatfs_vol") VolumeStrMSC2[] = "MSC2";
static const char __in_flash("fatfs_vol") VolumeStrMSC3[] = "MSC3";
static const char __in_flash("fatfs_vol") VolumeStrMSC4[] = "MSC4";
static const char __in_flash("fatfs_vol") VolumeStrMSC5[] = "MSC5";
static const char __in_flash("fatfs_vol") VolumeStrMSC6[] = "MSC6";
static const char __in_flash("fatfs_vol") VolumeStrMSC7[] = "MSC7";
const char __in_flash("fatfs_vols") * VolumeStr[FF_VOLUMES] = {
    VolumeStrMSC0, VolumeStrMSC1, VolumeStrMSC2, VolumeStrMSC3,
    VolumeStrMSC4, VolumeStrMSC5, VolumeStrMSC6, VolumeStrMSC7};

// String initializer
#define MSC_VOL0 "MSC0:"
static_assert(FF_VOLUMES <= 10); // one char 0-9 in "MSC0:"

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
static uint8_t msc_tuh_dev_csw_status[CFG_TUH_DEVICE_MAX];

static FIL *msc_validate_fil(int desc)
{
    if (desc < 0 || desc >= MSC_STD_FIL_MAX)
        return NULL;
    if (!msc_std_fil_pool[desc].obj.fs)
        return NULL;
    return &msc_std_fil_pool[desc];
}

// Some vendors pad their strings with spaces, others with zeros.
// This will ensure zeros, which prints better.
static void rtrims(uint8_t *s, size_t l)
{
    while (l--)
    {
        if (s[l] == ' ')
            s[l] = '\0';
        else
            break;
    }
}

int msc_status_count(void)
{
    int count = 0;
    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
        if (msc_volume_status[vol] == msc_volume_mounted)
            count++;
    return count;
}

int msc_status_response(char *buf, size_t buf_size, int state)
{
    if (state < 0 || state >= FF_VOLUMES)
        return -1;
    if (msc_volume_status[state] == msc_volume_mounted)
    {
        const char *xb = "MB";
        double size = msc_volume_size[state] / (1024 * 1024);
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
        rtrims(msc_inquiry_resp[state].vendor_id, 8);
        rtrims(msc_inquiry_resp[state].product_id, 16);
        rtrims(msc_inquiry_resp[state].product_rev, 4);
        snprintf(buf, buf_size, STR_STATUS_MSC,
                 VolumeStr[state],
                 size, xb,
                 msc_inquiry_resp[state].vendor_id,
                 msc_inquiry_resp[state].product_id,
                 msc_inquiry_resp[state].product_rev);
    }
    return state + 1;
}

static bool inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data)
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
    msc_volume_size[vol] = (uint64_t)block_count * (uint64_t)block_size;

    TCHAR volstr[6] = MSC_VOL0;
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

    return true;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
    uint8_t const lun = 0;
    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_volume_status[vol] == msc_volume_free)
        {
            msc_volume_status[vol] = msc_volume_inquiring;
            msc_volume_dev_addr[vol] = dev_addr;
            if (!tuh_msc_inquiry(dev_addr, lun, &msc_inquiry_resp[vol], inquiry_complete_cb, 0))
            {
                DBG("MSC inquiry failed for dev_addr %d\n", dev_addr);
                msc_volume_status[vol] = msc_volume_inquiry_failed;
            }
            break;
        }
    }
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    // Unblock any pending wait_for_disk_io. TinyUSB's msch_close()
    // never fires the user complete_cb on disconnect, so this is
    // the only chance to clear the busy flag.
    msc_tuh_dev_busy[dev_addr - 1] = false;

    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_volume_dev_addr[vol] == dev_addr &&
            msc_volume_status[vol] != msc_volume_free)
        {
            if (msc_volume_status[vol] == msc_volume_mounted)
            {
                TCHAR volstr[6] = MSC_VOL0;
                volstr[3] += vol;
                f_unmount(volstr);
            }
            msc_volume_status[vol] = msc_volume_free;
            msc_volume_dev_addr[vol] = 0;
            DBG("MSC unmounted dev_addr %d from vol %d\n", dev_addr, vol);
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
    msc_tuh_dev_csw_status[dev_addr - 1] = cb_data->csw->status;
    msc_tuh_dev_busy[dev_addr - 1] = false;
    return true;
}

DWORD get_fattime(void)
{
    struct timespec ts;
    struct tm tm;
    if (aon_timer_get_time(&ts))
    {
        time_t t = (time_t)ts.tv_sec;
        localtime_r(&t, &tm);
        if (tm.tm_year + 1900 >= 1980 && tm.tm_year + 1900 <= 2107)
            return ((DWORD)(tm.tm_year + 1900 - 1980) << 25) |
                   ((DWORD)(tm.tm_mon + 1) << 21) |
                   ((DWORD)tm.tm_mday << 16) |
                   ((WORD)tm.tm_hour << 11) |
                   ((WORD)tm.tm_min << 5) |
                   ((WORD)(tm.tm_sec >> 1));
    }
    return ((DWORD)0 << 25 | (DWORD)1 << 21 | (DWORD)1 << 16);
}

DSTATUS disk_status(BYTE pdrv)
{
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
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
    msc_tuh_dev_csw_status[dev_addr - 1] = MSC_CSW_STATUS_FAILED;
    msc_tuh_dev_busy[dev_addr - 1] = true;
    if (!tuh_msc_read10(dev_addr, lun, buff, sector, (uint16_t)count, disk_io_complete, 0))
    {
        msc_tuh_dev_busy[dev_addr - 1] = false;
        return RES_ERROR;
    }
    wait_for_disk_io(dev_addr);
    return (msc_tuh_dev_csw_status[dev_addr - 1] == MSC_CSW_STATUS_PASSED) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
    uint8_t const lun = 0;
    msc_tuh_dev_csw_status[dev_addr - 1] = MSC_CSW_STATUS_FAILED;
    msc_tuh_dev_busy[dev_addr - 1] = true;
    if (!tuh_msc_write10(dev_addr, lun, buff, sector, (uint16_t)count, disk_io_complete, 0))
    {
        msc_tuh_dev_busy[dev_addr - 1] = false;
        return RES_ERROR;
    }
    wait_for_disk_io(dev_addr);
    return (msc_tuh_dev_csw_status[dev_addr - 1] == MSC_CSW_STATUS_PASSED) ? RES_OK : RES_ERROR;
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
        *((DWORD *)buff) = tuh_msc_get_block_count(dev_addr, lun);
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

bool msc_std_handles(const char *path)
{
    (void)path;
    // MSC/FatFS is the catch-all handler
    return true;
}

int msc_std_open(const char *path, uint8_t flags, api_errno *err)
{
    const unsigned char RDWR = 0x03;
    const unsigned char CREAT = 0x10;
    const unsigned char TRUNC = 0x20;
    const unsigned char APPEND = 0x40;
    const unsigned char EXCL = 0x80;

    uint8_t mode = flags & RDWR;
    if (flags & CREAT)
    {
        if (flags & EXCL)
            mode |= FA_CREATE_NEW;
        else if (flags & TRUNC)
            mode |= FA_CREATE_ALWAYS;
        else if (flags & APPEND)
            mode |= FA_OPEN_APPEND;
        else
            mode |= FA_OPEN_ALWAYS;
    }

    FIL *fp = NULL;
    for (int i = 0; i < MSC_STD_FIL_MAX; i++)
    {
        if (!msc_std_fil_pool[i].obj.fs)
        {
            fp = &msc_std_fil_pool[i];
            break;
        }
    }
    if (!fp)
    {
        *err = API_EMFILE;
        return -1;
    }

    FRESULT fresult = f_open(fp, path, mode);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fresult(fresult);
        return -1;
    }

    // Return the index of the FIL in the pool
    return (int)(fp - msc_std_fil_pool);
}

int msc_std_close(int desc, api_errno *err)
{
    FIL *fp = msc_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return -1;
    }
    FRESULT fresult = f_close(fp);
    fp->obj.fs = NULL;
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fresult(fresult);
        return -1;
    }
    return 0;
}

std_rw_result msc_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    FIL *fp = msc_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    UINT br;
    FRESULT fresult = f_read(fp, buf, count, &br);
    *bytes_read = br;
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fresult(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

std_rw_result msc_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    FIL *fp = msc_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    *bytes_written = bw;
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fresult(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

int msc_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err)
{
    FIL *fp = msc_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return -1;
    }
    FSIZE_t absolute_offset;
    if (whence == SEEK_SET)
    {
        if (offset < 0)
        {
            *err = API_EINVAL;
            return -1;
        }
        absolute_offset = offset;
    }
    else if (whence == SEEK_CUR)
    {
        FSIZE_t current_pos = f_tell(fp);
        if (offset < 0 && (FSIZE_t)(-offset) > current_pos)
        {
            *err = API_EINVAL;
            return -1;
        }
        absolute_offset = current_pos + offset;
    }
    else if (whence == SEEK_END)
    {
        FSIZE_t file_size = f_size(fp);
        if (offset < 0 && (FSIZE_t)(-offset) > file_size)
        {
            *err = API_EINVAL;
            return -1;
        }
        absolute_offset = file_size + offset;
    }
    else
    {
        *err = API_EINVAL;
        return -1;
    }
    FRESULT fresult = f_lseek(fp, absolute_offset);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fresult(fresult);
        return -1;
    }
    FSIZE_t fpos = f_tell(fp);
    if (fpos > 0x7FFFFFFF)
        fpos = 0x7FFFFFFF;
    *pos = fpos;
    return 0;
}

int msc_std_sync(int desc, api_errno *err)
{
    FIL *fp = msc_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return -1;
    }
    FRESULT fresult = f_sync(fp);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fresult(fresult);
        return -1;
    }
    return 0;
}
