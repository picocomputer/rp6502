/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_HID_MOU_H_
#define _RIA_HID_MOU_H_

/* HID Mouse driver
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void mou_init(void);
void mou_stop(void);

// Set the extended register value.
bool mou_xreg(uint16_t word);

// Parse HID report descriptor for gamepad.
bool mou_mount(int slot, uint8_t const *desc_data, uint16_t desc_len);

// Clean up descriptor when device is disconnected.
bool mou_umount(int slot);

// Process HID report.
void mou_report(int slot, void const *report, size_t size);

#endif /* _RIA_HID_MOU_H_ */
