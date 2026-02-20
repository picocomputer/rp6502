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

// Timeout for read/write/sync SCSI commands.
// Needs headroom for 3.5" floppy disk drives.
#define MSC_SCSI_RW_TIMEOUT_MS 2000

// Overall deadline for msc_init_volume (first mount or re-probe).
// Covers INQUIRY + TUR + SENSE + START UNIT + delay + retry + capacity + mount.
#define MSC_INIT_TIMEOUT_MS 4000

// Time budget for any single USB MSC operation: one SCSI command or
// one reset recovery sequence (BOT: BMR + 2x CLEAR_HALT; CBI: class
// reset).  Also the floor given to each init stage so a nearly-expired
// overall deadline cannot starve an individual command.
#define MSC_OP_TIMEOUT_MS 500

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

// Build a FatFS volume path like "MSC0:" for volume.
static inline void msc_vol_path(TCHAR buf[6], uint8_t vol)
{
    static_assert(FF_VOLUMES <= 10);
    memcpy(buf, "MSC0:", 6);
    buf[3] += vol;
}

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
static uint8_t msc_volume_sense_key[FF_VOLUMES];
static uint8_t msc_volume_sense_asc[FF_VOLUMES];
static uint8_t msc_volume_sense_ascq[FF_VOLUMES];
static bool msc_volume_write_protected[FF_VOLUMES];

// This driver requires our custom TinyUSB: src/ria/usb/msc_host.c.
// It will not work with upstream: src/tinyusb/src/class/msc/msc_host.c
// These additional interfaces are not in upstream TinyUSB.
bool tuh_msc_is_cbi(uint8_t dev_addr);
bool tuh_msc_reset_recovery(uint8_t dev_addr);
bool tuh_msc_recovery_in_progress(uint8_t dev_addr);
void tuh_msc_abort_recovery(uint8_t dev_addr);

//--------------------------------------------------------------------+
// Synchronous I/O helpers
//--------------------------------------------------------------------+
// Spin-wait helpers that pump USB events via main_task() while waiting
// for in-flight commands to complete.

// Local superset of TinyUSB's msc_csw_status_t.
// We need an additional value for timeout.
typedef enum
{
    msc_status_passed,      // MSC_CSW_STATUS_PASSED
    msc_status_failed,      // MSC_CSW_STATUS_FAILED
    msc_status_phase_error, // MSC_CSW_STATUS_PHASE_ERROR
    msc_status_timed_out,
} msc_status_t;

static volatile bool msc_sync_busy[CFG_TUH_DEVICE_MAX];
static msc_status_t msc_sync_csw_status[CFG_TUH_DEVICE_MAX];

static bool msc_sync_complete_cb(uint8_t dev_addr,
                                 tuh_msc_complete_data_t const *cb_data)
{
    DBG("CSW: sig=%08lX tag=%08lX residue=%lu status=%u\n",
        (unsigned long)cb_data->csw->signature,
        (unsigned long)cb_data->csw->tag,
        (unsigned long)cb_data->csw->data_residue,
        cb_data->csw->status);
    msc_sync_csw_status[dev_addr - 1] = (msc_status_t)cb_data->csw->status;
    msc_sync_busy[dev_addr - 1] = false;
    return true;
}

// Wait for in-flight command to complete.
static msc_status_t msc_sync_wait_io(uint8_t dev_addr, absolute_time_t deadline)
{
    while (msc_sync_busy[dev_addr - 1])
    {
        if (!tuh_msc_mounted(dev_addr))
        {
            msc_sync_busy[dev_addr - 1] = false;
            return msc_status_timed_out;
        }
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
        {
            tuh_msc_reset_recovery(dev_addr);
            absolute_time_t rec_deadline = make_timeout_time_ms(MSC_OP_TIMEOUT_MS);
            while (tuh_msc_recovery_in_progress(dev_addr))
            {
                if (absolute_time_diff_us(get_absolute_time(), rec_deadline) <= 0)
                {
                    tuh_msc_abort_recovery(dev_addr);
                    break;
                }
                main_task();
            }
            msc_sync_busy[dev_addr - 1] = false;
            return msc_status_timed_out;
        }
        main_task();
    }
    return msc_sync_csw_status[dev_addr - 1];
}

