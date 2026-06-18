/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "tusb.h"
#include "class/msc/msc.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "host/hcd.h"
#include "str/str.h"
#include "sys/mem.h"
#include "usb/msc.h"
#include "usb/usb.h"
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

#define DBG_VOL(vol, fmt, ...)                                \
    DBG("MSC:%lums vol %u: " fmt,                             \
        (unsigned long)to_ms_since_boot(get_absolute_time()), \
        (unsigned)(vol),                                      \
        ##__VA_ARGS__)

#define DBG_CMD(vol, cmd, status)                                           \
    DBG_VOL(vol, cmd " (status=0x%02x sk=0x%02x asc=0x%02x ascq=0x%02x)\n", \
            (unsigned)(status),                                             \
            msc_pdrv[vol].sense_key, msc_pdrv[vol].sense_asc, msc_pdrv[vol].sense_ascq)

#define TU_LOG_DRV(...) TU_LOG(CFG_TUH_LOG_LEVEL, __VA_ARGS__)

// Superset of msc_csw_status_t with an additional timeout value.
typedef enum
{
    MSC_STATUS_PASSED,      // == MSC_CSW_STATUS_PASSED
    MSC_STATUS_FAILED,      // == MSC_CSW_STATUS_FAILED
    MSC_STATUS_PHASE_ERROR, // == MSC_CSW_STATUS_PHASE_ERROR
    MSC_STATUS_TIMED_OUT,   // returned on I/O timeout
} msc_status_t;

// File descriptor pool for open files
#define MSC_STD_FIL_MAX 8
static FIL msc_std_fil_pool[MSC_STD_FIL_MAX];

// Support up to four logical units per device
#define MSC_MAX_LUN_COUNT 4

// Timeout for read/write/sync SCSI commands and
// anything that might interact with motors.
// Needs headroom for 3.5" floppy disk drives.
#define MSC_SCSI_RW_TIMEOUT_MS 2500

// Time budget for SCSI commands which do
// not need to account for mechanical delay.
#define MSC_SCSI_OP_TIMEOUT_MS 250

// A synchronous FORMAT UNIT can take minutes on a floppy; this bounds the wait.
#define MSC_FORMAT_UNIT_TIMEOUT_MS 120000

// disk_status() issues a TUR on removable volumes only when this many
// milliseconds have elapsed since the last successful SCSI command.
// This detects media removal without adding overhead during active I/O.
#define MSC_DISK_STATUS_TIMEOUT_MS 250

// Validate essential settings from ffconf.h
static_assert(sizeof(TCHAR) == sizeof(char));
#ifdef NDEBUG
static_assert(FF_CODE_PAGE == 0);
#else
static_assert(FF_CODE_PAGE == RP6502_CODE_PAGE);
#endif
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
static_assert(FF_USE_MKFS == 1);
static_assert(FF_FS_LOCK == 8);
static_assert(FF_FS_NORTC == 0);
static_assert(FF_USE_TRIM == 1);
static_assert(FF_VOLUMES == 10);
static_assert(FF_STR_VOLUME_ID == 1);
#ifdef FF_VOLUME_STRS
#error FF_VOLUME_STRS must not be defined
#endif

// Place volume strings in flash
const char __in_flash("fatfs_vol") VolumeStrMSC0[] = "MSC0";
const char __in_flash("fatfs_vol") VolumeStrMSC1[] = "MSC1";
const char __in_flash("fatfs_vol") VolumeStrMSC2[] = "MSC2";
const char __in_flash("fatfs_vol") VolumeStrMSC3[] = "MSC3";
const char __in_flash("fatfs_vol") VolumeStrMSC4[] = "MSC4";
const char __in_flash("fatfs_vol") VolumeStrMSC5[] = "MSC5";
const char __in_flash("fatfs_vol") VolumeStrMSC6[] = "MSC6";
const char __in_flash("fatfs_vol") VolumeStrMSC7[] = "MSC7";
const char __in_flash("fatfs_vol") VolumeStrMSC8[] = "MSC8";
const char __in_flash("fatfs_vol") VolumeStrMSC9[] = "MSC9";
const char __in_flash("fatfs_vols") * VolumeStr[FF_VOLUMES] = {
    VolumeStrMSC0, VolumeStrMSC1, VolumeStrMSC2, VolumeStrMSC3,
    VolumeStrMSC4, VolumeStrMSC5, VolumeStrMSC6, VolumeStrMSC7,
    VolumeStrMSC8, VolumeStrMSC9};

// Build a FatFS volume path like "MSC0:" for volume.
void msc_vol_path(TCHAR buf[6], uint8_t vol)
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
} msc_volume_status_t;

// One volume per device LUN: vol == pdrv == slot, named MSCn: (n == slot). All
// media/geometry/sense state and the mounted FatFs object live here.
typedef struct
{
    msc_volume_status_t status;
    uint8_t dev_addr;
    uint8_t lun;
    bool removable;
    uint8_t spc_version; // 0x05=SPC-3, 0x06=SPC-4, 0x07=SPC-5
    uint64_t block_count;
    uint32_t block_size;
    uint8_t sense_key;
    uint8_t sense_asc;
    uint8_t sense_ascq;
    bool write_prot;
    bool unmap_supported;
    bool sync_cache_suppressed;
    bool lbpme;
    absolute_time_t last_ok;
    FATFS fatfs;
} msc_pdrv_t;

static msc_pdrv_t msc_pdrv[FF_VOLUMES];

enum
{
    MSC_STAGE_IDLE,
    MSC_STAGE_CMD,
    MSC_STAGE_DATA,
    MSC_STAGE_STATUS,
    MSC_STAGE_STATUS_RETRY,
};

enum
{
    RECOVERY_IDLE,
    RECOVERY_RESET,
    RECOVERY_CLEAR_IN,
    RECOVERY_CLEAR_OUT,
};

typedef struct
{
    volatile bool mounted;
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t protocol; // MSC_PROTOCOL_BOT or MSC_PROTOCOL_CBI*
    uint8_t subclass; // MSC_SUBCLASS_UFI, MSC_SUBCLASS_SFF, etc.
    uint8_t stage;
    uint8_t recovery_stage;
    void *buffer;
    uint32_t data_xferred; // bytes moved in the last data phase (host-observed)
    uint32_t cmd_xferred;  // data_xferred for the command's own data phase, before any autosense
    uint8_t max_lun;       // highest LUN index on this device (0 = single LUN)
} msc_interface_t;

typedef struct
{
    TUH_EPBUF_TYPE_DEF(msc_cbw_t, cbw);
    TUH_EPBUF_TYPE_DEF(msc_csw_t, csw);
    TUH_EPBUF_DEF(cbi_cmd, 12);    // CBI ADSC command buffer (UFI = 12 bytes)
    TUH_EPBUF_DEF(max_lun_buf, 1); // GET_MAX_LUN response (1 byte)
} msc_epbuf_t;

static msc_interface_t msc_itf[CFG_TUH_DEVICE_MAX];
CFG_TUH_MEM_SECTION static msc_epbuf_t msc_epbuf[CFG_TUH_DEVICE_MAX];

// Monotonically incrementing CBW tag for sequencing and stale-CSW detection.
static uint32_t msc_cbw_tag_counter = 0;

TU_ATTR_ALWAYS_INLINE static inline msc_interface_t *msc_get_itf(uint8_t daddr)
{
    return &msc_itf[daddr - 1];
}

TU_ATTR_ALWAYS_INLINE static inline msc_epbuf_t *msc_get_epbuf(uint8_t daddr)
{
    return &msc_epbuf[daddr - 1];
}

TU_ATTR_ALWAYS_INLINE static inline bool msc_is_bot(msc_interface_t const *p_msc)
{
    return p_msc->protocol == MSC_PROTOCOL_BOT;
}

// Resolve data endpoint from CBW direction.
TU_ATTR_ALWAYS_INLINE static inline uint8_t msc_data_ep(msc_interface_t const *p_msc,
                                                        msc_cbw_t const *cbw)
{
    return (cbw->dir & TUSB_DIR_IN_MASK) ? p_msc->ep_in : p_msc->ep_out;
}

// Fabricate a CSW and set stage to IDLE.
static void msc_complete_command(uint8_t daddr, uint8_t csw_status, uint32_t data_residue)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);
    p_msc->stage = MSC_STAGE_IDLE;
    epbuf->csw.data_residue = data_residue;
    epbuf->csw.status = csw_status;
}

// Submit data-phase transfer or complete with failure.
static void msc_start_data_phase(uint8_t daddr)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    msc_cbw_t const *cbw = &msc_get_epbuf(daddr)->cbw;
    // Reject transfers that exceed the 16-bit USB transfer length.
    // Callers must clamp transfer sizes before building the CBW.
    if (cbw->total_bytes > UINT16_MAX)
    {
        msc_complete_command(daddr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
        return;
    }
    p_msc->stage = MSC_STAGE_DATA;
    if (!usbh_edpt_xfer(daddr, msc_data_ep(p_msc, cbw), p_msc->buffer, (uint16_t)cbw->total_bytes))
    {
        msc_complete_command(daddr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
    }
}

static bool msc_mounted(uint8_t dev_addr)
{
    return msc_get_itf(dev_addr)->mounted;
}

static bool msc_ready(uint8_t dev_addr)
{
    msc_interface_t *p_msc = msc_get_itf(dev_addr);
    TU_VERIFY(p_msc->mounted);
    if (p_msc->stage != MSC_STAGE_IDLE)
        return false;
    if (p_msc->recovery_stage != RECOVERY_IDLE)
        return false;
    if (usbh_edpt_busy(dev_addr, p_msc->ep_in))
        return false;
    if (usbh_edpt_busy(dev_addr, p_msc->ep_out))
        return false;
    return true;
}

static uint8_t msc_get_maxlun(uint8_t dev_addr)
{
    return msc_get_itf(dev_addr)->max_lun;
}

static uint8_t msc_protocol(uint8_t dev_addr)
{
    return msc_get_itf(dev_addr)->protocol;
}

static msc_csw_status_t msc_csw_status(uint8_t dev_addr)
{
    return msc_get_epbuf(dev_addr)->csw.status;
}

static void msc_cancel_inflight(uint8_t dev_addr)
{
    msc_interface_t *p_msc = msc_get_itf(dev_addr);

    // Abort any control transfer this driver has in-flight on EP0 (CBI ADSC,
    // BOT reset/clear-halt). A no-op when EP0 isn't ours.
    tuh_edpt_abort_xfer(dev_addr, 0);

    tuh_edpt_abort_xfer(dev_addr, p_msc->ep_in);
    tuh_edpt_abort_xfer(dev_addr, p_msc->ep_out);

    p_msc->stage = MSC_STAGE_IDLE;
}

// Send CLEAR_FEATURE(ENDPOINT_HALT) to ep_addr on daddr.
static bool msc_clear_endpoint_halt(uint8_t daddr, uint8_t ep_addr,
                                    tuh_xfer_cb_t complete_cb, uintptr_t user_data)
{
    tusb_control_request_t const request = {
        .bmRequestType_bit = {
            .recipient = TUSB_REQ_RCPT_ENDPOINT,
            .type = TUSB_REQ_TYPE_STANDARD,
            .direction = TUSB_DIR_OUT},
        .bRequest = TUSB_REQ_CLEAR_FEATURE,
        .wValue = TUSB_REQ_FEATURE_EDPT_HALT,
        .wIndex = ep_addr,
        .wLength = 0};
    tuh_xfer_t xfer = {
        .daddr = daddr,
        .ep_addr = 0,
        .setup = &request,
        .buffer = NULL,
        .complete_cb = complete_cb,
        .user_data = user_data};
    return tuh_control_xfer(&xfer);
}

// Last-resort local clear so the transport does not remain wedged when a
// CLEAR_FEATURE request can't be queued.
static void msc_recovery_abort_to_idle(msc_interface_t *p_msc, uint8_t daddr)
{
    uint8_t const rhport = usbh_get_rhport(daddr);
    hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_in);
    hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_out);
    p_msc->recovery_stage = RECOVERY_IDLE;
}

