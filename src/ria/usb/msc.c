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

// Overall deadline for disk_initialize re-probe.
// Covers TUR + SENSE + START UNIT + delay + retry + mount.
#define DISK_INIT_TIMEOUT_MS 2500

// Minimum timeout for any individual SCSI command stage within
// disk_initialize.  Prevents a nearly-expired deadline from giving
// a command too little time to complete over CBI transport.
#define DISK_INIT_MIN_STAGE_MS 500

// Delay between retries in disk_initialize NOT_READY path.
// Gives the drive time to detect reinserted media.
#define DISK_INIT_DELAY_MS 1000

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
static bool msc_volume_needs_remount[FF_VOLUMES];

// This driver requires our custom TinyUSB: src/ria/usb/msc_host.c.
// It will not work with: src/tinyusb/src/class/msc/msc_host.c
// These additional interfaces are not in upstream TinyUSB.
bool tuh_msc_is_cbi(uint8_t dev_addr);
bool tuh_msc_reset_recovery(uint8_t dev_addr);
bool tuh_msc_recovery_in_progress(uint8_t dev_addr);
bool tuh_msc_abort_transfers(uint8_t dev_addr);

// Async SCSI wrappers (not in upstream header)
bool tuh_msc_read_format_capacities(uint8_t dev_addr, uint8_t lun, void *response,
                                    uint8_t alloc_length,
                                    tuh_msc_complete_cb_t complete_cb, uintptr_t arg);
bool tuh_msc_mode_sense6(uint8_t dev_addr, uint8_t lun, void *response,
                         tuh_msc_complete_cb_t complete_cb, uintptr_t arg);
bool tuh_msc_start_stop_unit(uint8_t dev_addr, uint8_t lun, bool start,
                             tuh_msc_complete_cb_t complete_cb, uintptr_t arg);

//--------------------------------------------------------------------+
// Synchronous I/O bookkeeping
//--------------------------------------------------------------------+
// Flag management for sync wrappers.  No spin-waits — the caller
// owns the event-pump loop (tuh_task_device_only lives here in msc.c).

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
    _sync_csw_status[dev_addr - 1] = MSC_CSW_STATUS_FAILED;
    _sync_busy[dev_addr - 1] = true;
    return _sync_complete_cb;
}

// Non-blocking: check whether the sync operation is still in progress.
static bool tuh_msc_sync_is_busy(uint8_t dev_addr)
{
    return _sync_busy[dev_addr - 1];
}

// Read the CSW status from the last completed sync operation.
static uint8_t tuh_msc_sync_csw_status(uint8_t dev_addr)
{
    return _sync_csw_status[dev_addr - 1];
}

// Clear the busy flag — call from tuh_msc_umount_cb to unblock any
// pending sync wait on disconnect, or after a failed async submit
// following tuh_msc_sync_begin().
static void tuh_msc_sync_clear_busy(uint8_t dev_addr)
{
    _sync_busy[dev_addr - 1] = false;
}

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
static absolute_time_t msc_poll_deadline[FF_VOLUMES]; // staleness guard
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
// #define tuh_task_device_only(_) main_task(); // also seems to work

// Track whether the last msc_sync_wait_io call timed out.
static bool msc_sync_timed_out;

// Compute milliseconds remaining until an absolute deadline.
static inline uint32_t ms_remaining(absolute_time_t deadline)
{
    int64_t us = absolute_time_diff_us(get_absolute_time(), deadline);
    return (us > 0) ? (uint32_t)(us / 1000) : 0;
}

// Like ms_remaining but with a floor of DISK_INIT_MIN_STAGE_MS.
// Returns 0 only when the deadline has truly expired — otherwise
// returns at least 500 ms so SCSI commands have time to complete.
static inline uint32_t stage_remaining(absolute_time_t deadline)
{
    uint32_t r = ms_remaining(deadline);
    return (r == 0) ? 0 : (r < DISK_INIT_MIN_STAGE_MS ? DISK_INIT_MIN_STAGE_MS : r);
}

