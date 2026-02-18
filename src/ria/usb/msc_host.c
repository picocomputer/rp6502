/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
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

- Add CBI (Control/Bulk/Interrupt) transport support: new ep_intr and protocol
  fields in msch_interface_t, cbi_cmd/cbi_status buffers in msch_epbuf_t,
  cbi_scsi_command() and cbi_adsc_complete() functions, and CBI branch in
  msch_xfer_cb() handling data-complete and interrupt-status stages.
- tuh_msc_scsi_command() routes to CBI or BOT transport based on protocol.
- tuh_msc_ready() explicitly checks stage==IDLE in addition to endpoint busy.
- msch_open() accepts CBI/CBI_NO_INTERRUPT protocols and UFI/SFF subclasses;
  iterates bNumEndpoints (not fixed 2) to handle bulk + interrupt endpoints.
- msch_set_config() skips GET_MAX_LUN and all SCSI enumeration (TUR, Read
  Capacity). msc.c handles everything in disk_initialize/msc_init_volume
  where the control pipe is guaranteed free.
- max_lun, capacity[], tuh_msc_get_maxlun(), tuh_msc_get_block_count(),
  tuh_msc_get_block_size(), tuh_msc_read10(), tuh_msc_write10() all removed:
  msc.c owns LUN enumeration and block size/count tracking. Add multi-LUN
  support there, not here.
- tuh_msc_reset_recovery() aborts any in-flight transfers and resets the
  MSC stage to idle.  For BOT devices it starts an async recovery state
  machine (BOT Mass Storage Reset -> CLEAR_FEATURE on ep_in -> CLEAR_FEATURE
  on ep_out) driven entirely by callbacks — no spin-wait, no nested
  tuh_task().  Use tuh_msc_recovery_in_progress() to poll completion.

*/

#include "tusb_option.h"

#if CFG_TUH_ENABLED && CFG_TUH_MSC

#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "host/hcd.h"

#include "class/msc/msc_host.h"

#include "pico/time.h"

// Level where CFG_TUSB_DEBUG must be at least for this driver is logged
#ifndef CFG_TUH_MSC_LOG_LEVEL
  #define CFG_TUH_MSC_LOG_LEVEL   CFG_TUH_LOG_LEVEL
#endif

#define TU_LOG_DRV(...)   TU_LOG(CFG_TUH_MSC_LOG_LEVEL, __VA_ARGS__)

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF
//--------------------------------------------------------------------+
enum {
  MSC_STAGE_IDLE = 0,
  MSC_STAGE_CMD,
  MSC_STAGE_DATA,
  MSC_STAGE_STATUS,
};

// Recovery states for tuh_msc_reset_recovery() state machine.
// BOT recovery chains: RESET -> CLEAR_IN -> CLEAR_OUT -> NONE (done).
enum {
  RECOVERY_NONE = 0,
  RECOVERY_BOT_RESET,
  RECOVERY_CLEAR_IN,
  RECOVERY_CLEAR_OUT,
};

typedef struct {
  uint8_t itf_num;
  uint8_t ep_in;
  uint8_t ep_out;
  uint8_t ep_intr;   // CBI interrupt endpoint (0 if BOT)
  uint8_t protocol;  // MSC_PROTOCOL_BOT or MSC_PROTOCOL_CBI*

  volatile bool configured; // Receive SET_CONFIGURE
  volatile bool mounted;    // Enumeration is complete

  // SCSI command data
  uint8_t stage;
  uint8_t recovery_stage; // BOT reset recovery state machine
  void* buffer;
  tuh_msc_complete_cb_t complete_cb;
  uintptr_t complete_arg;
} msch_interface_t;

typedef struct {
  TUH_EPBUF_TYPE_DEF(msc_cbw_t, cbw);
  TUH_EPBUF_TYPE_DEF(msc_csw_t, csw);
  TUH_EPBUF_DEF(cbi_cmd, 12);    // CBI ADSC command buffer (UFI = 12 bytes)
  TUH_EPBUF_DEF(cbi_status, 2);  // CBI interrupt status (2 bytes)
} msch_epbuf_t;

static msch_interface_t _msch_itf[CFG_TUH_DEVICE_MAX];
CFG_TUH_MEM_SECTION static msch_epbuf_t _msch_epbuf[CFG_TUH_DEVICE_MAX];

TU_ATTR_ALWAYS_INLINE static inline msch_interface_t* get_itf(uint8_t daddr) {
  return &_msch_itf[daddr - 1];
}

TU_ATTR_ALWAYS_INLINE static inline msch_epbuf_t* get_epbuf(uint8_t daddr) {
  return &_msch_epbuf[daddr - 1];
}

//--------------------------------------------------------------------+
// Weak stubs: invoked if no strong implementation is available
//--------------------------------------------------------------------+
TU_ATTR_WEAK void tuh_msc_mount_cb(uint8_t dev_addr) {
  (void) dev_addr;
}

