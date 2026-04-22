/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TUSB_CONFIG_H
#define _TUSB_CONFIG_H

// #undef CFG_TUSB_DEBUG
// #define CFG_TUSB_DEBUG 2

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

#define CFG_TUH_HUB (5)
#define CFG_TUH_HID (8)
// Boolean enable; volume count lives in usb/msc.c
#define CFG_TUH_MSC (1)
// Max simultaneous CDC devices
#define CFG_TUH_CDC (4)
// Enable all serial-adapter drivers
#define CFG_TUH_CDC_FTDI (1)
#define CFG_TUH_CDC_CP210X (1)
#define CFG_TUH_CDC_CH34X (1)
#define CFG_TUH_CDC_PL2303 (1)

// Devices; limited by free endpoints
#define CFG_TUH_DEVICE_MAX (16)

#endif
