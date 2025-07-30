/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MOU_H_
#define _MOU_H_

#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void mou_init(void);
void mou_stop(void);

// Set the extended register value.
bool mou_xreg(uint16_t word);

// Parse HID report descriptor for gamepad.
bool mou_mount(uint8_t slot, uint8_t const *desc_data, uint16_t desc_len);

// Clean up descriptor when device is disconnected.
void mou_umount(uint8_t slot);

// Process HID report.
void mou_report(uint8_t slot, void const *report, size_t size);

// Process HID boot report.
void mou_report_boot(uint8_t slot, void const *report, size_t size);

#endif /* _MOU_H_ */
