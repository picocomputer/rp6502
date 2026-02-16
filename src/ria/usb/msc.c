/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "tusb.h"
#include "str/str.h"
#include "usb/msc.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "pico/aon_timer.h"
#include <stdio.h>
#include <string.h>
#include "pico/time.h"

#define DEBUG_RIA_USB_MSC

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_MSC)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// File descriptor pool for open files
#define MSC_STD_FIL_MAX 8
static FIL msc_std_fil_pool[MSC_STD_FIL_MAX];

// Deadline for SCSI command completion and device ready polling
#define MSC_IO_TIMEOUT_MS 3000

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

#define MSC_VOL0 "MSC0:"
static_assert(FF_VOLUMES <= 10); // one char 0-9 in "MSC0:"

typedef enum
{
    msc_volume_free = 0,
    msc_volume_registered,
    msc_volume_mounted,
    msc_volume_ejected,
    msc_volume_failed,
} msc_volume_status_t;

static msc_volume_status_t msc_volume_status[FF_VOLUMES];
static uint8_t msc_volume_dev_addr[FF_VOLUMES];
static FATFS msc_fatfs_volumes[FF_VOLUMES];
static scsi_inquiry_resp_t msc_inquiry_resp[FF_VOLUMES];
static bool msc_volume_tur_ok[FF_VOLUMES];
static uint32_t msc_volume_block_count[FF_VOLUMES];
static uint32_t msc_volume_block_size[FF_VOLUMES];

static bool msc_tuh_dev_busy[CFG_TUH_DEVICE_MAX];
static uint8_t msc_tuh_dev_csw_status[CFG_TUH_DEVICE_MAX];

// This driver requires our custom TinyUSB: src/ria/usb/msc_host.c.
// It will not work with: src/tinyusb/src/class/msc/msc_host.c
// There's one additional interface we added.
bool tuh_msc_is_cbi(uint8_t dev_addr);
bool tuh_msc_reset_recovery(uint8_t dev_addr);
bool tuh_msc_read_format_capacities(uint8_t dev_addr, uint8_t lun, void *response,
                                    uint8_t alloc_length,
                                    tuh_msc_complete_cb_t complete_cb, uintptr_t arg);

// UFI READ FORMAT CAPACITIES response (first 12 bytes: header + 1 descriptor)
typedef struct TU_ATTR_PACKED
{
    uint8_t reserved[3];
    uint8_t capacity_list_length; // in bytes (8 per descriptor)
    uint32_t num_blocks;          // big-endian
    uint8_t descriptor_type;      // 1=unformatted, 2=formatted, 3=no media
    uint8_t block_size_hi;        // big-endian block size (3 bytes)
    uint8_t block_size_mid;
    uint8_t block_size_lo;
} ufi_format_capacity_resp_t;

static_assert(sizeof(ufi_format_capacity_resp_t) == 12, "size is not correct");

static bool disk_io_complete(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data)
{
    msc_tuh_dev_csw_status[dev_addr - 1] = cb_data->csw->status;
    msc_tuh_dev_busy[dev_addr - 1] = false;
    return true;
}

static void wait_for_disk_io(uint8_t dev_addr)
{
    absolute_time_t deadline = make_timeout_time_ms(MSC_IO_TIMEOUT_MS);
    while (msc_tuh_dev_busy[dev_addr - 1])
    {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
        {
            DBG("MSC dev %d: I/O timeout\n", dev_addr);
            tuh_msc_reset_recovery(dev_addr);
            msc_tuh_dev_busy[dev_addr - 1] = false;
            msc_tuh_dev_csw_status[dev_addr - 1] = MSC_CSW_STATUS_FAILED;
            return;
        }
        main_task();
    }
}

static bool wait_for_ready(uint8_t dev_addr)
{
    absolute_time_t deadline = make_timeout_time_ms(MSC_IO_TIMEOUT_MS);
    while (!tuh_msc_ready(dev_addr))
    {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
        {
            DBG("MSC dev %d: ready timeout\n", dev_addr);
            return false;
        }
        main_task();
    }
    return true;
}

