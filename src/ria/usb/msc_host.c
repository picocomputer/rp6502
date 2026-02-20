/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2025 Rumbledethumps
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

/*
This file overrides the TinyUSB file:
src/tinyusb/src/class/msc/msc_host.c
See cmake configuration for override.

Changes from upstream TinyUSB msc_host.c:

- Add CBI (Control/Bulk/Interrupt) transport support.
- tuh_msc_scsi_command() routes to CBI or BOT based on protocol.
- msch_open() accepts CBI/CBI_NO_INTERRUPT protocols and UFI/SFF subclasses;
  iterates bNumEndpoints to handle bulk + interrupt endpoints.
- msch_set_config() skips GET_MAX_LUN and all SCSI enumeration; msc.c handles
  everything in disk_initialize/msc_init_volume.
- max_lun, capacity[], tuh_msc_get_maxlun(), tuh_msc_get_block_count(),
  tuh_msc_get_block_size(), tuh_msc_read10(), tuh_msc_write10() removed:
  msc.c owns LUN enumeration and block size/count tracking.
- Async reset recovery state machine for both BOT and CBI transports.
*/

#include "tusb_option.h"

#if CFG_TUH_ENABLED && CFG_TUH_MSC

#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "host/hcd.h"

#include "class/msc/msc_host.h"

// Defined in hcd_rp2040.c — pause/resume hardware interrupt endpoint
// polling to prevent bus contention during control transfers on the
// shared EPX.  Reference-counted: nested pause/resume pairs are safe.
extern void hcd_pause_interrupt_eps(uint8_t rhport);
extern void hcd_resume_interrupt_eps(uint8_t rhport);
extern void hcd_force_resume_interrupt_eps(uint8_t rhport);

#ifndef CFG_TUH_MSC_LOG_LEVEL
#define CFG_TUH_MSC_LOG_LEVEL CFG_TUH_LOG_LEVEL
#endif

#define TU_LOG_DRV(...) TU_LOG(CFG_TUH_MSC_LOG_LEVEL, __VA_ARGS__)

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF
//--------------------------------------------------------------------+
enum
{
    MSC_STAGE_IDLE = 0,
    MSC_STAGE_CMD,
    MSC_STAGE_DATA,
    MSC_STAGE_STATUS,
    MSC_STAGE_STATUS_RETRY, // BOT CSW retry after 0-length or STALL
};

// Recovery state machine.
// BOT: BOT_RESET -> CLEAR_IN -> CLEAR_OUT -> NONE.
// CBI: CBI_RESET -> CLEAR_IN -> CLEAR_OUT -> NONE.
enum
{
    RECOVERY_NONE = 0,
    RECOVERY_BOT_RESET,
    RECOVERY_CBI_RESET,
    RECOVERY_CLEAR_IN,
    RECOVERY_CLEAR_OUT,
};

typedef struct
{
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t ep_intr;  // CBI interrupt endpoint (0 if BOT)
    uint8_t protocol; // MSC_PROTOCOL_BOT or MSC_PROTOCOL_CBI*
    uint8_t subclass; // MSC_SUBCLASS_UFI, MSC_SUBCLASS_SFF, etc.

    volatile bool configured;
    volatile bool mounted;

    uint8_t stage;
    uint8_t recovery_stage;
    void *buffer;
    tuh_msc_complete_cb_t complete_cb;
    uintptr_t complete_arg;
} msch_interface_t;

typedef struct
{
    TUH_EPBUF_TYPE_DEF(msc_cbw_t, cbw);
    TUH_EPBUF_TYPE_DEF(msc_csw_t, csw);
    TUH_EPBUF_DEF(cbi_cmd, 12);   // CBI ADSC command buffer (UFI = 12 bytes)
    TUH_EPBUF_DEF(cbi_status, 2); // CBI interrupt status (2 bytes)
} msch_epbuf_t;

