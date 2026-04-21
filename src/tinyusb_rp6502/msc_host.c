/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2026 Rumbledethumps
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if CFG_TUH_ENABLED && CFG_TUH_MSC

#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "host/hcd.h"

#include "class/msc/msc_host.h"

#ifndef CFG_TUH_MSC_LOG_LEVEL
#define CFG_TUH_MSC_LOG_LEVEL CFG_TUH_LOG_LEVEL
#endif

#define TU_LOG_DRV(...) TU_LOG(CFG_TUH_MSC_LOG_LEVEL, __VA_ARGS__)

//--------------------------------------------------------------------+
// TYPES AND DATA
//--------------------------------------------------------------------+

// Superset of msc_csw_status_t with an additional timeout value.
typedef enum
{
    MSC_STATUS_PASSED,      // == MSC_CSW_STATUS_PASSED
    MSC_STATUS_FAILED,      // == MSC_CSW_STATUS_FAILED
    MSC_STATUS_PHASE_ERROR, // == MSC_CSW_STATUS_PHASE_ERROR
    MSC_STATUS_TIMED_OUT,   // returned on I/O timeout
} msc_status_t;

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
    uint8_t max_lun; // highest LUN index on this device (0 = single LUN)
} msch_interface_t;

typedef struct
{
    TUH_EPBUF_TYPE_DEF(msc_cbw_t, cbw);
    TUH_EPBUF_TYPE_DEF(msc_csw_t, csw);
    TUH_EPBUF_DEF(cbi_cmd, 12);    // CBI ADSC command buffer (UFI = 12 bytes)
    TUH_EPBUF_DEF(max_lun_buf, 1); // GET_MAX_LUN response (1 byte)
} msch_epbuf_t;

static msch_interface_t _msch_itf[CFG_TUH_DEVICE_MAX];
CFG_TUH_MEM_SECTION static msch_epbuf_t _msch_epbuf[CFG_TUH_DEVICE_MAX];

// Monotonically incrementing CBW tag for sequencing and stale-CSW detection.
static uint32_t msc_cbw_tag_counter = 0;

TU_ATTR_ALWAYS_INLINE static inline msch_interface_t *get_itf(uint8_t daddr)
{
    return &_msch_itf[daddr - 1];
}

TU_ATTR_ALWAYS_INLINE static inline msch_epbuf_t *get_epbuf(uint8_t daddr)
{
    return &_msch_epbuf[daddr - 1];
}

TU_ATTR_ALWAYS_INLINE static inline bool is_bot(msch_interface_t const *p_msc)
{
    return p_msc->protocol == MSC_PROTOCOL_BOT;
}

// Resolve data endpoint from CBW direction.
TU_ATTR_ALWAYS_INLINE static inline uint8_t data_ep(msch_interface_t const *p_msc,
                                                    msc_cbw_t const *cbw)
{
    return (cbw->dir & TUSB_DIR_IN_MASK) ? p_msc->ep_in : p_msc->ep_out;
}

//--------------------------------------------------------------------+
// INTERNAL HELPERS
//--------------------------------------------------------------------+

// Fabricate a CSW and set stage to IDLE.
static void complete_command(uint8_t daddr, uint8_t csw_status, uint32_t data_residue)
{
    msch_interface_t *p_msc = get_itf(daddr);
    msch_epbuf_t *epbuf = get_epbuf(daddr);
    p_msc->stage = MSC_STAGE_IDLE;
    epbuf->csw.data_residue = data_residue;
    epbuf->csw.status = csw_status;
}

