/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PAD_H_
#define _PAD_H_

#include <stdint.h>
#include <stdbool.h>

/* Gamepad USB HID support with Xbox One XInput protocol
 *
 * Supports:
 * - Generic HID gamepads
 * - Sony DualShock 4 and DualSense controllers
 * - Xbox One and Xbox Series X/S controllers using XInput protocol
 * - Third-party Xbox One compatible controllers (PDP, PowerA, Hori, Razer)
 */

/* Kernel events
 */

void pad_init(void);
void pad_stop(void);

// Set the extended register value.
bool pad_xreg(uint16_t word);

// Parse HID report descriptor for gamepad.
void pad_mount(uint8_t idx, uint8_t const *desc_report, uint16_t desc_len,
               uint16_t vendor_id, uint16_t product_id);

// Clean up descriptor when device is disconnected.
void pad_umount(uint8_t idx);

// Process HID gamepad report.
void pad_report(uint8_t idx, uint8_t const *report, uint16_t len);

bool pad_is_valid(uint8_t idx);

#endif /* _PAD_H_ */
