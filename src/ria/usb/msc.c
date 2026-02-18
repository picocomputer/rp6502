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

// Initialize a CBW with standard boilerplate fields (for disk_read/write).
static inline void msc_cbw_init(msc_cbw_t *cbw, uint8_t lun)
{
    memset(cbw, 0, sizeof(msc_cbw_t));
    cbw->signature = MSC_CBW_SIGNATURE;
    cbw->tag = 0x54555342; // "TUSB"
    cbw->lun = lun;
}

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
    msc_volume_initialized, // SCSI init done, awaiting f_mount from msc_task
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
// It will not work with: src/tinyusb/src/class/msc/msc_host.c
// These additional interfaces are not in upstream TinyUSB.
bool tuh_msc_is_cbi(uint8_t dev_addr);
bool tuh_msc_reset_recovery(uint8_t dev_addr);
bool tuh_msc_recovery_in_progress(uint8_t dev_addr);

// Async SCSI wrappers (not in upstream header)
bool tuh_msc_read_format_capacities(uint8_t dev_addr, uint8_t lun, void *response,
                                    uint8_t alloc_length,
                                    tuh_msc_complete_cb_t complete_cb, uintptr_t arg);
bool tuh_msc_mode_sense6(uint8_t dev_addr, uint8_t lun, void *response,
                         tuh_msc_complete_cb_t complete_cb, uintptr_t arg);
bool tuh_msc_start_stop_unit(uint8_t dev_addr, uint8_t lun, bool start,
                             tuh_msc_complete_cb_t complete_cb, uintptr_t arg);

// Synchronous I/O bookkeeping (no spin-waits — msc_host.c is
// reentrant-safe; all event-pump loops live here in msc.c).
tuh_msc_complete_cb_t tuh_msc_sync_begin(uint8_t dev_addr);
bool tuh_msc_sync_is_busy(uint8_t dev_addr);
uint8_t tuh_msc_sync_csw_status(uint8_t dev_addr);
void tuh_msc_sync_clear_busy(uint8_t dev_addr);

//--------------------------------------------------------------------+
// Asynchronous volume initialization state machine
//--------------------------------------------------------------------+
// tuh_msc_mount_cb starts the SCSI init sequence via msc_vinit_submit().
// Each step fires a callback that advances to the next step — no
// spin-waits, no nested tuh_task(), no polling task.
typedef enum
{
    VINIT_IDLE,
    VINIT_INQUIRY,
    VINIT_INQUIRY_SENSE,
    VINIT_TUR,
    VINIT_SENSE,
    VINIT_START_UNIT,
    VINIT_TUR_UA,
    VINIT_SENSE_UA,
    VINIT_CAPACITY,
    VINIT_CAPACITY_SENSE,
    VINIT_READ_BLOCK0,
    VINIT_MODE_SENSE,
    VINIT_MODE_SENSE_P0,
} vinit_state_t;

static const char *const vinit_state_name[] = {
    [VINIT_IDLE] = "IDLE",
    [VINIT_INQUIRY] = "INQUIRY",
    [VINIT_INQUIRY_SENSE] = "INQUIRY_SENSE",
    [VINIT_TUR] = "TUR",
    [VINIT_SENSE] = "SENSE",
    [VINIT_START_UNIT] = "START_UNIT",
    [VINIT_TUR_UA] = "TUR_UA",
    [VINIT_SENSE_UA] = "SENSE_UA",
    [VINIT_CAPACITY] = "CAPACITY",
    [VINIT_CAPACITY_SENSE] = "CAPACITY_SENSE",
    [VINIT_READ_BLOCK0] = "READ_BLOCK0",
    [VINIT_MODE_SENSE] = "MODE_SENSE",
    [VINIT_MODE_SENSE_P0] = "MODE_SENSE_P0",
};

static vinit_state_t msc_vinit_state[FF_VOLUMES];
static scsi_sense_fixed_resp_t msc_vinit_sense_buf[FF_VOLUMES];
static union
{
    scsi_read_capacity10_resp_t cap10;
    scsi_mode_sense6_resp_t mode_sense;
    uint8_t rfc[12];
} msc_vinit_buf[FF_VOLUMES];

// Retry counters for unit attention handling during init.
#define VINIT_UA_RETRIES_MAX 5
#define VINIT_INQ_RETRIES_MAX 3
static uint8_t msc_vinit_ua_retries[FF_VOLUMES];
static uint8_t msc_vinit_inq_retries[FF_VOLUMES];

// Shared discard buffer for read-block-0 (forces device to populate
// mode pages — data is not used).  Shared across volumes since the
// contents are thrown away.
static uint8_t msc_vinit_block0[512];

//--------------------------------------------------------------------+
// Async TUR poll for removable media detection
//--------------------------------------------------------------------+
// msc_status_count kicks off an async TUR for each mounted removable
// volume. The callback handles sense + ejection without blocking.