// Probe an ejected removable volume for media via SCSI commands.
static bool msc_probe_media(uint8_t vol)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    uint8_t const lun = 0;

    if (!tuh_msc_mounted(dev_addr))
        return false;

    // Request Sense + Test Unit Ready, retrying to clear stacked
    // Unit Attention conditions (e.g. media change, power-on reset).
    scsi_sense_fixed_resp_t sense_resp;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        // Request Sense to clear one pending Unit Attention
        if (!wait_for_ready(dev_addr))
            return false;
        msc_tuh_dev_busy[dev_addr - 1] = true;
        if (!tuh_msc_request_sense(dev_addr, lun, &sense_resp, disk_io_complete, 0))
        {
            msc_tuh_dev_busy[dev_addr - 1] = false;
            return false;
        }
        wait_for_disk_io(dev_addr);

        // Test Unit Ready
        if (!wait_for_ready(dev_addr))
            return false;
        msc_tuh_dev_busy[dev_addr - 1] = true;
        if (!tuh_msc_test_unit_ready(dev_addr, lun, disk_io_complete, 0))
        {
            msc_tuh_dev_busy[dev_addr - 1] = false;
            return false;
        }
        wait_for_disk_io(dev_addr);
        if (msc_tuh_dev_csw_status[dev_addr - 1] == MSC_CSW_STATUS_PASSED)
            break;

        if (attempt == 2)
            return false; // still not ready after retries
    }

    // Read Capacity to get geometry.
    // CBI/UFI devices use READ FORMAT CAPACITIES (0x23) which provides
    // a descriptor type field indicating whether format detection is
    // complete (type 2 = formatted). This avoids a race where the
    // floppy drive reports TUR=ready before it has sensed the media
    // format, causing READ CAPACITY to return DD (720KB) geometry
    // instead of HD (1.44MB).
    if (tuh_msc_is_cbi(dev_addr))
    {
        ufi_format_capacity_resp_t fmt_resp;
        for (int attempt = 0; attempt < 5; attempt++)
        {
            if (!wait_for_ready(dev_addr))
                return false;
            memset(&fmt_resp, 0, sizeof(fmt_resp));
            msc_tuh_dev_busy[dev_addr - 1] = true;
            if (!tuh_msc_read_format_capacities(dev_addr, lun, &fmt_resp,
                                                sizeof(fmt_resp), disk_io_complete, 0))
            {
                msc_tuh_dev_busy[dev_addr - 1] = false;
                return false;
            }
            wait_for_disk_io(dev_addr);
            if (msc_tuh_dev_csw_status[dev_addr - 1] != MSC_CSW_STATUS_PASSED)
                return false;

            uint8_t desc_type = fmt_resp.descriptor_type & 0x03;
            if (desc_type == 2) // formatted media, geometry is valid
            {
                msc_volume_block_count[vol] = tu_ntohl(fmt_resp.num_blocks);
                msc_volume_block_size[vol] = ((uint32_t)fmt_resp.block_size_hi << 16) |
                                             ((uint32_t)fmt_resp.block_size_mid << 8) |
                                             (uint32_t)fmt_resp.block_size_lo;
                msc_volume_status[vol] = msc_volume_mounted;
                msc_volume_tur_ok[vol] = true;
                DBG("MSC vol %d: media detected (fmt cap)\n", vol);
                return true;
            }
            if (desc_type == 3) // no media present
                return false;

            // desc_type 1 (unformatted) or 0: drive is still sensing, retry
            DBG("MSC vol %d: format not ready (type %d), retrying\n", vol, desc_type);
            sleep_ms(200);
        }
        DBG("MSC vol %d: format detection timed out\n", vol);
        return false;
    }

    // BOT / non-UFI: use standard Read Capacity
    scsi_read_capacity10_resp_t cap_resp;
    if (!wait_for_ready(dev_addr))
        return false;
    msc_tuh_dev_busy[dev_addr - 1] = true;
    if (!tuh_msc_read_capacity(dev_addr, lun, &cap_resp, disk_io_complete, 0))
    {
        msc_tuh_dev_busy[dev_addr - 1] = false;
        return false;
    }
    wait_for_disk_io(dev_addr);
    if (msc_tuh_dev_csw_status[dev_addr - 1] != MSC_CSW_STATUS_PASSED)
        return false;

    msc_volume_block_count[vol] = tu_ntohl(cap_resp.last_lba) + 1;
    msc_volume_block_size[vol] = tu_ntohl(cap_resp.block_size);
    msc_volume_status[vol] = msc_volume_mounted;
    msc_volume_tur_ok[vol] = true;
    DBG("MSC vol %d: media detected\n", vol);
    return true;
}

