/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2021 Ha Thach (tinyusb.org) for Double Buffered
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

#if CFG_TUH_ENABLED && (CFG_TUSB_MCU == OPT_MCU_RP2040) && !CFG_TUH_RPI_PIO_USB && !CFG_TUH_MAX3421

#include "pico.h"
#include "portable/raspberrypi/rp2040/rp2040_usb.h"

//--------------------------------------------------------------------+
// INCLUDE
//--------------------------------------------------------------------+
#include "osal/osal.h"

#include "host/hcd.h"
#include "host/usbh.h"

// port 0 is native USB port, other is counted as software PIO
#define RHPORT_NATIVE 0

//--------------------------------------------------------------------+
// Low level rp2040 controller functions
//--------------------------------------------------------------------+

#ifndef PICO_USB_HOST_INTERRUPT_ENDPOINTS
#define PICO_USB_HOST_INTERRUPT_ENDPOINTS (USB_MAX_ENDPOINTS - 1)
#endif
static_assert(PICO_USB_HOST_INTERRUPT_ENDPOINTS <= USB_MAX_ENDPOINTS, "");

// Host mode uses one shared endpoint register for non-interrupt endpoints
static struct hw_endpoint ep_pool[1 + PICO_USB_HOST_INTERRUPT_ENDPOINTS];
#define epx (ep_pool[0])

// EP0 max packet size per device address. The RP2040 shares a single EPX for
// all control transfers, so its wMaxPacketSize can become stale when switching
// between devices with different EP0 sizes (e.g. low-speed MPS=8 vs full-speed
// MPS=64). We track the correct value here and apply it in hcd_setup_send().
static uint8_t _ep0_mps[CFG_TUH_DEVICE_MAX + CFG_TUH_HUB + 1]; // +1 for addr0

// The RP2040 SIE shares a single set of handshake latches (ACK_REC, NAK_REC,
// etc.) across EPX and interrupt endpoint transactions.  When the SIE finishes
// an EPX transaction and immediately starts an interrupt endpoint poll, the
// poll's handshake response overwrites the EPX latches before the IRQ handler
// can read them.  This causes a valid ACK'd EPX completion to appear as
// "no-ACK" — triggering the coincident-RX-Timeout failure path.
//
// To prevent this, we disable interrupt endpoint polling (int_ep_ctrl = 0)
// before every EPX START_TRANS and restore it after the IRQ handler has
// finished processing the EPX completion.  The saved value lives here so the
// IRQ handler can restore it.
static uint32_t _saved_int_ep_ctrl;

// Flags we set by default in sie_ctrl (we add other bits on top)
enum {
  SIE_CTRL_BASE = USB_SIE_CTRL_SOF_EN_BITS      | USB_SIE_CTRL_KEEP_ALIVE_EN_BITS |
                  USB_SIE_CTRL_PULLDOWN_EN_BITS | USB_SIE_CTRL_EP0_INT_1BUF_BITS
};

#ifndef USB_SIE_STATUS_RX_SHORT_PACKET_BITS
#define USB_SIE_STATUS_RX_SHORT_PACKET_BITS 0x00001000u
#endif

static struct hw_endpoint *get_dev_ep(uint8_t dev_addr, uint8_t ep_addr) {
  uint8_t num = tu_edpt_number(ep_addr);
  if (num == 0) {
    return &epx;
  }

  for (uint32_t i = 1; i < TU_ARRAY_SIZE(ep_pool); i++) {
    struct hw_endpoint *ep = &ep_pool[i];
    if (ep->configured && (ep->dev_addr == dev_addr) && (ep->ep_addr == ep_addr)) {
      return ep;
    }
  }

  return NULL;
}

TU_ATTR_ALWAYS_INLINE static inline uint8_t dev_speed(void) {
  return (usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS) >> USB_SIE_STATUS_SPEED_LSB;
}

TU_ATTR_ALWAYS_INLINE static inline bool need_pre(uint8_t dev_addr) {
  // If this device is different from the speed of the root device
  // (i.e. is a low speed device on a full speed hub) then need pre
  return hcd_port_speed_get(0) != tuh_speed_get(dev_addr);
}

TU_ATTR_ALWAYS_INLINE static inline bool trace_port3_request(uint8_t dev_addr, volatile uint8_t const setup_packet[8]) {
  if (dev_addr != 29) {
    return false;
  }

  if (setup_packet[4] != 0x03 || setup_packet[5] != 0x00) {
    return false;
  }

  return setup_packet[0] == 0x23 || setup_packet[0] == 0xA3;
}

TU_ATTR_ALWAYS_INLINE static inline void trace_port3_ep0_state(char const* tag, uint8_t dev_addr, uint8_t ep_addr,
                                                                uint16_t buflen) {
  uint32_t const sie = usb_hw->sie_status;
  uint16_t const bc0 = tu_u32_low16(*epx.buffer_control);
  uint8_t const pid0 = (bc0 & USB_BUF_CTRL_DATA1_PID) ? 1u : 0u;

  TU_LOG(2,
         "  TRACE %s dev=%u ep=0x%02x len=%u setup=%02x %02x %02x %02x %02x %02x %02x %02x "
         "sie=0x%08lx [ACK=%u NAK=%u STALL=%u RXTO=%u DSEQ=%u EPERR=%u SHORT=%u TCOMP=%u LS=%lu SPD=%lu] "
         "bc0=0x%04x pid0=%u next=%u active=%u\r\n",
         tag, dev_addr, ep_addr, buflen,
         usbh_dpram->setup_packet[0], usbh_dpram->setup_packet[1], usbh_dpram->setup_packet[2], usbh_dpram->setup_packet[3],
         usbh_dpram->setup_packet[4], usbh_dpram->setup_packet[5], usbh_dpram->setup_packet[6], usbh_dpram->setup_packet[7],
         sie,
         !!(sie & USB_SIE_STATUS_ACK_REC_BITS),
         !!(sie & USB_SIE_STATUS_NAK_REC_BITS),
         !!(sie & USB_SIE_STATUS_STALL_REC_BITS),
         !!(sie & USB_SIE_STATUS_RX_TIMEOUT_BITS),
         !!(sie & USB_SIE_STATUS_DATA_SEQ_ERROR_BITS),
         !!(sie & 0x00800000u),
         !!(sie & USB_SIE_STATUS_RX_SHORT_PACKET_BITS),
         !!(sie & USB_SIE_STATUS_TRANS_COMPLETE_BITS),
         (sie & USB_SIE_STATUS_LINE_STATE_BITS) >> USB_SIE_STATUS_LINE_STATE_LSB,
         (sie & USB_SIE_STATUS_SPEED_BITS) >> USB_SIE_STATUS_SPEED_LSB,
         bc0, pid0, epx.next_pid, epx.active);
}

