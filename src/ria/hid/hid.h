/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_HID_HID_H_
#define _RIA_HID_HID_H_

/* Common code shared among all HID and HID-like drivers.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// The various HID and HID-like drivers each have their own numbering for
// managing connections. We unify these indexes into assigned "slots".
#define HID_USB_START (0x00000)
#define HID_XIN_START (0x10000)
#define HID_BLE_START (0x20000)

uint32_t hid_extract_bits(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size);
int32_t hid_extract_signed(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size);
uint8_t hid_scale_analog(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max);
int8_t hid_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max);

#endif /* _RIA_HID_HID_H_ */