typedef enum
{
    POLL_IDLE,
    POLL_TUR,
    POLL_SENSE,
} msc_poll_state_t;

static msc_poll_state_t msc_poll_state[FF_VOLUMES];
static scsi_sense_fixed_resp_t msc_poll_sense_buf[FF_VOLUMES];
static bool msc_poll_cb(uint8_t dev_addr,
                        tuh_msc_complete_data_t const *cb_data);
// forward declaration — defined after msc_handle_io_error

//--------------------------------------------------------------------+
// Synchronous I/O helpers (spin-wait lives here, not in msc_host.c)
//--------------------------------------------------------------------+
// tuh_task_device_only pumps USB events for a single device without
// re-entering the full task tree. Defined in usb/usbh.c.
void tuh_task_device_only(uint8_t dev_addr);

// Track whether the last msc_sync_wait_io call timed out.
static bool msc_sync_timed_out;

// Wait until tuh_msc_ready() or timeout.
static bool msc_sync_wait_ready(uint8_t dev_addr, uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!tuh_msc_ready(dev_addr))
    {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
            return false;
        tuh_task_device_only(dev_addr);
    }
    return true;
}

// Wait ready + arm busy flag → return callback (NULL on timeout).
static tuh_msc_complete_cb_t msc_sync_begin(uint8_t dev_addr,
                                            uint32_t timeout_ms)
{
    if (!msc_sync_wait_ready(dev_addr, timeout_ms))
        return NULL;
    return tuh_msc_sync_begin(dev_addr);
}

// Wait for in-flight command to complete. On timeout, triggers
// reset recovery and returns MSC_CSW_STATUS_FAILED.
static uint8_t msc_sync_wait_io(uint8_t dev_addr, uint32_t timeout_ms)
{
    msc_sync_timed_out = false;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (tuh_msc_sync_is_busy(dev_addr))
    {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0)
        {
            tuh_msc_reset_recovery(dev_addr);
            absolute_time_t rec_deadline = make_timeout_time_ms(timeout_ms);
            while (tuh_msc_recovery_in_progress(dev_addr))
            {
                if (absolute_time_diff_us(get_absolute_time(), rec_deadline) <= 0)
                    break;
                tuh_task_device_only(dev_addr);
            }
            tuh_msc_sync_clear_busy(dev_addr);
            msc_sync_timed_out = true;
            return MSC_CSW_STATUS_FAILED;
        }
        tuh_task_device_only(dev_addr);
    }
    return tuh_msc_sync_csw_status(dev_addr);
}

// Perform reset recovery with blocking wait.
static void msc_sync_recovery(uint8_t dev_addr, uint32_t timeout_ms)
{
    tuh_msc_reset_recovery(dev_addr);
    absolute_time_t rec_deadline = make_timeout_time_ms(timeout_ms);
    while (tuh_msc_recovery_in_progress(dev_addr))
    {
        if (absolute_time_diff_us(get_absolute_time(), rec_deadline) <= 0)
            break;
        tuh_task_device_only(dev_addr);
    }
}


// Issue REQUEST SENSE and store result in per-volume sense arrays.
// Returns true if the transport succeeded (sense data is valid).
static bool msc_do_request_sense(uint8_t vol)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    if (dev_addr == 0)
        return false; // device disconnected

    tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, MSC_IO_TIMEOUT_MS);
    if (!cb)
        return false;
    scsi_sense_fixed_resp_t sense_resp;
    memset(&sense_resp, 0, sizeof(sense_resp));
    if (!tuh_msc_request_sense(dev_addr, 0, &sense_resp, cb, 0))
    {
        tuh_msc_sync_clear_busy(dev_addr);
        return false;
    }
    msc_sync_wait_io(dev_addr, MSC_IO_TIMEOUT_MS);

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
static bool msc_send_tur(uint8_t vol)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    if (dev_addr == 0)
        return false; // device disconnected
    tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, MSC_IO_TIMEOUT_MS);
    if (!cb)
        return false;
    if (!tuh_msc_test_unit_ready(dev_addr, 0, cb, 0))
    {
        tuh_msc_sync_clear_busy(dev_addr);
        return false;
    }
    return msc_sync_wait_io(dev_addr, MSC_IO_TIMEOUT_MS) == MSC_CSW_STATUS_PASSED;
}