static void msc_recovery_xfer_cb(tuh_xfer_t *xfer)
{
    uint8_t const daddr = xfer->daddr;
    msc_interface_t *p_msc = msc_get_itf(daddr);

    if (xfer->result != XFER_RESULT_SUCCESS)
    {
        uint8_t const rhport = usbh_get_rhport(daddr);
        switch (p_msc->recovery_stage)
        {
        case RECOVERY_CLEAR_IN:
            // ep_in clear-halt failed: drop its local stall, then still issue
            // the device-level clear on ep_out so it isn't left halted.
            hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_in);
            p_msc->recovery_stage = RECOVERY_CLEAR_OUT;
            if (!msc_clear_endpoint_halt(daddr, p_msc->ep_out, msc_recovery_xfer_cb, 0))
            {
                hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_out);
                p_msc->recovery_stage = RECOVERY_IDLE;
            }
            return;
        case RECOVERY_CLEAR_OUT:
            hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_out);
            p_msc->recovery_stage = RECOVERY_IDLE;
            return;
        default:
            break;
        }
        p_msc->recovery_stage = RECOVERY_CLEAR_IN;
        if (!msc_clear_endpoint_halt(daddr, p_msc->ep_in, msc_recovery_xfer_cb, 0))
            msc_recovery_abort_to_idle(p_msc, daddr);
        return;
    }

    switch (p_msc->recovery_stage)
    {
    case RECOVERY_RESET:
        p_msc->recovery_stage = RECOVERY_CLEAR_IN;
        if (!msc_clear_endpoint_halt(daddr, p_msc->ep_in, msc_recovery_xfer_cb, 0))
            msc_recovery_abort_to_idle(p_msc, daddr);
        break;

    case RECOVERY_CLEAR_IN:
    {
        uint8_t const rhport = usbh_get_rhport(daddr);
        hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_in);
        p_msc->recovery_stage = RECOVERY_CLEAR_OUT;
        if (!msc_clear_endpoint_halt(daddr, p_msc->ep_out, msc_recovery_xfer_cb, 0))
        {
            hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_out);
            p_msc->recovery_stage = RECOVERY_IDLE;
        }
        break;
    }

    case RECOVERY_CLEAR_OUT:
        hcd_edpt_clear_stall(usbh_get_rhport(daddr), daddr, p_msc->ep_out);
        p_msc->recovery_stage = RECOVERY_IDLE;
        break;

    default:
        p_msc->recovery_stage = RECOVERY_IDLE;
        break;
    }
}

// Fall back to endpoint clear-halt recovery when a reset request cannot
// be queued on EP0 (typically because the control pipe is busy).
static void msc_recovery_start_clear_halts(uint8_t daddr)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    p_msc->recovery_stage = RECOVERY_CLEAR_IN;
    if (!msc_clear_endpoint_halt(daddr, p_msc->ep_in, msc_recovery_xfer_cb, 0))
        msc_recovery_abort_to_idle(p_msc, daddr);
}

// Start async reset recovery.  Stage must be IDLE.
// msc_ready() returns false until recovery finishes.
static void msc_start_recovery(uint8_t daddr)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    if (!p_msc->ep_in)
        return;
    if (p_msc->recovery_stage != RECOVERY_IDLE)
        return;

    if (msc_is_bot(p_msc))
    {
        // BOT: Bulk-Only Mass Storage Reset, then clear halts.
        tusb_control_request_t const request = {
            .bmRequestType_bit = {
                .recipient = TUSB_REQ_RCPT_INTERFACE,
                .type = TUSB_REQ_TYPE_CLASS,
                .direction = TUSB_DIR_OUT},
            .bRequest = MSC_REQ_RESET,
            .wValue = 0,
            .wIndex = p_msc->itf_num,
            .wLength = 0};
        tuh_xfer_t xfer = {
            .daddr = daddr,
            .ep_addr = 0,
            .setup = &request,
            .buffer = NULL,
            .complete_cb = msc_recovery_xfer_cb,
            .user_data = 0};
        p_msc->recovery_stage = RECOVERY_RESET;
        if (!tuh_control_xfer(&xfer))
        {
            msc_recovery_start_clear_halts(daddr);
        }
        return;
    }

    // CBI reset: SEND_DIAGNOSTIC(SelfTest=1) via ADSC, then clear bulk endpoints.
    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);
    tu_memclr(epbuf->cbi_cmd, 12); // UFI spec: reserved CDB bytes shall be 0x00
    epbuf->cbi_cmd[0] = 0x1D;      // SEND_DIAGNOSTIC
    epbuf->cbi_cmd[1] = 0x04;      // SelfTest=1
    tusb_control_request_t const request = {
        .bmRequestType_bit = {
            .recipient = TUSB_REQ_RCPT_INTERFACE,
            .type = TUSB_REQ_TYPE_CLASS,
            .direction = TUSB_DIR_OUT},
        .bRequest = 0, // ADSC
        .wValue = 0,
        .wIndex = p_msc->itf_num,
        .wLength = 12};
    tuh_xfer_t xfer = {
        .daddr = daddr,
        .ep_addr = 0,
        .setup = &request,
        .buffer = epbuf->cbi_cmd,
        .complete_cb = msc_recovery_xfer_cb,
        .user_data = 0};
    p_msc->recovery_stage = RECOVERY_RESET;
    if (!tuh_control_xfer(&xfer))
    {
        msc_recovery_start_clear_halts(daddr);
    }
}

// Cancel any in-flight command and start async recovery, or
// force-stop an ongoing recovery that has stalled.
static void msc_abort(uint8_t daddr)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    if (!p_msc->ep_in)
        return;

    // If recovery is already in progress, force-stop it.
    if (p_msc->recovery_stage != RECOVERY_IDLE)
    {
        // Cancel the in-flight recovery request and restart recovery from a
        // clean state so we never return while transport may still be wedged.
        msc_cancel_inflight(daddr);
        p_msc->recovery_stage = RECOVERY_IDLE;
        msc_start_recovery(daddr);
        return;
    }

    // Nothing to abort.
    if (p_msc->stage == MSC_STAGE_IDLE)
        return;

    msc_cancel_inflight(daddr);
    msc_start_recovery(daddr);
}

static void msc_cbi_adsc_complete(tuh_xfer_t *xfer)
{
    uint8_t const daddr = xfer->daddr;
    msc_interface_t *p_msc = msc_get_itf(daddr);
    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);

    if (XFER_RESULT_SUCCESS != xfer->result)
    {
        msc_complete_command(daddr, MSC_CSW_STATUS_FAILED, epbuf->cbw.total_bytes);
        return;
    }

    // ADSC succeeded — run data phase if any, otherwise assume success (CB transport).
    if (epbuf->cbw.total_bytes && p_msc->buffer)
        msc_start_data_phase(daddr);
    else
        msc_complete_command(daddr, MSC_CSW_STATUS_PASSED, 0);
}

static bool msc_scsi_submit(uint8_t daddr, msc_cbw_t const *cbw, void *data)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    TU_VERIFY(p_msc->ep_in);
    TU_VERIFY(p_msc->stage == MSC_STAGE_IDLE);
    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);

    epbuf->cbw = *cbw;
    // Stamp signature and tag — callers don't need to set these.
    epbuf->cbw.signature = MSC_CBW_SIGNATURE;
    // Skip 0 on wrap: tag 0 is legal per spec but unconventional and
    // could confuse stale-CSW detection in edge cases.
    if (++msc_cbw_tag_counter == 0)
        ++msc_cbw_tag_counter;
    epbuf->cbw.tag = msc_cbw_tag_counter;
    p_msc->buffer = data;
    p_msc->stage = MSC_STAGE_CMD;

    if (msc_is_bot(p_msc))
    {
        // BOT transport
        TU_VERIFY(usbh_edpt_claim(daddr, p_msc->ep_out));

        if (!usbh_edpt_xfer(daddr, p_msc->ep_out, (uint8_t *)&epbuf->cbw, sizeof(msc_cbw_t)))
        {
            p_msc->stage = MSC_STAGE_IDLE;
            (void)usbh_edpt_release(daddr, p_msc->ep_out);
            return false;
        }

        return true;
    }

    // CBI: send CDB via ADSC (Accept Device-Specific Command) control request.
    tu_memclr(epbuf->cbi_cmd, 12);
    uint8_t cmd_len = cbw->cmd_len;
    if (cmd_len > 12)
        cmd_len = 12;
    memcpy(epbuf->cbi_cmd, cbw->command, cmd_len);

    // UFI always requires exactly 12 bytes in the ADSC data stage regardless of
    // the logical command length.  The buffer is already zero-padded to 12 bytes.
    uint8_t adsc_len = (p_msc->subclass == MSC_SUBCLASS_UFI) ? 12 : cmd_len;

    tusb_control_request_t const request = {
        .bmRequestType_bit = {
            .recipient = TUSB_REQ_RCPT_INTERFACE,
            .type = TUSB_REQ_TYPE_CLASS,
            .direction = TUSB_DIR_OUT},
        .bRequest = 0, // ADSC
        .wValue = 0,
        .wIndex = p_msc->itf_num,
        .wLength = adsc_len};

    tuh_xfer_t xfer = {
        .daddr = daddr,
        .ep_addr = 0,
        .setup = &request,
        .buffer = epbuf->cbi_cmd,
        .complete_cb = msc_cbi_adsc_complete,
        .user_data = 0};

    if (!tuh_control_xfer(&xfer))
    {
        p_msc->stage = MSC_STAGE_IDLE;
        return false;
    }
    return true;
}

