/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_NDEF_H_
#define _RIA_USB_NDEF_H_

/* The NDEF data of an NTAG image read from page 0. Pure parsing, kept apart
 * from the PN532 driver in nfc.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* True once the image holds the Terminator TLV, found by walking the TLVs, so
 * a 0xFE in the UID or in the data is not taken for it. */
bool ndef_complete(const uint8_t *tag, size_t len);

/* The text of the first Text record of the first NDEF message. */
bool ndef_parse_text(const uint8_t *tag_data, size_t len, char *buf, size_t buf_size);

#endif /* _RIA_USB_NDEF_H_ */