// Handle I/O error on a removable volume: unmount and mark ejected.
// The deferred f_mount stays registered so the next FatFS access
// triggers disk_initialize, which kicks off an async re-probe.
static void msc_handle_io_error(uint8_t vol)
{
    if (!msc_inquiry_resp[vol].is_removable)
        return;
    // If tuh_msc_umount_cb already freed this volume, don't resurrect it
    // as ejected — that would prevent the slot from being reused on re-plug.
    if (msc_volume_status[vol] == msc_volume_free)
        return;
    // Re-register deferred mount so FatFS calls disk_initialize on
    // the next file access, triggering the async re-probe.
    TCHAR volstr[6] = MSC_VOL0;
    volstr[3] += vol;
    f_mount(&msc_fatfs_volumes[vol], volstr, 0);
    msc_volume_status[vol] = msc_volume_ejected;
    msc_volume_tur_ok[vol] = false;
    msc_volume_block_count[vol] = 0;
    msc_volume_block_size[vol] = 0;
    msc_volume_write_protected[vol] = false;

    DBG("MSC vol %d: I/O error — marked ejected\n", vol);
}

// Async TUR poll callback for mounted removable volumes.
// Handles TUR → SENSE sequence; updates cached status on failure.
static bool msc_poll_cb(uint8_t dev_addr,
                        tuh_msc_complete_data_t const *cb_data)
{
    uint8_t vol = (uint8_t)cb_data->user_arg;
    if (vol >= FF_VOLUMES ||
        msc_volume_dev_addr[vol] != dev_addr ||
        msc_volume_status[vol] != msc_volume_mounted)
    {
        if (vol < FF_VOLUMES)
            msc_poll_state[vol] = POLL_IDLE;
        return true;
    }

    switch (msc_poll_state[vol])
    {
    case POLL_TUR:
        if (cb_data->csw->status == MSC_CSW_STATUS_PASSED)
        {
            msc_volume_tur_ok[vol] = true;
            msc_poll_state[vol] = POLL_IDLE;
        }
        else
        {
            memset(&msc_poll_sense_buf[vol], 0,
                   sizeof(msc_poll_sense_buf[vol]));
            if (tuh_msc_request_sense(dev_addr, 0,
                                      &msc_poll_sense_buf[vol],
                                      msc_poll_cb, (uintptr_t)vol))
            {
                msc_poll_state[vol] = POLL_SENSE;
            }
            else
            {
                msc_handle_io_error(vol);
                msc_poll_state[vol] = POLL_IDLE;
            }
        }
        break;

    case POLL_SENSE:
    {
        scsi_sense_fixed_resp_t const *s = &msc_poll_sense_buf[vol];
        if (s->response_code)
        {
            msc_volume_sense_key[vol] = s->sense_key;
            msc_volume_sense_asc[vol] = s->add_sense_code;
            msc_volume_sense_ascq[vol] = s->add_sense_qualifier;
        }
        msc_handle_io_error(vol);
        msc_poll_state[vol] = POLL_IDLE;
        break;
    }

    default:
        msc_poll_state[vol] = POLL_IDLE;
        break;
    }

    return true;
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
// Async init: submit helpers
//--------------------------------------------------------------------+
static bool msc_vinit_cb(uint8_t dev_addr,
                         tuh_msc_complete_data_t const *cb_data);
static void msc_vinit_submit(uint8_t vol);

static bool vinit_submit_inquiry(uint8_t vol)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    memset(&msc_inquiry_resp[vol], 0, sizeof(msc_inquiry_resp[vol]));
    return tuh_msc_inquiry(dev_addr, 0, &msc_inquiry_resp[vol],
                           msc_vinit_cb, (uintptr_t)vol);
}

static bool vinit_submit_tur(uint8_t vol)
{
    return tuh_msc_test_unit_ready(msc_volume_dev_addr[vol], 0,
                                   msc_vinit_cb, (uintptr_t)vol);
}

static bool vinit_submit_sense(uint8_t vol)
{
    memset(&msc_vinit_sense_buf[vol], 0, sizeof(msc_vinit_sense_buf[vol]));
    return tuh_msc_request_sense(msc_volume_dev_addr[vol], 0,
                                 &msc_vinit_sense_buf[vol],
                                 msc_vinit_cb, (uintptr_t)vol);
}

static bool vinit_submit_start_unit(uint8_t vol)
{
    return tuh_msc_start_stop_unit(msc_volume_dev_addr[vol], 0, true,
                                   msc_vinit_cb, (uintptr_t)vol);
}

static bool vinit_submit_capacity(uint8_t vol)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    if (tuh_msc_is_cbi(dev_addr))
    {
        memset(msc_vinit_buf[vol].rfc, 0, sizeof(msc_vinit_buf[vol].rfc));
        return tuh_msc_read_format_capacities(
            dev_addr, 0, msc_vinit_buf[vol].rfc,
            sizeof(msc_vinit_buf[vol].rfc),
            msc_vinit_cb, (uintptr_t)vol);
    }
    memset(&msc_vinit_buf[vol].cap10, 0, sizeof(msc_vinit_buf[vol].cap10));
    return tuh_msc_read_capacity(dev_addr, 0, &msc_vinit_buf[vol].cap10,
                                 msc_vinit_cb, (uintptr_t)vol);
}