bool __in_flash("msc_class_driver_init") msc_class_driver_init(void)
{
    TU_LOG_DRV("sizeof(msc_interface_t) = %u\r\n", sizeof(msc_interface_t));
    TU_LOG_DRV("sizeof(msc_epbuf_t) = %u\r\n", sizeof(msc_epbuf_t));
    tu_memclr(msc_itf, sizeof(msc_itf));
    return true;
}

// CBI (CB-only, interrupt endpoint unused) transfer-complete handler.
// Command status is inferred from the data-phase outcome.
static bool msc_cbi_xfer_cb(uint8_t dev_addr, xfer_result_t event, uint32_t xferred_bytes)
{
    msc_interface_t *p_msc = msc_get_itf(dev_addr);
    msc_cbw_t const *cbw = &msc_get_epbuf(dev_addr)->cbw;

    if (p_msc->stage != MSC_STAGE_DATA)
        return true;

    uint32_t const residue = (xferred_bytes <= cbw->total_bytes)
                                 ? cbw->total_bytes - xferred_bytes
                                 : 0;
    p_msc->data_xferred = xferred_bytes;

    // On STALL, clear the HCD-level halt so the controller can reuse the pipe.
    // The device-level CLEAR_FEATURE is issued by recovery below.
    if (event == XFER_RESULT_STALLED)
        hcd_edpt_clear_stall(usbh_get_rhport(dev_addr), dev_addr, msc_data_ep(p_msc, cbw));

    uint8_t const status = (event == XFER_RESULT_SUCCESS)
                               ? MSC_CSW_STATUS_PASSED
                               : MSC_CSW_STATUS_FAILED;
    msc_complete_command(dev_addr, status, residue);
    if (event != XFER_RESULT_SUCCESS)
        msc_start_recovery(dev_addr);
    return true;
}

// Callback following a device-level CLEAR_FEATURE(ENDPOINT_HALT) issued after a
// BOT data-phase or CSW-phase STALL.
// On success, queue CSW read; on failure, fail command and start recovery.
//   user_data == 0: first-attempt CSW read (data-phase STALL, BOT §6.6.1)
//   user_data == 1: retry CSW read         (CSW-phase STALL,  BOT §6.7.2)
static void msc_bot_clear_for_csw_cb(tuh_xfer_t *xfer)
{
    uint8_t const daddr = xfer->daddr;
    msc_interface_t *p_msc = msc_get_itf(daddr);
    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);
    msc_csw_t *csw = &epbuf->csw;
    msc_cbw_t const *cbw = &epbuf->cbw;
    if (xfer->result != XFER_RESULT_SUCCESS)
    {
        msc_complete_command(daddr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
        msc_start_recovery(daddr);
        return;
    }
    bool const is_retry = (xfer->user_data != 0);
    p_msc->stage = is_retry ? MSC_STAGE_STATUS_RETRY : MSC_STAGE_STATUS;
    if (!usbh_edpt_xfer(daddr, p_msc->ep_in, (uint8_t *)csw, (uint16_t)sizeof(msc_csw_t)))
    {
        msc_complete_command(daddr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
        msc_start_recovery(daddr);
    }
}

// BOT transfer-complete handler.
static bool msc_bot_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t event, uint32_t xferred_bytes)
{
    msc_interface_t *p_msc = msc_get_itf(dev_addr);
    msc_epbuf_t *epbuf = msc_get_epbuf(dev_addr);
    msc_cbw_t const *cbw = &epbuf->cbw;
    msc_csw_t *csw = &epbuf->csw;

    switch (p_msc->stage)
    {
    case MSC_STAGE_CMD:
        if (ep_addr != p_msc->ep_out)
            return true; // stale completion from a prior command
        if (event != XFER_RESULT_SUCCESS || xferred_bytes != sizeof(msc_cbw_t))
        {
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            msc_start_recovery(dev_addr);
            break;
        }
        if (cbw->total_bytes && p_msc->buffer)
        {
            msc_start_data_phase(dev_addr);
            break;
        }
        TU_ATTR_FALLTHROUGH;

    case MSC_STAGE_DATA:
        if (ep_addr != msc_data_ep(p_msc, cbw))
            return true; // stale completion from a prior command
        // Record host-side data-phase length so disk_read()/disk_write() can
        // reject a short transfer (total_bytes==0 falls through here from CMD).
        p_msc->data_xferred = cbw->total_bytes ? xferred_bytes : 0;
        if (event == XFER_RESULT_FAILED)
        {
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            msc_start_recovery(dev_addr);
            break;
        }
        if (event == XFER_RESULT_STALLED)
        {
            uint8_t const stalled_ep = msc_data_ep(p_msc, cbw);
            hcd_edpt_clear_stall(usbh_get_rhport(dev_addr), dev_addr, stalled_ep);
            if (msc_clear_endpoint_halt(dev_addr, stalled_ep, msc_bot_clear_for_csw_cb, 0))
            {
                p_msc->stage = MSC_STAGE_STATUS;
                break;
            }
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            msc_start_recovery(dev_addr);
            break;
        }
        // Read CSW
        p_msc->stage = MSC_STAGE_STATUS;
        if (!usbh_edpt_xfer(dev_addr, p_msc->ep_in, (uint8_t *)csw, (uint16_t)sizeof(msc_csw_t)))
        {
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            msc_start_recovery(dev_addr);
        }
        break;

    case MSC_STAGE_STATUS:
    case MSC_STAGE_STATUS_RETRY:
    {
        if (ep_addr != p_msc->ep_in)
            return true; // stale completion from a prior command
        bool should_retry = false;
        if (p_msc->stage != MSC_STAGE_STATUS_RETRY)
        {
            if (event == XFER_RESULT_SUCCESS && xferred_bytes == 0)
            {
                TU_LOG_DRV("  MSC BOT: 0-length CSW, retrying\r\n");
                should_retry = true;
            }
            else if (event == XFER_RESULT_STALLED)
            {
                TU_LOG_DRV("  MSC BOT: CSW STALL, clearing and retrying\r\n");
                hcd_edpt_clear_stall(usbh_get_rhport(dev_addr), dev_addr, p_msc->ep_in);
                if (msc_clear_endpoint_halt(dev_addr, p_msc->ep_in, msc_bot_clear_for_csw_cb, 1))
                {
                    p_msc->stage = MSC_STAGE_STATUS_RETRY;
                    break;
                }
                msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
                msc_start_recovery(dev_addr);
                break;
            }
        }

        if (should_retry)
        {
            p_msc->stage = MSC_STAGE_STATUS_RETRY;
            if (usbh_edpt_xfer(dev_addr, p_msc->ep_in, (uint8_t *)csw, (uint16_t)sizeof(msc_csw_t)))
                break;
            // Could not queue the retry read — treat as a hard transport error.
            TU_LOG_DRV("  MSC BOT: CSW retry xfer failed\r\n");
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            msc_start_recovery(dev_addr);
            break;
        }

        // Validate CSW per BOT spec §6.3
        p_msc->stage = MSC_STAGE_IDLE;
        bool csw_valid = (event == XFER_RESULT_SUCCESS &&
                          xferred_bytes == sizeof(msc_csw_t) &&
                          csw->signature == MSC_CSW_SIGNATURE &&
                          csw->tag == cbw->tag &&
                          csw->status <= MSC_CSW_STATUS_PHASE_ERROR &&
                          csw->data_residue <= cbw->total_bytes);
        if (!csw_valid)
        {
            // BOT §5.3.3: invalid CSW requires reset recovery.
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            msc_start_recovery(dev_addr);
        }
        else if (csw->status == MSC_CSW_STATUS_PHASE_ERROR)
        {
            // BOT §6.7.2: phase error requires reset recovery.
            // The raw device CSW (with PHASE_ERROR status) is preserved in
            // epbuf->csw so that msc_scsi_sync() can return
            // msc_status_phase_error to the caller.
            msc_start_recovery(dev_addr);
        }
        break;
    }

    default:
        break;
    }

    return true;
}

bool msc_class_driver_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t event, uint32_t xferred_bytes)
{
    if (msc_is_bot(msc_get_itf(dev_addr)))
        return msc_bot_xfer_cb(dev_addr, ep_addr, event, xferred_bytes);
    return msc_cbi_xfer_cb(dev_addr, event, xferred_bytes);
}

uint16_t msc_class_driver_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf, uint16_t max_len)
{
    (void)rhport;

    // Per-device state holds a single MSC interface; decline any further one
    // rather than clobbering the bound interface's endpoints.
    if (msc_get_itf(dev_addr)->ep_in)
        return 0;

    TU_VERIFY(MSC_PROTOCOL_BOT == desc_itf->bInterfaceProtocol ||
                  MSC_PROTOCOL_CBI == desc_itf->bInterfaceProtocol ||
                  MSC_PROTOCOL_CBI_NO_INTERRUPT == desc_itf->bInterfaceProtocol,
              0);

    if (desc_itf->bInterfaceProtocol == MSC_PROTOCOL_BOT)
    {
        TU_VERIFY(MSC_SUBCLASS_SCSI == desc_itf->bInterfaceSubClass, 0);
    }
    else // CBI
    {
        TU_VERIFY(MSC_SUBCLASS_UFI == desc_itf->bInterfaceSubClass ||
                      MSC_SUBCLASS_SFF == desc_itf->bInterfaceSubClass,
                  0);
    }

    // Walk descriptors to compute driver length
    uint16_t drv_len = sizeof(tusb_desc_interface_t);
    {
        uint8_t const *p = tu_desc_next(desc_itf);
        uint8_t const *end = ((uint8_t const *)desc_itf) + max_len;
        uint8_t ep_found = 0;
        while (ep_found < desc_itf->bNumEndpoints && p < end)
        {
            uint8_t len = ((tusb_desc_interface_t const *)p)->bLength;
            if (len == 0)
                break;
            if (tu_desc_type(p) == TUSB_DESC_ENDPOINT)
                ep_found++;
            drv_len = (uint16_t)(drv_len + len);
            p += len;
        }
    }
    TU_ASSERT(drv_len <= max_len, 0);

    msc_interface_t *p_msc = msc_get_itf(dev_addr);
    p_msc->protocol = desc_itf->bInterfaceProtocol;
    p_msc->subclass = desc_itf->bInterfaceSubClass;
    p_msc->max_lun = 0;

    uint8_t const *p_desc = tu_desc_next(desc_itf);
    uint8_t const *desc_end = ((uint8_t const *)desc_itf) + drv_len;
    uint8_t ep_count = 0;

    // Open bulk endpoints only. CBI interrupt endpoints are intentionally
    // ignored — the CB path infers command status from the data-phase outcome,
    // which avoids bInterval ms of latency per command.
    while (ep_count < desc_itf->bNumEndpoints && p_desc < desc_end)
    {
        if (tu_desc_type(p_desc) != TUSB_DESC_ENDPOINT)
        {
            p_desc = tu_desc_next(p_desc);
            continue;
        }
        ep_count++;
        tusb_desc_endpoint_t const *ep_desc = (tusb_desc_endpoint_t const *)p_desc;

        if (TUSB_XFER_BULK == ep_desc->bmAttributes.xfer)
        {
            TU_ASSERT(tuh_edpt_open(dev_addr, ep_desc), 0);
            if (TUSB_DIR_IN == tu_edpt_dir(ep_desc->bEndpointAddress))
                p_msc->ep_in = ep_desc->bEndpointAddress;
            else
                p_msc->ep_out = ep_desc->bEndpointAddress;
        }

        p_desc = tu_desc_next(p_desc);
    }

    TU_ASSERT(p_msc->ep_in, 0);
    TU_ASSERT(p_msc->ep_out, 0);
    p_msc->itf_num = desc_itf->bInterfaceNumber;
    return drv_len;
}

