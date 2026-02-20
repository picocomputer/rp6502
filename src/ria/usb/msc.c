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

// Delay to wait after issuing START UNIT
#define DISK_START_UNIT_DELAY_MS 1000

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
bool tuh_msc_abort_transfers(uint8_t dev_addr);

//--------------------------------------------------------------------+
// Synchronous I/O bookkeeping
//--------------------------------------------------------------------+
// Flag management for sync wrappers.  No spin-waits - the caller
// owns the event-pump loop (main_task() is called from the sync helpers).

static volatile bool _sync_busy[CFG_TUH_DEVICE_MAX];
static uint8_t _sync_csw_status[CFG_TUH_DEVICE_MAX];

static bool _sync_complete_cb(uint8_t dev_addr,
                              tuh_msc_complete_data_t const *cb_data)
{
    _sync_csw_status[dev_addr - 1] = cb_data->csw->status;
    _sync_busy[dev_addr - 1] = false;
    return true;
}

// Arm the sync busy flag and return the internal completion callback.
// Caller must ensure tuh_msc_ready() before calling.
// If the subsequent async submit fails, call tuh_msc_sync_clear_busy().
static tuh_msc_complete_cb_t tuh_msc_sync_begin(uint8_t dev_addr)
{
    // Defensive guard: arming busy on a not-ready device would block
    // the caller's sync-wait loop indefinitely.
    assert(tuh_msc_ready(dev_addr));
    if (!tuh_msc_ready(dev_addr))
        return NULL;
    _sync_csw_status[dev_addr - 1] = MSC_CSW_STATUS_FAILED;
    _sync_busy[dev_addr - 1] = true;
    return _sync_complete_cb;
}

// Clear the busy flag - call from tuh_msc_umount_cb to unblock any
// pending sync wait on disconnect, or after a failed async submit
// following tuh_msc_sync_begin().
static void tuh_msc_sync_clear_busy(uint8_t dev_addr)
{
    _sync_busy[dev_addr - 1] = false;
}

//--------------------------------------------------------------------+
// Synchronous I/O helpers (spin-wait lives here)
//--------------------------------------------------------------------+

// Track whether the last msc_sync_wait_io call timed out.
static bool msc_sync_timed_out;

// Return deadline or now+MSC_OP_TIMEOUT_MS, whichever is later.
// Prevents a nearly-expired init deadline from giving a command
// too little time to complete (especially over CBI transport).
static inline absolute_time_t stage_deadline(absolute_time_t deadline)
{
    absolute_time_t floor = make_timeout_time_ms(MSC_OP_TIMEOUT_MS);
    return absolute_time_diff_us(deadline, floor) > 0 ? floor : deadline;
}

// Wait ready + arm busy flag → return callback (NULL on deadline).
static tuh_msc_complete_cb_t msc_sync_begin(uint8_t dev_addr,
                                            absolute_time_t deadline)
{
    while (!tuh_msc_ready(dev_addr))
    {
        if (!tuh_msc_mounted(dev_addr))
            return NULL;
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
            return NULL;
        main_task();
    }
    return tuh_msc_sync_begin(dev_addr);
}

// Wait for in-flight command to complete.
static uint8_t msc_sync_wait_io(uint8_t dev_addr, absolute_time_t deadline)
{
    msc_sync_timed_out = false;
    while (_sync_busy[dev_addr - 1])
    {
        if (!tuh_msc_mounted(dev_addr))
        {
            tuh_msc_sync_clear_busy(dev_addr);
            msc_sync_timed_out = true;
            return MSC_CSW_STATUS_FAILED;
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
            tuh_msc_sync_clear_busy(dev_addr);
            msc_sync_timed_out = true;
            return MSC_CSW_STATUS_FAILED;
        }
        main_task();
    }
    return _sync_csw_status[dev_addr - 1];
}

// Perform reset recovery with blocking wait.
static void msc_sync_recovery(uint8_t dev_addr, absolute_time_t deadline)
{
    tuh_msc_reset_recovery(dev_addr);
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

// Core submit-and-wait helper.
static uint8_t msc_scsi_sync(uint8_t dev_addr, msc_cbw_t *cbw,
                             void *data, absolute_time_t deadline)
{
    tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, deadline);
    if (!cb)
        return MSC_CSW_STATUS_FAILED;
    if (!tuh_msc_scsi_command(dev_addr, cbw, data, cb, 0))
    {
        tuh_msc_sync_clear_busy(dev_addr);
        return MSC_CSW_STATUS_FAILED;
    }
    return msc_sync_wait_io(dev_addr, deadline);
}

