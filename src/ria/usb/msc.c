/*
 * Copyright (c) 2026 Rumbledethumps
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

// Time budget for any single USB MSC operation: one SCSI command or
// one reset recovery sequence (BOT: BMR + 2x CLEAR_HALT; CBI: class
// reset).  Also the floor given to each init stage so a nearly-expired
// overall deadline cannot starve an individual command.
#define MSC_OP_TIMEOUT_MS 500

// disk_status() issues a TUR on removable volumes only when this many
// milliseconds have elapsed since the last successful SCSI command.
// This detects media removal without adding overhead during active I/O.
#define MSC_DISK_STATUS_TIMEOUT_MS 100

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
static_assert(FF_VOLUMES == 10);
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
static const char __in_flash("fatfs_vol") VolumeStrMSC8[] = "MSC8";
static const char __in_flash("fatfs_vol") VolumeStrMSC9[] = "MSC9";
const char __in_flash("fatfs_vols") * VolumeStr[FF_VOLUMES] = {
    VolumeStrMSC0, VolumeStrMSC1, VolumeStrMSC2, VolumeStrMSC3,
    VolumeStrMSC4, VolumeStrMSC5, VolumeStrMSC6, VolumeStrMSC7,
    VolumeStrMSC8, VolumeStrMSC9};

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
static uint8_t msc_volume_lun[FF_VOLUMES];
static FATFS msc_fatfs_volumes[FF_VOLUMES];
static bool msc_volume_is_removable[FF_VOLUMES];
static uint32_t msc_volume_block_count[FF_VOLUMES];
static uint32_t msc_volume_block_size[FF_VOLUMES];
static uint8_t msc_volume_sense_key[FF_VOLUMES];
static uint8_t msc_volume_sense_asc[FF_VOLUMES];
static uint8_t msc_volume_sense_ascq[FF_VOLUMES];
static bool msc_volume_write_protected[FF_VOLUMES];
static absolute_time_t msc_volume_last_success[FF_VOLUMES];

// This driver requires our custom TinyUSB: src/ria/usb/msc_host.c.
// It will not work with upstream: src/tinyusb/src/class/msc/msc_host.c
// These additional interfaces are not in upstream TinyUSB.

// Superset of msc_csw_status_t (defined in msc_host.c) with a timeout value.
typedef enum
{
    MSC_STATUS_PASSED,      // == MSC_CSW_STATUS_PASSED
    MSC_STATUS_FAILED,      // == MSC_CSW_STATUS_FAILED
    MSC_STATUS_PHASE_ERROR, // == MSC_CSW_STATUS_PHASE_ERROR
    MSC_STATUS_TIMED_OUT,   // not a CSW status; returned on I/O timeout
} msc_status_t;

uint8_t tuh_msc_protocol(uint8_t dev_addr);
msc_status_t tuh_msc_scsi_sync(uint8_t dev_addr, msc_cbw_t *cbw,
                               const void *data, uint32_t timeout_ms);

// Override of the weak tuh_msc_pump() default in msc_host.c.
// Pumps USB events and all application tasks during blocking I/O.
// FatFs rentry would be a problem so main_task() does not call FatFs
// but it does call the required tuh_task().
void tuh_msc_pump(void) { main_task(); }

// Convert absolute deadline to remaining milliseconds, floored at MSC_OP_TIMEOUT_MS.
// Prevents a nearly-expired deadline from starving a subordinate command.
static inline uint32_t msc_floor_ms(absolute_time_t deadline)
{
    int64_t diff_us = absolute_time_diff_us(get_absolute_time(), deadline);
    uint32_t remaining = diff_us > 0 ? (uint32_t)(diff_us / 1000) : 0;
    return remaining > MSC_OP_TIMEOUT_MS ? remaining : MSC_OP_TIMEOUT_MS;
}

//--------------------------------------------------------------------+
// Synchronous SCSI command wrappers
//--------------------------------------------------------------------+
// Each wrapper builds a CDB, submits it via the transport API,
// and blocks until completion.  Returns the CSW status code.

// Initialize a CBW for a volume's LUN.
// Signature and tag are stamped by tuh_msc_scsi_submit().
static inline void msc_cbw_init(msc_cbw_t *cbw, uint8_t vol,
                                uint32_t total_bytes, uint8_t dir,
                                uint8_t cmd_len, const void *cmd)
{
    memset(cbw, 0, sizeof(msc_cbw_t));
    cbw->lun = msc_volume_lun[vol];
    cbw->total_bytes = total_bytes;
    cbw->dir = dir;
    cbw->cmd_len = cmd_len;
    memcpy(cbw->command, cmd, cmd_len);
}

// Core submit-and-wait helper with autosense.
// - BOT/CBI: on non-PASSED status, issue REQUEST SENSE and cache sense data.
// - CB (CBI no interrupt): issue REQUEST SENSE after every command to
//   determine command outcome because transport status is unavailable.
// Callers never need to explicitly issue REQUEST SENSE.
static msc_status_t msc_scsi_sync(uint8_t vol, msc_cbw_t *cbw,
                                  const void *data, uint32_t timeout_ms)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    if (dev_addr == 0)
        return MSC_STATUS_FAILED;

    msc_status_t status = tuh_msc_scsi_sync(dev_addr, cbw, data, timeout_ms);
    bool cb_no_interrupt = (tuh_msc_protocol(dev_addr) == MSC_PROTOCOL_CBI_NO_INTERRUPT);

    if (status == MSC_STATUS_PHASE_ERROR)
    {
        msc_volume_sense_key[vol] = SCSI_SENSE_NONE;
        msc_volume_sense_asc[vol] = 0;
        msc_volume_sense_ascq[vol] = 0;
    }
    else if (status != MSC_STATUS_TIMED_OUT)
    {
        // CB (no interrupt endpoint) has no transport status phase.
        // Determine command outcome via REQUEST SENSE for every command.
        bool need_autosense = (status != MSC_STATUS_PASSED) || cb_no_interrupt;
        if (!need_autosense)
        {
            msc_volume_last_success[vol] = get_absolute_time();
            return status;
        }

        scsi_sense_fixed_resp_t sense_resp;
        memset(&sense_resp, 0, sizeof(sense_resp));
        scsi_request_sense_t const sense_cmd = {
            .cmd_code = SCSI_CMD_REQUEST_SENSE,
            .alloc_length = sizeof(scsi_sense_fixed_resp_t)};
        msc_cbw_t sense_cbw;
        msc_cbw_init(&sense_cbw, vol, sizeof(scsi_sense_fixed_resp_t), TUSB_DIR_IN_MASK,
                     sizeof(sense_cmd), &sense_cmd);
        msc_status_t sense_status = tuh_msc_scsi_sync(
            dev_addr, &sense_cbw, &sense_resp, MSC_OP_TIMEOUT_MS);
        // On CBI with an interrupt endpoint, the interrupt status can reflect
        // prior command state rather than REQUEST SENSE completion. The sense
        // payload is authoritative when it is present and the transfer did not
        // time out.
        bool sense_data_valid = (sense_status == MSC_STATUS_PASSED) ||
                                (sense_status != MSC_STATUS_TIMED_OUT &&
                                 (tuh_msc_protocol(dev_addr) != MSC_PROTOCOL_BOT ||
                                  sense_resp.response_code != 0));
        if (sense_data_valid && sense_resp.response_code)
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

        if (cb_no_interrupt)
        {
            if (!sense_data_valid || sense_status == MSC_STATUS_TIMED_OUT)
            {
                status = MSC_STATUS_TIMED_OUT;
            }
            else if (msc_volume_sense_key[vol] == SCSI_SENSE_NONE ||
                     msc_volume_sense_key[vol] == SCSI_SENSE_RECOVERED_ERROR)
            {
                status = MSC_STATUS_PASSED;
                msc_volume_last_success[vol] = get_absolute_time();
            }
            else
            {
                status = MSC_STATUS_FAILED;
            }
        }
    }

    return status;
}

static msc_status_t msc_scsi_inquiry(uint8_t vol, uint32_t timeout_ms,
                                     scsi_inquiry_resp_t *resp)
{
    scsi_inquiry_t const cmd = {
        .cmd_code = SCSI_CMD_INQUIRY,
        .alloc_length = sizeof(scsi_inquiry_resp_t)};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_inquiry_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    return msc_scsi_sync(vol, &cbw, resp, timeout_ms);
}

static msc_status_t msc_scsi_test_unit_ready(uint8_t vol, uint32_t timeout_ms)
{
    scsi_test_unit_ready_t const cmd = {.cmd_code = SCSI_CMD_TEST_UNIT_READY};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, 0, TUSB_DIR_OUT, sizeof(cmd), &cmd);
    return msc_scsi_sync(vol, &cbw, NULL, timeout_ms);
}

static msc_status_t msc_scsi_read_capacity10(uint8_t vol, uint32_t timeout_ms,
                                             scsi_read_capacity10_resp_t *resp)
{
    scsi_read_capacity10_t const cmd = {.cmd_code = SCSI_CMD_READ_CAPACITY_10};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_read_capacity10_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    return msc_scsi_sync(vol, &cbw, resp, timeout_ms);
}

static msc_status_t msc_scsi_read_format_capacities(uint8_t vol, uint32_t timeout_ms,
                                                    void *resp, uint8_t alloc_length)
{
    scsi_read_format_capacity_t const cmd = {
        .cmd_code = SCSI_CMD_READ_FORMAT_CAPACITY,
        .alloc_length = tu_htons(alloc_length)};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, alloc_length, TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    return msc_scsi_sync(vol, &cbw, resp, timeout_ms);
}

static msc_status_t msc_scsi_mode_sense6(uint8_t vol, uint32_t timeout_ms,
                                         uint8_t page_code, scsi_mode_sense6_resp_t *resp)
{
    scsi_mode_sense6_t const cmd = {
        .cmd_code = SCSI_CMD_MODE_SENSE_6,
        .disable_block_descriptor = 1,
        .page_code = page_code,
        .alloc_length = sizeof(scsi_mode_sense6_resp_t),
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_mode_sense6_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    return msc_scsi_sync(vol, &cbw, resp, timeout_ms);
}

static msc_status_t msc_scsi_sync_cache10(uint8_t vol, uint32_t timeout_ms)
{
    uint8_t cmd[10] = {0x35}; // SYNCHRONIZE CACHE (10)
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, 0, TUSB_DIR_OUT, 10, cmd);
    return msc_scsi_sync(vol, &cbw, NULL, timeout_ms);
}

static msc_status_t msc_scsi_start_stop_unit(uint8_t vol, uint32_t timeout_ms,
                                             bool start, bool load_eject)
{
    scsi_start_stop_unit_t const cmd = {
        .cmd_code = SCSI_CMD_START_STOP_UNIT,
        .start = start ? 1u : 0u,
        .load_eject = load_eject ? 1u : 0u,
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, 0, TUSB_DIR_OUT, sizeof(cmd), &cmd);
    return msc_scsi_sync(vol, &cbw, NULL, timeout_ms);
}

//--------------------------------------------------------------------+
// SCSI read and write commands with automatic error recovery
//--------------------------------------------------------------------+

// Mark a removable volume as ejected and clear cached geometry.
static void msc_handle_io_error(uint8_t vol)
{
    DBG("MSC vol %d: msc_handle_io_error\n", vol);
    if (!msc_volume_is_removable[vol])
        return;
    if (msc_volume_status[vol] == msc_volume_free ||
        msc_volume_status[vol] == msc_volume_ejected)
        return;
    msc_volume_status[vol] = msc_volume_ejected;
    msc_volume_block_count[vol] = 0;
    msc_volume_block_size[vol] = 0;
    msc_volume_write_protected[vol] = false;
    DBG("MSC vol %d: media ejected\n", vol);
}

static msc_status_t msc_scsi_read10(uint8_t vol, uint32_t timeout_ms,
                                    void *buff, uint32_t lba, uint16_t block_count,
                                    uint32_t block_size)
{
    scsi_read10_t const cmd = {
        .cmd_code = SCSI_CMD_READ_10,
        .lba = tu_htonl(lba),
        .block_count = tu_htons(block_count)};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, block_count * block_size, TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_sync(vol, &cbw, buff, timeout_ms);
    if (status != MSC_STATUS_PASSED)
        msc_handle_io_error(vol);
    return status;
}

static msc_status_t msc_scsi_write10(uint8_t vol, uint32_t timeout_ms,
                                     const void *buff, uint32_t lba, uint16_t block_count,
                                     uint32_t block_size)
{
    scsi_write10_t const cmd = {
        .cmd_code = SCSI_CMD_WRITE_10,
        .lba = tu_htonl(lba),
        .block_count = tu_htons(block_count)};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, block_count * block_size, TUSB_DIR_OUT, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_sync(vol, &cbw, buff, timeout_ms);
    if (status != MSC_STATUS_PASSED)
        msc_handle_io_error(vol);
    return status;
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
static bool msc_read_capacity(uint8_t vol, uint32_t timeout_ms)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];

    if (!tuh_msc_mounted(dev_addr))
        return false;

    if (tuh_msc_protocol(dev_addr) != MSC_PROTOCOL_BOT)
    {
        // CBI: READ FORMAT CAPACITIES
        uint8_t rfc[12];
        memset(rfc, 0, sizeof(rfc));
        if (msc_scsi_read_format_capacities(vol, timeout_ms,
                                            rfc, sizeof(rfc)) !=
            MSC_STATUS_PASSED)
            return false;

        DBG("MSC vol %d: RFC list_len=%d [%02X %02X %02X %02X %02X %02X %02X %02X ...]\n",
            vol, rfc[3], rfc[4], rfc[5], rfc[6], rfc[7],
            rfc[8], rfc[9], rfc[10], rfc[11]);

        scsi_read_format_capacity_data_t const *cap =
            (scsi_read_format_capacity_data_t const *)rfc;
        if (cap->list_length < 8 || (cap->list_length % 8) != 0)
            return false;
        uint8_t desc_type = cap->descriptor_type & 0x03;
        if (desc_type == 3) // 0x03 = No Media Present
            return false;
        uint32_t blocks = tu_ntohl(cap->block_num);
        uint32_t bsize = ((uint32_t)cap->reserved2 << 16) | tu_ntohs(cap->block_size_u16);
        DBG("MSC vol %d: RFC %lu blocks, %lu bytes/block, type %d\n",
            vol, (unsigned long)blocks, (unsigned long)bsize, desc_type);
        // Accept power-of-2 sector sizes that FatFS supports.
        if (blocks == 0 || bsize == 0 ||
            (bsize & (bsize - 1)) != 0 || bsize > 4096)
            return false;
        msc_volume_block_count[vol] = blocks;
        msc_volume_block_size[vol] = bsize;
    }
    else
    {
        // BOT: READ CAPACITY(10)
        scsi_read_capacity10_resp_t cap10;
        memset(&cap10, 0, sizeof(cap10));
        msc_status_t cap_status = msc_scsi_read_capacity10(vol, timeout_ms, &cap10);
        if (cap_status != MSC_STATUS_PASSED)
        {
            // Autosense already populated sense data.
            if (msc_volume_is_removable[vol] &&
                msc_volume_sense_asc[vol] == 0x3A)
                DBG("MSC vol %d: capacity - medium not present\n", vol);
            return false;
        }
        uint32_t last_lba = tu_ntohl(cap10.last_lba);
        if (last_lba == 0xFFFFFFFF)
            return false; // > 2 TB, unsupported
        if (last_lba == 0)
            DBG("MSC vol %d: READ CAPACITY(10) returned last_lba=0\n", vol);
        uint32_t bsize = tu_ntohl(cap10.block_size);
        if (bsize == 0 ||
            (bsize & (bsize - 1)) != 0 ||
            bsize > 4096)
            return false;
        msc_volume_block_count[vol] = last_lba + 1;
        msc_volume_block_size[vol] = bsize;
    }

    return true;
}

// Determine write protection via MODE SENSE(6).  BOT only; CBI
// devices skip this (assumed not protected).  Non-fatal: defaults
// to not protected on failure.
static void msc_read_write_protect(uint8_t vol, uint32_t timeout_ms)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    msc_volume_write_protected[vol] = false;

    if (tuh_msc_protocol(dev_addr) != MSC_PROTOCOL_BOT)
        return;

    scsi_mode_sense6_resp_t ms;
    memset(&ms, 0, sizeof(ms));
    if (msc_scsi_mode_sense6(vol, timeout_ms, 0x3F, &ms) == MSC_STATUS_PASSED)
    {
        DBG("MSC vol %d: MODE SENSE WP=%d\n", vol, ms.write_protected);
        msc_volume_write_protected[vol] = ms.write_protected;
        return;
    }
}

//--------------------------------------------------------------------+
// Synchronous volume initialization
//--------------------------------------------------------------------+
// Called from disk_initialize() for both newly registered and ejected
// volumes.  Performs the full SCSI init sequence synchronously.

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
    absolute_time_t deadline = make_timeout_time_ms(MSC_INIT_TIMEOUT_MS);

    // ---- INQUIRY (first mount only) ----
    if (msc_volume_status[vol] == msc_volume_registered)
    {
        bool inquiry_ok = false;
        if (!tuh_msc_mounted(dev_addr))
            return msc_volume_failed;
        scsi_inquiry_resp_t inq;
        memset(&inq, 0, sizeof(inq));
        msc_status_t csw = msc_scsi_inquiry(vol, msc_floor_ms(deadline), &inq);

        if (csw == MSC_STATUS_PASSED)
        {
            inquiry_ok = true;
        }
        // Accept usable response data despite CSW failure (common on CBI)
        else if (inq.peripheral_qualifier != 3 &&
                 (inq.response_data_format == 1 ||
                  inq.response_data_format == 2) &&
                 inq.additional_length >= 31)
        {
            DBG("MSC vol %d: INQUIRY CSW failed but response valid\n", vol);
            inquiry_ok = true;
        }
        if (!inquiry_ok)
        {
            DBG("MSC vol %d: INQUIRY failed\n", vol);
            return msc_volume_failed;
        }
        msc_volume_is_removable[vol] = inq.is_removable;
        DBG("MSC vol %d: %s%s\n", vol,
            inq.vendor_id, inq.is_removable ? " (removable)" : "");
    }

    // ---- TUR / CLEAR UNIT ATTENTION ----
    // Poll TEST UNIT READY until the device is ready,
    // or we're sure media is absent.
    bool tur_ok = false;
    bool sent_start = false;
    for (int attempt = 0; attempt < 8; attempt++)
    {
        if (!tuh_msc_mounted(dev_addr))
            return msc_volume_failed;
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
            break;
        if (msc_scsi_test_unit_ready(vol, msc_floor_ms(deadline)) == MSC_STATUS_PASSED)
        {
            tur_ok = true;
            break;
        }
        uint8_t sk = msc_volume_sense_key[vol];
        uint8_t asc = msc_volume_sense_asc[vol];
        uint8_t ascq = msc_volume_sense_ascq[vol];
        // Medium Not Present: some drives (e.g. TEAC floppy) return
        // stale 2/3Ah/00h after media reinsertion. Allow one retry.
        if (asc == 0x3A)
        {
            if (msc_volume_status[vol] == msc_volume_ejected &&
                attempt == 0)
                continue;
            break;
        }
        // NOT READY (2) or UNIT ATTENTION (6) - retry
        if (sk == SCSI_SENSE_NOT_READY || sk == SCSI_SENSE_UNIT_ATTENTION)
        {
            // Per SBC-4 §5.25: on first NOT READY, send START STOP UNIT
            // (Start=1) once to kick media initialization.
            // Skip subcases that won't resolve without intervention.
            if (!sent_start && sk == SCSI_SENSE_NOT_READY)
            {
                if (asc == 0x04)
                {
                    // Manual intervention, standby, unavailable - hopeless
                    if (ascq == 0x03 || ascq == 0x0B || ascq == 0x0C)
                        break;
                }
                sent_start = true;
                DBG("MSC vol %d: START STOP UNIT (Start)\n", vol);
                msc_scsi_start_stop_unit(vol, msc_floor_ms(deadline),
                                         true, false);
            }
            continue;
        }
        // Any other sense key - stop retrying
        break;
    }
    if (!tur_ok && msc_volume_is_removable[vol])
        return msc_volume_ejected;

    // ---- CAPACITY ----
    if (!msc_read_capacity(vol, msc_floor_ms(deadline)))
        return msc_volume_is_removable[vol] ? msc_volume_ejected : msc_volume_failed;

    // ---- WRITE PROTECTION ----
    msc_read_write_protect(vol, msc_floor_ms(deadline));

    DBG("MSC vol %d: init ok, %lu blocks\n", vol,
        (unsigned long)msc_volume_block_count[vol]);
    return msc_volume_mounted;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
    uint8_t const max_lun = tuh_msc_get_maxlun(dev_addr);
    for (uint8_t lun = 0; lun <= max_lun; lun++)
    {
        // Find a free FatFS volume slot.
        uint8_t vol = FF_VOLUMES;
        for (uint8_t v = 0; v < FF_VOLUMES; v++)
        {
            if (msc_volume_status[v] == msc_volume_free)
            {
                vol = v;
                break;
            }
        }
        if (vol == FF_VOLUMES)
        {
            DBG("MSC mount: no free vol for dev %d LUN %d\n", dev_addr, lun);
            continue;
        }
        msc_volume_dev_addr[vol] = dev_addr;
        msc_volume_lun[vol] = lun;
        msc_volume_status[vol] = msc_volume_registered;
        TCHAR volstr[6];
        msc_vol_path(volstr, vol);
        f_mount(&msc_fatfs_volumes[vol], volstr, 0);
        DBG("MSC mount dev_addr %d LUN %d -> vol %d\n", dev_addr, lun, vol);
    }
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
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
            msc_volume_lun[vol] = 0;
            msc_volume_block_count[vol] = 0;
            msc_volume_block_size[vol] = 0;
            msc_volume_sense_key[vol] = 0;
            msc_volume_sense_asc[vol] = 0;
            msc_volume_sense_ascq[vol] = 0;
            msc_volume_write_protected[vol] = false;
            msc_volume_is_removable[vol] = false;
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
    bool const ejected = (msc_volume_status[vol] == msc_volume_ejected);

    if (!ejected && msc_volume_status[vol] != msc_volume_mounted)
    {
        DBG("MSC vol %d: disk_status, not mounted, status=%d\n", vol, msc_volume_status[vol]);
        return STA_NOINIT;
    }

    if (!ejected)
    {
        uint8_t const dev_addr = msc_volume_dev_addr[vol];
        if (dev_addr == 0 || !tuh_msc_mounted(dev_addr))
        {
            DBG("MSC vol %d: disk_status, device not mounted\n", vol);
            return STA_NOINIT;
        }
    }

    // TUR for removable volumes: rate-limited by MSC_DISK_STATUS_TIMEOUT_MS.
    if (msc_volume_is_removable[vol] &&
        absolute_time_diff_us(msc_volume_last_success[vol], get_absolute_time()) >=
            MSC_DISK_STATUS_TIMEOUT_MS * 1000)
    {
        msc_volume_last_success[vol] = get_absolute_time(); // always rate-limit
        DBG("MSC vol %d: disk_status, issuing TUR\n", vol);
        if (msc_scsi_test_unit_ready(vol, MSC_OP_TIMEOUT_MS) == MSC_STATUS_PASSED)
        {
            if (ejected)
            {
                DBG("MSC vol %d: disk_status, media reinserted\n", vol);
                return STA_NOINIT;
            }
        }
        else
        {
            DBG("MSC vol %d: disk_status, no media\n", vol);
            if (!ejected)
                msc_handle_io_error(vol);
            return STA_NOINIT | STA_NODISK;
        }
    }
    else if (ejected)
    {
        return STA_NOINIT | STA_NODISK;
    }

    return msc_volume_write_protected[vol] ? STA_PROTECT : 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
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

static DRESULT msc_status_to_dresult(uint8_t vol, msc_status_t status)
{
    if (status == MSC_STATUS_PASSED)
        return RES_OK;
    if (status == MSC_STATUS_TIMED_OUT)
        return RES_NOTRDY;
    uint8_t sk = msc_volume_sense_key[vol];
    if (sk == SCSI_SENSE_NOT_READY)
        return RES_NOTRDY;
    if (sk == SCSI_SENSE_DATA_PROTECT)
        return RES_WRPRT;
    if (sk == SCSI_SENSE_ILLEGAL_REQUEST)
        return RES_PARERR;
    return RES_ERROR;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    uint8_t vol = pdrv;
    uint32_t const block_size = msc_volume_block_size[vol];
    uint8_t const dev_addr = msc_volume_dev_addr[vol];
    DBG("MSC R> %lu+%u\n", (unsigned long)sector, count);
    if (block_size == 0 || dev_addr == 0)
        return RES_NOTRDY;
#if FF_LBA64
    if (sector > UINT32_MAX)
        return RES_PARERR;
#endif
    // Clamp each transfer so total_bytes fits the USB host transfer
    // length limit (uint16_t).
    uint16_t const max_blocks = (uint16_t)(UINT16_MAX / block_size);
    while (count > 0)
    {
        uint16_t n = (count > max_blocks) ? max_blocks : (uint16_t)count;
        msc_status_t status = msc_scsi_read10(vol, MSC_SCSI_RW_TIMEOUT_MS,
                                              buff, sector, n, block_size);
        if (status != MSC_STATUS_PASSED)
            return msc_status_to_dresult(vol, status);
        buff += (uint32_t)n * block_size;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    uint8_t vol = pdrv;
    if (msc_volume_write_protected[vol])
        return RES_WRPRT;
    uint32_t const block_size = msc_volume_block_size[vol];
    uint8_t const dev_addr = msc_volume_dev_addr[vol];
    DBG("MSC W> %lu+%u\n", (unsigned long)sector, count);
    if (block_size == 0 || dev_addr == 0)
        return RES_NOTRDY;
#if FF_LBA64
    if (sector > UINT32_MAX)
        return RES_PARERR;
#endif
    uint16_t const max_blocks = (uint16_t)(UINT16_MAX / block_size);
    while (count > 0)
    {
        uint16_t n = (count > max_blocks) ? max_blocks : (uint16_t)count;
        msc_status_t status = msc_scsi_write10(vol, MSC_SCSI_RW_TIMEOUT_MS,
                                               buff, sector, n, block_size);
        if (status != MSC_STATUS_PASSED)
            return msc_status_to_dresult(vol, status);
        buff += (uint32_t)n * block_size;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
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
        // Best effort: many USB flash drives don't implement
        // SYNCHRONIZE CACHE, so treat command failures as OK.
        msc_status_t status = msc_scsi_sync_cache10(vol, MSC_SCSI_RW_TIMEOUT_MS);
        return status == MSC_STATUS_TIMED_OUT ? RES_NOTRDY : RES_OK;
    }
    case GET_SECTOR_COUNT:
        *((DWORD *)buff) = msc_volume_block_count[vol];
        return RES_OK;
    case GET_SECTOR_SIZE:
        *((WORD *)buff) = (WORD)msc_volume_block_size[vol];
        return RES_OK;
    case GET_BLOCK_SIZE:
        *((DWORD *)buff) = 1;
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
        // Fully initialize and check removable media.
        if (msc_volume_status[vol] == msc_volume_mounted &&
            msc_volume_is_removable[vol] &&
            msc_scsi_test_unit_ready(vol, MSC_OP_TIMEOUT_MS) != MSC_STATUS_PASSED &&
            msc_volume_sense_asc[vol] == 0x3A)
        {
            msc_handle_io_error(vol);
        }
        else
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
        scsi_inquiry_resp_t inq;
        memset(&inq, 0, sizeof(inq));
        msc_status_t csw = msc_scsi_inquiry(vol, MSC_OP_TIMEOUT_MS, &inq);
        if (csw == MSC_STATUS_PASSED)
        {
            msc_inquiry_rtrims(inq.vendor_id, 8);
            msc_inquiry_rtrims(inq.product_id, 16);
            msc_inquiry_rtrims(inq.product_rev, 4);
            snprintf(buf, buf_size, STR_STATUS_MSC,
                     VolumeStr[vol],
                     sizebuf,
                     inq.vendor_id,
                     inq.product_id,
                     inq.product_rev);
        }
        else
        {
            snprintf(buf, buf_size, STR_STATUS_MSC,
                     VolumeStr[vol],
                     sizebuf,
                     STR_PARENS_NONE, STR_PARENS_NONE, STR_PARENS_NONE);
        }
    }
    return state + 1;
}

static FIL *msc_std_validate_fil(int desc)
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
    static_assert(FA_READ == 0x01);
    static_assert(FA_WRITE == 0x02);
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
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }

    return (int)(fp - msc_std_fil_pool);
}

int msc_std_close(int desc, api_errno *err)
{
    FIL *fp = msc_std_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return -1;
    }
    FRESULT fresult = f_close(fp);
    fp->obj.fs = NULL;
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

std_rw_result msc_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    FIL *fp = msc_std_validate_fil(desc);
    if (!fp)
    {
        *bytes_read = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    UINT br;
    FRESULT fresult = f_read(fp, buf, count, &br);
    *bytes_read = br;
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

std_rw_result msc_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    FIL *fp = msc_std_validate_fil(desc);
    if (!fp)
    {
        *bytes_written = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    *bytes_written = bw;
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

int msc_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err)
{
    FIL *fp = msc_std_validate_fil(desc);
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
        if (offset > 0 && (FSIZE_t)offset > (~(FSIZE_t)0) - current_pos)
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
        if (offset > 0 && (FSIZE_t)offset > (~(FSIZE_t)0) - file_size)
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
        *err = api_errno_from_fatfs(fresult);
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
    FIL *fp = msc_std_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return -1;
    }
    FRESULT fresult = f_sync(fp);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}