// Submit data-phase transfer or complete with failure.
static void start_data_phase(uint8_t daddr)
{
    msch_interface_t *p_msc = get_itf(daddr);
    msc_cbw_t const *cbw = &get_epbuf(daddr)->cbw;
    // Reject transfers that exceed the 16-bit USB transfer length.
    // Callers must clamp transfer sizes before building the CBW.
    if (cbw->total_bytes > UINT16_MAX)
    {
        complete_command(daddr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
        return;
    }
    p_msc->stage = MSC_STAGE_DATA;
    if (!usbh_edpt_xfer(daddr, data_ep(p_msc, cbw), p_msc->buffer, (uint16_t)cbw->total_bytes))
    {
        complete_command(daddr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
    }
}

//--------------------------------------------------------------------+
// Weak stubs
//--------------------------------------------------------------------+

TU_ATTR_WEAK void tuh_msc_mount_cb(uint8_t dev_addr)
{
    (void)dev_addr;
}

TU_ATTR_WEAK void tuh_msc_umount_cb(uint8_t dev_addr)
{
    (void)dev_addr;
}

// Weak pump hook called from tuh_msc_scsi_sync() while waiting for
// USB events.  Override should call tuh_task() along with its own
// application-level tasks that must run during blocking MSC I/O.
TU_ATTR_WEAK void tuh_msc_pump(void) { tuh_task(); }

//--------------------------------------------------------------------+
// PUBLIC API
//--------------------------------------------------------------------+

bool tuh_msc_mounted(uint8_t dev_addr)
{
    return get_itf(dev_addr)->mounted;
}

bool tuh_msc_ready(uint8_t dev_addr)
{
    msch_interface_t *p_msc = get_itf(dev_addr);
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

uint8_t tuh_msc_get_maxlun(uint8_t dev_addr)
{
    return get_itf(dev_addr)->max_lun;
}

uint8_t tuh_msc_protocol(uint8_t dev_addr)
{
    return get_itf(dev_addr)->protocol;
}

msc_csw_status_t tuh_msc_csw_status(uint8_t dev_addr)
{
    return get_epbuf(dev_addr)->csw.status;
}

//--------------------------------------------------------------------+
// Recovery State Machine
//--------------------------------------------------------------------+

static void cancel_inflight(uint8_t dev_addr)
{
    msch_interface_t *p_msc = get_itf(dev_addr);

    // If a CBI ADSC control transfer is in-flight, abort it.
    if (p_msc->stage == MSC_STAGE_CMD && !is_bot(p_msc))
    {
        tuh_edpt_abort_xfer(dev_addr, 0);
    }

    tuh_edpt_abort_xfer(dev_addr, p_msc->ep_in);
    tuh_edpt_abort_xfer(dev_addr, p_msc->ep_out);

    p_msc->stage = MSC_STAGE_IDLE;
}

// Send CLEAR_FEATURE(ENDPOINT_HALT) to ep_addr on daddr.
static bool clear_endpoint_halt(uint8_t daddr, uint8_t ep_addr,
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
static void recovery_abort_to_idle(msch_interface_t *p_msc, uint8_t daddr)
{
    uint8_t const rhport = usbh_get_rhport(daddr);
    hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_in);
    hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_out);
    p_msc->recovery_stage = RECOVERY_IDLE;
}

static void recovery_xfer_cb(tuh_xfer_t *xfer)
{
    uint8_t const daddr = xfer->daddr;
    msch_interface_t *p_msc = get_itf(daddr);

    if (xfer->result != XFER_RESULT_SUCCESS)
    {
        uint8_t const rhport = usbh_get_rhport(daddr);
        switch (p_msc->recovery_stage)
        {
        case RECOVERY_CLEAR_IN:
            hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_in);
            TU_ATTR_FALLTHROUGH;
        case RECOVERY_CLEAR_OUT:
            hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_out);
            p_msc->recovery_stage = RECOVERY_IDLE;
            return;
        default:
            break;
        }
        p_msc->recovery_stage = RECOVERY_CLEAR_IN;
        if (!clear_endpoint_halt(daddr, p_msc->ep_in, recovery_xfer_cb, 0))
            recovery_abort_to_idle(p_msc, daddr);
        return;
    }

    switch (p_msc->recovery_stage)
    {
    case RECOVERY_RESET:
        p_msc->recovery_stage = RECOVERY_CLEAR_IN;
        if (!clear_endpoint_halt(daddr, p_msc->ep_in, recovery_xfer_cb, 0))
            recovery_abort_to_idle(p_msc, daddr);
        break;

    case RECOVERY_CLEAR_IN:
    {
        uint8_t const rhport = usbh_get_rhport(daddr);
        hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_in);
        p_msc->recovery_stage = RECOVERY_CLEAR_OUT;
        if (!clear_endpoint_halt(daddr, p_msc->ep_out, recovery_xfer_cb, 0))
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
static void recovery_start_clear_halts(uint8_t daddr)
{
    msch_interface_t *p_msc = get_itf(daddr);
    p_msc->recovery_stage = RECOVERY_CLEAR_IN;
    if (!clear_endpoint_halt(daddr, p_msc->ep_in, recovery_xfer_cb, 0))
        recovery_abort_to_idle(p_msc, daddr);
}

// Start async reset recovery.  Stage must be IDLE.
// tuh_msc_ready() returns false until recovery finishes.
static void start_recovery(uint8_t daddr)
{
    msch_interface_t *p_msc = get_itf(daddr);
    if (!p_msc->ep_in)
        return;
    if (p_msc->recovery_stage != RECOVERY_IDLE)
        return;

    if (is_bot(p_msc))
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
            .complete_cb = recovery_xfer_cb,
            .user_data = 0};
        p_msc->recovery_stage = RECOVERY_RESET;
        if (!tuh_control_xfer(&xfer))
        {
            recovery_start_clear_halts(daddr);
        }
        return;
    }

    // CBI reset: SEND_DIAGNOSTIC(SelfTest=1) via ADSC, then clear bulk endpoints.
    msch_epbuf_t *epbuf = get_epbuf(daddr);
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
        .complete_cb = recovery_xfer_cb,
        .user_data = 0};
    p_msc->recovery_stage = RECOVERY_RESET;
    if (!tuh_control_xfer(&xfer))
    {
        recovery_start_clear_halts(daddr);
    }
}