TU_ATTR_WEAK void tuh_msc_umount_cb(uint8_t dev_addr) {
  (void) dev_addr;
}

//--------------------------------------------------------------------+
// PUBLIC API
//--------------------------------------------------------------------+
bool tuh_msc_mounted(uint8_t dev_addr) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  return p_msc->mounted;
}

bool tuh_msc_ready(uint8_t dev_addr) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  TU_VERIFY(p_msc->mounted);
  if (p_msc->stage != MSC_STAGE_IDLE) return false;
  // Block while async CBI recovery (clear-halt chain) is using
  // the control pipe — otherwise ADSC submissions will fail.
  if (p_msc->recovery_stage != RECOVERY_NONE) return false;
  if (usbh_edpt_busy(dev_addr, p_msc->ep_in)) return false;
  if (usbh_edpt_busy(dev_addr, p_msc->ep_out)) return false;
  return true;
}

bool tuh_msc_is_cbi(uint8_t dev_addr) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  return p_msc->protocol == MSC_PROTOCOL_CBI || p_msc->protocol == MSC_PROTOCOL_CBI_NO_INTERRUPT;
}

//--------------------------------------------------------------------+
// Recovery State Machine (BOT reset + clear halt)
//--------------------------------------------------------------------+
// Fully async — each step fires a callback that starts the next
// control transfer.  No spin-wait, no nested tuh_task().

static void _recovery_cb(tuh_xfer_t* xfer);

// Send CLEAR_FEATURE(ENDPOINT_HALT) as part of recovery chain.
static bool _recovery_clear_halt(uint8_t daddr, uint8_t ep_addr) {
  tusb_control_request_t const request = {
      .bmRequestType_bit = {
          .recipient = TUSB_REQ_RCPT_ENDPOINT,
          .type      = TUSB_REQ_TYPE_STANDARD,
          .direction = TUSB_DIR_OUT
      },
      .bRequest = TUSB_REQ_CLEAR_FEATURE,
      .wValue   = TUSB_REQ_FEATURE_EDPT_HALT,
      .wIndex   = ep_addr,
      .wLength  = 0
  };
  tuh_xfer_t xfer = {
      .daddr       = daddr,
      .ep_addr     = 0,
      .setup       = &request,
      .buffer      = NULL,
      .complete_cb = _recovery_cb,
      .user_data   = 0
  };
  return tuh_control_xfer(&xfer);
}

// Callback for each step of the BOT recovery chain.
// The control pipe is idle when this fires, so the next
// tuh_control_xfer() will succeed unless the device is gone.
static void _recovery_cb(tuh_xfer_t* xfer) {
  uint8_t const daddr = xfer->daddr;
  msch_interface_t* p_msc = get_itf(daddr);

  if (xfer->result != XFER_RESULT_SUCCESS) {
    // Transfer failed (device gone, STALL, etc.) — abandon recovery.
    p_msc->recovery_stage = RECOVERY_NONE;
    return;
  }

  uint8_t const rhport = usbh_get_rhport(daddr);

  switch (p_msc->recovery_stage) {
    case RECOVERY_BOT_RESET:
      // BOT Mass Storage Reset done — clear halt on bulk-in.
      p_msc->recovery_stage = RECOVERY_CLEAR_IN;
      if (!_recovery_clear_halt(daddr, p_msc->ep_in)) {
        p_msc->recovery_stage = RECOVERY_NONE;
      }
      break;

    case RECOVERY_CLEAR_IN:
      // ep_in clear halt done — reset host-side data toggle, start ep_out.
      hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_in);
      p_msc->recovery_stage = RECOVERY_CLEAR_OUT;
      if (!_recovery_clear_halt(daddr, p_msc->ep_out)) {
        p_msc->recovery_stage = RECOVERY_NONE;
      }
      break;

    case RECOVERY_CLEAR_OUT:
      // ep_out clear halt done — reset host-side data toggle.  Recovery complete.
      hcd_edpt_clear_stall(rhport, daddr, p_msc->ep_out);
      p_msc->recovery_stage = RECOVERY_NONE;
      break;

    default:
      p_msc->recovery_stage = RECOVERY_NONE;
      break;
  }
}

bool tuh_msc_recovery_in_progress(uint8_t dev_addr) {
  return get_itf(dev_addr)->recovery_stage != RECOVERY_NONE;
}