// Wait until tuh_msc_ready() or timeout.
// Also checks tuh_msc_mounted() to fail fast on device disconnect,
// since tuh_task_device_only() defers DEVICE_REMOVE events.
static bool msc_sync_wait_ready(uint8_t dev_addr, uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!tuh_msc_ready(dev_addr))
    {
        if (!tuh_msc_mounted(dev_addr))
            return false;
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
// reset recovery (or abort-only if msc_sync_abort_only is set)
// and returns MSC_CSW_STATUS_FAILED.
// Also checks tuh_msc_mounted() to fail fast on disconnect.
static uint8_t msc_sync_wait_io(uint8_t dev_addr, uint32_t timeout_ms)
{
    msc_sync_timed_out = false;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (tuh_msc_sync_is_busy(dev_addr))
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
// Note: for CBI with interrupt endpoint, the sense data buffer is
// filled during the data phase BEFORE interrupt status.  So even if
// the interrupt status times out, the sense data may still be valid
// (response_code != 0).
static bool msc_do_request_sense(uint8_t vol, uint32_t timeout_ms)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    if (dev_addr == 0)
        return false; // device disconnected

    tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, timeout_ms);
    if (!cb)
        return false;
    scsi_sense_fixed_resp_t sense_resp;
    memset(&sense_resp, 0, sizeof(sense_resp));
    if (!tuh_msc_request_sense(dev_addr, 0, &sense_resp, cb, 0))
    {
        tuh_msc_sync_clear_busy(dev_addr);
        return false;
    }
    msc_sync_wait_io(dev_addr, timeout_ms);

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
static bool msc_send_tur(uint8_t vol, uint32_t timeout_ms)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    if (dev_addr == 0)
        return false; // device disconnected
    tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, timeout_ms);
    if (!cb)
        return false;
    if (!tuh_msc_test_unit_ready(dev_addr, 0, cb, 0))
    {
        tuh_msc_sync_clear_busy(dev_addr);
        return false;
    }
    return msc_sync_wait_io(dev_addr, timeout_ms) == MSC_CSW_STATUS_PASSED;
}