static bool vinit_submit_mode_sense(uint8_t vol)
{
    memset(&msc_vinit_buf[vol].mode_sense, 0,
           sizeof(msc_vinit_buf[vol].mode_sense));
    return tuh_msc_mode_sense6(msc_volume_dev_addr[vol], 0,
                               &msc_vinit_buf[vol].mode_sense,
                               msc_vinit_cb, (uintptr_t)vol);
}

// MODE SENSE(6) with page 0 — fallback when page 0x3F is rejected.
static bool vinit_submit_mode_sense_p0(uint8_t vol)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = sizeof(scsi_mode_sense6_resp_t);
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = 6;
    cbw.command[0] = 0x1A; // MODE SENSE(6)
    cbw.command[1] = 0x08; // DBD=1
    cbw.command[2] = 0x00; // page code 0
    cbw.command[4] = sizeof(scsi_mode_sense6_resp_t);
    memset(&msc_vinit_buf[vol].mode_sense, 0,
           sizeof(msc_vinit_buf[vol].mode_sense));
    return tuh_msc_scsi_command(dev_addr, &cbw,
                                &msc_vinit_buf[vol].mode_sense,
                                msc_vinit_cb, (uintptr_t)vol);
}

// Read block 0 — forces some USB devices to populate mode pages
// with real values (Linux read_before_ms quirk).  Data is discarded.
static bool vinit_submit_read_block0(uint8_t vol)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    uint32_t bs = msc_volume_block_size[vol];
    if (bs == 0 || bs > sizeof(msc_vinit_block0))
        return false;
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = bs;
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = sizeof(scsi_read10_t);
    scsi_read10_t const cmd = {
        .cmd_code = SCSI_CMD_READ_10,
        .lba = 0,
        .block_count = tu_htons(1)};
    memcpy(cbw.command, &cmd, sizeof(cmd));
    return tuh_msc_scsi_command(dev_addr, &cbw, msc_vinit_block0,
                                msc_vinit_cb, (uintptr_t)vol);
}

// Dispatch the submission for the current vinit state.
static void msc_vinit_submit(uint8_t vol)
{
    DBG("MSC vol %d: vinit -> %s\n", vol,
        vinit_state_name[msc_vinit_state[vol]]);
    bool ok = false;
    switch (msc_vinit_state[vol])
    {
    case VINIT_INQUIRY:
        ok = vinit_submit_inquiry(vol);
        break;
    case VINIT_TUR:
    case VINIT_TUR_UA:
        ok = vinit_submit_tur(vol);
        break;
    case VINIT_INQUIRY_SENSE:
    case VINIT_SENSE:
    case VINIT_SENSE_UA:
    case VINIT_CAPACITY_SENSE:
        ok = vinit_submit_sense(vol);
        break;
    case VINIT_START_UNIT:
        ok = vinit_submit_start_unit(vol);
        break;
    case VINIT_CAPACITY:
        ok = vinit_submit_capacity(vol);
        break;
    case VINIT_READ_BLOCK0:
        ok = vinit_submit_read_block0(vol);
        break;
    case VINIT_MODE_SENSE:
        ok = vinit_submit_mode_sense(vol);
        break;
    case VINIT_MODE_SENSE_P0:
        ok = vinit_submit_mode_sense_p0(vol);
        break;
    default:
        return;
    }
    if (!ok)
    {
        DBG("MSC vol %d: vinit submit failed (state %d)\n",
            vol, msc_vinit_state[vol]);
        msc_vinit_state[vol] = VINIT_IDLE;
        msc_volume_status[vol] = msc_volume_failed;
    }
}

//--------------------------------------------------------------------+
// Async init: state transition helpers
//--------------------------------------------------------------------+
static void vinit_clear_sense(uint8_t vol)
{
    msc_volume_sense_key[vol] = SCSI_SENSE_NONE;
    msc_volume_sense_asc[vol] = 0;
    msc_volume_sense_ascq[vol] = 0;
}

static void vinit_store_sense(uint8_t vol)
{
    scsi_sense_fixed_resp_t const *s = &msc_vinit_sense_buf[vol];
    if (s->response_code)
    {
        msc_volume_sense_key[vol] = s->sense_key;
        msc_volume_sense_asc[vol] = s->add_sense_code;
        msc_volume_sense_ascq[vol] = s->add_sense_qualifier;
    }
    else
    {
        vinit_clear_sense(vol);
    }
    DBG("MSC vol %d: sense %d/%02Xh/%02Xh\n",
        vol, msc_volume_sense_key[vol],
        msc_volume_sense_asc[vol], msc_volume_sense_ascq[vol]);
}