bool tuh_msc_reset_recovery(uint8_t dev_addr) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  if (!p_msc->configured) return false;

  bool const is_cbi = (p_msc->protocol == MSC_PROTOCOL_CBI ||
                       p_msc->protocol == MSC_PROTOCOL_CBI_NO_INTERRUPT);

  // For CBI: if the ADSC control transfer is in-flight (stage == CMD),
  // abort the control pipe.  Otherwise the shared EP0 stays stuck and
  // ALL subsequent USB control transfers (to any device) hang.
  if (is_cbi && p_msc->stage == MSC_STAGE_CMD) {
    tuh_edpt_abort_xfer(dev_addr, 0);
  }

  // Abort any in-flight bulk transfers and reset MSC stage
  tuh_edpt_abort_xfer(dev_addr, p_msc->ep_in);
  tuh_edpt_abort_xfer(dev_addr, p_msc->ep_out);
  if (is_cbi && p_msc->ep_intr) {
    tuh_edpt_abort_xfer(dev_addr, p_msc->ep_intr);
  }
  p_msc->stage = MSC_STAGE_IDLE;

  if (is_cbi) {
    // CBI has no bulk-only reset, but we must clear halt on the
    // bulk endpoints to reset data toggles.  Without this, a PID
    // mismatch after an aborted transfer prevents subsequent SCSI
    // commands (e.g. REQUEST SENSE) from completing.
    p_msc->recovery_stage = RECOVERY_CLEAR_IN;
    if (!_recovery_clear_halt(dev_addr, p_msc->ep_in)) {
      p_msc->recovery_stage = RECOVERY_NONE;
    }
    return true;
  }

  // BOT transport: start async Bulk-Only Mass Storage Reset.
  // Recovery proceeds via _recovery_cb chain:
  //   BOT_RESET -> CLEAR_IN -> CLEAR_OUT -> NONE (done)
  tusb_control_request_t const request = {
      .bmRequestType_bit = {
          .recipient = TUSB_REQ_RCPT_INTERFACE,
          .type      = TUSB_REQ_TYPE_CLASS,
          .direction = TUSB_DIR_OUT
      },
      .bRequest = 0xFF, // Bulk-Only Mass Storage Reset
      .wValue   = 0,
      .wIndex   = p_msc->itf_num,
      .wLength  = 0
  };
  tuh_xfer_t xfer = {
      .daddr       = dev_addr,
      .ep_addr     = 0,
      .setup       = &request,
      .buffer      = NULL,
      .complete_cb = _recovery_cb,
      .user_data   = 0
  };
  p_msc->recovery_stage = RECOVERY_BOT_RESET;
  if (!tuh_control_xfer(&xfer)) {
    p_msc->recovery_stage = RECOVERY_NONE;
    return false;
  }

  return true;
}

//--------------------------------------------------------------------+
// CBI (Control/Bulk/Interrupt) Transport
//--------------------------------------------------------------------+
static void cbi_adsc_complete(tuh_xfer_t* xfer);

static bool cbi_scsi_command(uint8_t daddr, msc_cbw_t const* cbw, void* data,
                             tuh_msc_complete_cb_t complete_cb, uintptr_t arg) {
  msch_interface_t* p_msc = get_itf(daddr);
  TU_VERIFY(p_msc->configured);
  TU_VERIFY(p_msc->stage == MSC_STAGE_IDLE);
  msch_epbuf_t* epbuf = get_epbuf(daddr);

  // Save the CBW so callbacks can access lun, dir, total_bytes
  epbuf->cbw = *cbw;
  p_msc->buffer = data;
  p_msc->complete_cb = complete_cb;
  p_msc->complete_arg = arg;
  p_msc->stage = MSC_STAGE_CMD;

  // Copy command bytes to CBI command buffer (12 bytes for UFI)
  tu_memclr(epbuf->cbi_cmd, 12);
  uint8_t cmd_len = cbw->cmd_len;
  if (cmd_len > 12) cmd_len = 12;
  memcpy(epbuf->cbi_cmd, cbw->command, cmd_len);

  // Send ADSC (Accept Device-Specific Command) via control transfer
  tusb_control_request_t const request = {
      .bmRequestType_bit = {
          .recipient = TUSB_REQ_RCPT_INTERFACE,
          .type      = TUSB_REQ_TYPE_CLASS,
          .direction = TUSB_DIR_OUT
      },
      .bRequest = 0, // ADSC
      .wValue   = 0,
      .wIndex   = p_msc->itf_num,
      .wLength  = 12
  };

  tuh_xfer_t xfer = {
      .daddr       = daddr,
      .ep_addr     = 0,
      .setup       = &request,
      .buffer      = epbuf->cbi_cmd,
      .complete_cb = cbi_adsc_complete,
      .user_data   = arg
  };

  // Submit the ADSC control transfer.  If the control pipe is
  // momentarily busy (hub status query, enumeration), return false.
  // The caller's retry logic will re-attempt the SCSI command.
  if (!tuh_control_xfer(&xfer)) {
    p_msc->stage = MSC_STAGE_IDLE;
    return false;
  }
  return true;
}