// Pumps USB events and all application tasks during blocking I/O.
// FatFs re-entry would be a problem, so main_task() never calls FatFs
// but it does call the required tuh_task().
static void msc_pump(void) { main_task(); }

// Wait for transport ready, submit command, and wait for completion.
// No autosense — used directly for REQUEST SENSE itself.
// Calls msc_pump() while spinning.
static msc_status_t msc_scsi_sync(uint8_t dev_addr, msc_cbw_t *cbw,
                                  const void *data, uint32_t timeout_ms)
{
    uint32_t const start_ms = tusb_time_millis_api();

    // Wait for transport ready AND successfully submit the command.
    // CBI transport sends the CDB via an ADSC control transfer on the
    // shared EPX.  If another control transfer is in-flight (e.g. hub
    // Get Port Status), msc_scsi_submit() returns false even though
    // the MSC layer is idle.  We must retry submission rather than
    // failing the entire command.
    for (;;)
    {
        if (!msc_mounted(dev_addr))
            return MSC_STATUS_FAILED;
        if ((tusb_time_millis_api() - start_ms) >= timeout_ms)
            return MSC_STATUS_TIMED_OUT;
        if (!msc_ready(dev_addr))
        {
            msc_pump();
            continue;
        }
        // Cast away const: transport API uses void* for both directions.
        if (msc_scsi_submit(dev_addr, cbw, (void *)(uintptr_t)data))
            break;
        // Submit failed (control pipe busy) — pump events and retry
        msc_pump();
    }
    while (!msc_ready(dev_addr))
    {
        if (!msc_mounted(dev_addr))
            return MSC_STATUS_FAILED;
        if ((tusb_time_millis_api() - start_ms) >= timeout_ms)
        {
            msc_abort(dev_addr);
            return MSC_STATUS_TIMED_OUT;
        }
        msc_pump();
    }
    // CSW validation happens in msc_bot_xfer_cb: invalid CSWs are replaced with a
    // synthesized FAILED status and reset recovery is started. A device-reported
    // PHASE_ERROR is left intact so we can return it verbatim. Arrival here
    // implies msc_ready() returned true, i.e., recovery has drained.
    return (msc_status_t)msc_csw_status(dev_addr);
}

typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code;
    uint8_t : 3;
    bool disable_block_descriptor : 1;
    uint8_t : 4;
    uint8_t page_code : 6;
    uint8_t page_control : 2;
    uint8_t subpage_code;
    uint8_t reserved[3];
    uint16_t alloc_length; // big-endian
    uint8_t control;
} scsi_mode_sense10_t;
TU_VERIFY_STATIC(sizeof(scsi_mode_sense10_t) == 10, "size is not correct");

typedef struct TU_ATTR_PACKED
{
    uint16_t data_len; // big-endian
    uint8_t medium_type;
    uint8_t : 7;
    bool write_protected : 1; // bit 7: write protect
    uint8_t long_lba_bit;
    uint8_t reserved;
    uint16_t block_descriptor_len; // big-endian
} scsi_mode_sense10_resp_t;
TU_VERIFY_STATIC(sizeof(scsi_mode_sense10_resp_t) == 8, "size is not correct");

// SBC-3 §5.15.2: READ CAPACITY(16) (SERVICE ACTION IN, opcode 0x9E, SA 0x10).
typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code;       // 0x9E
    uint8_t service_action; // 0x10
    uint8_t reserved1[8];
    uint32_t alloc_length; // big-endian
    uint8_t reserved2;
    uint8_t control;
} scsi_read_capacity16_t;
TU_VERIFY_STATIC(sizeof(scsi_read_capacity16_t) == 16, "size is not correct");

typedef struct TU_ATTR_PACKED
{
    uint32_t last_lba_hi; // big-endian, bytes 0-3
    uint32_t last_lba_lo; // big-endian, bytes 4-7
    uint32_t block_size;  // big-endian, bytes 8-11
    uint8_t prot;         // byte 12
    uint8_t lbppbe;       // byte 13
    uint8_t lbpme_byte;   // byte 14: bit 7 = LBPME, bit 6 = LBPRZ
    uint8_t reserved[17]; // bytes 15-31
} scsi_read_capacity16_resp_t;
TU_VERIFY_STATIC(sizeof(scsi_read_capacity16_resp_t) == 32, "size is not correct");

// SBC-3 §5.6: READ(16) (opcode 0x88)
typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code; // 0x88
    uint8_t flags;
    uint32_t lba_hi;      // big-endian, bytes 2-5
    uint32_t lba_lo;      // big-endian, bytes 6-9
    uint32_t block_count; // big-endian, bytes 10-13
    uint8_t group_number; // byte 14
    uint8_t control;      // byte 15
} scsi_read16_t;
TU_VERIFY_STATIC(sizeof(scsi_read16_t) == 16, "size is not correct");

// SBC-3 §5.24: WRITE(16) (opcode 0x8A)
typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code; // 0x8A
    uint8_t flags;
    uint32_t lba_hi;      // big-endian, bytes 2-5
    uint32_t lba_lo;      // big-endian, bytes 6-9
    uint32_t block_count; // big-endian, bytes 10-13
    uint8_t group_number; // byte 14
    uint8_t control;      // byte 15
} scsi_write16_t;
TU_VERIFY_STATIC(sizeof(scsi_write16_t) == 16, "size is not correct");

// SPC-4 §7.8.16: Logical Block Provisioning VPD page (page code 0xB2).
// We only need the first 6 bytes.
typedef struct TU_ATTR_PACKED
{
    uint8_t peripheral_device_type : 5;
    uint8_t peripheral_qualifier : 3;
    uint8_t page_code;    // 0xB2
    uint16_t page_length; // big-endian, min 0x0004
    uint8_t threshold_exponent;
    // Byte 5 — bit 7 = LBPU, bit 6 = LBPWS, bit 5 = LBPWS10, bits 2:0 = LBPRZ
    uint8_t lbprz : 3;
    uint8_t : 2;
    bool lbpws10 : 1;
    bool lbpws : 1;
    bool lbpu : 1;
} scsi_vpd_lbp_t;
TU_VERIFY_STATIC(sizeof(scsi_vpd_lbp_t) == 6, "size is not correct");

// SBC-3 §5.25: UNMAP command (opcode 0x42)
typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code; // 0x42
    uint8_t anchor : 1;
    uint8_t : 7;
    uint8_t reserved[4];
    uint8_t group_number : 5;
    uint8_t : 3;
    uint16_t param_list_length; // big-endian
    uint8_t control;
} scsi_unmap_t;
TU_VERIFY_STATIC(sizeof(scsi_unmap_t) == 10, "size is not correct");

// SBC-3 §5.25.2: UNMAP block descriptor
typedef struct TU_ATTR_PACKED
{
    uint32_t lba_hi;      // big-endian, upper 32 bits
    uint32_t lba_lo;      // big-endian, lower 32 bits
    uint32_t block_count; // big-endian
    uint8_t reserved[4];
} scsi_unmap_block_desc_t;
TU_VERIFY_STATIC(sizeof(scsi_unmap_block_desc_t) == 16, "size is not correct");

// SBC-3 §5.25.1: UNMAP parameter list header + one block descriptor
typedef struct TU_ATTR_PACKED
{
    uint16_t data_length;       // big-endian (total bytes - 2)
    uint16_t block_desc_length; // big-endian
    uint8_t reserved[4];
    scsi_unmap_block_desc_t desc;
} scsi_unmap_param_t;
TU_VERIFY_STATIC(sizeof(scsi_unmap_param_t) == 24, "size is not correct");

// Initialize a CBW for a volume's LUN.
// Signature and tag are stamped by msc_scsi_submit().
static inline void msc_cbw_init(msc_cbw_t *cbw, uint8_t vol,
                                uint32_t total_bytes, uint8_t dir,
                                uint8_t cmd_len, const void *cmd)
{
    memset(cbw, 0, sizeof(msc_cbw_t));
    cbw->lun = msc_pdrv[vol].lun;
    cbw->total_bytes = total_bytes;
    cbw->dir = dir;
    cbw->cmd_len = cmd_len;
    memcpy(cbw->command, cmd, cmd_len);
}

