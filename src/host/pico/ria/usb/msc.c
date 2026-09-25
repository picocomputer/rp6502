/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/oem.h"
#include "core/sys/sys.h"
#include "tusb.h"
#include "class/msc/msc.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "host/hcd.h"
#include "core/str/str.h"
#include "ria/sys/com.h"
#include "ria/usb/msc.h"
#include "ria/usb/usb.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "core/sys/debug_log.h"
#include "pico/aon_timer.h"
#include <stdio.h>
#include <string.h>
#include "pico/time.h"

#define MSC_LOG_VOL(vol, LEVEL, fmt, ...) \
    RP6502_LOG(msc, LEVEL, "vol %u: " fmt, (unsigned)(vol), ##__VA_ARGS__)

#define MSC_LOG_CMD(vol, cmd, status)                                                  \
    MSC_LOG_VOL(vol, DEBUG, cmd " (status=0x%02x sk=0x%02x asc=0x%02x ascq=0x%02x)", \
                (unsigned)(status),                                                    \
                msc_pdrv[vol].sense_key, msc_pdrv[vol].sense_asc, msc_pdrv[vol].sense_ascq)

#define TU_LOG_DRV(...) TU_LOG(CFG_TUH_LOG_LEVEL, __VA_ARGS__)

typedef enum
{
    MSC_STATUS_PASSED,
    MSC_STATUS_FAILED,
    MSC_STATUS_PHASE_ERROR,
    MSC_STATUS_TIMED_OUT,
} msc_status_t;
// msc_scsi_sync casts a CSW status to msc_status_t, so these values must match.
static_assert((int)MSC_STATUS_PASSED == (int)MSC_CSW_STATUS_PASSED);
static_assert((int)MSC_STATUS_FAILED == (int)MSC_CSW_STATUS_FAILED);
static_assert((int)MSC_STATUS_PHASE_ERROR == (int)MSC_CSW_STATUS_PHASE_ERROR);

// Support up to four logical units per device
#define MSC_MAX_LUN_COUNT 4

// Timeout for read/write/sync SCSI commands and
// anything that might interact with motors.
// Needs headroom for 3.5" floppy disk drives.
#define MSC_SCSI_RW_TIMEOUT_MS 2500

// Time budget for SCSI commands which do
// not need to account for mechanical delay.
#define MSC_SCSI_OP_TIMEOUT_MS 250

// disk_status() issues a TUR on removable volumes only when this many
// milliseconds have elapsed since the last successful SCSI command.
// This detects media removal without adding overhead during active I/O.
#define MSC_DISK_STATUS_TIMEOUT_MS 250

// Validate essential settings from ffconf.h
static_assert(sizeof(TCHAR) == sizeof(char));
static_assert(FF_CODE_PAGE == 0);
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
static_assert(FF_FS_LOCK == 0);
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
static void msc_vol_path(TCHAR buf[6], uint8_t vol)
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

// The mount generation of a slot is incremented each time msc_mount_cb registers
// a volume in it and each time msc_umount_cb frees it, so mon/drive.c can detect
// a slot that a different device reused during a confirmation prompt. It is kept
// out of msc_pdrv because msc_umount_cb clears the slot with memset.
static uint8_t msc_mount_gen[FF_VOLUMES];

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
    uint8_t protocol;
    uint8_t subclass;
    uint8_t stage;
    uint8_t recovery_stage;
    bool cancelling;
    void *buffer;
    uint32_t data_xferred;
    uint32_t cmd_xferred;
    uint8_t max_lun;
} msc_interface_t;

typedef struct
{
    TUH_EPBUF_TYPE_DEF(msc_cbw_t, cbw);
    TUH_EPBUF_TYPE_DEF(msc_csw_t, csw);
    TUH_EPBUF_DEF(cbi_cmd, 12);
    TUH_EPBUF_DEF(max_lun_buf, 1);
} msc_epbuf_t;

static msc_interface_t msc_itf[CFG_TUH_DEVICE_MAX];
CFG_TUH_MEM_SECTION static msc_epbuf_t msc_epbuf[CFG_TUH_DEVICE_MAX];

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

TU_ATTR_ALWAYS_INLINE static inline uint8_t msc_data_ep(msc_interface_t const *p_msc,
                                                        msc_cbw_t const *cbw)
{
    return (cbw->dir & TUSB_DIR_IN_MASK) ? p_msc->ep_in : p_msc->ep_out;
}

// msc_scsi_sync reads the status written here. The residue is left unset because
// a residue is read only from a CSW that the device sent.
static void msc_complete_command(uint8_t daddr, uint8_t csw_status)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);
    p_msc->stage = MSC_STAGE_IDLE;
    epbuf->csw.status = csw_status;
}