static void cbi_adsc_complete(tuh_xfer_t* xfer) {
  uint8_t const daddr = xfer->daddr;
  msch_interface_t* p_msc = get_itf(daddr);
  msch_epbuf_t* epbuf = get_epbuf(daddr);

  if (XFER_RESULT_SUCCESS != xfer->result) {
    // ADSC failed - report as failed command
    p_msc->stage = MSC_STAGE_IDLE;
    if (p_msc->complete_cb) {
      epbuf->csw.signature = MSC_CSW_SIGNATURE;
      epbuf->csw.tag = epbuf->cbw.tag;
      epbuf->csw.data_residue = epbuf->cbw.total_bytes;
      epbuf->csw.status = MSC_CSW_STATUS_FAILED;
      tuh_msc_complete_data_t const cb_data = {
          .cbw = &epbuf->cbw,
          .csw = &epbuf->csw,
          .scsi_data = p_msc->buffer,
          .user_arg = p_msc->complete_arg
      };
      p_msc->complete_cb(daddr, &cb_data);
    }
    return;
  }

  // ADSC succeeded - start data phase if needed
  msc_cbw_t const* cbw = &epbuf->cbw;
  if (cbw->total_bytes && p_msc->buffer) {
    p_msc->stage = MSC_STAGE_DATA;
    uint8_t const ep_data = (cbw->dir & TUSB_DIR_IN_MASK) ? p_msc->ep_in : p_msc->ep_out;
    uint16_t xfer_len = (cbw->total_bytes > UINT16_MAX) ? UINT16_MAX : (uint16_t) cbw->total_bytes;
    if (!usbh_edpt_xfer(daddr, ep_data, p_msc->buffer, xfer_len)) {
      p_msc->stage = MSC_STAGE_IDLE;
      if (p_msc->complete_cb) {
        epbuf->csw.signature = MSC_CSW_SIGNATURE;
        epbuf->csw.tag = cbw->tag;
        epbuf->csw.data_residue = cbw->total_bytes;
        epbuf->csw.status = MSC_CSW_STATUS_FAILED;
        tuh_msc_complete_data_t const cb_data = {
            .cbw = cbw, .csw = &epbuf->csw,
            .scsi_data = p_msc->buffer, .user_arg = p_msc->complete_arg
        };
        p_msc->complete_cb(daddr, &cb_data);
      }
      return;
    }
  } else if (p_msc->ep_intr) {
    // No data, go straight to interrupt status
    p_msc->stage = MSC_STAGE_STATUS;
    if (!usbh_edpt_xfer(daddr, p_msc->ep_intr, epbuf->cbi_status, 2)) {
      p_msc->stage = MSC_STAGE_IDLE;
      if (p_msc->complete_cb) {
        epbuf->csw.signature = MSC_CSW_SIGNATURE;
        epbuf->csw.tag = cbw->tag;
        epbuf->csw.data_residue = 0;
        epbuf->csw.status = MSC_CSW_STATUS_FAILED;
        tuh_msc_complete_data_t const cb_data = {
            .cbw = cbw, .csw = &epbuf->csw,
            .scsi_data = p_msc->buffer, .user_arg = p_msc->complete_arg
        };
        p_msc->complete_cb(daddr, &cb_data);
      }
      return;
    }
  } else {
    // No data, no interrupt - assume success
    p_msc->stage = MSC_STAGE_IDLE;
    if (p_msc->complete_cb) {
      epbuf->csw.signature = MSC_CSW_SIGNATURE;
      epbuf->csw.tag = epbuf->cbw.tag;
      epbuf->csw.data_residue = 0;
      epbuf->csw.status = MSC_CSW_STATUS_PASSED;
      tuh_msc_complete_data_t const cb_data = {
          .cbw = &epbuf->cbw,
          .csw = &epbuf->csw,
          .scsi_data = p_msc->buffer,
          .user_arg = p_msc->complete_arg
      };
      p_msc->complete_cb(daddr, &cb_data);
    }
  }
}

//--------------------------------------------------------------------+
// PUBLIC API: SCSI COMMAND
//--------------------------------------------------------------------+
static inline void cbw_init(msc_cbw_t* cbw, uint8_t lun) {
  tu_memclr(cbw, sizeof(msc_cbw_t));
  cbw->signature = MSC_CBW_SIGNATURE;
  cbw->tag       = 0x54555342; // TUSB
  cbw->lun       = lun;
}

bool tuh_msc_scsi_command(uint8_t daddr, msc_cbw_t const* cbw, void* data,
                          tuh_msc_complete_cb_t complete_cb, uintptr_t arg) {
  msch_interface_t* p_msc = get_itf(daddr);
  TU_VERIFY(p_msc->configured);

  // CBI transport
  if (p_msc->protocol == MSC_PROTOCOL_CBI || p_msc->protocol == MSC_PROTOCOL_CBI_NO_INTERRUPT) {
    return cbi_scsi_command(daddr, cbw, data, complete_cb, arg);
  }

  // BOT transport
  TU_VERIFY(p_msc->stage == MSC_STAGE_IDLE);
  // claim endpoint
  TU_VERIFY(usbh_edpt_claim(daddr, p_msc->ep_out));
  msch_epbuf_t* epbuf = get_epbuf(daddr);

  epbuf->cbw = *cbw;
  p_msc->buffer = data;
  p_msc->complete_cb = complete_cb;
  p_msc->complete_arg = arg;
  p_msc->stage = MSC_STAGE_CMD;

  if (!usbh_edpt_xfer(daddr, p_msc->ep_out, (uint8_t*) &epbuf->cbw, sizeof(msc_cbw_t))) {
    p_msc->stage = MSC_STAGE_IDLE;
    (void) usbh_edpt_release(daddr, p_msc->ep_out);
    return false;
  }

  return true;
}