static uint8_t msc_inquiry_sync(uint8_t dev_addr, scsi_inquiry_resp_t *resp,
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
    return msc_scsi_sync(dev_addr, &cbw, resp, deadline);
}

static uint8_t msc_tur_sync(uint8_t dev_addr, absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = 0;
    cbw.dir = TUSB_DIR_OUT;
    cbw.cmd_len = sizeof(scsi_test_unit_ready_t);
    cbw.command[0] = SCSI_CMD_TEST_UNIT_READY;
    return msc_scsi_sync(dev_addr, &cbw, NULL, deadline);
}

static uint8_t msc_request_sense_sync(uint8_t dev_addr,
                                      scsi_sense_fixed_resp_t *resp,
                                      absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = sizeof(scsi_sense_fixed_resp_t);
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = sizeof(scsi_request_sense_t);
    scsi_request_sense_t const cmd = {
        .cmd_code = SCSI_CMD_REQUEST_SENSE,
        .alloc_length = sizeof(scsi_sense_fixed_resp_t)};
    memcpy(cbw.command, &cmd, sizeof(cmd));
    return msc_scsi_sync(dev_addr, &cbw, resp, deadline);
}

static uint8_t msc_start_stop_unit_sync(uint8_t dev_addr, bool start,
                                        absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = 0;
    cbw.dir = TUSB_DIR_OUT;
    cbw.cmd_len = 6;
    cbw.command[0] = 0x1B; // START STOP UNIT
    cbw.command[4] = start ? 0x01 : 0x00;
    return msc_scsi_sync(dev_addr, &cbw, NULL, deadline);
}

static uint8_t msc_read_capacity10_sync(uint8_t dev_addr,
                                        scsi_read_capacity10_resp_t *resp,
                                        absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = sizeof(scsi_read_capacity10_resp_t);
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = sizeof(scsi_read_capacity10_t);
    cbw.command[0] = SCSI_CMD_READ_CAPACITY_10;
    return msc_scsi_sync(dev_addr, &cbw, resp, deadline);
}

static uint8_t msc_read_format_capacities_sync(uint8_t dev_addr, void *resp,
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
    return msc_scsi_sync(dev_addr, &cbw, resp, deadline);
}

static uint8_t msc_mode_sense6_sync(uint8_t dev_addr, uint8_t page_code,
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
    return msc_scsi_sync(dev_addr, &cbw, resp, deadline);
}

static uint8_t msc_read10_sync(uint8_t dev_addr, void *buff,
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
    return msc_scsi_sync(dev_addr, &cbw, buff, deadline);
}

static uint8_t msc_write10_sync(uint8_t dev_addr, const void *buff,
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
    return msc_scsi_sync(dev_addr, &cbw, (void *)(uintptr_t)buff, deadline);
}

static uint8_t msc_sync_cache10_sync(uint8_t dev_addr,
                                     absolute_time_t deadline)
{
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = 0;
    cbw.dir = TUSB_DIR_OUT;
    cbw.cmd_len = 10;
    cbw.command[0] = 0x35; // SYNCHRONIZE CACHE (10)
    return msc_scsi_sync(dev_addr, &cbw, NULL, deadline);
}

// Issue REQUEST SENSE and store result in per-volume sense arrays.
static bool msc_do_request_sense(uint8_t vol, absolute_time_t deadline)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    if (dev_addr == 0)
        return false; // device disconnected

    scsi_sense_fixed_resp_t sense_resp;
    memset(&sense_resp, 0, sizeof(sense_resp));
    msc_request_sense_sync(dev_addr, &sense_resp, deadline);

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
    DBG("MSC vol %d: sense %d/%02Xh/%02Xh\n",
        vol, msc_volume_sense_key[vol],
        msc_volume_sense_asc[vol], msc_volume_sense_ascq[vol]);
    return true;
}

// Send a single TUR command and return whether it passed.
static bool msc_send_tur(uint8_t vol, absolute_time_t deadline)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    if (dev_addr == 0)
        return false; // device disconnected
    return msc_tur_sync(dev_addr, deadline) == MSC_CSW_STATUS_PASSED;
}