static void vinit_finish(uint8_t vol, msc_volume_status_t status)
{
    msc_vinit_state[vol] = VINIT_IDLE;
    DBG("MSC vol %d: vinit -> IDLE\n", vol);
    // When SCSI init succeeds, set initialized — msc_task() will
    // register the deferred f_mount and transition to mounted.
    if (status == msc_volume_mounted)
        status = msc_volume_initialized;
    msc_volume_status[vol] = status;
    if (status == msc_volume_initialized)
    {
        msc_volume_tur_ok[vol] = true;
        DBG("MSC vol %d: initialized %lu blocks\n", vol,
            (unsigned long)msc_volume_block_count[vol]);
    }
    else if (status == msc_volume_ejected)
    {
        DBG("MSC vol %d: removable, no media\n", vol);
    }
    else
    {
        DBG("MSC vol %d: init failed\n", vol);
    }
}

// Advance to capacity read (shared by all TUR-pass paths).
static void vinit_advance_to_capacity(uint8_t vol)
{
    msc_vinit_state[vol] = VINIT_CAPACITY;
    msc_vinit_submit(vol);
}

// Post-INQUIRY dispatch — shared by VINIT_INQUIRY success and
// VINIT_INQUIRY_SENSE fallback when the response data is usable.
static void vinit_post_inquiry(uint8_t vol)
{
    if (msc_inquiry_resp[vol].is_removable)
    {
        msc_vinit_state[vol] = VINIT_TUR;
        msc_vinit_submit(vol);
    }
    else
    {
        vinit_advance_to_capacity(vol);
    }
}

