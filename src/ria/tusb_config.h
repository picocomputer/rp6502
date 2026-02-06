/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TUSB_CONFIG_H
#define _TUSB_CONFIG_H

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_HOST

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif

#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUH_ENABLED (1)

#define CFG_TUH_ENUMERATION_BUFSIZE (1024)

// One root hub and four more
#define CFG_TUH_HUB (5)
// 8 for four keyboards and mice reporting as both
// 4 for gamepads, and 4 for weird device extras
#define CFG_TUH_HID (16)
// Enabled, usb/msc driver defines quantity of 8
#define CFG_TUH_MSC (1)
// CDC host (serial adapters)
#define CFG_TUH_CDC (8)
#define CFG_TUH_CDC_FTDI (1)
#define CFG_TUH_CDC_CP210X (1)
#define CFG_TUH_CDC_CH34X (1)
#define CFG_TUH_CDC_PL2303 (1)
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM { 115200, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 }
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM (CDC_CONTROL_LINE_STATE_DTR | CDC_CONTROL_LINE_STATE_RTS)
// No vendor driver
#define CFG_TUH_VENDOR (0)

// 8 drives, 8 input devices, and 8 CDC
#define CFG_TUH_DEVICE_MAX (24)

#endif