// Mark a removable volume as ejected and clear cached geometry.
static void msc_handle_io_error(uint8_t vol)
{
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

//--------------------------------------------------------------------+
// Shared SCSI initialization helpers
//--------------------------------------------------------------------+
// Building blocks of msc_init_volume().  Each function performs one
// SCSI operation synchronously using the sync helpers.  All use an
// absolute_time_t deadline to coordinate timeouts across the full
// init sequence.

static bool msc_spinup(uint8_t vol, absolute_time_t deadline)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];

    for (int retry = 0; retry < 4; retry++)
    {
        if (!tuh_msc_mounted(dev_addr))
            return false;

        if (msc_send_tur(vol, stage_deadline(deadline)))
        {
            DBG("MSC vol %d: spinup ok (retry %d)\n", vol, retry);
            return true;
        }

        msc_do_request_sense(vol, stage_deadline(deadline));
        uint8_t sk = msc_volume_sense_key[vol];
        uint8_t asc = msc_volume_sense_asc[vol];
        uint8_t ascq = msc_volume_sense_ascq[vol];

        if (sk == SCSI_SENSE_UNIT_ATTENTION)
        {
            // UA cleared by REQUEST SENSE; retry TUR immediately.
            DBG("MSC vol %d: UA %02Xh/%02Xh cleared\n", vol, asc, ascq);
            continue;
        }

        if (sk == SCSI_SENSE_NOT_READY)
        {
            if (asc == 0x3A)
            {
                DBG("MSC vol %d: medium not present\n", vol);
                if (tuh_msc_is_cbi(dev_addr))
                    msc_sync_recovery(dev_addr, stage_deadline(deadline));
                return false;
            }

            // Not ready (becoming ready, motor starting, etc.).
            // Send START STOP UNIT (start=1) to trigger detection.
            DBG("MSC vol %d: not ready %02Xh/%02Xh, START UNIT\n",
                vol, asc, ascq);
            msc_start_stop_unit_sync(dev_addr, true, stage_deadline(deadline));
            // Always delay DISK_START_UNIT_DELAY_MS no matter what.
            {
                absolute_time_t wake = make_timeout_time_ms(DISK_START_UNIT_DELAY_MS);
                while (absolute_time_diff_us(get_absolute_time(), wake) > 0)
                {
                    if (!tuh_msc_mounted(dev_addr))
                        return false;
                    main_task();
                }
            }
            continue;
        }

        // Unexpected sense key - stop.
        DBG("MSC vol %d: spinup sense %d/%02Xh/%02Xh, giving up\n",
            vol, sk, asc, ascq);
        break;
    }

    return false;
}

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
        if (msc_read_format_capacities_sync(dev_addr, rfc, sizeof(rfc),
                                            stage_deadline(deadline)) !=
            MSC_CSW_STATUS_PASSED)
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
        if (msc_read_capacity10_sync(dev_addr, &cap10,
                                     stage_deadline(deadline)) !=
            MSC_CSW_STATUS_PASSED)
        {
            // For removable: check if medium not present.
            if (msc_inquiry_resp[vol].is_removable)
            {
                msc_do_request_sense(vol, stage_deadline(deadline));
                if (msc_volume_sense_asc[vol] == 0x3A)
                    DBG("MSC vol %d: capacity - medium not present\n", vol);
            }
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

    uint8_t block0[512];
    msc_read10_sync(dev_addr, block0, 0, 1, bs, stage_deadline(deadline));
}

// Determine write protection via MODE SENSE(6).  BOT only; CBI
// devices skip this (assumed not protected).  Non-fatal: defaults
// to not protected on failure.
//
// Per SPC/SBC, try page 0x3F first (all pages).  If the device
// rejects it, fall back to page 0.  Write protection is indicated
// by the WP bit in the device-specific parameter of the mode
// parameter header.
static void msc_read_write_protect(uint8_t vol, absolute_time_t deadline)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    msc_volume_write_protected[vol] = false;

    if (tuh_msc_is_cbi(dev_addr))
        return; // CBI: skip MODE SENSE

    if (!tuh_msc_mounted(dev_addr))
        return;

    // Try MODE SENSE(6) page 0x3F (all pages)
    scsi_mode_sense6_resp_t ms;
    memset(&ms, 0, sizeof(ms));
    if (msc_mode_sense6_sync(dev_addr, 0x3F, &ms,
                             stage_deadline(deadline)) ==
        MSC_CSW_STATUS_PASSED)
    {
        DBG("MSC vol %d: MODE SENSE WP=%d\n", vol, ms.write_protected);
        msc_volume_write_protected[vol] = ms.write_protected;
        return;
    }

    // Page 0x3F rejected.  Fall back to page 0.
    DBG("MSC vol %d: MODE SENSE page 0x3F rejected, trying page 0\n", vol);

    memset(&ms, 0, sizeof(ms));
    if (msc_mode_sense6_sync(dev_addr, 0x00, &ms,
                             stage_deadline(deadline)) ==
        MSC_CSW_STATUS_PASSED)
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