// Mark a removable volume as ejected: clear cached geometry and
// set a flag so msc_task() performs the f_unmount/f_mount later.
// This function is safe to call from any context (USB callbacks,
// disk_read/disk_write, poll callbacks) because it never touches
// FatFS — the deferred remount in msc_task() handles that.
static void msc_handle_io_error(uint8_t vol)
{
    if (!msc_inquiry_resp[vol].is_removable)
        return;
    // If tuh_msc_umount_cb already freed this volume, don't resurrect it
    // as ejected — that would prevent the slot from being reused on re-plug.
    // Also skip if already ejected to avoid redundant state transitions.
    if (msc_volume_status[vol] == msc_volume_free ||
        msc_volume_status[vol] == msc_volume_ejected)
        return;

    msc_volume_status[vol] = msc_volume_ejected;
    msc_volume_tur_ok[vol] = false;
    msc_volume_block_count[vol] = 0;
    msc_volume_block_size[vol] = 0;
    msc_volume_write_protected[vol] = false;
    msc_volume_needs_remount[vol] = true;

    DBG("MSC vol %d: media ejected\n", vol);
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
        else if (cb_data->csw->status == MSC_CSW_STATUS_PHASE_ERROR)
        {
            // BOT spec §5.3.3: device state is undefined after phase
            // error — do NOT issue REQUEST_SENSE.  Let the next sync
            // I/O (disk_read/disk_write) handle reset recovery.
            DBG("MSC vol %d: poll TUR phase error\n", vol);
            msc_handle_io_error(vol);
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
                // Submission failure is transient (e.g. CBI control
                // pipe busy).  Do NOT eject — retry next poll cycle.
                DBG("MSC vol %d: poll SENSE submit failed, will retry\n",
                    vol);
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
        else
        {
            msc_volume_sense_key[vol] = SCSI_SENSE_NONE;
            msc_volume_sense_asc[vol] = 0;
            msc_volume_sense_ascq[vol] = 0;
        }
        uint8_t sk = msc_volume_sense_key[vol];
        uint8_t asc = msc_volume_sense_asc[vol];
        if ((sk == SCSI_SENSE_NOT_READY && asc == 0x3A) ||
            sk == SCSI_SENSE_UNIT_ATTENTION)
        {
            // Medium not present or media changed — eject.
            DBG("MSC vol %d: poll sense %d/%02Xh — ejecting\n",
                vol, sk, asc);
            msc_handle_io_error(vol);
            msc_poll_state[vol] = POLL_IDLE;
        }
        else
        {
            // Transient error — will retry on next poll cycle.
            DBG("MSC vol %d: poll sense %d/%02Xh (transient)\n",
                vol, sk, asc);
            msc_poll_state[vol] = POLL_IDLE;
        }
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
// Synchronous volume initialization
//--------------------------------------------------------------------+
// Called from msc_task() when a newly registered volume's device is
// ready.  Performs the full SCSI init sequence synchronously using
// the sync helpers (which pump USB events via tuh_task_device_only).
//
// Returns the resulting volume status:
//   msc_volume_mounted  — success (caller will f_mount)
//   msc_volume_ejected  — removable media not present
//   msc_volume_failed   — non-recoverable error
#define INIT_INQ_RETRIES_MAX 3
#define INIT_TUR_RETRIES_MAX 5

static msc_volume_status_t msc_init_volume(uint8_t vol)
{
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    scsi_inquiry_resp_t *inq = &msc_inquiry_resp[vol];

    // ---- INQUIRY (with UA retry) ----
    bool inquiry_ok = false;
    for (int retry = 0; retry <= INIT_INQ_RETRIES_MAX; retry++)
    {
        if (!tuh_msc_mounted(dev_addr))
            return msc_volume_failed;
        memset(inq, 0, sizeof(*inq));
        tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, MSC_IO_TIMEOUT_MS);
        if (!cb)
            return msc_volume_failed;
        if (!tuh_msc_inquiry(dev_addr, 0, inq, cb, 0))
        {
            tuh_msc_sync_clear_busy(dev_addr);
            return msc_volume_failed;
        }
        uint8_t csw = msc_sync_wait_io(dev_addr, MSC_IO_TIMEOUT_MS);
        msc_inquiry_rtrims(inq->vendor_id, 8);
        msc_inquiry_rtrims(inq->product_id, 16);
        msc_inquiry_rtrims(inq->product_rev, 4);

        if (csw == MSC_CSW_STATUS_PASSED)
        {
            inquiry_ok = true;
            break;
        }
        // Check sense for unit attention (0x28/0x29 during early enum)
        msc_do_request_sense(vol, MSC_IO_TIMEOUT_MS);
        uint8_t sk = msc_volume_sense_key[vol];
        uint8_t asc = msc_volume_sense_asc[vol];
        if (sk == SCSI_SENSE_UNIT_ATTENTION &&
            (asc == 0x28 || asc == 0x29) &&
            retry < INIT_INQ_RETRIES_MAX)
        {
            DBG("MSC vol %d: INQUIRY UA 0x%02X, retry %d\n",
                vol, asc, retry + 1);
            continue;
        }
        // Accept usable response data despite CSW failure
        if (inq->peripheral_qualifier != 3 &&
            (inq->response_data_format == 1 ||
             inq->response_data_format == 2) &&
            inq->additional_length >= 31)
        {
            DBG("MSC vol %d: INQUIRY CSW failed but response valid\n", vol);
            inquiry_ok = true;
        }
        break;
    }
    if (!inquiry_ok)
    {
        DBG("MSC vol %d: INQUIRY failed\n", vol);
        return msc_volume_failed;
    }
    DBG("MSC vol %d: %.8s %.16s rev %.4s%s\n", vol,
        inq->vendor_id, inq->product_id, inq->product_rev,
        inq->is_removable ? " (removable)" : "");

    // ---- TUR (removable media only) ----
    if (inq->is_removable)
    {
        bool tur_ok = false;
        for (int retry = 0; retry <= INIT_TUR_RETRIES_MAX; retry++)
        {
            if (!tuh_msc_mounted(dev_addr))
                return msc_volume_failed;
            if (msc_send_tur(vol, MSC_IO_TIMEOUT_MS))
            {
                tur_ok = true;
                break;
            }
            msc_do_request_sense(vol, MSC_IO_TIMEOUT_MS);
            uint8_t sk = msc_volume_sense_key[vol];
            uint8_t asc = msc_volume_sense_asc[vol];

            if (sk == SCSI_SENSE_NOT_READY && asc == 0x3A)
            {
                DBG("MSC vol %d: medium not present\n", vol);
                return msc_volume_ejected;
            }
            if (sk == SCSI_SENSE_NOT_READY)
            {
                // Try START UNIT
                DBG("MSC vol %d: not ready (%02Xh/%02Xh), START UNIT\n",
                    vol, asc, msc_volume_sense_ascq[vol]);
                tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, MSC_IO_TIMEOUT_MS);
                if (cb)
                {
                    if (tuh_msc_start_stop_unit(dev_addr, 0, true, cb, 0))
                        msc_sync_wait_io(dev_addr, MSC_IO_TIMEOUT_MS);
                    else
                        tuh_msc_sync_clear_busy(dev_addr);
                }
                continue;
            }
            if (sk == SCSI_SENSE_UNIT_ATTENTION &&
                (asc == 0x28 || asc == 0x29) &&
                retry < INIT_TUR_RETRIES_MAX)
            {
                DBG("MSC vol %d: UA 0x%02X, TUR retry %d\n",
                    vol, asc, retry + 1);
                continue;
            }
            DBG("MSC vol %d: TUR sense %d/%02Xh/%02Xh\n",
                vol, sk, asc, msc_volume_sense_ascq[vol]);
            return msc_volume_ejected;
        }
        if (!tur_ok)
            return msc_volume_ejected;
    }

    // ---- CAPACITY ----
    if (tuh_msc_is_cbi(dev_addr))
    {
        // CBI: READ FORMAT CAPACITIES
        uint8_t rfc[12];
        memset(rfc, 0, sizeof(rfc));
        tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, MSC_IO_TIMEOUT_MS);
        if (!cb)
            return msc_volume_failed;
        if (!tuh_msc_read_format_capacities(dev_addr, 0, rfc, sizeof(rfc),
                                            cb, 0))
        {
            tuh_msc_sync_clear_busy(dev_addr);
            return msc_volume_failed;
        }
        if (msc_sync_wait_io(dev_addr, MSC_IO_TIMEOUT_MS) !=
            MSC_CSW_STATUS_PASSED)
            return inq->is_removable ? msc_volume_ejected : msc_volume_failed;

        uint8_t list_len = rfc[3];
        if (list_len < 8)
            return inq->is_removable ? msc_volume_ejected : msc_volume_failed;
        uint8_t desc_type = rfc[4 + 4] & 0x03;
        if (desc_type == 3)
            return msc_volume_ejected; // no media
        uint8_t *desc0 = &rfc[4];
        uint32_t blocks = tu_ntohl(*(uint32_t *)&desc0[0]);
        uint32_t bsize = ((uint32_t)desc0[5] << 16) |
                         ((uint32_t)desc0[6] << 8) |
                         (uint32_t)desc0[7];
        DBG("MSC vol %d: RFC %lu blocks, %lu bytes/block, type %d\n",
            vol, (unsigned long)blocks, (unsigned long)bsize, desc_type);
        if (blocks == 0 || bsize != 512)
            return inq->is_removable ? msc_volume_ejected : msc_volume_failed;
        msc_volume_block_count[vol] = blocks;
        msc_volume_block_size[vol] = bsize;
        msc_volume_write_protected[vol] = false; // CBI: skip MODE SENSE
    }
    else
    {
        // BOT: READ CAPACITY 10
        scsi_read_capacity10_resp_t cap10;
        memset(&cap10, 0, sizeof(cap10));
        tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, MSC_IO_TIMEOUT_MS);
        if (!cb)
            return msc_volume_failed;
        if (!tuh_msc_read_capacity(dev_addr, 0, &cap10, cb, 0))
        {
            tuh_msc_sync_clear_busy(dev_addr);
            return msc_volume_failed;
        }
        if (msc_sync_wait_io(dev_addr, MSC_IO_TIMEOUT_MS) !=
            MSC_CSW_STATUS_PASSED)
        {
            if (inq->is_removable)
            {
                msc_do_request_sense(vol, MSC_IO_TIMEOUT_MS);
                if (msc_volume_sense_asc[vol] == 0x3A)
                {
                    DBG("MSC vol %d: capacity — medium not present\n", vol);
                    return msc_volume_ejected;
                }
            }
            return msc_volume_failed;
        }
        msc_volume_block_count[vol] = tu_ntohl(cap10.last_lba) + 1;
        msc_volume_block_size[vol] = tu_ntohl(cap10.block_size);

        // Read block 0 — forces some USB devices to populate mode
        // pages with real values (Linux read_before_ms quirk).
        // Non-fatal; data is discarded.
        uint32_t bs = msc_volume_block_size[vol];
        if (bs > 0 && bs <= 512 && tuh_msc_mounted(dev_addr))
        {
            uint8_t block0[512];
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
            cb = msc_sync_begin(dev_addr, MSC_IO_TIMEOUT_MS);
            if (cb)
            {
                if (tuh_msc_scsi_command(dev_addr, &cbw, block0, cb, 0))
                    msc_sync_wait_io(dev_addr, MSC_IO_TIMEOUT_MS);
                else
                    tuh_msc_sync_clear_busy(dev_addr);
            }
        }

        // MODE SENSE(6) — determine write protection
        scsi_mode_sense6_resp_t ms;
        memset(&ms, 0, sizeof(ms));
        cb = msc_sync_begin(dev_addr, MSC_IO_TIMEOUT_MS);
        if (cb && tuh_msc_mode_sense6(dev_addr, 0, &ms, cb, 0))
        {
            if (msc_sync_wait_io(dev_addr, MSC_IO_TIMEOUT_MS) ==
                MSC_CSW_STATUS_PASSED)
            {
                DBG("MSC vol %d: MODE SENSE WP=%d\n",
                    vol, ms.write_protected);
                msc_volume_write_protected[vol] = ms.write_protected;
            }
            else
            {
                // Fallback: MODE SENSE(6) page 0 instead of 0x3F
                DBG("MSC vol %d: MODE SENSE page 0x3F rejected, "
                    "trying page 0\n",
                    vol);
                memset(&ms, 0, sizeof(ms));
                msc_cbw_t cbw;
                msc_cbw_init(&cbw, 0);
                cbw.total_bytes = sizeof(scsi_mode_sense6_resp_t);
                cbw.dir = TUSB_DIR_IN_MASK;
                cbw.cmd_len = 6;
                cbw.command[0] = 0x1A; // MODE SENSE(6)
                cbw.command[1] = 0x08; // DBD=1
                cbw.command[2] = 0x00; // page code 0
                cbw.command[4] = sizeof(scsi_mode_sense6_resp_t);
                cb = msc_sync_begin(dev_addr, MSC_IO_TIMEOUT_MS);
                if (cb && tuh_msc_scsi_command(dev_addr, &cbw, &ms, cb, 0))
                {
                    if (msc_sync_wait_io(dev_addr, MSC_IO_TIMEOUT_MS) ==
                        MSC_CSW_STATUS_PASSED)
                    {
                        DBG("MSC vol %d: MODE SENSE page 0 WP=%d\n",
                            vol, ms.write_protected);
                        msc_volume_write_protected[vol] = ms.write_protected;
                    }
                    else
                    {
                        DBG("MSC vol %d: MODE SENSE not supported\n", vol);
                        msc_volume_write_protected[vol] = false;
                    }
                }
                else
                {
                    if (cb)
                        tuh_msc_sync_clear_busy(dev_addr);
                    msc_volume_write_protected[vol] = false;
                }
            }
        }
        else
        {
            if (cb)
                tuh_msc_sync_clear_busy(dev_addr);
            msc_volume_write_protected[vol] = false;
        }
    }

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