TU_ATTR_ALWAYS_INLINE static inline void epx_hard_reset(void) {
  usb_hw->sie_ctrl = SIE_CTRL_BASE; // stop SIE before clearing
  busy_wait_us(1);    // drain any in-flight SIE writeback
  usb_hw_clear->buf_status = 0x3u;
  *epx.buffer_control = 0;
}

TU_ATTR_ALWAYS_INLINE static inline void epx_clear_rx_timeout_sticky(void) {
  // RX_TIMEOUT can remain level-asserted across several IRQ entries after a
  // failed control phase. Clear it after SIE halt and spin briefly until the
  // SIE status source deasserts.
  uint32_t const sticky_mask = USB_SIE_STATUS_RX_TIMEOUT_BITS | 0x00800000u;
  for (uint8_t i = 0; i < 32; i++) {
    usb_hw_clear->sie_status = USB_SIE_STATUS_RX_TIMEOUT_BITS | 0x00800000u;
    busy_wait_us(1);
    if ((usb_hw->sie_status & sticky_mask) == 0) {
      break;
    }
  }
}

static void __tusb_irq_path_func(epx_sie_reset)(void) {
  // Aggressively reset EPX after a coincident RX_TIMEOUT without ACK, where
  // the SIE consumed the buffer but the device never acknowledged — leaving
  // hidden internal state corrupted.
  //
  // We must NOT toggle CONTROLLER_EN: that clears the SPEED bits in
  // sie_status, causing hcd_port_speed_get() to panic("Invalid speed") on
  // the very next TinyUSB retry.  Instead, halt the SIE via sie_ctrl,
  // scrub all buffer/status state, and clear every sticky status latch.
  // The SIE's per-transaction state (PID tracking, handshake FSM) resets
  // on the next START_TRANS write, so this is sufficient.
  usb_hw->sie_ctrl = 0;                    // kill SOF + all transaction bits
  busy_wait_us(2);
  usb_hw_clear->buf_status = 0xffffffffu;  // drain all pending completions
  *epx.buffer_control = 0;
  usb_hw_clear->sie_status = USB_SIE_STATUS_ACK_REC_BITS |
                             USB_SIE_STATUS_NAK_REC_BITS |
                             USB_SIE_STATUS_STALL_REC_BITS |
                             USB_SIE_STATUS_DATA_SEQ_ERROR_BITS |
                             USB_SIE_STATUS_RX_TIMEOUT_BITS |
                             USB_SIE_STATUS_RX_SHORT_PACKET_BITS |
                             USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                             0x00800000u;
  busy_wait_us(1);
  usb_hw->sie_ctrl = SIE_CTRL_BASE;        // restore SOF generation
  busy_wait_us(1);
  epx_clear_rx_timeout_sticky();
}

TU_ATTR_ALWAYS_INLINE static inline void epx_prepare_for_start(void) {
  // Defensive: zero EPX buffer_control before each phase start so stale
  // terminal values (e.g. 0x4000 from a late timeout edge) cannot influence
  // the next transaction. The root cause (no-ACK coincident timeout) is now
  // handled by epx_sie_reset(), but this remains as a safety net.
  if (!epx.active) {
    usb_hw_clear->buf_status = 0x3u;
    *epx.buffer_control = 0;
  }

  // Clear transient handshake/status latches before each EP0 phase start.
  // ACK/NAK can remain set after a successful prior request and should not
  // influence the next setup/data/status transaction.
  uint32_t const latch_clear_mask = USB_SIE_STATUS_ACK_REC_BITS |
                                    USB_SIE_STATUS_NAK_REC_BITS |
                                    USB_SIE_STATUS_STALL_REC_BITS |
                                    USB_SIE_STATUS_DATA_SEQ_ERROR_BITS |
                                    USB_SIE_STATUS_RX_TIMEOUT_BITS |
                                    USB_SIE_STATUS_RX_SHORT_PACKET_BITS |
                                    USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                                    0x00800000u;
  uint32_t const sticky_err_mask = USB_SIE_STATUS_DATA_SEQ_ERROR_BITS |
                                   USB_SIE_STATUS_STALL_REC_BITS |
                                   USB_SIE_STATUS_RX_TIMEOUT_BITS |
                                   0x00800000u;

  usb_hw_clear->sie_status = latch_clear_mask;
  busy_wait_us(1);

  // Fast path: do not hard-reset EPX between successful control phases.
  // Repeatedly forcing SIE idle + raw buffer_control writes here can itself
  // perturb the next phase. Only do destructive cleanup when an error remains latched.
  if (usb_hw->sie_status & sticky_err_mask) {
    epx_hard_reset();
    usb_hw_clear->sie_status = latch_clear_mask;
    busy_wait_us(1);
    epx_clear_rx_timeout_sticky();
  }
}

static void __tusb_irq_path_func(hw_xfer_complete)(struct hw_endpoint *ep, xfer_result_t xfer_result) {
  // Mark transfer as done before we tell the tinyusb stack
  uint8_t dev_addr = ep->dev_addr;
  uint8_t ep_addr = ep->ep_addr;
  uint xferred_len = ep->xferred_len;
  hw_endpoint_reset_transfer(ep);
  hcd_event_xfer_complete(dev_addr, ep_addr, xferred_len, xfer_result, true);
}

static void __tusb_irq_path_func(handle_hwbuf_status_bit)(uint bit, struct hw_endpoint *ep) {
  usb_hw_clear->buf_status = bit;

  if (!ep->active) return;

  const bool done = hw_endpoint_xfer_continue(ep);
  if (done) {
    hw_xfer_complete(ep, XFER_RESULT_SUCCESS);
  }
}