static void msc_start_data_phase(uint8_t daddr)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    msc_cbw_t const *cbw = &msc_get_epbuf(daddr)->cbw;
    if (cbw->total_bytes > UINT16_MAX)
    {
        msc_complete_command(daddr, MSC_CSW_STATUS_FAILED);
        return;
    }
    p_msc->stage = MSC_STAGE_DATA;
    if (!usbh_edpt_xfer(daddr, msc_data_ep(p_msc, cbw), p_msc->buffer, (uint16_t)cbw->total_bytes))
    {
        msc_complete_command(daddr, MSC_CSW_STATUS_FAILED);
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

    // tuh_edpt_abort_xfer on EP0 aborts the control transfer in flight to this
    // device, if there is one, and calls its complete_cb synchronously with
    // XFER_RESULT_ABORTED. msc_recovery_xfer_cb, msc_cbi_adsc_complete and
    // msc_get_max_lun_complete_cb return early while cancelling is set, so an
    // abort of one of their transfers does not advance recovery, complete the
    // command or register volumes.
    p_msc->cancelling = true;
    tuh_edpt_abort_xfer(dev_addr, 0);
    p_msc->cancelling = false;

    tuh_edpt_abort_xfer(dev_addr, p_msc->ep_in);
    tuh_edpt_abort_xfer(dev_addr, p_msc->ep_out);

    p_msc->stage = MSC_STAGE_IDLE;
}

static bool msc_clear_endpoint_halt(uint8_t daddr, uint8_t ep_addr,
                                    tuh_xfer_cb_t complete_cb, uintptr_t user_data)
{
    if (tu_edpt_number(ep_addr) == 0)
        return false;
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
    if (p_msc->cancelling)
        return;

    uint8_t const rhport = usbh_get_rhport(daddr);

    switch (p_msc->recovery_stage)
    {
    case RECOVERY_RESET:
        p_msc->recovery_stage = RECOVERY_CLEAR_IN;
        if (!msc_clear_endpoint_halt(daddr, p_msc->ep_in, msc_recovery_xfer_cb, 0))
            msc_recovery_abort_to_idle(p_msc, daddr);
        return;

    case RECOVERY_CLEAR_IN:
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

    if (xfer->result != XFER_RESULT_SUCCESS)
    {
        p_msc->recovery_stage = RECOVERY_CLEAR_IN;
        if (!msc_clear_endpoint_halt(daddr, p_msc->ep_in, msc_recovery_xfer_cb, 0))
            msc_recovery_abort_to_idle(p_msc, daddr);
    }
    else
    {
        p_msc->recovery_stage = RECOVERY_IDLE;
    }
}

static void msc_recovery_start_clear_halts(uint8_t daddr)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    p_msc->recovery_stage = RECOVERY_CLEAR_IN;
    if (!msc_clear_endpoint_halt(daddr, p_msc->ep_in, msc_recovery_xfer_cb, 0))
        msc_recovery_abort_to_idle(p_msc, daddr);
}

static void msc_start_recovery(uint8_t daddr)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    if (!p_msc->ep_in)
        return;
    if (p_msc->recovery_stage != RECOVERY_IDLE)
        return;

    if (msc_is_bot(p_msc))
    {
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

    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);
    tu_memclr(epbuf->cbi_cmd, 12);
    epbuf->cbi_cmd[0] = 0x1D; // SEND_DIAGNOSTIC
    epbuf->cbi_cmd[1] = 0x04; // SelfTest=1
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

static void msc_abort(uint8_t daddr)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    if (!p_msc->ep_in)
        return;

    if (p_msc->recovery_stage != RECOVERY_IDLE)
    {
        msc_cancel_inflight(daddr);
        p_msc->recovery_stage = RECOVERY_IDLE;
        msc_start_recovery(daddr);
        return;
    }

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
    if (p_msc->cancelling)
        return;

    if (XFER_RESULT_SUCCESS != xfer->result)
    {
        msc_complete_command(daddr, MSC_CSW_STATUS_FAILED);
        return;
    }

    if (epbuf->cbw.total_bytes && p_msc->buffer)
        msc_start_data_phase(daddr);
    else
        msc_complete_command(daddr, MSC_CSW_STATUS_PASSED);
}

static bool msc_scsi_submit(uint8_t daddr, msc_cbw_t const *cbw, void *data)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    TU_VERIFY(p_msc->ep_in);
    TU_VERIFY(p_msc->stage == MSC_STAGE_IDLE);
    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);

    epbuf->cbw = *cbw;
    epbuf->cbw.signature = MSC_CBW_SIGNATURE;
    if (++msc_cbw_tag_counter == 0)
        ++msc_cbw_tag_counter;
    epbuf->cbw.tag = msc_cbw_tag_counter;
    p_msc->buffer = data;
    p_msc->stage = MSC_STAGE_CMD;

    if (msc_is_bot(p_msc))
    {
        TU_VERIFY(usbh_edpt_claim(daddr, p_msc->ep_out));

        if (!usbh_edpt_xfer(daddr, p_msc->ep_out, (uint8_t *)&epbuf->cbw, sizeof(msc_cbw_t)))
        {
            p_msc->stage = MSC_STAGE_IDLE;
            (void)usbh_edpt_release(daddr, p_msc->ep_out);
            return false;
        }

        return true;
    }

    tu_memclr(epbuf->cbi_cmd, 12);
    uint8_t cmd_len = cbw->cmd_len;
    if (cmd_len > 12)
        cmd_len = 12;
    memcpy(epbuf->cbi_cmd, cbw->command, cmd_len);

    // A UFI command block in the ADSC data stage is always 12 bytes, so a
    // shorter command is sent zero-padded.
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