// Handle I/O error on a removable volume: unmount and mark ejected.
static void msc_handle_io_error(uint8_t vol)
{
    if (!msc_inquiry_resp[vol].is_removable)
        return;
    for (int i = 0; i < MSC_STD_FIL_MAX; i++)
    {
        if (msc_std_fil_pool[i].obj.fs == &msc_fatfs_volumes[vol])
            msc_std_fil_pool[i].obj.fs = NULL;
    }

    // Re-register deferred mount so FatFS probes again on next access
    TCHAR volstr[6] = MSC_VOL0;
    volstr[3] += vol;
    f_mount(&msc_fatfs_volumes[vol], volstr, 0);
    msc_volume_status[vol] = msc_volume_ejected;
    msc_volume_tur_ok[vol] = false;
    msc_volume_block_count[vol] = 0;
    msc_volume_block_size[vol] = 0;
    DBG("MSC vol %d: media removed (I/O error)\n", vol);
}

// Initialize a newly registered volume: inquiry + read capacity.
// Runs from main loop context where the control pipe is free.
static bool msc_init_volume(uint8_t vol)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    uint8_t const lun = 0;

    if (!tuh_msc_mounted(dev_addr))
        return false;

    // SCSI Inquiry
    if (!wait_for_ready(dev_addr))
        return false;
    memset(&msc_inquiry_resp[vol], 0, sizeof(msc_inquiry_resp[vol]));
    msc_tuh_dev_busy[dev_addr - 1] = true;
    if (!tuh_msc_inquiry(dev_addr, lun, &msc_inquiry_resp[vol], disk_io_complete, 0))
    {
        msc_tuh_dev_busy[dev_addr - 1] = false;
        return false;
    }
    wait_for_disk_io(dev_addr);

    if (msc_tuh_dev_csw_status[dev_addr - 1] != MSC_CSW_STATUS_PASSED)
    {
        // CBI/UFI floppy drives report pending sense (e.g. "Medium Not
        // Present") in the interrupt status even for INQUIRY, which does
        // not require media.  The bulk data phase already delivered the
        // 36-byte response.  Validate the response before continuing:
        // peripheral_qualifier 3 = LUN not supported, response_data_format
        // must be 1 (CCS) or 2 (SPC), additional_length >= 31 means the
        // full 36-byte standard response was returned.
        scsi_inquiry_resp_t const *inq = &msc_inquiry_resp[vol];
        if (inq->peripheral_qualifier == 3 ||
            (inq->response_data_format != 1 && inq->response_data_format != 2) ||
            inq->additional_length < 31)
        {
            DBG("MSC vol %d: inquiry failed (invalid response)\n", vol);
            msc_volume_status[vol] = msc_volume_failed;
            return false;
        }
        DBG("MSC vol %d: inquiry CSW not passed (response valid, continuing)\n", vol);
    }

    if (msc_inquiry_resp[vol].is_removable)
    {
        msc_volume_status[vol] = msc_volume_ejected;
        return msc_probe_media(vol);
    }

    // Non-removable: use Read Capacity (CBI/UFI non-removable is rare but handle it)
    scsi_read_capacity10_resp_t cap_resp;
    if (!wait_for_ready(dev_addr))
        return false;
    msc_tuh_dev_busy[dev_addr - 1] = true;
    if (!tuh_msc_read_capacity(dev_addr, lun, &cap_resp, disk_io_complete, 0))
    {
        msc_tuh_dev_busy[dev_addr - 1] = false;
        msc_volume_status[vol] = msc_volume_failed;
        return false;
    }
    wait_for_disk_io(dev_addr);
    if (msc_tuh_dev_csw_status[dev_addr - 1] != MSC_CSW_STATUS_PASSED)
    {
        msc_volume_status[vol] = msc_volume_failed;
        return false;
    }

    msc_volume_block_count[vol] = tu_ntohl(cap_resp.last_lba) + 1;
    msc_volume_block_size[vol] = tu_ntohl(cap_resp.block_size);
    msc_volume_status[vol] = msc_volume_mounted;
    DBG("MSC vol %d: initialized %lu blocks\n", vol,
        (unsigned long)msc_volume_block_count[vol]);
    return true;
}

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
static void msc_inquiry_rtrims(uint8_t *s, size_t l)
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
    {
        if (msc_volume_status[vol] == msc_volume_registered)
            msc_init_volume(vol);
        else if (msc_volume_status[vol] == msc_volume_ejected)
            msc_probe_media(vol);
        else if (msc_volume_status[vol] == msc_volume_mounted &&
                 msc_inquiry_resp[vol].is_removable)
        {
            msc_volume_tur_ok[vol] = false;
            disk_status(vol);
        }
        if (msc_volume_status[vol] == msc_volume_mounted ||
            msc_volume_status[vol] == msc_volume_ejected)
            count++;
    }
    return count;
}