// Return deadline or now+MSC_OP_TIMEOUT_MS, whichever is later.
// Prevents a nearly-expired deadline from starving a command.
static inline absolute_time_t msc_sync_deadline(absolute_time_t deadline)
{
    absolute_time_t floor = make_timeout_time_ms(MSC_OP_TIMEOUT_MS);
    return absolute_time_diff_us(deadline, floor) > 0 ? floor : deadline;
}

//--------------------------------------------------------------------+
// Synchronous SCSI command wrappers
//--------------------------------------------------------------------+
// Each wrapper builds a CDB, submits it via the async transport API,
// and blocks until completion.  Returns the CSW status code.

// Initialize a CBW with standard boilerplate fields.
static inline void msc_cbw_init(msc_cbw_t *cbw, uint8_t lun)
{
    memset(cbw, 0, sizeof(msc_cbw_t));
    cbw->signature = MSC_CBW_SIGNATURE;
    cbw->tag = 0x54555342; // "TUSB"
    cbw->lun = lun;
}

// Wait for transport ready, submit command, and wait for completion.
// No autosense - used directly for REQUEST SENSE itself.
static msc_status_t msc_scsi_sync_raw(uint8_t dev_addr, msc_cbw_t *cbw,
                                      void *data, absolute_time_t deadline)
{
    while (!tuh_msc_ready(dev_addr))
    {
        if (!tuh_msc_mounted(dev_addr))
            return msc_status_failed;
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
            return msc_status_failed;
        main_task();
    }
    msc_sync_csw_status[dev_addr - 1] = msc_status_failed;
    msc_sync_busy[dev_addr - 1] = true;
    if (!tuh_msc_scsi_command(dev_addr, cbw, data, msc_sync_complete_cb, 0))
    {
        msc_sync_busy[dev_addr - 1] = false;
        return msc_status_failed;
    }
    return msc_sync_wait_io(dev_addr, deadline);
}

// Core submit-and-wait helper with autosense. On non-PASSED status,
// automatically issues REQUEST SENSE and populates the per-volume
// sense arrays. Callers never need to explicitly request sense -
// it's already available after any failed command.
static msc_status_t msc_scsi_sync(uint8_t vol, msc_cbw_t *cbw,
                                  void *data, absolute_time_t deadline)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    if (dev_addr == 0)
        return msc_status_failed;
    msc_status_t status = msc_scsi_sync_raw(dev_addr, cbw, data, deadline);
    if (status != msc_status_passed)
    {
        scsi_sense_fixed_resp_t sense_resp;
        memset(&sense_resp, 0, sizeof(sense_resp));
        msc_cbw_t sense_cbw;
        msc_cbw_init(&sense_cbw, 0);
        sense_cbw.total_bytes = sizeof(scsi_sense_fixed_resp_t);
        sense_cbw.dir = TUSB_DIR_IN_MASK;
        sense_cbw.cmd_len = sizeof(scsi_request_sense_t);
        scsi_request_sense_t const sense_cmd = {
            .cmd_code = SCSI_CMD_REQUEST_SENSE,
            .alloc_length = sizeof(scsi_sense_fixed_resp_t)};
        memcpy(sense_cbw.command, &sense_cmd, sizeof(sense_cmd));
        msc_scsi_sync_raw(dev_addr, &sense_cbw, &sense_resp,
                          msc_sync_deadline(deadline));
        if (sense_resp.response_code)
        {
            msc_volume_sense_key[vol] = sense_resp.sense_key;
            msc_volume_sense_asc[vol] = sense_resp.add_sense_code;
            msc_volume_sense_ascq[vol] = sense_resp.add_sense_qualifier;
        }
        else
        {
            msc_volume_sense_key[vol] = SCSI_SENSE_NONE;
            msc_volume_sense_asc[vol] = 0;
            msc_volume_sense_ascq[vol] = 0;
        }
        DBG("MSC vol %d: autosense %d/%02Xh/%02Xh\n",
            vol, msc_volume_sense_key[vol],
            msc_volume_sense_asc[vol],
            msc_volume_sense_ascq[vol]);
    }
    return status;
}

static msc_status_t msc_inquiry_sync(uint8_t vol, scsi_inquiry_resp_t *resp,
                                     absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = sizeof(scsi_inquiry_resp_t);
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = sizeof(scsi_inquiry_t);
    scsi_inquiry_t const cmd = {
        .cmd_code = SCSI_CMD_INQUIRY,
        .alloc_length = sizeof(scsi_inquiry_resp_t)};
    memcpy(cbw.command, &cmd, sizeof(cmd));
    return msc_scsi_sync(vol, &cbw, resp, deadline);
}