bool tuh_msc_read_capacity(uint8_t dev_addr, uint8_t lun, scsi_read_capacity10_resp_t* response,
                           tuh_msc_complete_cb_t complete_cb, uintptr_t arg) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  TU_VERIFY(p_msc->configured);

  msc_cbw_t cbw;
  cbw_init(&cbw, lun);

  cbw.total_bytes = sizeof(scsi_read_capacity10_resp_t);
  cbw.dir        = TUSB_DIR_IN_MASK;
  cbw.cmd_len    = sizeof(scsi_read_capacity10_t);
  cbw.command[0] = SCSI_CMD_READ_CAPACITY_10;

  return tuh_msc_scsi_command(dev_addr, &cbw, response, complete_cb, arg);
}

bool tuh_msc_inquiry(uint8_t dev_addr, uint8_t lun, scsi_inquiry_resp_t* response,
                     tuh_msc_complete_cb_t complete_cb, uintptr_t arg) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  TU_VERIFY(p_msc->configured);

  msc_cbw_t cbw;
  cbw_init(&cbw, lun);

  cbw.total_bytes = sizeof(scsi_inquiry_resp_t);
  cbw.dir         = TUSB_DIR_IN_MASK;
  cbw.cmd_len     = sizeof(scsi_inquiry_t);

  scsi_inquiry_t const cmd_inquiry = {
      .cmd_code     = SCSI_CMD_INQUIRY,
      .alloc_length = sizeof(scsi_inquiry_resp_t)
  };
  memcpy(cbw.command, &cmd_inquiry, cbw.cmd_len); //-V1086

  return tuh_msc_scsi_command(dev_addr, &cbw, response, complete_cb, arg);
}

bool tuh_msc_test_unit_ready(uint8_t dev_addr, uint8_t lun, tuh_msc_complete_cb_t complete_cb, uintptr_t arg) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  TU_VERIFY(p_msc->configured);

  msc_cbw_t cbw;
  cbw_init(&cbw, lun);

  cbw.total_bytes = 0;
  cbw.dir        = TUSB_DIR_OUT;
  cbw.cmd_len    = sizeof(scsi_test_unit_ready_t);
  cbw.command[0] = SCSI_CMD_TEST_UNIT_READY;

  return tuh_msc_scsi_command(dev_addr, &cbw, NULL, complete_cb, arg);
}

bool tuh_msc_request_sense(uint8_t dev_addr, uint8_t lun, void* response,
                           tuh_msc_complete_cb_t complete_cb, uintptr_t arg) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  TU_VERIFY(p_msc->configured);

  msc_cbw_t cbw;
  cbw_init(&cbw, lun);

  cbw.total_bytes = sizeof(scsi_sense_fixed_resp_t);
  cbw.dir         = TUSB_DIR_IN_MASK;
  cbw.cmd_len     = sizeof(scsi_request_sense_t);

  scsi_request_sense_t const cmd_request_sense = {
      .cmd_code     = SCSI_CMD_REQUEST_SENSE,
      .alloc_length = sizeof(scsi_sense_fixed_resp_t)
  };
  memcpy(cbw.command, &cmd_request_sense, cbw.cmd_len); //-V1086

  return tuh_msc_scsi_command(dev_addr, &cbw, response, complete_cb, arg);
}

bool tuh_msc_read_format_capacities(uint8_t dev_addr, uint8_t lun, void* response,
                                    uint8_t alloc_length,
                                    tuh_msc_complete_cb_t complete_cb, uintptr_t arg) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  TU_VERIFY(p_msc->configured);

  msc_cbw_t cbw;
  cbw_init(&cbw, lun);

  cbw.total_bytes = alloc_length;
  cbw.dir         = TUSB_DIR_IN_MASK;
  cbw.cmd_len     = 12; // UFI command block is 12 bytes
  cbw.command[0]  = 0x23; // READ FORMAT CAPACITIES
  cbw.command[7]  = (alloc_length >> 8) & 0xFF;
  cbw.command[8]  = alloc_length & 0xFF;

  return tuh_msc_scsi_command(dev_addr, &cbw, response, complete_cb, arg);
}