static void __tusb_irq_path_func(handle_hwbuf_status)(void) {
  uint32_t buf_status = usb_hw->buf_status;
  TU_LOG(2, "  buf_status=0x%08lx sie_status=0x%08lx epx.pid=%u epx.bc=[0x%04x:0x%04x]\r\n",
         buf_status, usb_hw->sie_status, epx.next_pid,
         tu_u32_low16(*epx.buffer_control), tu_u32_high16(*epx.buffer_control));

  // Check EPX first
  uint32_t bit = 1u;
  if (buf_status & bit) {
    buf_status &= ~bit;
    struct hw_endpoint * ep = &epx;
    handle_hwbuf_status_bit(bit, ep);
  }

  // Check "interrupt" (asynchronous) endpoints for both IN and OUT
  for (uint i = 1; i <= PICO_USB_HOST_INTERRUPT_ENDPOINTS && buf_status; i++) {
    // EPX is bit 0 & 1
    // IEP1 IN  is bit 2
    // IEP1 OUT is bit 3
    // IEP2 IN  is bit 4
    // IEP2 OUT is bit 5
    // IEP3 IN  is bit 6
    // IEP3 OUT is bit 7
    // etc
    for (uint j = 0; j < 2; j++) {
      bit = 1 << (i * 2 + j);
      if (buf_status & bit) {
        buf_status &= ~bit;
        handle_hwbuf_status_bit(bit, &ep_pool[i]);
      }
    }
  }

  if (buf_status) {
    panic("Unhandled buffer %u\n", (uint) buf_status);
  }
}

static void __tusb_irq_path_func(hw_trans_complete)(void)
{
  if (usb_hw->sie_ctrl & USB_SIE_CTRL_SEND_SETUP_BITS)
  {
    pico_trace("Sent setup packet\n");
    struct hw_endpoint *ep = &epx;
    TU_LOG(2, "  hw_trans_complete: SETUP path, ep->active=%u sie_ctrl=0x%08lx\r\n",
           ep->active, usb_hw->sie_ctrl);
    // EPX may have been completed already by the DATA_SEQ_ERROR drain in the
    // same IRQ snapshot (drain consumed TRANS_COMPLETE before this block ran).
    // That is not an error — just a no-op here.
    if (!ep->active) return;
    // Set transferred length to 8 for a setup packet
    ep->xferred_len = 8;
    hw_xfer_complete(ep, XFER_RESULT_SUCCESS);
  }
  else
  {
    // Don't care. Will handle this in buff status
    return;
  }
}

