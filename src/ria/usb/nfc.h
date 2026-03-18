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

#endif /* _RIA_USB_NFC_H_ */