// Cancel any in-flight command and start async recovery, or
// force-stop an ongoing recovery that has stalled.
void tuh_msc_abort(uint8_t daddr)
{
    msch_interface_t *p_msc = get_itf(daddr);
    if (!p_msc->ep_in)
        return;

    // If recovery is already in progress, force-stop it.
    if (p_msc->recovery_stage != RECOVERY_IDLE)
    {
        // Cancel the in-flight recovery request and restart recovery from a
        // clean state so we never return while transport may still be wedged.
        tuh_edpt_abort_xfer(daddr, 0);
        cancel_inflight(daddr);
        p_msc->recovery_stage = RECOVERY_IDLE;
        start_recovery(daddr);
        return;
    }

    // Nothing to abort.
    if (p_msc->stage == MSC_STAGE_IDLE)
        return;

    cancel_inflight(daddr);
    start_recovery(daddr);
}

//--------------------------------------------------------------------+
// CBI (Control/Bulk/Interrupt) Transport
//--------------------------------------------------------------------+
static void cbi_adsc_complete(tuh_xfer_t *xfer)
{
    uint8_t const daddr = xfer->daddr;
    msch_interface_t *p_msc = get_itf(daddr);
    msch_epbuf_t *epbuf = get_epbuf(daddr);

    if (XFER_RESULT_SUCCESS != xfer->result)
    {
        complete_command(daddr, MSC_CSW_STATUS_FAILED, epbuf->cbw.total_bytes);
        return;
    }

    // ADSC succeeded — run data phase if any, otherwise assume success (CB transport).
    if (epbuf->cbw.total_bytes && p_msc->buffer)
        start_data_phase(daddr);
    else
        complete_command(daddr, MSC_CSW_STATUS_PASSED, 0);
}

//--------------------------------------------------------------------+
// PUBLIC API: SCSI COMMAND
//--------------------------------------------------------------------+
bool tuh_msc_scsi_submit(uint8_t daddr, msc_cbw_t const *cbw, void *data)
{
    msch_interface_t *p_msc = get_itf(daddr);
    TU_VERIFY(p_msc->ep_in);
    TU_VERIFY(p_msc->stage == MSC_STAGE_IDLE);
    msch_epbuf_t *epbuf = get_epbuf(daddr);

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

    if (is_bot(p_msc))
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
        .complete_cb = cbi_adsc_complete,
        .user_data = 0};

    if (!tuh_control_xfer(&xfer))
    {
        p_msc->stage = MSC_STAGE_IDLE;
        return false;
    }
    return true;
}