//--------------------------------------------------------------------+
// Async init: completion callback
//--------------------------------------------------------------------+
static bool msc_vinit_cb(uint8_t dev_addr,
                         tuh_msc_complete_data_t const *cb_data)
{
    uint8_t vol = (uint8_t)cb_data->user_arg;
    if (vol >= FF_VOLUMES ||
        msc_volume_dev_addr[vol] != dev_addr ||
        msc_volume_status[vol] != msc_volume_registered)
    {
        // Stale or cancelled — discard.
        if (vol < FF_VOLUMES)
            msc_vinit_state[vol] = VINIT_IDLE;
        return true;
    }

    uint8_t csw_status = cb_data->csw->status;

    switch (msc_vinit_state[vol])
    {
    // ---- INQUIRY ----
    case VINIT_INQUIRY:
    {
        msc_inquiry_rtrims(msc_inquiry_resp[vol].vendor_id, 8);
        msc_inquiry_rtrims(msc_inquiry_resp[vol].product_id, 16);
        msc_inquiry_rtrims(msc_inquiry_resp[vol].product_rev, 4);
        if (csw_status != MSC_CSW_STATUS_PASSED)
        {
            // Check for unit attention before giving up on INQUIRY.
            msc_vinit_state[vol] = VINIT_INQUIRY_SENSE;
            msc_vinit_submit(vol);
            break;
        }
        vinit_post_inquiry(vol);
        break;
    }

    case VINIT_INQUIRY_SENSE:
    {
        vinit_store_sense(vol);
        uint8_t sk = msc_volume_sense_key[vol];
        uint8_t asc = msc_volume_sense_asc[vol];
        // USB flash devices may report UA 0x28 (not-ready-to-ready)
        // or 0x29 (power-on/reset) during early enumeration.
        if (sk == SCSI_SENSE_UNIT_ATTENTION &&
            (asc == 0x28 || asc == 0x29) &&
            msc_vinit_inq_retries[vol] < VINIT_INQ_RETRIES_MAX)
        {
            msc_vinit_inq_retries[vol]++;
            DBG("MSC vol %d: INQUIRY unit attention (0x%02X), "
                "retry %d\n", vol, asc, msc_vinit_inq_retries[vol]);
            msc_vinit_state[vol] = VINIT_INQUIRY;
            msc_vinit_submit(vol);
            break;
        }
        // Not UA — check if the INQUIRY response data is usable.
        scsi_inquiry_resp_t const *inq = &msc_inquiry_resp[vol];
        if (inq->peripheral_qualifier == 3 ||
            (inq->response_data_format != 1 &&
             inq->response_data_format != 2) ||
            inq->additional_length < 31)
        {
            DBG("MSC vol %d: inquiry failed (invalid response)\n", vol);
            vinit_finish(vol, msc_volume_failed);
            return true;
        }
        DBG("MSC vol %d: inquiry CSW not passed "
            "(response valid, continuing)\n", vol);
        vinit_post_inquiry(vol);
        break;
    }

    // ---- TUR: one round, then decide ----
    case VINIT_TUR:
        if (csw_status == MSC_CSW_STATUS_PASSED)
        {
            vinit_clear_sense(vol);
            vinit_advance_to_capacity(vol);
        }
        else
        {
            msc_vinit_state[vol] = VINIT_SENSE;
            msc_vinit_submit(vol);
        }
        break;

    case VINIT_SENSE:
    {
        vinit_store_sense(vol);
        uint8_t sk = msc_volume_sense_key[vol];
        uint8_t asc = msc_volume_sense_asc[vol];
        if (sk == SCSI_SENSE_NOT_READY)
        {
            if (asc == 0x3A)
            {
                DBG("MSC vol %d: medium not present\n", vol);
                vinit_finish(vol, msc_volume_ejected);
                return true;
            }
            DBG("MSC vol %d: not ready (%02Xh/%02Xh), "
                "sending START UNIT\n",
                vol, asc, msc_volume_sense_ascq[vol]);
            msc_vinit_state[vol] = VINIT_START_UNIT;
            msc_vinit_submit(vol);
        }
        else if (sk == SCSI_SENSE_UNIT_ATTENTION)
        {
            // USB flash may report UA 0x28/0x29 repeatedly during
            // slow firmware start-up — retry TUR up to 5 times.
            if ((asc == 0x28 || asc == 0x29) &&
                msc_vinit_ua_retries[vol] < VINIT_UA_RETRIES_MAX)
            {
                msc_vinit_ua_retries[vol]++;
                DBG("MSC vol %d: UA 0x%02X, TUR retry %d\n",
                    vol, asc, msc_vinit_ua_retries[vol]);
                msc_vinit_state[vol] = VINIT_TUR;
                msc_vinit_submit(vol);
            }
            else
            {
                msc_vinit_state[vol] = VINIT_TUR_UA;
                msc_vinit_submit(vol);
            }
        }
        else
        {
            DBG("MSC vol %d: TUR sense %d/%02Xh/%02Xh\n",
                vol, sk, asc, msc_volume_sense_ascq[vol]);
            vinit_finish(vol, msc_volume_ejected);
        }
        break;
    }

    case VINIT_START_UNIT:
        msc_vinit_state[vol] = VINIT_TUR_UA;
        msc_vinit_submit(vol);
        break;

    case VINIT_TUR_UA:
        if (csw_status == MSC_CSW_STATUS_PASSED)
        {
            vinit_clear_sense(vol);
            vinit_advance_to_capacity(vol);
        }
        else
        {
            msc_vinit_state[vol] = VINIT_SENSE_UA;
            msc_vinit_submit(vol);
        }
        break;

    case VINIT_SENSE_UA:
    {
        vinit_store_sense(vol);
        uint8_t sk_ua = msc_volume_sense_key[vol];
        uint8_t asc_ua = msc_volume_sense_asc[vol];
        // Continue retrying for UA 0x28/0x29 (slow USB firmware).
        if (sk_ua == SCSI_SENSE_UNIT_ATTENTION &&
            (asc_ua == 0x28 || asc_ua == 0x29) &&
            msc_vinit_ua_retries[vol] < VINIT_UA_RETRIES_MAX)
        {
            msc_vinit_ua_retries[vol]++;
            DBG("MSC vol %d: UA 0x%02X post-start, TUR retry %d\n",
                vol, asc_ua, msc_vinit_ua_retries[vol]);
            msc_vinit_state[vol] = VINIT_TUR_UA;
            msc_vinit_submit(vol);
        }
        else
        {
            vinit_finish(vol, msc_volume_ejected);
        }
        break;
    }

    // ---- CAPACITY ----
    case VINIT_CAPACITY:
    {
        if (tuh_msc_is_cbi(dev_addr))
        {
            uint8_t *rfc = msc_vinit_buf[vol].rfc;
            uint8_t list_len = rfc[3];
            if (list_len < 8)
            {
                DBG("MSC vol %d: RFC list_len=%d (too short)\n",
                    vol, list_len);
                vinit_finish(vol, msc_inquiry_resp[vol].is_removable
                                      ? msc_volume_ejected
                                      : msc_volume_failed);
                return true;
            }
            uint8_t desc_type = rfc[4 + 4] & 0x03;
            if (desc_type == 3)
            {
                DBG("MSC vol %d: RFC desc_type=3 (no media)\n", vol);
                vinit_finish(vol, msc_volume_ejected);
                return true;
            }
            uint8_t *desc0 = &rfc[4];
            uint32_t blocks = tu_ntohl(*(uint32_t *)&desc0[0]);
            uint32_t bsize = ((uint32_t)desc0[5] << 16) |
                             ((uint32_t)desc0[6] << 8) |
                             (uint32_t)desc0[7];
            DBG("MSC vol %d: RFC %lu blocks, %lu bytes/block, type %d\n",
                vol, (unsigned long)blocks, (unsigned long)bsize, desc_type);
            if (blocks == 0 || bsize != 512)
            {
                vinit_finish(vol, msc_inquiry_resp[vol].is_removable
                                      ? msc_volume_ejected
                                      : msc_volume_failed);
                return true;
            }
            msc_volume_block_count[vol] = blocks;
            msc_volume_block_size[vol] = bsize;
            // CBI — skip MODE SENSE
            vinit_finish(vol, msc_volume_mounted);
        }
        else
        {
            if (csw_status != MSC_CSW_STATUS_PASSED)
            {
                // For removable media, do REQUEST SENSE to confirm
                // the failure reason (e.g. ASC 0x3A = no media).
                if (msc_inquiry_resp[vol].is_removable)
                {
                    msc_vinit_state[vol] = VINIT_CAPACITY_SENSE;
                    msc_vinit_submit(vol);
                }
                else
                {
                    vinit_finish(vol, msc_volume_failed);
                }
                return true;
            }
            scsi_read_capacity10_resp_t *cap = &msc_vinit_buf[vol].cap10;
            msc_volume_block_count[vol] = tu_ntohl(cap->last_lba) + 1;
            msc_volume_block_size[vol] = tu_ntohl(cap->block_size);
            // Read block 0 to force USB devices to populate mode
            // pages with real values (Linux read_before_ms quirk).
            msc_vinit_state[vol] = VINIT_READ_BLOCK0;
            msc_vinit_submit(vol);
        }
        break;
    }

    // ---- CAPACITY SENSE (removable only) ----
    case VINIT_CAPACITY_SENSE:
    {
        vinit_store_sense(vol);
        if (msc_volume_sense_asc[vol] == 0x3A)
        {
            DBG("MSC vol %d: capacity failed — medium not present\n", vol);
            vinit_finish(vol, msc_volume_ejected);
        }
        else
        {
            DBG("MSC vol %d: capacity failed — sense %d/%02Xh/%02Xh\n",
                vol, msc_volume_sense_key[vol],
                msc_volume_sense_asc[vol], msc_volume_sense_ascq[vol]);
            vinit_finish(vol, msc_volume_failed);
        }
        break;
    }

    // ---- READ BLOCK 0 (force mode page population) ----
    case VINIT_READ_BLOCK0:
        if (csw_status != MSC_CSW_STATUS_PASSED)
            DBG("MSC vol %d: block 0 read failed (non-fatal)\n", vol);
        msc_vinit_state[vol] = VINIT_MODE_SENSE;
        msc_vinit_submit(vol);
        break;

    // ---- MODE SENSE ----
    case VINIT_MODE_SENSE:
        if (csw_status == MSC_CSW_STATUS_PASSED)
        {
            scsi_mode_sense6_resp_t *ms = &msc_vinit_buf[vol].mode_sense;
            DBG("MSC vol %d: MODE SENSE WP=%d medium_type=0x%02X\n",
                vol, ms->write_protected, ms->medium_type);
            msc_volume_write_protected[vol] = ms->write_protected;
            vinit_finish(vol, msc_volume_mounted);
        }
        else
        {
            DBG("MSC vol %d: MODE SENSE(6) page 0x3F rejected, "
                "trying page 0\n", vol);
            msc_vinit_state[vol] = VINIT_MODE_SENSE_P0;
            msc_vinit_submit(vol);
        }
        break;

    case VINIT_MODE_SENSE_P0:
        if (csw_status == MSC_CSW_STATUS_PASSED)
        {
            scsi_mode_sense6_resp_t *ms = &msc_vinit_buf[vol].mode_sense;
            DBG("MSC vol %d: MODE SENSE(6) page 0 WP=%d\n",
                vol, ms->write_protected);
            msc_volume_write_protected[vol] = ms->write_protected;
        }
        else
        {
            DBG("MSC vol %d: MODE SENSE(6) not supported\n", vol);
        }
        vinit_finish(vol, msc_volume_mounted);
        break;

    default:
        break;
    }

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

void msc_task(void)
{
    static absolute_time_t next_poll_time;
    bool poll_due = time_reached(next_poll_time);
    if (poll_due)
        next_poll_time = make_timeout_time_ms(2500);

    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        switch (msc_volume_status[vol])
        {
        case msc_volume_registered:
            // Start async SCSI init when the device is ready.
            if (msc_vinit_state[vol] == VINIT_IDLE)
            {
                uint8_t dev_addr = msc_volume_dev_addr[vol];
                if (dev_addr != 0 && tuh_msc_mounted(dev_addr) &&
                    tuh_msc_ready(dev_addr))
                {
                    msc_vinit_inq_retries[vol] = 0;
                    msc_vinit_ua_retries[vol] = 0;
                    msc_vinit_state[vol] = VINIT_INQUIRY;
                    msc_vinit_submit(vol);
                }
            }
            break;

        case msc_volume_initialized:
        {
            // SCSI init complete — register deferred f_mount so
            // FatFS can access the volume on first file operation.
            TCHAR volstr[6] = MSC_VOL0;
            volstr[3] += vol;
            f_mount(&msc_fatfs_volumes[vol], volstr, 0);
            msc_volume_status[vol] = msc_volume_mounted;
            DBG("MSC vol %d: f_mount registered\n", vol);
            break;
        }

        case msc_volume_mounted:
            // Async TUR poll for removable media detection.
            if (poll_due && msc_inquiry_resp[vol].is_removable &&
                msc_poll_state[vol] == POLL_IDLE)
            {
                uint8_t dev_addr = msc_volume_dev_addr[vol];
                if (tuh_msc_ready(dev_addr) &&
                    tuh_msc_test_unit_ready(dev_addr, 0,
                                            msc_poll_cb, (uintptr_t)vol))
                {
                    msc_poll_state[vol] = POLL_TUR;
                    msc_volume_tur_ok[vol] = false;
                }
            }
            break;

        case msc_volume_ejected:
            // Re-probe ejected removable media (skip INQUIRY).
            if (poll_due && msc_vinit_state[vol] == VINIT_IDLE)
            {
                uint8_t dev_addr = msc_volume_dev_addr[vol];
                if (dev_addr != 0 && tuh_msc_mounted(dev_addr) &&
                    tuh_msc_ready(dev_addr))
                {
                    msc_volume_status[vol] = msc_volume_registered;
                    msc_vinit_ua_retries[vol] = 0;
                    msc_vinit_state[vol] = VINIT_TUR;
                    msc_vinit_submit(vol);
                }
            }
            break;

        default:
            break;
        }
    }
}