static msc_status_t msc_tur_sync(uint8_t vol, absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = 0;
    cbw.dir = TUSB_DIR_OUT;
    cbw.cmd_len = sizeof(scsi_test_unit_ready_t);
    cbw.command[0] = SCSI_CMD_TEST_UNIT_READY;
    return msc_scsi_sync(vol, &cbw, NULL, deadline);
}

static msc_status_t msc_read_capacity10_sync(uint8_t vol,
                                             scsi_read_capacity10_resp_t *resp,
                                             absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = sizeof(scsi_read_capacity10_resp_t);
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = sizeof(scsi_read_capacity10_t);
    cbw.command[0] = SCSI_CMD_READ_CAPACITY_10;
    return msc_scsi_sync(vol, &cbw, resp, deadline);
}

static msc_status_t msc_read_format_capacities_sync(uint8_t vol, void *resp,
                                                    uint8_t alloc_length,
                                                    absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = alloc_length;
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = 12;      // UFI 12-byte CDB
    cbw.command[0] = 0x23; // READ FORMAT CAPACITIES
    cbw.command[7] = (alloc_length >> 8) & 0xFF;
    cbw.command[8] = alloc_length & 0xFF;
    return msc_scsi_sync(vol, &cbw, resp, deadline);
}

static msc_status_t msc_mode_sense6_sync(uint8_t vol, uint8_t page_code,
                                         scsi_mode_sense6_resp_t *resp,
                                         absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = sizeof(scsi_mode_sense6_resp_t);
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = sizeof(scsi_mode_sense6_t);
    scsi_mode_sense6_t const cmd = {
        .cmd_code = SCSI_CMD_MODE_SENSE_6,
        .disable_block_descriptor = 1,
        .page_code = page_code,
        .alloc_length = sizeof(scsi_mode_sense6_resp_t),
    };
    memcpy(cbw.command, &cmd, sizeof(cmd));
    return msc_scsi_sync(vol, &cbw, resp, deadline);
}

static msc_status_t msc_sync_cache10_sync(uint8_t vol,
                                          absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = 0;
    cbw.dir = TUSB_DIR_OUT;
    cbw.cmd_len = 10;
    cbw.command[0] = 0x35; // SYNCHRONIZE CACHE (10)
    return msc_scsi_sync(vol, &cbw, NULL, deadline);
}

static msc_status_t msc_start_stop_unit_sync(uint8_t vol, bool start,
                                             bool load_eject,
                                             absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = 0;
    cbw.dir = TUSB_DIR_OUT;
    cbw.cmd_len = sizeof(scsi_start_stop_unit_t);
    scsi_start_stop_unit_t const cmd = {
        .cmd_code = SCSI_CMD_START_STOP_UNIT,
        .start = start ? 1u : 0u,
        .load_eject = load_eject ? 1u : 0u,
    };
    memcpy(cbw.command, &cmd, sizeof(cmd));
    return msc_scsi_sync(vol, &cbw, NULL, deadline);
}

//--------------------------------------------------------------------+
// SCSI read and write commands with automatic error recovery
//--------------------------------------------------------------------+

// Handle SCSI transfer failure: perform reset recovery if appropriate
// and mark removable volume as ejected.
// Pass the CSW status returned by the failed command; when it is
// msc_status_timed_out, msc_sync_wait_io() already ran recovery
// and we must not start a second overlapping one.
static void msc_xfer_error(uint8_t vol, msc_status_t status)
{
    uint8_t const dev_addr = msc_volume_dev_addr[vol];
    DBG("MSC xfer fail: timed_out=%d, cbi=%d\n",
        status == msc_status_timed_out, tuh_msc_is_cbi(dev_addr));
    if (dev_addr && status != msc_status_timed_out)
    {
        tuh_msc_reset_recovery(dev_addr);
        absolute_time_t deadline = make_timeout_time_ms(MSC_OP_TIMEOUT_MS);
        while (tuh_msc_recovery_in_progress(dev_addr))
        {
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
            {
                tuh_msc_abort_recovery(dev_addr);
                break;
            }
            main_task();
        }
    }
}