static bool msc_cbi_xfer_cb(uint8_t dev_addr, xfer_result_t event, uint32_t xferred_bytes)
{
    msc_interface_t *p_msc = msc_get_itf(dev_addr);
    msc_cbw_t const *cbw = &msc_get_epbuf(dev_addr)->cbw;

    if (p_msc->stage != MSC_STAGE_DATA)
        return true;

    p_msc->data_xferred = xferred_bytes;

    uint8_t const status = (event == XFER_RESULT_SUCCESS)
                               ? MSC_CSW_STATUS_PASSED
                               : MSC_CSW_STATUS_FAILED;
    msc_complete_command(dev_addr, status);

    // msc_scsi_command issues REQUEST SENSE after every CB or CBI command that
    // completes. After a STALL the halts are cleared on the host and the device so
    // that REQUEST SENSE can run. Reset recovery is not started because it sends
    // SEND DIAGNOSTIC, and any command sent before REQUEST SENSE discards the
    // pending sense data. A transport that stops responding after a command is
    // submitted is reset by msc_abort when msc_scsi_sync times out.
    if (event == XFER_RESULT_STALLED)
    {
        hcd_edpt_clear_stall(usbh_get_rhport(dev_addr), dev_addr, msc_data_ep(p_msc, cbw));
        msc_recovery_start_clear_halts(dev_addr);
    }
    return true;
}

static void msc_bot_clear_for_csw_cb(tuh_xfer_t *xfer)
{
    uint8_t const daddr = xfer->daddr;
    msc_interface_t *p_msc = msc_get_itf(daddr);
    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);
    msc_csw_t *csw = &epbuf->csw;
    if (xfer->result != XFER_RESULT_SUCCESS)
    {
        msc_complete_command(daddr, MSC_CSW_STATUS_FAILED);
        msc_start_recovery(daddr);
        return;
    }
    bool const is_retry = (xfer->user_data != 0);
    p_msc->stage = is_retry ? MSC_STAGE_STATUS_RETRY : MSC_STAGE_STATUS;
    if (!usbh_edpt_xfer(daddr, p_msc->ep_in, (uint8_t *)csw, (uint16_t)sizeof(msc_csw_t)))
    {
        msc_complete_command(daddr, MSC_CSW_STATUS_FAILED);
        msc_start_recovery(daddr);
    }
}

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
            return true; // The completion is from an earlier command.
        if (event != XFER_RESULT_SUCCESS || xferred_bytes != sizeof(msc_cbw_t))
        {
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED);
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
        // A command with no data phase falls through from MSC_STAGE_CMD on the
        // ep_out completion. Every such command this driver sends, TEST UNIT READY
        // and SYNCHRONIZE CACHE, has its direction set to OUT, so msc_data_ep
        // returns ep_out and the check passes.
        if (ep_addr != msc_data_ep(p_msc, cbw))
            return true; // The completion is from an earlier command.
        // On a fall-through from MSC_STAGE_CMD, xferred_bytes is the length of the
        // CBW, so the data length is recorded as 0.
        p_msc->data_xferred = cbw->total_bytes ? xferred_bytes : 0;
        if (event == XFER_RESULT_FAILED)
        {
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED);
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
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED);
            msc_start_recovery(dev_addr);
            break;
        }
        p_msc->stage = MSC_STAGE_STATUS;
        if (!usbh_edpt_xfer(dev_addr, p_msc->ep_in, (uint8_t *)csw, (uint16_t)sizeof(msc_csw_t)))
        {
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED);
            msc_start_recovery(dev_addr);
        }
        break;

    case MSC_STAGE_STATUS:
    case MSC_STAGE_STATUS_RETRY:
    {
        if (ep_addr != p_msc->ep_in)
            return true; // The completion is from an earlier command.
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
                msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED);
                msc_start_recovery(dev_addr);
                break;
            }
        }

        if (should_retry)
        {
            p_msc->stage = MSC_STAGE_STATUS_RETRY;
            if (usbh_edpt_xfer(dev_addr, p_msc->ep_in, (uint8_t *)csw, (uint16_t)sizeof(msc_csw_t)))
                break;
            TU_LOG_DRV("  MSC BOT: CSW retry xfer failed\r\n");
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED);
            msc_start_recovery(dev_addr);
            break;
        }

        p_msc->stage = MSC_STAGE_IDLE;
        bool csw_valid = (event == XFER_RESULT_SUCCESS &&
                          xferred_bytes == sizeof(msc_csw_t) &&
                          csw->signature == MSC_CSW_SIGNATURE &&
                          csw->tag == cbw->tag &&
                          csw->status <= MSC_CSW_STATUS_PHASE_ERROR &&
                          csw->data_residue <= cbw->total_bytes);
        if (!csw_valid)
        {
            msc_complete_command(dev_addr, MSC_CSW_STATUS_FAILED);
            msc_start_recovery(dev_addr);
        }
        else if (csw->status == MSC_CSW_STATUS_PHASE_ERROR)
        {
            // msc_complete_command is not called, so epbuf->csw keeps the device's
            // PHASE_ERROR status for msc_scsi_sync to return after recovery.
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
    else
    {
        TU_VERIFY(MSC_SUBCLASS_UFI == desc_itf->bInterfaceSubClass ||
                      MSC_SUBCLASS_SFF == desc_itf->bInterfaceSubClass,
                  0);
    }

    msc_interface_t *p_msc = msc_get_itf(dev_addr);
    p_msc->protocol = desc_itf->bInterfaceProtocol;
    p_msc->subclass = desc_itf->bInterfaceSubClass;
    p_msc->max_lun = 0;

    // CBI interrupt endpoints are not opened because msc_scsi_command takes the
    // status of a CB or CBI command from REQUEST SENSE.
    uint16_t drv_len = sizeof(tusb_desc_interface_t);
    uint8_t const *p_desc = tu_desc_next(desc_itf);
    uint8_t const *end = ((uint8_t const *)desc_itf) + max_len;
    uint8_t ep_count = 0;
    while (ep_count < desc_itf->bNumEndpoints && p_desc < end)
    {
        uint8_t const len = ((tusb_desc_interface_t const *)p_desc)->bLength;
        if (len == 0 || (uint16_t)(drv_len + len) > max_len)
        {
            if (p_msc->ep_in)
                tuh_edpt_close(dev_addr, p_msc->ep_in);
            if (p_msc->ep_out)
                tuh_edpt_close(dev_addr, p_msc->ep_out);
            p_msc->ep_in = 0;
            p_msc->ep_out = 0;
            return 0;
        }
        if (tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT)
        {
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
        }
        drv_len = (uint16_t)(drv_len + len);
        p_desc += len;
    }
    TU_ASSERT(p_msc->ep_in, 0);
    TU_ASSERT(p_msc->ep_out, 0);
    p_msc->itf_num = desc_itf->bInterfaceNumber;
    return drv_len;
}

