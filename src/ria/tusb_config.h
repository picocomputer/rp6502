/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TUSB_CONFIG_H
#define _TUSB_CONFIG_H

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_HOST

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

// Debug builds are quite noisy on v0.18.0 (March 2025).
// Override the debug level set by the tusb build system.
#undef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG (0)

#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif

#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUH_ENABLED (1)

#define CFG_TUH_ENUMERATION_BUFSIZE (256)

#define CFG_TUH_HUB (1)
#define CFG_TUH_HID (4)
#define CFG_TUH_MSC (1)
#define CFG_TUH_VENDOR (0)

#define CFG_TUH_DEVICE_MAX (8)

#endif