bool tuh_msc_mode_sense6(uint8_t dev_addr, uint8_t lun, void* response,
                         tuh_msc_complete_cb_t complete_cb, uintptr_t arg) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  TU_VERIFY(p_msc->configured);

  msc_cbw_t cbw;
  cbw_init(&cbw, lun);

  cbw.total_bytes = sizeof(scsi_mode_sense6_resp_t);
  cbw.dir         = TUSB_DIR_IN_MASK;
  cbw.cmd_len     = sizeof(scsi_mode_sense6_t);

  scsi_mode_sense6_t const cmd = {
      .cmd_code              = SCSI_CMD_MODE_SENSE_6,
      .disable_block_descriptor = 1,
      .page_code             = 0x3F,
      .alloc_length          = sizeof(scsi_mode_sense6_resp_t),
  };
  memcpy(cbw.command, &cmd, sizeof(cmd));

  return tuh_msc_scsi_command(dev_addr, &cbw, response, complete_cb, arg);
}

bool tuh_msc_start_stop_unit(uint8_t dev_addr, uint8_t lun, bool start,
                             tuh_msc_complete_cb_t complete_cb, uintptr_t arg) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  TU_VERIFY(p_msc->configured);

  msc_cbw_t cbw;
  cbw_init(&cbw, lun);

  cbw.total_bytes = 0;
  cbw.dir         = TUSB_DIR_OUT;
  cbw.cmd_len     = 6;
  cbw.command[0]  = 0x1B; // START STOP UNIT
  cbw.command[4]  = start ? 0x01 : 0x00;

  return tuh_msc_scsi_command(dev_addr, &cbw, NULL, complete_cb, arg);
}

//--------------------------------------------------------------------+
// CLASS-USBH API
//--------------------------------------------------------------------+
bool msch_init(void) {
  TU_LOG_DRV("sizeof(msch_interface_t) = %u\r\n", sizeof(msch_interface_t));
  TU_LOG_DRV("sizeof(msch_epbuf_t) = %u\r\n", sizeof(msch_epbuf_t));
  tu_memclr(_msch_itf, sizeof(_msch_itf));
  return true;
}

bool msch_deinit(void) {
  return true;
}

void msch_close(uint8_t dev_addr) {
  TU_VERIFY(dev_addr <= CFG_TUH_DEVICE_MAX,);
  msch_interface_t* p_msc = get_itf(dev_addr);
  TU_VERIFY(p_msc->configured,);

  TU_LOG_DRV("  MSCh close addr = %d\r\n", dev_addr);

  // Abort any in-flight transfers before notifying the application.
  // Without this, the transfer-complete callback fires on zeroed state.
  tuh_edpt_abort_xfer(dev_addr, p_msc->ep_in);
  tuh_edpt_abort_xfer(dev_addr, p_msc->ep_out);
  if (p_msc->ep_intr) {
    tuh_edpt_abort_xfer(dev_addr, p_msc->ep_intr);
  }
  // If a CBI ADSC control transfer is in-flight, abort EP0 too.
  bool const is_cbi = (p_msc->protocol == MSC_PROTOCOL_CBI ||
                       p_msc->protocol == MSC_PROTOCOL_CBI_NO_INTERRUPT);
  if (is_cbi && p_msc->stage == MSC_STAGE_CMD) {
    tuh_edpt_abort_xfer(dev_addr, 0);
  }
  p_msc->stage = MSC_STAGE_IDLE;

  // invoke Application Callback
  if (p_msc->mounted) {
    tuh_msc_umount_cb(dev_addr);
  }

  tu_memclr(p_msc, sizeof(msch_interface_t));
}