// Pumps USB events and all application tasks during blocking I/O.
// FatFs re-entry would be a problem, so sys_task() never calls FatFs
// but it does call the required tuh_task().
static void msc_pump(void) { sys_task(); }

static msc_status_t msc_scsi_sync(uint8_t dev_addr, msc_cbw_t *cbw,
                                  const void *data, uint32_t timeout_ms)
{
    uint32_t const start_ms = tusb_time_millis_api();

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
        if (msc_scsi_submit(dev_addr, cbw, (void *)(uintptr_t)data))
            break;
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
    bool write_protected : 1;
    uint8_t long_lba_byte; // byte 4: bit 0 = LONGLBA; unused
    uint8_t reserved;
    uint16_t block_descriptor_len; // big-endian
} scsi_mode_sense10_resp_t;
TU_VERIFY_STATIC(sizeof(scsi_mode_sense10_resp_t) == 8, "size is not correct");

typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code;
    uint8_t service_action;
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

typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code;
    uint8_t flags;
    uint32_t lba_hi;      // big-endian, bytes 2-5
    uint32_t lba_lo;      // big-endian, bytes 6-9
    uint32_t block_count; // big-endian, bytes 10-13
    uint8_t group_number; // byte 14
    uint8_t control;      // byte 15
} scsi_read16_t;
TU_VERIFY_STATIC(sizeof(scsi_read16_t) == 16, "size is not correct");

typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code;
    uint8_t flags;
    uint32_t lba_hi;      // big-endian, bytes 2-5
    uint32_t lba_lo;      // big-endian, bytes 6-9
    uint32_t block_count; // big-endian, bytes 10-13
    uint8_t group_number; // byte 14
    uint8_t control;      // byte 15
} scsi_write16_t;
TU_VERIFY_STATIC(sizeof(scsi_write16_t) == 16, "size is not correct");

typedef struct TU_ATTR_PACKED
{
    uint8_t peripheral_device_type : 5;
    uint8_t peripheral_qualifier : 3;
    uint8_t page_code;
    uint16_t page_length; // big-endian, min 0x0004
    uint8_t threshold_exponent;
    uint8_t lbprz : 3;
    uint8_t : 2;
    bool lbpws10 : 1;
    bool lbpws : 1;
    bool lbpu : 1;
} scsi_vpd_lbp_t;
TU_VERIFY_STATIC(sizeof(scsi_vpd_lbp_t) == 6, "size is not correct");

typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code;
    uint8_t anchor : 1;
    uint8_t : 7;
    uint8_t reserved[4];
    uint8_t group_number : 5;
    uint8_t : 3;
    uint16_t param_list_length; // big-endian
    uint8_t control;
} scsi_unmap_t;
TU_VERIFY_STATIC(sizeof(scsi_unmap_t) == 10, "size is not correct");

typedef struct TU_ATTR_PACKED
{
    uint32_t lba_hi;      // big-endian, upper 32 bits
    uint32_t lba_lo;      // big-endian, lower 32 bits
    uint32_t block_count; // big-endian
    uint8_t reserved[4];
} scsi_unmap_block_desc_t;
TU_VERIFY_STATIC(sizeof(scsi_unmap_block_desc_t) == 16, "size is not correct");

