/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_MOUSE_H_
#define _CORE_HID_MOUSE_H_

/* HID Mouse driver
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "core/hid/hid.h"

/* Main events
 */

#define MOUSE_MAX_MICE 4

/* One mouse, as the driver reads it. A descriptor is parsed into this;
 * a machine with no descriptor to parse writes one out. */
// Mouse descriptors are normalized to this structure.
typedef struct mouse_connection
{
    bool valid;
    int slot;          // HID slot
    uint8_t report_id; // If non zero, the first report byte must match and will be skipped
    uint16_t button_offsets[8];
    bool x_relative;   // Will be true for mice
    uint16_t x_offset; // X axis
    uint8_t x_size;
    uint16_t y_offset; // Y axis
    uint8_t y_size;
    uint16_t wheel_offset; // Wheel/scroll wheel
    uint8_t wheel_size;
    uint16_t pan_offset; // Horizontal pan/tilt
    uint8_t pan_size;
    uint8_t buttons; // Last reported button state
} mouse_connection_t;

void mouse_init(void);
void mouse_stop(void);

// Set the extended register value.
bool mouse_xreg(uint16_t word);

// Whether a program has asked for this device's block.
bool mouse_is_mapped(void);

/* A host that decodes its own pointer, in place of a report. dx/dy are
 * in the block's counter units and fractions are carried between calls;
 * the wheel and pan bytes are 8-bit wrapping accumulators, and the
 * button byte is in HID order (bit 0 left, 1 right, 2 middle). */
void mouse_host_move(float dx, float dy);
void mouse_host_wheel(int dwheel, int dpan);
void mouse_host_buttons(uint8_t buttons);

// Parse HID report descriptor for mouse.
bool mouse_mount(int slot, const mouse_connection_t *desc);

// Clean up descriptor when device is disconnected.
bool mouse_umount(int slot);

// Process HID report.
void mouse_report(int slot, uint8_t const *report, size_t size);

#endif /* _CORE_HID_MOUSE_H_ */