//--------------------------------------------------------------------+
// PUBLIC API: SYNCHRONOUS I/O
//--------------------------------------------------------------------+
// Wait for transport ready, submit command, and wait for completion.
// No autosense — used directly for REQUEST SENSE itself.
// Calls tuh_msc_pump() while spinning.
msc_status_t tuh_msc_scsi_sync(uint8_t dev_addr, msc_cbw_t *cbw,
                               const void *data, uint32_t timeout_ms)
{
    uint32_t const start_ms = tusb_time_millis_api();

    // Wait for transport ready AND successfully submit the command.
    // CBI transport sends the CDB via an ADSC control transfer on the
    // shared EPX.  If another control transfer is in-flight (e.g. hub
    // Get Port Status), tuh_msc_scsi_submit() returns false even though
    // the MSC layer is idle.  We must retry submission rather than
    // failing the entire command.
    for (;;)
    {
        if (!tuh_msc_mounted(dev_addr))
            return MSC_STATUS_FAILED;
        if ((tusb_time_millis_api() - start_ms) >= timeout_ms)
            return MSC_STATUS_TIMED_OUT;
        if (!tuh_msc_ready(dev_addr))
        {
            tuh_msc_pump();
            continue;
        }
        // Cast away const: transport API uses void* for both directions.
        if (tuh_msc_scsi_submit(dev_addr, cbw, (void *)(uintptr_t)data))
            break;
        // Submit failed (control pipe busy) — pump events and retry
        tuh_msc_pump();
    }
    while (!tuh_msc_ready(dev_addr))
    {
        if (!tuh_msc_mounted(dev_addr))
            return MSC_STATUS_FAILED;
        if ((tusb_time_millis_api() - start_ms) >= timeout_ms)
        {
            tuh_msc_abort(dev_addr);
            return MSC_STATUS_TIMED_OUT;
        }
        tuh_msc_pump();
    }
    // CSW validation happens in bot_xfer_cb: invalid CSWs are replaced with a
    // synthesized FAILED status and reset recovery is started. A device-reported
    // PHASE_ERROR is left intact so we can return it verbatim. Arrival here
    // implies tuh_msc_ready() returned true, i.e., recovery has drained.
    return (msc_status_t)tuh_msc_csw_status(dev_addr);
}

//--------------------------------------------------------------------+
// CLASS-USBH API
//--------------------------------------------------------------------+
bool msch_init(void)
{
    TU_LOG_DRV("sizeof(msch_interface_t) = %u\r\n", sizeof(msch_interface_t));
    TU_LOG_DRV("sizeof(msch_epbuf_t) = %u\r\n", sizeof(msch_epbuf_t));
    tu_memclr(_msch_itf, sizeof(_msch_itf));
    return true;
}

bool msch_deinit(void)
{
    return true;
}

void msch_close(uint8_t dev_addr)
{
    msch_interface_t *p_msc = get_itf(dev_addr);
    TU_VERIFY(p_msc->ep_in, );

    TU_LOG_DRV("  MSCh close addr = %d\r\n", dev_addr);

    cancel_inflight(dev_addr);
    p_msc->recovery_stage = RECOVERY_IDLE;

    tuh_edpt_close(dev_addr, p_msc->ep_in);
    tuh_edpt_close(dev_addr, p_msc->ep_out);

    if (p_msc->mounted)
        tuh_msc_umount_cb(dev_addr);

    tu_memclr(p_msc, sizeof(msch_interface_t));
    tu_memclr(get_epbuf(dev_addr), sizeof(msch_epbuf_t));
}

