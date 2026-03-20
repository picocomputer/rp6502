/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_NFC_H_
#define _RIA_USB_NFC_H_

/* NFC driver for USB VCP connected PN532
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void nfc_task(void);

/* Configuration setting NFC
 */

void nfc_load_enabled(const char *str);
bool nfc_set_enabled(uint8_t val);
uint8_t nfc_get_enabled(void);

// Parse raw NDEF TLV bytes and extract the first Well Known Text record
// into buf (NUL-terminated). Returns false if no text record is found.
bool nfc_parse_text(const uint8_t *ndef, size_t len, char *buf, size_t buf_size);

#endif /* _RIA_USB_NFC_H_ */
