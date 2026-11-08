/*
 * SPDX-FileCopyrightText: Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * SPDX-FileCopyrightText: Copyright (c) 2021 Ha Thach (tinyusb.org) for Double Buffered
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if CFG_TUSB_MCU == OPT_MCU_RP2040 && CFG_TUH_ENABLED

  #include <stdlib.h>
  #include "portable/raspberrypi/rp2040/rp2040_usb.h"

  #include "host/hcd.h"

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTOTYPE
//--------------------------------------------------------------------+
critical_section_t rp2usb_lock;

// in src/core/api/std.c: a read_xram forwards each packet as it lands
void std_xram_landed(const void *dst, uint32_t n);

//--------------------------------------------------------------------+
// Implementation
//--------------------------------------------------------------------+
// Provide own byte by byte memcpy as not all copies are aligned.
// Use volatile to prevent compiler from widening to 16/32-bit accesses
// which cause hard fault on RP2350 when dst/src point to USB DPRAM.
static void unaligned_memcpy(uint8_t *dst, const uint8_t *src, size_t n) {
  volatile uint8_t *vdst = dst;
  const volatile uint8_t *vsrc = src;
  while (n--) {
    *vdst++ = *vsrc++;
  }
}

void rp2usb_init(void) {
  // Reset usb controller
  reset_block(RESETS_RESET_USBCTRL_BITS);
  unreset_block_wait(RESETS_RESET_USBCTRL_BITS);

  #ifdef __GNUC__
    // Clear any previous state just in case
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Warray-bounds"
    #if __GNUC__ > 6
      #pragma GCC diagnostic ignored "-Wstringop-overflow"
    #endif
  #endif
  memset(usb_dpram, 0, sizeof(*usb_dpram));
  #ifdef __GNUC__
    #pragma GCC diagnostic pop
  #endif

  // Mux the controller to the onboard usb phy
  usb_hw->muxing = USB_USB_MUXING_TO_PHY_BITS | USB_USB_MUXING_SOFTCON_BITS;

  TU_LOG2_INT(sizeof(hw_endpoint_t));

  critical_section_init(&rp2usb_lock);
}

void rp2usb_deinit(void) {
  critical_section_deinit(&rp2usb_lock);
}

void __tusb_irq_path_func(rp2usb_reset_transfer)(hw_endpoint_t *ep) {
  ep->state         = EPSTATE_IDLE;
  ep->remaining_len = 0;
  ep->xferred_len   = 0;
  ep->user_buf      = 0;
}

void __tusb_irq_path_func(bufctrl_write32)(io_rw_32 *buf_reg, uint32_t value) {
  const uint32_t current    = *buf_reg;
  const uint32_t avail_mask = USB_BUF_CTRL_AVAIL | (USB_BUF_CTRL_AVAIL << 16);
  if (current & value & avail_mask) {
    panic("buf_ctrl @ 0x%lX already available", (uintptr_t)buf_reg);
  }
  *buf_reg = value & ~(USB_BUF_CTRL_AVAIL | (USB_BUF_CTRL_AVAIL << 16)); // write other bits first

  // Section 12.7.3.7.1 Concurrent access: after write to buffer control,
  // wait for USB controller to see the update before setting AVAILABLE. The host moves to the second EPX buffer
  // by itself.
  if (value & (USB_BUF_CTRL_AVAIL | (USB_BUF_CTRL_AVAIL << 16))) {
    busy_wait_at_least_cycles(12);
    *buf_reg = value; // then set AVAILABLE bit last
  }
}

void __tusb_irq_path_func(bufctrl_write16)(io_rw_16 *buf_reg16, uint16_t value) {
  const uint16_t current = *buf_reg16;
  if (current & value & USB_BUF_CTRL_AVAIL) {
    panic("buf_ctrl @ 0x%lX already available", (uintptr_t)buf_reg16);
  }
  *buf_reg16 = value & (uint16_t)~USB_BUF_CTRL_AVAIL; // write other bits first

  // Section 12.7.3.7.1 Concurrent access
  if (value & USB_BUF_CTRL_AVAIL) {
    busy_wait_at_least_cycles(12);
    *buf_reg16 = value; // then set AVAILABLE bit last
  }
}

// prepare buffer, move data if tx, return buffer control
uint16_t __tusb_irq_path_func(bufctrl_prepare16)(hw_endpoint_t *ep, uint8_t *dpram_buf, bool is_rx) {
  const uint16_t buflen = tu_min16(ep->remaining_len, ep->max_packet_size);
  ep->remaining_len -= buflen;

  uint16_t buf_ctrl = buflen | USB_BUF_CTRL_AVAIL;
  if (ep->next_pid) {
    buf_ctrl |= USB_BUF_CTRL_DATA1_PID;
  }
  ep->next_pid ^= 1u;

  if (!is_rx) {
    if (buflen) {
      // Copy data from user buffer to hw buffer
      unaligned_memcpy(dpram_buf, ep->user_buf, buflen);
      ep->user_buf += buflen;
    }

    buf_ctrl |= USB_BUF_CTRL_FULL;
  }

  // Is this the last buffer? Will trigger the trans complete irq but also stop it polling.
  // This is used to detect setup packets being sent in host mode
  if (ep->remaining_len == 0) {
    buf_ctrl |= USB_BUF_CTRL_LAST;
  }

  return buf_ctrl;
}

// Start transaction on hw buffer
void __tusb_irq_path_func(rp2usb_buffer_start)(hw_endpoint_t *ep, io_rw_32 *ep_reg, io_rw_32 *buf_reg, bool is_rx) {
  // always compute and start with buffer 0
  uint32_t buf_ctrl = bufctrl_prepare16(ep, ep->dpram_buf, is_rx) | USB_BUF_CTRL_SEL;
  uint32_t ep_ctrl  = *ep_reg;

  // Only bulk transfers are double buffered
  if (ep->remaining_len && ep->transfer_type == TUSB_XFER_BULK) {
    // Use buffer 1 (double buffered) if there is still data
    buf_ctrl |= (uint32_t)bufctrl_prepare16(ep, ep->dpram_buf + 64, is_rx) << 16;
    ep_ctrl |= EP_CTRL_DOUBLE_BUFFERED_BITS;
  } else {
    // Only buf0 used: clear DOUBLE_BUFFERED so controller doesn't toggle buffer selector
    ep_ctrl &= ~(uint32_t)EP_CTRL_DOUBLE_BUFFERED_BITS;
  }
  *ep_reg = ep_ctrl;

  // Finally, write to buffer control; START_TRANS starts the transfer
  bufctrl_write32(buf_reg, buf_ctrl);
}

// sync endpoint buffer and return transferred bytes
static uint16_t __tusb_irq_path_func(bufctrl_sync16)(hw_endpoint_t *ep, bool is_rx, uint16_t buf_ctrl,
                                                     uint8_t *dpram_buf) {
  const uint16_t xferred_bytes = buf_ctrl & USB_BUF_CTRL_LEN_MASK;

  if (!is_rx) {
    // We are continuing a transfer here. If we are TX, we have successfully
    // sent some data can increase the length we have sent
    assert(!(buf_ctrl & USB_BUF_CTRL_FULL));
  } else {
    // If we have received some data, so can increase the length
    // we have received AFTER we have copied it to the user buffer at the appropriate offset
    assert(buf_ctrl & USB_BUF_CTRL_FULL);
    unaligned_memcpy(ep->user_buf, dpram_buf, xferred_bytes);
    std_xram_landed(ep->user_buf, xferred_bytes);
    ep->user_buf += xferred_bytes;
  }
  ep->xferred_len += xferred_bytes;

  // Short packet
  if (xferred_bytes < ep->max_packet_size) {
    // Reduce total length as this is last packet
    ep->remaining_len = 0;
  }

  return xferred_bytes;
}

// Returns true if transfer is complete.
// buf_id: which buffer completed (from BUFF_CPU_SHOULD_HANDLE, only used for double-buffered).
bool __tusb_irq_path_func(rp2usb_xfer_continue)(hw_endpoint_t *ep, io_rw_32 *ep_reg, io_rw_32 *buf_reg, uint8_t buf_id,
                                                bool is_rx) {
  hw_endpoint_lock_update(ep, 1);

  const bool is_double = (*ep_reg) & EP_CTRL_DOUBLE_BUFFERED_BITS;

  // Double-buffered: buf_id from BUFF_CPU_SHOULD_HANDLE indicates which buffer completed.
  uint8_t *dpram_buf = ep->dpram_buf;
  if (buf_id) {
    dpram_buf += 64; // buf1 offset
  }

  io_rw_16 *buf_reg16  = (io_rw_16 *)buf_reg;
  uint16_t  buf_ctrl16 = *(buf_reg16 + buf_id);

  const uint16_t xact_bytes = bufctrl_sync16(ep, is_rx, buf_ctrl16, dpram_buf);
  const bool     is_last    = buf_ctrl16 & USB_BUF_CTRL_LAST;
  const bool     is_short   = xact_bytes < ep->max_packet_size;
  const bool     is_done    = is_short || is_last;

  // Short packet on rx with double buffer: the controller has ended the transfer, and the other buffer is unused.
  // Roll back its PID and reset the buffer control.
  if (is_short && is_double && is_rx && !is_last) {
    ep->next_pid ^= 1u;
    *buf_reg = 0;
    hw_endpoint_lock_update(ep, -1);
    return true;
  }

  if (!is_done && ep->remaining_len > 0) {
    // ping-pong: arm the completed buffer with new data
    const uint16_t buf_ctrl16_new = bufctrl_prepare16(ep, dpram_buf, is_rx);
    bufctrl_write16(buf_reg16 + buf_id, buf_ctrl16_new);
  }

  hw_endpoint_lock_update(ep, -1);
  return is_done;
}

#endif
