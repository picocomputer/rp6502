/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_MOU_H_
#define _CORE_HID_MOU_H_

/* HID Mouse driver
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "core/hid/hid.h"

/* Main events
 */

void mou_init(void);
void mou_stop(void);

// Set the extended register value.
bool mou_xreg(uint16_t word);

// Whether a program has asked for this device's block.
bool mou_is_mapped(void);

/* A host that decodes its own pointer, in place of a report. dx/dy are
 * in the block's counter units and fractions are carried between calls;
 * the wheel and pan bytes are 8-bit wrapping accumulators, and the
 * button byte is in HID order (bit 0 left, 1 right, 2 middle). */
void mou_host_move(float dx, float dy);
void mou_host_wheel(int dwheel, int dpan);
void mou_host_buttons(uint8_t buttons);

// Parse HID report descriptor for mouse.
bool mou_mount(int slot, const hid_report_map_t *map);

// Clean up descriptor when device is disconnected.
bool mou_umount(int slot);

// Process HID report.
void mou_report(int slot, uint8_t const *report, size_t size);

#endif /* _CORE_HID_MOU_H_ */