// Mark a removable volume as ejected and clear cached geometry.
static void msc_handle_io_error(uint8_t vol)
{
    DBG("MSC vol %d: msc_handle_io_error\n", vol);
    if (!msc_inquiry_resp[vol].is_removable)
        return;
    if (msc_volume_status[vol] == msc_volume_free ||
        msc_volume_status[vol] == msc_volume_ejected)
        return;
    msc_volume_status[vol] = msc_volume_ejected;
    msc_volume_tur_ok[vol] = false;
    msc_volume_block_count[vol] = 0;
    msc_volume_block_size[vol] = 0;
    msc_volume_write_protected[vol] = false;
    DBG("MSC vol %d: media ejected\n", vol);
}

static bool msc_read10_sync(uint8_t vol, void *buff,
                            uint32_t lba, uint16_t block_count,
                            uint32_t block_size,
                            absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = block_count * block_size;
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = sizeof(scsi_read10_t);
    scsi_read10_t const cmd = {
        .cmd_code = SCSI_CMD_READ_10,
        .lba = tu_htonl(lba),
        .block_count = tu_htons(block_count)};
    memcpy(cbw.command, &cmd, sizeof(cmd));
    msc_status_t status = msc_scsi_sync(vol, &cbw, buff, deadline);
    if (status != msc_status_passed)
    {
        msc_xfer_error(vol, status);
        msc_handle_io_error(vol);
        return false;
    }
    return true;
}

static bool msc_write10_sync(uint8_t vol, const void *buff,
                             uint32_t lba, uint16_t block_count,
                             uint32_t block_size,
                             absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = block_count * block_size;
    cbw.dir = TUSB_DIR_OUT;
    cbw.cmd_len = sizeof(scsi_write10_t);
    scsi_write10_t const cmd = {
        .cmd_code = SCSI_CMD_WRITE_10,
        .lba = tu_htonl(lba),
        .block_count = tu_htons(block_count)};
    memcpy(cbw.command, &cmd, sizeof(cmd));
    // Cast away const: transport API uses void* for both directions.
    msc_status_t status = msc_scsi_sync(vol, &cbw, (void *)(uintptr_t)buff, deadline);
    if (status != msc_status_passed)
    {
        msc_xfer_error(vol, status);
        msc_handle_io_error(vol);
        return false;
    }
    return true;
}

//--------------------------------------------------------------------+
// Shared SCSI initialization helpers
//--------------------------------------------------------------------+
// Building blocks of msc_init_volume().  Each function performs one
// SCSI operation synchronously using the sync helpers.  All use an
// absolute_time_t deadline to coordinate timeouts across the full
// init sequence.

// Read device capacity.
// CBI: READ FORMAT CAPACITIES (UFI mandatory command).
// BOT: READ CAPACITY(10) (SBC mandatory command).
// Returns true on success, populating block_count and block_size.
static bool msc_read_capacity(uint8_t vol, absolute_time_t deadline)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];

    if (!tuh_msc_mounted(dev_addr))
        return false;

    if (tuh_msc_is_cbi(dev_addr))
    {
        // CBI: READ FORMAT CAPACITIES
        uint8_t rfc[12];
        memset(rfc, 0, sizeof(rfc));
        if (msc_read_format_capacities_sync(vol, rfc, sizeof(rfc),
                                            deadline) !=
            msc_status_passed)
            return false;

        uint8_t list_len = rfc[3];
        if (list_len < 8)
            return false;
        uint8_t desc_type = rfc[4 + 4] & 0x03;
        if (desc_type == 3) // no media
            return false;
        uint8_t *desc0 = &rfc[4];
        uint32_t blocks = tu_ntohl(tu_unaligned_read32(desc0));
        uint32_t bsize = ((uint32_t)desc0[5] << 16) |
                         ((uint32_t)desc0[6] << 8) |
                         (uint32_t)desc0[7];
        DBG("MSC vol %d: RFC %lu blocks, %lu bytes/block, type %d\n",
            vol, (unsigned long)blocks, (unsigned long)bsize, desc_type);
        if (blocks == 0 || bsize != 512)
            return false;
        msc_volume_block_count[vol] = blocks;
        msc_volume_block_size[vol] = bsize;
    }
    else
    {
        // BOT: READ CAPACITY(10)
        scsi_read_capacity10_resp_t cap10;
        memset(&cap10, 0, sizeof(cap10));
        if (msc_read_capacity10_sync(vol, &cap10,
                                     deadline) !=
            msc_status_passed)
        {
            // Autosense already populated sense data.
            if (msc_inquiry_resp[vol].is_removable &&
                msc_volume_sense_asc[vol] == 0x3A)
                DBG("MSC vol %d: capacity - medium not present\n", vol);
            return false;
        }
        uint32_t last_lba = tu_ntohl(cap10.last_lba);
        if (last_lba == 0xFFFFFFFF)
            return false; // > 2 TB, unsupported
        msc_volume_block_count[vol] = last_lba + 1;
        msc_volume_block_size[vol] = tu_ntohl(cap10.block_size);
    }

    return true;
}