static msc_volume_status_t msc_init_volume(uint8_t vol)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    scsi_inquiry_resp_t *inq = &msc_inquiry_resp[vol];
    bool first_mount = (msc_volume_status[vol] == msc_volume_registered);
    absolute_time_t deadline = make_timeout_time_ms(MSC_INIT_TIMEOUT_MS);

    // ---- INQUIRY (first mount only) ----
    if (first_mount)
    {
        bool inquiry_ok = false;
        if (!tuh_msc_mounted(dev_addr))
            return msc_volume_failed;
        memset(inq, 0, sizeof(*inq));
        uint8_t csw = msc_inquiry_sync(dev_addr, inq, stage_deadline(deadline));
        msc_inquiry_rtrims(inq->vendor_id, 8);
        msc_inquiry_rtrims(inq->product_id, 16);
        msc_inquiry_rtrims(inq->product_rev, 4);

        if (csw == MSC_CSW_STATUS_PASSED)
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

    // ---- CLEAR UNIT ATTENTION ----
    if (inq->is_removable)
    {
        if (!msc_spinup(vol, deadline))
            return msc_volume_ejected;
    }
    else
    {
        if (!msc_send_tur(vol, stage_deadline(deadline)))
            msc_do_request_sense(vol, stage_deadline(deadline));
    }

    // ---- CAPACITY ----
    if (!msc_read_capacity(vol, deadline))
        return inq->is_removable ? msc_volume_ejected : msc_volume_failed;

    // ---- READ BLOCK 0 (non-fatal) ----
    msc_read_block_zero(vol, deadline);

    // ---- WRITE PROTECTION ----
    msc_read_write_protect(vol, deadline);

    msc_volume_tur_ok[vol] = true;
    DBG("MSC vol %d: init ok, %lu blocks\n", vol,
        (unsigned long)msc_volume_block_count[vol]);
    return msc_volume_mounted;
}

static FIL *msc_validate_fil(int desc)
{
    if (desc < 0 || desc >= MSC_STD_FIL_MAX)
        return NULL;
    if (!msc_std_fil_pool[desc].obj.fs)
        return NULL;
    return &msc_std_fil_pool[desc];
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
    if (msc_volume_status[state] == msc_volume_registered ||
        msc_volume_status[state] == msc_volume_mounted ||
        msc_volume_status[state] == msc_volume_ejected)
    {
        if (msc_volume_status[state] == msc_volume_mounted &&
            msc_inquiry_resp[state].is_removable &&
            !msc_send_tur(state, make_timeout_time_ms(MSC_OP_TIMEOUT_MS)))
            msc_handle_io_error(state);
        disk_initialize(state);

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
    // Clear the busy flag first - msch_close() never fires the
    // user complete_cb on disconnect, so any pending
    // sync wait would spin forever without this.
    tuh_msc_sync_clear_busy(dev_addr);

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
            msc_volume_sense_key[vol] = 0;
            msc_volume_sense_asc[vol] = 0;
            msc_volume_sense_ascq[vol] = 0;
            msc_volume_write_protected[vol] = false;
            msc_volume_tur_ok[vol] = false;
            DBG("MSC unmounted dev_addr %d from vol %d\n", dev_addr, vol);
        }
    }
}

