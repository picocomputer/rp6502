/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PAD_H_
#define _PAD_H_

#include <stdint.h>
#include <stdbool.h>

#define PAD_MAX_PLAYERS 4

/* Kernel events
 */

void pad_init(void);
void pad_stop(void);

// Set the extended register value.
bool pad_xreg(uint16_t word);

// Parse HID report descriptor for gamepad.
bool pad_mount(uint8_t slot, uint8_t const *desc_data, uint16_t desc_len,
               uint16_t vendor_id, uint16_t product_id);

// Clean up descriptor when device is disconnected.
bool pad_umount(uint8_t slot);

// Process HID gamepad report.
void pad_report(uint8_t slot, uint8_t const *data, uint16_t len);

void pad_home_button(uint8_t slot, bool pressed);

int pad_get_player_num(uint8_t slot);

#endif /* _PAD_H_ */