// Read block 0 to force USB devices to populate mode pages with real
// values before MODE SENSE (Linux read_before_ms quirk).
// BOT only, non-fatal; data is discarded.
static void msc_read_block_zero(uint8_t vol, absolute_time_t deadline)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    uint32_t bs = msc_volume_block_size[vol];

    if (bs == 0 || bs > 512 || !tuh_msc_mounted(dev_addr))
        return;

    if (tuh_msc_is_cbi(dev_addr))
        return; // CBI: not needed

    // Non-fatal probe read: call msc_scsi_sync_raw directly to avoid
    // autosense and triggering error recovery on failure. Ignore result.
    uint8_t block0[512];
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = bs;
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = sizeof(scsi_read10_t);
    scsi_read10_t const cmd = {
        .cmd_code = SCSI_CMD_READ_10,
        .lba = tu_htonl(0),
        .block_count = tu_htons(1)};
    memcpy(cbw.command, &cmd, sizeof(cmd));
    msc_scsi_sync_raw(dev_addr, &cbw, block0, deadline);
}

// Determine write protection via MODE SENSE(6).  BOT only; CBI
// devices skip this (assumed not protected).  Non-fatal: defaults
// to not protected on failure.
static void msc_read_write_protect(uint8_t vol, absolute_time_t deadline)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    msc_volume_write_protected[vol] = false;

    if (tuh_msc_is_cbi(dev_addr))
        return;

    if (!tuh_msc_mounted(dev_addr))
        return;

    // Try MODE SENSE(6) page 0x3F (all pages)
    scsi_mode_sense6_resp_t ms;
    memset(&ms, 0, sizeof(ms));
    if (msc_mode_sense6_sync(vol, 0x3F, &ms, deadline) == msc_status_passed)
    {
        DBG("MSC vol %d: MODE SENSE WP=%d\n", vol, ms.write_protected);
        msc_volume_write_protected[vol] = ms.write_protected;
        return;
    }

    // Page 0x3F rejected.  Fall back to page 0. Linux does this.
    DBG("MSC vol %d: MODE SENSE page 0x3F rejected, trying page 0\n", vol);

    memset(&ms, 0, sizeof(ms));
    if (msc_mode_sense6_sync(vol, 0x00, &ms, deadline) == msc_status_passed)
    {
        DBG("MSC vol %d: MODE SENSE page 0 WP=%d\n", vol, ms.write_protected);
        msc_volume_write_protected[vol] = ms.write_protected;
    }
    else
    {
        DBG("MSC vol %d: MODE SENSE not supported\n", vol);
    }
}

//--------------------------------------------------------------------+
// Synchronous volume initialization
//--------------------------------------------------------------------+
// Called from disk_initialize() for both newly registered and ejected
// volumes.  Performs the full SCSI init sequence synchronously using
// the sync helpers (which pump USB events via main_task()).
//
// For first mount (registered), sends INQUIRY to identify the device.
// For re-probe (ejected), cached inquiry data is reused.
//
// Returns the resulting volume status:
//   msc_volume_mounted  - success
//   msc_volume_ejected  - removable media not present
//   msc_volume_failed   - non-recoverable error

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