// Core SCSI helper with autosense.
// Transparently retries on UNIT ATTENTION.
static msc_status_t msc_scsi_command(uint8_t vol, msc_cbw_t *cbw,
                                     const void *data, uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    uint8_t dev_addr = msc_pdrv[vol].dev_addr;
    msc_status_t status = MSC_STATUS_FAILED;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        int64_t remaining_ms = absolute_time_diff_us(get_absolute_time(), deadline) / 1000;
        uint32_t attempt_timeout = remaining_ms > MSC_SCSI_OP_TIMEOUT_MS
                                       ? (uint32_t)remaining_ms
                                       : MSC_SCSI_OP_TIMEOUT_MS;
        status = msc_scsi_sync(dev_addr, cbw, data, attempt_timeout);
        // Capture this command's own data-phase length before the REQUEST SENSE
        // below (autosense is itself a data phase that overwrites data_xferred).
        msc_get_itf(dev_addr)->cmd_xferred = msc_get_itf(dev_addr)->data_xferred;
        if (status == MSC_STATUS_TIMED_OUT)
            return status;
        if (status == MSC_STATUS_PHASE_ERROR)
        {
            msc_pdrv[vol].sense_key = SCSI_SENSE_NONE;
            msc_pdrv[vol].sense_asc = 0;
            msc_pdrv[vol].sense_ascq = 0;
            return status;
        }
        // Only BOT carries a CSW status; CBI/CB infer it from REQUEST SENSE below.
        if (status == MSC_STATUS_PASSED && msc_protocol(dev_addr) == MSC_PROTOCOL_BOT)
        {
            msc_pdrv[vol].last_ok = get_absolute_time();
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
        remaining_ms = absolute_time_diff_us(get_absolute_time(), deadline) / 1000;
        uint32_t sense_timeout = remaining_ms > MSC_SCSI_OP_TIMEOUT_MS
                                     ? (uint32_t)remaining_ms
                                     : MSC_SCSI_OP_TIMEOUT_MS;
        msc_status_t sense_status = msc_scsi_sync(
            dev_addr, &sense_cbw, &sense_resp, sense_timeout);
        bool sense_data_valid = (sense_status == MSC_STATUS_PASSED) ||
                                (sense_status != MSC_STATUS_TIMED_OUT &&
                                 sense_resp.response_code != 0);
        if (sense_data_valid && sense_resp.response_code)
        {
            msc_pdrv[vol].sense_key = sense_resp.sense_key;
            msc_pdrv[vol].sense_asc = sense_resp.add_sense_code;
            msc_pdrv[vol].sense_ascq = sense_resp.add_sense_qualifier;
        }
        else
        {
            msc_pdrv[vol].sense_key = SCSI_SENSE_NONE;
            msc_pdrv[vol].sense_asc = 0;
            msc_pdrv[vol].sense_ascq = 0;
        }
        if (msc_protocol(dev_addr) != MSC_PROTOCOL_BOT)
        {
            // CB: sense data is the only outcome indicator (no transport status).
            // CBI: sense data overrides the interrupt status to handle recovered errors.
            if (!sense_data_valid || sense_status == MSC_STATUS_TIMED_OUT)
            {
                status = MSC_STATUS_TIMED_OUT;
            }
            else if (msc_pdrv[vol].sense_key == SCSI_SENSE_NONE ||
                     msc_pdrv[vol].sense_key == SCSI_SENSE_RECOVERED_ERROR)
            {
                status = MSC_STATUS_PASSED;
                msc_pdrv[vol].last_ok = get_absolute_time();
            }
            else
            {
                status = MSC_STATUS_FAILED;
            }
        }
        if (status == MSC_STATUS_FAILED &&
            msc_pdrv[vol].sense_key == SCSI_SENSE_UNIT_ATTENTION &&
            !time_reached(deadline))
            continue;
        return status;
    }
    return status;
}

static msc_status_t msc_scsi_inquiry(uint8_t vol,
                                     scsi_inquiry_resp_t *resp)
{
    scsi_inquiry_t const cmd = {
        .cmd_code = SCSI_CMD_INQUIRY,
        .alloc_length = sizeof(scsi_inquiry_resp_t)};
    memset(resp, 0, sizeof(*resp));
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_inquiry_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_RW_TIMEOUT_MS);
    // Per SPC-4 §6.4.1, INQUIRY is one of the few commands that executes in any
    // device state and explicitly does not clear a UNIT ATTENTION condition.
    if (msc_pdrv[vol].sense_key == SCSI_SENSE_UNIT_ATTENTION &&
        resp->response_data_format != 0)
        status = MSC_STATUS_PASSED;
    DBG_CMD(vol, "INQUIRY", status);
    return status;
}

static msc_status_t msc_scsi_test_unit_ready(uint8_t vol)
{
    scsi_test_unit_ready_t const cmd = {.cmd_code = SCSI_CMD_TEST_UNIT_READY};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, 0, TUSB_DIR_OUT, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, NULL, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "TUR", status);
    return status;
}

// UFI FORMAT UNIT (opcode 0x04) for one track/head, matching the Linux ufiformat
// tool: a whole-disk request is acked without formatting, so the medium is
// formatted one track at a time (the host loops track x head). Per the UFI spec,
// CDB byte 1 = 0x17 (FmtData=1, CmpList=0, Defect List Format=7), byte 2 = track,
// byte 8 = parameter-list length (12). The parameter list is a defect-list header
// (FOV | DCRT | STPF, with the head/side in bit 0, defect-list length 8) plus an
// 8-byte format descriptor carrying the medium geometry (a Formattable Descriptor
// from READ FORMAT CAPACITIES). The command is synchronous; completion is polled
// via REQUEST SENSE (msc_dsk_format_poll).
static msc_status_t msc_scsi_format_unit(uint8_t vol, uint8_t track, uint8_t head)
{
    uint8_t cmd[12] = {0x04, 0x17, track, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x0C, 0x00, 0x00, 0x00};
    uint32_t blocks = (uint32_t)msc_pdrv[vol].block_count;
    uint32_t bsize = msc_pdrv[vol].block_size;
    uint8_t param[12] = {
        0x00, (uint8_t)(0xB0 | head), 0x00, 0x08, // header: FOV|DCRT|STPF | side
        (uint8_t)(blocks >> 24), (uint8_t)(blocks >> 16),
        (uint8_t)(blocks >> 8), (uint8_t)blocks, // format descriptor: number of blocks
        0x00,                                    // reserved
        (uint8_t)(bsize >> 16), (uint8_t)(bsize >> 8), (uint8_t)bsize}; // block length
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(param), TUSB_DIR_OUT, sizeof(cmd), cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, param, MSC_FORMAT_UNIT_TIMEOUT_MS);
    DBG_CMD(vol, "FORMAT UNIT", status);
    return status;
}

// SCSI SANITIZE (opcode 0x48). service_action 0x02=BLOCK ERASE, 0x03=CRYPTO
// ERASE; neither carries a parameter list. With immed the command returns at
// once and the erase runs in the background (poll progress via REQUEST SENSE).
static msc_status_t msc_scsi_sanitize(uint8_t vol, uint8_t service_action, bool immed)
{
    uint8_t cmd[10] = {0x48, (uint8_t)((immed ? 0x80 : 0) | (service_action & 0x1F)),
                       0, 0, 0, 0, 0, 0, 0, 0};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, 0, TUSB_DIR_OUT, sizeof(cmd), cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, NULL, MSC_FORMAT_UNIT_TIMEOUT_MS);
    DBG_CMD(vol, "SANITIZE", status);
    return status;
}

// Raw REQUEST SENSE into an 18-byte buffer (keeps the FORMAT-progress field).
static msc_status_t msc_scsi_request_sense_raw(uint8_t vol, uint8_t resp[18])
{
    scsi_request_sense_t const cmd = {
        .cmd_code = SCSI_CMD_REQUEST_SENSE,
        .alloc_length = 18};
    memset(resp, 0, 18);
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, 18, TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    return msc_scsi_sync(msc_pdrv[vol].dev_addr, &cbw, resp, MSC_SCSI_RW_TIMEOUT_MS);
}

static msc_status_t msc_scsi_read_capacity10(uint8_t vol, scsi_read_capacity10_resp_t *resp)
{
    scsi_read_capacity10_t const cmd = {.cmd_code = SCSI_CMD_READ_CAPACITY_10};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_read_capacity10_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_OP_TIMEOUT_MS);
    DBG_CMD(vol, "READ CAPACITY(10)", status);
    return status;
}

static msc_status_t msc_scsi_read_capacity16(uint8_t vol, scsi_read_capacity16_resp_t *resp)
{
    scsi_read_capacity16_t const cmd = {
        .cmd_code = 0x9E,
        .service_action = 0x10,
        .alloc_length = tu_htonl(sizeof(scsi_read_capacity16_resp_t)),
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_read_capacity16_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_OP_TIMEOUT_MS);
    DBG_CMD(vol, "READ CAPACITY(16)", status);
    return status;
}

#if FF_LBA64
static msc_status_t msc_scsi_read16(uint8_t vol,
                                    void *buff, uint64_t lba, uint16_t block_count,
                                    uint32_t block_size)
{
    scsi_read16_t const cmd = {
        .cmd_code = 0x88,
        .lba_hi = tu_htonl((uint32_t)(lba >> 32)),
        .lba_lo = tu_htonl((uint32_t)lba),
        .block_count = tu_htonl(block_count),
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, (uint32_t)block_count * block_size, TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, buff, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "READ(16)", status);
    return status;
}

static msc_status_t msc_scsi_write16(uint8_t vol,
                                     const void *buff, uint64_t lba, uint16_t block_count,
                                     uint32_t block_size)
{
    scsi_write16_t const cmd = {
        .cmd_code = 0x8A,
        .lba_hi = tu_htonl((uint32_t)(lba >> 32)),
        .lba_lo = tu_htonl((uint32_t)lba),
        .block_count = tu_htonl(block_count),
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, (uint32_t)block_count * block_size, TUSB_DIR_OUT, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, buff, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "WRITE(16)", status);
    return status;
}
#endif // FF_LBA64

static msc_status_t msc_scsi_read_format_capacities(uint8_t vol, void *resp)
{
    scsi_read_format_capacity_t const cmd = {
        .cmd_code = SCSI_CMD_READ_FORMAT_CAPACITY,
        .alloc_length = tu_htons(sizeof(scsi_read_format_capacity_data_t))};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_read_format_capacity_data_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "READ FORMAT CAPACITIES", status);
    return status;
}

static msc_status_t msc_scsi_mode_sense6(uint8_t vol, uint8_t page_code, scsi_mode_sense6_resp_t *resp)
{
    scsi_mode_sense6_t const cmd = {
        .cmd_code = SCSI_CMD_MODE_SENSE_6,
        .disable_block_descriptor = 1,
        .page_code = page_code,
        .alloc_length = sizeof(scsi_mode_sense6_resp_t),
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_mode_sense6_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_OP_TIMEOUT_MS);
    DBG_CMD(vol, "MODE SENSE(6)", status);
    return status;
}

static msc_status_t msc_scsi_mode_sense10(uint8_t vol,
                                          uint8_t page_code, scsi_mode_sense10_resp_t *resp)
{
    scsi_mode_sense10_t const cmd = {
        .cmd_code = 0x5A,
        .disable_block_descriptor = 1,
        .page_code = page_code,
        .alloc_length = tu_htons(sizeof(scsi_mode_sense10_resp_t)),
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_mode_sense10_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_OP_TIMEOUT_MS);
    DBG_CMD(vol, "MODE SENSE(10)", status);
    return status;
}

static msc_status_t msc_scsi_sync_cache10(uint8_t vol)
{
    uint8_t cmd[10] = {0x35}; // SYNCHRONIZE CACHE (10)
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, 0, TUSB_DIR_OUT, 10, cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, NULL, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "SYNC CACHE(10)", status);
    return status;
}