void msc_task(void)
{
    static absolute_time_t next_poll_time;
    bool poll_due = time_reached(next_poll_time);
    if (poll_due)
#ifdef NDEBUG
        next_poll_time = make_timeout_time_ms(250);
#else
        next_poll_time = make_timeout_time_ms(10000);
#endif

    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        switch (msc_volume_status[vol])
        {
        case msc_volume_registered:
        {
            uint8_t dev_addr = msc_volume_dev_addr[vol];
            if (dev_addr != 0 && tuh_msc_mounted(dev_addr) &&
                tuh_msc_ready(dev_addr))
            {
                msc_volume_status_t result = msc_init_volume(vol);
                msc_volume_status[vol] = result;
                if (result == msc_volume_mounted)
                {
                    TCHAR volstr[6] = MSC_VOL0;
                    volstr[3] += vol;
                    f_mount(&msc_fatfs_volumes[vol], volstr, 0);
                    DBG("MSC vol %d: f_mount registered\n", vol);
                }
                else if (result == msc_volume_ejected)
                {
                    TCHAR volstr[6] = MSC_VOL0;
                    volstr[3] += vol;
                    f_mount(&msc_fatfs_volumes[vol], volstr, 0);
                    DBG("MSC vol %d: removable, no media\n", vol);
                }
            }
            break;
        }

        case msc_volume_mounted:
            // Async TUR poll for removable media detection.
            if (poll_due && msc_inquiry_resp[vol].is_removable)
            {
                // Guard against stale poll state: if a poll callback
                // never fired (rare HCD issue), force back to IDLE so
                // polling isn't permanently stuck.
                if (msc_poll_state[vol] != POLL_IDLE &&
                    time_reached(msc_poll_deadline[vol]))
                {
                    DBG("MSC vol %d: poll state %d stale, forcing IDLE\n",
                        vol, msc_poll_state[vol]);
                    msc_poll_state[vol] = POLL_IDLE;
                }
                if (msc_poll_state[vol] == POLL_IDLE)
                {
                    uint8_t dev_addr = msc_volume_dev_addr[vol];
                    if (tuh_msc_ready(dev_addr) &&
                        tuh_msc_test_unit_ready(dev_addr, 0,
                                                msc_poll_cb, (uintptr_t)vol))
                    {
                        msc_poll_state[vol] = POLL_TUR;
                        msc_poll_deadline[vol] =
                            make_timeout_time_ms(MSC_IO_TIMEOUT_MS);
                        msc_volume_tur_ok[vol] = false;
                    }
                }
            }
            break;

        case msc_volume_ejected:
            // Deferred FatFS cleanup: unmount the stale filesystem
            // and re-register a deferred f_mount so that the next
            // FatFS access calls disk_initialize() for re-probe.
            // This must happen here (main loop) rather than from
            // disk_read/disk_write callbacks or poll callbacks,
            // because f_unmount invalidates state that FatFS may
            // be actively using in those contexts.
            if (msc_volume_needs_remount[vol])
            {
                TCHAR volstr[6] = MSC_VOL0;
                volstr[3] += vol;
                f_unmount(volstr);
                f_mount(&msc_fatfs_volumes[vol], volstr, 0);
                msc_volume_needs_remount[vol] = false;
                DBG("MSC vol %d: deferred FatFS remount\n", vol);
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
            msc_poll_state[vol] = POLL_IDLE; // cancel in-progress poll
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
            msc_volume_needs_remount[vol] = false;
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
        // For BOT: non-timeout failures (CSW phase error, etc.) need
        // halt clearing on the data endpoint.
        // For CBI: non-timeout failures are SCSI-level errors (CHECK
        // CONDITION reported via interrupt status).  USB recovery is
        // wrong here — CLEAR_FEATURE on CBI bulk endpoints generates
        // new UNIT ATTENTIONs that make subsequent re-probe fail.
        // Timeout failures are handled inside msc_sync_wait_io already.
        if (!msc_sync_timed_out && !tuh_msc_is_cbi(dev_addr))
            msc_sync_recovery(dev_addr, MSC_IO_TIMEOUT_MS);
        // For removable media, always transition to ejected state.
        // FatFS's find_volume() checks disk_status() before each
        // access — when it sees STA_NOINIT, it calls disk_initialize()
        // which runs the full UA retry loop to re-probe the device.
        // Attempting TUR+SENSE here is counterproductive: CBI recovery
        // (CLEAR_FEATURE) generates new UNIT ATTENTIONs that mask the
        // original failure and prevent the re-probe from succeeding.
        // For non-removable media, msc_handle_io_error is a no-op.
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

// Synchronous re-probe for ejected removable media.
// Reads capacity (and MODE SENSE for BOT), updates per-volume
// arrays, and transitions to msc_volume_mounted on success.
static bool msc_sync_mount_media(uint8_t vol, uint32_t timeout_ms)
{
    // TODO do timeouts like disk_initialize
    uint8_t dev_addr = msc_volume_dev_addr[vol];
    if (dev_addr == 0)
        return false;

    DBG("MSC vol %d: sync_mount_media, dev_addr=%d timeout=%lu cbi=%d\n",
        vol, dev_addr, (unsigned long)timeout_ms, tuh_msc_is_cbi(dev_addr));

    if (tuh_msc_is_cbi(dev_addr))
    {
        uint8_t rfc[12];
        memset(rfc, 0, sizeof(rfc));
        tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, timeout_ms);
        if (!cb)
        {
            DBG("MSC vol %d: sync_mount — RFC sync_begin failed\n", vol);
            return false;
        }
        if (!tuh_msc_read_format_capacities(dev_addr, 0, rfc, sizeof(rfc),
                                            cb, 0))
        {
            DBG("MSC vol %d: sync_mount — RFC submit failed\n", vol);
            tuh_msc_sync_clear_busy(dev_addr);
            return false;
        }
        uint8_t csw = msc_sync_wait_io(dev_addr, timeout_ms);
        DBG("MSC vol %d: sync_mount — RFC csw=%d timed_out=%d\n",
            vol, csw, msc_sync_timed_out);
        if (csw != MSC_CSW_STATUS_PASSED)
            return false;
        DBG("MSC vol %d: sync_mount — RFC data: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X\n",
            vol, rfc[0], rfc[1], rfc[2], rfc[3],
            rfc[4], rfc[5], rfc[6], rfc[7],
            rfc[8], rfc[9], rfc[10], rfc[11]);
        uint8_t list_len = rfc[3];
        if (list_len < 8)
        {
            DBG("MSC vol %d: sync_mount — RFC list_len=%d < 8\n", vol, list_len);
            return false;
        }
        uint8_t desc_type = rfc[4 + 4] & 0x03;
        if (desc_type == 3)
        {
            DBG("MSC vol %d: sync_mount — RFC desc_type=3 (no media)\n", vol);
            return false;
        }
        uint8_t *desc0 = &rfc[4];
        uint32_t blocks = tu_ntohl(*(uint32_t *)&desc0[0]);
        uint32_t bsize = ((uint32_t)desc0[5] << 16) |
                         ((uint32_t)desc0[6] << 8) |
                         (uint32_t)desc0[7];
        DBG("MSC vol %d: sync_mount — RFC blocks=%lu bsize=%lu desc_type=%d\n",
            vol, (unsigned long)blocks, (unsigned long)bsize, desc_type);
        if (blocks == 0 || bsize != 512)
        {
            DBG("MSC vol %d: sync_mount — RFC bad geometry\n", vol);
            return false;
        }
        msc_volume_block_count[vol] = blocks;
        msc_volume_block_size[vol] = bsize;
        msc_volume_write_protected[vol] = false; // CBI: skip MODE SENSE
    }
    else
    {
        scsi_read_capacity10_resp_t cap10;
        memset(&cap10, 0, sizeof(cap10));
        tuh_msc_complete_cb_t cb = msc_sync_begin(dev_addr, timeout_ms);
        if (!cb)
            return false;
        if (!tuh_msc_read_capacity(dev_addr, 0, &cap10, cb, 0))
        {
            tuh_msc_sync_clear_busy(dev_addr);
            return false;
        }
        if (msc_sync_wait_io(dev_addr, timeout_ms) !=
            MSC_CSW_STATUS_PASSED)
            return false;
        msc_volume_block_count[vol] = tu_ntohl(cap10.last_lba) + 1;
        msc_volume_block_size[vol] = tu_ntohl(cap10.block_size);

        // MODE SENSE(6) — failure is non-fatal.
        scsi_mode_sense6_resp_t ms;
        memset(&ms, 0, sizeof(ms));
        cb = msc_sync_begin(dev_addr, timeout_ms);
        if (cb && tuh_msc_mode_sense6(dev_addr, 0, &ms, cb, 0))
        {
            if (msc_sync_wait_io(dev_addr, timeout_ms) ==
                MSC_CSW_STATUS_PASSED)
                msc_volume_write_protected[vol] = ms.write_protected;
            else
                msc_volume_write_protected[vol] = false;
        }
        else
        {
            if (cb)
                tuh_msc_sync_clear_busy(dev_addr);
            msc_volume_write_protected[vol] = false;
        }
    }

    // No f_unmount/f_mount here — disk_initialize is called from
    // FatFS find_volume(), which already holds the FATFS object
    // registered by msc_handle_io_error().  Updating the geometry
    // and status is sufficient; FatFS will read the boot sector
    // and mount the new media itself.
    msc_volume_tur_ok[vol] = true;
    msc_volume_status[vol] = msc_volume_mounted;
    DBG("MSC vol %d: sync mount %lu blocks\n", vol,
        (unsigned long)msc_volume_block_count[vol]);
    return true;
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

    // Sync re-probe for ejected removable media.
    // After media reinsertion, devices typically report one or more
    // UNIT ATTENTION conditions (06/28 "media changed", 06/29 "reset").
    //
    // The CBI recovery chain (triggered by the I/O failure that
    // caused ejection) already sent a CBI device reset (SEND_DIAGNOSTIC
    // with SelfTest=1) and cleared endpoint halts.  The floppy needs
    // time to complete the self-test and re-detect media.
    DBG("MSC vol %d: disk_initialize, status=%d\n", pdrv, msc_volume_status[pdrv]);
    if (msc_volume_status[pdrv] == msc_volume_ejected)
    {
        uint8_t dev_addr = msc_volume_dev_addr[pdrv];
        DBG("MSC vol %d: disk_init — ejected, dev_addr=%d mounted=%d\n",
            pdrv, dev_addr, dev_addr ? tuh_msc_mounted(dev_addr) : 0);
        if (dev_addr == 0 || !tuh_msc_mounted(dev_addr))
            return STA_NOINIT | STA_NODISK;

        absolute_time_t deadline = make_timeout_time_ms(DISK_INIT_TIMEOUT_MS);
        bool tur_ok = false;
        for (int retry = 0; retry < 2; retry++)
        {
            if (!tuh_msc_mounted(dev_addr))
            {
                DBG("MSC vol %d: disk_init — dev unmounted during retry %d\n", pdrv, retry);
                return STA_NOINIT | STA_NODISK;
            }
            uint32_t remaining = stage_remaining(deadline);
            DBG("MSC vol %d: disk_init — retry %d, remaining=%lu ms\n",
                pdrv, retry, (unsigned long)remaining);
            if (remaining == 0)
            {
                DBG("MSC vol %d: disk_init — deadline expired before TUR\n", pdrv);
                break;
            }
            if (msc_send_tur(pdrv, remaining))
            {
                DBG("MSC vol %d: disk_init — TUR ok on retry %d\n", pdrv, retry);
                tur_ok = true;
                break;
            }
            DBG("MSC vol %d: disk_init — TUR failed on retry %d\n", pdrv, retry);
            // TUR failed — read sense to determine cause.
            // For CBI with interrupt endpoint: even if TUR timed out
            // (interrupt status never arrived), REQUEST_SENSE may still
            // succeed because its data phase (18 bytes on bulk-in)
            // provides the sense data before the interrupt status.
            // The sense buffer is valid even if the command "times out"
            // in the interrupt status phase.
            remaining = stage_remaining(deadline);
            if (remaining == 0)
                break;
            msc_do_request_sense(pdrv, remaining);
            uint8_t sk = msc_volume_sense_key[pdrv];
            uint8_t asc = msc_volume_sense_asc[pdrv];
            DBG("MSC vol %d: disk_init — sense %d/%02Xh/%02Xh\n",
                pdrv, sk, asc, msc_volume_sense_ascq[pdrv]);
            if (sk == SCSI_SENSE_UNIT_ATTENTION)
            {
                // Reading sense cleared the UA; retry TUR immediately
                DBG("MSC vol %d: disk_init — UA %02Xh cleared\n",
                    pdrv, asc);
                continue;
            }
            if (sk == SCSI_SENSE_NOT_READY)
            {
                // NOT READY covers both 3A (medium not present) and other
                // sub-codes (04=becoming ready, etc.).  Send START UNIT
                // (start=1) to trigger media detection.
                //
                // The key insight: if START UNIT itself STALLs (fails),
                // the floppy is definitively telling us there's no media.
                // If START UNIT succeeds, the drive accepted the command
                // and may be spinning up reinserted media — keep retrying.
                DBG("MSC vol %d: disk_init — not ready %02Xh, START UNIT\n",
                    pdrv, asc);
                bool start_ok = false;
                remaining = stage_remaining(deadline);
                DBG("MSC vol %d: disk_init — START UNIT, remaining=%lu ms\n",
                    pdrv, (unsigned long)remaining);
                tuh_msc_complete_cb_t cb = remaining
                                               ? msc_sync_begin(dev_addr, remaining)
                                               : NULL;
                if (cb)
                {
                    if (tuh_msc_start_stop_unit(dev_addr, 0, true, cb, 0))
                    {
                        uint8_t csw = msc_sync_wait_io(dev_addr,
                                                       stage_remaining(deadline));
                        start_ok = (csw == MSC_CSW_STATUS_PASSED);
                        DBG("MSC vol %d: disk_init — START UNIT csw=%d start_ok=%d\n",
                            pdrv, csw, start_ok);
                    }
                    else
                    {
                        DBG("MSC vol %d: disk_init — START UNIT submit failed\n", pdrv);
                        tuh_msc_sync_clear_busy(dev_addr);
                    }
                }
                else
                {
                    DBG("MSC vol %d: disk_init — START UNIT sync_begin failed (remaining=%lu)\n",
                        pdrv, (unsigned long)remaining);
                }
                if (!start_ok && asc == 0x3A)
                {
                    // START UNIT failed AND sense was "medium not present"
                    // — no point retrying, there's genuinely no disk.
                    // STOP UNIT won't work (drive STALLs all SCSI
                    // commands when empty).  Instead, send a CBI device
                    // reset (SEND_DIAGNOSTIC SelfTest=1) which forces
                    // the drive back to its power-on state with motor
                    // off and LED extinguished.
                    DBG("MSC vol %d: disk_init — no medium, CBI reset\n", pdrv);
                    if (tuh_msc_is_cbi(dev_addr))
                        msc_sync_recovery(dev_addr,
                                          stage_remaining(deadline));
                    break;
                }
                // Delay before retry — pump USB events so the stack
                // stays alive for this and other devices.
                {
                    remaining = ms_remaining(deadline);
                    if (remaining > DISK_INIT_DELAY_MS)
                        remaining = DISK_INIT_DELAY_MS;
                    absolute_time_t wake = make_timeout_time_ms(remaining);
                    while (absolute_time_diff_us(get_absolute_time(), wake) > 0)
                    {
                        if (!tuh_msc_mounted(dev_addr))
                            return STA_NOINIT | STA_NODISK;
                        tuh_task_device_only(dev_addr);
                    }
                }
                continue;
            }
            // Unexpected sense — bail out rather than loop forever.
            DBG("MSC vol %d: disk_init — sense %d/%02Xh, giving up\n",
                pdrv, sk, asc);
            break;
        }
        DBG("MSC vol %d: disk_init — loop done, tur_ok=%d\n", pdrv, tur_ok);
        if (!tur_ok)
            return STA_NOINIT | STA_NODISK;
        {
            uint32_t mount_remaining = stage_remaining(deadline);
            DBG("MSC vol %d: disk_init — calling sync_mount_media, remaining=%lu ms\n",
                pdrv, (unsigned long)mount_remaining);
            if (!msc_sync_mount_media(pdrv, mount_remaining))
            {
                DBG("MSC vol %d: disk_init — sync_mount_media FAILED\n", pdrv);
                return STA_NOINIT | STA_NODISK;
            }
        }
        // Fall through to return mounted status.
    }

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