static msc_volume_status_t msc_init_volume(uint8_t vol)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    scsi_inquiry_resp_t *inq = &msc_inquiry_resp[vol];
    absolute_time_t deadline = make_timeout_time_ms(MSC_INIT_TIMEOUT_MS);

    // ---- INQUIRY (first mount only) ----
    if (msc_volume_status[vol] == msc_volume_registered)
    {
        bool inquiry_ok = false;
        if (!tuh_msc_mounted(dev_addr))
            return msc_volume_failed;
        memset(inq, 0, sizeof(*inq));
        msc_status_t csw = msc_inquiry_sync(vol, inq, msc_sync_deadline(deadline));
        msc_inquiry_rtrims(inq->vendor_id, 8);
        msc_inquiry_rtrims(inq->product_id, 16);
        msc_inquiry_rtrims(inq->product_rev, 4);

        if (csw == msc_status_passed)
        {
            inquiry_ok = true;
        }
        // Accept usable response data despite CSW failure (common on CBI)
        else if (inq->peripheral_qualifier != 3 &&
                 (inq->response_data_format == 1 ||
                  inq->response_data_format == 2) &&
                 inq->additional_length >= 31)
        {
            DBG("MSC vol %d: INQUIRY CSW failed but response valid\n", vol);
            inquiry_ok = true;
        }
        if (!inquiry_ok)
        {
            DBG("MSC vol %d: INQUIRY failed\n", vol);
            return msc_volume_failed;
        }
        DBG("MSC vol %d: %.8s %.16s rev %.4s%s\n", vol,
            inq->vendor_id, inq->product_id, inq->product_rev,
            inq->is_removable ? " (removable)" : "");
    }

    // ---- TUR / CLEAR UNIT ATTENTION ----
    // Poll TEST UNIT READY until the device is ready, or we're sure
    // media is absent.  Per SPC-4, after media change the device may
    // report UNIT ATTENTION (sense key 6) or NOT READY (sense key 2)
    // while the device initialises the card.  We retry for any
    // non-ready condition except ASC 0x3A (Medium Not Present),
    // which means the slot is genuinely empty.
    bool tur_ok = false;
    bool sent_start = false;
    for (int attempt = 0; attempt < 8; attempt++)
    {
        if (!tuh_msc_mounted(dev_addr))
            return msc_volume_failed;
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
            break;
        if (msc_tur_sync(vol, msc_sync_deadline(deadline)) == msc_status_passed)
        {
            tur_ok = true;
            break;
        }
        uint8_t sk = msc_volume_sense_key[vol];
        uint8_t asc = msc_volume_sense_asc[vol];
        uint8_t ascq = msc_volume_sense_ascq[vol];
        // Medium Not Present - no point retrying
        if (asc == 0x3A)
            break;
        // NOT READY (2) or UNIT ATTENTION (6) - retry
        if (sk == SCSI_SENSE_NOT_READY || sk == SCSI_SENSE_UNIT_ATTENTION)
        {
            // Per SBC-4 §5.25: on first NOT READY, send START STOP UNIT
            // (Start=1) once to kick media initialisation.
            // Skip subcases that won't resolve without intervention.
            if (!sent_start && sk == SCSI_SENSE_NOT_READY &&
                inq->is_removable)
            {
                if (asc == 0x04)
                {
                    // Manual intervention, standby, unavailable - hopeless
                    if (ascq == 0x03 || ascq == 0x0B || ascq == 0x0C)
                        break;
                }
                sent_start = true;
                DBG("MSC vol %d: START STOP UNIT (Start)\n", vol);
                msc_start_stop_unit_sync(vol, true, false,
                                         msc_sync_deadline(deadline));
            }
            continue;
        }
        // Any other sense key - stop retrying
        break;
    }
    if (!tur_ok && inq->is_removable)
        return msc_volume_ejected;

    // ---- CAPACITY ----
    if (!msc_read_capacity(vol, msc_sync_deadline(deadline)))
        return inq->is_removable ? msc_volume_ejected : msc_volume_failed;

    // ---- READ BLOCK 0 (non-fatal) ----
    msc_read_block_zero(vol, msc_sync_deadline(deadline));

    // ---- WRITE PROTECTION ----
    msc_read_write_protect(vol, msc_sync_deadline(deadline));

    msc_volume_tur_ok[vol] = true;
    DBG("MSC vol %d: init ok, %lu blocks\n", vol,
        (unsigned long)msc_volume_block_count[vol]);
    return msc_volume_mounted;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_volume_status[vol] == msc_volume_free)
        {
            msc_volume_dev_addr[vol] = dev_addr;
            msc_volume_status[vol] = msc_volume_registered;
            TCHAR volstr[6];
            msc_vol_path(volstr, vol);
            f_mount(&msc_fatfs_volumes[vol], volstr, 0);
            DBG("MSC mount dev_addr %d -> vol %d\n", dev_addr, vol);
            break;
        }
    }
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    // msch_close() never fires the user complete_cb on disconnect,
    // so clear the busy flag to unblock any pending sync wait.
    msc_sync_busy[dev_addr - 1] = false;

    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_volume_dev_addr[vol] == dev_addr &&
            msc_volume_status[vol] != msc_volume_free)
        {
            TCHAR volstr[6];
            msc_vol_path(volstr, vol);
            f_unmount(volstr);
            msc_volume_status[vol] = msc_volume_free;
            msc_volume_dev_addr[vol] = 0;
            msc_volume_block_count[vol] = 0;
            msc_volume_block_size[vol] = 0;
            msc_volume_sense_key[vol] = 0;
            msc_volume_sense_asc[vol] = 0;
            msc_volume_sense_ascq[vol] = 0;
            msc_volume_write_protected[vol] = false;
            msc_volume_tur_ok[vol] = false;
            DBG("MSC unmounted dev_addr %d from vol %d\n", dev_addr, vol);
        }
    }
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
    uint8_t vol = pdrv;
    if (vol >= FF_VOLUMES)
        return STA_NOINIT;
    if (msc_volume_status[vol] == msc_volume_ejected)
        return STA_NOINIT | STA_NODISK;
    if (msc_volume_status[vol] != msc_volume_mounted)
        return STA_NOINIT;
    uint8_t const dev_addr = msc_volume_dev_addr[vol];
    if (dev_addr == 0 || !tuh_msc_mounted(dev_addr))
        return STA_NOINIT;

    return msc_volume_write_protected[vol] ? STA_PROTECT : 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv >= FF_VOLUMES)
        return STA_NOINIT;
    uint8_t vol = pdrv;

    DBG("MSC vol %d: disk_initialize, status=%d\n", vol, msc_volume_status[vol]);

    if (msc_volume_status[vol] == msc_volume_registered ||
        msc_volume_status[vol] == msc_volume_ejected)
    {
        uint8_t dev_addr = msc_volume_dev_addr[vol];
        if (dev_addr == 0 || !tuh_msc_mounted(dev_addr))
            return STA_NOINIT |
                   (msc_volume_status[vol] == msc_volume_ejected ? STA_NODISK : 0);

        msc_volume_status_t result = msc_init_volume(vol);
        if (result != msc_volume_failed)
            msc_volume_status[vol] = result;
    }

    if (msc_volume_status[vol] == msc_volume_ejected)
        return STA_NOINIT | STA_NODISK;

    if (msc_volume_status[vol] != msc_volume_mounted)
        return STA_NOINIT;

    return msc_volume_write_protected[vol] ? STA_PROTECT : 0;
}