int msc_status_response(char *buf, size_t buf_size, int state)
{
    if (state < 0 || state >= FF_VOLUMES)
        return -1;
    if (msc_volume_status[state] == msc_volume_mounted ||
        msc_volume_status[state] == msc_volume_ejected)
    {
        char sizebuf[24];
        if (msc_volume_status[state] == msc_volume_ejected)
        {
            snprintf(sizebuf, sizeof(sizebuf), "%s", STR_PARENS_NO_MEDIA);
        }
        else
        {
            // Floppy-era media (under 5 MB): raw/1024/1000
            // Everything else: pure decimal raw/1000/1000
            const char *xb;
            double size;
            uint64_t raw = (uint64_t)msc_volume_block_count[state] *
                           (uint64_t)msc_volume_block_size[state];
            if (raw < 5000000ULL)
            {
                xb = "KB";
                size = raw / 1024.0;
                if (size >= 1000)
                {
                    xb = "MB";
                    size /= 1000;
                }
                // no %g, strip zeros manually
                char num[16];
                snprintf(num, sizeof(num), "%.3f", size);
                char *p = num + strlen(num) - 1;
                while (*p == '0')
                    *p-- = '\0';
                if (*p == '.')
                    *p = '\0';
                snprintf(sizebuf, sizeof(sizebuf), "%s %s", num, xb);
            }
            else
            {
                xb = "MB";
                size = raw / 1e6;
                if (size >= 1000)
                {
                    xb = "GB";
                    size /= 1000;
                }
                if (size >= 1000)
                {
                    xb = "TB";
                    size /= 1000;
                }
                snprintf(sizebuf, sizeof(sizebuf), "%.1f %s", size, xb);
            }
        }
        msc_inquiry_rtrims(msc_inquiry_resp[state].vendor_id, 8);
        msc_inquiry_rtrims(msc_inquiry_resp[state].product_id, 16);
        msc_inquiry_rtrims(msc_inquiry_resp[state].product_rev, 4);
        snprintf(buf, buf_size, STR_STATUS_MSC,
                 VolumeStr[state],
                 sizebuf,
                 msc_inquiry_resp[state].vendor_id,
                 msc_inquiry_resp[state].product_id,
                 msc_inquiry_resp[state].product_rev);
    }
    return state + 1;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
    // TinyUSB callback when a USB mass storage device is attached.
    // Register a deferred FatFS mount; SCSI commands (inquiry,
    // read capacity) run later from disk_initialize on the main
    // loop where the control pipe is guaranteed free.
    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_volume_status[vol] == msc_volume_free)
        {
            msc_volume_dev_addr[vol] = dev_addr;
            msc_volume_status[vol] = msc_volume_registered;
            TCHAR volstr[6] = MSC_VOL0;
            volstr[3] += vol;
            f_mount(&msc_fatfs_volumes[vol], volstr, 0);
            DBG("MSC mount dev_addr %d -> vol %d\n", dev_addr, vol);
            break;
        }
    }
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    // TinyUSB callback when a USB mass storage device is detached.
    // Clear the busy flag first — msch_close() never fires the
    // user complete_cb on disconnect, so any pending
    // wait_for_disk_io would spin forever without this.
    msc_tuh_dev_busy[dev_addr - 1] = false;

    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_volume_dev_addr[vol] == dev_addr &&
            msc_volume_status[vol] != msc_volume_free)
        {
            TCHAR volstr[6] = MSC_VOL0;
            volstr[3] += vol;
            f_unmount(volstr);
            msc_volume_status[vol] = msc_volume_free;
            msc_volume_dev_addr[vol] = 0;
            msc_volume_block_count[vol] = 0;
            msc_volume_block_size[vol] = 0;
            DBG("MSC unmounted dev_addr %d from vol %d\n", dev_addr, vol);
        }
    }
}