bool msch_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t event, uint32_t xferred_bytes) {
  msch_interface_t* p_msc = get_itf(dev_addr);
  msch_epbuf_t* epbuf = get_epbuf(dev_addr);
  msc_cbw_t const * cbw = &epbuf->cbw;
  msc_csw_t       * csw = &epbuf->csw;

  // CBI transport
  if (p_msc->protocol == MSC_PROTOCOL_CBI || p_msc->protocol == MSC_PROTOCOL_CBI_NO_INTERRUPT) {
    switch (p_msc->stage) {
      case MSC_STAGE_DATA:
        // Data complete - get status via interrupt or finish
        if (event != XFER_RESULT_SUCCESS || !p_msc->ep_intr) {
          // No interrupt endpoint, or data phase failed - complete now
          p_msc->stage = MSC_STAGE_IDLE;
          if (p_msc->complete_cb) {
            csw->signature = MSC_CSW_SIGNATURE;
            csw->tag = cbw->tag;
            csw->data_residue = cbw->total_bytes - xferred_bytes;
            csw->status = (event == XFER_RESULT_SUCCESS) ? MSC_CSW_STATUS_PASSED : MSC_CSW_STATUS_FAILED;
            tuh_msc_complete_data_t const cb_data = {
                .cbw = cbw, .csw = csw,
                .scsi_data = p_msc->buffer, .user_arg = p_msc->complete_arg
            };
            (void) p_msc->complete_cb(dev_addr, &cb_data);
          }
        } else {
          p_msc->stage = MSC_STAGE_STATUS;
          if (!usbh_edpt_xfer(dev_addr, p_msc->ep_intr, epbuf->cbi_status, 2)) {
            // Interrupt xfer failed - complete as failed
            p_msc->stage = MSC_STAGE_IDLE;
            if (p_msc->complete_cb) {
              csw->signature = MSC_CSW_SIGNATURE;
              csw->tag = cbw->tag;
              csw->data_residue = cbw->total_bytes - xferred_bytes;
              csw->status = MSC_CSW_STATUS_FAILED;
              tuh_msc_complete_data_t const cb_data = {
                  .cbw = cbw, .csw = csw,
                  .scsi_data = p_msc->buffer, .user_arg = p_msc->complete_arg
              };
              (void) p_msc->complete_cb(dev_addr, &cb_data);
            }
          }
        }
        break;

      case MSC_STAGE_STATUS: {
        // CBI interrupt status received (2 bytes: ASC, ASCQ)
        // ASC == 0x00 means command passed, non-zero means error
        p_msc->stage = MSC_STAGE_IDLE;
        if (p_msc->complete_cb) {
          uint8_t asc = (xferred_bytes >= 1) ? epbuf->cbi_status[0] : 0;
          csw->signature = MSC_CSW_SIGNATURE;
          csw->tag = cbw->tag;
          csw->data_residue = 0;
          csw->status = (asc == 0 && event == XFER_RESULT_SUCCESS) ?
                        MSC_CSW_STATUS_PASSED : MSC_CSW_STATUS_FAILED;
          tuh_msc_complete_data_t const cb_data = {
              .cbw = cbw, .csw = csw,
              .scsi_data = p_msc->buffer, .user_arg = p_msc->complete_arg
          };
          (void) p_msc->complete_cb(dev_addr, &cb_data);
        }
        break;
      }

      default:
        break;
    }
    return true;
  }

  // BOT transport
  switch (p_msc->stage) {
    case MSC_STAGE_CMD:
      // CBW send failed — report error to caller
      if (ep_addr != p_msc->ep_out || event != XFER_RESULT_SUCCESS || xferred_bytes != sizeof(msc_cbw_t)) {
        p_msc->stage = MSC_STAGE_IDLE;
        if (p_msc->complete_cb) {
          csw->signature = MSC_CSW_SIGNATURE;
          csw->tag = cbw->tag;
          csw->data_residue = cbw->total_bytes;
          csw->status = MSC_CSW_STATUS_FAILED;
          tuh_msc_complete_data_t const cb_data = {
              .cbw = cbw, .csw = csw,
              .scsi_data = p_msc->buffer, .user_arg = p_msc->complete_arg
          };
          (void) p_msc->complete_cb(dev_addr, &cb_data);
        }
        break;
      }
      if (cbw->total_bytes && p_msc->buffer) {
        // Data stage if any
        p_msc->stage = MSC_STAGE_DATA;
        uint8_t const ep_data = (cbw->dir & TUSB_DIR_IN_MASK) ? p_msc->ep_in : p_msc->ep_out;
        uint16_t data_len = (cbw->total_bytes > UINT16_MAX) ? UINT16_MAX : (uint16_t) cbw->total_bytes;
        if (!usbh_edpt_xfer(dev_addr, ep_data, p_msc->buffer, data_len)) {
          p_msc->stage = MSC_STAGE_IDLE;
          if (p_msc->complete_cb) {
            csw->signature = MSC_CSW_SIGNATURE;
            csw->tag = cbw->tag;
            csw->data_residue = cbw->total_bytes;
            csw->status = MSC_CSW_STATUS_FAILED;
            tuh_msc_complete_data_t const cb_data = {
                .cbw = cbw, .csw = csw,
                .scsi_data = p_msc->buffer, .user_arg = p_msc->complete_arg
            };
            (void) p_msc->complete_cb(dev_addr, &cb_data);
          }
          break;
        }
        break;
      }
      TU_ATTR_FALLTHROUGH; // fallthrough to data stage

    case MSC_STAGE_DATA:
      // Status stage
      p_msc->stage = MSC_STAGE_STATUS;
      if (!usbh_edpt_xfer(dev_addr, p_msc->ep_in, (uint8_t*) csw, (uint16_t) sizeof(msc_csw_t))) {
        p_msc->stage = MSC_STAGE_IDLE;
        if (p_msc->complete_cb) {
          csw->signature = MSC_CSW_SIGNATURE;
          csw->tag = cbw->tag;
          csw->data_residue = cbw->total_bytes;
          csw->status = MSC_CSW_STATUS_FAILED;
          tuh_msc_complete_data_t const cb_data = {
              .cbw = cbw, .csw = csw,
              .scsi_data = p_msc->buffer, .user_arg = p_msc->complete_arg
          };
          (void) p_msc->complete_cb(dev_addr, &cb_data);
        }
      }
      break;

    case MSC_STAGE_STATUS: {
      // Validate CSW per BOT spec §6.3:
      // - dCSWSignature must be 53425355h
      // - dCSWTag must match the CBW tag
      // If either fails, treat as protocol error.
      p_msc->stage = MSC_STAGE_IDLE;
      bool csw_valid = (event == XFER_RESULT_SUCCESS &&
                        xferred_bytes == sizeof(msc_csw_t) &&
                        csw->signature == MSC_CSW_SIGNATURE &&
                        csw->tag == cbw->tag);
      if (!csw_valid) {
        csw->status = MSC_CSW_STATUS_FAILED;
      }
      if (p_msc->complete_cb != NULL) {
        tuh_msc_complete_data_t const cb_data = {
            .cbw = cbw,
            .csw = csw,
            .scsi_data = p_msc->buffer,
            .user_arg = p_msc->complete_arg
        };
        (void) p_msc->complete_cb(dev_addr, &cb_data);
      }
      break;
    }

    default:
      // unknown state
      break;
  }

  return true;
}