typedef struct TU_ATTR_PACKED
{
    uint16_t data_length;       // big-endian (total bytes - 2)
    uint16_t block_desc_length; // big-endian
    uint8_t reserved[4];
    scsi_unmap_block_desc_t desc;
} scsi_unmap_param_t;
TU_VERIFY_STATIC(sizeof(scsi_unmap_param_t) == 24, "size is not correct");

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

// The remaining time is floored at MSC_SCSI_OP_TIMEOUT_MS, so a command and its
// autosense each get at least that long even when the deadline is near or has
// passed. The autosense is the REQUEST SENSE that msc_scsi_command sends after a
// CB or CBI command completes or a BOT command returns FAILED. msc_scsi_command
// can therefore return up to two MSC_SCSI_OP_TIMEOUT_MS after its timeout_ms has
// elapsed.
static uint32_t msc_remaining_timeout(absolute_time_t deadline)
{
    int64_t remaining_ms = absolute_time_diff_us(get_absolute_time(), deadline) / 1000;
    return remaining_ms > MSC_SCSI_OP_TIMEOUT_MS ? (uint32_t)remaining_ms : MSC_SCSI_OP_TIMEOUT_MS;
}

static void msc_clear_sense(uint8_t vol)
{
    msc_pdrv[vol].sense_key = SCSI_SENSE_NONE;
    msc_pdrv[vol].sense_asc = 0;
    msc_pdrv[vol].sense_ascq = 0;
}

static msc_status_t msc_scsi_command(uint8_t vol, msc_cbw_t *cbw,
                                     const void *data, uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    uint8_t dev_addr = msc_pdrv[vol].dev_addr;
    // Events are pumped while an earlier command waits, so an unplug can free the
    // slot before this call, and a dev_addr of 0 would index msc_itf[-1].
    if (dev_addr == 0)
        return MSC_STATUS_FAILED;
    msc_status_t status = MSC_STATUS_FAILED;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        uint32_t attempt_timeout = msc_remaining_timeout(deadline);
        status = msc_scsi_sync(dev_addr, cbw, data, attempt_timeout);
        // The command's data length is saved before REQUEST SENSE below, because
        // the autosense data phase overwrites data_xferred.
        msc_get_itf(dev_addr)->cmd_xferred = msc_get_itf(dev_addr)->data_xferred;
        if (status == MSC_STATUS_TIMED_OUT)
            return status;
        if (status == MSC_STATUS_PHASE_ERROR)
        {
            msc_clear_sense(vol);
            return status;
        }
        // A BOT command's PASSED status comes from the device's CSW. A CB or CBI
        // command's PASSED status means only that its transfers succeeded, so its
        // status is taken from REQUEST SENSE below.
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
        uint32_t sense_timeout = msc_remaining_timeout(deadline);
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
            msc_clear_sense(vol);
        }
        if (msc_protocol(dev_addr) != MSC_PROTOCOL_BOT)
        {
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
            msc_pdrv[vol].sense_asc != 0x28 && // A media change is not retried so disk_status can detect it.
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
    // INQUIRY runs even when a UNIT ATTENTION is pending and does not clear it, so
    // INQUIRY data that arrives with a UNIT ATTENTION is accepted.
    if (msc_pdrv[vol].sense_key == SCSI_SENSE_UNIT_ATTENTION &&
        resp->response_data_format != 0)
        status = MSC_STATUS_PASSED;
    MSC_LOG_CMD(vol, "INQUIRY", status);
    return status;
}

static msc_status_t msc_scsi_test_unit_ready(uint8_t vol)
{
    scsi_test_unit_ready_t const cmd = {.cmd_code = SCSI_CMD_TEST_UNIT_READY};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, 0, TUSB_DIR_OUT, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, NULL, MSC_SCSI_RW_TIMEOUT_MS);
    MSC_LOG_CMD(vol, "TUR", status);
    return status;
}

// FORMAT UNIT (opcode 0x04) is sent for one track and one head. In the CDB,
// byte 1 is 0x17 (FmtData=1, CmpList=0, Defect List Format=7), byte 2 is the
// track and byte 8 is the parameter list length of 12. The parameter list is a
// defect list header, whose byte 1 holds 0xB0 (FOV=1, DCRT=1, STPF=1) with the
// head in bit 0 and whose defect list length is 8, followed by an 8-byte format
// descriptor that holds the block count, a reserved byte and the 3-byte block
// length from msc_pdrv.
static msc_status_t msc_scsi_format_unit(uint8_t vol, uint8_t track, uint8_t head)
{
    uint8_t cmd[12] = {0x04, 0x17, track, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x0C, 0x00, 0x00, 0x00};
    uint32_t blocks = (uint32_t)msc_pdrv[vol].block_count;
    uint32_t bsize = msc_pdrv[vol].block_size;
    uint8_t param[12] = {
        0x00, (uint8_t)(0xB0 | head), 0x00, 0x08,
        (uint8_t)(blocks >> 24), (uint8_t)(blocks >> 16),
        (uint8_t)(blocks >> 8), (uint8_t)blocks,
        0x00,
        (uint8_t)(bsize >> 16), (uint8_t)(bsize >> 8), (uint8_t)bsize};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(param), TUSB_DIR_OUT, sizeof(cmd), cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, param, MSC_SCSI_RW_TIMEOUT_MS);
    MSC_LOG_CMD(vol, "FORMAT UNIT", status);
    return status;
}