static msch_interface_t _msch_itf[CFG_TUH_DEVICE_MAX];
CFG_TUH_MEM_SECTION static msch_epbuf_t _msch_epbuf[CFG_TUH_DEVICE_MAX];

TU_ATTR_ALWAYS_INLINE static inline msch_interface_t *get_itf(uint8_t daddr)
{
    return &_msch_itf[daddr - 1];
}

TU_ATTR_ALWAYS_INLINE static inline msch_epbuf_t *get_epbuf(uint8_t daddr)
{
    return &_msch_epbuf[daddr - 1];
}

TU_ATTR_ALWAYS_INLINE static inline bool is_cbi(msch_interface_t const *p_msc)
{
    return p_msc->protocol == MSC_PROTOCOL_CBI ||
           p_msc->protocol == MSC_PROTOCOL_CBI_NO_INTERRUPT;
}

// Resolve data endpoint from CBW direction.
TU_ATTR_ALWAYS_INLINE static inline uint8_t data_ep(msch_interface_t const *p_msc,
                                                    msc_cbw_t const *cbw)
{
    return (cbw->dir & TUSB_DIR_IN_MASK) ? p_msc->ep_in : p_msc->ep_out;
}

// Fabricate a CSW, set stage to IDLE, and invoke the completion callback.
static void _complete(uint8_t daddr, uint8_t csw_status, uint32_t data_residue)
{
    msch_interface_t *p_msc = get_itf(daddr);
    msch_epbuf_t *epbuf = get_epbuf(daddr);
    p_msc->stage = MSC_STAGE_IDLE;
    if (p_msc->complete_cb)
    {
        epbuf->csw.signature = MSC_CSW_SIGNATURE;
        epbuf->csw.tag = epbuf->cbw.tag;
        epbuf->csw.data_residue = data_residue;
        epbuf->csw.status = csw_status;
        tuh_msc_complete_data_t const cb_data = {
            .cbw = &epbuf->cbw,
            .csw = &epbuf->csw,
            .scsi_data = p_msc->buffer,
            .user_arg = p_msc->complete_arg};
        p_msc->complete_cb(daddr, &cb_data);
    }
}

// Submit data-phase transfer or complete with failure.
static void _start_data_phase(uint8_t daddr, msch_interface_t *p_msc,
                              msc_cbw_t const *cbw)
{
    p_msc->stage = MSC_STAGE_DATA;
    uint16_t xfer_len = (cbw->total_bytes > UINT16_MAX)
                            ? UINT16_MAX
                            : (uint16_t)cbw->total_bytes;
    if (!usbh_edpt_xfer(daddr, data_ep(p_msc, cbw), p_msc->buffer, xfer_len))
    {
        _complete(daddr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
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
    if (p_msc->recovery_stage != RECOVERY_NONE)
        return false;
    if (usbh_edpt_busy(dev_addr, p_msc->ep_in))
        return false;
    if (usbh_edpt_busy(dev_addr, p_msc->ep_out))
        return false;
    return true;
}

bool tuh_msc_is_cbi(uint8_t dev_addr)
{
    return is_cbi(get_itf(dev_addr));
}

//--------------------------------------------------------------------+
// Recovery State Machine
//--------------------------------------------------------------------+
static void _recovery_cb(tuh_xfer_t *xfer);

static bool _recovery_clear_halt(uint8_t daddr, uint8_t ep_addr)
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
        .complete_cb = _recovery_cb,
        .user_data = 0};
    return tuh_control_xfer(&xfer);
}

static void _cancel_inflight(uint8_t dev_addr)
{
    msch_interface_t *p_msc = get_itf(dev_addr);

    // If a CBI ADSC control transfer is in-flight, abort it and release
    // the interrupt EP pause that tuh_msc_scsi_command() acquired.
    if (p_msc->stage == MSC_STAGE_CMD && is_cbi(p_msc))
    {
        tuh_edpt_abort_xfer(dev_addr, 0);
        hcd_resume_interrupt_eps(usbh_get_rhport(dev_addr));
    }

    tuh_edpt_abort_xfer(dev_addr, p_msc->ep_in);
    tuh_edpt_abort_xfer(dev_addr, p_msc->ep_out);
    if (p_msc->ep_intr)
    {
        tuh_edpt_abort_xfer(dev_addr, p_msc->ep_intr);
    }

    p_msc->stage = MSC_STAGE_IDLE;
}