// Handle SCSI transfer failure: perform BOT reset recovery if appropriate
// and mark removable volume as ejected.
static void msc_xfer_error(uint8_t pdrv)
{
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
    DBG("MSC xfer fail: timed_out=%d, cbi=%d\n",
        msc_sync_timed_out, tuh_msc_is_cbi(dev_addr));
    if (dev_addr && !msc_sync_timed_out && !tuh_msc_is_cbi(dev_addr))
        msc_sync_recovery(dev_addr, make_timeout_time_ms(MSC_OP_TIMEOUT_MS));
    msc_handle_io_error(pdrv);
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
    if (pdrv >= FF_VOLUMES)
        return STA_NOINIT;
    if (msc_volume_status[pdrv] == msc_volume_ejected)
        return STA_NOINIT | STA_NODISK;
    if (msc_volume_status[pdrv] != msc_volume_mounted)
        return STA_NOINIT;
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
    if (dev_addr == 0 || !tuh_msc_mounted(dev_addr))
        return STA_NOINIT;

    DSTATUS res = 0;
    if (msc_volume_write_protected[pdrv])
        res |= STA_PROTECT;

    return res;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv >= FF_VOLUMES)
        return STA_NOINIT;

    DBG("MSC vol %d: disk_initialize, status=%d\n", pdrv, msc_volume_status[pdrv]);

    if (msc_volume_status[pdrv] == msc_volume_registered ||
        msc_volume_status[pdrv] == msc_volume_ejected)
    {
        uint8_t dev_addr = msc_volume_dev_addr[pdrv];
        if (dev_addr == 0 || !tuh_msc_mounted(dev_addr))
            return STA_NOINIT |
                   (msc_volume_status[pdrv] == msc_volume_ejected ? STA_NODISK : 0);

        msc_volume_status_t result = msc_init_volume(pdrv);
        if (result != msc_volume_failed)
            msc_volume_status[pdrv] = result;
    }

    if (msc_volume_status[pdrv] == msc_volume_ejected)
        return STA_NOINIT | STA_NODISK;

    if (msc_volume_status[pdrv] != msc_volume_mounted)
        return STA_NOINIT;

    DSTATUS res = 0;
    if (msc_volume_write_protected[pdrv])
        res |= STA_PROTECT;
    return res;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv >= FF_VOLUMES)
        return RES_PARERR;
    uint32_t const block_size = msc_volume_block_size[pdrv];
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
    DBG("MSC R> %lu+%u\n", (unsigned long)sector, count);
    if (block_size == 0 || dev_addr == 0)
        return RES_ERROR;
    TU_ASSERT(count <= UINT16_MAX, RES_PARERR);

    absolute_time_t deadline = make_timeout_time_ms(MSC_SCSI_RW_TIMEOUT_MS);
    uint8_t status = msc_read10_sync(dev_addr, buff, sector, (uint16_t)count,
                                     block_size, stage_deadline(deadline));
    if (status != MSC_CSW_STATUS_PASSED)
    {
        msc_xfer_error(pdrv);
        return RES_ERROR;
    }
    if (sector == 0)
    {
        DBG("MSC vol %d: sector 0 dump:", pdrv);
        for (int i = 0; i < 64; i++)
        {
            if (i % 16 == 0)
                DBG("\n  %04X: ", i);
            DBG(" %02X", buff[i]);
        }
        DBG("\n  ...\n  01F0: ");
        for (int i = 0x1F0; i < 0x200; i++)
            DBG(" %02X", buff[i]);
        DBG("\n");
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv >= FF_VOLUMES)
        return RES_PARERR;
    uint32_t const block_size = msc_volume_block_size[pdrv];
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
    DBG("MSC W> %lu+%u\n", (unsigned long)sector, count);
    if (block_size == 0 || dev_addr == 0)
        return RES_ERROR;
    TU_ASSERT(count <= UINT16_MAX, RES_PARERR);

    absolute_time_t deadline = make_timeout_time_ms(MSC_SCSI_RW_TIMEOUT_MS);
    uint8_t status = msc_write10_sync(dev_addr, buff, sector, (uint16_t)count,
                                      block_size, stage_deadline(deadline));
    if (status != MSC_CSW_STATUS_PASSED)
    {
        msc_xfer_error(pdrv);
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv >= FF_VOLUMES)
        return RES_PARERR;
    switch (cmd)
    {
    case CTRL_SYNC:
    {
        // SCSI SYNCHRONIZE CACHE (10): flush the device's write cache.
        // Skip for write-protected volumes (no writes to flush) and
        // for disconnected devices (can't send commands).
        if (msc_volume_write_protected[pdrv])
            return RES_OK;
        uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
        if (dev_addr == 0 || !tuh_msc_mounted(dev_addr))
            return RES_OK; // device gone, nothing to flush
        if (msc_volume_block_size[pdrv] == 0)
            return RES_OK;

        // Best effort: ignore errors — many USB flash drives don't
        // implement SYNCHRONIZE CACHE.
        absolute_time_t deadline = make_timeout_time_ms(MSC_SCSI_RW_TIMEOUT_MS);
        msc_sync_cache10_sync(dev_addr, stage_deadline(deadline));
        return RES_OK;
    }
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