// CBI (CB-only, interrupt endpoint unused) transfer-complete handler.
// Command status is inferred from the data-phase outcome.
static bool cbi_xfer_cb(uint8_t dev_addr, xfer_result_t event, uint32_t xferred_bytes)
{
    msch_interface_t *p_msc = get_itf(dev_addr);
    msc_cbw_t const *cbw = &get_epbuf(dev_addr)->cbw;

    if (p_msc->stage != MSC_STAGE_DATA)
        return true;

    uint32_t const residue = (xferred_bytes <= cbw->total_bytes)
                                 ? cbw->total_bytes - xferred_bytes
                                 : 0;

    if (event == XFER_RESULT_FAILED && residue > 0)
    {
        // Some bytes arrived before the error (short/partial bulk transfer).
        // Re-arm from the already-transferred offset to attempt the remainder.
        if (usbh_edpt_xfer(dev_addr, data_ep(p_msc, cbw),
                           (uint8_t *)p_msc->buffer + xferred_bytes,
                           (uint16_t)residue))
            return true;
        // Re-arm failed (device gone); fall through to completion.
    }

    // On STALL, clear the HCD-level halt so the controller can reuse the pipe.
    // The device-level CLEAR_FEATURE is issued by recovery below.
    if (event == XFER_RESULT_STALLED)
        hcd_edpt_clear_stall(usbh_get_rhport(dev_addr), dev_addr, data_ep(p_msc, cbw));

    uint8_t const status = (event == XFER_RESULT_SUCCESS)
                               ? MSC_CSW_STATUS_PASSED
                               : MSC_CSW_STATUS_FAILED;
    complete_command(dev_addr, status, residue);
    if (event != XFER_RESULT_SUCCESS)
        start_recovery(dev_addr);
    return true;
}

// Callback following a device-level CLEAR_FEATURE(ENDPOINT_HALT) issued after a
// BOT data-phase or CSW-phase STALL.
// On success, queue CSW read; on failure, fail command and start recovery.
//   user_data == 0: first-attempt CSW read (data-phase STALL, BOT §6.6.1)
//   user_data == 1: retry CSW read         (CSW-phase STALL,  BOT §6.7.2)
static void bot_clear_for_csw_cb(tuh_xfer_t *xfer)
{
    uint8_t const daddr = xfer->daddr;
    msch_interface_t *p_msc = get_itf(daddr);
    msch_epbuf_t *epbuf = get_epbuf(daddr);
    msc_csw_t *csw = &epbuf->csw;
    msc_cbw_t const *cbw = &epbuf->cbw;
    if (xfer->result != XFER_RESULT_SUCCESS)
    {
        complete_command(daddr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
        start_recovery(daddr);
        return;
    }
    bool const is_retry = (xfer->user_data != 0);
    p_msc->stage = is_retry ? MSC_STAGE_STATUS_RETRY : MSC_STAGE_STATUS;
    if (!usbh_edpt_xfer(daddr, p_msc->ep_in, (uint8_t *)csw, (uint16_t)sizeof(msc_csw_t)))
    {
        complete_command(daddr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
        start_recovery(daddr);
    }
}

// BOT transfer-complete handler.
static bool bot_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t event, uint32_t xferred_bytes)
{
    msch_interface_t *p_msc = get_itf(dev_addr);
    msch_epbuf_t *epbuf = get_epbuf(dev_addr);
    msc_cbw_t const *cbw = &epbuf->cbw;
    msc_csw_t *csw = &epbuf->csw;

    switch (p_msc->stage)
    {
    case MSC_STAGE_CMD:
        if (ep_addr != p_msc->ep_out || event != XFER_RESULT_SUCCESS || xferred_bytes != sizeof(msc_cbw_t))
        {
            complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            start_recovery(dev_addr);
            break;
        }
        if (cbw->total_bytes && p_msc->buffer)
        {
            start_data_phase(dev_addr);
            break;
        }
        TU_ATTR_FALLTHROUGH;

    case MSC_STAGE_DATA:
        if (ep_addr != data_ep(p_msc, cbw))
        {
            complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            start_recovery(dev_addr);
            break;
        }
        if (event == XFER_RESULT_FAILED)
        {
            complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            start_recovery(dev_addr);
            break;
        }
        if (event == XFER_RESULT_STALLED)
        {
            uint8_t const stalled_ep = data_ep(p_msc, cbw);
            hcd_edpt_clear_stall(usbh_get_rhport(dev_addr), dev_addr, stalled_ep);
            if (clear_endpoint_halt(dev_addr, stalled_ep, bot_clear_for_csw_cb, 0))
            {
                p_msc->stage = MSC_STAGE_STATUS;
                break;
            }
            complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            start_recovery(dev_addr);
            break;
        }
        // Read CSW
        p_msc->stage = MSC_STAGE_STATUS;
        if (!usbh_edpt_xfer(dev_addr, p_msc->ep_in, (uint8_t *)csw, (uint16_t)sizeof(msc_csw_t)))
        {
            complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            start_recovery(dev_addr);
        }
        break;

    case MSC_STAGE_STATUS:
    case MSC_STAGE_STATUS_RETRY:
    {
        if (ep_addr != p_msc->ep_in)
        {
            complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            start_recovery(dev_addr);
            break;
        }
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
                if (clear_endpoint_halt(dev_addr, p_msc->ep_in, bot_clear_for_csw_cb, 1))
                {
                    p_msc->stage = MSC_STAGE_STATUS_RETRY;
                    break;
                }
                complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
                start_recovery(dev_addr);
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
            complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            start_recovery(dev_addr);
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
            complete_command(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            start_recovery(dev_addr);
        }
        else if (csw->status == MSC_CSW_STATUS_PHASE_ERROR)
        {
            // BOT §6.7.2: phase error requires reset recovery.
            // The raw device CSW (with PHASE_ERROR status) is preserved in
            // epbuf->csw so that tuh_msc_scsi_sync() can return
            // msc_status_phase_error to the caller.
            start_recovery(dev_addr);
        }
        break;
    }

    default:
        break;
    }

    return true;
}

bool msch_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t event, uint32_t xferred_bytes)
{
    if (is_bot(get_itf(dev_addr)))
        return bot_xfer_cb(dev_addr, ep_addr, event, xferred_bytes);
    return cbi_xfer_cb(dev_addr, event, xferred_bytes);
}