static msc_status_t msc_scsi_read_capacity10(uint8_t vol, scsi_read_capacity10_resp_t *resp)
{
    scsi_read_capacity10_t const cmd = {.cmd_code = SCSI_CMD_READ_CAPACITY_10};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_read_capacity10_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_OP_TIMEOUT_MS);
    MSC_LOG_CMD(vol, "READ CAPACITY(10)", status);
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
    MSC_LOG_CMD(vol, "READ CAPACITY(16)", status);
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
    MSC_LOG_CMD(vol, "READ(16)", status);
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
    MSC_LOG_CMD(vol, "WRITE(16)", status);
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
    MSC_LOG_CMD(vol, "READ FORMAT CAPACITIES", status);
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
    MSC_LOG_CMD(vol, "MODE SENSE(6)", status);
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
    MSC_LOG_CMD(vol, "MODE SENSE(10)", status);
    return status;
}

static msc_status_t msc_scsi_sync_cache10(uint8_t vol)
{
    uint8_t cmd[10] = {0x35}; // SYNCHRONIZE CACHE (10)
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, 0, TUSB_DIR_OUT, 10, cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, NULL, MSC_SCSI_RW_TIMEOUT_MS);
    MSC_LOG_CMD(vol, "SYNC CACHE(10)", status);
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
    MSC_LOG_CMD(vol, "UNMAP", status);
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
    MSC_LOG_CMD(vol, "READ(10)", status);
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
    MSC_LOG_CMD(vol, "WRITE(10)", status);
    return status;
}

static bool msc_read_capacity(uint8_t vol)
{
    uint8_t dev_addr = msc_pdrv[vol].dev_addr;

    if (msc_protocol(dev_addr) != MSC_PROTOCOL_BOT)
    {
        // The descriptor type is not checked for 11b (no media) because
        // disk_initialize calls this only after TEST UNIT READY passes.
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
        // READ CAPACITY(16) is needed for a last LBA above 0xFFFFFFFF and for the
        // LBPME bit.
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
            return false;
#endif
        msc_pdrv[vol].block_count = last_lba64 + 1;
        msc_pdrv[vol].block_size = bsize16;
        msc_pdrv[vol].lbpme = (cap16.lbpme_byte >> 7) & 1;
        MSC_LOG_VOL(vol, DEBUG, "READ CAPACITY(16): %llu blocks, %lu bytes/block, LBPME=%d",
                    (unsigned long long)msc_pdrv[vol].block_count,
                    (unsigned long)bsize16, msc_pdrv[vol].lbpme);
        return true;
    }

    scsi_read_capacity10_resp_t cap10 = {0};
    if (msc_scsi_read_capacity10(vol, &cap10) != MSC_STATUS_PASSED)
        return false;
    uint32_t last_lba = tu_ntohl(cap10.last_lba);
    if (last_lba == 0xFFFFFFFF)
        return false; // 0xFFFFFFFF means the last LBA is too large for READ CAPACITY(10).
    uint32_t bsize = tu_ntohl(cap10.block_size);
    if (bsize == 0 || (bsize & (bsize - 1)) != 0 || bsize > 4096)
        return false;
    msc_pdrv[vol].block_count = last_lba + 1;
    msc_pdrv[vol].block_size = bsize;
    return true;
}

// MODE SENSE is sent for all pages (0x3F) with an allocation length that covers
// only the mode parameter header, which holds the write-protect bit whatever
// pages follow.
static void msc_sense_write_protect(uint8_t vol)
{
    uint8_t dev_addr = msc_pdrv[vol].dev_addr;
    if (msc_protocol(dev_addr) == MSC_PROTOCOL_BOT)
    {
        scsi_mode_sense6_resp_t ms6;
        if (msc_scsi_mode_sense6(vol, 0x3F, &ms6) == MSC_STATUS_PASSED)
        {
            MSC_LOG_VOL(vol, DEBUG, "MODE SENSE(6) WP=%d", ms6.write_protected);
            msc_pdrv[vol].write_prot = ms6.write_protected;
        }
    }
    else
    {
        scsi_mode_sense10_resp_t ms10;
        if (msc_scsi_mode_sense10(vol, 0x3F, &ms10) == MSC_STATUS_PASSED)
        {
            MSC_LOG_VOL(vol, DEBUG, "MODE SENSE(10) WP=%d", ms10.write_protected);
            msc_pdrv[vol].write_prot = ms10.write_protected;
        }
    }
}

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
    MSC_LOG_CMD(vol, "INQUIRY VPD B2", status);
    if (status != MSC_STATUS_PASSED || resp.page_code != 0xB2)
        return false;
    MSC_LOG_VOL(vol, DEBUG, "VPD B2: LBPU=%d", resp.lbpu);
    return resp.lbpu;
}

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
            RP6502_LOG(msc, WARN, "no free pdrv for dev %d LUN %d", dev_addr, lun);
            break;
        }
        msc_pdrv[pdrv].dev_addr = dev_addr;
        msc_pdrv[pdrv].lun = lun;
        msc_pdrv[pdrv].status = msc_volume_registered;
        msc_mount_gen[pdrv]++;
        // With opt 0, f_mount only registers the volume, and FatFs calls
        // disk_initialize on the first access to it.
        TCHAR volstr[6];
        msc_vol_path(volstr, pdrv);
        f_mount(&msc_pdrv[pdrv].fatfs, volstr, 0);
        MSC_LOG_VOL(pdrv, INFO, "registered dev_addr %d LUN %d", dev_addr, lun);
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
        msc_mount_gen[pdrv]++;
        MSC_LOG_VOL(pdrv, INFO, "unmounted (dev_addr %d)", dev_addr);
    }
}