// End recovery and resume interrupt EP polling.
static void _recovery_done(uint8_t daddr, msch_interface_t *p_msc)
{
    p_msc->recovery_stage = RECOVERY_NONE;
    hcd_resume_interrupt_eps(usbh_get_rhport(daddr));
}

static void _recovery_cb(tuh_xfer_t *xfer)
{
    uint8_t const daddr = xfer->daddr;
    msch_interface_t *p_msc = get_itf(daddr);

    if (xfer->result != XFER_RESULT_SUCCESS)
    {
        _recovery_done(daddr, p_msc);
        return;
    }

    uint8_t const rhport = usbh_get_rhport(daddr);

    switch (p_msc->recovery_stage)
    {
    case RECOVERY_BOT_RESET:
    case RECOVERY_CBI_RESET:
        p_msc->recovery_stage = RECOVERY_CLEAR_IN;
        if (!_recovery_clear_halt(daddr, p_msc->ep_in))
        {
            _recovery_done(daddr, p_msc);
        }
        break;

    case RECOVERY_CLEAR_IN:
        hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_in);
        p_msc->recovery_stage = RECOVERY_CLEAR_OUT;
        if (!_recovery_clear_halt(daddr, p_msc->ep_out))
        {
            _recovery_done(daddr, p_msc);
        }
        break;

    case RECOVERY_CLEAR_OUT:
        hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_out);
        _recovery_done(daddr, p_msc);
        break;

    default:
        _recovery_done(daddr, p_msc);
        break;
    }
}

bool tuh_msc_recovery_in_progress(uint8_t dev_addr)
{
    return get_itf(dev_addr)->recovery_stage != RECOVERY_NONE;
}

void tuh_msc_abort_recovery(uint8_t dev_addr)
{
    msch_interface_t *p_msc = get_itf(dev_addr);
    if (p_msc->recovery_stage == RECOVERY_NONE)
        return;
    tuh_edpt_abort_xfer(dev_addr, 0);
    _recovery_done(dev_addr, p_msc);
}

bool tuh_msc_reset_recovery(uint8_t dev_addr)
{
    msch_interface_t *p_msc = get_itf(dev_addr);
    if (!p_msc->configured)
        return false;

    _cancel_inflight(dev_addr);

    // Pause interrupt EP polling for the entire recovery chain.
    // Every step uses control transfers on the shared EPX.
    uint8_t const rhport = usbh_get_rhport(dev_addr);
    hcd_pause_interrupt_eps(rhport);

    if (is_cbi(p_msc))
    {
        // CBI reset: SEND_DIAGNOSTIC(SelfTest=1) via ADSC, then clear bulk
        // endpoints.  Matches Linux usb-storage usb_stor_CB_reset().
        msch_epbuf_t *epbuf = get_epbuf(dev_addr);
        memset(epbuf->cbi_cmd, 0xFF, 12);
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
            .daddr = dev_addr,
            .ep_addr = 0,
            .setup = &request,
            .buffer = epbuf->cbi_cmd,
            .complete_cb = _recovery_cb,
            .user_data = 0};
        p_msc->recovery_stage = RECOVERY_CBI_RESET;
        if (!tuh_control_xfer(&xfer))
        {
            // ADSC failed — fall back to just clearing endpoints
            p_msc->recovery_stage = RECOVERY_CLEAR_IN;
            if (!_recovery_clear_halt(dev_addr, p_msc->ep_in))
            {
                _recovery_done(dev_addr, p_msc);
            }
        }
        return true;
    }

    // BOT: Bulk-Only Mass Storage Reset, then clear halts.
    tusb_control_request_t const request = {
        .bmRequestType_bit = {
            .recipient = TUSB_REQ_RCPT_INTERFACE,
            .type = TUSB_REQ_TYPE_CLASS,
            .direction = TUSB_DIR_OUT},
        .bRequest = 0xFF, // Bulk-Only Mass Storage Reset
        .wValue = 0,
        .wIndex = p_msc->itf_num,
        .wLength = 0};
    tuh_xfer_t xfer = {
        .daddr = dev_addr,
        .ep_addr = 0,
        .setup = &request,
        .buffer = NULL,
        .complete_cb = _recovery_cb,
        .user_data = 0};
    p_msc->recovery_stage = RECOVERY_BOT_RESET;
    if (!tuh_control_xfer(&xfer))
    {
        _recovery_done(dev_addr, p_msc);
        return false;
    }

    return true;
}