static void __tusb_irq_path_func(hcd_rp2040_irq)(void)
{
  uint32_t status = usb_hw->ints;
  uint32_t handled = 0;
  bool suppress_completion_paths = false;
  bool defer_timeout_clear = false;
  bool defer_timeout_sanitize = false;
  TU_LOG(2, "  IRQ ints=0x%08lx sie_status=0x%08lx sie_ctrl=0x%08lx epx.active=%u\r\n",
         status, usb_hw->sie_status, usb_hw->sie_ctrl, epx.active);
  if (trace_port3_request(epx.dev_addr, usbh_dpram->setup_packet) &&
      (status & (USB_INTS_BUFF_STATUS_BITS | USB_INTS_TRANS_COMPLETE_BITS |
                 USB_INTS_ERROR_DATA_SEQ_BITS | USB_INTS_ERROR_RX_TIMEOUT_BITS))) {
    trace_port3_ep0_state("P3_IRQ", epx.dev_addr, epx.ep_addr, epx.remaining_len);
  }

  if ( status & USB_INTS_HOST_CONN_DIS_BITS )
  {
    handled |= USB_INTS_HOST_CONN_DIS_BITS;
    // Clear the speed-change latch first so subsequent reads of sie_status
    // reflect the new (post-event) state.  The busy_wait gives the APB write
    // time to propagate before we read the bits back via dev_speed(); without
    // it the read can return 0 on a fast connect path (Heisenbug: the delay
    // introduced by UART logging masked this window).
    usb_hw_clear->sie_status = USB_SIE_STATUS_SPEED_BITS;
    busy_wait_us(1);

    if ( dev_speed() )
    {
      hcd_event_device_attach(RHPORT_NATIVE, true);
    }
    else
    {
      // Device disconnected.  The SIE can have BUFF_STATUS and/or
      // TRANS_COMPLETE set simultaneously in this same status snapshot
      // (a transfer completed as the cable was pulled).  We must silence
      // the hardware and reset all endpoint software state NOW — before
      // the subsequent if-blocks run — so they find nothing to process.
      // If we let handle_hwbuf_status() or hw_trans_complete() run after
      // this, they deliver hcd_event_xfer_complete for a device the stack
      // is about to tear down, corrupting enumeration state.

      // Stop the SIE immediately (no new transactions).
      usb_hw->sie_ctrl = SIE_CTRL_BASE;
      busy_wait_us(1);  // drain any in-flight SIE writeback

      // Disable all EP1-15 interrupt endpoint polling.
      usb_hw->int_ep_ctrl = 0;
      _saved_int_ep_ctrl = 0;

      // Clear every pending buf_status bit so handle_hwbuf_status() is a
      // guaranteed no-op if BUFF_STATUS was also set in this snapshot.
      usb_hw_clear->buf_status = 0xffffffffu;
      *epx.buffer_control = 0;

      // Reset software endpoint state without delivering any completions.
      // hcd_device_close() (called by the stack from task context in
      // response to the remove event below) does the full teardown.
      if (epx.active) hw_endpoint_reset_transfer(&epx);
      for (uint i = 1; i < TU_ARRAY_SIZE(ep_pool); i++) {
        if (ep_pool[i].active) hw_endpoint_reset_transfer(&ep_pool[i]);
      }

      hcd_event_device_remove(RHPORT_NATIVE, true);
    }
  }

  if ( status & USB_INTS_HOST_RESUME_BITS )
  {
    // Remote wakeup: the device drove resume signalling.  The SIE resumes
    // SOF generation automatically; class drivers will re-arm their IN
    // endpoints on the next poll.  Acknowledge and move on.
    handled |= USB_INTS_HOST_RESUME_BITS;
    usb_hw_clear->sie_status = USB_SIE_STATUS_RESUME_BITS;
  }

  if ( status & USB_INTS_STALL_BITS )
  {
    // We have rx'd a stall from the device.
    // NOTE THIS SHOULD HAVE PRIORITY OVER BUFF_STATUS AND TRANS_COMPLETE
    // as the stall is an alternative response to one of those events.
    pico_trace("Stall REC\n");
    handled |= USB_INTS_STALL_BITS;
    usb_hw_clear->sie_status = USB_SIE_STATUS_STALL_REC_BITS;
    if (epx.active)
      hw_xfer_complete(&epx, XFER_RESULT_STALLED);
    epx_hard_reset();
  }

  if ( status & USB_INTS_ERROR_DATA_SEQ_BITS )
  {
    // NOTE THIS MUST HAVE PRIORITY OVER BUFF_STATUS AND TRANS_COMPLETE.
    // DATA_SEQ fires together with BUFF_STATUS in the same snapshot. If
    // BUFF_STATUS runs first, handle_hwbuf_status() sees the FULL bit and
    // accepts the wrong-PID data as SUCCESS, clearing ep->active — this
    // handler then becomes a no-op and the corrupted data is delivered.
    // By handling DATA_SEQ first and draining EPX buf_status before
    // BUFF_STATUS runs, we prevent that silent data poisoning.
    //
    // USB_INTS_ERROR_DATA_SEQ_BITS is wired to SIE_STATUS which only tracks
    // EPx — this interrupt never fires for EP1-15.
    handled |= USB_INTS_ERROR_DATA_SEQ_BITS;
    // DATA_SEQ is an error alternative to normal completion; if the same IRQ
    // snapshot also carries BUFF_STATUS/TRANS_COMPLETE, suppress those paths
    // so the transfer is never reported as SUCCESS.
    suppress_completion_paths = true;
    handled |= status & (USB_INTS_BUFF_STATUS_BITS | USB_INTS_TRANS_COMPLETE_BITS);
    // Clear DATA_SEQ_ERROR and also the undocumented bit 23 (0x00800000) which
    // the silicon sets alongside it and is never otherwise cleared.
    usb_hw_clear->sie_status = USB_SIE_STATUS_DATA_SEQ_ERROR_BITS |
                   USB_SIE_STATUS_RX_SHORT_PACKET_BITS |
                   USB_SIE_STATUS_RX_TIMEOUT_BITS |
                   USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                   0x00800000u;
    // Let status-clear propagate before touching EPX state.
    busy_wait_us(1);
    TU_LOG(2, "  Data Seq Error: [0] = 0x%04x  [1] = 0x%04x\r\n",
           tu_u32_low16(*epx.buffer_control), tu_u32_high16(*epx.buffer_control));
    epx_hard_reset(); // drain EPX bits before BUFF_STATUS runs
    epx_clear_rx_timeout_sticky();
    if (epx.active)
      hw_xfer_complete(&epx, XFER_RESULT_FAILED);
  }

  if ( status & USB_INTS_ERROR_RX_TIMEOUT_BITS )
  {
    handled |= USB_INTS_ERROR_RX_TIMEOUT_BITS;
    bool const timeout_with_completion = (status & USB_INTS_TRANS_COMPLETE_BITS) != 0;
    bool const timeout_with_data_completion =
      (status & USB_INTS_BUFF_STATUS_BITS) &&
      (status & USB_INTS_TRANS_COMPLETE_BITS);
    bool const late_timeout_after_complete = !epx.active && timeout_with_completion;
    TU_LOG(2, "  RX Timeout: epx.active=%u ep_addr=0x%02x\r\n", epx.active, epx.ep_addr);
    TU_LOG(2, "  RX Timeout detail: dev_addr_ctrl=0x%08lx sie_ctrl=0x%08lx setup=%02x %02x %02x %02x %02x %02x %02x %02x\r\n",
           usb_hw->dev_addr_ctrl, usb_hw->sie_ctrl,
           usbh_dpram->setup_packet[0], usbh_dpram->setup_packet[1],
           usbh_dpram->setup_packet[2], usbh_dpram->setup_packet[3],
           usbh_dpram->setup_packet[4], usbh_dpram->setup_packet[5],
           usbh_dpram->setup_packet[6], usbh_dpram->setup_packet[7]);

    if (timeout_with_completion) {
      // RX_TIMEOUT arrived in the same snapshot as TRANS_COMPLETE and/or
      // BUFF_STATUS. Check whether the device actually ACK'd the transaction.
      // If ACK_REC is absent, the SIE consumed the buffer but the device
      // never acknowledged — a "phantom completion." Letting the completion
      // paths run would report SUCCESS for un-ACK'd data, leaving the SIE's
      // internal PID/transaction state corrupted. This later manifests as a
      // DATA_SEQ_ERROR on a subsequent transfer to a different device.
      uint32_t const sie_snap = usb_hw->sie_status;
      bool const has_ack = (sie_snap & USB_SIE_STATUS_ACK_REC_BITS) != 0;

      if (!has_ack && epx.active) {
        // No ACK: suppress completions, fail the transfer, and do an
        // aggressive SIE reset to clear hidden internal state.
        TU_LOG(2, "  RX Timeout coincident-no-ACK: failing transfer, resetting SIE\r\n");
        suppress_completion_paths = true;
        handled |= status & (USB_INTS_BUFF_STATUS_BITS | USB_INTS_TRANS_COMPLETE_BITS);
        hw_xfer_complete(&epx, XFER_RESULT_FAILED);
        epx_sie_reset();
      } else {
        // ACK present (or epx already inactive): the transaction completed
        // on the wire. Let BUFF_STATUS/TRANS_COMPLETE run first, then
        // sanitize EPX afterwards to clear the timeout residue.
        if (timeout_with_data_completion) {
          TU_LOG(2, "  RX Timeout coincident-with-ACK (data+trans): deferring cleanup\r\n");
        } else {
          TU_LOG(2, "  RX Timeout coincident-with-ACK (trans): deferring cleanup\r\n");
        }
        defer_timeout_clear = true;
        defer_timeout_sanitize = true;
      }
    } else if (!epx.active) {
      // Timeout source while EPX is already inactive is stale/late edge noise.
      // This can arrive as 0x58 (BUFF_STATUS+TRANS_COMPLETE+RX_TIMEOUT) and
      // leave EPX bufctrl in a bad terminal value (e.g. 0x4000), poisoning the
      // next EP0 transfer. Sanitize EPX state but do not fail any transfer.
      if (late_timeout_after_complete) {
        TU_LOG(2, "  RX Timeout late-after-complete: sanitizing EPX\r\n");
      }
      epx_hard_reset();
      usb_hw_clear->sie_status = USB_SIE_STATUS_RX_TIMEOUT_BITS |
                                 USB_SIE_STATUS_RX_SHORT_PACKET_BITS |
                                 USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                                 0x00800000u;
      epx_clear_rx_timeout_sticky();
    } else {
      // RX timeout is an error alternative to BUFF_STATUS/TRANS_COMPLETE for
      // EPX. In mixed snapshots (e.g. 0x58), prefer timeout and suppress the
      // completion paths to avoid accepting corrupted control phases as SUCCESS.
      suppress_completion_paths = true;
      handled |= status & (USB_INTS_BUFF_STATUS_BITS | USB_INTS_TRANS_COMPLETE_BITS);

      hw_xfer_complete(&epx, XFER_RESULT_FAILED);

      epx_hard_reset();

      // Clear timeout/trans-complete after reset, then quench any sticky timeout
      // assertion before letting EP0 continue.
      usb_hw_clear->sie_status = USB_SIE_STATUS_RX_TIMEOUT_BITS |
                                 USB_SIE_STATUS_RX_SHORT_PACKET_BITS |
                                 USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                                 0x00800000u;
      epx_clear_rx_timeout_sticky();
    }
  }

  if ( (status & USB_INTS_BUFF_STATUS_BITS) && !suppress_completion_paths )
  {
    handled |= USB_INTS_BUFF_STATUS_BITS;
    TU_LOG(2, "Buffer complete\r\n");
    handle_hwbuf_status();
  }

  if ( (status & USB_INTS_TRANS_COMPLETE_BITS) && !suppress_completion_paths )
  {
    handled |= USB_INTS_TRANS_COMPLETE_BITS;
    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS;
    TU_LOG(2, "Transfer complete\r\n");
    hw_trans_complete();
  }

  if (defer_timeout_clear) {
    usb_hw_clear->sie_status = USB_SIE_STATUS_RX_TIMEOUT_BITS |
                               USB_SIE_STATUS_RX_SHORT_PACKET_BITS |
                               0x00800000u;
    busy_wait_us(1);
    if (!epx.active) {
      if (defer_timeout_sanitize) {
        // A completion-coincident timeout indicates EPX state may be dirty even
        // when the transfer reports success. Reset EPX datapath now that
        // completion processing is done so the next control phase starts clean.
        epx_hard_reset();
      }
      usb_hw_clear->sie_status = USB_SIE_STATUS_ACK_REC_BITS |
                                 USB_SIE_STATUS_NAK_REC_BITS |
                                 USB_SIE_STATUS_STALL_REC_BITS |
                                 USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                                 USB_SIE_STATUS_RX_SHORT_PACKET_BITS |
                                 USB_SIE_STATUS_RX_TIMEOUT_BITS |
                                 0x00800000u;
      epx_clear_rx_timeout_sticky();
    }
  }

  // Restore interrupt endpoint polling only when the EPX transfer is fully
  // complete (or failed).  Multi-packet double-buffered transfers span several
  // IRQs — intermediate buffer-pair completions leave epx.active==true.
  // Re-enabling polling at that point would let the SIE start an interrupt
  // endpoint poll that clobbers the handshake latches mid-transfer.
  if (_saved_int_ep_ctrl && !epx.active) {
    usb_hw->int_ep_ctrl = _saved_int_ep_ctrl;
    _saved_int_ep_ctrl = 0;
  }

  if ( status ^ handled )
  {
    panic("Unhandled IRQ 0x%x\n", (uint) (status ^ handled));
  }
}

