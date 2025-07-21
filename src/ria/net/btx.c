/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/wfi.h"
void btx_task(void) {}
void btx_print_status(void) {}
#else

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_WFI)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#include "pico.h"
#include "tusb_config.h"
#include "net/cyw.h"
#include "usb/pad.h"

// We can use the same indexing as hid and xin so long as we keep clear
static uint8_t btx_slot_to_pad_idx(int slot)
{
    return CFG_TUH_HID + PAD_MAX_PLAYERS + slot;
}

// TODO:
// This file will add support for gamepads over bluetooth classic and ble.
// src/ria/usb/pad.h already supports processing the HID descriptor and reports.
// src/ria/net/cyw.h has a ready indicator for cyw43_arch_init

void btx_task(void) {}
void btx_disconnect(void) {}
void btx_print_status(void) {}

#endif /* RP6502_RIA_W */