int msc_status_count(void)
{
    int count = 0;
    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
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
            DBG("MSC mount dev_addr %d -> vol %d\n", dev_addr, vol);
            // msc_task() will start SCSI init and register
            // the deferred f_mount after init completes.
            break;
        }
    }
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    // TinyUSB callback when a USB mass storage device is detached.
    // Clear the busy flag first — msch_close() never fires the
    // user complete_cb on disconnect, so any pending
    // sync wait would spin forever without this.
    tuh_msc_sync_clear_busy(dev_addr);

    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_volume_dev_addr[vol] == dev_addr &&
            msc_volume_status[vol] != msc_volume_free)
        {
            msc_vinit_state[vol] = VINIT_IDLE; // cancel in-progress init
            msc_poll_state[vol] = POLL_IDLE;   // cancel in-progress poll
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
            msc_vinit_ua_retries[vol] = 0;
            msc_vinit_inq_retries[vol] = 0;
            DBG("MSC unmounted dev_addr %d from vol %d\n", dev_addr, vol);
        }
    }
}

static DRESULT msc_scsi_xfer(uint8_t pdrv, msc_cbw_t *cbw, void *buff)
{
    uint8_t const dev_addr = msc_volume_dev_addr[pdrv];
    if (dev_addr == 0)
        return RES_ERROR; // device disconnected
    tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, MSC_IO_TIMEOUT_MS);
    if (!cb)
        return RES_ERROR;
    if (!tuh_msc_scsi_command(dev_addr, cbw, buff, cb, 0))
    {
        tuh_msc_sync_clear_busy(dev_addr);
        return RES_ERROR;
    }
    uint8_t status = msc_sync_wait_io(dev_addr, MSC_IO_TIMEOUT_MS);
    if (status != MSC_CSW_STATUS_PASSED)
    {
        // Scrub the device now so the next probe finds it in a
        // known-good SCSI state and recovers without delay.
        if (!msc_sync_timed_out)
            msc_sync_recovery(dev_addr, MSC_IO_TIMEOUT_MS);
        msc_send_tur(pdrv);
        msc_do_request_sense(pdrv);
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
    if (msc_volume_status[pdrv] == msc_volume_ejected)
        return STA_NOINIT;
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
    DBG("MSC R> %lu+%u\n", (unsigned long)sector, count);
    if (block_size == 0)
        return RES_ERROR;

    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
    cbw.total_bytes = count * block_size;
    cbw.dir = TUSB_DIR_IN_MASK;
    cbw.cmd_len = sizeof(scsi_read10_t);
    scsi_read10_t const cmd = {
        .cmd_code = SCSI_CMD_READ_10,
        .lba = tu_htonl(sector),
        .block_count = tu_htons((uint16_t)count)};
    memcpy(cbw.command, &cmd, sizeof(cmd));

    DRESULT res = msc_scsi_xfer(pdrv, &cbw, buff);
    if (sector == 0 && res == RES_OK)
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
    return res;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv >= FF_VOLUMES)
        return RES_PARERR;
    uint32_t const block_size = msc_volume_block_size[pdrv];
    DBG("MSC W> %lu+%u\n", (unsigned long)sector, count);
    if (block_size == 0)
        return RES_ERROR;

    msc_cbw_t cbw;
    msc_cbw_init(&cbw, 0);
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
    if (pdrv >= FF_VOLUMES)
        return RES_PARERR;
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
