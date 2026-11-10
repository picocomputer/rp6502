/*
 * SPDX-FileCopyrightText: Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * SPDX-FileCopyrightText: Copyright (c) 2021 Ha Thach (tinyusb.org) for Double Buffered
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if CFG_TUH_ENABLED && (CFG_TUSB_MCU == OPT_MCU_RP2040) && !CFG_TUH_RPI_PIO_USB && !CFG_TUH_MAX3421

  #include "pico.h"

  #if !defined(PICO_RP2350) || PICO_RP2350 != 1
    #error RP2350 only
  #endif

// port 0 is native USB port, other is counted as software PIO
  #define RHPORT_NATIVE 0

  //--------------------------------------------------------------------+
  // INCLUDE
  //--------------------------------------------------------------------+
  #include "portable/raspberrypi/rp2040/rp2040_usb.h"
  #include "osal/osal.h"

  #include "host/hcd.h"
  #include "host/usbh.h"

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

// Every transfer runs on EPX. The controller's interrupt endpoint polling is not used: a poll that gets the wrong
// DATA PID is never ACKed, so the device repeats it forever, and EPX takes bogus completions meanwhile.
// ep_pool[0] is EP0 of every address; usbh runs one control transfer at a time.
enum {
  EP_POOL_SIZE = 32 // EP0 and 31 endpoints
};
static hw_endpoint_t  ep_pool[EP_POOL_SIZE];
static hw_endpoint_t *epx = &ep_pool[0]; // current active endpoint

// EP0 max packet size per address
static uint8_t ep0_mps[CFG_TUH_DEVICE_MAX + CFG_TUH_HUB + 1];

// An interrupt transfer waits for its poll interval in this state
  #define EPSTATE_WAITING EPSTATE_ABORTING

// Poll interval and time of the next poll of interrupt endpoints, by ep_pool index
static uint8_t  int_interval_ms[EP_POOL_SIZE];
static uint32_t int_due_us[EP_POOL_SIZE];

// After a STOP_TRANS, EPX starts nothing until the next SOF
static bool epx_hold;

// The transfer EPX was stopped on: a transaction on the wire may still complete, so the toggles of its unsent
// buffers are given back at the SOF that ends the hold
static hw_endpoint_t *epx_stopped;

// The controller repeats a timed out transaction until stopped; the third timeout fails the transfer
static uint8_t epx_timeouts;

// Transfers a NAK switched out of EPX, one bit per ep_pool index. Once per SOF one of them polls until it NAKs,
// then EPX goes back, so idle INs do not switch EPX on every NAK.
static uint32_t       epx_parked;
static bool           epx_nak_turn;                 // the next NAK lets a parked transfer poll
static hw_endpoint_t *epx_turn_from;                // EPX goes back to it after the poll
static hw_endpoint_t *epx_polled = &ep_pool[0];     // the parked transfer that polled last

TU_VERIFY_STATIC(EP_POOL_SIZE <= 32, "epx_parked has one bit per ep_pool index");
TU_ATTR_ALWAYS_INLINE static inline uint32_t pool_bit(uint32_t idx) {
  return 1u << idx;
}

// The SIE resets the root bus on attach; hcd_port_reset() re-runs that by dropping VBUS_DETECT
enum {
  ROOT_RERESET_MAX    = 3,   // re-resets per plug-in
  ROOT_RERESET_LOW_US = 1000 // VBUS_DETECT low time
};
static bool    root_bus_reset;     // the SIE reset the bus at the last attach
static bool    root_rereset;       // a re-reset waits for its attach
static uint8_t root_rereset_count; // re-resets since the last unplug

enum {
  SIE_CTRL_SPEED_DISCONNECT = 0,
  SIE_CTRL_SPEED_LOW        = 1,
  SIE_CTRL_SPEED_FULL       = 2,
};

enum {
  EPX_CTRL_DEFAULT = EP_CTRL_ENABLE_BITS | EP_CTRL_INTERRUPT_PER_BUFFER | offsetof(usb_host_dpram_t, epx_data)
};

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

static hw_endpoint_t *edpt_alloc(void) {
  for (uint i = 1; i < TU_ARRAY_SIZE(ep_pool); i++) {
    hw_endpoint_t *ep = &ep_pool[i];
    if (ep->max_packet_size == 0) {
      return ep;
    }
  }
  return NULL;
}

int hcd_free_ep_count(void) {
  int count = 0;
  for (uint i = 1; i < TU_ARRAY_SIZE(ep_pool); i++) {
    if (ep_pool[i].max_packet_size == 0) {
      count++;
    }
  }
  return count;
}

static hw_endpoint_t *edpt_find(uint8_t daddr, uint8_t ep_addr) {
  if (tu_edpt_number(ep_addr) == 0) {
    return &ep_pool[0];
  }
  for (uint32_t i = 1; i < TU_ARRAY_SIZE(ep_pool); i++) {
    hw_endpoint_t *ep = &ep_pool[i];
    if ((ep->dev_addr == daddr) && (ep->max_packet_size > 0) && (ep->ep_addr == ep_addr)) {
      return ep;
    }
  }

  return NULL;
}

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

TU_ATTR_ALWAYS_INLINE static inline uint8_t dev_speed(void) {
  return (usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS) >> USB_SIE_STATUS_SPEED_LSB;
}

TU_ATTR_ALWAYS_INLINE static inline bool need_pre(uint8_t dev_addr) {
  // If this device is different to the speed of the root device
  // (i.e. is a low speed device on a full speed hub) then need pre
  return hcd_port_speed_get(0) != tuh_speed_get(dev_addr);
}

// Point ep_pool[0] at daddr's EP0
static void ep0_select(uint8_t daddr, uint8_t ep_addr) {
  hw_endpoint_t *ep   = &ep_pool[0];
  ep->dev_addr        = daddr;
  ep->ep_addr         = ep_addr;
  ep->need_pre        = need_pre(daddr);
  ep->max_packet_size = ep0_mps[daddr];
}

//--------------------------------------------------------------------+
// EPX
//--------------------------------------------------------------------+
TU_ATTR_ALWAYS_INLINE static inline void sie_stop_xfer(void) {
  uint32_t sie_ctrl = (usb_hw->sie_ctrl & SIE_CTRL_BASE_MASK) | USB_SIE_CTRL_STOP_TRANS_BITS;
  usb_hw->sie_ctrl  = sie_ctrl;
  while (usb_hw->sie_ctrl & USB_SIE_CTRL_STOP_TRANS_BITS) {}
}

// Dispatch waits for the next SOF
static void __tusb_irq_path_func(epx_hold_until_sof)(void) {
  epx_hold = true;
  (void)usb_hw->sof_rd; // acknowledges a SOF already raised
  usb_hw_set->inte = USB_INTE_HOST_SOF_BITS;
}

static void __tusb_irq_path_func(sie_start_xfer)(bool send_setup, bool is_rx, bool need_pre) {
  // drop status latched before this transaction
  usb_hw_clear->sie_status = USB_SIE_STATUS_RX_TIMEOUT_BITS | USB_SIE_STATUS_STALL_REC_BITS |
                             USB_SIE_STATUS_TRANS_COMPLETE_BITS | USB_SIE_STATUS_DATA_SEQ_ERROR_BITS;
  usb_hw_clear->nak_poll   = USB_NAK_POLL_EPX_STOPPED_ON_NAK_BITS;
  epx_timeouts             = 0;

  uint32_t sie_ctrl = usb_hw->sie_ctrl & SIE_CTRL_BASE_MASK; // preserve base bits
  if (send_setup) {
    sie_ctrl |= USB_SIE_CTRL_SEND_SETUP_BITS;
  } else {
    sie_ctrl |= (is_rx ? USB_SIE_CTRL_RECEIVE_DATA_BITS : USB_SIE_CTRL_SEND_DATA_BITS);
  }
  if (need_pre) {
    sie_ctrl |= USB_SIE_CTRL_PREAMBLE_EN_BITS;
  }

  // START_TRANS bit on SIE_CTRL has the same behavior as the AVAILABLE bit
  // described in RP2350 Datasheet, section "12.7.3.7.1. Concurrent access".
  // We write everything except the START_TRANS bit first, then wait some cycles.
  usb_hw->sie_ctrl = sie_ctrl;
  busy_wait_at_least_cycles(12);
  usb_hw->sie_ctrl = sie_ctrl | USB_SIE_CTRL_START_TRANS_BITS;
}

// Undo the PID toggle of buffers NOT completed on the wire and return their length.
// A buffer completed on wire means: controller reached STATUS phase (ACK received).
//   OUT completed: FULL cleared to 0 in STATUS phase (was 1 when armed)
//   IN  completed: FULL set to 1 in STATUS phase (was 0 when armed)
// So undo when: AVAIL=1 (never started), or (OUT: FULL=1) or (IN: FULL=0)
static uint16_t __tusb_irq_path_func(epx_undo_unsent)(hw_endpoint_t *ep) {
  uint32_t   buf_ctrl = usbh_dpram->epx_buf_ctrl;
  const bool is_out   = (tu_edpt_dir(ep->ep_addr) == TUSB_DIR_OUT);
  uint16_t   len      = 0;

  do {
    const uint16_t bc16 = (uint16_t)buf_ctrl;
    if (bc16) {
      const bool avail = (bc16 & USB_BUF_CTRL_AVAIL);
      const bool full  = (bc16 & USB_BUF_CTRL_FULL);
      if (avail || (is_out ? full : !full)) {
        len += bc16 & USB_BUF_CTRL_LEN_MASK;
        ep->next_pid ^= 1u;
      }
    }

    if (usbh_dpram->epx_ctrl & EP_CTRL_DOUBLE_BUFFERED_BITS) {
      buf_ctrl >>= 16;
    } else {
      buf_ctrl = 0;
    }
  } while (buf_ctrl > 0);

  return len;
}

// Save buffer context for EPX preemption (called after the SIE stopped)
static void __tusb_irq_path_func(epx_save_context)(hw_endpoint_t *ep) {
  const uint16_t len = epx_undo_unsent(ep);
  ep->remaining_len += len;
  if (tu_edpt_dir(ep->ep_addr) == TUSB_DIR_OUT) {
    ep->user_buf -= len;
  }
  usbh_dpram->epx_buf_ctrl = 0;
  ep->state                = EPSTATE_PENDING;
}

// switch epx to new endpoint and start the transfer
static void __tusb_irq_path_func(epx_switch_ep)(hw_endpoint_t *ep) {
  const bool is_setup = (ep->state == EPSTATE_PENDING_SETUP);

  epx       = ep; // switch pointer
  ep->state = EPSTATE_ACTIVE;
  epx_parked &= ~pool_bit((uint32_t)(ep - ep_pool));

  if (is_setup) {
    usb_hw->dev_addr_ctrl = ep->dev_addr;
    sie_start_xfer(true, false, ep->need_pre);
  } else {
    // an interrupt endpoint runs as bulk and gives EPX up at its first NAK
    uint8_t type = ep->transfer_type;
    if (type == TUSB_XFER_INTERRUPT) {
      type                 = TUSB_XFER_BULK;
      usb_hw_set->nak_poll = USB_NAK_POLL_STOP_EPX_ON_NAK_BITS;
    }
    usbh_dpram->epx_ctrl = EPX_CTRL_DEFAULT | ((uint32_t)type << EP_CTRL_BUFFER_TYPE_LSB);
    rp2usb_buffer_start(ep, &usbh_dpram->epx_ctrl, &usbh_dpram->epx_buf_ctrl,
                        tu_edpt_dir(ep->ep_addr) == TUSB_DIR_IN);
    usb_hw->dev_addr_ctrl = (uint32_t)(ep->dev_addr | (tu_edpt_number(ep->ep_addr) << USB_ADDR_ENDP_ENDPOINT_LSB));
    sie_start_xfer(false, tu_edpt_dir(ep->ep_addr) == TUSB_DIR_IN, ep->need_pre);
  }
}

TU_ATTR_ALWAYS_INLINE static inline bool epx_is_pending(const hw_endpoint_t *ep) {
  return ep->state == EPSTATE_PENDING || ep->state == EPSTATE_PENDING_SETUP;
}

// An interrupt transfer may poll once its interval has passed
TU_ATTR_ALWAYS_INLINE static inline bool int_edpt_due(uint32_t idx) {
  return (int32_t)(time_us_32() - int_due_us[idx]) >= 0;
}

// Half a frame early, so the SOF one interval after this poll finds the next one due
static void __tusb_irq_path_func(int_edpt_schedule)(const hw_endpoint_t *ep) {
  const uint32_t idx = (uint32_t)(ep - ep_pool);
  int_due_us[idx]    = time_us_32() + int_interval_ms[idx] * 1000u - 500u;
}

// Round-robin find next pending ep after current epx, current epx last; parked ones only if they may be
static hw_endpoint_t *__tusb_irq_path_func(epx_next_pending)(hw_endpoint_t *cur_ep, bool parked) {
  const uint     cur_idx = (uint)(cur_ep - &ep_pool[0]);
  const uint32_t skip    = parked ? 0 : epx_parked;
  for (uint n = 1; n <= TU_ARRAY_SIZE(ep_pool); n++) {
    const uint i = (cur_idx + n) % TU_ARRAY_SIZE(ep_pool);
    if (epx_is_pending(&ep_pool[i]) && !(skip & pool_bit(i))) {
      return &ep_pool[i];
    }
  }
  return NULL;
}

// Release an unfinished EPX transfer's buffers: a STALL or RX timeout leaves one AVAILABLE, and a buffer completed
// before it leaves BUFF_STATUS pending, which the ISR would otherwise apply to the next transfer
static void __tusb_irq_path_func(epx_drop_buffers)(void) {
  usbh_dpram->epx_buf_ctrl = 0;
  for (uint i = 0; i < 2 && (usb_hw->buf_status & 1u); i++) { // re-sets once if both buffers completed
    usb_hw_clear->buf_status = 1u;
  }
}

// Stop the SIE on epx; its buffers are settled at the next SOF
static void __tusb_irq_path_func(epx_stop)(void) {
  sie_stop_xfer();
  epx_stopped = epx;
  epx_hold_until_sof();
}

// Cancel ep's transfer without completing it; if it owns EPX, stop the SIE and drop status latched for it so the
// ISR cannot apply it to the next transfer. Caller holds the critical section and starts the next pending ep.
static void epx_retire(hw_endpoint_t *ep) {
  if (ep == epx && ep->state == EPSTATE_ACTIVE) {
    epx_stop();
    usb_hw_clear->sie_status =
      USB_SIE_STATUS_RX_TIMEOUT_BITS | USB_SIE_STATUS_TRANS_COMPLETE_BITS | USB_SIE_STATUS_STALL_REC_BITS;
    usb_hw_clear->nak_poll = USB_NAK_POLL_EPX_STOPPED_ON_NAK_BITS;
  }

  rp2usb_reset_transfer(ep);
}

static void __tusb_irq_path_func(epx_start_next_pending)(void) {
  if (epx->state != EPSTATE_ACTIVE && !epx_hold) {
    hw_endpoint_t *next_ep = epx_next_pending(epx, true);
    if (next_ep != NULL) {
      epx_switch_ep(next_ep);
    }
  }
}

//--------------------------------------------------------------------+
// Interrupt handlers
//--------------------------------------------------------------------+
static void __tusb_irq_path_func(xfer_complete_isr)(hw_endpoint_t *ep, xfer_result_t xfer_result) {
  if (ep->transfer_type == TUSB_XFER_INTERRUPT) {
    int_edpt_schedule(ep);
  }

  // Mark transfer as done before we tell the tinyusb stack
  uint32_t xferred_len = ep->xferred_len;
  rp2usb_reset_transfer(ep);
  hcd_event_xfer_complete(ep->dev_addr, ep->ep_addr, xferred_len, xfer_result, true);
}

static void __tusb_irq_path_func(handle_buf_status_isr)(void) {
  // Double-buffered: if both buffers completed at once, buf_status re-sets
  // immediately after clearing (datasheet Table 406). Process the second buffer too.
  while (usb_hw->buf_status & 1u) {
    const uint8_t buf_id     = (usb_hw->buf_cpu_should_handle & 1u) ? 1 : 0;
    usb_hw_clear->buf_status = 1u; // clear
    if (epx->state != EPSTATE_ACTIVE) {
      continue; // completed after STOP_TRANS: settled at SOF
    }
    epx_timeouts = 0;

    if (rp2usb_xfer_continue(epx, &usbh_dpram->epx_ctrl, &usbh_dpram->epx_buf_ctrl, buf_id,
                             tu_edpt_dir(epx->ep_addr) == TUSB_DIR_IN)) {
      xfer_complete_isr(epx, XFER_RESULT_SUCCESS);
    }
  }
}

// HOST_SOF is enabled while EPX is held, a transfer is parked or an interrupt transfer waits
static void __tusb_irq_path_func(sof_service)(void) {
  (void)usb_hw->sof_rd; // acknowledges HOST_SOF
  epx_hold = false;
  if (epx_stopped != NULL) {
    epx_undo_unsent(epx_stopped);
    epx_drop_buffers();
    epx_stopped = NULL;
  }

  uint32_t pending = 0;
  bool     waiting = false;
  for (uint i = 0; i < TU_ARRAY_SIZE(ep_pool); i++) {
    hw_endpoint_t *ep = &ep_pool[i];
    if (ep->state == EPSTATE_WAITING) {
      if (int_edpt_due(i)) {
        ep->state            = EPSTATE_PENDING; // takes EPX at the next NAK
        usb_hw_set->nak_poll = USB_NAK_POLL_STOP_EPX_ON_NAK_BITS;
      } else {
        waiting = true;
      }
    }
    if (epx_is_pending(ep)) {
      pending |= pool_bit(i);
    }
  }

  // a parked transfer may take EPX at the next NAK
  epx_parked &= pending;
  if (epx_parked) {
    epx_nak_turn         = true;
    usb_hw_set->nak_poll = USB_NAK_POLL_STOP_EPX_ON_NAK_BITS;
  }

  if (!epx_parked && !waiting) {
    usb_hw_clear->inte = USB_INTE_HOST_SOF_BITS;
  }
}

// the SDK registers this as the ISR, so it records its own span; under tuh_int_handler() the depth counter keeps one
static void __tusb_irq_path_func(hcd_rp2350_irq)(void) {
  if (TUH_SYSVIEW_ISR_TRACED) { TU_SYSVIEW_ISR_ENTER(); }
  const uint32_t status = usb_hw->ints;

  if (status & USB_INTS_HOST_CONN_DIS_BITS) {
    usb_hw_clear->sie_status = USB_SIE_STATUS_SPEED_BITS; // ack first so a later change raises it again
    const uint8_t speed      = dev_speed();
    if (speed == SIE_CTRL_SPEED_DISCONNECT) {
      if (!root_rereset) {
        root_rereset_count = 0;
      }
      hcd_event_device_remove(RHPORT_NATIVE, true);
    } else {
      usb_hw->sie_ctrl = SIE_CTRL_BASE | (speed == SIE_CTRL_SPEED_LOW ? USB_SIE_CTRL_KEEP_ALIVE_EN_BITS
                                                                      : USB_SIE_CTRL_SOF_EN_BITS);
      root_rereset   = false;
      root_bus_reset = true;
      hcd_event_device_attach(RHPORT_NATIVE, true);
    }
  }

  // Completions first, then the error latches
  if (status & USB_INTS_BUFF_STATUS_BITS) {
    handle_buf_status_isr();
  }

  if (status & USB_INTS_TRANS_COMPLETE_BITS) {
    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS;
    if (epx->state == EPSTATE_ACTIVE && (usb_hw->sie_ctrl & USB_SIE_CTRL_SEND_SETUP_BITS)) {
      usb_hw->sie_ctrl = usb_hw->sie_ctrl & SIE_CTRL_BASE_MASK; // clear setup bit
      epx->xferred_len = 8;
      xfer_complete_isr(epx, XFER_RESULT_SUCCESS);
    }
  }

  // a NAK between RX timeouts makes them not consecutive
  const uint32_t err = usb_hw->sie_status & (USB_SIE_STATUS_STALL_REC_BITS | USB_SIE_STATUS_RX_TIMEOUT_BITS |
                                             USB_SIE_STATUS_NAK_REC_BITS);
  if (err & USB_SIE_STATUS_NAK_REC_BITS) {
    epx_timeouts = 0;
  }
  if (err & (USB_SIE_STATUS_STALL_REC_BITS | USB_SIE_STATUS_RX_TIMEOUT_BITS)) {
    usb_hw_clear->sie_status = err;
    if (epx->state == EPSTATE_ACTIVE && ((err & USB_SIE_STATUS_STALL_REC_BITS) || ++epx_timeouts >= 3)) {
      epx_stop();
      xfer_complete_isr(epx, (err & USB_SIE_STATUS_STALL_REC_BITS) ? XFER_RESULT_STALLED : XFER_RESULT_FAILED);
    }
  }

  // The controller ACKs and drops an IN with the wrong DATA PID, and the buffer stays armed
  if (status & USB_INTS_ERROR_DATA_SEQ_BITS) {
    usb_hw_clear->sie_status = USB_SIE_STATUS_DATA_SEQ_ERROR_BITS;
  }

  if (status & USB_INTS_EPX_STOPPED_ON_NAK_BITS) {
    usb_hw_clear->nak_poll = USB_NAK_POLL_EPX_STOPPED_ON_NAK_BITS;
    if (epx->state == EPSTATE_ACTIVE) {
      hw_endpoint_t *next_ep;
      if (epx->transfer_type == TUSB_XFER_INTERRUPT) {
        // a NAKed poll waits for its interval
        epx_save_context(epx);
        int_edpt_schedule(epx);
        epx->state       = EPSTATE_WAITING;
        usb_hw_set->inte = USB_INTE_HOST_SOF_BITS;
        next_ep          = epx_next_pending(epx, true);
      } else {
        next_ep = epx_next_pending(epx, false);
        if (next_ep == NULL && epx_nak_turn) {
          epx_nak_turn = false;
          next_ep      = epx_next_pending(epx_polled, true);
          if (next_ep != NULL) {
            epx_turn_from = epx;
            epx_polled    = next_ep;
          }
        } else if (next_ep == NULL && epx_turn_from) {
          if (epx_is_pending(epx_turn_from)) {
            next_ep = epx_turn_from;
          }
          epx_turn_from = NULL;
        }
        if (next_ep == NULL) {
          usb_hw_clear->nak_poll = USB_NAK_POLL_STOP_EPX_ON_NAK_BITS;
          next_ep                = epx;
        } else {
          epx_parked |= pool_bit((uint32_t)(epx - ep_pool));
          usb_hw_set->inte = USB_INTE_HOST_SOF_BITS;
        }
        epx_save_context(epx); // re-armed from buffer 0
      }
      if (next_ep != NULL) {
        epx_switch_ep(next_ep);
      }
    }
  }

  // read live: a hold above acknowledged the SOF it must not see
  if (usb_hw->ints & USB_INTS_HOST_SOF_BITS) {
    sof_service();
  }

  epx_start_next_pending();

  if (TUH_SYSVIEW_ISR_TRACED) { TU_SYSVIEW_ISR_EXIT(); }
}

void __tusb_irq_path_func(hcd_int_handler)(uint8_t rhport, bool in_isr) {
  (void)rhport;
  (void)in_isr;
  hcd_rp2350_irq();
}

//--------------------------------------------------------------------+
// HCD API
//--------------------------------------------------------------------+
bool hcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
  (void)rhport;
  (void)rh_init;
  pico_trace("hcd_init %d\n", rhport);
  assert(rhport == 0);

  // Reset any previous state
  rp2usb_init();

  // Force VBUS detect to always present, for now we assume vbus is always provided (without using VBUS En)
  usb_hw->pwr = USB_USB_PWR_VBUS_DETECT_BITS | USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS;

  // Remove shared irq if it was previously added so as not to fill up shared irq slots
  irq_remove_handler(USBCTRL_IRQ, hcd_rp2350_irq);
  irq_add_shared_handler(USBCTRL_IRQ, hcd_rp2350_irq, PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);

  // clear epx eps
  memset(&ep_pool, 0, sizeof(ep_pool));
  ep_pool[0].dpram_buf = usbh_dpram->epx_data; // EP0, transfer_type CONTROL (0)

  // Enable in host mode with SOF / Keep alive on
  usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS | USB_MAIN_CTRL_HOST_NDEVICE_BITS;
  usb_hw->sie_ctrl  = SIE_CTRL_BASE;
  // HOST_RESUME stays off: no host suspend support, and nothing would clear SIE_STATUS.RESUME
  usb_hw->inte = USB_INTE_BUFF_STATUS_BITS | USB_INTE_HOST_CONN_DIS_BITS | USB_INTE_STALL_BITS |
                 USB_INTE_TRANS_COMPLETE_BITS | USB_INTE_ERROR_RX_TIMEOUT_BITS | USB_INTE_ERROR_DATA_SEQ_BITS |
                 USB_INTE_EPX_STOPPED_ON_NAK_BITS;

  return true;
}

bool hcd_deinit(uint8_t rhport) {
  (void)rhport;
  irq_remove_handler(USBCTRL_IRQ, hcd_rp2350_irq);
  reset_block(RESETS_RESET_USBCTRL_BITS);
  unreset_block_wait(RESETS_RESET_USBCTRL_BITS);

  // Release allocated resources
  rp2usb_deinit();

  return true;
}

// The controller resets the bus itself 100 ms after attach or VBUS_DETECT: SE0 for 50 ms, then it sets SPEED and
// raises HOST_CONN_DIS. SIE_CTRL.RESET_BUS self-clears within ~1 us. A reset of a device already attached drops
// VBUS_DETECT, which ends in a remove and a new attach.
void hcd_port_reset(uint8_t rhport) {
  (void)rhport;
  if (root_bus_reset) {
    root_bus_reset = false;
    return;
  }
  if (root_rereset_count >= ROOT_RERESET_MAX) {
    return;
  }
  root_rereset_count++;
  root_rereset      = true;
  usb_hw_clear->pwr = USB_USB_PWR_VBUS_DETECT_BITS;
  busy_wait_us_32(ROOT_RERESET_LOW_US);
  usb_hw_set->pwr = USB_USB_PWR_VBUS_DETECT_BITS;
}

void hcd_port_reset_end(uint8_t rhport) {
  (void)rhport;
}

bool hcd_port_connect_status(uint8_t rhport) {
  (void)rhport;
  return !root_rereset && (usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS);
}

tusb_speed_t hcd_port_speed_get(uint8_t rhport) {
  (void)rhport;
  switch (dev_speed()) {
    case SIE_CTRL_SPEED_LOW:
      return TUSB_SPEED_LOW;
    case SIE_CTRL_SPEED_FULL:
      return TUSB_SPEED_FULL;
    default:
      return TUSB_SPEED_INVALID;
  }
}

// Cancel ep's transfer and free its slot, which a new endpoint may take before the next SOF
static void edpt_release(hw_endpoint_t *ep) {
  epx_retire(ep);
  if (epx_stopped == ep) {
    epx_stopped = NULL;
    epx_drop_buffers();
  }
  ep->max_packet_size = 0; // mark as unused
}

// Close all opened endpoint belong to this device
void hcd_device_close(uint8_t rhport, uint8_t dev_addr) {
  (void)rhport;

  rp2usb_critical_enter();

  if (ep_pool[0].dev_addr == dev_addr) {
    epx_retire(&ep_pool[0]); // the EP0 slot stays: it serves every address
  }

  for (size_t i = 1; i < TU_ARRAY_SIZE(ep_pool); i++) {
    hw_endpoint_t *ep = &ep_pool[i];
    if (ep->dev_addr == dev_addr && ep->max_packet_size > 0) {
      edpt_release(ep);
    }
  }
  epx_start_next_pending();

  rp2usb_critical_exit();
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
  if (ep_desc->bmAttributes.xfer == TUSB_XFER_CONTROL) {
    ep0_mps[dev_addr] = (uint8_t)tu_edpt_packet_size(ep_desc);
    return true;
  }

  hw_endpoint_t *ep = edpt_alloc();
  TU_ASSERT(ep);

  ep->max_packet_size = tu_edpt_packet_size(ep_desc);
  ep->ep_addr         = ep_desc->bEndpointAddress;
  ep->dev_addr        = dev_addr;
  ep->transfer_type   = ep_desc->bmAttributes.xfer;
  ep->need_pre        = need_pre(dev_addr);
  ep->next_pid        = 0u;
  ep->dpram_buf       = usbh_dpram->epx_data;

  // full and low speed bInterval is in frames
  const uint32_t idx   = (uint32_t)(ep - ep_pool);
  int_interval_ms[idx] = ep_desc->bInterval ? ep_desc->bInterval : 1;
  int_due_us[idx]      = time_us_32();

  return true;
}

bool hcd_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep_addr) {
  (void)rhport;
  hw_endpoint_t *ep = edpt_find(daddr, ep_addr);
  TU_VERIFY(ep != NULL);

  rp2usb_critical_enter();
  edpt_release(ep);
  epx_start_next_pending();
  rp2usb_critical_exit();

  return true;
}

bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void)rhport;
  hw_endpoint_t *ep = edpt_find(dev_addr, ep_addr);
  TU_VERIFY(ep != NULL);

  rp2usb_critical_enter();
  epx_retire(ep);
  epx_start_next_pending();
  rp2usb_critical_exit();

  return true;
}

bool hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen) {
  (void)rhport;

  hw_endpoint_t *ep = edpt_find(dev_addr, ep_addr);
  TU_ASSERT(ep);

  rp2usb_critical_enter();

  // Control data and status stages start with DATA1
  if (tu_edpt_number(ep_addr) == 0) {
    ep0_select(dev_addr, ep_addr);
    ep->next_pid = 1;
  }

  ep->user_buf      = buffer;
  ep->remaining_len = buflen;
  ep->xferred_len   = 0;
  const uint32_t idx = (uint32_t)(ep - ep_pool);
  epx_parked &= ~pool_bit(idx);

  if (ep->transfer_type == TUSB_XFER_INTERRUPT && !int_edpt_due(idx) &&
      int_due_us[idx] - time_us_32() <= int_interval_ms[idx] * 1000u) {
    ep->state        = EPSTATE_WAITING;
    usb_hw_set->inte = USB_INTE_HOST_SOF_BITS;
  } else {
    ep->state = EPSTATE_PENDING;
    if (epx->state == EPSTATE_ACTIVE || epx_hold) {
      usb_hw_set->nak_poll = USB_NAK_POLL_STOP_EPX_ON_NAK_BITS;
    } else {
      epx_switch_ep(ep);
    }
  }

  rp2usb_critical_exit();
  return true;
}

bool hcd_setup_send(uint8_t rhport, uint8_t dev_addr, const uint8_t setup_packet[8]) {
  (void)rhport;

  hw_endpoint_t *ep = &ep_pool[0];

  rp2usb_critical_enter();

  // Copy data into setup packet buffer (usbh only schedules one setup at a time)
  for (uint8_t i = 0; i < 8; i++) {
    usbh_dpram->setup_packet[i] = setup_packet[i];
  }

  ep0_select(dev_addr, 0); // setup is OUT
  ep->remaining_len = 8;
  ep->xferred_len   = 0;
  ep->state         = EPSTATE_PENDING_SETUP;
  epx_parked &= ~pool_bit(0);

  if (epx->state == EPSTATE_ACTIVE || epx_hold) {
    usb_hw_set->nak_poll = USB_NAK_POLL_STOP_EPX_ON_NAK_BITS;
  } else {
    epx_switch_ep(ep);
  }

  rp2usb_critical_exit();
  return true;
}

// The halted endpoint's transfer has already completed or been aborted
bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void)rhport;
  hw_endpoint_t *ep = edpt_find(dev_addr, ep_addr);
  TU_VERIFY(ep != NULL);
  ep->next_pid = 0;
  return true;
}

#endif