static void msc_get_max_lun_complete_cb(tuh_xfer_t *xfer)
{
    uint8_t daddr = xfer->daddr;
    msc_interface_t *p_msc = msc_get_itf(daddr);
    msc_epbuf_t *epbuf = msc_get_epbuf(daddr);
    if (p_msc->cancelling)
        return;

    // A device with a single LUN may STALL GET_MAX_LUN, so a failed request leaves
    // max_lun at 0.
    if (xfer->result == XFER_RESULT_SUCCESS)
    {
        uint8_t ml = epbuf->max_lun_buf[0];
        if (ml >= MSC_MAX_LUN_COUNT)
            ml = MSC_MAX_LUN_COUNT - 1;
        p_msc->max_lun = ml;
    }

    p_msc->mounted = true;
    msc_mount_cb(daddr);
    usbh_driver_set_config_complete(daddr, p_msc->itf_num);
}

bool msc_class_driver_set_config(uint8_t daddr, uint8_t itf_num)
{
    msc_interface_t *p_msc = msc_get_itf(daddr);
    TU_ASSERT(p_msc->itf_num == itf_num);

    if (!msc_is_bot(p_msc))
    {
        p_msc->mounted = true;
        msc_mount_cb(daddr);
        usbh_driver_set_config_complete(daddr, p_msc->itf_num);
        return true;
    }

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
        MSC_LOG_VOL(vol, DEBUG, "disk_status, not mounted, status=%d", msc_pdrv[vol].status);
        return STA_NOINIT;
    }
    // Test for removed media if we haven't used the drive in a while.
    if (msc_pdrv[vol].removable &&
        time_reached(delayed_by_ms(msc_pdrv[vol].last_ok, MSC_DISK_STATUS_TIMEOUT_MS)))
    {
        MSC_LOG_VOL(vol, DEBUG, "disk_status, issuing TUR");
        // A TUR that times out leaves the volume mounted so a slow drive is not
        // dropped. A read or write that times out still returns RES_NOTRDY.
        if (msc_scsi_test_unit_ready(vol) == MSC_STATUS_FAILED)
        {
            uint8_t asc = msc_pdrv[vol].sense_asc;
            if (asc == 0x3A || asc == 0x28) // MEDIUM NOT PRESENT or MAY HAVE CHANGED
            {
                // The removable flag and spc_version are kept because
                // disk_initialize does not repeat INQUIRY for an ejected volume.
                msc_pdrv[vol].status = msc_volume_ejected;
                msc_pdrv[vol].block_count = 0;
                msc_pdrv[vol].block_size = 0;
                msc_pdrv[vol].write_prot = false;
                msc_clear_sense(vol);
                return STA_NOINIT;
            }
        }
        // Events are pumped during the TUR, so an unplug can free the slot.
        if (msc_pdrv[vol].status != msc_volume_mounted)
            return STA_NOINIT;
    }
    return msc_pdrv[vol].write_prot ? STA_PROTECT : 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    uint8_t vol = pdrv;
    MSC_LOG_VOL(vol, DEBUG, "disk_initialize, status=%d", msc_pdrv[vol].status);

    if (msc_pdrv[vol].status == msc_volume_registered ||
        msc_pdrv[vol].status == msc_volume_ejected)
    {
        if (msc_pdrv[vol].status == msc_volume_registered)
        {
            scsi_inquiry_resp_t inq;
            if (msc_scsi_inquiry(vol, &inq) != MSC_STATUS_PASSED)
                return STA_NOINIT;
            msc_pdrv[vol].removable = inq.is_removable;
            msc_pdrv[vol].spc_version = inq.version;
        }

        // The first TUR after a medium is inserted fails with UNIT ATTENTION,
        // ASC 0x28, which msc_scsi_command does not retry so that disk_status can
        // detect a media change. The autosense REQUEST SENSE clears the
        // condition, so a second TUR can pass.
        bool tur_ok = msc_scsi_test_unit_ready(vol) == MSC_STATUS_PASSED;
        if (!tur_ok && (msc_pdrv[vol].sense_key == SCSI_SENSE_NOT_READY ||
                        msc_pdrv[vol].sense_asc == 0x28))
            tur_ok = msc_scsi_test_unit_ready(vol) == MSC_STATUS_PASSED;
        if (!tur_ok)
        {
            if (msc_pdrv[vol].removable)
                msc_pdrv[vol].status = msc_volume_ejected;
        }
        else if (msc_read_capacity(vol))
        {
            msc_sense_write_protect(vol);
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
    // usbh_edpt_xfer takes a 16-bit length, so each transfer is limited to
    // UINT16_MAX bytes.
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
        // On a write, cmd_xferred counts the bytes the host sent, so the CSW
        // residue is also checked for bytes the device did not process.
        if (msc_get_itf(dev_addr)->cmd_xferred != (uint32_t)n * block_size)
            return RES_ERROR;
        if (msc_protocol(dev_addr) == MSC_PROTOCOL_BOT &&
            msc_get_epbuf(dev_addr)->csw.data_residue != 0)
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
        // f_mkfs aligns its data area to this many sectors, so 1 means no
        // alignment.
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
        if ((end - start) >= (LBA_t)UINT32_MAX)
            return RES_PARERR;
        msc_status_t status = msc_scsi_unmap(vol, start, (uint32_t)(end - start + 1));
        return status == MSC_STATUS_TIMED_OUT ? RES_NOTRDY : RES_OK;
    }
    default:
        return RES_PARERR;
    }
}