//--------------------------------------------------------------------+
// CBI (Control/Bulk/Interrupt) Transport
//--------------------------------------------------------------------+
static void cbi_adsc_complete(tuh_xfer_t *xfer);

static void cbi_adsc_complete(tuh_xfer_t *xfer)
{
    uint8_t const daddr = xfer->daddr;
    msch_interface_t *p_msc = get_itf(daddr);
    msch_epbuf_t *epbuf = get_epbuf(daddr);

    // Resume interrupt EP polling paused in cbi_scsi_command().
    hcd_resume_interrupt_eps(usbh_get_rhport(daddr));

    if (XFER_RESULT_SUCCESS != xfer->result)
    {
        _complete(daddr, MSC_CSW_STATUS_FAILED, epbuf->cbw.total_bytes);
        return;
    }

    // ADSC succeeded — start data phase or status phase
    msc_cbw_t const *cbw = &epbuf->cbw;
    if (cbw->total_bytes && p_msc->buffer)
    {
        _start_data_phase(daddr, p_msc, cbw);
    }
    else if (p_msc->ep_intr)
    {
        // No data — read interrupt status directly
        p_msc->stage = MSC_STAGE_STATUS;
        if (!usbh_edpt_xfer(daddr, p_msc->ep_intr, epbuf->cbi_status, 2))
        {
            _complete(daddr, MSC_CSW_STATUS_FAILED, 0);
        }
    }
    else
    {
        // CBI_NO_INTERRUPT with no data — assume success
        _complete(daddr, MSC_CSW_STATUS_PASSED, 0);
    }
}