void __tusb_irq_path_func(hcd_int_handler)(uint8_t rhport, bool in_isr) {
  (void) rhport;
  (void) in_isr;
  hcd_rp2040_irq();
}

static struct hw_endpoint *_next_free_interrupt_ep(void)
{
  for ( uint i = 1; i < TU_ARRAY_SIZE(ep_pool); i++ )
  {
    struct hw_endpoint *ep = &ep_pool[i];
    if ( !ep->configured )
    {
      // Will be configured by hw_endpoint_init / hw_endpoint_allocate
      ep->interrupt_num = (uint8_t) (i - 1);
      return ep;
    }
  }
  return NULL;
}

static hw_endpoint_t *hw_endpoint_allocate(uint8_t transfer_type) {
  hw_endpoint_t *ep = NULL;

  if (transfer_type == TUSB_XFER_CONTROL) {
    ep                   = &epx;
    ep->endpoint_control = &usbh_dpram->epx_ctrl;
    ep->buffer_control   = &usbh_dpram->epx_buf_ctrl;
    ep->hw_data_buf      = &usbh_dpram->epx_data[0];
  } else {
    // Note: even though datasheet name these "Interrupt" endpoints. These are actually
    // "Asynchronous" endpoints and can be used for other type such as: Bulk  (ISO need confirmation)
    ep = _next_free_interrupt_ep();
    TU_VERIFY(ep, NULL);
    pico_info("Allocate %s ep %d\n", tu_edpt_type_str(transfer_type), ep->interrupt_num);
    ep->endpoint_control = &usbh_dpram->int_ep_ctrl[ep->interrupt_num].ctrl;
    ep->buffer_control   = &usbh_dpram->int_ep_buffer_ctrl[ep->interrupt_num].ctrl;
    // 0 for epx (double buffered): TODO increase to 1024 for ISO
    // 2x64 for intep0
    // 3x64 for intep1
    // etc
    // EP data buffers are allocated from the epx_data region (0x180 onwards)
    // We treat epx_data as a flat array and assume 64-byte blocks for interrupts
    ep->hw_data_buf = &usbh_dpram->epx_data[64 * (ep->interrupt_num + 2)];
  }

  return ep;
}

static void hw_endpoint_init(struct hw_endpoint *ep, uint8_t dev_addr, uint8_t ep_addr, uint16_t wMaxPacketSize,
                             uint8_t transfer_type, uint8_t bmInterval) {
  // Already has data buffer, endpoint control, and buffer control allocated at this point
  assert(ep->hw_data_buf);

  uint8_t const num = tu_edpt_number(ep_addr);
  tusb_dir_t const dir = tu_edpt_dir(ep_addr);

  ep->ep_addr = ep_addr;
  ep->dev_addr = dev_addr;

  // Response to a setup packet on EP0 starts with pid of 1
  ep->next_pid = (num == 0 ? 1u : 0u);
  ep->wMaxPacketSize = wMaxPacketSize;
  ep->transfer_type = transfer_type;
  ep->rx = (dir == TUSB_DIR_IN);

  pico_trace("hw_endpoint_init dev %d ep %02X xfer %d\n", ep->dev_addr, ep->ep_addr, transfer_type);
  pico_trace("dev %d ep %02X setup buffer @ 0x%p\n", ep->dev_addr, ep->ep_addr, ep->hw_data_buf);
  uint dpram_offset = hw_data_offset(ep->hw_data_buf);
  // Bits 0-5 should be 0
  assert(!(dpram_offset & 0b111111));

  // Fill in endpoint control register with buffer offset.
  // Bits [15:6] of the EP_CTRL register hold dpram_offset directly (offset is
  // always 64-byte aligned so bits [5:0] are zero).  EP_CTRL_HOST_INTERRUPT_INTERVAL_LSB
  // is bit 16, which is above the address field — do NOT shift dpram_offset up.
  uint32_t ctrl_value = EP_CTRL_ENABLE_BITS | EP_CTRL_INTERRUPT_PER_BUFFER |
                        ((uint32_t)transfer_type << EP_CTRL_BUFFER_TYPE_LSB) |
                        dpram_offset;
  if (bmInterval) {
    ctrl_value |= (uint32_t)((bmInterval - 1) << EP_CTRL_HOST_INTERRUPT_INTERVAL_LSB);
  }

  io_rw_32 *ctrl_reg = ep->endpoint_control;
  *ctrl_reg          = ctrl_value;
  pico_trace("endpoint control (0x%p) <- 0x%lx\n", ctrl_reg, ctrl_value);
  ep->configured = true;

  if (ep != &epx) {
    // Endpoint has its own addr_endp and interrupt bits to be setup!
    // This is an interrupt/async endpoint. so need to set up ADDR_ENDP register with:
    // - device address
    // - endpoint number / direction
    // - preamble
    uint32_t reg = (uint32_t)(dev_addr | (num << USB_ADDR_ENDP1_ENDPOINT_LSB));

    if (dir == TUSB_DIR_OUT) {
      reg |= USB_ADDR_ENDP1_INTEP_DIR_BITS;
    }

    if (need_pre(dev_addr)) {
      reg |= USB_ADDR_ENDP1_INTEP_PREAMBLE_BITS;
    }
    usb_hw->int_ep_addr_ctrl[ep->interrupt_num] = reg;

    // Finally, enable interrupt that endpoint
    usb_hw_set->int_ep_ctrl = 1 << (ep->interrupt_num + 1);

    // If it's an interrupt endpoint we need to set up the buffer control register
  }
}

