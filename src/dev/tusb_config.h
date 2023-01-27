/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TUSB_CONFIG_H
#define _TUSB_CONFIG_H

#define CFG_TUSB_MCU OPT_MCU_RP2040
#define CFG_TUSB_OS OPT_OS_PICO
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_HOST

#define CFG_TUH_ENUMERATION_BUFSIZE (256)

#define CFG_TUH_HUB (1)
#define CFG_TUH_HID (4)
#define CFG_TUH_MSC (1)
#define CFG_TUH_VENDOR (0)

#define CFG_TUH_DEVICE_MAX (8)

#define CFG_TUH_HID_EPIN_BUFSIZE (64)
#define CFG_TUH_HID_EPOUT_BUFSIZE (64)

#endif