//--------------------------------------------------------------------+
// PUBLIC API: SCSI COMMAND
//--------------------------------------------------------------------+
bool tuh_msc_scsi_command(uint8_t daddr, msc_cbw_t const *cbw, void *data,
                          tuh_msc_complete_cb_t complete_cb, uintptr_t arg)
{
    msch_interface_t *p_msc = get_itf(daddr);
    TU_VERIFY(p_msc->configured);
    TU_VERIFY(p_msc->stage == MSC_STAGE_IDLE);
    msch_epbuf_t *epbuf = get_epbuf(daddr);

    epbuf->cbw = *cbw;
    p_msc->buffer = data;
    p_msc->complete_cb = complete_cb;
    p_msc->complete_arg = arg;
    p_msc->stage = MSC_STAGE_CMD;

    if (is_cbi(p_msc))
    {
        // CBI: send CDB via ADSC (Accept Device-Specific Command) control request.
        tu_memclr(epbuf->cbi_cmd, 12);
        uint8_t cmd_len = cbw->cmd_len;
        if (cmd_len > 12)
            cmd_len = 12;
        memcpy(epbuf->cbi_cmd, cbw->command, cmd_len);

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
            .complete_cb = cbi_adsc_complete,
            .user_data = arg};

        // Pause interrupt EP polling while the ADSC control transfer is
        // in-flight.  The RP2040 SIE shares EPX for control and interrupt
        // endpoints; concurrent polling can delay the STATUS phase.
        hcd_pause_interrupt_eps(usbh_get_rhport(daddr));

        if (!tuh_control_xfer(&xfer))
        {
            p_msc->stage = MSC_STAGE_IDLE;
            hcd_resume_interrupt_eps(usbh_get_rhport(daddr));
            return false;
        }
        return true;
    }

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
    TU_VERIFY(dev_addr <= CFG_TUH_DEVICE_MAX, );
    msch_interface_t *p_msc = get_itf(dev_addr);
    TU_VERIFY(p_msc->configured, );

    TU_LOG_DRV("  MSCh close addr = %d\r\n", dev_addr);

    _cancel_inflight(dev_addr);

    // Force-resume resets the pause refcount to zero so stale pauses
    // from killed recovery callbacks don't accumulate.
    hcd_force_resume_interrupt_eps(usbh_get_rhport(dev_addr));

    p_msc->recovery_stage = RECOVERY_NONE;

    if (p_msc->ep_in)
        tuh_edpt_close(dev_addr, p_msc->ep_in);
    if (p_msc->ep_out)
        tuh_edpt_close(dev_addr, p_msc->ep_out);
    if (p_msc->ep_intr)
        tuh_edpt_close(dev_addr, p_msc->ep_intr);

    if (p_msc->mounted)
    {
        tuh_msc_umount_cb(dev_addr);
    }

    tu_memclr(p_msc, sizeof(msch_interface_t));
}