// TODO disk_ functions need to precisely return all of
// RES_OK, RES_ERROR, RES_WRPRT, RES_NOTRDY, and RES_PARERR.
// Look deep into fatfs to make sure you understand their details.
// Currently we lack return state from bools in msc_read10_sync and
// msc_write10_sync, and ignore what msc_sync_cache10_sync returns.

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv >= FF_VOLUMES)
        return RES_PARERR;
    uint8_t vol = pdrv;
    uint32_t const block_size = msc_volume_block_size[vol];
    uint8_t const dev_addr = msc_volume_dev_addr[vol];
    DBG("MSC R> %lu+%u\n", (unsigned long)sector, count);
    if (block_size == 0 || dev_addr == 0)
        return RES_ERROR;
    TU_ASSERT(count <= UINT16_MAX, RES_PARERR);

    absolute_time_t deadline = make_timeout_time_ms(MSC_SCSI_RW_TIMEOUT_MS);
    return msc_read10_sync(vol, buff, sector, (uint16_t)count,
                           block_size, msc_sync_deadline(deadline))
               ? RES_OK
               : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv >= FF_VOLUMES)
        return RES_PARERR;
    uint8_t vol = pdrv;
    uint32_t const block_size = msc_volume_block_size[vol];
    uint8_t const dev_addr = msc_volume_dev_addr[vol];
    DBG("MSC W> %lu+%u\n", (unsigned long)sector, count);
    if (block_size == 0 || dev_addr == 0)
        return RES_ERROR;
    TU_ASSERT(count <= UINT16_MAX, RES_PARERR);

    absolute_time_t deadline = make_timeout_time_ms(MSC_SCSI_RW_TIMEOUT_MS);
    return msc_write10_sync(vol, buff, sector, (uint16_t)count,
                            block_size, msc_sync_deadline(deadline))
               ? RES_OK
               : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv >= FF_VOLUMES)
        return RES_PARERR;
    uint8_t vol = pdrv;
    switch (cmd)
    {
    case CTRL_SYNC:
    {
        // SCSI SYNCHRONIZE CACHE (10): flush the device's write cache.
        // Skip for write-protected volumes (no writes to flush) and
        // for disconnected devices (can't send commands).
        if (msc_volume_write_protected[vol])
            return RES_OK;
        uint8_t const dev_addr = msc_volume_dev_addr[vol];
        if (dev_addr == 0 || !tuh_msc_mounted(dev_addr))
            return RES_OK; // device gone, nothing to flush
        if (msc_volume_block_size[vol] == 0)
            return RES_OK;

        // Best effort: ignore errors - many USB flash drives don't
        // implement SYNCHRONIZE CACHE.
        absolute_time_t deadline = make_timeout_time_ms(MSC_SCSI_RW_TIMEOUT_MS);
        msc_sync_cache10_sync(vol, msc_sync_deadline(deadline));
        return RES_OK;
    }
    case GET_SECTOR_COUNT:
        *((DWORD *)buff) = msc_volume_block_count[vol];
        return RES_OK;
    case GET_SECTOR_SIZE:
        *((WORD *)buff) = (WORD)msc_volume_block_size[vol];
        return RES_OK;
    case GET_BLOCK_SIZE:
        *((DWORD *)buff) = 1; // 1 sector
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

int msc_status_count(void)
{
    int count = 0;
    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_volume_status[vol] == msc_volume_registered ||
            msc_volume_status[vol] == msc_volume_mounted ||
            msc_volume_status[vol] == msc_volume_ejected)
            count++;
    }
    return count;
}

