/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "usb/nfc.h"
#include "usb/vcp.h"
#include "str/str.h"
#include <stdio.h>

#define DEBUG_RIA_USB_NFC

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_NFC)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

/* TODO

NFC driver for USB VCP connected PN532

Use the vcp_std_open interface.

Add a persistent "SET NFC 0|1|2|86" option similar to SET BLE.
0 disables NFC and releases the VCP.
1 enables NFC and claims VCP.
2 scans all VCPs to find PN532.
86 clears memory with vcp_set_nfc_device("")

Because scanning can send data which devices may not expect,
we only attempt scanning for NFC devices when SET NFC 2, which
immediately degrades to 1 and begins the scan.

The VCP driver will be able to record and retrive a hash of the
vid/pid/serial/name/rev/etc which can be saved in cfg.

static vcp_hash_dev(uint8_t idx, char *hash) // fixed hash length
bool vcp_get_nfc_device(char *name, size_t size);
void vcp_set_nfc_device(const char *name);

vcp_get_nfc_device should avoid duplicate work with a boolean
that resets with mount/unmount.

nfc driver will have a task that will, when enabled, try
vcp_get_nfc_device, which will return true and the 
device name when it becomes available.

nfc driver must not block. Make everything a state machine.
DBG() all state transitions.

DBG() the following events:
detected, read ndef success, read ndef fail, removed

the driver should constantly poll for cards to read.

*/