//--------------------------------------------------------------------+
// HCD API
//--------------------------------------------------------------------+
bool hcd_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {
  (void) rhport;
  (void) rh_init;
  pico_trace("hcd_init %d\n", rhport);
  assert(rhport == 0);

  // Reset any previous state
  rp2040_usb_init();

  // Force VBUS detect to always present, for now we assume vbus is always provided (without using VBUS En)
  usb_hw->pwr = USB_USB_PWR_VBUS_DETECT_BITS | USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS;

  // Remove shared irq if it was previously added so as not to fill up shared irq slots
  irq_remove_handler(USBCTRL_IRQ, hcd_rp2040_irq);

  irq_add_shared_handler(USBCTRL_IRQ, hcd_rp2040_irq, PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);

  // clear epx and interrupt eps
  memset(&ep_pool, 0, sizeof(ep_pool));
  memset(_ep0_mps, 0, sizeof(_ep0_mps));

  // Enable in host mode with SOF / Keep alive on
  usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS | USB_MAIN_CTRL_HOST_NDEVICE_BITS;
  usb_hw->sie_ctrl = SIE_CTRL_BASE;
  usb_hw->inte = USB_INTE_BUFF_STATUS_BITS      |
                 USB_INTE_HOST_CONN_DIS_BITS    |
                 USB_INTE_HOST_RESUME_BITS      |
                 USB_INTE_STALL_BITS            |
                 USB_INTE_TRANS_COMPLETE_BITS   |
                 USB_INTE_ERROR_RX_TIMEOUT_BITS |
                 USB_INTE_ERROR_DATA_SEQ_BITS   ;

  return true;
}

bool hcd_deinit(uint8_t rhport) {
  (void) rhport;

  irq_remove_handler(USBCTRL_IRQ, hcd_rp2040_irq);
  reset_block(RESETS_RESET_USBCTRL_BITS);
  unreset_block_wait(RESETS_RESET_USBCTRL_BITS);

  return true;
}

void hcd_port_reset(uint8_t rhport)
{
  (void) rhport;
  pico_trace("hcd_port_reset\n");
  TU_ASSERT(rhport == 0, );
  // TODO: Nothing to do here yet. Perhaps need to reset some state?
}

void hcd_port_reset_end(uint8_t rhport)
{
  (void) rhport;
  TU_ASSERT(rhport == 0, );
  // TODO: Nothing to do here yet. Perhaps need to reset some state?
}

bool hcd_port_connect_status(uint8_t rhport)
{
  (void) rhport;
  pico_trace("hcd_port_connect_status\n");
  TU_ASSERT(rhport == 0);
  return usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS;
}

tusb_speed_t hcd_port_speed_get(uint8_t rhport)
{
  (void) rhport;
  TU_ASSERT(rhport == 0);

  // TODO: Should enumval this register
  switch ( dev_speed() )
  {
    case 1:
      return TUSB_SPEED_LOW;
    case 2:
      return TUSB_SPEED_FULL;
    default:
      panic("Invalid speed\n");
      // return TUSB_SPEED_INVALID;
  }
}

// Close all opened endpoint belong to this device
void hcd_device_close(uint8_t rhport, uint8_t dev_addr) {
  pico_trace("hcd_device_close %d\n", dev_addr);
  (void) rhport;

  // Clear saved EP0 max packet size
  if (dev_addr < TU_ARRAY_SIZE(_ep0_mps)) {
    _ep0_mps[dev_addr] = 0;
  }

  // reset epx if it belongs to this device (regardless of active state so
  // stale AVAIL/FULL bits in buffer_control are always cleared on close).
  // Critical section: an IRQ between the epx.active check and
  // hw_endpoint_reset_transfer would deliver hcd_event_xfer_complete, letting
  // the class driver re-arm EPX; the close would then zero endpoint_control
  // and buffer_control under the live new transfer (Race 3).
  if (epx.configured && epx.dev_addr == dev_addr) {
    uint32_t const saved = save_and_disable_interrupts();
    if (epx.active) hw_endpoint_reset_transfer(&epx);
    epx.configured = false;
    *epx.endpoint_control = 0;
    *epx.buffer_control = 0;
    restore_interrupts(saved);
  }

  // dev0 only has ep0
  if (dev_addr != 0) {
    for (size_t i = 1; i < TU_ARRAY_SIZE(ep_pool); i++) {
      hw_endpoint_t *ep = &ep_pool[i];
      if (ep->dev_addr == dev_addr && ep->configured) {
        // Critical section: same Race 3 as EPX — IRQ between the configured
        // check and hw_endpoint_reset_transfer can deliver a completion and
        // let the class driver re-arm before we zero endpoint_control.
        uint32_t const saved = save_and_disable_interrupts();
        // in case it is an interrupt endpoint, disable it
        usb_hw_clear->int_ep_ctrl = (1 << (ep->interrupt_num + 1));
        usb_hw->int_ep_addr_ctrl[ep->interrupt_num] = 0;

        // unconfigure the endpoint
        ep->configured = false;
        *ep->endpoint_control  = 0;
        *ep->buffer_control = 0;
        hw_endpoint_reset_transfer(ep);
        restore_interrupts(saved);
      }
    }
  }
}

