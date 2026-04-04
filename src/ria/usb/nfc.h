/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_NFC_H_
#define _RIA_USB_NFC_H_

/* NFC driver for USB VCP connected PN532
 */

#include "api/api.h"
#include "api/std.h"
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
bool nfc_parse_text(const uint8_t *tag_data, size_t len, char *buf, size_t buf_size);

/* 6502 std driver interface
 */

bool nfc_std_handles(const char *name);
int nfc_std_open(const char *name, uint8_t flags, api_errno *err);
int nfc_std_close(int desc, api_errno *err);
std_rw_result nfc_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result nfc_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);

#endif /* _RIA_USB_NFC_H_ */