static DRESULT msc_scsi_xfer(uint8_t pdrv, msc_cbw_t *cbw, void *buff)
{
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
    msc_tuh_dev_csw_status[dev_addr - 1] = MSC_CSW_STATUS_FAILED;
    msc_tuh_dev_busy[dev_addr - 1] = true;
    if (!wait_for_ready(dev_addr))
    {
        msc_tuh_dev_busy[dev_addr - 1] = false;
        return RES_ERROR;
    }
    if (!tuh_msc_scsi_command(dev_addr, cbw, buff, disk_io_complete, 0))
    {
        msc_tuh_dev_busy[dev_addr - 1] = false;
        msc_handle_io_error(pdrv);
        return RES_ERROR;
    }
    wait_for_disk_io(dev_addr);
    if (msc_tuh_dev_csw_status[dev_addr - 1] != MSC_CSW_STATUS_PASSED)
    {
        msc_handle_io_error(pdrv);
        return RES_ERROR;
    }
    return RES_OK;
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
    if (msc_volume_status[pdrv] == msc_volume_ejected ||
        msc_volume_status[pdrv] == msc_volume_registered)
        return STA_NOINIT;
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
    if (!tuh_msc_mounted(dev_addr))
        return STA_NOINIT;

    // TUR on removable volumes so FatFS detects media removal
    if (msc_inquiry_resp[pdrv].is_removable && !msc_volume_tur_ok[pdrv])
    {
        uint8_t const lun = 0;
        if (!wait_for_ready(dev_addr))
            return STA_NOINIT;
        msc_tuh_dev_busy[dev_addr - 1] = true;
        bool ok = tuh_msc_test_unit_ready(dev_addr, lun, disk_io_complete, 0);
        if (ok)
        {
            wait_for_disk_io(dev_addr);
            ok = (msc_tuh_dev_csw_status[dev_addr - 1] == MSC_CSW_STATUS_PASSED);
        }
        else
        {
            msc_tuh_dev_busy[dev_addr - 1] = false;
        }
        if (!ok)
        {
            msc_handle_io_error(pdrv);
            return STA_NOINIT;
        }
        msc_volume_tur_ok[pdrv] = true;
    }

    return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (msc_volume_status[pdrv] == msc_volume_registered)
    {
        if (!msc_init_volume(pdrv))
            return STA_NOINIT;
    }
    if (msc_volume_status[pdrv] == msc_volume_ejected)
    {
        if (!msc_probe_media(pdrv))
            return STA_NOINIT;
    }
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    uint32_t const block_size = msc_volume_block_size[pdrv];
    DBG("MSC R> %lu+%u\n", (unsigned long)sector, count);
    if (block_size == 0)
        return RES_ERROR;

    msc_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = MSC_CBW_SIGNATURE;
    cbw.tag = 0x54555342;
    cbw.lun = 0;
    cbw.total_bytes = count * block_size;
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = sizeof(scsi_read10_t);
    scsi_read10_t const cmd = {
        .cmd_code = SCSI_CMD_READ_10,
        .lba = tu_htonl(sector),
        .block_count = tu_htons((uint16_t)count)};
    memcpy(cbw.command, &cmd, sizeof(cmd));

    return msc_scsi_xfer(pdrv, &cbw, buff);
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    uint32_t const block_size = msc_volume_block_size[pdrv];
    DBG("MSC W> %lu+%u\n", (unsigned long)sector, count);
    if (block_size == 0)
        return RES_ERROR;

    msc_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = MSC_CBW_SIGNATURE;
    cbw.tag = 0x54555342;
    cbw.lun = 0;
    cbw.total_bytes = count * block_size;
    cbw.dir = TUSB_DIR_OUT;
    cbw.cmd_len = sizeof(scsi_write10_t);
    scsi_write10_t const cmd = {
        .cmd_code = SCSI_CMD_WRITE_10,
        .lba = tu_htonl(sector),
        .block_count = tu_htons((uint16_t)count)};
    memcpy(cbw.command, &cmd, sizeof(cmd));

    return msc_scsi_xfer(pdrv, &cbw, (void *)(uintptr_t)buff);
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    switch (cmd)
    {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *((DWORD *)buff) = msc_volume_block_count[pdrv];
        return RES_OK;
    case GET_SECTOR_SIZE:
        *((WORD *)buff) = (WORD)msc_volume_block_size[pdrv];
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