bool msch_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t event, uint32_t xferred_bytes)
{
    msch_interface_t *p_msc = get_itf(dev_addr);
    msch_epbuf_t *epbuf = get_epbuf(dev_addr);
    msc_cbw_t const *cbw = &epbuf->cbw;
    msc_csw_t *csw = &epbuf->csw;

    // CBI transport
    if (is_cbi(p_msc))
    {
        switch (p_msc->stage)
        {
        case MSC_STAGE_DATA:
            // CBI spec §2.4.3.1.3: clear bulk pipe after data STALL
            if (event == XFER_RESULT_STALLED)
            {
                hcd_edpt_clear_stall(usbh_get_rhport(dev_addr), dev_addr, data_ep(p_msc, cbw));
            }
            if (event != XFER_RESULT_SUCCESS || !p_msc->ep_intr)
            {
                uint8_t status = (event == XFER_RESULT_SUCCESS) ? MSC_CSW_STATUS_PASSED : MSC_CSW_STATUS_FAILED;
                _complete(dev_addr, status, cbw->total_bytes - xferred_bytes);
            }
            else
            {
                // Data succeeded — read interrupt status
                p_msc->stage = MSC_STAGE_STATUS;
                if (!usbh_edpt_xfer(dev_addr, p_msc->ep_intr, epbuf->cbi_status, 2))
                {
                    _complete(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes - xferred_bytes);
                }
            }
            break;

        case MSC_STAGE_STATUS:
        {
            // CBI interrupt status received (2 bytes)
            uint8_t csw_status;
            if (event != XFER_RESULT_SUCCESS || xferred_bytes < 1)
            {
                csw_status = MSC_CSW_STATUS_FAILED;
            }
            else if (p_msc->subclass == MSC_SUBCLASS_UFI)
            {
                // UFI: byte 0 = ASC, byte 1 = ASCQ. ASC == 0 means success.
                csw_status = (epbuf->cbi_status[0] == 0) ? MSC_CSW_STATUS_PASSED : MSC_CSW_STATUS_FAILED;
            }
            else
            {
                // SFF-8070i and others: byte 0 = bType (must be 0x00),
                // byte 1 bits [1:0] encode status per CBI spec §3.4.3.1.2:
                //   0x00 = pass, 0x01 = fail, 0x02 = phase error.
                if (epbuf->cbi_status[0] != 0)
                {
                    csw_status = MSC_CSW_STATUS_FAILED;
                }
                else
                {
                    switch (epbuf->cbi_status[1] & 0x03)
                    {
                    case 0x00:
                        csw_status = MSC_CSW_STATUS_PASSED;
                        break;
                    case 0x02:
                        csw_status = MSC_CSW_STATUS_PHASE_ERROR;
                        break;
                    default:
                        csw_status = MSC_CSW_STATUS_FAILED;
                        break;
                    }
                }
            }
            _complete(dev_addr, csw_status, 0);
            break;
        }

        default:
            break;
        }
        return true;
    }

    // BOT transport
    switch (p_msc->stage)
    {
    case MSC_STAGE_CMD:
        if (ep_addr != p_msc->ep_out || event != XFER_RESULT_SUCCESS || xferred_bytes != sizeof(msc_cbw_t))
        {
            _complete(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
            break;
        }
        if (cbw->total_bytes && p_msc->buffer)
        {
            _start_data_phase(dev_addr, p_msc, cbw);
            break;
        }
        TU_ATTR_FALLTHROUGH;

    case MSC_STAGE_DATA:
        // BOT spec §6.7.2: if data endpoint STALLs, clear it then read CSW.
        if (event == XFER_RESULT_STALLED)
        {
            hcd_edpt_clear_stall(usbh_get_rhport(dev_addr), dev_addr, data_ep(p_msc, cbw));
        }
        // Read CSW
        p_msc->stage = MSC_STAGE_STATUS;
        if (!usbh_edpt_xfer(dev_addr, p_msc->ep_in, (uint8_t *)csw, (uint16_t)sizeof(msc_csw_t)))
        {
            _complete(dev_addr, MSC_CSW_STATUS_FAILED, cbw->total_bytes);
        }
        break;

    case MSC_STAGE_STATUS:
    case MSC_STAGE_STATUS_RETRY:
    {
        bool const is_retry = (p_msc->stage == MSC_STAGE_STATUS_RETRY);

        // BOT spec §6.7.2: 0-length CSW or CSW STALL — retry CSW read once.
        if (!is_retry)
        {
            if (event == XFER_RESULT_SUCCESS && xferred_bytes == 0)
            {
                TU_LOG_DRV("  MSC BOT: 0-length CSW, retrying\r\n");
                p_msc->stage = MSC_STAGE_STATUS_RETRY;
                if (usbh_edpt_xfer(dev_addr, p_msc->ep_in, (uint8_t *)csw, (uint16_t)sizeof(msc_csw_t)))
                {
                    break;
                }
            }
            if (event == XFER_RESULT_STALLED)
            {
                TU_LOG_DRV("  MSC BOT: CSW STALL, clearing and retrying\r\n");
                hcd_edpt_clear_stall(usbh_get_rhport(dev_addr), dev_addr, p_msc->ep_in);
                p_msc->stage = MSC_STAGE_STATUS_RETRY;
                if (usbh_edpt_xfer(dev_addr, p_msc->ep_in, (uint8_t *)csw, (uint16_t)sizeof(msc_csw_t)))
                {
                    break;
                }
            }
        }

        // Validate CSW per BOT spec §6.3
        p_msc->stage = MSC_STAGE_IDLE;
        bool csw_valid = (event == XFER_RESULT_SUCCESS &&
                          xferred_bytes == sizeof(msc_csw_t) &&
                          csw->signature == MSC_CSW_SIGNATURE &&
                          csw->tag == cbw->tag);
        if (!csw_valid)
        {
            csw->status = MSC_CSW_STATUS_FAILED;
        }
        if (p_msc->complete_cb != NULL)
        {
            tuh_msc_complete_data_t const cb_data = {
                .cbw = cbw,
                .csw = csw,
                .scsi_data = p_msc->buffer,
                .user_arg = p_msc->complete_arg};
            (void)p_msc->complete_cb(dev_addr, &cb_data);
        }
        break;
    }

    default:
        break;
    }

    return true;
}

//--------------------------------------------------------------------+
// MSC Enumeration
//--------------------------------------------------------------------+

uint16_t msch_open(uint8_t rhport, uint8_t dev_addr, const tusb_desc_interface_t *desc_itf, uint16_t max_len)
{
    (void)rhport;

    TU_VERIFY(MSC_PROTOCOL_BOT == desc_itf->bInterfaceProtocol ||
                  MSC_PROTOCOL_CBI == desc_itf->bInterfaceProtocol ||
                  MSC_PROTOCOL_CBI_NO_INTERRUPT == desc_itf->bInterfaceProtocol,
              0);

    TU_VERIFY(MSC_SUBCLASS_SCSI == desc_itf->bInterfaceSubClass ||
                  MSC_SUBCLASS_UFI == desc_itf->bInterfaceSubClass ||
                  MSC_SUBCLASS_SFF == desc_itf->bInterfaceSubClass,
              0);

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
    p_msc->ep_intr = 0;

    // Linux unusual_devs.h quirk: force CBI_NO_INTERRUPT for known devices.
    //   0x0644:0x0000  TEAC Floppy Drive
    //   0x04e6:0x0001  Matshita LS-120
    //   0x04e6:0x0007  Sony Hifd
    bool force_no_intr = false;
    if (p_msc->protocol == MSC_PROTOCOL_CBI)
    {
        uint16_t vid, pid;
        tuh_vid_pid_get(dev_addr, &vid, &pid);
        if ((vid == 0x0644 && pid == 0x0000) ||
            (vid == 0x04e6 && pid == 0x0001) ||
            (vid == 0x04e6 && pid == 0x0007))
        {
            p_msc->protocol = MSC_PROTOCOL_CBI_NO_INTERRUPT;
            force_no_intr = true;
        }
    }

    uint8_t const *p_desc = tu_desc_next(desc_itf);
    uint8_t const *desc_end = ((uint8_t const *)desc_itf) + drv_len;
    uint8_t ep_count = 0;

    while (ep_count < desc_itf->bNumEndpoints && p_desc < desc_end)
    {
        if (tu_desc_type(p_desc) != TUSB_DESC_ENDPOINT)
        {
            p_desc = tu_desc_next(p_desc);
            continue;
        }
        ep_count++;
        tusb_desc_endpoint_t const *ep_desc = (tusb_desc_endpoint_t const *)p_desc;

        if (force_no_intr && TUSB_XFER_INTERRUPT == ep_desc->bmAttributes.xfer)
        {
            p_desc = tu_desc_next(p_desc);
            continue;
        }

        TU_ASSERT(tuh_edpt_open(dev_addr, ep_desc), 0);

        if (TUSB_XFER_BULK == ep_desc->bmAttributes.xfer)
        {
            if (TUSB_DIR_IN == tu_edpt_dir(ep_desc->bEndpointAddress))
            {
                p_msc->ep_in = ep_desc->bEndpointAddress;
            }
            else
            {
                p_msc->ep_out = ep_desc->bEndpointAddress;
            }
        }
        else if (TUSB_XFER_INTERRUPT == ep_desc->bmAttributes.xfer)
        {
            p_msc->ep_intr = ep_desc->bEndpointAddress;
        }

        p_desc = tu_desc_next(p_desc);
    }

    // Verify required bulk endpoints were found
    TU_ASSERT(p_msc->ep_in, 0);
    TU_ASSERT(p_msc->ep_out, 0);

    p_msc->itf_num = desc_itf->bInterfaceNumber;

    return drv_len;
}

bool msch_set_config(uint8_t daddr, uint8_t itf_num)
{
    msch_interface_t *p_msc = get_itf(daddr);
    TU_ASSERT(p_msc->itf_num == itf_num);
    p_msc->configured = true;
    p_msc->mounted = true;
    tuh_msc_mount_cb(daddr);
    usbh_driver_set_config_complete(daddr, p_msc->itf_num);
    return true;
}

#endif
