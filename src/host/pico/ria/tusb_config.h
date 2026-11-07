/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TUSB_CONFIG_H
#define _TUSB_CONFIG_H

#include <stdalign.h>

#define CFG_TUSB_DEBUG_PRINTF rp6502_log_tusb

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
#define CFG_TUH_MEM_ALIGN alignas(4)
#endif

#define CFG_TUH_ENABLED (1)

#define CFG_TUH_ENUMERATION_BUFSIZE (1024)

#define CFG_TUH_HUB (5)
#define CFG_TUH_HID (8)
// Custom MSC driver lives in usb/msc.c
#define CFG_TUH_MSC (0)
// Max simultaneous CDC devices
#define CFG_TUH_CDC (4)
// Enable all serial-adapter drivers
#define CFG_TUH_CDC_FTDI (1)
#define CFG_TUH_CDC_CP210X (1)
#define CFG_TUH_CDC_CH34X (1)
#define CFG_TUH_CDC_PL2303 (1)
// Max simultaneous MIDI 1.0 devices
#define CFG_TUH_MIDI (4)
#define CFG_TUH_MIDI_STREAM_API (0)

// Devices; limited by free endpoints
#define CFG_TUH_DEVICE_MAX (16)

// More than all producers can have in flight
#define CFG_TUH_TASK_QUEUE_SZ (64)

// Per attach, on the root port and behind a hub
#define CFG_TUH_ENUM_ATTEMPT_MAX (8)

// A failed TinyUSB assert returns without a breakpoint, debugger or not.
#define CFG_TUSB_DEBUG_BREAKPOINT ria_tusb_breakpoint
static inline void ria_tusb_breakpoint(void) {}

// Level 2 and 3 log formats in flash; level 1 (asserts) stays in RAM.
#if CFG_TUSB_DEBUG >= 2
#include <stdint.h>
#include "common/tusb_debug.h"
#define RIA_TUSB_LOG(fmt, ...)                                                         \
    ({                                                                                 \
        static const char __attribute__((section(".flashdata.tusb_log"))) f_[] = fmt; \
        (void)tu_printf(f_, ##__VA_ARGS__);                                            \
    })
#undef TU_LOG2
#define TU_LOG2(...) RIA_TUSB_LOG(__VA_ARGS__)
#undef TU_LOG2_INT
#define TU_LOG2_INT(_x) TU_LOG2(#_x " = %ld\r\n", (unsigned long)(_x))
#undef TU_LOG2_HEX
#define TU_LOG2_HEX(_x) TU_LOG2(#_x " = 0x%lX\r\n", (unsigned long)(_x))
#if CFG_TUSB_DEBUG >= 3
#undef TU_LOG3
#define TU_LOG3 TU_LOG2
#undef TU_LOG3_INT
#define TU_LOG3_INT TU_LOG2_INT
#undef TU_LOG3_HEX
#define TU_LOG3_HEX TU_LOG2_HEX
#endif
#endif

// Asserts name the file, one RAM literal per file instead of a __func__ per function.
#if CFG_TUSB_DEBUG
#include "common/tusb_verify.h"
#undef TU_MESS_FAILED
#define TU_MESS_FAILED() TU_LOG1(__FILE_NAME__ ":%d: ASSERT FAILED\r\n", __LINE__)
#endif

#endif