void msc_drive_reenumerate(uint8_t pdrv)
{
    if (pdrv >= FF_VOLUMES || msc_pdrv[pdrv].status == msc_volume_free)
        return;
    TCHAR volstr[6];
    msc_vol_path(volstr, pdrv);
    f_unmount(volstr);
    f_mount(&msc_pdrv[pdrv].fatfs, volstr, 0);
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

int msc_status_response(char *buf, size_t buf_size, int state, unsigned)
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
            oem_snprintf(buf, buf_size, STR_STATUS_MSC,
                              VolumeStr[vol],
                              sizebuf,
                              inq.vendor_id,
                              inq.product_id,
                              inq.product_rev);
        }
        else
        {
            oem_snprintf(buf, buf_size, STR_STATUS_MSC,
                              VolumeStr[vol],
                              sizebuf,
                              S(STR_PARENS_NONE), S(STR_PARENS_NONE), "");
        }
    }
    return state + 1;
}

int msc_drive_vol_from_name(const char *name)
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
    if (n == 1 && buf[0] >= '0' && buf[0] <= '9' && (buf[0] - '0') < FF_VOLUMES)
        return buf[0] - '0';
    return -1;
}

static bool msc_drive_pdrv_of_vol(uint8_t vol, uint8_t *pdrv)
{
    if (vol >= FF_VOLUMES || msc_pdrv[vol].status == msc_volume_free)
        return false;
    *pdrv = vol;
    return true;
}

bool msc_drive_get_info(uint8_t vol, msc_drive_info_t *out)
{
    uint8_t pdrv;
    if (!msc_drive_pdrv_of_vol(vol, &pdrv))
        return false;
    if (disk_status(pdrv) == STA_NOINIT)
        disk_initialize(pdrv);
    if (msc_pdrv[pdrv].dev_addr == 0) // An unplug handled during the calls above can free the slot.
        return false;
    msc_interface_t *p_msc = msc_get_itf(msc_pdrv[pdrv].dev_addr);
    out->present = (msc_pdrv[pdrv].status == msc_volume_mounted);
    out->removable = msc_pdrv[pdrv].removable;
    out->write_prot = msc_pdrv[pdrv].write_prot;
    out->is_floppy = !msc_is_bot(p_msc); // msc_class_driver_open accepts CB and CBI only with UFI or SFF.
    out->block_count = msc_pdrv[pdrv].block_count;
    out->block_size = msc_pdrv[pdrv].block_size;
    out->gen = msc_mount_gen[vol];
    msc_vol_path(out->path, vol);
    return true;
}

bool msc_drive_inquiry_strings(uint8_t vol, char vendor[9], char product[17], char rev[5])
{
    uint8_t pdrv;
    if (!msc_drive_pdrv_of_vol(vol, &pdrv))
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

bool msc_drive_serial(uint8_t vol, char *dst, size_t dst_size)
{
    uint8_t pdrv;
    if (!msc_drive_pdrv_of_vol(vol, &pdrv) || dst_size == 0)
        return false;
    const void *s = usb_string_fetch_serial(msc_pdrv[pdrv].dev_addr);
    if (!s)
        return false;
    usb_desc_string_to_oem(s, USB_DESC_STRING_BUF_SIZE, dst, dst_size);
    return dst[0] != '\0';
}

bool msc_drive_read(uint8_t vol, void *buf, uint64_t lba, uint32_t count)
{
    uint8_t pdrv;
    if (!msc_drive_pdrv_of_vol(vol, &pdrv))
        return false;
    return disk_read(pdrv, (BYTE *)buf, (LBA_t)lba, count) == RES_OK;
}

bool msc_drive_write(uint8_t vol, const void *buf, uint64_t lba, uint32_t count)
{
    uint8_t pdrv;
    if (!msc_drive_pdrv_of_vol(vol, &pdrv))
        return false;
    return disk_write(pdrv, (const BYTE *)buf, (LBA_t)lba, count) == RES_OK;
}

bool msc_drive_format_track(uint8_t vol, uint8_t track, uint8_t head)
{
    uint8_t pdrv;
    if (!msc_drive_pdrv_of_vol(vol, &pdrv))
        return false;
    if (msc_pdrv[pdrv].write_prot)
        return false;
    return msc_scsi_format_unit(pdrv, track, head) == MSC_STATUS_PASSED;
}