//--------------------------------------------------------------------+
// MSC Enumeration
//--------------------------------------------------------------------+

uint16_t msch_open(uint8_t rhport, uint8_t dev_addr, const tusb_desc_interface_t *desc_itf, uint16_t max_len) {
  (void) rhport;
  // Accept BOT and CBI protocols
  TU_VERIFY(MSC_PROTOCOL_BOT              == desc_itf->bInterfaceProtocol ||
            MSC_PROTOCOL_CBI              == desc_itf->bInterfaceProtocol ||
            MSC_PROTOCOL_CBI_NO_INTERRUPT == desc_itf->bInterfaceProtocol, 0);
  // Accept SCSI transparent, UFI (floppy), and SFF-8070i subclasses
  TU_VERIFY(MSC_SUBCLASS_SCSI == desc_itf->bInterfaceSubClass ||
            MSC_SUBCLASS_UFI  == desc_itf->bInterfaceSubClass ||
            MSC_SUBCLASS_SFF  == desc_itf->bInterfaceSubClass, 0);

  // msc driver length is fixed
  const uint16_t drv_len =
    (uint16_t)(sizeof(tusb_desc_interface_t) + desc_itf->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
  TU_ASSERT(drv_len <= max_len, 0);

  msch_interface_t *p_msc = get_itf(dev_addr);
  p_msc->protocol = desc_itf->bInterfaceProtocol;
  p_msc->ep_intr = 0;

  // Linux unusual_devs.h quirk. Force CBI_NO_INTERRUPT.
  //   0x0644:0x0000  TEAC Floppy Drive
  //   0x04e6:0x0001  Matshita LS-120
  //   0x04e6:0x0007  Sony Hifd
  bool force_no_intr = false;
  if (p_msc->protocol == MSC_PROTOCOL_CBI) {
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    if ((vid == 0x0644 && pid == 0x0000) ||
        (vid == 0x04e6 && pid == 0x0001) ||
        (vid == 0x04e6 && pid == 0x0007)) {
      p_msc->protocol = MSC_PROTOCOL_CBI_NO_INTERRUPT;
      force_no_intr = true;
    }
  }

  uint8_t const *p_desc = tu_desc_next(desc_itf);
  uint8_t const *desc_end = ((uint8_t const*)desc_itf) + drv_len;
  uint8_t ep_count = 0;

  while (ep_count < desc_itf->bNumEndpoints && p_desc < desc_end) {
    // Skip non-endpoint descriptors (e.g. class-specific)
    if (tu_desc_type(p_desc) != TUSB_DESC_ENDPOINT) {
      p_desc = tu_desc_next(p_desc);
      continue;
    }
    ep_count++;
    tusb_desc_endpoint_t const *ep_desc = (tusb_desc_endpoint_t const *)p_desc;

    if (force_no_intr && TUSB_XFER_INTERRUPT == ep_desc->bmAttributes.xfer) {
      // Skip opening the interrupt endpoint
      p_desc = tu_desc_next(p_desc);
      continue;
    }

    TU_ASSERT(tuh_edpt_open(dev_addr, ep_desc), 0);

    if (TUSB_XFER_BULK == ep_desc->bmAttributes.xfer) {
      if (TUSB_DIR_IN == tu_edpt_dir(ep_desc->bEndpointAddress)) {
        p_msc->ep_in = ep_desc->bEndpointAddress;
      } else {
        p_msc->ep_out = ep_desc->bEndpointAddress;
      }
    } else if (TUSB_XFER_INTERRUPT == ep_desc->bmAttributes.xfer) {
      // CBI interrupt endpoint (always IN)
      p_msc->ep_intr = ep_desc->bEndpointAddress;
    }

    p_desc = tu_desc_next(p_desc);
  }

  p_msc->itf_num = desc_itf->bInterfaceNumber;

  return drv_len;
}

bool msch_set_config(uint8_t daddr, uint8_t itf_num) {
  msch_interface_t* p_msc = get_itf(daddr);
  TU_ASSERT(p_msc->itf_num == itf_num);
  p_msc->configured = true;
  p_msc->mounted = true;
  tuh_msc_mount_cb(daddr);
  usbh_driver_set_config_complete(daddr, p_msc->itf_num);
  return true;
}

#endif
