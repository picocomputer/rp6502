/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_KBD_H_
#define _CORE_HID_KBD_H_

/* Which keys are down, as a bitmap the 6502 polls. What that spells is
 * core/hid/kbt.h.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "core/hid/hid.h"

/* Main events
 */

void kbd_init(void);
void kbd_stop(void);

// Claim this device, if its map describes a keyboard.
bool kbd_mount(int slot, const hid_report_map_t *map,
               uint16_t vendor_id, uint16_t product_id);

// Clean up descriptor when device is disconnected.
bool kbd_umount(int slot);

// Process HID keyboard report.
void kbd_report(int slot, uint8_t const *data, size_t size);

// Report ID of the keyboard on this slot, or 0 if its report map uses none.
uint8_t kbd_get_report_id(int slot);

// Set the extended register value.
bool kbd_xreg(uint16_t word);

/* What the terminal half needs back: the merged modifier byte, whether a
 * key is still held (auto-repeat asks), and the lock lamps, which live here
 * because they also ride in the bitmap the 6502 reads. */
uint8_t kbd_get_modifier(void);
bool kbd_key_down(uint8_t keycode);
uint8_t kbd_get_leds(void);
void kbd_toggle_lock(uint8_t bit);

// A host that decodes its own keyboard, in place of a report.
void kbd_hid_set(uint8_t keycode, bool down);

#endif /* _CORE_HID_KBD_H_ */