//--------------------------------------------------------------------+
// MSC Enumeration
//--------------------------------------------------------------------+

uint16_t msch_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf, uint16_t max_len)
{
    (void)rhport;

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

    msch_interface_t *p_msc = get_itf(dev_addr);
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

static void get_max_lun_complete_cb(tuh_xfer_t *xfer)
{
    uint8_t daddr = xfer->daddr;
    msch_interface_t *p_msc = get_itf(daddr);
    msch_epbuf_t *epbuf = get_epbuf(daddr);

    if (xfer->result == XFER_RESULT_SUCCESS)
    {
        // Clamp to CFG_TUH_MSC_MAXLUN-1 per BOT spec §3.2
        uint8_t ml = epbuf->max_lun_buf[0];
        if (ml >= CFG_TUH_MSC_MAXLUN)
            ml = CFG_TUH_MSC_MAXLUN - 1;
        p_msc->max_lun = ml;
    }
    // else: STALL means no LUNs beyond 0; max_lun stays 0.

    p_msc->mounted = true;
    tuh_msc_mount_cb(daddr);
    usbh_driver_set_config_complete(daddr, p_msc->itf_num);
}

bool msch_set_config(uint8_t daddr, uint8_t itf_num)
{
    msch_interface_t *p_msc = get_itf(daddr);
    TU_ASSERT(p_msc->itf_num == itf_num);

    // CBI/CB: single-LUN by spec, skip GET_MAX_LUN.
    if (!is_bot(p_msc))
    {
        p_msc->mounted = true;
        tuh_msc_mount_cb(daddr);
        usbh_driver_set_config_complete(daddr, p_msc->itf_num);
        return true;
    }

    // BOT: issue GET_MAX_LUN; completion fires get_max_lun_complete_cb.
    msch_epbuf_t *epbuf = get_epbuf(daddr);
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
        .complete_cb = get_max_lun_complete_cb,
        .user_data = 0};
    if (!tuh_control_xfer(&xfer))
    {
        // Control pipe busy or error — proceed with LUN 0 only.
        p_msc->mounted = true;
        tuh_msc_mount_cb(daddr);
        usbh_driver_set_config_complete(daddr, p_msc->itf_num);
    }
    return true;
}

#endif