static msc_status_t msc_scsi_unmap(uint8_t vol, LBA_t lba, uint32_t block_count)
{
    scsi_unmap_t const cmd = {
        .cmd_code = 0x42,
        .param_list_length = tu_htons(sizeof(scsi_unmap_param_t)),
    };
    scsi_unmap_param_t param = {
        .data_length = tu_htons(sizeof(scsi_unmap_param_t) - 2),
        .block_desc_length = tu_htons(sizeof(scsi_unmap_block_desc_t)),
        .desc = {
#if FF_LBA64
            .lba_hi = tu_htonl((uint32_t)((uint64_t)lba >> 32)),
#endif
            .lba_lo = tu_htonl((uint32_t)lba),
            .block_count = tu_htonl(block_count),
        },
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(param), TUSB_DIR_OUT, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, &param, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "UNMAP", status);
    return status;
}

static msc_status_t msc_scsi_read10(uint8_t vol,
                                    void *buff, uint32_t lba, uint16_t block_count,
                                    uint32_t block_size)
{
    scsi_read10_t const cmd = {
        .cmd_code = SCSI_CMD_READ_10,
        .lba = tu_htonl(lba),
        .block_count = tu_htons(block_count)};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, block_count * block_size, TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, buff, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "READ(10)", status);
    return status;
}

static msc_status_t msc_scsi_write10(uint8_t vol,
                                     const void *buff, uint32_t lba, uint16_t block_count,
                                     uint32_t block_size)
{
    scsi_write10_t const cmd = {
        .cmd_code = SCSI_CMD_WRITE_10,
        .lba = tu_htonl(lba),
        .block_count = tu_htons(block_count)};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, block_count * block_size, TUSB_DIR_OUT, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, buff, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "WRITE(10)", status);
    return status;
}

// Read device capacity.
// CBI: READ FORMAT CAPACITIES (UFI mandatory command).
// BOT: READ CAPACITY(16) on SPC-3+ (needed for >2TB and LBPME);
//      READ CAPACITY(10) on SPC-2 devices (mandatory SBC command).
// Returns true on success, populating block_count and block_size.
static bool msc_read_capacity(uint8_t vol)
{
    uint8_t dev_addr = msc_pdrv[vol].dev_addr;

    if (msc_protocol(dev_addr) != MSC_PROTOCOL_BOT)
    {
        // CBI: READ FORMAT CAPACITIES
        scsi_read_format_capacity_data_t rfc = {0};
        if (msc_scsi_read_format_capacities(vol, &rfc) != MSC_STATUS_PASSED)
            return false;
        if (rfc.list_length < 8 || (rfc.list_length % 8) != 0)
            return false;
        uint32_t blocks = tu_ntohl(rfc.block_num);
        uint32_t bsize = ((uint32_t)rfc.reserved2 << 16) | tu_ntohs(rfc.block_size_u16);
        if (blocks == 0 || bsize == 0 ||
            (bsize & (bsize - 1)) != 0 || bsize > 4096)
            return false;
        msc_pdrv[vol].block_count = blocks;
        msc_pdrv[vol].block_size = bsize;
        return true;
    }

    if (msc_pdrv[vol].spc_version >= 0x05)
    {
        // BOT SPC-3+: READ CAPACITY(16) — required for >2TB and LBPME.
        scsi_read_capacity16_resp_t cap16 = {0};
        if (msc_scsi_read_capacity16(vol, &cap16) != MSC_STATUS_PASSED)
            return false;
        uint64_t last_lba64 = ((uint64_t)tu_ntohl(cap16.last_lba_hi) << 32) |
                              (uint64_t)tu_ntohl(cap16.last_lba_lo);
        uint32_t bsize16 = tu_ntohl(cap16.block_size);
        if (bsize16 == 0 || (bsize16 & (bsize16 - 1)) != 0 || bsize16 > 4096)
            return false;
#if !FF_LBA64
        if (last_lba64 > UINT32_MAX)
            return false; // >2TB not supported without FF_LBA64
#endif
        msc_pdrv[vol].block_count = last_lba64 + 1;
        msc_pdrv[vol].block_size = bsize16;
        msc_pdrv[vol].lbpme = (cap16.lbpme_byte >> 7) & 1;
        DBG_VOL(vol, "READ CAPACITY(16): %llu blocks, %lu bytes/block, LBPME=%d\n",
                (unsigned long long)msc_pdrv[vol].block_count,
                (unsigned long)bsize16, msc_pdrv[vol].lbpme);
        return true;
    }

    // BOT SPC-2: READ CAPACITY(10) — mandatory SBC command.
    scsi_read_capacity10_resp_t cap10 = {0};
    if (msc_scsi_read_capacity10(vol, &cap10) != MSC_STATUS_PASSED)
        return false;
    uint32_t last_lba = tu_ntohl(cap10.last_lba);
    if (last_lba == 0xFFFFFFFF)
        return false; // >2TB sentinel; device should have advertised SPC-3+
    uint32_t bsize = tu_ntohl(cap10.block_size);
    if (bsize == 0 || (bsize & (bsize - 1)) != 0 || bsize > 4096)
        return false;
    msc_pdrv[vol].block_count = last_lba + 1;
    msc_pdrv[vol].block_size = bsize;
    return true;
}

// Determine write protection via MODE SENSE.
// CBI: MODE SENSE(10) (UFI mandatory command, opcode 0x5A).
// BOT: MODE SENSE(6) (SBC mandatory command, opcode 0x1A).
// Requests only the mode parameter header (all pages 0x3F, DBD=1) —
// write_protected lives in the header regardless of which pages follow.
// Non-fatal: defaults to not protected on failure.
static void msc_sense_write_protect(uint8_t vol)
{
    uint8_t dev_addr = msc_pdrv[vol].dev_addr;
    if (msc_protocol(dev_addr) == MSC_PROTOCOL_BOT)
    {
        scsi_mode_sense6_resp_t ms6;
        if (msc_scsi_mode_sense6(vol, 0x3F, &ms6) == MSC_STATUS_PASSED)
        {
            DBG_VOL(vol, "MODE SENSE(6) WP=%d\n", ms6.write_protected);
            msc_pdrv[vol].write_prot = ms6.write_protected;
        }
    }
    else
    {
        scsi_mode_sense10_resp_t ms10;
        if (msc_scsi_mode_sense10(vol, 0x3F, &ms10) == MSC_STATUS_PASSED)
        {
            DBG_VOL(vol, "MODE SENSE(10) WP=%d\n", ms10.write_protected);
            msc_pdrv[vol].write_prot = ms10.write_protected;
        }
    }
}

// Probe VPD page B2 to check whether the device supports SCSI UNMAP.
static bool msc_probe_unmap(uint8_t vol)
{
    scsi_inquiry_t const cmd = {
        .cmd_code = SCSI_CMD_INQUIRY,
        .reserved1 = 1, // EVPD = 1 (bit 0 of byte 1)
        .page_code = 0xB2,
        .alloc_length = sizeof(scsi_vpd_lbp_t),
    };
    scsi_vpd_lbp_t resp;
    memset(&resp, 0, sizeof(resp));
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(resp), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, &resp, MSC_SCSI_OP_TIMEOUT_MS);
    DBG_CMD(vol, "INQUIRY VPD B2", status);
    if (status != MSC_STATUS_PASSED || resp.page_code != 0xB2)
        return false;
    DBG_VOL(vol, "VPD B2: LBPU=%d\n", resp.lbpu);
    return resp.lbpu;
}

// Allocate a free physical-drive slot. Returns FF_VOLUMES if none.
static uint8_t msc_pdrv_alloc(void)
{
    for (uint8_t p = 0; p < FF_VOLUMES; p++)
        if (msc_pdrv[p].status == msc_volume_free)
            return p;
    return FF_VOLUMES;
}

static void msc_mount_cb(uint8_t dev_addr)
{
    uint8_t const max_lun = msc_get_maxlun(dev_addr);
    for (uint8_t lun = 0; lun <= max_lun; lun++)
    {
        uint8_t pdrv = msc_pdrv_alloc();
        if (pdrv == FF_VOLUMES)
        {
            DBG("MSC mount: no free pdrv for dev %d LUN %d\n", dev_addr, lun);
            break;
        }
        msc_pdrv[pdrv].dev_addr = dev_addr;
        msc_pdrv[pdrv].lun = lun;
        msc_pdrv[pdrv].status = msc_volume_registered;
        // Lazy mount only (no disk I/O here); SCSI is unsafe in this USB
        // callback. Bring-up (READ CAPACITY etc.) runs in disk_initialize on
        // first FatFs access in the task tier.
        TCHAR volstr[6];
        msc_vol_path(volstr, pdrv);
        f_mount(&msc_pdrv[pdrv].fatfs, volstr, 0);
        DBG_VOL(pdrv, "registered dev_addr %d LUN %d\n", dev_addr, lun);
    }
}

static void msc_umount_cb(uint8_t dev_addr)
{
    for (uint8_t pdrv = 0; pdrv < FF_VOLUMES; pdrv++)
    {
        if (msc_pdrv[pdrv].status == msc_volume_free ||
            msc_pdrv[pdrv].dev_addr != dev_addr)
            continue;
        TCHAR volstr[6];
        msc_vol_path(volstr, pdrv);
        f_unmount(volstr);
        memset(&msc_pdrv[pdrv], 0, sizeof(msc_pdrv[pdrv]));
        DBG_VOL(pdrv, "unmounted (dev_addr %d)\n", dev_addr);
    }
}

static void msc_get_max_lun_complete_cb(tuh_xfer_t *xfer)
{
    uint8_t daddr = xfer->daddr;
    msc_interface_t *p_msc = msc_get_itf(daddr);
    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);

    if (xfer->result == XFER_RESULT_SUCCESS)
    {
        // Clamp to MSC_MAX_LUN_COUNT-1 per BOT spec §3.2
        uint8_t ml = epbuf->max_lun_buf[0];
        if (ml >= MSC_MAX_LUN_COUNT)
            ml = MSC_MAX_LUN_COUNT - 1;
        p_msc->max_lun = ml;
    }
    // else: STALL means no LUNs beyond 0; max_lun stays 0.

    p_msc->mounted = true;
    msc_mount_cb(daddr);
    usbh_driver_set_config_complete(daddr, p_msc->itf_num);
}