uint32_t hcd_frame_number(uint8_t rhport) {
  (void)rhport;
  return usb_hw->sof_rd;
}

void hcd_int_enable(uint8_t rhport) {
  (void)rhport;
  irq_set_enabled(USBCTRL_IRQ, true);
}

void hcd_int_disable(uint8_t rhport) {
  (void)rhport;
  // todo we should check this is disabling from the correct core; note currently this is never called
  irq_set_enabled(USBCTRL_IRQ, false);
}

//--------------------------------------------------------------------+
// Endpoint API
//--------------------------------------------------------------------+
bool hcd_edpt_open(uint8_t rhport, uint8_t dev_addr, const tusb_desc_endpoint_t *ep_desc) {
  (void)rhport;
  pico_trace("hcd_edpt_open dev_addr %d, ep_addr %d\n", dev_addr, ep_desc->bEndpointAddress);
  hw_endpoint_t *ep = hw_endpoint_allocate(ep_desc->bmAttributes.xfer);
  TU_ASSERT(ep);

  hw_endpoint_init(ep, dev_addr, ep_desc->bEndpointAddress, tu_edpt_packet_size(ep_desc), ep_desc->bmAttributes.xfer,
                   ep_desc->bInterval);

  // Remember EP0 max packet size so hcd_setup_send() can restore it
  if (ep_desc->bEndpointAddress == 0x00 &&
      dev_addr < TU_ARRAY_SIZE(_ep0_mps)) {
    _ep0_mps[dev_addr] = (uint8_t) tu_edpt_packet_size(ep_desc);
  }

  return true;
}

bool hcd_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep_addr) {
  (void) rhport;

  struct hw_endpoint *ep = get_dev_ep(daddr, ep_addr);
  if (!ep || !ep->configured || ep == &epx) {
    return true; // EP0 (epx) is shared
  }

  uint32_t const saved = save_and_disable_interrupts();

  // Disable the interrupt endpoint in hardware
  usb_hw_clear->int_ep_ctrl = (1u << (ep->interrupt_num + 1));
  usb_hw->int_ep_addr_ctrl[ep->interrupt_num] = 0;

  // Unconfigure and reset
  ep->configured = false;
  *ep->endpoint_control  = 0;
  *ep->buffer_control = 0;
  hw_endpoint_reset_transfer(ep);

  restore_interrupts(saved);
  return true;
}

bool hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen) {
  (void) rhport;

  pico_trace("hcd_edpt_xfer dev_addr %d, ep_addr 0x%x, len %d\n", dev_addr, ep_addr, buflen);

  const uint8_t    ep_num = tu_edpt_number(ep_addr);
  tusb_dir_t const ep_dir = tu_edpt_dir(ep_addr);

  // Get appropriate ep. Either EPX or interrupt endpoint
  struct hw_endpoint *ep = get_dev_ep(dev_addr, ep_addr);

  TU_ASSERT(ep);

  // EP should be inactive
  TU_ASSERT(!ep->active);

  // For EP0 (shared EPX): always re-init on every phase.
  // EPX is shared across all devices; any intervening hub or other control transfer
  // may have left next_pid in an indeterminate state. hw_endpoint_init() sets
  // next_pid=1 for EP0, which is correct for both DATA and STATUS stages.
  if (ep_num == 0) {
    uint16_t mps = (dev_addr < TU_ARRAY_SIZE(_ep0_mps)) ? _ep0_mps[dev_addr] : 0;
    if (mps == 0) mps = 8;
    hw_endpoint_init(ep, dev_addr, ep_addr, mps, TUSB_XFER_CONTROL, 0);
  }

  // If a normal transfer (non-interrupt) then initiate using
  // sie ctrl registers. Otherwise, interrupt ep registers should
  // already be configured
  if (ep == &epx) {
    // Disable USB IRQ for the window between ep->active=true (set inside
    // hw_endpoint_xfer_start) and the final START_TRANS write.  Without this,
    // a STALL/RX_TIMEOUT/DATA_SEQ IRQ can fire, see ep->active==true, call
    // hw_xfer_complete (clearing active), then we write START_TRANS anyway —
    // hardware starts a real transaction with ep->active==false (Race 1).
    uint32_t const saved = save_and_disable_interrupts();
    epx_prepare_for_start();
    hw_endpoint_xfer_start(ep, buffer, buflen);

    if (trace_port3_request(dev_addr, usbh_dpram->setup_packet)) {
      trace_port3_ep0_state("P3_XFER_PRESTART", dev_addr, ep_addr, buflen);
    }

    // That has set up buffer control, endpoint control etc
    // for host we have to initiate the transfer
    usb_hw->dev_addr_ctrl = (uint32_t) (dev_addr | (ep_num << USB_ADDR_ENDP_ENDPOINT_LSB));

    uint32_t flags = USB_SIE_CTRL_START_TRANS_BITS | SIE_CTRL_BASE |
                     (ep_dir ? USB_SIE_CTRL_RECEIVE_DATA_BITS : USB_SIE_CTRL_SEND_DATA_BITS) |
                     (need_pre(dev_addr) ? USB_SIE_CTRL_PREAMBLE_EN_BITS : 0);
    // Suppress interrupt endpoint polling for the duration of this EPX
    // transaction so the SIE cannot overwrite EPX handshake latches.
    _saved_int_ep_ctrl = usb_hw->int_ep_ctrl;
    usb_hw->int_ep_ctrl = 0;
    // START_TRANS bit on SIE_CTRL seems to exhibit the same behavior as the AVAILABLE bit
    // described in RP2040 Datasheet, release 2.1, section "4.1.2.5.1. Concurrent access".
    // We write everything except the START_TRANS bit first, then wait some cycles.
    TU_LOG(2, "  edpt_xfer START dev=%u ep=0x%02x pid=%u bc=[0x%04x:0x%04x] buf_status=0x%08lx\r\n",
           dev_addr, ep_addr, ep->next_pid,
           tu_u32_low16(*ep->buffer_control), tu_u32_high16(*ep->buffer_control),
           usb_hw->buf_status);
    usb_hw->sie_ctrl = flags & ~USB_SIE_CTRL_START_TRANS_BITS;
    busy_wait_us(1);
    usb_hw->sie_ctrl = flags;
    if (trace_port3_request(dev_addr, usbh_dpram->setup_packet)) {
      trace_port3_ep0_state("P3_XFER_ARMED", dev_addr, ep_addr, buflen);
    }
    restore_interrupts(saved);
  } else {
    hw_endpoint_xfer_start(ep, buffer, buflen);
  }

  return true;
}

bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void) rhport;
  struct hw_endpoint *ep = get_dev_ep(dev_addr, ep_addr);
  if (!ep) return true;

  // Critical section: disabling the endpoint in HW does not dequeue an
  // already-latched NVIC BUFF_STATUS pending.  Without the critical section
  // the IRQ can fire after the HW disable, complete the transfer, let the
  // class driver re-arm (new buffer_control written), then we zero
  // buffer_control under the new live transfer (Race 4).
  uint32_t const saved = save_and_disable_interrupts();

  if (ep == &epx) {
    usb_hw->sie_ctrl = SIE_CTRL_BASE;
    busy_wait_us(1);
  } else {
    usb_hw_clear->int_ep_ctrl = (1u << (ep->interrupt_num + 1));
  }

  // Reset software endpoint state if it is still marked active.
  if (ep->active) {
    hw_endpoint_reset_transfer(ep);
  }

  // Clear hardware buf_ctrl and pending BUFF_STATUS so a stale completion
  // cannot fire after the next transfer is armed on this endpoint.
  *ep->buffer_control = 0;
  if (ep == &epx) {
    // Restore interrupt endpoint polling since this EPX transaction is done.
    if (_saved_int_ep_ctrl) {
      usb_hw->int_ep_ctrl = _saved_int_ep_ctrl;
      _saved_int_ep_ctrl = 0;
    }
    usb_hw_clear->buf_status = 0x3u;
  } else {
    usb_hw_clear->buf_status = 0x3u << ((ep->interrupt_num + 1) * 2);
    usb_hw_set->int_ep_ctrl = (1u << (ep->interrupt_num + 1));
  }

  restore_interrupts(saved);
  return true;
}

bool hcd_setup_send(uint8_t rhport, uint8_t dev_addr, uint8_t const setup_packet[8])
{
  (void) rhport;

  // Copy data into setup packet buffer
  for (uint8_t i = 0; i < 8; i++) {
    usbh_dpram->setup_packet[i] = setup_packet[i];
  }

  // Configure EP0 struct with setup info for the trans complete
  hw_endpoint_t *ep = hw_endpoint_allocate((uint8_t)TUSB_XFER_CONTROL);
  TU_ASSERT(ep);

  // EPX should be inactive
  TU_ASSERT(!ep->active);

  // EP0 out — use the saved MPS for this device, not the (possibly stale) EPX value
  uint16_t mps = (dev_addr < TU_ARRAY_SIZE(_ep0_mps)) ? _ep0_mps[dev_addr] : 0;
  if (mps == 0) {
    if (dev_addr != 0) {
      TU_LOG(2, "hcd_setup_send: dev_addr=%u has no saved EP0 MPS\r\n", dev_addr);
      TU_ASSERT(false);
    }
    // use the USB 2.0 §9.6.1 mandatory minimum of 8.
    mps = 8;
  }
  hw_endpoint_init(ep, dev_addr, 0x00, mps, 0, 0);

  TU_ASSERT(ep->configured);

  // Disable USB IRQ for the window between ep->active=true and the final
  // START_TRANS write.  A STALL/RX_TIMEOUT/DATA_SEQ IRQ fired in this window
  // would see ep->active==true, call hw_xfer_complete (clearing active), then
  // we write START_TRANS — hardware fires a real transaction with ep->active
  // already false; the TRANS_COMPLETE handler silently no-ops (Race 1).
  TU_LOG(2, "  setup_send dev=%u pid=%u bc=[0x%04x:0x%04x] buf_status=0x%08lx sie_status=0x%08lx\r\n",
         dev_addr, ep->next_pid,
         tu_u32_low16(*ep->buffer_control), tu_u32_high16(*ep->buffer_control),
         usb_hw->buf_status, usb_hw->sie_status);
    TU_LOG(2, "  setup pkt: %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
      setup_packet[0], setup_packet[1], setup_packet[2], setup_packet[3],
      setup_packet[4], setup_packet[5], setup_packet[6], setup_packet[7]);
  if (trace_port3_request(dev_addr, setup_packet)) {
    trace_port3_ep0_state("P3_SETUP_PRE", dev_addr, 0x00, 8);
  }
  uint32_t const saved = save_and_disable_interrupts();
  epx_prepare_for_start();
  ep->remaining_len = 8;
  ep->active = true;

  // Set device address
  usb_hw->dev_addr_ctrl = dev_addr;

  // Set pre if we are a low speed device on full speed hub
  uint32_t const flags = SIE_CTRL_BASE | USB_SIE_CTRL_SEND_SETUP_BITS | USB_SIE_CTRL_START_TRANS_BITS |
                         (need_pre(dev_addr) ? USB_SIE_CTRL_PREAMBLE_EN_BITS : 0);

  // Suppress interrupt endpoint polling for the duration of this EPX
  // transaction so the SIE cannot overwrite EPX handshake latches.
  _saved_int_ep_ctrl = usb_hw->int_ep_ctrl;
  usb_hw->int_ep_ctrl = 0;
  // START_TRANS bit on SIE_CTRL seems to exhibit the same behavior as the AVAILABLE bit
  // described in RP2040 Datasheet, release 2.1, section "4.1.2.5.1. Concurrent access".
  // We write everything except the START_TRANS bit first, then wait some cycles.
  usb_hw->sie_ctrl = flags & ~USB_SIE_CTRL_START_TRANS_BITS;
  busy_wait_us(1);
  usb_hw->sie_ctrl = flags;
  if (trace_port3_request(dev_addr, setup_packet)) {
    trace_port3_ep0_state("P3_SETUP_ARMED", dev_addr, 0x00, 8);
  }
  restore_interrupts(saved);

  return true;
}

bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void) rhport;
  struct hw_endpoint *ep = get_dev_ep(dev_addr, ep_addr);
  if (ep) {
    // Critical section: prepare_ep_buffer() (IRQ context) reads ep->next_pid
    // and then does ep->next_pid ^= 1u.  Without protection our zero-write
    // can land between the read and the XOR, losing the reset (Race 5).
    uint32_t const saved = save_and_disable_interrupts();
    ep->next_pid = 0;
    if (ep != &epx) {
      *ep->buffer_control = 0;
      usb_hw_clear->buf_status = 0x3u << ((ep->interrupt_num + 1) * 2);
    }
    restore_interrupts(saved);
  }
  return true;
}

#endif