int msc_status_response(char *buf, size_t buf_size, int state)
{
    if (state < 0 || state >= FF_VOLUMES)
        return -1;
    uint8_t vol = state;
    if (msc_volume_status[vol] == msc_volume_registered ||
        msc_volume_status[vol] == msc_volume_mounted ||
        msc_volume_status[vol] == msc_volume_ejected)
    {
        if (msc_volume_status[vol] == msc_volume_mounted &&
            msc_inquiry_resp[vol].is_removable &&
            msc_tur_sync(vol, make_timeout_time_ms(MSC_OP_TIMEOUT_MS)) != msc_status_passed &&
            msc_volume_sense_asc[vol] == 0x3A)
        {
            msc_volume_status[vol] = msc_volume_ejected;
            msc_volume_tur_ok[vol] = false;
            msc_volume_block_count[vol] = 0;
            msc_volume_block_size[vol] = 0;
            msc_volume_write_protected[vol] = false;
        }
        disk_initialize(vol);

        char sizebuf[24];
        if (msc_volume_status[vol] != msc_volume_mounted)
        {
            snprintf(sizebuf, sizeof(sizebuf), "%s", STR_PARENS_NO_MEDIA);
        }
        else
        {
            // Floppy-era media (under 5 MB): raw/1024/1000
            // Everything else: pure decimal raw/1000/1000
            const char *xb;
            double size;
            uint64_t raw = (uint64_t)msc_volume_block_count[vol] *
                           (uint64_t)msc_volume_block_size[vol];
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
        snprintf(buf, buf_size, STR_STATUS_MSC,
                 VolumeStr[vol],
                 sizebuf,
                 msc_inquiry_resp[vol].vendor_id,
                 msc_inquiry_resp[vol].product_id,
                 msc_inquiry_resp[vol].product_rev);
    }
    return state + 1;
}

static FIL *msc_validate_fil(int desc)
{
    if (desc < 0 || desc >= MSC_STD_FIL_MAX)
        return NULL;
    if (!msc_std_fil_pool[desc].obj.fs)
        return NULL;
    return &msc_std_fil_pool[desc];
}

bool msc_std_handles(const char *path)
{
    (void)path;
    // MSC/FatFS is the catch-all handler
    return true;
}

int msc_std_open(const char *path, uint8_t flags, api_errno *err)
{
    const uint8_t RDWR = 0x03;
    const uint8_t CREAT = 0x10;
    const uint8_t TRUNC = 0x20;
    const uint8_t APPEND = 0x40;
    const uint8_t EXCL = 0x80;

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