bool msc_class_driver_set_config(uint8_t daddr, uint8_t itf_num)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    TU_ASSERT(p_msc->itf_num == itf_num);

    // CBI/CB: single-LUN by spec, skip GET_MAX_LUN.
    if (!msc_is_bot(p_msc))
    {
        p_msc->mounted = true;
        msc_mount_cb(daddr);
        usbh_driver_set_config_complete(daddr, p_msc->itf_num);
        return true;
    }

    // BOT: issue GET_MAX_LUN; completion fires msc_get_max_lun_complete_cb.
    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);
    epbuf->max_lun_buf[0] = 0;
    tusb_control_request_t const request = {
        .bmRequestType_bit = {
            .recipient = TUSB_REQ_RCPT_INTERFACE,
            .type = TUSB_REQ_TYPE_CLASS,
            .direction = TUSB_DIR_IN},
        .bRequest = MSC_REQ_GET_MAX_LUN,
        .wValue = 0,
        .wIndex = p_msc->itf_num,
        .wLength = 1};
    tuh_xfer_t xfer = {
        .daddr = daddr,
        .ep_addr = 0,
        .setup = &request,
        .buffer = epbuf->max_lun_buf,
        .complete_cb = msc_get_max_lun_complete_cb,
        .user_data = 0};
    if (!tuh_control_xfer(&xfer))
    {
        // Control pipe busy or error — proceed with LUN 0 only.
        p_msc->mounted = true;
        msc_mount_cb(daddr);
        usbh_driver_set_config_complete(daddr, p_msc->itf_num);
    }
    return true;
}

void msc_class_driver_close(uint8_t dev_addr)
{
    msc_interface_t *p_msc = msc_get_itf(dev_addr);
    TU_VERIFY(p_msc->ep_in, );

    TU_LOG_DRV("  MSCh close addr = %d\r\n", dev_addr);

    msc_cancel_inflight(dev_addr);
    p_msc->recovery_stage = RECOVERY_IDLE;

    tuh_edpt_close(dev_addr, p_msc->ep_in);
    tuh_edpt_close(dev_addr, p_msc->ep_out);

    if (p_msc->mounted)
        msc_umount_cb(dev_addr);

    tu_memclr(p_msc, sizeof(msc_interface_t));
    tu_memclr(msc_get_epbuf(dev_addr), sizeof(msc_epbuf_t));
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
    // We only support partition 0. One vol per physical drive.
    uint8_t vol = pdrv;
    if (msc_pdrv[vol].status != msc_volume_mounted)
    {
        DBG_VOL(vol, "disk_status, not mounted, status=%d\n", msc_pdrv[vol].status);
        return STA_NOINIT;
    }
    // Test for removed media if we haven't used the drive in a while.
    if (msc_pdrv[vol].removable &&
        time_reached(delayed_by_ms(msc_pdrv[vol].last_ok, MSC_DISK_STATUS_TIMEOUT_MS)))
    {
        DBG_VOL(vol, "disk_status, issuing TUR\n");
        if (msc_scsi_test_unit_ready(vol) == MSC_STATUS_FAILED)
        {
            uint8_t asc = msc_pdrv[vol].sense_asc;
            if (asc == 0x3A || asc == 0x28) // MEDIUM NOT PRESENT or MAY HAVE CHANGED
            {
                // Clear media state only. Device-level traits (removable,
                // spc_version) are preserved so disk_initialize skips the
                // re-INQUIRY on media re-insertion.
                msc_pdrv[vol].status = msc_volume_ejected;
                msc_pdrv[vol].block_count = 0;
                msc_pdrv[vol].block_size = 0;
                msc_pdrv[vol].write_prot = false;
                msc_pdrv[vol].sense_key = SCSI_SENSE_NONE;
                msc_pdrv[vol].sense_asc = 0;
                msc_pdrv[vol].sense_ascq = 0;
                return STA_NOINIT;
            }
        }
    }
    return msc_pdrv[vol].write_prot ? STA_PROTECT : 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    uint8_t vol = pdrv;
    DBG_VOL(vol, "disk_initialize, status=%d\n", msc_pdrv[vol].status);

    if (msc_pdrv[vol].status == msc_volume_registered ||
        msc_pdrv[vol].status == msc_volume_ejected)
    {
        // ---- INQUIRY (first mount only) ----
        if (msc_pdrv[vol].status == msc_volume_registered)
        {
            scsi_inquiry_resp_t inq;
            if (msc_scsi_inquiry(vol, &inq) != MSC_STATUS_PASSED)
                return STA_NOINIT;
            msc_pdrv[vol].removable = inq.is_removable;
            msc_pdrv[vol].spc_version = inq.version;
        }

        // ---- TUR ----
        bool tur_ok = msc_scsi_test_unit_ready(vol) == MSC_STATUS_PASSED;
        if (!tur_ok && msc_pdrv[vol].sense_key == SCSI_SENSE_NOT_READY)
            tur_ok = msc_scsi_test_unit_ready(vol) == MSC_STATUS_PASSED;
        if (!tur_ok)
        {
            if (msc_pdrv[vol].removable)
                msc_pdrv[vol].status = msc_volume_ejected;
        }
        // ---- CAPACITY ----
        else if (msc_read_capacity(vol))
        {
            // ---- WRITE PROTECTION ----
            msc_sense_write_protect(vol);
            // ---- UNMAP SUPPORT ----
            if (!msc_pdrv[vol].write_prot &&
                msc_protocol(msc_pdrv[vol].dev_addr) == MSC_PROTOCOL_BOT &&
                msc_pdrv[vol].lbpme)
                msc_pdrv[vol].unmap_supported = msc_probe_unmap(vol);
            msc_pdrv[vol].status = msc_volume_mounted;
        }
    }

    if (msc_pdrv[vol].status == msc_volume_ejected)
        return STA_NODISK;

    if (msc_pdrv[vol].status != msc_volume_mounted)
        return STA_NOINIT;

    return msc_pdrv[vol].write_prot ? STA_PROTECT : 0;
}

static DRESULT msc_status_to_dresult(uint8_t vol, msc_status_t status)
{
    if (status == MSC_STATUS_PASSED)
        return RES_OK;
    if (status == MSC_STATUS_TIMED_OUT)
        return RES_NOTRDY;
    uint8_t sk = msc_pdrv[vol].sense_key;
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
    uint32_t const block_size = msc_pdrv[vol].block_size;
    uint8_t const dev_addr = msc_pdrv[vol].dev_addr;
    if (block_size == 0 || dev_addr == 0)
        return RES_NOTRDY;
    // Clamp each transfer so total_bytes fits the USB host transfer
    // length limit (uint16_t).
    uint16_t const max_blocks = (uint16_t)(UINT16_MAX / block_size);
    while (count > 0)
    {
        uint16_t n = (count > max_blocks) ? max_blocks : (uint16_t)count;
        msc_status_t status;
#if FF_LBA64
        if (sector > UINT32_MAX)
            status = msc_scsi_read16(vol, buff, (uint64_t)sector, n, block_size);
        else
#endif
            status = msc_scsi_read10(vol, buff, (uint32_t)sector, n, block_size);
        if (status != MSC_STATUS_PASSED)
            return msc_status_to_dresult(vol, status);
        if (msc_get_itf(dev_addr)->cmd_xferred != (uint32_t)n * block_size)
            return RES_ERROR;
        buff += (uint32_t)n * block_size;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    uint8_t vol = pdrv;
    if (msc_pdrv[vol].write_prot)
        return RES_WRPRT;
    uint32_t const block_size = msc_pdrv[vol].block_size;
    uint8_t const dev_addr = msc_pdrv[vol].dev_addr;
    if (block_size == 0 || dev_addr == 0)
        return RES_NOTRDY;
    uint16_t const max_blocks = (uint16_t)(UINT16_MAX / block_size);
    while (count > 0)
    {
        uint16_t n = (count > max_blocks) ? max_blocks : (uint16_t)count;
        msc_status_t status;
#if FF_LBA64
        if (sector > UINT32_MAX)
            status = msc_scsi_write16(vol, buff, (uint64_t)sector, n, block_size);
        else
#endif
            status = msc_scsi_write10(vol, buff, (uint32_t)sector, n, block_size);
        if (status != MSC_STATUS_PASSED)
            return msc_status_to_dresult(vol, status);
        // A short OUT data phase means not all blocks were written.
        if (msc_get_itf(dev_addr)->cmd_xferred != (uint32_t)n * block_size)
            return RES_ERROR;
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
        if (msc_pdrv[vol].dev_addr == 0)
            return RES_NOTRDY;
        if (msc_pdrv[vol].write_prot)
            return RES_OK;
        if (msc_protocol(msc_pdrv[vol].dev_addr) != MSC_PROTOCOL_BOT)
            return RES_OK;
        if (msc_pdrv[vol].sync_cache_suppressed)
            return RES_OK;
        msc_status_t status = msc_scsi_sync_cache10(vol);
        if (status == MSC_STATUS_FAILED &&
            msc_pdrv[vol].sense_key == SCSI_SENSE_ILLEGAL_REQUEST)
        {
            msc_pdrv[vol].sync_cache_suppressed = true;
            return RES_OK;
        }
        return msc_status_to_dresult(vol, status);
    }
    case GET_SECTOR_COUNT:
#if FF_LBA64
        *((LBA_t *)buff) = msc_pdrv[vol].block_count;
#else
        *((DWORD *)buff) = (DWORD)msc_pdrv[vol].block_count;
#endif
        return RES_OK;
    case GET_SECTOR_SIZE:
        *((WORD *)buff) = (WORD)msc_pdrv[vol].block_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        // 1 = erase block size unknown; FatFs treats as single-sector alignment.
        *((DWORD *)buff) = 1;
        return RES_OK;
    case CTRL_TRIM:
    {
        if (!msc_pdrv[vol].unmap_supported)
            return RES_OK;
        LBA_t *rt = (LBA_t *)buff;
        LBA_t start = rt[0];
        LBA_t end = rt[1];
        if (start > end)
            return RES_PARERR;
        // UNMAP block descriptor block_count is 32-bit.
        if ((end - start) >= (LBA_t)UINT32_MAX)
            return RES_PARERR;
        msc_status_t status = msc_scsi_unmap(vol, start, (uint32_t)(end - start + 1));
        return status == MSC_STATUS_TIMED_OUT ? RES_NOTRDY : RES_OK;
    }
    default:
        return RES_PARERR;
    }
}

// Remount after format/zero so the next access re-reads the new (or, after
// zero, absent) filesystem.
void msc_dsk_reenumerate(uint8_t pdrv)
{
    if (pdrv >= FF_VOLUMES || msc_pdrv[pdrv].status == msc_volume_free)
        return;
    TCHAR volstr[6];
    msc_vol_path(volstr, pdrv);
    f_unmount(volstr);
    f_mount(&msc_pdrv[pdrv].fatfs, volstr, 0);
}

int msc_status_count(void)
{
    int count = 0;
    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
        if (msc_pdrv[vol].status != msc_volume_free)
            count++;
    return count;
}

// Some vendors pad their strings with spaces, others with zeros.
// This will ensure zeros, which prints better.
static void msc_inquiry_rtrim(uint8_t *s, size_t l)
{
    while (l--)
    {
        if (s[l] == ' ')
            s[l] = '\0';
        else
            break;
    }
}

int msc_status_response(char *buf, size_t buf_size, int state)
{
    if (state < 0 || state >= FF_VOLUMES)
        return -1;
    uint8_t vol = state;
    if (msc_pdrv[vol].status != msc_volume_free)
    {
        uint8_t pdrv = vol;
        // Refresh or init media status
        if (disk_status(pdrv) == STA_NOINIT)
            disk_initialize(pdrv);

        char sizebuf[24];
        if (msc_pdrv[pdrv].status != msc_volume_mounted)
            snprintf(sizebuf, sizeof(sizebuf), "%s", S(STR_PARENS_NO_MEDIA));
        else
            str_size((uint64_t)msc_pdrv[pdrv].block_count * msc_pdrv[pdrv].block_size,
                     sizebuf, sizeof(sizebuf));
        scsi_inquiry_resp_t inq;
        msc_status_t status = msc_scsi_inquiry(pdrv, &inq);
        if (status == MSC_STATUS_PASSED)
        {
            msc_inquiry_rtrim(inq.vendor_id, 8);
            msc_inquiry_rtrim(inq.product_id, 16);
            msc_inquiry_rtrim(inq.product_rev, 4);
            snprintf_utf8(buf, buf_size, STR_STATUS_MSC,
                          VolumeStr[vol],
                          sizebuf,
                          inq.vendor_id,
                          inq.product_id,
                          inq.product_rev);
        }
        else
        {
            snprintf_utf8(buf, buf_size, STR_STATUS_MSC,
                          VolumeStr[vol],
                          sizebuf,
                          S(STR_PARENS_NONE), S(STR_PARENS_NONE), "");
        }
    }
    return state + 1;
}

/* ---- Disk utility (mon/dsk.c) support -------------------------------------
 * These resolve a logical volume (MSCn:) to its physical drive and expose
 * device/geometry/format primitives. All run in the FatFs-safe task tier.
 */

// Map an "MSCn"/"MSCn:" name or the "n"/"n:" shortcut (case-insensitive) to a
// logical volume index, or -1. The volume need not be in use; callers validate
// with msc_dsk_get_info.
int msc_dsk_vol_from_name(const char *name)
{
    char buf[6];
    size_t n = 0;
    while (name[n] && name[n] != ':' && n < sizeof(buf) - 1)
    {
        buf[n] = name[n];
        n++;
    }
    buf[n] = '\0';
    for (uint8_t v = 0; v < FF_VOLUMES; v++)
        if (strcasecmp(buf, VolumeStr[v]) == 0)
            return v;
    // "n:"/"n" shortcut for MSCn (same as FatFs's numeric volume IDs).
    if (n == 1 && buf[0] >= '0' && buf[0] <= '9' && (buf[0] - '0') < FF_VOLUMES)
        return buf[0] - '0';
    return -1;
}

bool msc_dsk_pdrv_of_vol(uint8_t vol, uint8_t *pdrv)
{
    if (vol >= FF_VOLUMES || msc_pdrv[vol].status == msc_volume_free)
        return false;
    *pdrv = vol;
    return true;
}

bool msc_dsk_get_info(uint8_t vol, msc_dsk_info_t *out)
{
    uint8_t pdrv;
    if (!msc_dsk_pdrv_of_vol(vol, &pdrv))
        return false;
    if (disk_status(pdrv) == STA_NOINIT)
        disk_initialize(pdrv);
    msc_interface_t *p_msc = msc_get_itf(msc_pdrv[pdrv].dev_addr);
    out->present = (msc_pdrv[pdrv].status == msc_volume_mounted);
    out->removable = msc_pdrv[pdrv].removable;
    out->write_prot = msc_pdrv[pdrv].write_prot;
    out->is_floppy = !msc_is_bot(p_msc) ||
                     p_msc->subclass == MSC_SUBCLASS_UFI ||
                     p_msc->subclass == MSC_SUBCLASS_SFF;
    out->block_count = msc_pdrv[pdrv].block_count;
    out->block_size = msc_pdrv[pdrv].block_size;
    return true;
}

bool msc_dsk_inquiry_strings(uint8_t vol, char vendor[9], char product[17], char rev[5])
{
    uint8_t pdrv;
    if (!msc_dsk_pdrv_of_vol(vol, &pdrv))
        return false;
    scsi_inquiry_resp_t inq;
    if (msc_scsi_inquiry(pdrv, &inq) != MSC_STATUS_PASSED)
        return false;
    msc_inquiry_rtrim(inq.vendor_id, 8);
    msc_inquiry_rtrim(inq.product_id, 16);
    msc_inquiry_rtrim(inq.product_rev, 4);
    memcpy(vendor, inq.vendor_id, 8);
    vendor[8] = '\0';
    memcpy(product, inq.product_id, 16);
    product[16] = '\0';
    memcpy(rev, inq.product_rev, 4);
    rev[4] = '\0';
    return true;
}

bool msc_dsk_serial(uint8_t vol, char *dst, size_t dst_size)
{
    uint8_t pdrv;
    if (!msc_dsk_pdrv_of_vol(vol, &pdrv) || dst_size == 0)
        return false;
    const void *s = usb_string_fetch_serial(msc_pdrv[pdrv].dev_addr);
    if (!s)
        return false;
    usb_desc_string_to_oem(s, USB_DESC_STRING_BUF_SIZE, dst, dst_size);
    return dst[0] != '\0';
}

bool msc_dsk_read(uint8_t vol, void *buf, uint64_t lba, uint32_t count)
{
    uint8_t pdrv;
    if (!msc_dsk_pdrv_of_vol(vol, &pdrv))
        return false;
    return disk_read(pdrv, (BYTE *)buf, (LBA_t)lba, count) == RES_OK;
}

bool msc_dsk_write(uint8_t vol, const void *buf, uint64_t lba, uint32_t count)
{
    uint8_t pdrv;
    if (!msc_dsk_pdrv_of_vol(vol, &pdrv))
        return false;
    return disk_write(pdrv, (const BYTE *)buf, (LBA_t)lba, count) == RES_OK;
}

// Start a FORMAT UNIT for one track/head. 0=started (poll with msc_dsk_format_poll
// until that track finishes), -1=error/refused.
int msc_dsk_format_start(uint8_t vol, uint8_t track, uint8_t head)
{
    uint8_t pdrv;
    if (!msc_dsk_pdrv_of_vol(vol, &pdrv))
        return -1;
    if (msc_pdrv[pdrv].write_prot)
        return -1;
    if (msc_scsi_format_unit(pdrv, track, head) == MSC_STATUS_PASSED)
        return 0;
    // A track format still running answers the autosense REQUEST SENSE with NOT
    // READY / FORMAT IN PROGRESS (04/04); that is "started", not an error.
    if (msc_pdrv[pdrv].sense_key == SCSI_SENSE_NOT_READY &&
        msc_pdrv[pdrv].sense_asc == 0x04 && msc_pdrv[pdrv].sense_ascq == 0x04)
        return 0;
    return -1;
}

// Poll background FORMAT UNIT progress. -1=error/failed, 0..99=percent,
// 100=complete. "Format in progress" (NOT READY 04/04) reports the SKSV percent;
// once that clears the format only succeeded if the unit is actually ready.
int msc_dsk_format_poll(uint8_t vol)
{
    uint8_t pdrv;
    if (!msc_dsk_pdrv_of_vol(vol, &pdrv))
        return -1;
    uint8_t s[18];
    if (msc_scsi_request_sense_raw(pdrv, s) == MSC_STATUS_TIMED_OUT)
        return -1;
    if ((s[2] & 0x0F) == SCSI_SENSE_NOT_READY && s[12] == 0x04 && s[13] == 0x04)
    {
        if (s[15] & 0x80) // SKSV: progress indication valid
        {
            uint32_t prog = ((uint32_t)s[16] << 8) | s[17];
            int pct = (int)((prog * 100) / 65536);
            return pct > 99 ? 99 : pct;
        }
        return 0;
    }
    return msc_scsi_test_unit_ready(pdrv) == MSC_STATUS_PASSED ? 100 : -1;
}

// Start a background SANITIZE (IMMED), trying crypto erase then block erase.
// 0=crypto started, 1=block started, -1=no sanitize support (caller overwrites),
// -2=cannot proceed.
int msc_dsk_sanitize_start(uint8_t vol)
{
    uint8_t pdrv;
    if (!msc_dsk_pdrv_of_vol(vol, &pdrv))
        return -2;
    if (msc_pdrv[pdrv].write_prot)
        return -2;
    // Crypto erase is instant on self-encrypting devices; block erase physically
    // clears all NAND. A rejected service action leaves device state unchanged.
    if (msc_scsi_sanitize(pdrv, 0x03, true) == MSC_STATUS_PASSED)
        return 0;
    if (msc_scsi_sanitize(pdrv, 0x02, true) == MSC_STATUS_PASSED)
        return 1;
    return -1;
}

// Poll SANITIZE progress. -1=error, 0..99=percent, 100=complete.
int msc_dsk_sanitize_poll(uint8_t vol)
{
    uint8_t pdrv;
    if (!msc_dsk_pdrv_of_vol(vol, &pdrv))
        return -1;
    uint8_t s[18];
    if (msc_scsi_request_sense_raw(pdrv, s) == MSC_STATUS_TIMED_OUT)
        return -1;
    // NOT READY + ASC/ASCQ 0x04/0x1B == "sanitize in progress".
    if ((s[2] & 0x0F) == SCSI_SENSE_NOT_READY && s[12] == 0x04 && s[13] == 0x1B)
    {
        if (s[15] & 0x80) // SKSV: progress indication valid
        {
            uint32_t prog = ((uint32_t)s[16] << 8) | s[17];
            int pct = (int)((prog * 100) / 65536);
            return pct > 99 ? 99 : pct;
        }
        return 0;
    }
    return 100;
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
    // Low two bits of the public `flags` mirror FA_READ/FA_WRITE on purpose
    // so `flags & RDWR` passes straight through to f_open().
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

std_rw_result msc_std_close(int desc, api_errno *err)
{
    FIL *fp = msc_std_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    FRESULT fresult = f_close(fp);
    fp->obj.fs = NULL;
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return STD_ERROR;
    }
    return STD_OK;
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

std_rw_result msc_std_sync(int desc, api_errno *err)
{
    FIL *fp = msc_std_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    FRESULT fresult = f_sync(fp);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}
